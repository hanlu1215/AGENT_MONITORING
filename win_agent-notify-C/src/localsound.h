// 本机出声：状态变化时直接在跑程序这台电脑上响提示音 / 念一句。
//   · 提示音：自己合成正弦波（不是 winsound.Beep 的方波），带淡入淡出
//   · 朗读：默认 SAPI；配置成 "edge" 时可以调 .venv 里的 edge-tts（和 Ubuntu 同嗓音）
// 全程队列 + 后台线程：绝不阻塞主循环，绝不抛异常。
#pragma once

#include <string>

#include "config.h"

namespace cn {

// 响一声（+可选念一句）。立刻返回。
void LocalSoundAlert(const Config& cfg, const std::string& status, const std::string& text,
                     const std::string& agent, bool force = false);

// 退出前调用：结束播放线程
void LocalSoundShutdown();

// 合成提示音（自测用）：返回 PCM16 单声道数据
std::string SynthToneSequence(const std::string& sequence_spec, double volume, int rate);

}  // namespace cn
