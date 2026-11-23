#pragma once

// What the two programs of a start into BIOS setup share (test_redfish_setup.cpp: the press itself, what is kept beside
// the settings and the way out; test_redfish_setup_queued.cpp: a change the controller only queued, its task, and the
// prune of records): the press asked and waited for, its words, the paces it is given, and the controller by hand made
// to offer it.

#include <memory>
#include <string>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QTest>

#include <qkvm/core/redfish_device.h>
#include <qkvm/core/setup_overrides.h>

#include "redfish_by_hand.h"
#include "redfish_device_tests.h"
#include "stand_in.h"

#include <qt_windows.h>

namespace qkvm::core::testing {

using namespace Qt::StringLiterals;

// Has the controller do something, and says what came of it within `withinMs` ("never" where nothing came). The answers
// are held apart from this call: a press it gave up on is still told what became of it, by its driver's end at the
// latest, on the program's loop (~RedfishDevice), after this has returned; a list on this stack would be written into
// then, wherever the next case's stands (a builder VM, 2026-11-19: one failed case made the next crash, 0xC0000005).
inline base::Result<Done> doneBy(RedfishDevice& device, const char* kind, int withinMs = 30000)
{
    const auto answers = std::make_shared<QList<base::Result<Done>>>();
    device.act(qkvm::core::Action::plain(QString::fromLatin1(kind)),
               [answers](const base::Result<Done>& answer) { answers->append(answer); });
    if (!QTest::qWaitFor([&answers] { return !answers->isEmpty(); }, withinMs)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers->takeFirst();
}

inline std::string codeOf(const base::Result<Done>& what)
{
    return what ? std::string("ok") : what.error().code;
}

inline std::string saidBy(const base::Result<Done>& what)
{
    return what ? what.value().note.toStdString() : what.error().message;
}

inline bool says(const base::Result<Done>& what, const char* part)
{
    return saidBy(what).find(part) != std::string::npos;
}

// The press's times made short: its restart within 4 s, the whole of it 6 s (two and four minutes in life); its
// clearing on the way out 4 s (thirty); and what may be left kept by the test, in memory. (1.5 s and 3 s before: on a
// loaded build box the sign-in and the reads before the restart took longer than 1.5 s in tests whose subject is
// something else, and each ended "took more than 1 seconds" now and then. A test of the restart's own time answers past
// whatever it has.)
inline RedfishDevice::Pace setupPace()
{
    RedfishDevice::Pace pace = quick();
    pace.setupWithinMs = 4000;
    pace.setupPatienceMs = 6000;
    pace.bootQuietMs = 1500;
    pace.confirmWithinMs = 600;
    pace.confirmEveryMs = 100;
    pace.setupClearWithinMs = 4000;
    pace.overrides = std::make_shared<qkvm::core::SetupOverrides>();
    return pace;
}

// ...and for a press whose task is followed: its restart within 6 s, the whole of it 9 s, so that the reads before the
// change, on a loaded PC under the address sanitizer, leave its looks their time (outside review of D1, L5).
inline RedfishDevice::Pace taskPace()
{
    RedfishDevice::Pace pace = setupPace();
    pace.setupWithinMs = 6000;
    pace.setupPatienceMs = 9000;
    return pace;
}

// What a server's card says of it, after a look.
inline QString leftOnTheCard(RedfishDevice& device)
{
    return lookedAt(device).server.value_or(qkvm::core::ServerReadings{}).setupLeft;
}

// A controller that offers a start into the BIOS setup, as AMI's boards list it.
inline void offersSetup(RedfishByHand& bmc)
{
    bmc.bootTargets = QJsonArray{u"None"_s, u"Pxe"_s, u"Hdd"_s, u"Cd"_s, u"BiosSetup"_s};
}

// The changes of its boot override, in the order they came: "Once BiosSetup", "Disabled".
inline QStringList bootChanges(const RedfishByHand& bmc)
{
    QStringList all;
    for (const QJsonObject& change : bmc.changes) {
        const QJsonObject boot = change.value(u"Boot"_s).toObject();
        if (!boot.isEmpty()) {
            all.append((boot.value(u"BootSourceOverrideEnabled"_s).toString() + u' ' +
                        boot.value(u"BootSourceOverrideTarget"_s).toString())
                           .trimmed());
        }
    }
    return all;
}

inline QByteArray bytesOf(const QString& file)
{
    QFile read(file);
    return read.open(QIODevice::ReadOnly) ? read.readAll() : QByteArray("(none)");
}

inline bool writeTo(const QString& file, const QByteArray& content)
{
    QFile made(file);
    return made.open(QIODevice::WriteOnly) && made.write(content) == content.size();
}

// From its first clearing on, its computer shows the BIOS setup override kept for every boot, whatever is changed (a
// board whose change is answered and never made).
inline void stillShownForEveryBootAfterTheClearing(RedfishByHand& bmc)
{
    bmc.whenAsked = [&bmc](const Asked& asked) {
        if (asked.method == "PATCH" && asked.body.contains("Disabled")) {
            bmc.bootSaidAs = QJsonObject{{u"BootSourceOverrideEnabled"_s, u"Continuous"_s},
                                         {u"BootSourceOverrideTarget"_s, u"BiosSetup"_s},
                                         {u"BootSourceOverrideTarget@Redfish.AllowableValues"_s, bmc.bootTargets}};
        }
    };
}

// Opens a file as a program does that lets nobody else in while it has it (one that looks for viruses, one that makes
// copies).
inline HANDLE holdAlone(const QString& file)
{
    return CreateFileW(QDir::toNativeSeparators(file).toStdWString().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace qkvm::core::testing
