# ESP32-S3-N16R8 · Codex 状态灯（带提示音）

板子连上 WiFi 后开一个 HTTP 服务器，电脑上的 **codex-feishu-notify** 每推送一次状态，
板载 RGB 灯就换一种颜色，**需要审批和完成时还会响一声**——不盯屏幕，抬头就看到、听到。

```
ESP32_S3_N16R8/
├── ESP32S3_WebLED/
│   └── ESP32S3_WebLED.ino      ← 主程序（用 Arduino IDE 打开这个文件）
└── README.md
```

---

## 1. 灯的颜色 + 声音对照

| Codex 状态 | 灯的颜色 | 表现 | 声音 |
| --- | --- | --- | --- |
| 空闲 / 不工作 | 🟢 绿色 | 常亮（开机默认就是这个） | 无 |
| 工作中 | 🟡 黄色 | 常亮 | 无 |
| **等待审批** | 🔴 红色 | **呼吸（明暗循环），最显眼** | **"嘀 嘀 嘀"三声**，之后每 30 秒补一声提醒（最多 5 次） |
| **已完成** | 🔵 蓝色 | 亮 20 秒后回绿色 | **"嘀—嘀↑"两声**（一低一高） |
| 已中断 / 失败 | 🟣 品红 | 亮 20 秒后回绿色 | 默认不响（可开，见第 2 节） |
| 普通通知 | 🩵 青色 | 亮 20 秒后回绿色 | 无 |

两条自动兜底规则（板子自己判断，不依赖电脑）：

- **完成/中断/通知** 这类瞬时状态显示 `TRANSIENT_HOLD_MS`（默认 20 秒）后回到绿色空闲。
- 黄色/红色如果 `IDLE_TIMEOUT_MS`（默认 15 分钟）都没收到新推送（比如电脑关机了），
  也会自动回到绿色空闲，不会一直卡在"工作中"。

> 如果你的板载灯是普通单色 LED（不是 WS2812），颜色没法显示，固件会自动改成用闪烁节奏区分：
> 工作中常亮、等待审批慢闪、完成/中断连闪 3 下、空闲熄灭。

## 2. 蜂鸣器怎么接（硬件）

**当前固件默认按"无源蜂鸣器"驱动**（`BUZZER_IS_PASSIVE 1`），信号脚是 **GPIO4**。

### 先回答一个常见问题：不接蜂鸣器能用吗？

**能，完全不影响灯和推送。** 两种做法，任选：

| 做法 | 效果 |
| --- | --- |
| 什么都不改（`BUZZER_ENABLED` 保持 1，但不插蜂鸣器） | 一切正常。只是开机自检和状态推送时去驱动一个空引脚，**没有任何副作用**：不报错、不重启、不影响 WiFi 和灯。 |
| 把 `BUZZER_ENABLED` 改成 `0`（更干净） | 整个蜂鸣器代码被编译掉，连空引脚都不碰；`/api/beep` 会回一句"已禁用"。 |

也就是说：**先不接、先跑起来完全没问题**；蜂鸣器到货了再接上、重新烧一次固件即可。

### 方案 A：无源蜂鸣器模块（3 个针脚，最省事）

就是那种蓝色小板上带三极管、三个针脚的模块（`VCC` / `GND` / `I-O`，或标 `+ - S`）。
模块**自带驱动电路**，可以直接接，不需要额外元件：

| 模块针脚 | 接到 ESP32-S3 | 说明 |
| --- | --- | --- |
| `VCC`（或 `+`） | **3V3** | 3.3V 就能响；模块标 5V 的话接板上 `5V` 针脚也行 |
| `GND`（或 `-`） | **GND** | 任意一个 GND |
| `I/O`（或 `IN` / `S`） | **GPIO4** | 信号脚；改这里要同步改固件里的 `BUZZER_PIN` |

固件不用改别的（`BUZZER_IS_PASSIVE 1` 已经是默认值），烧录后开机就会响一声。

### 方案 B：裸无源蜂鸣器（只有 2 个针脚）+ 一个三极管

如果你买到的是**没有模块**的裸蜂鸣器（直径 12mm、只有 `+` `-` 两个脚），
**不要直接接 GPIO**：蜂鸣器要 20~40mA，而 ESP32 单个 GPIO 建议不超过 20mA，
直连容易烧口、或者让板子反复重启（症状就是"一响就重启"）。用一个小三极管驱动：

```
        3V3 ─────┬─────────────────┐
                 │                 │
              [蜂鸣器 +]      1N4148 续流二极管
                 │          （带标记的一端接 3V3）
              [蜂鸣器 -]           │
                 │                 │
                 ├─────────────────┘
                 │
              ┌──┴──┐    NPN 三极管 (S8050 / 2N2222 / 8050)
   GPIO4 ──1kΩ──┤ B   │    C = 集电极（接蜂鸣器 -）
              │     │    B = 基极（串 1kΩ 电阻）
              └──┬──┘    E = 发射极（接 GND）
                 │
                GND
```

需要的元件：NPN 三极管 1 个、1kΩ 电阻 1 个、1N4148 二极管 1 个（保护用，建议接）。面包板上几分钟就能搭好。

> 无源蜂鸣器靠频率发声，经过三极管开关后音量会比模块小一些；觉得轻就优先用方案 A 那种带驱动的模块。

### 如果你手上其实是有源蜂鸣器（给电就响的那种）

把固件里 `BUZZER_IS_PASSIVE` 改成 `0` 即可。有源模块（3 针脚）接线一样：
`VCC→3V3`、`GND→GND`、`I/O→GPIO4`，但要注意模块分"高电平触发"和"低电平触发"两种：

| 现象 | 改法 |
| --- | --- |
| 该响时不响、平时一直响 | 触发极性反了，把 `BUZZER_ACTIVE_LEVEL` 在 `HIGH` 和 `LOW` 之间换个方向 |

### 固件里相关的设置

```cpp
#define BUZZER_ENABLED         1     // 1 = 用蜂鸣器; 0 = 不用(没接就填 0)
#define BUZZER_PIN             4     // 信号脚
#define BUZZER_IS_PASSIVE      1     // 1 = 无源蜂鸣器(默认); 0 = 有源蜂鸣器
#define BUZZER_ACTIVE_LEVEL    HIGH  // 仅"有源模块"用: 低电平触发改成 LOW
#define BUZZER_BEEP_ON_BOOT    1     // 开机自检响一声
#define BUZZER_WAITING_BEEPS   3     // 等待审批: 响几声
#define BUZZER_COMPLETED_BEEPS 2     // 完成: 响几声
#define BUZZER_ON_INTERRUPTED  0     // 中断是否也响
#define APPROVAL_REMIND_MS     30000UL  // 等待期间每 30 秒补一声; 0 = 不补
#define APPROVAL_REMIND_MAX    5     // 最多补几次; 0 = 不限

// 无源蜂鸣器的发声频率(Hz): 常见谐振点在 2~3kHz, 这个区间最响
#define TONE_LOW   1800              // "完成"的第一声(低)
#define TONE_HIGH  2700              // 提示音主频率(等待审批用它)
#define TONE_ALERT 900               // "中断"音
```

### 验证接线是否成功

三种办法，任选其一：

1. **上电自检**：烧录后按一下板子复位键（或重新插 USB），会先白闪 3 下，然后**响一声**——响了就说明接线没问题。
2. **网页按钮**：浏览器打开 `http://<板子IP>`，点"测试蜂鸣器"。
3. **命令行**：`python -m codex_notify device --beep`，或直接
   `curl "http://192.168.1.50/api/beep?n=2&ms=120"`。

### 嫌吵 / 嫌哑？这样调

- 嫌吵：`BUZZER_WAITING_BEEPS` 改成 1~2、`APPROVAL_REMIND_MS` 改成 0（不补提醒）、或者 `BUZZER_ENABLED` 直接改 0。
- 音调太尖或太闷：把 `TONE_HIGH` 在 2000~3500 之间试，无源蜂鸣器换频率音量差别很明显。
- 觉得太轻：确认 `VCC` 接的是 3V3；有源/无源别搞混（无源配 3 脚模块最响）。

## 3. Arduino IDE 设置

打开 `ESP32S3_WebLED.ino`，在 **工具 → 开发板** 里按下表配置：

| 选项 | 值 | 说明 |
| --- | --- | --- |
| 开发板 | **ESP32S3 Dev Module** | |
| Flash Size | **16MB (128Mb)** | N16 = 16MB Flash |
| PSRAM | **OPI PSRAM** | R8 = 8MB Octal SPI PSRAM，**必须选 OPI** |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) | 或 Default 16MB |
| USB CDC On Boot | **Enabled** | 用板载 USB 口看串口日志时必须开启 |
| Upload Speed | 921600 | |
| Port | 选择对应的 COM 口 | |

> 若串口监视器没有输出，把 `USB CDC On Boot` 设为 Enabled 后重新编译烧录。

## 4. 烧录前要改的几行

都在 `.ino` 顶部的"用户配置区"：

```cpp
// 1) WiFi —— 必须是 2.4GHz，ESP32-S3 不支持 5GHz
const char *WIFI_SSID     = "WIFI";
const char *WIFI_PASSWORD = "PASSWORD";

// 2) LED（默认就行；板载是普通单色灯才改）
#define LED_IS_WS2812   1       // 1 = WS2812 RGB 灯珠; 0 = 普通单色 LED
#define LED_PIN         48      // ESP32-S3-DevKitC-1 常见 48，少数板子是 38
#define LED_BRIGHTNESS  120     // 亮度 0~255

// 3) 蜂鸣器（见第 2 节；没接就改成 0）
#define BUZZER_ENABLED     1
#define BUZZER_PIN         4
#define BUZZER_IS_PASSIVE  1    // 无源蜂鸣器=1（默认），有源蜂鸣器=0

// 4) 功耗（板子烫手就开这个，见第 10 节）
#define LOW_POWER          1    // 1 = 省电(推荐); 0 = 性能优先
```

### 建议：给板子固定 IP

因为电脑端要按 IP 把状态推过来，**板子 IP 变了就要改配置**，所以推荐固定 IP：

```cpp
#define USE_STATIC_IP 1                 // 打开固定 IP
const char *STATIC_IP      = "192.168.1.50";   // 挑一个路由器网段内没被占用的地址
const char *STATIC_GATEWAY = "192.168.1.1";
const char *STATIC_SUBNET  = "255.255.255.0";
const char *STATIC_DNS     = "192.168.1.1";
```

嫌麻烦就用默认的 DHCP，从串口日志里记下它拿到的 IP（也可以用端口转发，电脑端填转发地址+端口即可）。

## 5. 烧录与查看

1. 按住开发板 **BOOT** 键，插入 USB，再松开（多数板子用原生 USB 口可直接烧录，无需按键）。
2. Arduino IDE 点击 **上传**。
3. 打开 **串口监视器**（波特率 `115200`），会看到：

```
========================================
  ESP32-S3-N16R8  Codex 状态灯
========================================
  Flash : 16 MB    PSRAM: 8192 KB
  CPU   : 160 MHz     功耗模式: 省电(推荐)
[WiFi] 连接成功!
[PWR] 省电模式: WiFi modem sleep 开, 发射功率 11dBm
[WiFi] 本机 IP 地址: 192.168.1.50
[HTTP] 服务器已启动 (端口 80)
[HTTP] 状态接口: http://192.168.1.50/api/codex
========================================
```

**把这里打印的 IP（或你的转发地址+端口）填到电脑上 `codex-feishu-notify/config.json` 的 `device` 里**
（见第 8 节）。之后每次收到推送还会打印：

```
[CODEX] 状态 -> waiting_approval (等待审批)  文本: 需要确认是否删除 build 目录
[CODEX] 来自 192.168.1.23
[BEEP] 等待审批提醒 1 次
```

## 6. 网页控制

浏览器打开 `http://<板子IP>`（或 mDNS 的 `http://esp32s3.local`）：

- 中间的大圆点就是当前状态的颜色，下面显示状态名、任务名、最近一次推送的说明。
- **跟随 Codex（自动）**：默认模式，灯由电脑推来的状态决定。
- **测试蜂鸣器**：响两声，用来确认蜂鸣器接线，不需要电脑参与。
- **手动开灯 / 手动关灯**：自己控制（白光），会切到手动模式；再点"跟随 Codex"就切回来。
  （收到新的状态推送时也会自动切回跟随模式。）

## 7. 接口一览

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 网页控制界面 |
| **POST** | **`/api/codex`** | **接收状态推送（电脑端调用的就是这个）** |
| GET | `/api/codex?status=working&text=xx` | 浏览器里手动测状态 |
| GET | `/api/codex` | 只查询当前状态，不改灯 |
| GET | `/api/beep?n=2&ms=120` | 测试蜂鸣器（`n` 声数，`ms` 每声时长） |
| GET | `/api/status` | 设备状态 JSON（IP / 信号 / 运行时间 / 当前颜色 / 是否启用蜂鸣器） |
| GET | `/api/led?state=1` | 手动开灯（`1`/`on`/`off`/`0`/`toggle`） |
| GET | `/api/led/toggle` | 手动翻转 |
| GET | `/api/mode?m=auto` | 切回跟随 Codex（`m=manual` 切手动） |

`POST /api/codex` 的请求体（电脑端自动发送）：

```json
{ "status": "working", "text": "开始整理数据", "thread": "演示任务",
  "source": "会话日志", "agent": "codex" }
```

### 两个来源，互不覆盖

`agent` 字段用来区分是谁在推状态（`codex` / `claude`，不填按 `codex` 算）。
板子为每个来源**各存一份状态**，灯只显示"更紧急"的那个：

```
等待审批 > 已中断 > 工作中 > 已完成 > 通知 > 空闲
（优先级相同就显示最近更新的那个）
```

所以 Codex 在跑的同时 Claude Code 完成了任务，**不会把 Codex 的"工作中"顶掉**——
只有 Claude 进入更紧急的状态（比如等你审批）才会接管灯。网页上两个来源各显示一行，
粗体的那个就是当前灯上显示的状态；串口日志里也会带上来源名：

```
[Codex] 状态 -> working (工作中)  文本: 开始整理数据
[Claude Code] 状态 -> completed (已完成)  文本: 已经改好了
```

**提示音和提醒都是各算各的**：谁的状态变化谁就出声；等待审批的周期性提醒按来源独立计数，
所以两边都在等审批时，会分别提醒（日志里能看到 `[提醒] Codex 仍在等待审批…` /
`[提醒] Claude Code 仍在等待审批…`）。优先级只影响**灯显示哪一个**，不影响出声。

`status` 中英文都认：

| 含义 | 可用值 |
| --- | --- |
| 工作中 | `working` / `work` / `busy` / `running` / `工作中` / `运行中` |
| 等待审批 | `waiting_approval` / `waiting` / `approval` / `permission` / `等待审批` |
| 已完成 | `completed` / `done` / `finished` / `已完成` / `完成` |
| 已中断 | `interrupted` / `aborted` / `failed` / `已中断` / `失败` |
| 空闲 | `idle` / `空闲` / `不工作` |
| 通知 | `info` / `通知` |

`GET /api/status` 返回示例：

```json
{
  "ok": true, "led": true, "mode": "auto", "buzzer": true,
  "codex": "waiting_approval", "codex_label": "等待审批", "color": "#FF0000",
  "text": "需要确认是否删除 build 目录", "thread": "演示任务",
  "last_push_age": 3, "ip": "192.168.1.50", "rssi": -52,
  "uptime": 128, "heap": 254000, "link": "connected"
}
```

## 8. 和电脑端（codex-feishu-notify）对接

在 `outputs/codex-feishu-notify/config.json` 里填板子地址（直连局域网就填板子 IP；
走端口转发就填转发地址 + 对外端口）：

```json
{
  "device": {
    "enabled": true,
    "ip": "192.168.1.50",
    "port": 80,
    "path": "/api/codex"
  }
}
```

> 想**同时**推给多个设备（比如这块板子 + 一台跑语音播报的 Ubuntu），把地址写到
> `config.json` 的 `devices` 数组里即可，详见电脑端 README 的 13.2.1 节。

改完在电脑上验证（只推板子、不发飞书，适合反复调灯和调音）：

```powershell
cd <codex-feishu-notify 所在目录>
python -m codex_notify device --probe                    # 板子在不在线
python -m codex_notify device --beep                     # 让板子响两声(试蜂鸣器)
python -m codex_notify device --status waiting_approval  # 红灯呼吸 + 三声提示音
python -m codex_notify device --status completed         # 蓝灯 + 完成提示音
python -m codex_notify device --status idle              # 绿灯
```

之后正常跑监控（`scripts\start.ps1`），每次状态变化都会**同时**推飞书和板子；
板子不在线只会记一条调试日志，绝不影响飞书推送。

## 9. 常见问题

| 现象 | 排查方法 |
| --- | --- |
| 串口无输出 | 工具里把 `USB CDC On Boot` 设为 **Enabled** 后重新烧录 |
| 连不上 WiFi | 确认是 2.4GHz 网络；SSID/密码无误；企业认证（Portal）网络连不上 |
| PSRAM 显示 0 KB | 开发板设置里 `PSRAM` 改成 **OPI PSRAM** 后重新烧录 |
| LED 不亮或一直白 | 板载可能是普通单色 LED：把 `LED_IS_WS2812` 改为 `0`、`LED_PIN` 改为 `2` |
| LED 一直不变色 | 用 `python -m codex_notify device --probe` 看板子在不在线；再确认 config.json 的 IP/端口对不对 |
| 蜂鸣器完全不响 | ① `BUZZER_ENABLED` 是不是 0；② 万用表确认 VCC 有 3.3V；③ 有源/无源搞反了（无源要 `BUZZER_IS_PASSIVE 1`，有源要 `0`）；④ 有源模块触发极性反了，改 `BUZZER_ACTIVE_LEVEL` |
| 蜂鸣器一直响 | 有源模块触发极性反了：`BUZZER_ACTIVE_LEVEL` 换个方向 |
| 只有很轻的"嗒"声、没有音调 | 有源/无源弄反了，把 `BUZZER_IS_PASSIVE` 换成另一个值 |
| 按复位就重启 / 串口反复打印 | 蜂鸣器电流太大导致掉电（裸蜂鸣器直连 GPIO 的典型症状），按第 2 节方案 B 加三极管驱动 |
| 网页打不开 | 确认手机/电脑与板子同网段；路由器若开了 AP 隔离，电脑就连不上板子 |
| 板子摸着烫手 | 通常正常（最烫的是 3.3V 稳压器）。确认 `LOW_POWER` 是 1，详见第 10 节 |

## 10. 功耗与发热（省电开关）

板子摸着温热是正常的——最烫的通常是那颗 **3.3V 稳压器**（把 USB 的 5V 降到 3.3V，
多出来的电压全变成热）。判断标准：**能用手指按住 5 秒左右（约 50~60°C）属于正常**；
碰一下就疼、有异味、或者伴随随机重启，才需要认真排查（多半是供电或外设接错）。

固件顶部有一个总开关：

```cpp
#define LOW_POWER       1     // 1 = 省电(推荐); 0 = 性能优先
```

| | 省电模式 `LOW_POWER 1` | 性能模式 `LOW_POWER 0` |
| --- | --- | --- |
| WiFi | modem sleep **开**（射频按需唤醒） | 射频常开（**这是原来发热的主因**） |
| CPU | 160MHz | 240MHz |
| 发射功率 | 11dBm | 默认 20dBm |
| 灯光动画刷新 | 40ms | 20ms |
| 平均电流（5V 侧） | 约 **45~70mA** | 约 **100~140mA**，峰值 300mA |
| 副作用 | HTTP 首次响应慢 100~200ms | 无 |

推送和灯**完全不受影响**：modem sleep 只是让射频在没数据时打盹，收到请求照样能回，
只是第一次可能慢个一两百毫秒（我们的推送超时是 2 秒，完全够）。

还能再省一点：

- `LED_BRIGHTNESS` 从 120 降到 60（灯珠本身不热，但更省电、也更不刺眼）。
- 想更狠可以把 `setCpuFrequencyMhz(160)` 改成 `80`，不过个别核心版本在 80MHz 下 WiFi 兼容性略差，出问题就退回 160。
- **如果最烫的是稳压器**：用外部 3.3V 电源直接喂板子的 `3V3` 脚（此时**绝对不要同时插 USB**），旁路掉稳压器就基本不热了。

另外两点：**蓝牙全程没有开**（ESP32 的蓝牙比 WiFi 更耗电），一开始就省掉了；
开机时串口会打印当前 CPU 频率和功耗模式，方便你确认开关有没有生效。

## 11. 说明

- 板子只**被动接收**状态，电脑端只做单向推送；板子不会反向控制你的电脑。
- 固件用 Arduino 核心自带的 `rgbLedWrite()`（核心 3.x）或 `neopixelWrite()`（核心 2.x）
  驱动 WS2812，**不需要安装任何额外库**；蜂鸣器用核心自带的 `tone()` / `noTone()`
  （有源蜂鸣器则用 `digitalWrite`），同样不需要装库。
- 蜂鸣器播放用的是**非阻塞小队列**（`beepPush()` / `beepUpdate()`），
  响铃期间 HTTP 服务照常响应，不会卡住推送。
- 网页 HTML 放在 Flash（`PROGMEM`）里，不占 RAM。
- 兼容 ESP32 Arduino 核心 2.x 与 3.x。
