// The VM step "upgrade keeps everything" (tools/test_upgrade_keeps_vm.ps1): what an upgrade made of a published
// version's profile, which that version's own writer (qkvm-test-write-profile, built at its commit) put into the
// Windows Credential Manager and the profile folder of whoever runs this, compared file by file and credential by
// credential, with this build's own Windows code reading them, as the upgraded program does. A test program, never in a
// package, and ONLY for a throwaway virtual machine: it refuses unless it is told --disposable, the machine's firmware
// names a hypervisor, and the profile Windows names for whoever runs it holds qkvm-disposable-vm.txt with the
// machine's name as Windows names it. It writes
// nothing but its report, and says no secret: names, and "same", "changed" or "missing".
//
//     qkvm-test-check-profile --disposable <fixture folder> <report.json>

#include <cstdio>
#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>

#include <qkvm/core/secrets.h>

#include "disposable_machine.h"
#include "profile_loading.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

int refuse(const QString& why)
{
    std::fprintf(stderr, "qkvm-test-check-profile: %s\n", qPrintable(why));
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList said = QCoreApplication::arguments().mid(1);
    if (said.size() != 3 || said.at(0) != u"--disposable"_s) {
        return refuse(u"--disposable <fixture folder> <report.json>"_s);
    }
    if (const QString why = testing::whyNotDisposable(testing::profileOfWindows(), testing::machineNameOfWindows(),
                                                      testing::firmwareOfWindows());
        !why.isEmpty()) {
        return refuse(u"this is no machine made for this test: "_s + why);
    }
    const testing::profiles::Fixture fixture(said.at(1));
    if (!fixture.whole()) {
        return refuse(u"no whole fixture at "_s + said.at(1));
    }
    // Where the program keeps its profile for this user (core::Settings::usualFile: Qt's local data, "QKVM Desktop").
    const QString folder =
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(u"QKVM Desktop"_s);
    const std::unique_ptr<core::SecretStore> store = core::windowsSecrets();
    const QJsonObject report = testing::profiles::check(fixture, *store, folder, QStringLiteral(QKVM_DESKTOP_VERSION));
    QFile out(said.at(2));
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(report).toJson()) < 0) {
        return refuse(u"the report could not be written"_s);
    }
    const bool passed = report["passed"_L1].toBool();
    std::printf("%s\n", passed ? "passed" : "FAILED");
    for (const QJsonValue one : report["failures"_L1].toArray()) {
        std::printf("  %s\n", qPrintable(one.toString()));
    }
    return passed ? 0 : 1;
}
