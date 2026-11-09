// 动作计划播放器 —— 2026-11-09 晚。
//
// 外面推一份 JSON 时间线进来（{"plan":{...}} 或 {"trajectory":{...}}），
// 身体照着演：每帧按 t = now - startTime 找到所处的前后两帧，线性插值，
// 把白名单里的参数 Set 进模型。播完（t > 总时长）自动停止，交回待机。
//
// 这个头**不包含 Cubism 类型**（和 ctl 一个规矩）：解析、排序、插值、时钟都在
// plan.cpp 里；main.cpp 拿 plan::sample() 给的「名字 + 数值」自己去 Set。
//
// 参数白名单（名字就是模型里真实的参数 id，**不是** ParamAngleX 那种空壳名）：
//   AngleX AngleY AngleZ PositionX PositionY PositionZ
//   ParamEyeBallX ParamEyeBallY ParamBodyAngleZ ParamMouthOpenY LeanInput
// 名单外的一律跳过，同一个名字只警告一次。

#pragma once

#include <string>

namespace plan {

constexpr int kMaxParams = 11;

// 白名单，顺序固定（main 按这个顺序建索引缓存）。
extern const char* const kNames[kMaxParams];

// 某个名字在白名单里的槽位；不在名单里返回 -1。
int slotOf(const char* name);

// 一帧的采样结果。name 指向 kNames 里的常量字符串，跨帧稳定，可以直接拿去比较。
struct Sample {
    int n = 0;
    const char* name[kMaxParams] = {nullptr};
    float value[kMaxParams] = {0.0f};
};

// 收到一份计划：解析成功就从头开始播；解析失败只警告，**不打断**正在播的那一个。
// json 是计划体原文（`{...}`），可以带外层 "plan"/"trajectory" 包装，也认裸的。
void start(const std::string& json, double now);

// 停止（{"plan":null} 或播完）。
void stop();

bool active();

// 起播时刻（main 拿它算交叉淡入的进度）。
double startTime();

// 总时长（秒）。
double total();

// 当前这份计划的标题（日志用），没有就是 ""。
const char* title();

// 计划里可选的表情名（`"expr":"＞＜"`）。没有就是 ""。
// 起播时由 main 触发一次切换，这样「被戳一下」能同时换脸。
const char* expr();

// 这份计划有没有在驱动某个参数（嘴要判断：计划驱动嘴时，说话仍然优先）。
bool drives(const char* name);

// 每帧取一次。返回 false 表示没有计划 / 已经播完（此时已自动 stop）。
bool sample(double now, Sample& out);

}  // namespace plan
