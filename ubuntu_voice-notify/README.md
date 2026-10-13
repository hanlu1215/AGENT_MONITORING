# codex-voice-notify · Ubuntu 语音播报服务

跑在 Ubuntu 上的小服务，干两件事：

1. **接收 Codex 状态推送**，用扬声器发出不同的提示音（相当于 ESP32 那块板子的角色，只是把灯换成声音、不需要灯）；
2. **提供一个网页**，在手机或电脑上输入文字，服务器收到后用**女声朗读**出来。

接口和 ESP32 那块板子**完全一样**（`POST /api/codex`、`GET /api/beep`、`GET /api/status`），
所以电脑端的 `codex-feishu-notify` 不用改代码，只要在 `devices` 里加上这台机器就行。

```
codex-voice-notify/
├── server.py                    ← 服务本体（只用 Python 标准库）
├── config.json                  ← 配置：端口、提示音、女声、静音
├── codex-voice-notify.service   ← systemd 单元模板（开机自启）
├── tools/selftest.py            ← 离线自测（不需要声卡）
└── audio_cache/                 ← 自动生成：合成好的提示音 WAV
```

---

## 1. 装依赖

服务本身只用标准库，但**发声**要靠系统里的播放器和 TTS，建议装这三样：

```bash
# 1) 播放器（装了哪个就用哪个）
sudo apt install -y pulseaudio-utils     # 提供 paplay（PulseAudio 桌面）
# 或者纯 ALSA：sudo apt install -y alsa-utils      （提供 aplay）
# Ubuntu 22.10+ 默认 PipeWire，一般自带 pw-play

# 2) 离线 TTS（保底，务必装上）
sudo apt install -y espeak-ng            # 中文女声 cmn+f3，音质一般但离线可靠

# 3) 想听"自然的女声"就再装这个（需要能上外网）
pip install edge-tts
```

三种 TTS 的取舍：

| 后端 | 音质 | 联网 | 中文女声 |
| --- | --- | --- | --- |
| **edge-tts** | ★★★★★ 自然 | 需要 | `zh-CN-XiaoxiaoNeural`（晓晓，默认）、`zh-CN-XiaoyiNeural` 等 |
| piper | ★★★★ 不错 | 不需要 | `zh_CN-huayan-medium`（要自己下模型） |
| espeak-ng | ★★ 机器人味 | 不需要 | `cmn+f3` |

看看本机认到了什么：

```bash
python3 server.py --list-tools
```

输出里「播放器」和「TTS 后端」都不是"未找到"就能用。

## 2. 跑起来

```bash
cd codex-voice-notify
python3 server.py                        # 默认监听 0.0.0.0:8080
python3 server.py --port 8081            # 换端口
python3 server.py --test-sounds          # 依次播放五种状态的提示音（调音量用）
python3 server.py --say "你好，我是语音播报"   # 直接试一句朗读
python3 server.py --mute                 # 启动时静音（只跑网页和接口）
python3 server.py --diagnose             # 音频体检：播放器/输出设备/TTS 一次查清
python3 server.py --warmup               # 预合成 config 里 tts.warmup 的常用短语（点它们就秒出声）
```

启动后终端会打印：

```
  网页     : http://<本机IP>:8080/
  推送接口 : http://<本机IP>:8080/api/codex
```

同一局域网的手机/电脑打开那个网址即可。Ubuntu 防火墙开着的话放行端口：

```bash
sudo ufw allow 8080/tcp
```

## 3. 和电脑端（codex-feishu-notify）对接

在 Windows 那台机器的 `config.json` 里，把这台 Ubuntu 加进设备列表（可以和 ESP32 一起推）：

```json
{
  "devices": [
    { "name": "esp32-led",    "ip": "192.168.1.50", "port": 80,   "path": "/api/codex" },
    { "name": "ubuntu-voice", "ip": "192.168.1.88", "port": 8080, "path": "/api/codex" }
  ]
}
```

用现成的设备命令验证（只推设备、不发飞书）：

```powershell
python -m codex_notify check                             # 看识别到了几个目标
python -m codex_notify device --probe                    # 逐个探测在不在线
python -m codex_notify device --beep                     # 让两台设备都响两声
python -m codex_notify device --status waiting_approval  # 播"等待审批"的提示音
```

之后正常跑监控，**每次状态变化会：先并行推所有设备（局域网毫秒级，马上出声）再推飞书**。
某个设备不通只会记一行日志，不影响其它设备和飞书。

## 4. 状态 → 提示音

| Codex 状态 | 声音 | 说明 |
| --- | --- | --- |
| **工作中** | "嘀"一声（880Hz）**+ 念一句"开始工作"** | 每次开工播报一次 |
| **等待审批** | "嘀嘀嘀"三声（2700Hz）**+ 念一句"等待审批"** | 之后**每 30 秒补一声**滴滴（不重复念话），最多 5 次，直到状态改变 |
| **已完成** | 一低一高（1500→2700Hz）**+ 念一句"任务已完成"** | 像"搞定"的上扬音，紧跟一句语音 |
| **已中断** | 两声低沉（900→700Hz）**+ 念一句"任务中断"** | 一听就知道出事了 |
| 通知 | 一声清亮（2000Hz） | |
| 空闲 | 无声 | |

提示音是**服务自己用正弦波合成**的（标准库 `array` + `wave`），不需要 sox、beep 之类工具，
也不用准备音频文件。想改音色就改 `config.json` 的 `sounds`，每项是 `[频率Hz, 时长ms]`，
频率写 `0` 就是静音间隔，例如：

```json
"waiting_approval": [[2700, 90], [0, 80], [2700, 90], [0, 80], [2700, 120], [0, 100]]
```

改完重启服务即可（换了参数会自动生成新的 WAV）。

如果还想让它在等待审批时**把推送的文字读出来**（例如"需要确认是否删除 build 目录"），
把 `config.json` 里这段打开：

```json
"announce": { "enabled": true, "statuses": ["waiting_approval", "completed"] }
```

### 固定提示语（"滴滴"之后紧跟一句语音）

`announce.phrases` 里可以给每个状态配一句固定台词，会**在提示音之后**念出来：

```json
"announce": {
  "phrases": {
    "working": "{agent} 开始工作",
    "waiting_approval": "{agent} 需要审批",
    "completed": "{agent} 任务已完成",
    "interrupted": "{agent} 任务中断"
  }
}
```

`{agent}` 会替换成来源名——**Codex** 或 **Claude Code**。所以推送来自 Claude Code 时，
你听到的是"Claude Code 需要审批"，来自 Codex 时是"Codex 需要审批"，一听就知道是谁在找你。

来源靠推送里的 `agent` 字段区分（`codex` / `claude`，不填按 Codex 算）。
启动预热会把**两种说法都**合成好，所以哪一边第一次触发都是秒出声。
另外 `GET /api/status` 的返回里也带了 `agent` 和 `agent_label`，网页标题和最近推送里同样能看到。

### 两个来源完全独立（Codex / Claude Code）

服务给每个来源**各留一份状态**，互不覆盖也不互相打断：

| | 行为 |
| --- | --- |
| 播报 | 谁的状态变了就播报谁——Codex 进了等待审批、Claude 完成任务，两声会先后都响 |
| 提醒 | **各算各的**：两边都在等审批时，会分别按 `interval_sec` 的节奏提醒 |
| 网页 | 两个来源各显示一行（带各自颜色）；大圆点显示更紧急的那个 |
| 接口 | `GET /api/status` 的 `agents` 数组同时给出两边状态；顶层 `codex`/`agent` 仍是"大圆点显示的那个"，兼容旧调用 |

优先级（决定大圆点显示谁）：`等待审批 > 已中断 > 工作中 > 已完成 > 通知 > 空闲`，
同级比"谁更新得晚"。注意：**这只影响那两个大圆点/灯**，播报和提醒始终是两条独立的路。

两个要点：

- **只合成一次**：这些台词会走和网页朗读同一条缓存（`tts.cache`），第一次合成后存进
  `audio_cache/cache_*.mp3`，之后每次触发都直接调缓存文件，**不再联网合成、零等待**。
- **启动即预热**：服务启动时会自动把 `announce.phrases` 和 `tts.warmup` 里的句子后台合成一遍，
  所以第一次真的遇到"等待审批"时也是秒出声。也可以手动跑：`python3 server.py --warmup`。

想改台词就改这里的文字（改了会自动重新合成一次）；不想要某个状态说话，把那一行删掉即可。
周期性提醒（等待审批每 30 秒那声）只放滴滴，不会重复念台词，免得吵。

## 5. 接口一览

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 网页（朗读 + 状态显示 + 测试按钮） |
| **POST** | **`/api/codex`** | **接收状态推送（和 ESP32 同接口）** |
| GET | `/api/codex?status=working&text=xx` | 浏览器里直接测 |
| GET | `/api/beep?n=2&ms=120` | 响两声（和 ESP32 同接口） |
| GET | `/api/status` | 当前状态 JSON（含最近 20 条历史） |
| POST | `/api/speak` | `{"text":"要念的内容","voice":"可选"}` → 女声朗读 |
| POST | `/api/mute` | `{"mute":true}` 静音；GET `/api/mute?m=1` 也行 |
| GET | `/health` | 健康检查（能看到播放器、TTS 后端、是否静音） |
| GET | `/help` | 接口速查 |

`POST /api/codex` 请求体：

```json
{ "status": "waiting_approval", "text": "需要确认是否删除 build 目录", "thread": "演示任务", "source": "会话日志" }
```

`status` 中英文都认：`working` / `工作中`、`waiting_approval` / `等待审批`、
`completed` / `已完成`、`interrupted` / `已中断`、`idle` / `空闲`、`info` / `通知`。

命令行测试：

```bash
curl -X POST http://127.0.0.1:8080/api/codex \
  -H "Content-Type: application/json" \
  -d '{"status":"waiting_approval","text":"需要确认"}'
curl -X POST http://127.0.0.1:8080/api/speak \
  -H "Content-Type: application/json" -d '{"text":"任务完成了，请检查"}'
```

## 6. 网页怎么用

浏览器打开 `http://<这台机器的IP>:8080/`：

- **上面**显示状态（颜色和 ESP32 灯是同一套：空闲绿、工作中黄、等待审批红、完成蓝、中断紫、通知青）：
  大圆点显示更紧急的那个来源，下面两个来源（Codex / Claude Code）**各显示一行**，互不覆盖。
  （没有 `agents` 字段的旧推送也能正常显示。）
- **中间**输入任意文字，点「🔊 朗读」（或按 `Ctrl+Enter`）——服务器会用女声念出来。
  下面还有几个常用短语的快捷按钮，点一下直接播。
- **「🔕 静音」**：提示音和朗读都不出声（网页照常工作），再点一下恢复。
- **「测试各状态提示音」**：点某个状态就能试听对应的提示音，用来调音量最方便。
- 底部显示运行时间和当前用的播放器 / TTS 后端，还有一条操作日志。

## 7. 女声怎么调

**如果现在听起来像机器人**，那是 `espeak-ng` 的声音——它是共振峰合成，天生就这个味，
调参数只能让它"没那么难听"，想真正拟人必须换成**神经语音**。按这个顺序升级：

**① edge-tts（推荐，最像真人，免费）**

```bash
pip install edge-tts
python3 server.py --list-voices      # 列出所有可选中文音色，带男女标注
# 然后重启服务即可：backend 默认 auto，会自动优先用 edge-tts
```

它用的是微软的神经语音（晓晓等），**需要能上外网**。装好后 espeak-ng 自动退化成"没网时的备用"。

**② piper（离线神经语音，音质仅次于 edge）**

没有外网就用它：下载 `piper` 可执行文件 + 中文模型（`zh_CN-huayan-medium.onnx` 和同名 `.json`），然后：

```json
"tts": { "backend": "piper", "piper_model": "/opt/piper/zh_CN-huayan-medium.onnx" }
```

**③ 只能留在 espeak（离线最省事，但始终是机器人音）**

把语速放慢、音调压低会稍微自然一点：

```json
"tts": { "espeak_voice": "cmn+f3", "espeak_speed": 145, "espeak_pitch": 42 }
```

都在 `config.json` 的 `tts` 段：

| 字段 | 说明 |
| --- | --- |
| `backend` | `auto`（推荐）/ `edge` / `piper` / `espeak`，指定了就优先用它 |
| `voice` | edge-tts 的音色，默认 `zh-CN-XiaoxiaoNeural`（晓晓，女声） |
| `rate` | 语速，如 `+20%` 更快、`-10%` 更慢 |
| `volume` | 音量偏移，如 `+30%` |
| `pitch` | 音调，如 `-20Hz` 更低沉柔和（晓晓默认偏高，降一点更像真人） |
| `espeak_voice` | espeak-ng 的音色，`cmn+f3` 是中文女声 |
| `espeak_speed` / `espeak_pitch` | 仅 espeak 用：语速（字数/分钟，默认 160）和音调（0~99，默认 50） |
| `cache` | 是否缓存合成结果（默认 `true`），同一句话第二次说是秒出声 |
| `warmup` | 启动时预合成的常用短语数组，例如 `["吃饭了", "任务已完成，请检查"]` |
| `piper_model` | piper 的模型路径（用 piper 才需要） |
| `max_chars` | 单条最长朗读字数，默认 300，超出截断 |

常用中文女声（edge-tts）：

| 音色 | 特点 |
| --- | --- |
| `zh-CN-XiaoxiaoNeural` | 晓晓，通用自然（默认） |
| `zh-CN-XiaoyiNeural` | 晓伊，声音更甜一点 |
| `zh-CN-liaoning-XiaobeiNeural` | 辽宁小北，东北口音 |
| `zh-TW-HsiaoChenNeural` | 台湾国语女声 |

查全部可选音色（会标出当前在用的那个）：`python3 server.py --list-voices`。

### 让出声更快（延迟是从哪来的）

一次朗读的总时间 = **TTS 合成** + **转码** + **播放器启动**：

| 环节 | 典型耗时 | 怎么省 |
| --- | --- | --- |
| edge-tts 联网合成 | **1~4 秒**（看得到微软服务器的网络） | 用缓存（重复的话 = 0）；或改用本地合成的 piper |
| mp3 → wav 转码 | ~0.5 秒 | `sudo apt install -y mpg123`，有原生 mp3 播放器就不转码 |
| 播放器试错 | 首次 ~0.5 秒，之后 0 | 服务会记住上次成功的播放器，重启也保留 |

服务内置了三个提速手段：

1. **结果缓存**（`tts.cache`，默认开）：同一句话第二次说直接播缓存，**零等待**。
   缓存按"文本 + 音色 + 语速 + 音调"算 key，改了参数会自动重新合成，文件在 `audio_cache/cache_*`。
2. **启动预热**（`tts.warmup`）：启动时后台把这些常用短语合成好，网页上点它们就是秒出声。
   改完配置也可以手动跑：`python3 server.py --warmup`。
3. **优先用 Python 库合成**：装了 `edge-tts` 就用它的库接口（少一次进程启动，省 0.3~1 秒），失败才回退命令行。

想**彻底摆脱网络延迟**，就换成本地合成的 **piper**（见上面第 ② 条）：短句大约 0.3~0.5 秒出声，
而且断网也能用，代价是要先下 60MB 左右的模型。

## 8. 开成系统服务（可选，推荐）

**方式一：用户级 systemd（推荐）**

因为 PulseAudio / PipeWire 是"每个登录用户"的，服务挂在你的用户会话下最不容易出无声问题：

```bash
mkdir -p ~/.config/systemd/user
cp codex-voice-notify.service ~/.config/systemd/user/
# 编辑这个文件：删掉 User= 和 Group= 两行，把 WorkingDirectory / ExecStart 改成实际路径，
# 并把 [Install] 里的 WantedBy=multi-user.target 改成 WantedBy=default.target
systemctl --user daemon-reload
systemctl --user enable --now codex-voice-notify
journalctl --user -u codex-voice-notify -f      # 看日志
```

还要让服务在你不登录时也保持运行（不然注销就停了）：

```bash
sudo loginctl enable-linger $USER
```

**方式二：系统级 systemd**

把 `codex-voice-notify.service` 里的 `User=%i` / `Group=%i` 改成你的用户名，
`WorkingDirectory` / `ExecStart` 改成实际路径，然后：

```bash
sudo cp codex-voice-notify.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now codex-voice-notify
```

注意：系统级服务跑在 root 的会话里，**默认访问不到你的桌面音频**。所以文件里预置了
`XDG_RUNTIME_DIR` 和 `PULSE_SERVER` 两行环境变量（其中 `1000` 要换成你的用户 ID，
用 `id -u` 查）。如果还是没声音，就用方式一。

## 9. 常见问题

| 现象 | 排查 |
| --- | --- |
| **完全没有声音** | 先跑 `python3 server.py --diagnose`：它会逐个播放器试播并打印返回码，一眼看出是"没有播放器"、"播放器报错"还是"播放成功但你没听到" |
| 日志出现 `[警告] 没找到 TTS` | 这台机器没装任何 TTS，**朗读会降级成两声提示音**（属正常兜底）。装 `sudo apt install -y espeak-ng`（离线）或 `pip install edge-tts`（音质好，要联网） |
| 日志出现 `[错误] paplay 返回 1: ...` | 播放器自己报错了，看后面的英文：常见是"连不上 PulseAudio"（SSH/容器环境）或"设备被占用"。服务会自动换下一个播放器重试 |
| **声音是噪音 / 沙沙声**（日志里有 `Format not recognised`） | 这是 **mp3 被当成裸 PCM 播放**了：你用 edge-tts（只输出 mp3），但 pw-play/paplay 的 libsndfile 没带 mp3 解码，最后落到只能放 wav 的 `aplay`。装个解码器即可：`sudo apt install -y ffmpeg`（推荐，服务会自动把 mp3 转成 wav 再播）或 `sudo apt install -y mpg123` |
| `pactl` 连不上（服务在 SSH/容器里） | 没有音频会话。改在桌面终端跑，或用第 8 节的 `systemd --user` 方式 + `loginctl enable-linger` |
| 播放器成功但听不到 | 输出设备不对：`pactl list short sinks` 看名字 → `pactl set-default-sink <音箱名字>`；PipeWire 用 `wpctl status` / `wpctl set-default <id>` |
| 想分清是系统问题还是服务问题 | 绕过本服务直接测：`speaker-test -c 2 -t sine -f 440`。它也没声就是系统音频问题，跟本服务无关 |
| 提示"没找到播放器" | 装 `pulseaudio-utils`（paplay）或 `alsa-utils`（aplay），PipeWire 桌面通常自带 `pw-play` |
| 有 `[播放]` 但听不到 | ① 输出设备不对：`pactl list short sinks` 看列表，`pactl set-default-sink <名字>` 切换；PipeWire 用 `wpctl status`。② 音量太小：`alsamixer` 或 `pavucontrol` 调 |
| 跑在 SSH 里没声音 | SSH 会话里没有音频会话，属正常。要出声就用 systemd 用户服务（第 8 节方式一），或在桌面终端里跑 |
| 服务重启后没声音 | 多半是 systemd 环境缺 `XDG_RUNTIME_DIR` / `PULSE_SERVER`，见第 8 节 |
| **朗读没声音** | `python3 server.py --list-tools` 看 TTS 后端；没装就用 `sudo apt install espeak-ng`。装了 edge-tts 但报错多半是**没联网**，它会自动回退到 espeak-ng（音质会变机器人） |
| 朗读是男声/女声不对 | 改 `tts.voice`（edge）或 `tts.espeak_voice`（espeak，女声用 `cmn+f3`） |
| **声音像机器人** | 你正在用 espeak-ng（共振峰合成，天生这样）。`pip install edge-tts` 后重启就会自动换成拟人的神经语音；没有外网就上 piper。见第 7 节 |
| 装了 edge-tts 却没生效 | 确认能联网（`edge-tts --text 测试 --write-media /tmp/t.mp3`）；`python3 server.py --list-tools` 看 TTS 后端是不是 `edge`；失败时会自动回退到 espeak，日志里有 `[错误] edge 合成失败` |
| 声音太吵 / 太轻 | `audio.volume` 调 0~1（只影响提示音）；朗读音量用 `tts.volume`，如 `+50%` |
| 想临时全部静音 | 网页上点「静音」，或 `python3 server.py --mute`，或 `curl -X POST .../api/mute -d '{"mute":true}'` |
| 端口被占用 | `python3 server.py --port 8081`，或改 `config.json` 的 `server.port` |
| 想防止别人乱用 | `server.token` 填个随机串，之后请求要带 `Authorization: Bearer <token>`（网页也要 `?token=`） |
| 手机上打不开网页 | 确认手机和这台机器在同一局域网；`ufw allow 8080/tcp`；路由器别开 AP 隔离 |

## 10. 自测

不需要声卡也能验证功能（提示音合成 + 所有接口）：

```bash
python3 tools/selftest.py
```

它会在本机随机端口起一个临时实例，检查：五种提示音的 WAV 合成时长是否正确、
网页能否打开、`/health`、`POST /api/codex`（含中文状态）、`/api/status`、
`/api/beep`、`/api/speak`、静音开关、404/400 等，共 20 项。

这套自测已在 Windows 上跑通（提示音合成和 HTTP 接口与平台无关），
**唯一没法在这台机器上验证的是 Ubuntu 上的实际发声**——那取决于你的声卡、PulseAudio/PipeWire 状态，
所以第一次部署时请务必跑一次 `python3 server.py --test-sounds` 亲耳确认。









Codex 语音播报服务 —— 用户级 systemd 部署

    以用户级 systemd 服务（systemctl --user）方式运行，天然接入当前登录会话的 PulseAudio/PipeWire，声音最稳。

1. 前置权限

确保运行服务的用户（示例为 han）对程序目录有读写权限，否则会出现 [Errno 13] Permission denied：
bash

sudo chown -R han:han /opt/codex-voice-notify

2. 创建用户级 unit 文件

路径：~/.config/systemd/user/codex-voice-notify.service
bash

mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/codex-voice-notify.service <<'EOF'
[Unit]
Description=Codex voice notify (status sounds + text-to-speech web page)
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/codex-voice-notify
ExecStart=/usr/bin/python3 /opt/codex-voice-notify/server.py
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target
EOF

    ⚠️ 注意：codex-voice-notify.service 必须是文件，不能是目录（否则 cat 会报 Is a directory）。

3. 启用并启动
bash

systemctl --user daemon-reload
systemctl --user enable --now codex-voice-notify

4. 查看状态与日志
bash

# 当前状态
systemctl --user status codex-voice-notify

# 实时日志
journalctl --user -u codex-voice-notify -f

5. 注销后仍运行（可选）

默认情况下，用户注销后用户级服务会停止。若希望后台常驻：
bash

sudo loginctl enable-linger han

取消常驻：
bash

sudo loginctl disable-linger han

6. 停止 / 重启 / 关闭服务
bash

# 立即停止（本次）
systemctl --user stop codex-voice-notify

# 重启
systemctl --user restart codex-voice-notify

# 取消开机/登录自启，并停止
systemctl --user disable --now codex-voice-notify

7. 完全卸载
bash

# 停用并禁用
systemctl --user disable --now codex-voice-notify

# 删除 unit 文件
rm ~/.config/systemd/user/codex-voice-notify.service

# 重新加载 systemd 用户实例
systemctl --user daemon-reload

# （可选）清理常驻
sudo loginctl disable-linger han

8. 常见问题
现象	原因 / 处理
cat: ... Is a directory	unit 路径被建成了目录，需删掉目录、改建成文件
[Errno 13] Permission denied	缓存/程序目录权限不对，执行第 1 步 chown
服务在跑但没声音	确认是用户级服务、已登录同一会话；必要时检查 pactl info
注销后服务停了	执行 sudo loginctl enable-linger han
