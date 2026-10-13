# win_agent-notify-C

Windows 原生 GUI 程序，用于监控 Codex 和 Claude Code 的工作状态，并把状态发送到飞书、ESP32 或其他 HTTP 接收端，也可在本机播放提示音和语音。

程序只构建一个图形界面版：`win_agent-notify-C.exe`。正常启动不会打开命令提示窗口。核心代码使用 C++17 和 Windows 自带 API；Edge TTS 语音合成是可选功能。

## 1. 快速开始

1. 启动 `win_agent-notify-C.exe`，首次启动时程序会自动创建 `config.json`。
2. 在 GUI 中配置飞书和设备地址，勾选需要启用的推送通道。
3. 点击「启动监控」，再用「测试推送」验证各通道。

首次生成配置时默认只勾选「本机提示音」和「本机朗读」；飞书、`esp32-led` 和 `ubuntu-voice` 均默认关闭。飞书 Webhook 和两个设备 URL 是示例地址，使用前请在 GUI 中换成自己的地址并手动勾选启用。签名密钥默认为空。`config.json` 含有本机设置和访问凭据，根目录 `.gitignore` 会忽略它；不要把它提交到代码仓库。

## 2. GUI 用法

| 控件 | 作用 |
| --- | --- |
| Codex 会话日志 | 开启或停止读取本机 Codex 会话记录 |
| Claude Code（只读） | 开启或停止读取本机 Claude Code 会话状态 |
| 飞书勾选框 | 控制是否向飞书推送 |
| 飞书「设置…」 | 设置机器人 Webhook 和签名密钥。保存时密钥输入框为空即清除密钥、不使用签名认证 |
| 设备勾选框 | 每个接收端单独控制是否推送 |
| 设备「设置…」 | 修改该接收端的完整 HTTP URL，支持 IP 或域名 |
| 本机提示音 / 本机朗读 | 控制本机声音通知 |
| 开启隐私屏幕 | 用全屏界面遮住桌面；这不是 Windows 安全锁屏，agent 仍会继续工作 |
| 启动监控 / 停止监控 | 启动或停止状态监控和本地 HTTP 接口 |
| 测试推送 | 向勾选的通道发送测试状态 |
| 重置状态 | 将设备状态重置为空闲 |

GUI 设置保存到程序目录的 `config.json`，保存后立即生效。手动编辑配置后，点任一勾选框可保存当前选项；也可以重启程序使配置重新加载。

## 3. 手动构建 GUI 程序

### 工具链

Windows 10/11 64 位，安装 MSYS2 的 UCRT64 工具链以及 CMake、Ninja：

```powershell
winget install MSYS2.MSYS2
```

安装 MSYS2 后，在「MSYS2 UCRT64」终端执行：

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

### 配置和构建

在 PowerShell 中切换到 `win_agent-notify-C` 目录，执行：

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"

cmake -S . -B build -G Ninja `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_RC_COMPILER=C:/msys64/ucrt64/bin/windres.exe `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --target win_agent-notify-C --parallel
```

编译成功后，GUI 程序生成在项目根目录：`win_agent-notify-C.exe`。构建过程没有批处理脚本，也不会生成控制台版。`build/` 是可随时删除并重新生成的中间文件目录。

如果 MSYS2 安装位置不同，把上面的编译器路径改成实际的 UCRT64 `g++.exe` 和 `windres.exe` 路径。图标资源来自 `res/app.ico`；更换图标后重新构建即可。

## 4. 配置与迁移

`config.json` 是本机配置文件，由首次启动时自动生成。默认包含 `esp32-led` 和 `ubuntu-voice` 两个设备项，两者初始 URL 均为 `http://127.0.0.1:12345/api/codex`（示例地址）；飞书默认 Webhook 为 `https://open.feishu.cn/open-apis/bot/v2/hook/xxxxx`，签名密钥为空。使用前应在 GUI 中改成实际服务地址。设备只使用完整 `url` 字段，例如：

```json
{
  "name": "ubuntu-voice",
  "enabled": true,
  "url": "http://ubuntu-voice.example:12380/api/codex",
  "timeout_sec": 5.0,
  "retries": 2
}
```

把示例域名改成接收端的 IP 或 DNS 名称。URL 必须包含协议、端口和完整路径。GUI 的「设置…」会修改对应设备的 `url`。

旧版本设备配置中的 `ip`、`port`、`path` 会在启动时合并成一个 `url` 并保存；新配置无需再使用这些字段。保留 `enabled` 控制开关、`timeout_sec` 请求超时和 `retries` 重试次数。

常用配置：

| 字段 | 说明 |
| --- | --- |
| `feishu.webhook_url` | 飞书机器人 Webhook；也可从 GUI 设置 |
| `feishu.secret` | 可选签名密钥；机器人启用了签名校验时填写 |
| `feishu.on` | 飞书接收的状态，默认仅 `waiting_approval` 和 `completed` |
| `devices[]` | 接收端列表，每个对象含名称、`enabled`、`url` 和可选超时/重试设置 |
| `local_sound.speech_backend` | `edge` 使用 Edge TTS；`sapi` 使用 Windows 系统语音 |
| `api.host` / `api.port` | 本程序本地 HTTP 接口监听地址和端口，默认 `127.0.0.1:17865` |
| `api.token` | 非空时，本地 API 要求 `Authorization: Bearer <token>`；接收端设备推送不使用这个 token |

程序会读取本机的 Codex / Claude Code 会话数据。换电脑时需在新电脑上安装对应 agent，监控才能发现其会话。

## 5. 接收端 HTTP 协议

每个启用的设备目标收到一个 HTTP `POST`。程序把 `devices[].url` 中的地址原样作为请求目标，例如：

```http
POST /api/codex HTTP/1.1
Host: ubuntu-voice.example:12380
Content-Type: application/json; charset=utf-8
```

请求体是 UTF-8 JSON：

```json
{
  "status": "waiting_approval",
  "text": "等你授权执行：命令摘要",
  "thread": "任务或项目名称",
  "source": "会话日志",
  "agent": "codex"
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `status` | string | 规范状态值，见下表 |
| `text` | string | 补充说明；发送前最多截取 200 个 Unicode 码点，可能为空 |
| `thread` | string | 任务/会话显示名；可能为空 |
| `source` | string | 事件来源描述，例如 `会话日志`、`状态校准` 或 Claude 状态来源；用于展示和诊断，不应作为固定枚举 |
| `agent` | string | agent 标识，通常为 `codex` 或 `claude` |

`status` 可能取以下值：

| 值 | 含义 |
| --- | --- |
| `working` | 正在工作 |
| `waiting_approval` | 等待用户审批/授权 |
| `completed` | 任务完成或会话结束 |
| `interrupted` | 任务中断 |
| `idle` | 空闲状态校准 |
| `info` | 普通通知或测试消息 |

接收端建议将最新状态覆盖到对应设备显示上，并容忍重复请求：程序会在状态变化时推送，也会周期性发送状态校准，因此同一个状态可能再次到达。若设备启用多个 agent，建议至少按 `agent` 区分状态；若要显示多个任务，再按 `thread` 维护各任务状态。

接收端应尽快返回 HTTP `2xx`。响应体不作解析，返回 JSON 便于诊断，例如：

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{"ok":true}
```

任何 HTTP `4xx/5xx` 都视为失败。超时和失败会按设备的 `timeout_sec`、`retries` 设置处理；超时的目标会进入冷却，避免拖慢状态监控。

### 可选设备接口

程序的设备检测按钮还会请求以下路径。接收端不实现也能接收状态推送，但检测和蜂鸣功能不可用：

| 方法与路径 | 作用 | 建议响应 |
| --- | --- | --- |
| `GET /api/status` | 连通性检测 | `200`，可返回 `{"ok":true}` |
| `GET /api/beep?n=2&ms=120` | 让设备蜂鸣指定次数和时长 | `200`，执行后返回 `{"ok":true}` |

这两个路径相对于设备 URL 中 `/api/` 之前的主机和端口。例如配置 `http://host:12380/api/codex` 时，检测路径为 `http://host:12380/api/status`。

## 6. 本程序提供的本地 API

启动监控后，默认监听 `http://127.0.0.1:17865`。API 供其他本机程序提交状态或查询运行情况：

| 方法与路径 | 作用 |
| --- | --- |
| `GET /health` | 健康状态及设备最近推送结果 |
| `GET /status` | 当前 agent/线程状态 |
| `POST /status` | 提交状态并按通道配置推送 |
| `POST /notify` | 发送自定义通知 |
| `POST /test` | 发送测试消息 |
| `POST /config/reload` | 重新加载配置文件 |

`POST /status` 示例：

```json
{
  "status": "working",
  "text": "正在生成报告",
  "thread": "weekly-report",
  "source": "外部接口",
  "agent": "codex"
}
```

## 7. 语音、运行文件与迁移

- 默认朗读使用 Edge TTS 的晓晓语音。`.venv` 和网络用于生成新语音；命中 `audio_cache/` 时可直接播放已有缓存。系统也可配置为 Windows SAPI。
- 清空缓存后要手动重新生成晓晓语音：在 `win_agent-notify-C` 目录打开 PowerShell，运行 `.\.venv\Scripts\python.exe -m pip install edge-tts pypinyin`（只需安装一次），然后运行 `.\.venv\Scripts\python.exe tools\generate_voice_cache.py`。脚本读取 `config.json` 里的声音、语速、音量、缓存目录和自定义播报文案，并生成程序可直接识别的缓存文件名；需要访问网络连接 Edge TTS。只生成某些状态可加 `--status waiting_approval` 等参数，单独生成一段文字可用 `--text "Codex 需要审批"`。
- 如果 Edge TTS 返回 HTTP 403，先确认 Windows 日期和时间已同步，再更新库：`.\.venv\Scripts\python.exe -m pip install --upgrade edge-tts`。仍失败时通常是当前网络到语音服务的连接被拒绝，可切换网络，或通过 `--proxy http://127.0.0.1:7890` 指定可用代理。
- `config.json` 保存本机设置和凭据；迁移时复制后检查飞书密钥和设备 URL。
- `audio_cache/` 是可再生成的缓存；想在无 Python 的电脑上沿用已有 Edge TTS 音频，可以一起复制缓存目录。
- `state.json` 保存会话状态与读取位置；不复制时程序会重新建立状态基线。
- `codex-notify.log` 是运行日志，可删除后重新生成。
- `res/app.ico` 是构建时嵌入 exe 的图标资源，`res/logo.png` 是同一张图的 512px PNG（README、网页想用可以用它）。两个都是由 `tools/make_icon.py` 生成的：想改图标就改那个脚本，然后运行
  `.\.venv\Scripts\python.exe -m pip install Pillow`（只需一次）和 `.\.venv\Scripts\python.exe tools\make_icon.py`，再重新构建 exe。

## 8. 故障排查

- **飞书无消息**：确认 GUI 中飞书已勾选、Webhook 正确、机器人仍在群内；若配置签名校验，检查 secret。默认只推送审批和完成状态。
- **设备无消息**：检查设备勾选状态、完整 URL、端口和接收端是否返回 `2xx`。日志会记录设备名和失败原因。
- **Ubuntu 更换 IP**：点 Ubuntu 通道旁的「设置…」更新 URL，不需要重新编译。
- **等待审批误报**：Codex 没有公开的“正在申请审批”日志事件，程序依据未完成的工具调用推断；耗时很长的命令可能被误判。可调高 `watch.approval_after_sec` 或设为 `0` 关闭。
- **语音声音变化**：查看 `.venv` 中是否安装 Edge TTS，网络合成失败时会回退到系统语音。已有缓存可以继续播放。
