// The VM step "upgrade keeps everything" (tools/test_upgrade_keeps_vm.ps1): puts a published version's profile into the
// Windows Credential Manager and the profile folder of whoever runs it, byte for byte. Built at the fixture's own
// commit (tests/fixtures/profiles/README.md), so that the credentials are written by that version's own Windows code,
// as the person's were (review 012, M3); the profile files are copied as that version left them. A test program, never
// in a package, and ONLY for a throwaway virtual machine: it refuses unless it is told --disposable, the machine's
// firmware names a hypervisor, and the profile Windows names for whoever runs it holds qkvm-disposable-vm.txt with the
// machine's name as Windows names it; and it writes only where the program keeps nothing yet. No secret is said: what
// it says is names.
//
//     qkvm-test-write-profile --disposable <fixture folder>

#include <cstdio>
#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

#include <qkvm/core/secrets.h>

#include "disposable_machine.h"
#include "profile_writing.h"

using namespace qkvm;

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList said = QCoreApplication::arguments().mid(1);
    if (said.size() != 2 || said.at(0) != QStringLiteral("--disposable")) {
        std::fprintf(stderr, "qkvm-test-write-profile: --disposable <fixture folder>\n");
        return 2;
    }
    if (const QString why = testing::whyNotDisposable(testing::profileOfWindows(), testing::machineNameOfWindows(),
                                                      testing::firmwareOfWindows());
        !why.isEmpty()) {
        std::fprintf(stderr, "qkvm-test-write-profile: this is no machine made for this test: %s\n", qPrintable(why));
        return 2;
    }
    // Where the program keeps its profile for this user: Qt's local data, under the application's internal name.
    const QString folder = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                               .filePath(QStringLiteral("QKVM Desktop"));
    // (This version's own, with its own default of what its secrets are kept under.)
    const std::unique_ptr<core::SecretStore> store = core::windowsSecrets();
    const QStringList wrong = testing::profiles::writeProfile(said.at(1), *store, folder);
    for (const QString& one : wrong) {
        std::fprintf(stderr, "qkvm-test-write-profile: %s\n", qPrintable(one));
    }
    std::printf("%s: %s into %s and Windows Credential Manager\n", wrong.isEmpty() ? "written" : "NOT WRITTEN",
                qPrintable(QDir::toNativeSeparators(said.at(1))), qPrintable(QDir::toNativeSeparators(folder)));
    return wrong.isEmpty() ? 0 : 1;
}
