// 只读读 Codex 应用自己的消息队列（queue_1.sqlite）：
// 某线程还有排队消息时，就不该报"任务已完成"。
//
// winsqlite3.dll 是 Windows 自带的 SQLite（没有导入库），所以用 LoadLibrary 动态加载。
// 任何失败一律当作"没有排队"，绝不影响主流程。
#pragma once

#include <set>
#include <string>

namespace cn {

// 返回"还有排队消息"的线程 id 集合。max_age_sec > 0 时太老的记录不算数。
std::set<std::string> PendingThreadIds(const std::string& db_path, double max_age_sec);

bool HasQueuedWork(const std::string& thread_id, const std::string& db_path, double max_age_sec);

}  // namespace cn
