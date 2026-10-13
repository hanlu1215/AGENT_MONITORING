#!/usr/bin/env python3
"""`python -m edge_tts` 的替代入口：先切到新端点，再执行官方命令行。

主程序（localsound.cpp 的 SynthEdge）要现合成一段没缓存的语音时调的就是这个脚本，
参数和 `python -m edge_tts` 完全一样：

    python tools/edge_tts_cli.py --voice zh-CN-XiaoxiaoNeural --text "你好" --write-media out.mp3

为什么要多这一层：PyPI 上的 edge-tts 还在连微软已经停用的旧合成地址（会 403），
这里借 edge_tts_endpoint 把地址换掉。等库自己适配了就能直接删掉这个文件。

设 EDGE_TTS_LEGACY=1 可以退回旧地址（迁移还没轮到这台机器时用）。
"""

from __future__ import annotations

import os
import runpy
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import edge_tts_endpoint  # noqa: E402


def main() -> int:
    url = edge_tts_endpoint.apply_from_env()
    host = url.split("/")[2]
    print(f"[edge-tts] 合成端点：{host}", file=sys.stderr)
    # 打完补丁再跑官方 CLI，参数、退出码、异常行为都和原来一致
    runpy.run_module("edge_tts", run_name="__main__", alter_sys=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
