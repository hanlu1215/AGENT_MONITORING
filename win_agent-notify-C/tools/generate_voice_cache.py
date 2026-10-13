#!/usr/bin/env python3
"""预生成 Edge TTS（晓晓）语音缓存，供 win_agent-notify-C 直接播放。

文件名必须和 C++ 端 localsound.cpp 算出来的**逐字节一致**，否则程序找不到缓存，
会在运行时现场重新合成（没网时就直接退回系统语音）：

    tts-<PinyinSlug(文本, 48)>-<sha1(voice|rate|volume|文本)[:6]>.mp3

所以下面 pinyin_slug() 是 src/util/pinyin.cpp 里 PinyinSlug() 的逐条移植：
两张表（C++ 的 pinyin_data.cpp 和这里的 pypinyin）本来就是同源生成的，
但**拼接规则必须是手写一份**——两边不一致时程序会认不出生成的音频。改任何一边都要改另一边。

合成端点走 edge_tts_endpoint（微软新地址）：旧的 speech.platform.bing.com
从 2026-10 起对合成请求返回 403，这里会自动切到 api.msedgeservices.com。

用法：
    .\\.venv\\Scripts\\python.exe tools\\generate_voice_cache.py            # 生成全部默认播报
    .\\.venv\\Scripts\\python.exe tools\\generate_voice_cache.py --verify   # 只看该有哪些文件、缺哪些
    .\\.venv\\Scripts\\python.exe tools\\generate_voice_cache.py --status waiting_approval
    .\\.venv\\Scripts\\python.exe tools\\generate_voice_cache.py --text "开会了"
    .\\.venv\\Scripts\\python.exe tools\\generate_voice_cache.py --proxy http://127.0.0.1:7890
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path


APP_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import edge_tts_endpoint  # noqa: E402

# 与 src/localsound.cpp 的 DefaultPhrases() 保持一致
DEFAULT_PHRASES = {
    "working": "{agent} 开始工作",
    "waiting_approval": "{agent} 需要审批",
    "completed": "{agent} 任务已完成",
    "interrupted": "{agent} 任务中断",
}
# 与 AgentLabel() 一致
AGENTS = ("Codex", "Claude Code")

SLUG_MAX_LENGTH = 48          # C++ 端调的是 PinyinSlug(job.phrase, 48)
DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"
DEFAULT_RATE = "+0%"
DEFAULT_VOLUME = "+0%"

# 直连失败时自动尝试的本机代理端口（Clash / v2ray 等的常见默认值）
AUTO_PROXY_PORTS = (
    ("http://127.0.0.1:7890", 7890),
    ("http://127.0.0.1:7897", 7897),
    ("http://127.0.0.1:10809", 10809),
    ("http://127.0.0.1:1080", 1080),
)


class SynthesisError(RuntimeError):
    """带 HTTP 状态码的合成失败，便于 main() 给出针对性提示。"""

    def __init__(self, message: str, status: int = 0):
        super().__init__(message)
        self.status = status


def safe_stdout() -> None:
    """控制台代码页装不下的字符不要让脚本崩掉（同 ubuntu_voice-notify/server.py）。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):
            pass


# ---------------------------------------------------------------------------
#  文件名（必须与 C++ 端一致）
# ---------------------------------------------------------------------------

def _is_hanzi(code: int) -> bool:
    """与 src/util/pinyin.cpp 的 IsHanzi() 完全一致。"""
    return (0x4E00 <= code <= 0x9FFF
            or 0x3400 <= code <= 0x4DBF
            or 0xF900 <= code <= 0xFAFF)


def _append_with_dash(out: list[str], piece: str) -> None:
    """对应 C++ 的 AppendWithDash()：需要时补一个 '-'，再拼上这一段。"""
    if not piece:
        return
    if out and out[-1] != "-":
        out.append("-")
    out.append(piece)


def pinyin_slug(text: str, max_length: int = SLUG_MAX_LENGTH) -> str:
    """汉字转拼音 slug。逐条对齐 src/util/pinyin.cpp: PinyinSlug()。

    关键点：**每个汉字音节之间要插 '-'**（"开始工作" -> "kai-shi-gong-zuo"），
    不能像 ''.join(pieces) 那样把相邻音节直接粘成 "kaishigongzuo"——
    后者会和 C++ 生成的文件名对不上，程序永远找不到缓存。
    """
    try:
        from pypinyin import Style, lazy_pinyin
    except ImportError as exc:
        raise SystemExit("缺少 pypinyin，请运行：python -m pip install pypinyin") from exc

    out: list[str] = []
    for char in text:
        code = ord(char)
        if ("a" <= char <= "z") or ("0" <= char <= "9"):
            out.append(char)                      # ASCII 字母数字：直接接上，不加 '-'
        elif "A" <= char <= "Z":
            out.append(char.lower())
        elif _is_hanzi(code):
            syllable = lazy_pinyin(char, style=Style.NORMAL, errors=lambda c: [""])[0]
            _append_with_dash(out, syllable or f"u{code:04x}")
        elif char == "-":
            if out and out[-1] != "-":
                out.append("-")
        else:
            if out and out[-1] != "-":            # 空格、标点等一律当分隔符
                out.append("-")

    slug = "".join(out).rstrip("-")
    if len(slug) > max_length:
        slug = slug[:max_length].rstrip("-")
    return slug


def cache_name(phrase: str, voice: str, rate: str, volume: str) -> str:
    """算出 C++ 端会查找的文件名（localsound.cpp: Play() 里那三行）。"""
    slug = pinyin_slug(phrase) or "voice"
    fingerprint = hashlib.sha1(
        f"{voice}|{rate}|{volume}|{phrase}".encode("utf-8")
    ).hexdigest()[:6]
    return f"tts-{slug}-{fingerprint}.mp3"


# ---------------------------------------------------------------------------
#  配置与待生成清单
# ---------------------------------------------------------------------------

def load_config() -> dict:
    path = APP_DIR / "config.json"
    if not path.exists():
        print(f"提示：没找到 {path}，按默认设置生成。")
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"读取配置失败：{path}\n{exc}") from exc


def read_settings(config: dict):
    sound = config.get("local_sound") or {}
    voice = sound.get("voice") or DEFAULT_VOICE
    rate = sound.get("voice_rate") or DEFAULT_RATE
    volume = sound.get("voice_volume") or DEFAULT_VOLUME
    cache_dir = Path(sound.get("cache_dir") or "audio_cache")
    if not cache_dir.is_absolute():
        cache_dir = APP_DIR / cache_dir
    return voice, rate, volume, cache_dir


def build_phrases(config: dict, args) -> list[str]:
    """返回要合成的文本列表（已把 {agent} 展开）。"""
    if args.text is not None:
        agent = args.agent or "Codex"
        return [args.text.replace("{agent}", agent).strip()]

    custom = (config.get("local_sound") or {}).get("phrases") or {}
    phrases: list[str] = []
    for status, default in DEFAULT_PHRASES.items():
        if args.status and status not in args.status:
            continue
        template = custom.get(status) or default
        for agent in AGENTS:
            text = template.replace("{agent}", agent).strip()
            if text and text not in phrases:
                phrases.append(text)
    return phrases


# ---------------------------------------------------------------------------
#  合成的传输层
# ---------------------------------------------------------------------------

def _local_proxy_available(port: int) -> bool:
    import socket

    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.4):
            return True
    except OSError:
        return False


def candidate_routes(args) -> list:
    """直连优先；直连不行再依次试环境变量和本机常见代理端口。"""
    if args.proxy:
        return [args.proxy]
    if args.no_proxy:
        return [None]

    routes = [None]
    env_proxy = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy") \
        or os.environ.get("ALL_PROXY") or os.environ.get("all_proxy")
    if env_proxy:
        routes.append(env_proxy)
    for url, port in AUTO_PROXY_PORTS:
        if url not in routes and _local_proxy_available(port):
            routes.append(url)
    return routes


async def _synthesize_api(phrase, destination, voice, rate, volume, proxy) -> None:
    import edge_tts

    kwargs = dict(voice=voice, rate=rate, volume=volume)
    # 不同版本的 Communicate() 支持的参数不完全一样：带 proxy 失败就退回不带。
    try:
        communicate = edge_tts.Communicate(phrase, proxy=proxy, **kwargs)
    except TypeError:
        if proxy:
            raise SynthesisError("当前 edge-tts 版本不支持 proxy 参数，请先升级：pip install -U edge-tts")
        communicate = edge_tts.Communicate(phrase, **kwargs)
    await communicate.save(str(destination))


def _condense(text: str) -> str:
    """把一大段报错压成一行：优先取异常那一行，否则取最后一个非空行。"""
    lines = [line.strip() for line in (text or "").splitlines() if line.strip()]
    picked = ""
    for line in reversed(lines):
        if re.match(r"^[\w.]*(Error|Exception|error)\b.*:", line):
            picked = line
            break
    if not picked:
        picked = lines[-1] if lines else ""
    return picked.split(", url=")[0].strip()[:300]


def _reason(exc: Exception) -> str:
    """异常 -> 一行可读的原因（去掉又长又没用的 URL）。"""
    text = str(exc).split(", url=")[0].strip() or type(exc).__name__
    return f"{type(exc).__name__}: {text}"


def _synthesize_cli(phrase, destination, voice, rate, volume, proxy, cli_entry) -> None:
    """对应 ubuntu_voice-notify/server.py 的做法：库接口失败时退回命令行试一次。"""
    if cli_entry is None:
        raise SynthesisError("找不到 edge_tts_cli.py，无法回退到命令行")
    command = [
        sys.executable, str(cli_entry), "--voice", voice,
        f"--rate={rate}", f"--volume={volume}", "--text", phrase,
        "--write-media", str(destination),
    ]
    if proxy:
        command.extend(["--proxy", proxy])
    result = subprocess.run(
        command, capture_output=True, text=True, encoding="utf-8",
        errors="replace", timeout=120, check=False,
    )
    if result.returncode != 0 or not (destination.is_file() and destination.stat().st_size):
        detail = result.stderr or result.stdout or "命令行未生成音频"
        status = 403 if "403" in detail else 0
        raise SynthesisError(f"命令行重试：{_condense(detail)}", status=status)


async def synthesize_one(phrase, destination, voice, rate, volume, routes) -> tuple:
    """依次尝试 端点 × 线路，返回 (端点说明, 线路说明)。"""
    temporary = destination.with_name(destination.name + ".part.mp3")
    cli_entry = Path(__file__).resolve().parent / "edge_tts_cli.py"
    last_error = None

    for legacy in (False, True):
        edge_tts_endpoint.apply(legacy=legacy)
        label = "旧端点" if legacy else "新端点"
        for route in routes:
            for _attempt in (1, 2):                 # 每条线路重试两次，扛住偶发握手失败
                temporary.unlink(missing_ok=True)
                try:
                    await _synthesize_api(phrase, temporary, voice, rate, volume, route)
                except Exception as exc:            # noqa: BLE001 - 统一转成带状态码的错误
                    last_error = exc
                    status = getattr(exc, "status", 0) or (403 if "403" in str(exc) else 0)
                    try:
                        _synthesize_cli(phrase, temporary, voice, rate, volume, route,
                                        cli_entry if cli_entry.exists() else None)
                    except Exception as cli_exc:    # noqa: BLE001
                        last_error = cli_exc
                        status = getattr(cli_exc, "status", 0) or status
                        temporary.unlink(missing_ok=True)
                        if status in (401, 403):
                            break                   # 这个端点被拒，别浪费时间重试
                        await asyncio.sleep(1.0)
                        continue

                if temporary.is_file() and temporary.stat().st_size:
                    temporary.replace(destination)
                    return label, ("直连" if route is None else f"经代理 {route}")
                last_error = SynthesisError("合成没有产生有效 MP3")
                await asyncio.sleep(1.0)

            temporary.unlink(missing_ok=True)

    status = getattr(last_error, "status", 0) or (403 if "403" in str(last_error or "") else 0)
    raise SynthesisError(
        last_error if isinstance(last_error, SynthesisError) else _reason(last_error),
        status=status,
    )


async def run(phrases, voice, rate, volume, cache_dir, routes, force) -> int:
    cache_dir.mkdir(parents=True, exist_ok=True)
    ok = skipped = failed = blocked = 0

    for phrase in phrases:
        name = cache_name(phrase, voice, rate, volume)
        destination = cache_dir / name
        if destination.is_file() and destination.stat().st_size and not force:
            print(f"已存在，跳过：{name}")
            skipped += 1
            continue
        try:
            endpoint, via = await synthesize_one(phrase, destination, voice, rate, volume, routes)
        except SynthesisError as exc:
            failed += 1
            if exc.status in (401, 403):
                blocked += 1
            suffix = f"（HTTP {exc.status}）" if exc.status else ""
            print(f"失败{suffix}：{phrase}\n    {exc}", file=sys.stderr)
        else:
            ok += 1
            print(f"已生成（{endpoint}/{via}）：{name}  [{phrase}]")

    print(f"\n完成：新生成 {ok} 个，已存在 {skipped} 个，失败 {failed} 个。")
    if ok:
        print(f"缓存目录：{cache_dir}")
    if blocked:
        print(
            f"\n{blocked} 条被 Microsoft 拒绝（HTTP 401/403）。新端点和旧端点都试过了，"
            "\n所以不是端点选错，通常是这个网络出口被限流或不在服务区："
            "\n  · 确认系统日期时间准确（时区错也会被拒）；"
            "\n  · 换线路：--proxy http://127.0.0.1:7890，或换一个能正常访问 Edge 朗读的网络；"
            "\n  · 暂时拿不到晓晓时，把 config.json 的 local_sound.speech_backend 改成 \"sapi\"，"
            "程序会用 Windows 自带的中文女声。",
            file=sys.stderr,
        )
    return 1 if failed else 0


def verify(phrases, voice, rate, volume, cache_dir) -> int:
    """只列出程序会查找的文件名，以及本地有没有。"""
    print(f"语音：{voice}  语速：{rate}  音量：{volume}")
    print(f"缓存目录：{cache_dir}\n")
    missing = 0
    for phrase in phrases:
        name = cache_name(phrase, voice, rate, volume)
        path = cache_dir / name
        state = "有" if path.is_file() and path.stat().st_size else "缺"
        if state == "缺":
            missing += 1
        print(f"[{state}] {name}\n       {phrase}")
    print(f"\n共 {len(phrases)} 条，缺 {missing} 条。")
    return 1 if missing else 0


def main() -> int:
    safe_stdout()
    parser = argparse.ArgumentParser(
        description="按 C++ 程序使用的文件名生成晓晓（zh-CN-XiaoxiaoNeural）语音缓存。"
    )
    parser.add_argument("--status", action="append", choices=sorted(DEFAULT_PHRASES),
                        help="只生成指定状态，可重复；省略时生成全部。")
    parser.add_argument("--text", help="只生成这一句自定义文本。")
    parser.add_argument("--agent", help="配合 --text 替换 {agent}，默认 Codex。")
    parser.add_argument("--force", action="store_true", help="已有的缓存也重新生成。")
    parser.add_argument("--verify", action="store_true", help="只检查该有哪些文件、缺哪些，不联网。")
    parser.add_argument("--proxy", help="强制走指定代理，例如 http://127.0.0.1:7890。")
    parser.add_argument("--no-proxy", action="store_true", help="只直连，不自动尝试本机代理。")
    parser.add_argument("--legacy", action="store_true",
                        help="只用微软的旧合成端点（默认先试新端点，失败再试旧的）。")
    args = parser.parse_args()

    config = load_config()
    voice, rate, volume, cache_dir = read_settings(config)
    phrases = build_phrases(config, args)
    if not phrases:
        print("没有需要生成的文本。", file=sys.stderr)
        return 1

    if args.verify:
        return verify(phrases, voice, rate, volume, cache_dir)

    routes = candidate_routes(args)
    # 先落一次 patch，下面打印的端点才是真正要用的那个
    edge_tts_endpoint.apply(legacy=args.legacy)

    described = " -> ".join("直连" if r is None else r for r in routes)
    print(f"语音：{voice}  语速：{rate}  音量：{volume}")
    print(f"缓存目录：{cache_dir}")
    print(f"端点：{edge_tts_endpoint.current().split('/')[2]}"
          f"{'（仅旧端点）' if args.legacy else '（失败会自动回退旧端点）'}")
    print(f"线路：{described}\n", flush=True)

    try:
        return asyncio.run(run(phrases, voice, rate, volume, cache_dir, routes, args.force))
    except KeyboardInterrupt:
        print("\n已中断。", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
