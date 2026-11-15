// What a version's own Windows code makes of each secret of a profile (review 012, M3): built at that version's commit
// (tests/fixtures/profiles/README.md), it keeps each secret of a fixture's secrets.json through that version's own
// windowsSecrets(), its core/src/windows_secrets.cpp compiled against a Credential Manager in memory, and writes down
// the very records it made (type, persistence, flags, target name, user name, blob) and what it asked of the store to
// write, list, read and erase them: every filter, type and flag of those calls. Nothing of it reaches Windows' own
// store. Tests only, never in a package.
//
//     qkvm-test-credential-shape <fixture folder> <commit>      (writes <fixture folder>/credentials.json)

#include "windows_secrets_in_memory.h"

#include <cstdio>
#include <cstdlib>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace qkvm;

namespace {

[[noreturn]] void stop(const QString& why)
{
    std::fprintf(stderr, "qkvm-test-credential-shape: %s\n", qPrintable(why));
    std::exit(2);
}

QJsonArray numbers(const std::vector<DWORD>& all)
{
    QJsonArray said;
    for (const DWORD one : all) {
        said.append(static_cast<double>(one));
    }
    return said;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (argc != 3) {
        stop(QStringLiteral("give a fixture folder and the commit this was built at"));
    }
    const QDir fixture(QString::fromLocal8Bit(argv[1]));
    QFile secretsFile(fixture.filePath(QStringLiteral("secrets.json")));
    if (!secretsFile.open(QIODevice::ReadOnly)) {
        stop(QStringLiteral("no secrets.json in that folder"));
    }
    const QJsonObject secrets = QJsonDocument::fromJson(secretsFile.readAll()).object();
    if (secrets.isEmpty()) {
        stop(QStringLiteral("secrets.json holds nothing"));
    }
    testing::credentials::clear();
    // (This version's own, with its own default of what its secrets are kept under.)
    const std::unique_ptr<core::SecretStore> store = core::capturedWindowsSecrets();
    QJsonObject made;
    for (auto one = secrets.begin(); one != secrets.end(); ++one) {
        const QByteArray bytes =
            QByteArray::fromBase64(one.value().toObject()[QStringLiteral("bytes")].toString().toLatin1());
        const std::size_t before = testing::credentials::kept().size();
        if (!store->keep(one.key(), bytes) || testing::credentials::kept().size() != before + 1) {
            stop(QStringLiteral("%1 was not kept as one record").arg(one.key()));
        }
        const testing::credentials::Record& record = testing::credentials::kept().back();
        made.insert(one.key(), QJsonObject{{QStringLiteral("target"), QString::fromStdWString(record.target)},
                                           {QStringLiteral("type"), static_cast<double>(record.type)},
                                           {QStringLiteral("persist"), static_cast<double>(record.persist)},
                                           {QStringLiteral("flags"), static_cast<double>(record.flags)},
                                           {QStringLiteral("user_name"), QString::fromStdWString(record.userName)},
                                           {QStringLiteral("comment"), QString::fromStdWString(record.comment)},
                                           {QStringLiteral("target_alias"), QString::fromStdWString(record.alias)},
                                           {QStringLiteral("attributes"), static_cast<double>(record.attributes)},
                                           {QStringLiteral("blob"), QString::fromLatin1(record.blob.toBase64())}});
    }
    // How it lists and reads what it kept.
    const base::Result<QStringList> listed = store->names();
    if (!listed || listed.value().size() != secrets.size()) {
        stop(QStringLiteral("this version does not list what it kept"));
    }
    for (auto one = secrets.begin(); one != secrets.end(); ++one) {
        const base::Result<std::optional<QByteArray>> read = store->read(one.key());
        if (!read || !read.value()) {
            stop(QStringLiteral("this version does not read back %1").arg(one.key()));
        }
    }
    // And how it erases them ("Erase saved secrets"): every one gone.
    for (auto one = secrets.begin(); one != secrets.end(); ++one) {
        if (!store->erase(one.key())) {
            stop(QStringLiteral("this version does not erase %1").arg(one.key()));
        }
    }
    if (!testing::credentials::kept().empty()) {
        stop(QStringLiteral("this version leaves records behind when it erases"));
    }
    const testing::credentials::Asked& asked = testing::credentials::asked();
    QJsonArray filters;
    for (const std::wstring& filter : asked.filters) {
        filters.append(QString::fromStdWString(filter));
    }
    const QJsonObject shape{
        {QStringLiteral("made_by"),
         QJsonObject{{QStringLiteral("commit"), QString::fromLocal8Bit(argv[2])},
                     {QStringLiteral("source"), QStringLiteral("desktop/core/src/windows_secrets.cpp")}}},
        {QStringLiteral("credentials"), made},
        {QStringLiteral("write_flags"), numbers(asked.writeFlags)},
        {QStringLiteral("list_filters"), filters},
        {QStringLiteral("list_flags"), numbers(asked.enumerateFlags)},
        {QStringLiteral("read_types"), numbers(asked.readTypes)},
        {QStringLiteral("read_flags"), numbers(asked.readFlags)},
        {QStringLiteral("erase_types"), numbers(asked.deleteTypes)},
        {QStringLiteral("erase_flags"), numbers(asked.deleteFlags)},
    };
    QFile out(fixture.filePath(QStringLiteral("credentials.json")));
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(shape).toJson()) <= 0) {
        stop(QStringLiteral("credentials.json was not written"));
    }
    std::printf("wrote the shapes of %lld secrets\n", static_cast<long long>(secrets.size()));
    return 0;
}
