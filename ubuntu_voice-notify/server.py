#!/usr/bin/env python3
"""Codex 状态语音播报服务（Ubuntu 版，对应 ESP32 那块板子的角色）。

两个功能：
  1. 接收状态推送（和 ESP32 同样的接口），按不同状态用扬声器发出不同的提示音；
  2. 提供一个网页，可以在手机/电脑上输入文字，服务器收到后用女声朗读出来。

接口（与 ESP32 完全一致，所以电脑端 codex-feishu-notify 不用改任何东西）：
    POST /api/codex    {"status":"working","text":"...","thread":"...","source":"..."}
    GET  /api/codex?status=working
    GET  /api/beep?n=2&ms=120
    GET  /api/status
    GET  /health
  额外：
    POST /api/speak    {"text":"要朗读的内容","voice":"可选"}
    POST /api/mute     {"mute":true}   /  GET /api/mute?m=1
    GET  /             网页（输文字朗读 + 状态显示 + 测试各状态提示音）

只依赖 Python 标准库；发声调用系统里的播放器（pw-play / paplay / aplay / ffplay），
朗读调用系统里的 TTS（edge-tts / piper / espeak-ng），装哪个用哪个。
"""

from __future__ import annotations

import argparse
import array
import asyncio
import hashlib
import json
import math
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import wave
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

BASE = Path(__file__).resolve().parent
DEFAULT_CONFIG_PATH = BASE / "config.json"

# ---------------------------------------------------------------------------
#  默认配置
# ---------------------------------------------------------------------------
DEFAULTS: dict = {
    "server": {
        "host": "0.0.0.0",       # 0.0.0.0 = 局域网内其它设备也能访问
        "port": 8080,
        "token": "",             # 非空时请求需带 Authorization: Bearer <token>
    },
    "audio": {
        "player": "auto",        # auto / pw-play / paplay / aplay / ffplay
        "volume": 0.6,           # 提示音音量 0~1（朗读音量由 TTS 决定）
        "mute": False,           # true = 全局静音
        "device": "",            # 一般留空；多声卡时填 ALSA 设备，例如 "hw:1,0"
        "cache_dir": "audio_cache",
    },
    # 提示音：一串 [频率Hz, 时长ms]，频率 0 表示静音间隔
    "sounds": {
        "idle": [],
        "working": [[880, 90], [0, 70]],
        "waiting_approval": [[2700, 90], [0, 80], [2700, 90], [0, 80], [2700, 120], [0, 100]],
        "completed": [[1500, 120], [0, 60], [2700, 200], [0, 80]],
        "interrupted": [[900, 150], [0, 90], [700, 260], [0, 80]],
        "info": [[2000, 100], [0, 80]],
    },
    "remind": {
        "enabled": True,
        "statuses": ["waiting_approval"],   # 这些状态会周期性重复提醒
        "interval_sec": 30,
        "max_times": 5,                     # 0 = 不限次数
    },
    "tts": {
        "backend": "auto",       # auto / edge / piper / espeak
        "voice": "zh-CN-XiaoxiaoNeural",   # edge-tts 的中文女声（晓晓）
        "rate": "+0%",
        "volume": "+0%",
        "pitch": "+0Hz",         # 音调，微调能让声音更柔和，例如 "-20Hz"
        "piper_model": "",       # 例如 /opt/piper/zh_CN-huayan-medium.onnx
        "espeak_voice": "cmn+f3",          # 中文女声
        "espeak_speed": 160,     # espeak 语速（字数/分钟），慢一点更像人
        "espeak_pitch": 50,      # espeak 音调 0~99
        "cache": True,           # 同一句话第二次说直接播缓存，零等待
        "warmup": [],            # 启动时预合成的常用短语，例如 ["吃饭了", "任务已完成，请检查"]
        "max_chars": 300,
    },
    "announce": {
        # 动态播报：开启后，这些状态推送里带 text 时，会把推送的文字也念出来
        "enabled": False,
        "statuses": ["waiting_approval"],
        # 固定提示语：先用提示音(滴滴)，紧接着把这句话念出来。
        # {agent} 会替换成来源名（Codex / Claude Code），这样一听就知道是谁。
        # 每句话只会合成一次，之后直接调用缓存(见 tts.cache)，不会每次都联网合成。
        "phrases": {
            "working": "{agent} 开始工作",
            "waiting_approval": "{agent} 需要审批",
            "completed": "{agent} 任务已完成",
            "interrupted": "{agent} 任务中断",
        },
    },
}

STATUS_LABEL = {
    "idle": "空闲",
    "working": "工作中",
    "waiting_approval": "等待审批",
    "completed": "已完成",
    "interrupted": "已中断",
    "info": "通知",
}

# 哪个智能体在推状态（codex / claude），播报时会把名字念出来
AGENT_LABEL = {
    "codex": "Codex",
    "claude": "Claude Code",
    "claude-code": "Claude Code",
    "claudecode": "Claude Code",
    "cc": "Claude Code",
    "manual": "手动",
}
DEFAULT_AGENT = "codex"


def agent_label(agent) -> str:
    key = str(agent or DEFAULT_AGENT).strip().lower()
    return AGENT_LABEL.get(key, str(agent).strip() or AGENT_LABEL[DEFAULT_AGENT])


def normalize_agent(agent) -> str:
    """把来源归一成两个槽位之一：codex / claude。"""
    key = str(agent or "").strip().lower()
    if "claude" in key or key == "cc":
        return "claude"
    return "codex"


# 两个来源的槽位（顺序即网页显示顺序）
AGENT_SLOTS = (("codex", "Codex"), ("claude", "Claude Code"))

# 状态 -> 颜色，和 ESP32 灯 / 飞书卡片同一套
STATE_COLOR = {
    "idle": "#00C800",
    "working": "#FF9600",
    "waiting_approval": "#FF0000",
    "completed": "#005AFF",
    "interrupted": "#FF00A0",
    "info": "#00B4FF",
}

# 灯/网页上"更紧急"的排前面
STATUS_PRIORITY = {
    "waiting_approval": 5,
    "interrupted": 4,
    "working": 3,
    "completed": 2,
    "info": 1,
    "idle": 0,
}


def expand_phrases(cfg: dict) -> list[str]:
    """把 announce.phrases 里带 {agent} 的模板展开成每种智能体的实际句子（用于预热）。"""
    out: list[str] = []
    for value in ((cfg.get("announce") or {}).get("phrases") or {}).values():
        text = str(value).strip()
        if not text:
            continue
        if "{agent}" in text:
            for label in ("Codex", "Claude Code"):
                expanded = text.replace("{agent}", label)
                if expanded not in out:
                    out.append(expanded)
        elif text not in out:
            out.append(text)
    return out

STATUS_ALIASES = {
    "working": "working", "work": "working", "busy": "working", "running": "working",
    "工作中": "working", "工作": "working", "运行中": "working", "进行中": "working",
    "waiting_approval": "waiting_approval", "waiting": "waiting_approval",
    "approval": "waiting_approval", "permission": "waiting_approval",
    "等待审批": "waiting_approval", "待审批": "waiting_approval", "等待确认": "waiting_approval",
    "completed": "completed", "complete": "completed", "done": "completed", "finished": "completed",
    "已完成": "completed", "完成": "completed",
    "interrupted": "interrupted", "aborted": "interrupted", "failed": "interrupted",
    "已中断": "interrupted", "中断": "interrupted", "失败": "interrupted",
    "idle": "idle", "空闲": "idle", "不工作": "idle",
    "info": "info", "notify": "info", "通知": "info",
}


def normalize_status(value) -> str:
    key = str(value or "info").strip().lower()
    return STATUS_ALIASES.get(key, key if key in STATUS_LABEL else "info")


def deep_merge(base: dict, override: dict) -> dict:
    out = json.loads(json.dumps(base))
    for key, value in (override or {}).items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = deep_merge(out[key], value)
        else:
            out[key] = value
    return out


def load_config(path=None) -> dict:
    target = Path(path) if path else DEFAULT_CONFIG_PATH
    user = {}
    if target.exists():
        with open(target, "r", encoding="utf-8") as fh:
            user = json.load(fh)
    cfg = deep_merge(DEFAULTS, user)
    cfg["_path"] = str(target)
    return cfg


def safe_stdout() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):
            pass


def log(msg: str) -> None:
    print(f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}  {msg}", flush=True)


def clean(value) -> str:
    """清掉无法编码的字符（例如 Windows 上被截断的 emoji 造成的孤立代理项），
    否则 json.dumps(...).encode('utf-8') 会抛异常，整条推送就没了。"""
    return str(value or "").encode("utf-8", "replace").decode("utf-8", "replace")


# ---------------------------------------------------------------------------
#  提示音：自己合成 WAV，不依赖 sox / beep 之类的工具
# ---------------------------------------------------------------------------
def build_tone_wav(path: Path, sequence, volume: float = 0.6, sample_rate: int = 44100) -> float:
    """把 [[频率, 时长ms], ...] 合成一个 WAV 文件，返回总时长(秒)。"""
    frames = array.array("h")
    fade = max(1, int(sample_rate * 0.005))          # 5ms 淡入淡出，避免爆音
    total = 0.0
    for item in sequence or []:
        freq, ms = float(item[0]), float(item[1])
        count = max(0, int(sample_rate * ms / 1000.0))
        total += ms / 1000.0
        if freq <= 0:
            frames.extend([0] * count)
            continue
        amplitude = int(32767 * max(0.0, min(1.0, volume)))
        for i in range(count):
            gate = 1.0
            if i < fade:
                gate = i / fade
            elif i > count - fade:
                gate = max(0.0, (count - i) / fade)
            frames.append(int(amplitude * gate * math.sin(2 * math.pi * freq * i / sample_rate)))
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(frames.tobytes())
    return total


# ---------------------------------------------------------------------------
#  播放器 / TTS 探测
# ---------------------------------------------------------------------------
#  (命令, 是否支持 mp3, 附加参数)
PLAYERS = [
    ("pw-play", True, []),
    ("paplay", True, []),
    ("ffplay", True, ["-nodisp", "-autoexit", "-loglevel", "quiet"]),
    ("mpg123", True, ["-q"]),                 # 专放 mp3，没有 ffmpeg 时的救命稻草
    ("aplay", False, ["-q"]),
]

TTS_BACKENDS = ("edge", "piper", "espeak")


def find_player(prefer: str = "auto"):
    if prefer and prefer != "auto":
        found = shutil.which(prefer)
        if found:
            for name, mp3_ok, extra in PLAYERS:
                if name == prefer:
                    return found, mp3_ok, extra
            return found, True, []
        log(f"[警告] 配置的播放器 {prefer} 不存在，改用自动探测")
    for name, mp3_ok, extra in PLAYERS:
        found = shutil.which(name)
        if found:
            return found, mp3_ok, extra
    return None, False, []


def find_tts(prefer: str = "auto"):
    order = list(TTS_BACKENDS)
    if prefer and prefer != "auto":
        order = [prefer] + [b for b in order if b != prefer]
    for backend in order:
        if backend == "edge" and shutil.which("edge-tts"):
            return "edge"
        if backend == "piper" and shutil.which("piper"):
            return "piper"
        if backend == "espeak" and (shutil.which("espeak-ng") or shutil.which("espeak")):
            return "espeak"
    return None


def available_players() -> list:
    """按优先级列出本机所有可用的播放器：[(路径, 支持mp3, 附加参数), ...]"""
    found = []
    for name, mp3_ok, extra in PLAYERS:
        path = shutil.which(name)
        if path:
            found.append((path, mp3_ok, extra))
    return found


def player_supports(ext: str, mp3_ok: bool) -> bool:
    if ext == ".wav":
        return True
    if ext == ".mp3":
        return bool(mp3_ok or shutil.which("ffplay") or shutil.which("mpg123"))
    return True


# ---------------------------------------------------------------------------
#  音频播放线程：所有发声都走这里，保证不重叠、不阻塞 HTTP
# ---------------------------------------------------------------------------
class AudioPlayer:
    def __init__(self, cfg: dict) -> None:
        self.cfg = cfg
        self.queue: queue.Queue = queue.Queue(maxsize=32)
        self.preferred: str | None = None      # 上次播放成功的播放器
        self.unusable: set = set()             # 试过失败的播放器，排到最后
        self._load_preferred()                 # 记住上次成功的播放器，重启也生效
        threading.Thread(target=self._loop, name="audio", daemon=True).start()

    def _preferred_file(self) -> Path:
        cache = Path(self.cfg["audio"].get("cache_dir") or "audio_cache")
        if not cache.is_absolute():
            cache = BASE / cache
        cache.mkdir(parents=True, exist_ok=True)
        return cache / "preferred_player.txt"

    def _load_preferred(self) -> None:
        try:
            saved = self._preferred_file().read_text(encoding="utf-8").strip()
            if saved and shutil.which(saved):
                self.preferred = shutil.which(saved)
        except OSError:
            pass

    def _remember_preferred(self, exe: str) -> None:
        self.preferred = exe
        try:
            self._preferred_file().write_text(Path(exe).name, encoding="utf-8")
        except OSError:
            pass

    # ---- 对外接口 ----

    def play_sequence(self, sequence, tag: str = "") -> None:
        self._put({"kind": "tone", "sequence": sequence, "tag": tag})

    def speak(self, text: str, voice=None, tag: str = "speak") -> None:
        self._put({"kind": "speak", "text": text, "voice": voice, "tag": tag})

    def _put(self, job: dict) -> None:
        try:
            self.queue.put_nowait(job)
        except queue.Full:
            log("[警告] 播放队列已满，丢弃一条任务")

    def _loop(self) -> None:
        while True:
            job = self.queue.get()
            try:
                if job["kind"] == "tone":
                    self._play_tone(job["sequence"], job.get("tag", ""))
                else:
                    self._play_speech(job["text"], job.get("voice"))
            except Exception as exc:  # noqa: BLE001 - 播放出错不能让服务挂掉
                log(f"[错误] 播放失败: {exc}")
            finally:
                self.queue.task_done()

    def _cache_path(self, name: str) -> Path:
        cache = Path(self.cfg["audio"].get("cache_dir") or "audio_cache")
        if not cache.is_absolute():
            cache = BASE / cache
        cache.mkdir(parents=True, exist_ok=True)
        return cache / name

    def _play_tone(self, sequence, tag: str) -> None:
        if not sequence:
            return
        volume = float(self.cfg["audio"].get("volume") or 0.6)
        key = json.dumps(sequence) + f"|{volume}"
        path = self._cache_path(f"tone_{abs(hash(key)) % 10**12}.wav")
        if not path.exists():
            build_tone_wav(path, sequence, volume=volume)
        self._play_file(path, tag or "tone")

    def _play_speech(self, text: str, voice) -> None:
        text = " ".join(clean(text).split())
        if not text:
            return
        limit = int(self.cfg["tts"].get("max_chars") or 300)
        if len(text) > limit:
            text = text[: limit - 1] + "…"

        backend = find_tts(self.cfg["tts"].get("backend", "auto"))
        if not backend:
            log("[警告] 没找到 TTS（edge-tts / piper / espeak-ng），改播两声提示音")
            self._play_tone([[1200, 120], [0, 80], [1200, 120]], "tts-missing")
            return

        # ---- 缓存：同一句话第二次说就直接播，零等待 ----
        cache_on = bool(self.cfg["tts"].get("cache", True))
        cache_key = self._tts_cache_key(text, voice, backend)
        suffix = ".mp3" if backend == "edge" else ".wav"
        cached = self._cache_path(f"cache_{cache_key}{suffix}")
        if cache_on and cached.exists():
            log(f"[缓存] 命中，直接播放：{text[:30]}{'…' if len(text) > 30 else ''}")
            self._play_file(cached, f"tts:{backend}(缓存)", mp3_prefer=True)
            return

        stamp = datetime.now().strftime("%H%M%S%f")
        out = cached if cache_on else self._cache_path(f"tts_{stamp}{suffix}")
        started = time.time()
        try:
            cmd = self._build_cmd(text, voice, backend, out)
        except ValueError as exc:
            log(f"[警告] {exc}，改用 espeak-ng")
            self.cfg["tts"]["backend"] = "espeak"
            return self._play_speech(text, voice)

        log(f"[TTS] {backend} 开始合成: {text[:40]}{'…' if len(text) > 40 else ''}")
        done, error = self._synthesize(text, voice, backend, out, cmd)
        if not done:
            log(f"[错误] {backend} 合成失败: {error}")
            self._play_tone([[1200, 120], [0, 80], [1200, 120]], "tts-failed")
            return
        log(f"[TTS] 合成完成，用时 {time.time() - started:.1f}s")

        self._play_file(out, f"tts:{backend}", mp3_prefer=True)

    def _synthesize(self, text: str, voice, backend: str, out: Path, cmd: list) -> tuple[bool, str]:
        """把 text 合成到 out。返回 (是否成功, 错误信息)。"""
        try:
            if backend == "edge":
                if self._synth_edge(text, voice, out):   # Python 库合成，少一次进程启动
                    return True, ""
                subprocess.run(cmd, timeout=120, check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            elif backend == "piper":                     # piper 从标准输入读文本
                subprocess.run(cmd, input=text.encode("utf-8"), timeout=120, check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            else:
                subprocess.run(cmd, timeout=120, check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        except subprocess.CalledProcessError as exc:
            return False, (exc.stderr or b"").decode("utf-8", "replace")[:200]
        except (subprocess.TimeoutExpired, OSError) as exc:
            return False, f"{type(exc).__name__}: {exc}"
        if not out.exists() or out.stat().st_size == 0:
            return False, "合成结果为空"
        return True, ""

    def warmup(self, text: str, voice=None) -> bool:
        """提前把一句话合成好放进缓存（不播放），这样点它的时候是秒出声。"""
        if not bool(self.cfg["tts"].get("cache", True)):
            return False
        backend = find_tts(self.cfg["tts"].get("backend", "auto"))
        if not backend:
            return False
        text = " ".join(str(text or "").split())
        if not text:
            return False
        suffix = ".mp3" if backend == "edge" else ".wav"
        cached = self._cache_path(f"cache_{self._tts_cache_key(text, voice, backend)}{suffix}")
        if cached.exists():
            return True
        cmd = self._build_cmd(text, voice, backend, cached)
        started = time.time()
        done, error = self._synthesize(text, voice, backend, cached, cmd)
        if done:
            log(f"[预热] 已缓存：{text[:24]}{'…' if len(text) > 24 else ''}"
                f"（用时 {time.time() - started:.1f}s）")
        else:
            log(f"[预热] 失败：{text[:24]} -> {error}")
        return done

    def _tts_cache_key(self, text: str, voice, backend: str) -> str:
        tts = self.cfg["tts"]
        blob = "|".join([
            backend, text, str(voice or tts.get("voice") or ""),
            str(tts.get("rate") or ""), str(tts.get("volume") or ""), str(tts.get("pitch") or ""),
            str(tts.get("espeak_voice") or ""), str(tts.get("espeak_speed") or ""),
            str(tts.get("espeak_pitch") or ""), str(tts.get("piper_model") or ""),
        ])
        return hashlib.sha1(blob.encode("utf-8")).hexdigest()[:16]

    def _build_cmd(self, text: str, voice, backend: str, out: Path) -> list:
        """拼出各后端合成用的命令行。piper 没配模型时抛 ValueError。"""
        tts = self.cfg["tts"]
        if backend == "edge":
            cmd = ["edge-tts", "--voice", voice or tts.get("voice") or "zh-CN-XiaoxiaoNeural"]
            # 语速/音量/音调只在显式配置时才传，避免旧版 edge-tts 不认参数
            for flag, key in (("--rate", "rate"), ("--volume", "volume"), ("--pitch", "pitch")):
                value = str(tts.get(key) or "").strip()
                if value and value not in ("+0%", "+0Hz", "0"):
                    cmd += [flag, value]
            return cmd + ["--text", text, "--write-media", str(out)]
        if backend == "piper":
            model = tts.get("piper_model") or ""
            if not model or not Path(model).exists():
                raise ValueError("piper 没配模型（tts.piper_model）")
            return ["piper", "--model", model, "--output_file", str(out)]
        exe = shutil.which("espeak-ng") or "espeak"
        return [exe, "-v", tts.get("espeak_voice") or "cmn+f3",
                "-s", str(int(tts.get("espeak_speed") or 160)),
                "-p", str(int(tts.get("espeak_pitch") or 50)),
                "-w", str(out), text]

    def _synth_edge(self, text: str, voice, out: Path) -> bool:
        """优先用 edge_tts 这个 Python 库合成（比命令行少一次进程启动）。成功返回 True。"""
        try:
            import edge_tts  # type: ignore
        except ImportError:
            return False

        params = {}
        for key in ("rate", "volume", "pitch"):
            value = str(self.cfg["tts"].get(key) or "").strip()
            if value and value not in ("+0%", "+0Hz", "0"):
                params[key] = value
        speak_voice = voice or self.cfg["tts"].get("voice") or "zh-CN-XiaoxiaoNeural"

        async def run() -> None:
            try:
                communicate = edge_tts.Communicate(text, speak_voice, **params)
            except TypeError:            # 旧版 edge_tts 不认 pitch
                params.pop("pitch", None)
                communicate = edge_tts.Communicate(text, speak_voice, **params)
            await communicate.save(str(out))

        try:
            asyncio.run(run())
        except Exception as exc:  # noqa: BLE001 - 失败就回到命令行方式
            log(f"[警告] edge-tts 库合成失败（{exc}），改用命令行")
            return False
        return out.exists() and out.stat().st_size > 0

    def _play_file(self, path: Path, tag: str, player=None, extra=None,
                   mp3_prefer: bool = False) -> None:
        if self.cfg["audio"].get("mute", False):
            log(f"[静音] 跳过 {tag}")
            return
        if not path.exists():
            return
        source_path = path

        # 候选播放器：先用指定的，失败就按优先级换下一个再试
        candidates: list = []
        if player:
            candidates.append((player, list(extra or [])))
        for exe, _mp3, args in available_players():
            if exe != player:
                candidates.append((exe, list(args)))
        # 上次成功的排最前；已知不可用的排最后（这样不会每次都先撞一遍墙）
        if self.preferred:
            candidates.sort(key=lambda item: 0 if item[0] == self.preferred else 1)
        candidates.sort(key=lambda item: 1 if item[0] in self.unusable else 0)
        if not candidates:
            log("[警告] 没找到播放器（pw-play / paplay / aplay / ffplay）")
            log("        装一个：sudo apt install -y pulseaudio-utils   或   alsa-utils")
            return

        # mp3 处理：优先交给能原生解码 mp3 的播放器（零等待）；没有才用 ffmpeg 转 wav。
        # 为什么不能直接用 pw-play/paplay：很多发行版的 libsndfile 不带 mp3 解码，
        # 它们会拒绝，而 aplay 会把 mp3 当裸 PCM 放成噪音（下面有硬性过滤）。
        if path.suffix.lower() == ".mp3":
            native = [exe for exe, _m, _a in available_players()
                      if Path(exe).stem.lower() in ("mpg123", "ffplay", "mpv")]
            if native and mp3_prefer:
                order = {exe: index for index, exe in enumerate(native)}
                candidates.sort(key=lambda item: order.get(item[0], 99))
                log(f"[提示] 用 {Path(native[0]).name} 直接播 mp3（省掉转码）")
            elif shutil.which("ffmpeg"):
                wav_path = path.with_suffix(".wav")
                try:
                    convert = subprocess.run(
                        ["ffmpeg", "-y", "-loglevel", "error", "-i", str(path), str(wav_path)],
                        timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                    )
                    if convert.returncode == 0 and wav_path.exists():
                        path = wav_path
                        log("[转换] mp3 -> wav（本机没有原生 mp3 播放器，用 ffmpeg 转）")
                    else:
                        error = (convert.stderr or b"").decode("utf-8", "replace").strip()[:150]
                        log(f"[警告] ffmpeg 转换失败: {error}")
                except (subprocess.TimeoutExpired, OSError) as exc:
                    log(f"[警告] ffmpeg 转换异常: {exc}")
            else:
                log("[警告] 既没有 mp3 播放器（mpg123/ffplay）也没装 ffmpeg，只能直接交给播放器")

        # 硬性规则：aplay 只能放 wav，绝不拿它去放 mp3（会变成噪音）
        if path.suffix.lower() != ".wav":
            usable = [item for item in candidates if "aplay" not in Path(item[0]).name]
            if len(usable) != len(candidates):
                log("[提示] 跳过 aplay（它只能播放 wav，放 mp3 会出噪音）")
            candidates = usable
            if not candidates:
                log("[错误] 这段音频是 mp3，但本机没有能解码 mp3 的播放器。")
                log("        装一个就好：sudo apt install -y mpg123   或   sudo apt install -y ffmpeg")
                return

        device = str(self.cfg["audio"].get("device") or "").strip()
        last_error = ""
        for index, (exe, extra_args) in enumerate(candidates):
            cmd = [exe] + list(extra_args)
            if device:
                if "pw-play" in exe or "paplay" in exe:
                    cmd += ["--device", device]
                elif "aplay" in exe:
                    cmd += ["-D", device]
            cmd.append(str(path))
            try:
                result = subprocess.run(cmd, timeout=120,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            except (subprocess.TimeoutExpired, OSError) as exc:
                last_error = f"{type(exc).__name__}: {exc}"
                log(f"[错误] {Path(exe).name} 执行异常: {last_error}")
                continue
            if result.returncode == 0:
                if self.preferred != exe:
                    self._remember_preferred(exe)
                    log(f"[播放器] 后续优先用 {Path(exe).name}（本次成功，已记住）")
                log(f"[播放] {tag} -> {path.name}（{Path(exe).name}）")
                last_error = ""
                break
            last_error = (result.stderr or b"").decode("utf-8", "replace").strip()[:300]
            log(f"[错误] {Path(exe).name} 返回 {result.returncode}: {last_error}")
            self.unusable.add(exe)
            if index == 0 and len(candidates) > 1:
                log("[提示] 换下一个播放器再试…")

        if last_error:
            log("[错误] 声音没能放出来，排查建议：")
            log("        python3 server.py --diagnose      # 一键体检音频环境")
            log("        pactl list short sinks            # 有哪些输出设备（PipeWire 用 wpctl status）")
            log("        speaker-test -c 2 -t sine -f 440  # 绕过本服务直接测声")
            log("        若出现 'Format not recognised'：播放器不支持 mp3，")
            log("        装个解码器即可：sudo apt install -y ffmpeg   （或 mpg123）")
            log("        注意：SSH / 容器里默认没有声音会话，见 README 第 9 节")

        for temp in {source_path, path}:      # 朗读产生的临时文件，播完就删
            if temp.name.startswith("tts_"):
                try:
                    temp.unlink()
                except OSError:
                    pass


# ---------------------------------------------------------------------------
#  服务状态
# ---------------------------------------------------------------------------
class Service:
    def __init__(self, cfg: dict) -> None:
        self.cfg = cfg
        self.player = AudioPlayer(cfg)
        self.lock = threading.RLock()
        self.started = time.time()
        # 每个来源一个独立的状态槽：各存各的，互不覆盖，也各自提醒
        self.slots = {
            key: {
                "key": key, "label": label, "status": "idle", "text": "", "thread": "",
                "source": "", "last_push": 0.0, "reminders": 0, "last_remind": 0.0,
            }
            for key, label in AGENT_SLOTS
        }
        self.history: list = []
        threading.Thread(target=self._remind_loop, name="reminder", daemon=True).start()
        warmup = [str(item).strip() for item in (self.cfg["tts"].get("warmup") or [])
                  if str(item).strip()]
        # 固定提示语也要预热（含 {agent} 展开后的两种说法），
        # 这样第一次遇到"等待审批"就是秒出声，而不是先等几秒合成
        for text in expand_phrases(self.cfg):
            if text not in warmup:
                warmup.append(text)
        if warmup:
            threading.Thread(target=self._warmup, args=(warmup,), name="tts-warmup",
                             daemon=True).start()

    def _warmup(self, texts: list) -> None:
        log(f"[预热] 后台预合成 {len(texts)} 条常用短语（先说过的以后就是秒出声）")
        for text in texts:
            self.player.warmup(text)
        log("[预热] 完成")

    # ---- 状态变化 ----

    def set_status(self, status, text: str = "", thread_name: str = "", source: str = "") -> str:
        return self.set_status_with_agent(status, text, thread_name, source, "codex")

    def set_status_with_agent(self, status, text: str = "", thread_name: str = "",
                              source: str = "", agent: str = "codex") -> str:
        status = normalize_status(status)
        key = normalize_agent(agent)
        with self.lock:
            slot = self.slots[key]
            changed = status != slot["status"]
            slot["status"] = status
            slot["text"] = clean(text).strip()
            slot["thread"] = clean(thread_name).strip()
            slot["source"] = clean(source).strip()
            slot["last_push"] = time.time()
            if changed:
                slot["reminders"] = 0
                slot["last_remind"] = time.time()
            self.history.append({
                "at": datetime.now().strftime("%H:%M:%S"),
                "status": status,
                "label": STATUS_LABEL.get(status, status),
                "agent": slot["label"],
                "agent_key": key,
                "text": slot["text"][:120],
                "thread": slot["thread"][:60],
            })
            del self.history[:-50]
        if changed:
            # 谁变了就播报谁的（两个来源互不影响，可以先后各响一次）
            self.play_status_sound(status, key)
            announce = self.cfg.get("announce") or {}
            if announce.get("enabled") and status in (announce.get("statuses") or []) and slot["text"]:
                self.player.speak(f"{slot['label']}：{slot['text']}", tag=f"announce:{status}")
        return status

    def play_status_sound(self, status, agent: str = "codex") -> None:
        normalized = normalize_status(status)
        sequence = (self.cfg.get("sounds") or {}).get(normalized) or []
        if sequence:
            self.player.play_sequence(sequence, tag=normalized)
        else:
            log(f"[提示音] {normalized} 未配置声音，跳过")

        # 固定提示语：先"滴滴"，紧接着念一句（队列是先进先出，顺序天然正确）
        # 同一句话只会合成一次，之后直接读缓存，不会重复联网合成。
        phrase = ((self.cfg.get("announce") or {}).get("phrases") or {}).get(normalized)
        if phrase and str(phrase).strip():
            text = str(phrase).replace("{agent}", agent_label(agent)).strip()
            log(f"[播报] {normalized}（{agent_label(agent)}）-> {text}")
            self.player.speak(text, tag=f"phrase:{normalized}")

    def _remind_loop(self) -> None:
        while True:
            time.sleep(1.0)
            try:
                remind = self.cfg.get("remind") or {}
                if not remind.get("enabled", True):
                    continue
                interval = float(remind.get("interval_sec") or 0)
                if interval <= 0:
                    continue
                limit = int(remind.get("max_times") or 0)
                statuses = remind.get("statuses") or []
                now = time.time()
                # 每个来源分别判断、分别提醒：Codex 在等审批、Claude 在干活时互不干扰
                for key, _label in AGENT_SLOTS:
                    with self.lock:
                        slot = self.slots[key]
                        status = slot["status"]
                        count = slot["reminders"]
                        last = slot["last_remind"]
                    if status not in statuses:
                        continue
                    if limit and count >= limit:
                        continue
                    if now - last < interval:
                        continue
                    with self.lock:
                        slot["reminders"] = count + 1
                        slot["last_remind"] = now
                    log(f"[提醒] {slot['label']} 仍是 {status}，第 {count + 1} 次提醒")
                    self.play_status_sound(status, key)
            except Exception as exc:  # noqa: BLE001 - 提醒线程不能死
                log(f"[错误] 提醒线程: {exc}")

    # ---- 查询 ----

    def player_name(self):
        player, _, _ = find_player(self.cfg["audio"].get("player", "auto"))
        return player

    def snapshot(self) -> dict:
        now = time.time()
        with self.lock:
            agents = []
            for key, label in AGENT_SLOTS:
                slot = self.slots[key]
                agents.append({
                    "key": key,
                    "label": label,
                    "state": slot["status"],
                    "state_label": STATUS_LABEL.get(slot["status"], slot["status"]),
                    "color": STATE_COLOR.get(slot["status"], "#7D8DA4"),
                    "text": slot["text"],
                    "thread": slot["thread"],
                    "source": slot["source"],
                    "last_push_age": int(now - slot["last_push"]) if slot["last_push"] else None,
                })
            # 顶层字段保留"灯上显示的那个"（优先级最高的来源），方便旧调用方和探测命令
            winner = self.winning_key()
            top = self.slots[winner]
            age = int(now - top["last_push"]) if top["last_push"] else None
            player = self.player_name()
            return {
                "ok": True,
                "codex": top["status"],
                "codex_label": STATUS_LABEL.get(top["status"], top["status"]),
                "agent": winner,
                "agent_label": agent_label(winner),
                "text": top["text"],
                "thread": top["thread"],
                "source": top["source"],
                "last_push_age": age,
                "agents": agents,
                "mute": bool(self.cfg["audio"].get("mute", False)),
                "player": Path(player).name if player else None,
                "tts": find_tts(self.cfg["tts"].get("backend", "auto")),
                "uptime": int(time.time() - self.started),
                "history": self.history[-20:],
            }

    def winning_key(self) -> str:
        """灯/网页上大圆点显示谁：先比紧急程度，再比谁更新得晚。"""
        best = AGENT_SLOTS[0][0]
        best_score = -1
        for key, _label in AGENT_SLOTS:
            slot = self.slots[key]
            score = STATUS_PRIORITY.get(slot["status"], 0)
            if score > best_score or (score == best_score
                                      and slot["last_push"] > self.slots[best]["last_push"]):
                best_score = score
                best = key
        return best


SERVICE = None


# ---------------------------------------------------------------------------
#  网页
# ---------------------------------------------------------------------------
WEB_HTML = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Codex 语音播报</title>
<style>
  *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
  body{margin:0;min-height:100vh;padding:22px;display:flex;justify-content:center;
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","Microsoft YaHei",Roboto,sans-serif;
    background:radial-gradient(900px 620px at 50% -15%,#1e3a5f 0%,#0b1120 55%,#06090f 100%);
    color:#e6edf7}
  .wrap{width:100%;max-width:520px}
  .card{background:rgba(19,28,46,.75);border:1px solid rgba(120,170,255,.16);border-radius:22px;
    padding:22px;margin-bottom:16px;box-shadow:0 20px 50px rgba(0,0,0,.5);
    backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px)}
  h1{margin:0 0 4px;font-size:18px;font-weight:600;letter-spacing:.5px}
  .sub{margin:0 0 14px;font-size:12px;color:#8ea3c0}
  .state{display:flex;align-items:center;gap:10px;margin-bottom:8px}
  .dot{width:14px;height:14px;border-radius:50%;background:#4ade80;box-shadow:0 0 12px currentColor}
  .state b{font-size:16px;letter-spacing:.5px}
  .hint{font-size:11.5px;color:#7186a3;line-height:1.6;word-break:break-word;white-space:pre-wrap}
  .agents{display:flex;gap:8px;justify-content:center;flex-wrap:wrap;margin-top:12px}
  .agents span{font-size:12px;padding:6px 11px;border-radius:999px;
    background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.1);color:#c7d6ea}
  .agents i{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
  textarea{width:100%;min-height:96px;padding:12px;border-radius:14px;resize:vertical;
    background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.12);color:#e6edf7;
    font-family:inherit;font-size:15px;line-height:1.5}
  textarea:focus{outline:none;border-color:rgba(34,211,238,.6)}
  .row{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}
  .btn{flex:1 1 40%;min-width:118px;padding:12px 14px;border-radius:13px;font-size:14px;font-weight:600;
    cursor:pointer;font-family:inherit;color:#dbe6f5;background:rgba(255,255,255,.07);
    border:1px solid rgba(255,255,255,.12);transition:.2s}
  .btn:hover{background:rgba(255,255,255,.14);transform:translateY(-1px)}
  .btn:active{transform:scale(.98)}
  .btn.main{background:linear-gradient(135deg,#0891b2,#22d3ee);color:#04202a;border:none}
  .chips{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
  .chip{padding:7px 12px;border-radius:999px;font-size:12.5px;cursor:pointer;
    background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.1);color:#b9cbe3}
  .chip:hover{background:rgba(255,255,255,.13)}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:4px}
  .grid div{background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.07);
    border-radius:12px;padding:10px 12px}
  .k{display:block;font-size:10.5px;color:#7186a3;letter-spacing:.6px;margin-bottom:4px}
  .v{display:block;font-size:13.5px;font-weight:600;color:#d5e3f5;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .log{font-size:12px;color:#93a7c2;line-height:1.7;max-height:170px;overflow:auto;margin-top:12px}
  .ok{color:#4ade80}.bad{color:#f87171}
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <h1>Codex 语音播报</h1>
    <p class="sub">Ubuntu 状态提示音 + 文字朗读</p>
    <div class="state"><span class="dot" id="dot"></span><b id="label">空闲</b></div>
    <div class="hint" id="hint">等待推送…</div>
    <div class="agents" id="agents"></div>
  </div>
  <div class="card">
    <h1 style="font-size:16px">输入文字，让音箱念出来</h1>
    <p class="sub">支持中文，用的是女声（快捷键 Ctrl+Enter 也可以发送）</p>
    <textarea id="text" placeholder="例如：吃饭了 / 任务已完成，请检查 / 记得提交代码"></textarea>
    <div class="row">
      <button class="btn main" onclick="speak()">🔊 朗读</button>
      <button class="btn" onclick="toggleMute()" id="muteBtn">🔕 静音</button>
    </div>
    <div class="chips" id="chips"></div>
  </div>
  <div class="card">
    <h1 style="font-size:16px">测试各状态提示音</h1>
    <div class="row" id="tests"></div>
  </div>
  <div class="card">
    <div class="grid">
      <div><span class="k">运行时间</span><span class="v" id="uptime">--</span></div>
      <div><span class="k">播放器 / TTS</span><span class="v" id="tools">--</span></div>
    </div>
    <div class="log" id="log"></div>
  </div>
</div>
<script>
var COLORS = {"idle":"#4ade80","working":"#ffd60a","waiting_approval":"#ff453a",
              "completed":"#4da3ff","interrupted":"#d946ef","info":"#22d3ee"};
var LABELS = {"idle":"空闲","working":"工作中","waiting_approval":"等待审批",
              "completed":"已完成","interrupted":"已中断","info":"通知"};
var PHRASES = ["啵啵","臭屁屁","任务已完成，请检查","帮我看一下这个报错","收到，我马上处理"];
var MUTED = false;

function fmt(s){s=Math.floor(s);var h=Math.floor(s/3600),m=Math.floor((s%3600)/60);
  if(h>0)return h+" 小时 "+m+" 分"; if(m>0)return m+" 分 "+(s%60)+" 秒"; return s+" 秒";}

function push(line, cls){
  var log = document.getElementById("log");
  var t = new Date().toLocaleTimeString();
  log.innerHTML = '<div class="'+(cls||'')+'">'+t+"  "+line+"</div>" + log.innerHTML;
}

function render(d){
  var color = COLORS[d.codex] || "#7d8da4";
  var dot = document.getElementById("dot");
  dot.style.background = color; dot.style.color = color;
  document.getElementById("label").textContent = d.codex_label || "空闲";
  document.getElementById("label").style.color = color;

  // 两个来源各显示一行：粗体的是当前大圆点在显示的那个（按紧急程度选出来的）
  var list = document.getElementById("agents");
  if (d.agents && d.agents.length) {
    list.innerHTML = d.agents.map(function(a){
      var c = a.color || "#7d8da4";
      var style = (a.key === d.agent) ? "font-weight:700;border-color:" + c + "66;" : "opacity:.72;";
      return '<span style="' + style + '"><i style="background:' + c + '"></i>'
           + (a.label || a.key) + " · " + (a.state_label || a.state) + "</span>";
    }).join("");
  }

  var bits = [];
  if(d.thread) bits.push("任务：" + d.thread);
  if(d.text) bits.push(d.text);
  if(d.last_push_age !== null && d.last_push_age !== undefined) bits.push("最近推送 " + d.last_push_age + " 秒前");
  document.getElementById("hint").textContent = bits.join("\n") || "还没有收到任何推送";
  document.getElementById("uptime").textContent = fmt(d.uptime || 0);
  document.getElementById("tools").textContent = (d.player || "无播放器") + " / " + (d.tts || "无TTS");
  MUTED = !!d.mute;
  var btn = document.getElementById("muteBtn");
  btn.textContent = MUTED ? "🔔 取消静音" : "🔕 静音";
  btn.style.color = MUTED ? "#f87171" : "";
}

function refresh(){
  fetch("/api/status").then(function(r){return r.json();}).then(render).catch(function(){});
}

function speak(){
  var box = document.getElementById("text");
  var text = box.value.trim();
  if(!text){ push("请输入要朗读的文字","bad"); return; }
  fetch("/api/speak",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({text:text})})
    .then(function(r){return r.json();})
    .then(function(d){
      if(d.ok){ push("已提交朗读：" + text); box.value = ""; }
      else { push("朗读失败：" + (d.detail || d.error || "未知错误"), "bad"); }
    })
    .catch(function(){ push("请求失败，检查网络","bad"); });
}

function toggleMute(){
  fetch("/api/mute",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({mute:!MUTED})})
    .then(function(r){return r.json();})
    .then(function(d){ render(d); push(MUTED ? "已静音（提示音和朗读都不出声）" : "已取消静音"); })
    .catch(function(){ push("请求失败","bad"); });
}

function testSound(status){
  fetch("/api/codex",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({status:status,text:"（测试）",source:"网页测试"})})
    .then(function(r){return r.json();})
    .then(function(d){ render(d); push("测试提示音：" + (LABELS[status] || status)); })
    .catch(function(){ push("请求失败","bad"); });
}

document.getElementById("text").addEventListener("keydown", function(e){
  if((e.ctrlKey || e.metaKey) && e.key === "Enter") speak();
});

(function init(){
  var chips = document.getElementById("chips");
  PHRASES.forEach(function(p){
    var el = document.createElement("span");
    el.className = "chip";
    el.textContent = p;
    el.onclick = function(){ document.getElementById("text").value = p; speak(); };
    chips.appendChild(el);
  });
  var tests = document.getElementById("tests");
  Object.keys(LABELS).forEach(function(k){
    if(k === "idle") return;
    var b = document.createElement("button");
    b.className = "btn";
    b.textContent = LABELS[k];
    b.onclick = function(){ testSound(k); };
    tests.appendChild(b);
  });
  refresh();
  setInterval(refresh, 2000);
})();
</script>
</body>
</html>
"""


# ---------------------------------------------------------------------------
#  HTTP 服务
# ---------------------------------------------------------------------------
USAGE = """Codex 语音播报服务

网页        GET  /                     输入文字 -> 女声朗读
状态推送    POST /api/codex            {"status":"working","text":"...","thread":"..."}
            GET  /api/codex?status=working
测试提示音  GET  /api/beep?n=2&ms=120
朗读        POST /api/speak            {"text":"要念的内容","voice":"可选"}
静音        POST /api/mute             {"mute":true}   /  GET /api/mute?m=1
状态查询    GET  /api/status
健康检查    GET  /health
"""


class Handler(BaseHTTPRequestHandler):
    server_version = "CodexVoiceNotify/1.0"

    def log_message(self, fmt, *args):  # noqa: D102 - 走自定义日志
        log(f"[HTTP] {self.address_string()} {fmt % args}")

    # ---- 工具 ----

    def _send(self, body: bytes, content_type: str, code: int = 200) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, payload: dict, code: int = 200) -> None:
        self._send(json.dumps(payload, ensure_ascii=False, indent=2).encode("utf-8"),
                   "application/json; charset=utf-8", code)

    def _html(self, body: str) -> None:
        self._send(body.encode("utf-8"), "text/html; charset=utf-8")

    def _text(self, body: str, code: int = 200) -> None:
        self._send(body.encode("utf-8"), "text/plain; charset=utf-8", code)

    def _body(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        raw = self.rfile.read(length).decode("utf-8", "replace")
        try:
            data = json.loads(raw)
            if isinstance(data, dict):
                return data
        except json.JSONDecodeError:
            pass
        return {k: v[0] for k, v in parse_qs(raw).items()}

    def _authorized(self) -> bool:
        token = str(SERVICE.cfg["server"].get("token") or "").strip()
        if not token:
            return True
        header = self.headers.get("Authorization") or ""
        if header.lower().startswith("bearer ") and header[7:].strip() == token:
            return True
        return parse_qs(urlparse(self.path).query).get("token", [""])[0] == token

    def _ok(self) -> bool:
        if self._authorized():
            return True
        self._json({"ok": False, "error": "unauthorized"}, 401)
        return False

    # ---- 路由 ----

    def do_GET(self) -> None:  # noqa: N802
        route = urlparse(self.path).path.rstrip("/") or "/"
        query = parse_qs(urlparse(self.path).query)

        if route in ("/", "/index.html"):
            return self._html(WEB_HTML)
        if route in ("/help", "/api"):
            return self._text(USAGE)
        if not self._ok():
            return

        if route == "/health":
            player, _, _ = find_player(SERVICE.cfg["audio"].get("player", "auto"))
            return self._json({
                "ok": True,
                "pid": os.getpid(),
                "player": Path(player).name if player else None,
                "tts": find_tts(SERVICE.cfg["tts"].get("backend", "auto")),
                "mute": bool(SERVICE.cfg["audio"].get("mute", False)),
                "uptime": int(time.time() - SERVICE.started),
            })
        if route == "/api/status":
            return self._json(SERVICE.snapshot())
        if route == "/api/codex":
            value = (query.get("status", [""])[0] or "").strip()
            if value:
                SERVICE.set_status_with_agent(
                    value,
                    text=query.get("text", [""])[0],
                    thread_name=query.get("thread", [""])[0],
                    source="网页/GET",
                    agent=query.get("agent", [""])[0],
                )
            return self._json(SERVICE.snapshot())
        if route == "/api/beep":
            times = max(1, min(8, int(query.get("n", ["2"])[0] or 2)))
            ms = max(20, min(2000, int(query.get("ms", ["120"])[0] or 120)))
            sequence = []
            for _ in range(times):
                sequence += [[2000, ms], [0, 80]]
            SERVICE.player.play_sequence(sequence, tag="api-beep")
            return self._json({"ok": True, "beeps": times, "ms": ms})
        if route == "/api/mute":
            value = (query.get("m", ["1"])[0] or "1").lower()
            SERVICE.cfg["audio"]["mute"] = value in ("1", "true", "on", "yes", "静音")
            return self._json(SERVICE.snapshot())
        return self._json({"ok": False, "error": f"未知路径 {route}", "usage": USAGE}, 404)

    def do_POST(self) -> None:  # noqa: N802
        route = urlparse(self.path).path.rstrip("/") or "/"
        if not self._ok():
            return
        data = self._body()

        if route == "/api/codex":
            raw_status = str(data.get("status") or "").strip()
            if not raw_status:
                return self._json({"ok": False, "error": "缺少 status"}, 400)
            SERVICE.set_status_with_agent(
                raw_status,
                text=str(data.get("text") or ""),
                thread_name=str(data.get("thread") or ""),
                source=str(data.get("source") or "推送"),
                agent=str(data.get("agent") or ""),
            )
            snapshot = SERVICE.snapshot()
            extra = f" - {snapshot['text'][:40]}" if snapshot["text"] else ""
            log(f"[推送] {snapshot['codex']}（{snapshot['codex_label']}）{extra}")
            return self._json(snapshot)

        if route == "/api/speak":
            text = str(data.get("text") or data.get("message") or "").strip()
            if not text:
                return self._json({"ok": False, "error": "缺少 text"}, 400)
            SERVICE.player.speak(text, voice=data.get("voice"), tag="api-speak")
            log(f"[朗读] {text[:60]}{'…' if len(text) > 60 else ''}")
            return self._json({"ok": True, "queued": True, "text": text[:200],
                               "tts": find_tts(SERVICE.cfg["tts"].get("backend", "auto"))})

        if route == "/api/mute":
            value = data.get("mute")
            if isinstance(value, str):
                value = value.lower() in ("1", "true", "on", "yes", "静音")
            SERVICE.cfg["audio"]["mute"] = bool(value)
            return self._json(SERVICE.snapshot())

        if route == "/api/beep":
            SERVICE.player.play_sequence([[2000, 120], [0, 80], [2000, 120]], tag="api-beep")
            return self._json({"ok": True})

        return self._json({"ok": False, "error": f"未知路径 {route}", "usage": USAGE}, 404)


# ---------------------------------------------------------------------------
#  main
# ---------------------------------------------------------------------------
def _run(cmd, timeout: float = 10) -> tuple[int, str, str]:
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return (result.returncode,
                (result.stdout or b"").decode("utf-8", "replace"),
                (result.stderr or b"").decode("utf-8", "replace"))
    except Exception as exc:  # noqa: BLE001
        return -1, "", f"{type(exc).__name__}: {exc}"


def diagnose(cfg: dict) -> int:
    """一键体检音频环境：有没有播放器、能不能真出声、输出设备是什么、TTS 通不通。"""
    log("=" * 64)
    log("  音频环境体检")
    log("=" * 64)
    log(f"运行平台   : {sys.platform}")
    log(f"配置文件   : {cfg['_path']}")
    log(f"当前静音   : {cfg['audio'].get('mute')}（静音时提示音会主动跳过）")
    log("环境变量   :")
    for key in ("USER", "XDG_RUNTIME_DIR", "PULSE_SERVER", "PULSE_RUNTIME_PATH",
                "DISPLAY", "WAYLAND_DISPLAY"):
        log(f"    {key:18} = {os.environ.get(key) or '(未设置)'}")

    snd = Path("/dev/snd")
    if snd.exists():
        log(f"声卡设备   : /dev/snd 存在 -> {sorted(p.name for p in snd.iterdir())[:8]}")
    else:
        note = "（Linux 上说明没有声卡或容器没挂载 /dev/snd，必然没声音）" if os.name != "nt" else ""
        log(f"声卡设备   : /dev/snd 不存在 {note}")

    test_wav = BASE / "audio_cache" / "diagnose.wav"
    build_tone_wav(test_wav, [[880, 400]], volume=0.7)
    players = available_players()
    names = ", ".join(Path(p).name for p, _, _ in players) or "无"
    log(f"找到播放器 : {names}")
    working_players: list = []
    for exe, _mp3, extra in players:
        code, _out, err = _run([exe] + list(extra) + [str(test_wav)], timeout=30)
        if code == 0:
            working_players.append(Path(exe).name)
            log(f'    {Path(exe).name:9} 试播成功（刚才应该响了一声「嘀」）')
        else:
            log(f"    {Path(exe).name:9} 失败(返回码 {code}): {err.strip()[:180]}")
    if not players:
        log("    ！一个播放器都没有：sudo apt install -y pulseaudio-utils   或   alsa-utils")
    elif working_players:
        log(f"建议播放器 : {working_players[0]}（服务会自动优先用最近一次成功的那个，失败过的会自动排到最后）")
    else:
        log("    ！所有播放器试播都失败了 —— 多半是音频服务/输出设备的问题，见下面结论")

    if shutil.which("pactl"):
        code, out, err = _run(["pactl", "info"])
        if code == 0:
            for line in out.strip().splitlines():
                if line.startswith(("Server Name", "Server Version", "Default Sink", "Default Source")):
                    log("音频服务   : " + line)
        else:
            log(f"音频服务   : pactl 连不上（{err.strip()[:120]}）→ PulseAudio/PipeWire 没在跑，或没连上用户会话")
        code, out, _err = _run(["pactl", "list", "short", "sinks"])
        log("输出设备   :")
        for line in (out.strip().splitlines() or ["    (无)"])[:6]:
            log("    " + line)
    if shutil.which("wpctl"):
        code, out, _err = _run(["wpctl", "status"])
        if code == 0:
            log("PipeWire   :")
            started = False
            for line in out.splitlines():
                if "Sinks:" in line:
                    started = True
                if started:
                    log("    " + line.strip())
                if started and line.strip().startswith("Sources:"):
                    break
    if shutil.which("aplay"):
        code, out, err = _run(["aplay", "-l"])
        log("ALSA 设备  :")
        for line in ((out or err).strip().splitlines() or ["    (无)"])[:8]:
            log("    " + line)

    backend = find_tts(cfg["tts"].get("backend", "auto"))
    log(f"TTS 后端   : {backend or '无 —— 装一个: sudo apt install -y espeak-ng，或 pip install edge-tts'}")
    has_ffmpeg = bool(shutil.which("ffmpeg"))
    has_mpg123 = bool(shutil.which("mpg123"))
    log(f"mp3 解码   : ffmpeg={'有' if has_ffmpeg else '无'}   mpg123={'有' if has_mpg123 else '无'}")
    if backend == "edge" and not (has_ffmpeg or has_mpg123):
        log("    ⚠️ edge-tts 只输出 mp3，而 pw-play/paplay 常常没有 mp3 解码；")
        log("       不装 ffmpeg/mpg123 会播不出来、甚至被 aplay 放成噪音。")
        log("       建议执行：sudo apt install -y ffmpeg")
    if backend == "espeak":
        exe = shutil.which("espeak-ng") or shutil.which("espeak")
        code, _out, err = _run([exe, "-v", cfg["tts"].get("espeak_voice") or "cmn+f3",
                                "-w", str(BASE / "audio_cache" / "diagnose_tts.wav"), "测试"], timeout=30)
        log(f"    espeak 试合成: {'成功' if code == 0 else '失败 ' + err.strip()[:150]}")
    elif backend == "edge":
        code, _out, err = _run(["edge-tts", "--voice", cfg["tts"].get("voice"),
                                "--text", "测试", "--write-media",
                                str(BASE / "audio_cache" / "diagnose_tts.mp3")], timeout=60)
        log(f"    edge-tts 试合成: {'成功' if code == 0 else '失败（多半是没联网）' + err.strip()[:150]}")
    elif backend == "piper":
        log(f"    piper 模型: {cfg['tts'].get('piper_model') or '未配置（tts.piper_model）'}")

    log("-" * 64)
    log("结论提示：")
    log("  · 播放器试播成功但你没听到 → 输出设备不对（HDMI/USB/耳机），")
    log("    用 'pactl list short sinks' 看名字，再 'pactl set-default-sink <名字>' 切到音箱")
    log("  · pactl 连不上 → 你多半在 SSH / 容器里跑，没有音频会话；")
    log("    改用桌面终端跑，或用 README 第 8 节的 systemd --user 方式")
    log("  · TTS 显示'无' → 装 espeak-ng（离线保底）或 edge-tts（音质好，要联网）")
    log("=" * 64)
    return 0


def list_voices(cfg: dict) -> int:
    """列出 edge-tts 的可用中文音色，方便挑一个更像真人的。"""
    if not shutil.which("edge-tts"):
        log("还没装 edge-tts。装它就能用微软的神经语音（比 espeak 自然得多）：")
        log("    pip install edge-tts")
        log("装完再执行：python3 server.py --list-voices")
        return 1

    log("正在向 edge-tts 查询音色列表…")
    code, out, err = _run(["edge-tts", "--list-voices"], timeout=60)
    if code != 0:
        log(f"查询失败：{err.strip()[:200]}")
        log("提示：edge-tts 需要联网。没有外网就用 piper（离线神经语音），见 README。")
        return 1

    current = str(cfg["tts"].get("voice") or "")
    log("=" * 64)
    log(f"  中文音色（当前配置：{current or '未设置'}）")
    log("=" * 64)
    shown = 0
    for line in out.splitlines():
        # Name: zh-CN-XiaoxiaoNeural  Gender: Female  ...
        if "zh-" not in line and "cmn-" not in line:
            continue
        if "Female" not in line and "Male" not in line:
            continue
        name = ""
        gender = "Female" if "Female" in line else "Male"
        for part in line.split():
            if part.startswith(("zh-", "cmn-")):
                name = part
                break
        if not name:
            continue
        mark = "  ← 当前使用" if name == current else ""
        log(f"  {name:36} {'女声' if gender == 'Female' else '男声'}{mark}")
        shown += 1

    log("-" * 64)
    log(f"共 {shown} 个中文音色。挑好后改 config.json：")
    log('    "tts": { "backend": "edge", "voice": "zh-CN-XiaoyiNeural" }')
    log("推荐女声：zh-CN-XiaoxiaoNeural（晓晓，标准） / zh-CN-XiaoyiNeural（晓伊，更甜）")
    log("          zh-CN-liaoning-XiaobeiNeural（东北） / zh-TW-HsiaoChenNeural（台湾国语）")
    log("还可微调 rate（语速，如 +10%）、pitch（音调，如 -20Hz 更低沉柔和）、volume。")
    return 0


def main(argv=None) -> int:
    global SERVICE
    safe_stdout()
    parser = argparse.ArgumentParser(description="Codex 状态语音播报服务（Ubuntu）")
    parser.add_argument("--config", help="配置文件路径，默认同目录 config.json")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    parser.add_argument("--mute", action="store_true", help="启动时静音")
    parser.add_argument("--list-tools", action="store_true", help="看看本机有哪些播放器/TTS 可用")
    parser.add_argument("--diagnose", action="store_true", help="音频环境体检（测播放器、输出设备、TTS）")
    parser.add_argument("--list-voices", action="store_true", help="列出 edge-tts 的中文音色（挑更像真人的）")
    parser.add_argument("--warmup", action="store_true", help="把 config 里 tts.warmup 的常用短语预合成进缓存")
    parser.add_argument("--test-sounds", action="store_true", help="依次播放各状态的提示音")
    parser.add_argument("--say", help="直接朗读一句话")
    args = parser.parse_args(argv)

    cfg = load_config(args.config)
    if args.host:
        cfg["server"]["host"] = args.host
    if args.port:
        cfg["server"]["port"] = int(args.port)
    if args.mute:
        cfg["audio"]["mute"] = True

    if args.list_tools:
        player, mp3_ok, _ = find_player(cfg["audio"].get("player", "auto"))
        log(f"配置文件 : {cfg['_path']}")
        log("播放器   : " + (player or "未找到（装一个: sudo apt install pulseaudio-utils 或 alsa-utils）")
            + (f"   mp3={'支持' if mp3_ok else '不支持'}" if player else ""))
        log("TTS 后端 : " + (find_tts(cfg["tts"].get("backend", "auto"))
                             or "未找到（sudo apt install espeak-ng，或 pip install edge-tts）"))
        return 0

    if args.diagnose:
        return diagnose(cfg)

    if args.list_voices:
        return list_voices(cfg)

    if args.warmup:
        texts = [str(item).strip() for item in (cfg["tts"].get("warmup") or []) if str(item).strip()]
        for text in ((cfg.get("announce") or {}).get("phrases") or {}).values():
            text = str(text).strip()
            if text and text not in texts:
                texts.append(text)
        if not texts:
            log("config.json 里的 tts.warmup 和 announce.phrases 都是空的，没什么可预热。")
            log('可以这样写： "tts": {"warmup": ["吃饭了"]}, "announce": {"phrases": {"completed": "任务已完成"}}')
            return 0
        player = AudioPlayer(cfg)          # 只建播放器/合成器，不启动服务
        log(f"[预热] 预合成 {len(texts)} 条常用短语…")
        for text in texts:
            player.warmup(text)
        log(f"[预热] 完成，缓存目录：{player._cache_path('')}")
        return 0

    SERVICE = Service(cfg)

    if args.test_sounds:
        for status in ("working", "waiting_approval", "completed", "interrupted", "info"):
            log(f"播放 {status} …")
            SERVICE.player.play_sequence((cfg["sounds"] or {}).get(status) or [], tag=status)
            SERVICE.player.queue.join()
            time.sleep(0.3)
        return 0

    if args.say:
        SERVICE.player.speak(args.say, tag="cli")
        SERVICE.player.queue.join()
        return 0

    host = str(cfg["server"]["host"])
    port = int(cfg["server"]["port"])
    player, _, _ = find_player(cfg["audio"].get("player", "auto"))
    shown = "本机IP" if host in ("0.0.0.0", "::") else host
    log("=" * 62)
    log("  Codex 语音播报服务")
    log("=" * 62)
    log(f"  配置文件 : {cfg['_path']}")
    log(f"  播放器   : {player or '未找到！装一个: sudo apt install pulseaudio-utils'}")
    log("  TTS      : " + (find_tts(cfg["tts"].get("backend", "auto"))
                           or "未找到（sudo apt install espeak-ng，或 pip install edge-tts）"))
    log(f"  网页     : http://{shown}:{port}/")
    log(f"  推送接口 : http://{shown}:{port}/api/codex")
    log("=" * 62)

    try:
        server = ThreadingHTTPServer((host, port), Handler)
    except OSError as exc:
        log(f"[错误] 无法监听 {host}:{port} -> {exc}")
        log("       换端口：--port 8081，或改 config.json 里的 server.port")
        return 2
    server.daemon_threads = True
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("已停止")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
