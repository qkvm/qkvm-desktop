#include "keys_kept.h"

namespace qkvm::ui {

KeysKept::KeysKept(int forgetAfterMs, QObject* parent) : QObject(parent)
{
    m_forget.setSingleShot(true);
    m_forget.setInterval(forgetAfterMs);
    connect(&m_forget, &QTimer::timeout, this, [this] {
        // An "up" that has not come by now is not waited for any longer.
        if (!m_down.empty()) {
            m_down.clear();
            emit changed();
        }
    });
}

void KeysKept::keep(std::uint16_t usage)
{
    if (usage != 0 && m_down.insert(usage).second) {
        forgetLater();
        emit changed();
    }
}

void KeysKept::letGo(std::uint16_t usage)
{
    if (m_down.erase(usage) > 0) {
        if (m_down.empty()) {
            m_forget.stop();
        }
        emit changed();
    }
}

void KeysKept::controlBegan()
{
    ++m_controlling;
    m_forget.stop();
}

void KeysKept::controlEnded()
{
    if (m_controlling > 0) {
        --m_controlling;
    }
    forgetLater();
}

void KeysKept::forgetLater()
{
    // (From when the last window gave them back, and not anew with every key that is let go.)
    if (m_controlling == 0 && !m_down.empty() && !m_forget.isActive()) {
        m_forget.start();
    }
}

} // namespace qkvm::ui
