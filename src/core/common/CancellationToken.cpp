#include "CancellationToken.h"

#include "core/base/ModuleBase.h"

namespace DeepLux {

CancellationToken::CancellationToken(QObject* parent) : QObject(parent) {}

CancellationToken::~CancellationToken() {
    // T2.8：模块的 token 表存的是裸指针，本对象消失前必须把自己摘干净。
    // 不加这一步时，token 先于模块销毁就会在表里留下悬垂指针，
    // 下次 isCancellationRequested() 即解引用野指针。
    ModuleBase::detachCancellationToken(this);
}

void CancellationToken::cancel() {
    bool expected = false;
    if (m_cancelled.compare_exchange_strong(expected, true)) {
        emit cancelled();
    }
}

bool CancellationToken::isCancelled() const {
    return m_cancelled.load(std::memory_order_acquire);
}

void CancellationToken::reset() {
    m_cancelled.store(false, std::memory_order_release);
}

} // namespace DeepLux
