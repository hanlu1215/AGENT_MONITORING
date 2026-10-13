"""把 edge-tts 的合成请求切到微软的新端点。

背景：微软把 Edge「大声朗读」的语音合成服务从旧地址迁到了新地址，
并按地区逐步停用旧地址 —— 旧地址的合成 WebSocket 开始直接返回 HTTP 403
（`speech.platform.bing.com` 目前只剩语音列表这类非核心查询还能用）。

    旧  wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1
        ?TrustedClientToken=6A5AA1D4EAFF4E9FB37E23D68491D6F4
    新  wss://api.msedgeservices.com/tts/cognitiveservices/websocket/v1
        ?Ocp-Apim-Subscription-Key=6A5AA1D4EAFF4E9FB37E23D68491D6F4

新端点认的还是**同一个内置 token**，只是参数名从 TrustedClientToken 变成了
Ocp-Apim-Subscription-Key；`Sec-MS-GEC` / `Sec-MS-GEC-Version` 照旧，库自己会带。

PyPI 上的 edge-tts（截至 7.2.8）还在用旧地址，所以这里在运行时把它的
模块级 `WSS_URL` 换掉。`Communicate()` 是在**调用时**从模块全局取这个名字的，
所以打进 `edge_tts.communicate` 就能生效，不用改库、不用打补丁包。

等 edge-tts 自己适配了新地址之后，这个模块就可以删掉了。
"""

from __future__ import annotations

import os

from edge_tts.constants import TRUSTED_CLIENT_TOKEN

NEW_WSS_URL = (
    "wss://api.msedgeservices.com/tts/cognitiveservices/websocket/v1"
    f"?Ocp-Apim-Subscription-Key={TRUSTED_CLIENT_TOKEN}"
)
LEGACY_WSS_URL = (
    "wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1"
    f"?TrustedClientToken={TRUSTED_CLIENT_TOKEN}"
)


def apply(legacy: bool = False) -> str:
    """切换合成端点，返回当前生效的 URL。

    legacy=True 用旧地址（迁移还没轮到这台机器时用得上）。
    """
    import edge_tts.communicate as communicate
    import edge_tts.constants as constants

    url = LEGACY_WSS_URL if legacy else NEW_WSS_URL
    # 两边都改：Communicate() 读的是 communicate 里的名字，
    # 别的调用方（以及将来的版本）可能读 constants 里的。
    communicate.WSS_URL = url
    constants.WSS_URL = url
    return url


def apply_from_env() -> str:
    """按环境变量 EDGE_TTS_LEGACY=1 决定用哪个端点。"""
    return apply(legacy=os.environ.get("EDGE_TTS_LEGACY") == "1")


def current() -> str:
    import edge_tts.communicate as communicate

    return communicate.WSS_URL
