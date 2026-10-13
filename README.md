# AGENT_MONITORING · AI Agent 状态播报

盯着本机在跑的 **Codex** 和 **Claude Code**，一旦状态变化（开始工作 / 等你授权 / 任务完成 / 中断），
就用**灯、声音、飞书消息**告诉你 —— 不用一直盯屏幕，抬头或听一声就知道该不该过去。

```
┌─────────────────────────────┐
│  Codex 会话日志  ~/.codex/   │
│  Claude Code   ~/.claude/   │
└──────────────┬──────────────┘
               │ 只读监控（不装钩子、不改它们的配置）
               ▼
      ┌─────────────────────┐
      │  win_agent-notify-C │   Windows 主程序（GUI）
      └──────────┬──────────┘
                 │ 状态一变就并行推送
     ┌───────────┼───────────┬──────────────┐
     ▼           ▼           ▼              ▼
  本机提示音   飞书机器人   ESP32 状态灯   Ubuntu 语音播报
  + 语音朗读   （手机上看） （抬头看）     （听得见）
```

## 三个程序

| 目录 | 语言 | 跑在哪 | 作用 |
| --- | --- | --- | --- |
| [win_agent-notify-C](win_agent-notify-C/) | C++17 + Win32 | Windows 桌面 | **主程序**。读会话日志、判断状态、往所有通道推。带 GUI，双击就能用，无控制台窗口 |
| [ESP32S3_WebLED](ESP32S3_WebLED/) | Arduino | ESP32-S3 开发板 | 接收端。板载 RGB 灯变色，等待审批时呼吸红色并响三声 |
| [ubuntu_voice-notify](ubuntu_voice-notify/) | Python 3（纯标准库） | 任意 Linux | 接收端。用音箱播不同提示音，另带一个手机可开的网页朗读界面 |

三个程序**互相独立**，只通过同一套 HTTP 推送协议通信（`POST /api/codex`）。
可以只上 ESP32，也可以只上 Ubuntu，或者两个都要；主程序不在时接收端就当没收到。

## 状态和颜色

六个状态在三端是**同一套颜色**：空闲🟢 工作中🟡 等待审批🔴（呼吸） 已完成🔵 中断🟣 通知🩵。

同时有多个 agent 在跑时，按紧急程度决定"灯/大字显示谁"：

```
等待审批 > 已中断 > 工作中 > 已完成 > 通知 > 空闲
```

注意这只影响**显示**：Codex 和 Claude 的播报、提醒是各算各的，不会互相覆盖。
推送里带 `agent` 字段（`codex` / `claude`）来区分来源。

## 快速开始

**1. 先把主程序跑起来**（Windows，需要 MSYS2 UCRT64 工具链）

```bash
cd win_agent-notify-C
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe \
  -DCMAKE_RC_COMPILER=C:/msys64/ucrt64/bin/windres.exe \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target win_agent-notify-C --parallel
```

生成的 `win_agent-notify-C.exe` 就在该目录下，直接双击。
首次启动会自动生成 `config.json`，在界面里勾选要用的通道、填好飞书 Webhook 和设备地址，点「启动监控」。

**2. 再按需要部署接收端**（可选，可以都不要）

- ESP32：用 Arduino IDE 打开 `ESP32S3_WebLED/ESP32S3_WebLED.ino`，改 WiFi 和板子型号后烧录；
- Ubuntu：把 `ubuntu_voice-notify/` 拷过去，`python3 server.py` 即可。

两边的详细步骤、接线、排查都在各自的 README 里。

## 推送协议（三端唯一约定）

主程序向每个启用的设备 `POST` 一个 UTF-8 JSON：

```json
{ "status": "waiting_approval", "text": "等你授权执行：命令摘要",
  "thread": "任务名称", "source": "会话日志", "agent": "codex" }
```

接收端要**幂等**（同一状态可能重复收到），尽快返回 `2xx`。
改协议时三个程序都要跟着改。

## 隐私

- 程序**只读**读取 Codex / Claude Code 的本地会话文件，不装钩子、不改它们的配置，也不回写。
- **没有遥测**：除了你配置的飞书 Webhook 和各设备地址，不会往任何地方发数据。
- 但 `text` 字段会包含**命令原文**和 agent 的最后一句回复，这些会离开本机（推到飞书云端 / 局域网设备）。
  部署前请确认你接受这一点。
- `config.json`（含飞书密钥）和运行日志都被 `.gitignore` 忽略，**不要**提交到仓库。

## 许可

目前还没有 LICENSE 文件。打算公开的话建议补一个（MIT / Apache-2.0 等），
否则默认是"保留所有权利"，别人无法合法使用。
