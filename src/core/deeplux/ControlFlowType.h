#pragma once

namespace DeepLux {

/**
 * @brief 流程控制类型
 *
 * 每个模块根据自身语义声明控制流角色（而非名称约定）。
 * RunEngine 据此决定执行顺序，消除字符串匹配隐式约定。
 */
enum class ControlFlowType {
    Sequential,       // 顺序执行，执行后进入下一个模块
    Conditional,      // 条件分支入口（如 If, Condition）
    ConditionalElse,  // 条件分支 else/else-if
    ConditionalEnd,   // 条件分支结束
    Loop,             // 固定循环入口
    LoopEnd,          // 固定循环出口
    StopLoop,         // 提前终止循环
    While,            // 条件循环入口
    WhileEnd          // 条件循环出口
};

} // namespace DeepLux
