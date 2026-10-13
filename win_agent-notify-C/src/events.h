// 会话 jsonl 解析：增量读取 + 事件归一化。
//    working / waiting_approval / completed / interrupted / progress / meta
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "json.h"

namespace cn {

struct Event {
    std::string kind;
    std::string thread_id;
    std::string turn_id;
    std::string text;
    std::string at;
    double timestamp = 0.0;
    std::string dedupe;
    Json detail = Json::Object();
};

// 跨记录的小状态：记住"正在等审批"的工具调用 id
struct ClassifyMemo {
    std::set<std::string> approvals;

    // 每个会话文件里"发出去了、还没有结果"的工具调用。
    // Codex 桌面版请求授权时，会话日志里**只有**这条调用、没有对应的 output，
    // 所以这是判断"等待审批"的唯一线索（代价见 README）。
    struct PendingFile {
        std::set<std::string> open;      // 还没有结果的 call_id
        std::string last_call_id;        // 最近一次调用
        std::string last_command;        // 它的命令 / 补丁路径（截断过）
        double last_record_ts = 0.0;     // 这个文件里最后一条记录的时间
    };
    std::map<std::string, PendingFile> files;
    // 已经为哪一批待定调用推过"等待审批"（避免重复推）
    std::map<std::string, std::string> pushed_approval;
};

// 会话文件名里的线程 id（重启后续读时 session_meta 已经读不到了，靠它兜底）
std::string ThreadIdFromPath(const std::string& path);

// 把一行会话记录转换成 0..n 个归一化事件。
// file_key 用来登记"发出去了、还没结果的工具调用"（判等待审批用），传会话文件路径即可。
std::vector<Event> Classify(const Json& record, const std::string& thread_id, ClassifyMemo* memo,
                            const std::string& file_key = "");

// 内部复核报文（不该出现在推送摘要里）
bool LooksInternal(const std::string& text);

// 按字节偏移增量读取一个会话 jsonl 文件
class SessionTail {
public:
    SessionTail(std::string path, long long offset);

    // 返回新读到的 JSON 记录；半个行按**字节**留到下次
    std::vector<Json> ReadNew();

    long long offset() const { return offset_; }
    void set_offset(long long value) { offset_ = value < 0 ? 0 : value; }
    // 上一轮是不是因为文件被截断/重建才从头读的
    bool was_reset() const { return was_reset_; }

private:
    std::string path_;
    long long offset_ = 0;
    bool was_reset_ = false;
    std::string buffer_;   // 上一轮留下的半行（字节）
};

// 按修改时间倒序列出会话文件
std::vector<std::string> IterSessionFiles(const std::vector<std::string>& dirs, int max_files,
                                          int max_age_days);

// session_index.jsonl -> 线程 id 到标题的映射
std::map<std::string, std::string> LoadThreadNames(const std::string& index_file);

}  // namespace cn
