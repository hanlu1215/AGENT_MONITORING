#!/usr/bin/env python3
"""Generate Edge TTS MP3 files using the cache names expected by the C++ app."""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import re
import sys
from pathlib import Path


APP_DIR = Path(__file__).resolve().parents[1]
DEFAULT_PHRASES = {
    "working": "{agent} 开始工作",
    "waiting_approval": "{agent} 需要审批",
    "completed": "{agent} 任务已完成",
    "interrupted": "{agent} 任务中断",
}
AGENTS = ("Codex", "Claude Code")


def load_config():
    path = APP_DIR / "config.json"
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"读取配置失败：{path}\n{exc}") from exc


def pinyin_slug(text: str, max_length: int = 48) -> str:
    try:
        from pypinyin import Style, lazy_pinyin
    except ImportError as exc:
        raise SystemExit("缺少 pypinyin，请运行：python -m pip install pypinyin") from exc

    hanzi = lambda ch: 0x4E00 <= ord(ch) <= 0x9FFF or 0x3400 <= ord(ch) <= 0x4DBF or 0xF900 <= ord(ch) <= 0xFAFF
    pieces = []
    for char in text:
        code = ord(char)
        if char.isascii() and char.isalnum():
            pieces.append(char.lower())
        elif char == "-":
            pieces.append("-")
        elif hanzi(char):
            syllable = lazy_pinyin(char, style=Style.NORMAL, errors=lambda chars: [""])[0]
            pieces.append(syllable if syllable else f"u{code:04x}")
        else:
            pieces.append("-")

    slug = re.sub(r"-+", "-", "".join(pieces)).strip("-")
    return slug[:max_length].rstrip("-")


def read_settings(config):
    sound = config.get("local_sound", {})
    voice = sound.get("voice", "zh-CN-XiaoxiaoNeural")
    rate = sound.get("voice_rate", "+0%")
    volume = sound.get("voice_volume", "+0%")
    cache_dir = Path(sound.get("cache_dir", "audio_cache"))
    if not cache_dir.is_absolute():
        cache_dir = APP_DIR / cache_dir
    return voice, rate, volume, cache_dir


def build_phrases(config, args):
    if args.text is not None:
        agent = args.agent or "Codex"
        return [(args.text.replace("{agent}", agent),)]

    custom = config.get("local_sound", {}).get("phrases", {})
    result = []
    for status, default in DEFAULT_PHRASES.items():
        if args.status and status not in args.status:
            continue
        phrase = custom.get(status) or default
        for agent in AGENTS:
            result.append((phrase.replace("{agent}", agent),))

    # Some custom configs can map different statuses to the same spoken text.
    return list(dict.fromkeys(result))


async def synthesize(phrases, voice, rate, volume, cache_dir, proxy=None):
    try:
        import edge_tts
    except ImportError as exc:
        raise SystemExit("缺少 edge-tts，请运行：python -m pip install edge-tts") from exc

    cache_dir.mkdir(parents=True, exist_ok=True)
    for (phrase,) in phrases:
        if not phrase.strip():
            continue
        slug = pinyin_slug(phrase) or "voice"
        fingerprint = hashlib.sha1(
            f"{voice}|{rate}|{volume}|{phrase}".encode("utf-8")
        ).hexdigest()[:6]
        destination = cache_dir / f"tts-{slug}-{fingerprint}.mp3"
        if destination.is_file() and destination.stat().st_size:
            print(f"已存在，跳过：{destination.name}")
            continue
        temporary = destination.with_name(destination.name + ".part.mp3")
        try:
            await edge_tts.Communicate(
                phrase, voice=voice, rate=rate, volume=volume, proxy=proxy
            ).save(str(temporary))
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
        print(f"已生成：{destination.name}  [{phrase}]")


def main():
    parser = argparse.ArgumentParser(
        description="按 C++ 程序使用的格式生成晓晓 Edge TTS 缓存音频。"
    )
    parser.add_argument(
        "--status",
        action="append",
        choices=sorted(DEFAULT_PHRASES),
        help="只生成指定状态，可重复传入；省略时生成全部默认状态。",
    )
    parser.add_argument("--text", help="只生成这句自定义文本。")
    parser.add_argument("--agent", help="配合 --text 替换 {agent}，默认 Codex。")
    parser.add_argument(
        "--proxy",
        help="可选 HTTP/HTTPS 代理，例如 http://127.0.0.1:7890。",
    )
    args = parser.parse_args()
    config = load_config()
    voice, rate, volume, cache_dir = read_settings(config)
    phrases = build_phrases(config, args)
    print(f"语音：{voice}，语速：{rate}，音量：{volume}")
    print(f"缓存目录：{cache_dir}")
    try:
        asyncio.run(synthesize(phrases, voice, rate, volume, cache_dir, args.proxy))
    except Exception as exc:
        status = getattr(exc, "status", None)
        if status == 403:
            print(
                "Edge TTS 服务返回 HTTP 403，拒绝了 WebSocket 连接。"
                "请先校准 Windows 日期和时间，再更新 edge-tts 后重试；"
                "若当前网络限制了 speech.platform.bing.com，换网络或传入可用代理（--proxy）。",
                file=sys.stderr,
            )
        else:
            print(f"生成失败（{type(exc).__name__}）：{exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
