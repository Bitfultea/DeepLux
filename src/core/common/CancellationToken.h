#pragma once

#include <QObject>
#include <QMutex>
#include <atomic>

namespace DeepLux {

/**
 * @brief Cancellation token for requesting cancellation of long-running operations.
 *
 * Multiple consumers can check isCancelled() and react to cancellation.
 * Multiple producers can call cancel() to request cancellation.
 */
class CancellationToken : public QObject
{
    Q_OBJECT

public:
    explicit CancellationToken(QObject* parent = nullptr);

    /// Request cancellation
    Q_INVOKABLE void cancel();

    /// Check if cancellation has been requested
    Q_INVOKABLE bool isCancelled() const;

    /// Reset the token for reuse (e.g. at start of a new operation)
    Q_INVOKABLE void reset();

    /// Atomic cancelled flag accessible without locking
    bool isCancelledFast() const { return m_cancelled.load(std::memory_order_acquire); }

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
class ScopedCancellation
{
public:
    explicit ScopedCancellation(CancellationToken* token) : m_token(token) {}
    ~ScopedCancellation() {
        if (m_token) m_token->cancel();
    }
    CancellationToken* m_token;
};

} // namespace DeepLux
