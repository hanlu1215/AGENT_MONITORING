/*
 * ============================================================================
 *  ESP32-S3-N16R8   Codex 状态灯
 * ----------------------------------------------------------------------------
 *  功能:
 *    1. 上电自动连接 WiFi 并启动 HTTP 服务器(端口 80)
 *    2. 接收电脑上 codex-feishu-notify 推来的状态, 用板载 RGB 灯显示颜色:
 *         空闲/不工作 -> 绿色      工作中   -> 黄色
 *         等待审批     -> 红色(呼吸) 已完成   -> 蓝色(20 秒后回绿色)
 *         已中断       -> 品红(20 秒后回绿色) 其它 -> 青色
 *    3. 保留原来的网页控制: 可手动开关灯, 也能一键切回"跟随 Codex"
 *
 *  接收接口(供 codex-feishu-notify 调用):
 *     POST /api/codex     {"status":"working","text":"...","thread":"..."}
 *     GET  /api/codex?status=working&text=...     (浏览器里可直接测)
 *
 *  Arduino IDE 开发板设置 (工具 -> 开发板):
 *    开发板        : ESP32S3 Dev Module
 *    Flash Size    : 16MB (128Mb)
 *    PSRAM         : OPI PSRAM          <- N16R8 必须选 OPI
 *    Partition Sch.: 16M Flash (3MB APP/9.9MB FATFS) 或 Default 16MB
 *    USB CDC On Boot: Enabled           <- 用板载 USB 口看串口日志必须开启
 *    Upload Speed  : 921600
 * ============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>

// 想要用 http://esp32s3.local 访问就把下面改成 1
#define USE_MDNS 1

// 想给板子固定 IP(推荐, 这样 config.json 里填的地址不会变), 把下面改成 1
#define USE_STATIC_IP 0

#if USE_MDNS
#include <ESPmDNS.h>
#endif

// ============================================================================
//  用户配置区
// ============================================================================

// ---- WiFi (必须是 2.4GHz, ESP32-S3 不支持 5GHz) ----
const char *WIFI_SSID     = "WIFI";
const char *WIFI_PASSWORD = "PASSWORD";

// ---- 静态 IP (USE_STATIC_IP 为 1 时才生效, 要跟路由器同网段) ----
const char *STATIC_IP      = "192.168.1.50";
const char *STATIC_GATEWAY = "192.168.1.1";
const char *STATIC_SUBNET  = "255.255.255.0";
const char *STATIC_DNS     = "192.168.1.1";

// ---- 板载 LED ----
// 绝大多数 ESP32-S3-DevKitC-1 / N16R8 开发板的板载灯是一颗 WS2812(可寻址 RGB)
// 灯珠, 数据脚接在 GPIO48 上, 用 LED_IS_WS2812 = 1。
// 如果是普通单色 LED(常见接 GPIO2), 改成:
//     #define LED_IS_WS2812  0
//     #define LED_PIN        2
// 单色灯无法显示颜色, 会自动退化为"不同闪烁节奏表示不同状态"。
#define LED_IS_WS2812   1       // 1 = WS2812 RGB 灯珠; 0 = 普通单色 LED
#define LED_PIN         48      // 板载 LED 引脚
#define LED_BRIGHTNESS  120     // 亮度 0~255 (WS2812 亮度过高会刺眼)
#define LED_ON_LEVEL    HIGH    // 普通 LED 的有效电平(共阳接法请改为 LOW)

// ---- 状态灯行为 ----
#define TRANSIENT_HOLD_MS  20000UL    // 完成/中断/通知 显示多久后回到空闲绿色
#define IDLE_TIMEOUT_MS    900000UL   // 超过这么久没收到推送, 视为空闲(15 分钟)
#define BREATH_PERIOD_MS   1600UL     // 等待审批时红色呼吸一次的时间

// ---- 蜂鸣器（等待审批 / 完成时出声提醒）----
// 接线见 README 的"蜂鸣器怎么接"一节；没有接蜂鸣器就把 BUZZER_ENABLED 改成 0
#define BUZZER_ENABLED         1      // 0 = 完全不响(没接蜂鸣器就填 0, 代码会被编译掉)
#define BUZZER_PIN             4      // 蜂鸣器信号脚
// 无源蜂鸣器需要给它频率才响; 有源蜂鸣器给电就响(此时频率参数被忽略)
#define BUZZER_IS_PASSIVE      1      // 1 = 无源蜂鸣器(当前默认); 0 = 有源蜂鸣器
#define BUZZER_ACTIVE_LEVEL    HIGH   // 仅"有源模块"用: 高电平触发填 HIGH, 低电平触发填 LOW
#define BUZZER_BEEP_ON_BOOT    1      // 开机响一声, 顺便验证接线
#define BUZZER_WAITING_BEEPS   3      // 进入"等待审批": 连响 3 声
#define BUZZER_COMPLETED_BEEPS 2      // "已完成": 响 2 声(一低一高)
#define BUZZER_ON_INTERRUPTED  0      // "已中断"是否也响(默认关)
#define APPROVAL_REMIND_MS     30000UL // 还在等待时每 30 秒补一声提醒, 0 = 不提醒
#define APPROVAL_REMIND_MAX    5      // 最多提醒几次, 0 = 不限次数

// ---- 无源蜂鸣器的发声频率(Hz) ----
// 常见无源蜂鸣器的谐振点在 2~3kHz 附近, 这个区间最响; 觉得太尖或太闷可以自己调。
#define TONE_LOW   1800     // "完成"音的第一声(低)
#define TONE_HIGH  2700     // 提示音主频率(等待审批用它)
#define TONE_ALERT 900      // "中断"音(默认不开)

// ---- 其它 ----
#define SERIAL_BAUD     115200
#define WIFI_TIMEOUT_MS 20000
#define MDNS_HOST       "esp32s3"     // 改成 "codex-led" 则可用 http://codex-led.local

// ---- 低功耗 / 发热控制 ----
// 1 = 省电模式(推荐): 打开 WiFi 省电、CPU 降到 160MHz、降低发射功率,
//     实测能把平均电流从 100~140mA 降到 45~70mA, 板子(尤其稳压器)明显不烫;
//     代价是 HTTP 首次响应可能慢 100~200ms(推送和灯完全不受影响)。
// 0 = 性能优先: 射频满负荷、CPU 240MHz, 板子会更热。
#define LOW_POWER       1

#if LOW_POWER
#define LED_REFRESH_MS  40     // 省电时动画刷慢一点, 少唤醒 CPU
#else
#define LED_REFRESH_MS  20
#endif

// ============================================================================

WebServer server(80);

// ---- Codex 状态定义 ----
enum CodexState {
  ST_IDLE,          // 空闲 / 不工作
  ST_WORKING,       // 工作中
  ST_WAITING,       // 等待审批
  ST_COMPLETED,     // 已完成
  ST_INTERRUPTED,   // 已中断 / 失败
  ST_INFO,          // 普通通知
  ST_UNKNOWN
};

// ---- 两个来源各存一份状态，互不覆盖（Codex / Claude Code）----
// 灯显示哪一个：按优先级 waiting > interrupted > working > completed > idle，
// 同优先级比"最近更新"，所以两个智能体同时跑也不会互相顶掉。
#define AGENT_COUNT 2

struct AgentSlot {
  const char *key;        // 上报用的标识
  const char *label;      // 显示/日志用的名字
  CodexState  state;
  String      text;
  String      thread;
  uint32_t    lastPushMs;
  bool        hasPush;
  uint32_t    stateSinceMs;
  uint8_t     reminders;     // 这个来源已经提醒过几次
  uint32_t    lastRemindMs;  // 上次提醒时间（按来源各算各的）
};

AgentSlot agents[AGENT_COUNT] = {
  { "codex",  "Codex",       ST_IDLE, "", "", 0, false, 0, 0, 0 },
  { "claude", "Claude Code", ST_IDLE, "", "", 0, false, 0, 0, 0 },
};

// ---- 手动模式(网页上开关灯) ----
bool manualMode   = false;           // false = 跟随 Codex
bool manualLedOn  = false;

// ---- 蜂鸣器(用一个小队列做非阻塞播放, 不会卡住 HTTP 服务) ----
struct BeepStep { uint16_t freq; uint16_t ms; };   // freq = 0 表示静音间隔
#define BEEP_QUEUE_MAX 8
BeepStep beepQueue[BEEP_QUEUE_MAX];
uint8_t  beepHead        = 0;
uint8_t  beepCount       = 0;
uint32_t beepStepStart   = 0;
bool     beeping         = false;

uint32_t lastWifiCheck = 0;
uint32_t lastLedRefresh = 0;

// ---------------------------------------------------------------------------
//  状态 -> 文字 / 颜色
// ---------------------------------------------------------------------------
const char *stateKey(CodexState s) {
  switch (s) {
    case ST_IDLE:        return "idle";
    case ST_WORKING:     return "working";
    case ST_WAITING:     return "waiting_approval";
    case ST_COMPLETED:   return "completed";
    case ST_INTERRUPTED: return "interrupted";
    case ST_INFO:        return "info";
    default:             return "unknown";
  }
}

const char *stateLabel(CodexState s) {
  switch (s) {
    case ST_IDLE:        return "空闲";
    case ST_WORKING:     return "工作中";
    case ST_WAITING:     return "等待审批";
    case ST_COMPLETED:   return "已完成";
    case ST_INTERRUPTED: return "已中断";
    case ST_INFO:        return "通知";
    default:             return "未知";
  }
}

void stateColor(CodexState s, uint8_t &r, uint8_t &g, uint8_t &b) {
  switch (s) {
    case ST_IDLE:        r = 0;   g = 200; b = 0;    break;   // 绿色
    case ST_WORKING:     r = 255; g = 150; b = 0;    break;   // 黄色
    case ST_WAITING:     r = 255; g = 0;   b = 0;    break;   // 红色
    case ST_COMPLETED:   r = 0;   g = 90;  b = 255;  break;   // 蓝色
    case ST_INTERRUPTED: r = 255; g = 0;   b = 160;  break;   // 品红
    case ST_INFO:        r = 0;   g = 180; b = 255;  break;   // 青色
    default:             r = 120; g = 120; b = 120;  break;   // 灰白
  }
}

// 状态是"瞬时"的(完成/中断/通知), 显示一会儿就自动回空闲
bool isTransient(CodexState s) {
  return s == ST_COMPLETED || s == ST_INTERRUPTED || s == ST_INFO;
}

// 结合超时规则, 算出某个来源此刻真正要显示的状态
CodexState slotEffective(const AgentSlot &slot, uint32_t now) {
  if (slot.hasPush) {
    uint32_t age = now - slot.lastPushMs;
    if (isTransient(slot.state) && age > TRANSIENT_HOLD_MS) return ST_IDLE;
    if (!isTransient(slot.state) && age > IDLE_TIMEOUT_MS) return ST_IDLE;
  }
  return slot.state;
}

// 越紧急的数字越大：等待审批 > 中断 > 工作中 > 已完成 > 通知 > 空闲
int statePriority(CodexState s) {
  switch (s) {
    case ST_WAITING:     return 5;
    case ST_INTERRUPTED: return 4;
    case ST_WORKING:     return 3;
    case ST_COMPLETED:   return 2;
    case ST_INFO:        return 1;
    default:             return 0;
  }
}

// 灯该听谁的：先比优先级, 一样就看谁更新得晚
int winningSlot(uint32_t now) {
  int best = 0;
  int bestScore = -1;
  for (int i = 0; i < AGENT_COUNT; i++) {
    int score = statePriority(slotEffective(agents[i], now));
    if (score > bestScore || (score == bestScore && agents[i].lastPushMs > agents[best].lastPushMs)) {
      bestScore = score;
      best = i;
    }
  }
  return best;
}

// 灯的当前状态（= 优先级最高的那个来源）
CodexState effectiveState(uint32_t now) {
  return slotEffective(agents[winningSlot(now)], now);
}

int agentIndex(const String &raw) {
  String v = raw;
  v.trim();                 // 不依赖后面定义的 trimStr，避免前向引用
  v.toLowerCase();
  if (v.indexOf("claude") >= 0 || v == "cc") return 1;
  return 0;
}

// 这个状态此刻是否应该亮着
//   RGB 灯: 每个状态都有颜色(空闲=绿色), 所以恒为亮
//   单色灯: 空闲熄灭, 靠闪烁节奏区分其它状态
bool ledIsLit(CodexState s) {
#if LED_IS_WS2812
  (void)s;
  return true;
#else
  return s != ST_IDLE;
#endif
}

// ---------------------------------------------------------------------------
//  点灯
// ---------------------------------------------------------------------------
void writeRgb(uint8_t r, uint8_t g, uint8_t b) {
#if LED_IS_WS2812
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(LED_PIN, r, g, b);          // ESP32 Arduino 核心 3.x
#else
  neopixelWrite(LED_PIN, r, g, b);        // ESP32 Arduino 核心 2.x
#endif
#else
  bool on = (r + g + b) > 0;
  digitalWrite(LED_PIN, on ? LED_ON_LEVEL : (LED_ON_LEVEL ? LOW : HIGH));
#endif
}

// ---------------------------------------------------------------------------
//  蜂鸣器
// ---------------------------------------------------------------------------
void buzzerOn(uint16_t freq) {
#if BUZZER_ENABLED
#if BUZZER_IS_PASSIVE
  tone(BUZZER_PIN, freq ? freq : TONE_HIGH);   // 无源蜂鸣器: 给频率
#else
  (void)freq;                                   // 有源蜂鸣器: 给电就响
  digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LEVEL);
#endif
#else
  (void)freq;
#endif
}

void buzzerOff() {
#if BUZZER_ENABLED
#if BUZZER_IS_PASSIVE
  noTone(BUZZER_PIN);
#else
  digitalWrite(BUZZER_PIN, (BUZZER_ACTIVE_LEVEL == HIGH) ? LOW : HIGH);
#endif
#endif
}

// 往队列里塞一声(freq=0 表示这段是静音间隔)
void beepPush(uint16_t freq, uint16_t ms) {
#if BUZZER_ENABLED
  if (beepCount >= BEEP_QUEUE_MAX) return;
  uint8_t idx = (uint8_t)((beepHead + beepCount) % BEEP_QUEUE_MAX);
  beepQueue[idx].freq = freq;
  beepQueue[idx].ms   = ms;
  beepCount++;
#else
  (void)freq; (void)ms;
#endif
}

void beepClear() {
  beepHead    = 0;
  beepCount   = 0;
  beeping     = false;
  buzzerOff();
}

// 只在开机自检时用的阻塞版(例如响一声验证接线)
void beepBlocking(uint16_t freq, uint16_t ms) {
#if BUZZER_ENABLED
  buzzerOn(freq);
  delay(ms);
  buzzerOff();
  delay(30);
#else
  (void)freq; (void)ms;
#endif
}

// 在 loop 里不停调用, 按队列逐声播放, 全程不阻塞
void beepUpdate(uint32_t now) {
#if BUZZER_ENABLED
  if (beepCount == 0) {
    if (beeping) { buzzerOff(); beeping = false; }
    return;
  }
  BeepStep step = beepQueue[beepHead];
  if (!beeping) {
    beepStepStart = now;
    beeping       = true;
    if (step.freq == 0) buzzerOff();
    else                buzzerOn(step.freq);
    return;
  }
  if (now - beepStepStart >= step.ms) {
    beepHead = (uint8_t)((beepHead + 1) % BEEP_QUEUE_MAX);
    beepCount--;
    beeping = false;
    beepUpdate(now);                 // 紧接着播下一声
  }
#else
  (void)now;
#endif
}

// 不同状态配不同提示音
void beepForState(CodexState st) {
#if BUZZER_ENABLED
  if (st == ST_WAITING) {
    // 等待审批: "嘀 嘀 嘀" 三声短促
    for (int i = 0; i < BUZZER_WAITING_BEEPS; i++) {
      beepPush(TONE_HIGH, 90);
      beepPush(0, 80);
    }
  } else if (st == ST_COMPLETED) {
    // 完成: 一低一高的两声称"搞定"
    for (int i = 0; i < BUZZER_COMPLETED_BEEPS; i++) {
      beepPush(i == 0 ? TONE_LOW : TONE_HIGH, i == 0 ? 120 : 200);
      if (i == 0) beepPush(0, 70);
    }
    beepPush(0, 60);
  }
#if BUZZER_ON_INTERRUPTED
  else if (st == ST_INTERRUPTED) {
    beepPush(TONE_ALERT, 150);
    beepPush(0, 90);
    beepPush(TONE_ALERT / 2, 250);
    beepPush(0, 80);
  }
#endif
#else
  (void)st;
#endif
}

// 单色灯用的闪烁节奏: 返回此刻是否应该亮
bool blinkGate(CodexState s, uint32_t now, uint32_t stateAge) {
  switch (s) {
    case ST_WORKING:     return true;                                  // 常亮
    case ST_WAITING:     return (now % 1000UL) < 500UL;                // 慢闪
    case ST_COMPLETED:
    case ST_INTERRUPTED:                                               // 连闪 3 下
      return (stateAge < 900UL) && ((stateAge / 150UL) % 2UL == 0UL);
    case ST_INFO:                                                      // 连闪 2 下
      return (stateAge < 600UL) && ((stateAge / 150UL) % 2UL == 0UL);
    default:             return false;
  }
}

void applyLed() {
  uint32_t now = millis();

  // ---- 手动模式: 白光 / 熄灭 ----
  if (manualMode) {
    uint8_t v = manualLedOn ? LED_BRIGHTNESS : 0;
    writeRgb(v, v, v);
    return;
  }

  // ---- 跟随智能体：取优先级最高的那个来源 ----
  int winner = winningSlot(now);
  CodexState st = slotEffective(agents[winner], now);
  uint8_t r, g, b;
  stateColor(st, r, g, b);

  uint32_t stateAge = now - agents[winner].stateSinceMs;
  bool lit = true;

  // 等待审批: 红色呼吸, 比常亮更容易被注意到
  if (st == ST_WAITING) {
    uint32_t phase = now % BREATH_PERIOD_MS;
    uint32_t half  = BREATH_PERIOD_MS / 2UL;
    // 三角波: 亮度在 25% ~ 100% 之间来回
    uint32_t level = (phase < half) ? phase : (BREATH_PERIOD_MS - phase);
    level = 64UL + (level * 191UL) / half;          // 64 ~ 255
    r = (uint8_t)((r * level) / 255UL);
    g = (uint8_t)((g * level) / 255UL);
    b = (uint8_t)((b * level) / 255UL);
    // 呼吸的峰值同样受 LED_BRIGHTNESS 约束, 免得比其它状态刺眼
    r = (uint8_t)((r * LED_BRIGHTNESS) / 255UL);
    g = (uint8_t)((g * LED_BRIGHTNESS) / 255UL);
    b = (uint8_t)((b * LED_BRIGHTNESS) / 255UL);
    lit = true;
  } else {
    // 统一按配置亮度缩放
    r = (uint8_t)((r * LED_BRIGHTNESS) / 255UL);
    g = (uint8_t)((g * LED_BRIGHTNESS) / 255UL);
    b = (uint8_t)((b * LED_BRIGHTNESS) / 255UL);
  }

#if !LED_IS_WS2812
  lit = lit && blinkGate(st, now, stateAge);          // 单色灯靠闪烁区分状态
#else
  (void)stateAge;
#endif

  if (!lit) { r = 0; g = 0; b = 0; }
  writeRgb(r, g, b);
}

// 按来源更新状态：idx 0 = Codex, 1 = Claude Code。两边各存一份, 互不覆盖。
void setAgentState(int idx, CodexState st, const String &text, const String &thread) {
  if (idx < 0 || idx >= AGENT_COUNT) idx = 0;
  AgentSlot &slot = agents[idx];

  bool changed = (st != slot.state);
  slot.state   = st;
  slot.text    = text;
  slot.thread  = thread;
  slot.lastPushMs = millis();
  slot.hasPush    = true;
  if (changed) {
    slot.stateSinceMs = millis();
    beepForState(st);            // 提示音照旧：等待审批/完成/工作时出声
    slot.reminders  = 0;
    slot.lastRemindMs = millis();
  }
  manualMode = false;            // 收到推送就回到"跟随智能体"
  applyLed();

  Serial.printf("[%s] 状态 -> %s (%s)  文本: %s\n",
                slot.label, stateKey(st), stateLabel(st), slot.text.c_str());
}

// 兼容老调用（工具里没带 agent 时按 Codex 处理）
void setCodexState(CodexState st, const String &text, const String &thread) {
  setAgentState(0, st, text, thread);
}

// ---------------------------------------------------------------------------
//  小工具: 取 JSON 字段 / 去空格 / HTML 转义
// ---------------------------------------------------------------------------
String trimStr(const String &s) {
  int a = 0, b = (int)s.length();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
  return s.substring(a, b);
}

// 极简 JSON 取值(只处理我们自己发的这种扁平结构, 不依赖任何库)
String jsonValue(const String &body, const char *key) {
  String pattern = "\"" + String(key) + "\"";
  int i = body.indexOf(pattern);
  if (i < 0) return "";
  int colon = body.indexOf(':', i + pattern.length());
  if (colon < 0) return "";
  int p = colon + 1;
  while (p < (int)body.length() &&
         (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' || body[p] == '\n')) p++;
  if (p >= (int)body.length()) return "";
  if (body[p] == '"') {
    int end = p + 1;
    while (end < (int)body.length()) {
      if (body[end] == '\\') { end += 2; continue; }
      if (body[end] == '"') break;
      end++;
    }
    if (end > (int)body.length()) return "";
    return body.substring(p + 1, end);
  }
  int end = p;
  while (end < (int)body.length() && body[end] != ',' && body[end] != '}') end++;
  return trimStr(body.substring(p, end));
}

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\')      { out += '\\'; out += c; }
    else if (c == '\n' || c == '\r') { out += ' '; }
    else if ((uint8_t)c < 0x20)      { /* 丢掉控制字符 */ }
    else                             { out += c; }
  }
  return out;
}

// 认识中英文状态写法
CodexState parseState(const String &raw) {
  String v = trimStr(raw);
  if (v.length() == 0) return ST_UNKNOWN;

  if (v.indexOf("工作中") >= 0 || v.indexOf("工作") >= 0 || v.indexOf("运行中") >= 0 ||
      v.indexOf("进行中") >= 0) return ST_WORKING;
  if (v.indexOf("等待审批") >= 0 || v.indexOf("待审批") >= 0 || v.indexOf("审批") >= 0 ||
      v.indexOf("等待确认") >= 0 || v.indexOf("等待") >= 0) return ST_WAITING;
  if (v.indexOf("已完成") >= 0 || v.indexOf("完成") >= 0) return ST_COMPLETED;
  if (v.indexOf("已中断") >= 0 || v.indexOf("中断") >= 0 || v.indexOf("失败") >= 0) return ST_INTERRUPTED;
  if (v.indexOf("空闲") >= 0 || v.indexOf("不工作") >= 0 || v.indexOf("闲置") >= 0) return ST_IDLE;
  if (v.indexOf("通知") >= 0 || v.indexOf("消息") >= 0) return ST_INFO;

  v.toLowerCase();
  if (v == "working" || v == "work" || v == "busy" || v == "running") return ST_WORKING;
  if (v == "waiting_approval" || v == "waiting" || v == "approval" ||
      v == "permission" || v == "wait") return ST_WAITING;
  if (v == "completed" || v == "complete" || v == "done" || v == "finished") return ST_COMPLETED;
  if (v == "interrupted" || v == "aborted" || v == "failed" || v == "error") return ST_INTERRUPTED;
  if (v == "idle" || v == "free" || v == "none") return ST_IDLE;
  if (v == "info" || v == "notify") return ST_INFO;
  return ST_UNKNOWN;
}

// ---------------------------------------------------------------------------
//  状态 JSON(网页和外部程序都读它)
// ---------------------------------------------------------------------------
String buildStatusJson() {
  uint32_t now = millis();
  int winner = winningSlot(now);
  CodexState st = slotEffective(agents[winner], now);
  uint8_t r, g, b;
  stateColor(st, r, g, b);
  char colorHex[8];
  snprintf(colorHex, sizeof(colorHex), "#%02X%02X%02X", r, g, b);

  bool lit = manualMode ? manualLedOn : ledIsLit(st);
  uint32_t age = agents[winner].hasPush ? (now - agents[winner].lastPushMs) / 1000UL : 0;

  String json = "{";
  json += "\"ok\":true";
  json += ",\"led\":";              json += lit ? "true" : "false";
  json += ",\"mode\":\"";           json += manualMode ? "manual" : "auto";       json += "\"";
  json += ",\"buzzer\":";           json += BUZZER_ENABLED ? "true" : "false";
  // 灯当前显示的是哪个来源
  json += ",\"agent\":\"";          json += agents[winner].key;                    json += "\"";
  json += ",\"agent_label\":\"";    json += agents[winner].label;                  json += "\"";
  json += ",\"codex\":\"";          json += stateKey(st);                          json += "\"";
  json += ",\"codex_label\":\"";    json += stateLabel(st);                        json += "\"";
  json += ",\"color\":\"";          json += colorHex;                              json += "\"";
  json += ",\"text\":\"";           json += jsonEscape(agents[winner].text);        json += "\"";
  json += ",\"thread\":\"";         json += jsonEscape(agents[winner].thread);      json += "\"";
  json += ",\"last_push_age\":";    json += String((unsigned long)age);
  // 每个来源各报一份状态，网页和排查都看得到
  json += ",\"agents\":[";
  for (int i = 0; i < AGENT_COUNT; i++) {
    CodexState s = slotEffective(agents[i], now);
    char hex[8];
    uint8_t ar, ag, ab;
    stateColor(s, ar, ag, ab);
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", ar, ag, ab);
    if (i) json += ",";
    json += "{\"key\":\"";    json += agents[i].key;
    json += "\",\"label\":\""; json += agents[i].label;
    json += "\",\"state\":\""; json += stateKey(s);
    json += "\",\"state_label\":\""; json += stateLabel(s);
    json += "\",\"color\":\""; json += hex;
    json += "\",\"text\":\"";  json += jsonEscape(agents[i].text);
    json += "\",\"thread\":\""; json += jsonEscape(agents[i].thread);
    json += "\"}";
  }
  json += "]";
  json += ",\"ip\":\"";             json += WiFi.localIP().toString();             json += "\"";
  json += ",\"rssi\":";             json += String((int)WiFi.RSSI());
  json += ",\"uptime\":";           json += String((unsigned long)(millis() / 1000UL));
  json += ",\"heap\":";             json += String((unsigned long)ESP.getFreeHeap());
  json += ",\"link\":\"";           json += (WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected"; json += "\"";
  json += "}";
  return json;
}

// ---------------------------------------------------------------------------
//  网页
// ---------------------------------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Codex 状态灯 · ESP32-S3</title>
<style>
  *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
  body{
    margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","Microsoft YaHei",Roboto,sans-serif;
    background:radial-gradient(900px 620px at 50% -15%,#1e3a5f 0%,#0b1120 55%,#06090f 100%);
    color:#e6edf7;padding:22px;
  }
  .card{
    width:100%;max-width:430px;padding:30px 26px 24px;border-radius:26px;text-align:center;
    background:rgba(19,28,46,.75);border:1px solid rgba(120,170,255,.16);
    box-shadow:0 26px 60px rgba(0,0,0,.6),inset 0 1px 0 rgba(255,255,255,.06);
    backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);
  }
  h1{margin:0 0 6px;font-size:19px;font-weight:600;letter-spacing:.6px}
  .sub{margin:0 0 22px;font-size:12px;color:#8ea3c0;letter-spacing:.4px}
  .orb{
    width:118px;height:118px;margin:0 auto 18px;border-radius:50%;
    background:#26303f;border:1px solid rgba(255,255,255,.07);
    transition:background .35s,box-shadow .35s;
  }
  .state{font-size:17px;font-weight:700;letter-spacing:1.2px;margin-bottom:5px}
  .mode{display:inline-block;font-size:11px;padding:3px 10px;border-radius:999px;
    background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.1);color:#9fb3cc;margin-bottom:10px}
  .hint{font-size:11.5px;color:#66788f;margin-bottom:20px;min-height:32px;line-height:1.5;
    white-space:pre-wrap;word-break:break-word}
  .agents{display:flex;gap:8px;justify-content:center;flex-wrap:wrap;margin:-10px 0 18px}
  .agents span{font-size:11.5px;padding:5px 10px;border-radius:999px;
    background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.1);color:#c7d6ea}
  .agents i{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
  .row{display:flex;gap:10px;margin-bottom:14px}
  .btn{flex:1;padding:12px 0;border-radius:14px;font-size:13.5px;font-weight:600;letter-spacing:.5px;
    cursor:pointer;transition:.2s;font-family:inherit;color:#dbe6f5;
    background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.1)}
  .btn:hover{background:rgba(255,255,255,.12);transform:translateY(-1px)}
  .btn:active{transform:scale(.97)}
  .btn.primary:hover{background:rgba(34,211,238,.22);border-color:rgba(34,211,238,.55);color:#a5f3fc}
  .btn.on:hover{background:rgba(74,222,128,.2);border-color:rgba(74,222,128,.5);color:#bbf7d0}
  .btn.off:hover{background:rgba(148,163,184,.2);border-color:rgba(148,163,184,.5)}
  .info{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:6px}
  .info div{background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.07);
    border-radius:13px;padding:10px 12px;text-align:left}
  .k{display:block;font-size:10.5px;color:#7186a3;letter-spacing:.6px;margin-bottom:5px}
  .v{display:block;font-size:13.5px;font-weight:600;color:#d5e3f5;
    white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:#4ade80;
    margin-right:6px;vertical-align:middle}
  .err{color:#f87171}
</style>
</head>
<body>
<div class="card">
  <h1>Codex 状态灯</h1>
  <p class="sub">ESP32-S3-N16R8 &middot; 板载 RGB</p>

  <div class="orb" id="orb"></div>
  <div class="state" id="stateText">--</div>
  <div class="mode" id="modeText">--</div>
  <div class="hint" id="hint">等待数据…</div>
  <div class="agents" id="agents"></div>

  <div class="row">
    <button class="btn on"  onclick="setLed(1)">手动开灯</button>
    <button class="btn off" onclick="setLed(0)">手动关灯</button>
  </div>
  <div class="row">
    <button class="btn primary" onclick="setMode('auto')">跟随 Codex（自动）</button>
    <button class="btn" onclick="testBeep()">测试蜂鸣器</button>
  </div>

  <div class="info">
    <div><span class="k">IP 地址</span><span class="v" id="ip">--</span></div>
    <div><span class="k">信号强度</span><span class="v" id="rssi">--</span></div>
    <div><span class="k">运行时间</span><span class="v" id="uptime">--</span></div>
    <div><span class="k">最近推送</span><span class="v" id="lastpush">--</span></div>
  </div>
</div>

<script>
  function fmtUptime(s){
    s = Math.floor(s);
    var h = Math.floor(s/3600), m = Math.floor((s%3600)/60), ss = s%60;
    if (h > 0) return h + ' 小时 ' + m + ' 分';
    if (m > 0) return m + ' 分 ' + ss + ' 秒';
    return ss + ' 秒';
  }

  function render(d){
    var orb = document.getElementById('orb');
    var color = d.color || '#7d8da4';
    if (d.led){
      orb.style.background = 'radial-gradient(circle at 34% 28%, #ffffff 0%, ' + color + ' 62%, #05080e 125%)';
      orb.style.boxShadow = '0 0 42px ' + color + 'b0';
    } else {
      orb.style.background = '#26303f';
      orb.style.boxShadow = 'none';
    }

    var st = document.getElementById('stateText');
    st.textContent = d.codex_label || '--';
    st.style.color = d.led ? color : '#7d8da4';

    document.getElementById('modeText').textContent =
      d.mode === 'manual' ? '手动模式' : '跟随智能体';

    // 两个来源各显示一行：粗体的那个就是当前显示在灯上的（按优先级选出来的）
    var list = document.getElementById('agents');
    if (d.agents && d.agents.length) {
      list.innerHTML = d.agents.map(function(a){
        var c = a.color || '#7d8da4';
        var weight = (a.key === d.agent) ? 'font-weight:700;border-color:' + c + '66;' : 'opacity:.7;';
        return '<span style="' + weight + '"><i style="background:' + c + '"></i>'
             + (a.label || a.key) + ' · ' + (a.state_label || a.state) + '</span>';
      }).join('');
    }

    var hint = [];
    if (d.thread) hint.push('任务：' + d.thread);
    if (d.text)   hint.push(d.text);
    if (!d.thread && !d.text) hint.push(d.mode === 'manual' ? '手动控制中' : '暂时没有收到 Codex 状态');
    document.getElementById('hint').textContent = hint.join('\n');

    if (d.ip) document.getElementById('ip').textContent = d.ip;
    if (d.rssi !== undefined) document.getElementById('rssi').textContent = d.rssi + ' dBm';
    if (d.uptime !== undefined) document.getElementById('uptime').textContent = fmtUptime(d.uptime);
    document.getElementById('lastpush').textContent =
      (d.last_push_age === undefined) ? '--' : (d.last_push_age + ' 秒前');
  }

  function showErr(t){
    var e = document.getElementById('hint');
    e.textContent = t;
    e.classList.add('err');
    setTimeout(function(){ e.classList.remove('err'); }, 2500);
  }

  function setLed(v){
    fetch('/api/led?state=' + (v ? 1 : 0))
      .then(function(r){ return r.json(); }).then(render)
      .catch(function(){ showErr('请求失败, 请检查网络连接'); });
  }

  function setMode(m){
    fetch('/api/mode?m=' + m)
      .then(function(r){ return r.json(); }).then(render)
      .catch(function(){ showErr('请求失败, 请检查网络连接'); });
  }

  function testBeep(){
    fetch('/api/beep?n=2&ms=120')
      .then(function(r){ return r.json(); }).then(render)
      .catch(function(){ showErr('请求失败, 请检查网络连接'); });
  }

  function refresh(){
    fetch('/api/status')
      .then(function(r){ return r.json(); })
      .then(render)
      .catch(function(){});
  }

  refresh();
  setInterval(refresh, 2000);
</script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------------
//  HTTP 处理
// ---------------------------------------------------------------------------
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleApiStatus() {
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

// POST /api/codex  {"status":"working","text":"...","thread":"..."}
// GET  /api/codex?status=working&text=...      (不带参数则只查询当前状态)
void handleApiCodex() {
  String v     = trimStr(server.arg("status"));
  String text  = trimStr(server.arg("text"));
  String thrd  = trimStr(server.arg("thread"));
  String who   = trimStr(server.arg("agent"));

  if (v.length() == 0 && server.hasArg("plain")) {
    String body = server.arg("plain");
    v    = jsonValue(body, "status");
    if (text.length() == 0) text = jsonValue(body, "text");
    if (thrd.length() == 0) thrd = jsonValue(body, "thread");
    if (who.length() == 0)  who  = jsonValue(body, "agent");
  }

  if (v.length() > 0) {
    CodexState st = parseState(v);
    if (st == ST_UNKNOWN) {
      Serial.printf("[状态] 无法识别: %s\n", v.c_str());
      server.send(400, "application/json; charset=utf-8",
                  "{\"ok\":false,\"msg\":\"unknown status\"}");
      return;
    }
    if (text.length() > 120) text = text.substring(0, 120) + "…";
    if (thrd.length() > 60)  thrd = thrd.substring(0, 60) + "…";
    int idx = agentIndex(who);
    setAgentState(idx, st, text, thrd);
    Serial.printf("[%s] 来自 %s\n", agents[idx].label,
                  server.client().remoteIP().toString().c_str());
  }

  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

// 手动开关灯(会切到手动模式)
void handleApiLed() {
  if (server.hasArg("state")) {
    String v = server.arg("state");
    v.toLowerCase();
    if (v == "1" || v == "on" || v == "true" || v == "open")        manualLedOn = true;
    else if (v == "0" || v == "off" || v == "false" || v == "close") manualLedOn = false;
    else if (v == "toggle")                                          manualLedOn = !manualLedOn;
  } else {
    manualLedOn = !manualLedOn;
  }
  manualMode = true;
  beepClear();                      // 手动接管就别响了
  applyLed();
  Serial.printf("[WEB] 手动模式 LED -> %s\n", manualLedOn ? "ON" : "OFF");
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

void handleApiToggle() {
  manualLedOn = !manualLedOn;
  manualMode  = true;
  beepClear();
  applyLed();
  Serial.printf("[WEB] 手动模式 LED 翻转 -> %s\n", manualLedOn ? "ON" : "OFF");
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

// 切换 自动/手动: /api/mode?m=auto
void handleApiMode() {
  String m = trimStr(server.arg("m"));
  m.toLowerCase();
  if (m == "auto" || m == "codex" || m == "1") {
    manualMode = false;
    for (int i = 0; i < AGENT_COUNT; i++) agents[i].stateSinceMs = millis();
  } else if (m == "manual" || m == "0") {
    manualMode = true;
  }
  applyLed();
  Serial.printf("[WEB] 模式 -> %s\n", manualMode ? "手动" : "跟随智能体");
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

// 测试蜂鸣器: /api/beep?n=2&ms=120  (n=响几声, ms=每声多长)
void handleApiBeep() {
  int n  = server.hasArg("n")  ? server.arg("n").toInt()  : 2;
  int ms = server.hasArg("ms") ? server.arg("ms").toInt() : 120;
  if (n < 1)  n = 1;
  if (n > 8)  n = 8;
  if (ms < 20)   ms = 20;
  if (ms > 2000) ms = 2000;

#if BUZZER_ENABLED
  for (int i = 0; i < n; i++) {
    beepPush(TONE_HIGH, (uint16_t)ms);
    beepPush(0, 80);
  }
  Serial.printf("[BEEP] 测试 %d 声 x %d ms\n", n, ms);
#else
  Serial.println("[BEEP] 已禁用(把 BUZZER_ENABLED 改成 1 才响)");
#endif
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

void handleNotFound() {
  server.send(404, "application/json; charset=utf-8", "{\"ok\":false,\"msg\":\"not found\"}");
}

// ---------------------------------------------------------------------------
//  WiFi
// ---------------------------------------------------------------------------
void connectWiFi() {
  Serial.printf("[WiFi] 正在连接 \"%s\" ...\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
#if LOW_POWER
  // 打开 WiFi 省电(modem sleep): 射频按需唤醒, 这是最省电的一处改动
  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_11dBm);   // 默认 20dBm 功率过剩, 板子通常就在同一房间
  Serial.println("[PWR] 省电模式: WiFi modem sleep 开, 发射功率 11dBm");
#else
  WiFi.setSleep(false);            // 关闭省电, 让响应更快(更耗电也更热)
  Serial.println("[PWR] 性能模式: WiFi 不省电, 发射功率默认");
#endif
#if USE_STATIC_IP
  IPAddress ip, gw, sn, dns;
  ip.fromString(STATIC_IP);
  gw.fromString(STATIC_GATEWAY);
  sn.fromString(STATIC_SUBNET);
  dns.fromString(STATIC_DNS);
  if (!WiFi.config(ip, gw, sn, dns)) {
    Serial.println("[WiFi] 静态 IP 配置失败, 改用 DHCP");
  } else {
    Serial.printf("[WiFi] 使用静态 IP: %s\n", STATIC_IP);
  }
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] 连接成功!");
    Serial.print("[WiFi] 本机 IP 地址: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WiFi] 信号强度    : ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("[WiFi] 连接失败, 将在后台持续重试...");
  }
}

// ---------------------------------------------------------------------------
//  setup
// ---------------------------------------------------------------------------
void setup() {
#if LOW_POWER
  setCpuFrequencyMhz(160);   // 240 -> 160MHz: 先降频再初始化串口, 避免串口时钟切换杂波
#endif
  Serial.begin(SERIAL_BAUD);
  delay(300);

  Serial.println();
  Serial.println("========================================");
  Serial.println("  ESP32-S3-N16R8  Codex 状态灯");
  Serial.println("========================================");
  Serial.printf("  Flash : %lu MB    PSRAM: %lu KB\n",
                (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)),
                (unsigned long)(ESP.getPsramSize() / 1024));
  Serial.printf("  CPU   : %u MHz     功耗模式: %s\n",
                (unsigned)getCpuFrequencyMhz(),
                LOW_POWER ? "省电(推荐)" : "性能");

#if !LED_IS_WS2812
  pinMode(LED_PIN, OUTPUT);
#endif

  // 蜂鸣器: 开机响一声, 顺便验证接线对不对
#if BUZZER_ENABLED
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerOff();
#endif

  // 开机: 白闪 3 下, 然后进入空闲绿色
  for (int i = 0; i < 3; i++) {
    writeRgb(LED_BRIGHTNESS, LED_BRIGHTNESS, LED_BRIGHTNESS); delay(130);
    writeRgb(0, 0, 0);                                        delay(130);
  }
  for (int i = 0; i < AGENT_COUNT; i++) {
    agents[i].state   = ST_IDLE;
    agents[i].stateSinceMs = millis();
  }
  applyLed();

#if BUZZER_ENABLED && BUZZER_BEEP_ON_BOOT
  beepBlocking(TONE_HIGH, 120);
#endif

  connectWiFi();

#if USE_MDNS
  if (MDNS.begin(MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", MDNS_HOST);
  }
#endif

  server.on("/",                HTTP_GET,  handleRoot);
  server.on("/api/status",      HTTP_GET,  handleApiStatus);
  server.on("/api/codex",       HTTP_POST, handleApiCodex);   // 推状态(主接口)
  server.on("/api/codex",       HTTP_GET,  handleApiCodex);   // 查询/浏览器测试
  server.on("/api/led",         HTTP_GET,  handleApiLed);
  server.on("/api/led/toggle",  HTTP_GET,  handleApiToggle);
  server.on("/api/mode",        HTTP_GET,  handleApiMode);
  server.on("/api/beep",        HTTP_GET,  handleApiBeep);    // 测试蜂鸣器
  server.on("/favicon.ico",     HTTP_GET,  []() { server.send(204, "text/plain", ""); });
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[HTTP] 服务器已启动 (端口 80)");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[HTTP] 网页地址: http://");
    Serial.println(WiFi.localIP());
    Serial.print("[HTTP] 状态接口: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/api/codex");
  }
  Serial.println("========================================\n");

  applyLed();
}

// ---------------------------------------------------------------------------
//  loop
// ---------------------------------------------------------------------------
void loop() {
  server.handleClient();

  uint32_t now = millis();

  // 呼吸/闪烁动画, 按 LOW_POWER 决定刷新间隔
  if (now - lastLedRefresh >= LED_REFRESH_MS) {
    lastLedRefresh = now;
    applyLed();
  }

  // 蜂鸣器: 播放队列 + 等待审批期间定时提醒
  beepUpdate(now);
#if BUZZER_ENABLED && APPROVAL_REMIND_MS > 0
  // 按来源分别提醒：Codex 和 Claude Code 都在等审批时, 两个都会各自提醒
  if (!manualMode) {
    for (int i = 0; i < AGENT_COUNT; i++) {
      if (slotEffective(agents[i], now) != ST_WAITING) continue;
      if (APPROVAL_REMIND_MAX && agents[i].reminders >= APPROVAL_REMIND_MAX) continue;
      if (now - agents[i].lastRemindMs < APPROVAL_REMIND_MS) continue;
      agents[i].lastRemindMs = now;
      agents[i].reminders++;
      beepPush(TONE_HIGH, 90);
      beepPush(0, 80);
      Serial.printf("[提醒] %s 仍在等待审批，第 %d 次提醒\n",
                    agents[i].label, agents[i].reminders);
    }
  }
#endif

  // 每 5 秒检查 WiFi, 断线自动重连
  if (now - lastWifiCheck > 5000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] 连接已断开, 正在重连...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  delay(2);
}
