#pragma once

#include <memory>

#include <QString>

#include <qkvm/base/result.h>

namespace qkvm::core {

// Whether the program starts by itself when its user signs in to Windows. Asked and changed by the
// settings box, and by nothing else: the program never puts itself on that list unasked.
class AutoStart {
public:
    virtual ~AutoStart() = default;

    // Whether Windows would start this program as things are now: it is on Windows' list with the
    // place of this program's file, and nobody has switched it off in Windows' own settings.
    virtual bool isOn() = 0;
    // On: put on the list, and what was switched off in Windows' settings is switched on again,
    // since whoever ticks the box means it. Packaged apps respect Windows user/policy disablement.
    // Off: taken off the list.
    virtual base::Result<void> set(bool on) = 0;
};

// Where Windows keeps one user's list of what starts with them, and where it notes what that user
// switched off on it ("Startup apps" in Windows' settings, and the Task Manager): both under the
// user's own part of the registry, which takes no administrator. A test names keys of its own.
struct StartKeys {
    QString list = QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    QString switchedOff =
        QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run");
};

// Windows' own list. `program`: this program's file, as Windows is to start it. `name`: what its
// entry on the list is called. A package family selects Windows-managed startup tasks instead.
std::unique_ptr<AutoStart> windowsAutoStart(const QString& program,
                                            const QString& name = QStringLiteral("QKVM Desktop"),
                                            const StartKeys& keys = {}, const QString& packageFamily = {});
QString windowsPackageFamily();

} // namespace qkvm::core
