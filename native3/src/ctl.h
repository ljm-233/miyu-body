// 本地控制口：Unix domain socket，一行一条 JSON。
// 2026-11-09 晚加的。外面（DSH 插件 / 脚本）往这儿推「她在说什么、什么心情」，
// 身体这边收到就动嘴、切表情。
//
// 这个头只暴露「状态查询 + 每帧一次」的极薄接口，不把 Cubism 类型漏出来 ——
// 参数怎么写、表情怎么 Create 都留在 main.cpp 里，ctl.cpp 只管 socket 和解析。

#pragma once

#include <string>

namespace ctl {

// 在最后建 socket；失败只在日志里说一句，绝不影响渲染。
// 路径：$XDG_RUNTIME_DIR/miyu-body.sock，拿不到就 /tmp/miyu-body-$UID.sock
void init();

// 每帧调一次：非阻塞 poll，最多接一个连接，收完一行就 close。
void poll(double now);

// 当前该给 ParamMouthOpenY 写多少（0..1）；返回 < 0 表示「没在说话」。
float mouth(double now);

// 有新的表情请求就取出来（取走后清空）。返回 true 表示 out 有效。
// out 为 "" 或 "默认" → 淡出回默认表情。
bool takeExpr(std::string& out);

// 最近一次 mood（0..100），没设过是 -1。
int mood();

// socket 路径，日志用。
const char* sockPath();

}  // namespace ctl
