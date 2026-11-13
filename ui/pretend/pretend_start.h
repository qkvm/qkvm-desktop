#pragma once

#include <QList>

#include <qkvm/core/auto_start.h>

namespace qkvm::ui {

// Windows' list of what starts with a user, as far as the settings box deals with it: on or off,
// and a Windows that does not take a change. For the tests and the rigs, which never touch the
// list of whoever runs them; the program itself never uses it.
class PretendedStart final : public core::AutoStart {
public:
    bool isOn() override { return on; }
    base::Result<void> set(bool wanted) override
    {
        asked.append(wanted);
        if (refuses) {
            return base::Error{"auto_start", "Windows did not take it (error 5)."};
        }
        on = wanted;
        return {};
    }

    bool on = false;
    bool refuses = false;
    // Every change that was asked for, in order.
    QList<bool> asked;
};

} // namespace qkvm::ui
