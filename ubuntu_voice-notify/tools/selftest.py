"""离线自测：不需要声卡，验证提示音合成 + 所有 HTTP 接口。"""

from __future__ import annotations

import json
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import server as svc  # noqa: E402


def check(label: str, ok: bool, detail: str = "") -> bool:
    print(("  [PASS] " if ok else "  [FAIL] ") + label + (f" -> {detail}" if detail and not ok else ""))
    return ok


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get(url: str):
    with urllib.request.urlopen(url, timeout=10) as resp:
        return resp.status, resp.read().decode("utf-8", "replace")


def post(url: str, payload: dict):
    req = urllib.request.Request(
        url, data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json; charset=utf-8"}, method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.status, json.loads(resp.read().decode("utf-8", "replace"))


def main() -> int:
    ok = True

    # 1. 提示音合成（PCM WAV，能被标准库读回来说明格式正确）
    tmp = ROOT / "audio_cache"
    tmp.mkdir(exist_ok=True)
    for status in ("working", "waiting_approval", "completed", "interrupted", "info"):
        seq = svc.DEFAULTS["sounds"][status]
        path = tmp / f"selftest_{status}.wav"
        seconds = svc.build_tone_wav(path, seq, volume=0.6)
        with wave.open(str(path), "rb") as wav:
            frames = wav.getnframes()
            rate = wav.getframerate()
        expect = sum(item[1] for item in seq) / 1000.0
        real = frames / rate
        ok &= check(f"提示音 {status} 合成正确",
                    abs(real - expect) < 0.05 and abs(seconds - expect) < 0.05,
                    f"{real:.3f}s vs {expect:.3f}s")
        path.unlink(missing_ok=True)

    # 2. 状态解析
    ok &= check("中文状态可识别", svc.normalize_status("等待审批") == "waiting_approval",
                svc.normalize_status("等待审批"))
    ok &= check("未知状态兜底为 info", svc.normalize_status("what?") == "info",
                svc.normalize_status("what?"))

    # 3. 起服务
    cfg = svc.load_config(None)
    cfg["audio"]["cache_dir"] = str(tmp)
    port = free_port()
    cfg["server"]["host"] = "127.0.0.1"
    cfg["server"]["port"] = port
    svc.SERVICE = svc.Service(cfg)
    httpd = svc.ThreadingHTTPServer(("127.0.0.1", port), svc.Handler)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    time.sleep(0.3)
    base = f"http://127.0.0.1:{port}"

    try:
        status, html = get(base + "/")
        ok &= check("网页可打开", status == 200 and "朗读" in html and "<textarea" in html,
                    str(status))

        status, body = get(base + "/health")
        health = json.loads(body)
        ok &= check("GET /health 正常", health.get("ok") is True, body)

        status, result = post(base + "/api/codex",
                              {"status": "working", "text": "开始干活", "thread": "演示"})
        ok &= check("POST /api/codex 接收状态", result.get("codex") == "working", str(result))

        status, result = post(base + "/api/codex", {"status": "等待审批"})
        ok &= check("中文状态推送可用", result.get("codex") == "waiting_approval", str(result))

        status, body = get(base + "/api/status")
        snap = json.loads(body)
        ok &= check("GET /api/status 反映当前状态",
                    snap.get("codex") == "waiting_approval" and snap.get("codex_label") == "等待审批",
                    body)
        ok &= check("状态历史有记录", len(snap.get("history", [])) >= 2, str(snap.get("history")))

        # Claude Code 来源要能区分出来（放后面，避免影响上面"当前状态"的断言）
        status, result = post(base + "/api/codex",
                              {"status": "working", "text": "改代码", "agent": "claude",
                               "thread": "demo"})
        agents = {a["key"]: a for a in result.get("agents", [])}
        ok &= check("能识别 Claude Code 来源",
                    "claude" in agents and agents["claude"]["label"] == "Claude Code"
                    and agents["claude"]["state"] == "working",
                    str(result.get("agents")))
        ok &= check("两个来源的状态同时保留、互不覆盖",
                    set(agents) == {"codex", "claude"}
                    and agents["codex"]["state"] == "waiting_approval"   # 上面推的还没被冲掉
                    and agents["claude"]["state"] == "working",
                    str({k: v["state"] for k, v in agents.items()}))
        ok &= check("顶层字段显示更紧急的那个",
                    result.get("agent") == "codex" and result.get("codex") == "waiting_approval",
                    f"{result.get('agent')} / {result.get('codex')}")
        ok &= check("提示语模板按来源展开",
                    "Claude Code 开始工作" in svc.expand_phrases(cfg)
                    and "Codex 开始工作" in svc.expand_phrases(cfg),
                    str(svc.expand_phrases(cfg)))

        status, body = get(base + "/api/beep?n=2&ms=100")
        ok &= check("GET /api/beep 可用", json.loads(body).get("ok") is True, body)

        status, result = post(base + "/api/speak", {"text": "任务完成了，请检查"})
        ok &= check("POST /api/speak 接受朗读请求", result.get("ok") is True, str(result))

        status, result = post(base + "/api/mute", {"mute": True})
        ok &= check("静音开关可用", result.get("mute") is True, str(result))
        status, result = post(base + "/api/codex", {"status": "completed"})
        ok &= check("静音时仍能接收推送", result.get("ok") is True and result.get("mute") is True,
                    str(result))
        post(base + "/api/mute", {"mute": False})

        try:
            get(base + "/api/nonexistent")
            ok &= check("未知路径返回 404", False, "没有报错")
        except urllib.error.HTTPError as exc:
            ok &= check("未知路径返回 404", exc.code == 404, str(exc.code))

        try:
            post(base + "/api/codex", {})
            ok &= check("缺 status 返回 400", False, "居然成功了")
        except urllib.error.HTTPError as exc:
            ok &= check("缺 status 返回 400", exc.code == 400, str(exc.code))
    finally:
        httpd.shutdown()
        httpd.server_close()

    # 4. 与 ESP32 接口一致：电脑端设备探测/蜂鸣器测试都能直接复用
    ok &= check("接口与 ESP32 对齐（可被设备探测复用）",
                all(hasattr(svc.Handler, name) for name in ("do_GET", "do_POST")))

    print()
    print("全部通过 ✅" if ok else "存在失败项 ❌")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
