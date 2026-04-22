#include "CancellationToken.h"

namespace DeepLux {

CancellationToken::CancellationToken(QObject* parent)
    : QObject(parent)
{
}

void CancellationToken::cancel()
{
    bool expected = false;
    if (m_cancelled.compare_exchange_strong(expected, true)) {
        emit cancelled();
    }
}

bool CancellationToken::isCancelled() const
{
    return m_cancelled.load(std::memory_order_acquire);
}

void CancellationToken::reset()
{
    m_cancelled.store(false, std::memory_order_release);
}

} // namespace DeepLux
