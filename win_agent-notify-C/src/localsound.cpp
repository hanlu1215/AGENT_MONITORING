#define INITGUID   // CLSID_SpVoice / IID_ISpVoice 在 MinGW 下需要它来产出定义
#include "localsound.h"

#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "util/crypto.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/pinyin.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

constexpr int kSampleRate = 44100;

struct ToneStep {
    int freq;
    int ms;
};

using ToneSequence = std::vector<ToneStep>;

const std::map<std::string, ToneSequence>& DefaultTones() {
    static const std::map<std::string, ToneSequence> kTones = {
        {"working", {{880, 90}, {0, 70}}},
        {"waiting_approval", {{2700, 90}, {0, 80}, {2700, 90}, {0, 80}, {2700, 120}, {0, 100}}},
        {"completed", {{1500, 120}, {0, 60}, {2700, 200}, {0, 80}}},
        {"interrupted", {{900, 150}, {0, 90}, {700, 260}, {0, 80}}},
        {"idle", {}},
        {"info", {{2000, 100}, {0, 80}}},
        {"test", {{880, 90}, {0, 50}, {1100, 90}}},
    };
    return kTones;
}

const std::map<std::string, std::string>& DefaultPhrases() {
    static const std::map<std::string, std::string> kPhrases = {
        {"working", "{agent} 开始工作"},
        {"waiting_approval", "{agent} 需要审批"},
        {"completed", "{agent} 任务已完成"},
        {"interrupted", "{agent} 任务中断"},
    };
    return kPhrases;
}

// 在 PATH 里找一个可执行文件
std::string WhichOnPath(const std::string& name) {
    wchar_t buffer[32768];
    DWORD len = SearchPathW(nullptr, Utf8ToWide(name).c_str(), nullptr, 32768, buffer, nullptr);
    if (len == 0 || len >= 32768) return std::string();
    return WideToUtf8(std::wstring(buffer, len));
}

std::string AgentLabel(const std::string& agent) {
    std::string key = ToLowerAscii(Trim(agent));
    if (key == "claude") return "Claude Code";
    if (key == "codex") return "Codex";
    return Trim(agent).empty() ? "Codex" : Trim(agent);
}

ToneSequence SequenceFromConfig(const Config& cfg, const std::string& status) {
    const Json& custom = cfg.get("local_sound").get("tones");
    const Json& list = custom.is_object() ? custom.get(status) : Json();
    ToneSequence out;
    if (list.is_array() && !list.empty()) {
        for (size_t i = 0; i < list.size(); ++i) {
            const Json& step = list.at(i);
            if (!step.is_array() || step.size() < 2) continue;
            out.push_back({int(step.at(0).as_int(0)), int(step.at(1).as_int(0))});
        }
        return out;
    }
    auto found = DefaultTones().find(status);
    return found == DefaultTones().end() ? ToneSequence() : found->second;
}

// ---- 提示音合成 ----

std::string SynthTones(const ToneSequence& sequence, double volume, int rate) {
    std::string pcm;
    for (const ToneStep& step : sequence) {
        int count = int(double(rate) * (step.ms > 0 ? step.ms : 0) / 1000.0);
        if (count <= 0) continue;
        if (step.freq <= 0) {
            pcm.append(size_t(count) * 2, '\0');   // 停顿
            continue;
        }
        int fade_in = (std::max)(1, int(rate * 0.006));
        int fade_out = (std::max)(1, int(rate * 0.030));
        for (int i = 0; i < count; ++i) {
            double env = 1.0;
            env = (std::min)(env, double(i) / fade_in);
            env = (std::min)(env, double(count - i) / fade_out);
            int value = int(32767.0 * volume * env *
                            std::sin(2.0 * 3.14159265358979323846 * step.freq * i / rate));
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            pcm.push_back(char(value & 0xFF));
            pcm.push_back(char((value >> 8) & 0xFF));
        }
    }
    return pcm;
}

// 播放 PCM16 单声道（同步，播完才返回）
bool PlayPcm(const std::string& pcm, int rate) {
    if (pcm.empty()) return false;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = DWORD(rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = WORD(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!done) return false;
    HWAVEOUT device = nullptr;
    MMRESULT opened = waveOutOpen(&device, WAVE_MAPPER, &format, (DWORD_PTR)done, 0,
                                  CALLBACK_EVENT);
    if (opened != MMSYSERR_NOERROR) {
        CloseHandle(done);
        LogDebug("local", Cat("没有可用声卡（waveOutOpen=", int(opened), "），跳过提示音"));
        return false;
    }

    std::string buffer = pcm;   // 播放期间必须保持有效
    WAVEHDR header{};
    header.lpData = buffer.data();
    header.dwBufferLength = DWORD(buffer.size());
    bool ok = false;
    if (waveOutPrepareHeader(device, &header, sizeof(header)) == MMSYSERR_NOERROR) {
        if (waveOutWrite(device, &header, sizeof(header)) == MMSYSERR_NOERROR) {
            DWORD expect_ms = DWORD(double(buffer.size()) / (rate * 2) * 1000.0);
            ULONGLONG started = GetTickCount64();
            // 不能只等事件：事件可能被提前触发（有些驱动在 open 时就信号一次），
            // 那样我们会以为播完了、提前关闭设备 —— 声音被拦腰截断，
            // 紧接着的朗读就叠在残留的提示音上，听起来就是"开头一段电流音"。
            // 所以以 WAVEHDR 的 WHDR_DONE 为准，事件只当"醒来看看"用。
            ULONGLONG deadline = started + expect_ms + 5000;
            while (!(header.dwFlags & WHDR_DONE) && GetTickCount64() < deadline) {
                WaitForSingleObject(done, 20);
            }
            ok = (header.dwFlags & WHDR_DONE) != 0;
            ULONGLONG elapsed = GetTickCount64() - started;
            LogDebug("local", Cat("提示音播放 ", (long long)elapsed, "ms（期望 ",
                                  (long long)expect_ms, "ms）"));
            if (ok && elapsed + 50 < expect_ms) {
                LogWarn("local", Cat("提示音提前结束（期望 ", (long long)expect_ms, "ms，实际 ",
                                     (long long)elapsed, "ms）"));
            }
        }
        if (!(header.dwFlags & WHDR_DONE)) {
            // 还没播完就叫停，设备会截断 —— 宁可多等一会
            LogDebug("local", "提示音没播完就收尾了");
        }
        waveOutUnprepareHeader(device, &header, sizeof(header));
    }
    waveOutClose(device);
    CloseHandle(done);
    return ok;
}

// ---- 朗读 ----

// 用 MCI 放一个音频文件（mp3 也能放），放完才返回
bool PlayFileMci(const std::string& path, int timeout_sec) {
    std::wstring alias = L"cn_tts_" + std::to_wstring(GetCurrentThreadId());
    std::wstring open = L"open \"" + Utf8ToWide(path) + L"\" alias " + alias;
    if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) != 0) return false;
    std::wstring play = L"play " + alias + L" wait";
    (void)timeout_sec;
    MCIERROR played = mciSendStringW(play.c_str(), nullptr, 0, nullptr);
    mciSendStringW((L"close " + alias).c_str(), nullptr, 0, nullptr);
    return played == 0;
}

// 最后一次兜底：Windows 自带 SAPI。本机没装中文语音时它不会出声。
bool SpeakSapi(const std::string& text, int rate) {
    static thread_local bool com_ready = false;
    static thread_local ISpVoice* voice = nullptr;
    if (!com_ready) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // RPC_E_CHANGED_MODE 说明这个线程已经初始化过，也能用
        com_ready = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        if (com_ready) {
            HRESULT created = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                               IID_ISpVoice, (void**)&voice);
            if (FAILED(created)) voice = nullptr;
        }
    }
    if (!voice) return false;
    voice->SetRate(LONG(rate));
    std::wstring wide = Utf8ToWide(text);
    return SUCCEEDED(voice->Speak(wide.c_str(), SPF_DEFAULT, nullptr));
}

// 这个 python 里装没装 edge_tts？（每个候选只试一次）
bool HasEdgeTts(const std::string& python) {
    std::wstring command = L"\"" + Utf8ToWide(python) + L"\" -c \"import edge_tts\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return false;
    }
    DWORD waited = WaitForSingleObject(process.hProcess, 8000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
    } else {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return waited == WAIT_OBJECT_0 && code == 0;
}

std::string FindEdgePython(const std::string& exe_dir) {
    static std::string cached;
    static bool searched = false;
    if (searched) return cached;
    searched = true;

    // 1) 程序旁边的 .venv（README 推荐的做法，也是唯一不需要装 Python 的做法）
    const char* relatives[] = {"\\.venv\\Scripts\\python.exe", "\\.venv\\bin\\python.exe",
                               "\\.venv\\Scripts\\pythonw.exe"};
    for (const char* relative : relatives) {
        std::string candidate = exe_dir + relative;
        if (FileExists(candidate)) {
            cached = candidate;
            return cached;
        }
    }
    // 2) PATH 里的 python —— 拷到别的电脑时，系统里可能已经装了 edge_tts
    for (const char* name : {"python.exe", "pythonw.exe"}) {
        std::string found = WhichOnPath(name);
        if (!found.empty() && HasEdgeTts(found)) {
            LogInfo("local", Cat("用系统里的 Python 做朗读：", found));
            cached = found;
            return cached;
        }
    }
    LogInfo("local", "没找到带 edge_tts 的 Python（程序旁边没有 .venv），"
                     "朗读将退回系统语音，音质会差一些");
    return cached;
}

bool SynthEdge(const std::string& exe_dir, const std::string& text, const std::string& dest,
               const std::string& voice, const std::string& rate, const std::string& volume,
               int timeout_sec) {
    std::string python = FindEdgePython(exe_dir);
    if (python.empty()) return false;
    std::string part = dest + ".part.mp3";
    RemoveFile(part);

    // 微软把合成服务搬到了新地址 api.msedgeservices.com，旧的 speech.platform.bing.com
    // 对合成请求直接返回 403（按地区逐步停用）。PyPI 上的 edge-tts 还在连旧地址，
    // 所以优先走 tools/edge_tts_cli.py —— 它先把地址换掉，再转调官方命令行；
    // 这个脚本不在时（比如只拷了 exe）退回原来的 `-m edge_tts`。
    const std::string shim = PathJoin(exe_dir, "tools/edge_tts_cli.py");
    const std::wstring entry = FileExists(shim)
        ? L"\"" + Utf8ToWide(shim) + L"\""
        : L"-m edge_tts";

    std::wstring command = L"\"" + Utf8ToWide(python) + L"\" " + entry + L" --voice " +
                           Utf8ToWide(voice) + L" --rate=" + Utf8ToWide(rate) +
                           L" --volume=" + Utf8ToWide(volume) + L" --text " +
                           Utf8ToWide("\"" + text + "\"") + L" --write-media \"" +
                           Utf8ToWide(part) + L"\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return false;
    }
    DWORD waited = WaitForSingleObject(process.hProcess, DWORD(timeout_sec) * 1000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        RemoveFile(part);
        return false;
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    if (FileSize(part) > 0) {
        MoveFileExW(Utf8ToWide(part).c_str(), Utf8ToWide(dest).c_str(),
                    MOVEFILE_REPLACE_EXISTING);
        return true;
    }
    RemoveFile(part);
    return false;
}

// 同一种原因只记一条日志，免得每次出声都刷屏
void WarnOnce(const char* tag, const char* message) {
    static std::set<std::string> seen;
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    if (seen.insert(tag).second) LogWarn("local", message);
}

// ---- 播放器 ----

struct Job {
    std::string status;
    std::string agent;
    bool tone = false;
    bool speech = false;
    ToneSequence sequence;
    double volume = 0.32;
    std::string phrase;
    std::string backend;
    std::string voice;
    std::string voice_rate;
    std::string voice_volume;
    std::string cache_dir;
    int speech_rate = 1;
};

class SoundEngine {
public:
    void Enqueue(Job job) {
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (queue_.size() >= 8) queue_.pop_front();   // 丢掉最老的一条，保住最新的
            queue_.push_back(std::move(job));
        }
        EnsureWorker();
        cv_.notify_all();
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> guard(lock_);
            stopping_ = true;
        }
        cv_.notify_all();
        std::thread worker;
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (worker_.joinable()) worker = std::move(worker_);
        }
        if (worker.joinable()) worker.join();
    }

private:
    void EnsureWorker() {
        std::lock_guard<std::mutex> guard(lock_);
        if (worker_.joinable()) return;
        worker_ = std::thread([this] { Loop(); });
    }

    void Loop() {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> guard(lock_);
                cv_.wait_for(guard, std::chrono::seconds(30),
                             [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                if (queue_.empty()) {
                    worker_.detach();      // 闲太久就退出，下次再拉起
                    worker_ = std::thread();
                    return;
                }
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                Play(job);
            } catch (...) {
                LogDebug("local", "本机提示音播放失败");
            }
            if (Stopping()) return;
        }
    }

    bool Stopping() {
        std::lock_guard<std::mutex> guard(lock_);
        return stopping_;
    }

    void Play(const Job& job) {
        if (job.tone) {
            std::string pcm = SynthTones(job.sequence, job.volume, kSampleRate);
            PlayPcm(pcm, kSampleRate);
        }
        if (job.speech && !job.phrase.empty()) {
            if (job.backend == "edge") {
                // 文件名 = 播报内容的拼音 + 一小段指纹。
                //   拼音段：一眼能看出这句话是什么（"tts-kaishi-gongzuo-…"）
                //   指纹段：嗓音/语速/音量或文案一变，就是另一个文件，不会播错
                std::string slug = PinyinSlug(job.phrase, 48);
                if (slug.empty()) slug = "voice";
                std::string fingerprint = Sha1Hex(job.voice + "|" + job.voice_rate + "|" +
                                                  job.voice_volume + "|" + job.phrase);
                std::string mp3 = PathJoin(
                    job.cache_dir,
                    Cat("tts-", slug, "-", fingerprint.substr(0, 6), ".mp3"));
                bool ready = FileExists(mp3) && FileSize(mp3) > 0;
                if (!ready) {
                    EnsureDir(job.cache_dir);
                    ready = SynthEdge(ExeDir(), job.phrase, mp3, job.voice, job.voice_rate,
                                      job.voice_volume, 60);
                    if (!ready) {
                        WarnOnce("synth", "edge-tts 合成失败（多半是没网或没装），"
                                          "改用系统语音朗读");
                    }
                }
                if (ready) {
                    if (PlayFileMci(mp3, 60)) return;
                    WarnOnce("play", "音频播放失败，改用系统语音朗读");
                }
            }
            SpeakSapi(job.phrase, job.speech_rate);
        }
    }

    std::mutex lock_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::thread worker_;
    bool stopping_ = false;
};

SoundEngine& Engine() {
    static SoundEngine engine;
    return engine;
}

std::mutex g_last_lock;
std::map<std::string, double> g_last_at;

}  // namespace

void LocalSoundAlert(const Config& cfg, const std::string& status, const std::string& text,
                     const std::string& agent, bool force) {
    (void)text;
    try {
        const Json& section = cfg.get("local_sound");
        if (!section.is_object()) return;
        if (!section.get_bool("enabled", true) && !force) return;

        std::string normalized = ToLowerAscii(Trim(status.empty() ? "info" : status));
        double gap = section.get_double("min_interval_sec", 0.0);
        double now = NowUnix();
        {
            std::lock_guard<std::mutex> guard(g_last_lock);
            if (!force && gap > 0 && now - g_last_at[normalized] < gap) return;
            g_last_at[normalized] = now;
        }

        Job job;
        job.status = normalized;
        job.agent = agent;
        job.tone = section.get_bool("tone", true);
        job.speech = section.get_bool("speech", false);
        job.volume = section.get_double("tone_volume", 0.32);
        job.backend = ToLowerAscii(section.get_str("speech_backend", "edge"));
        job.voice = section.get_str("voice", "zh-CN-XiaoxiaoNeural");
        job.voice_rate = section.get_str("voice_rate", "+0%");
        job.voice_volume = section.get_str("voice_volume", "+0%");
        job.speech_rate = int(section.get_int("speech_rate", 1));
        job.cache_dir = ResolvePath(section.get_str("cache_dir", "audio_cache"));

        if (job.tone) job.sequence = SequenceFromConfig(cfg, normalized);
        if (job.speech) {
            const Json& custom = section.get("phrases");
            const Json& value = custom.is_object() ? custom.get(normalized) : Json();
            std::string phrase = value.is_string() ? value.as_string() : std::string();
            if (phrase.empty()) {
                auto found = DefaultPhrases().find(normalized);
                if (found != DefaultPhrases().end()) phrase = found->second;
            }
            job.phrase = Trim(ReplaceAll(phrase, "{agent}", AgentLabel(agent)));
        }
        if (!job.tone && !job.speech) return;
        Engine().Enqueue(std::move(job));
    } catch (...) {
        // 出声失败绝不能影响推送主流程
    }
}

void LocalSoundShutdown() { Engine().Shutdown(); }

std::string SynthToneSequence(const std::string& sequence_spec, double volume, int rate) {
    ToneSequence sequence;
    // spec 形如 "880:90,0:70"
    for (const std::string& item : Split(sequence_spec, ',')) {
        std::string text = Trim(item);
        if (text.empty()) continue;
        size_t colon = text.find(':');
        if (colon == std::string::npos) continue;
        try {
            sequence.push_back({std::stoi(text.substr(0, colon)),
                                std::stoi(text.substr(colon + 1))});
        } catch (...) {
        }
    }
    return SynthTones(sequence, volume, rate);
}

}  // namespace cn
