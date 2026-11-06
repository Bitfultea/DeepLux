#pragma once

namespace DeepLux {

/**
 * @brief 模块执行亲和性（后台执行设计 §6.1）
 *
 * - Gui：模块只能在 GUI 线程执行；后台执行线程通过 BlockingQueuedConnection 回到 GUI 线程。
 * - Worker：模块可在执行线程（后台 worker / 并行池）内联执行。
 *
 * 默认 Gui（未审计插件保持 GUI 亲和），由 metadata.json 的 execution.affinity 解析。
 */
enum class ExecutionAffinity { Gui, Worker };

} // namespace DeepLux
