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

// 有新的动作计划就取出来（取走后清空）。返回 true 表示有新请求：
//   · out = 计划体原文（从 `{` 到配对的 `}`，含外层 "plan"/"trajectory" 包装）
//   · out = ""  → 停止当前计划（客户端推的 `"plan":null`）
// 这里只做「找出这段对象文本」这一件事，真正的解析在 plan.cpp（它有正经的 JSON 扫描器）。
bool takePlan(std::string& out);

// 最近一次 mood（0..100），没设过是 -1。
int mood();

// 第十六节：网页设置页的两项（main 每帧喂一次，改了就下一句生效）。
//   setMouthScale：说话时嘴动时长倍率（0.5–3，超范围就地夹紧）
//   setVoice：说话时要不要出声 + 音量 0–1（播预生成 wav，没有就静默）
void setMouthScale(float s);

// 二十六：让身体自己说一句（点击反应用）。和外部推 {"say":...} 走同一套：
// 嘴型包络 + 有缓存就出声。now 由调用方给（main 的帧时间）。
void say(const std::string& text, double now);
void voiceWarmUp();   // 启动时预热输出流（第一声会被 pipewire 吞掉）
void setVoice(bool on, float vol);

// socket 路径，日志用。
const char* sockPath();

}  // namespace ctl
