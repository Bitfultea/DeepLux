#pragma once

#include "../platform/Platform.h"

#include <QMutex>
#include <QObject>
#include <atomic>

namespace DeepLux {

/**
 * @brief Cancellation token for requesting cancellation of long-running operations.
 *
 * Multiple consumers can check isCancelled() and react to cancellation.
 * Multiple producers can call cancel() to request cancellation.
 */
class DEEPLUX_API CancellationToken : public QObject {
    Q_OBJECT

public:
    explicit CancellationToken(QObject* parent = nullptr);

    /// T2.8（2026-11-10 评审裁定）：析构时把本 token 从所有模块的借用表中摘除。
    ///
    /// 兜底防线，与 ModuleBase::execute() 的作用域守卫互补：守卫负责「本次执行结束」，
    /// 析构负责「token 先于模块消失」。两道合起来保证表里永不留下悬垂指针，即使调用方
    /// 传入的是栈上或短生命周期 token，也不会让后续 isCancellationRequested() 解引用野指针。
    ~CancellationToken() override;

    /// Request cancellation
    Q_INVOKABLE void cancel();

    /// Check if cancellation has been requested
    Q_INVOKABLE bool isCancelled() const;

    /// Reset the token for reuse (e.g. at start of a new operation)
    Q_INVOKABLE void reset();

    /// Atomic cancelled flag accessible without locking
    bool isCancelledFast() const {
        return m_cancelled.load(std::memory_order_acquire);
    }

signals:
    void cancelled();

private:
    std::atomic<bool> m_cancelled{false};
    mutable QMutex m_mutex;
};

/**
 * @brief RAII guard that cancels a token on destruction.
 *
 * Usage: place at function entry to ensure cancellation on early return.
 */
class ScopedCancellation {
public:
    explicit ScopedCancellation(CancellationToken* token) : m_token(token) {}
    ~ScopedCancellation() {
        if (m_token)
            m_token->cancel();
    }
    CancellationToken* m_token;
};

} // namespace DeepLux
