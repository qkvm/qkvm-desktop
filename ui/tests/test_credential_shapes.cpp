// This build's own Windows code (core/src/windows_secrets.cpp, compiled against a Credential Manager in memory) finds
// the records every published version's own Windows code wrote, as each fixture's credentials.json has them: by the
// filter it lists with, the type it reads by, the target names those records have; writes them again in the same
// shape (review 012, M3); erases them all; and asks the store with the very filters, types and flags that version
// asked it with (review 012 confirm, N3). Nothing of it reaches Windows' own store.
#include "windows_secrets_in_memory.h"

#include <set>
#include <utility>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// Every published version, by each kind of profile it has a fixture of: none may go.
const QStringList published{
    u"0.1.0"_s, u"0.1.1"_s, u"0.1.2"_s, u"0.1.0-wall-screen"_s, u"0.1.1-wall-screen"_s, u"0.1.2-wall-screen"_s};

QJsonObject jsonIn(const QString& file)
{
    QFile kept(file);
    return kept.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(kept.readAll()).object() : QJsonObject();
}

// A record as the fixture's version wrote it.
testing::credentials::Record recordOf(const QJsonObject& said)
{
    testing::credentials::Record record;
    record.target = said[u"target"_s].toString().toStdWString();
    record.type = static_cast<DWORD>(said[u"type"_s].toInteger());
    record.persist = static_cast<DWORD>(said[u"persist"_s].toInteger());
    record.flags = static_cast<DWORD>(said[u"flags"_s].toInteger());
    record.userName = said[u"user_name"_s].toString().toStdWString();
    record.comment = said[u"comment"_s].toString().toStdWString();
    record.alias = said[u"target_alias"_s].toString().toStdWString();
    record.attributes = static_cast<DWORD>(said[u"attributes"_s].toInteger());
    record.blob = QByteArray::fromBase64(said[u"blob"_s].toString().toLatin1());
    return record;
}

// The values one kind of call was made with, each once and in order: what is compared (how often a call is made is
// not: a later build may read once more, but never by another type or flag).
QList<double> distinct(const std::vector<DWORD>& all)
{
    const std::set<DWORD> once(all.begin(), all.end());
    return {once.begin(), once.end()};
}

QList<double> distinct(const QJsonValue& said)
{
    std::set<double> once;
    const QJsonArray all = said.toArray();
    for (const auto& one : all) {
        once.insert(one.toDouble());
    }
    return {once.begin(), once.end()};
}

QString shown(const QList<double>& all)
{
    QStringList said;
    for (const double one : all) {
        said.append(QString::number(one));
    }
    return u'[' + said.join(u", ") + u']';
}

} // namespace

class TestCredentialShapes : public QObject {
    Q_OBJECT

private slots:
    void init() { testing::credentials::clear(); }

    void everyRecordAVersionWroteIsFoundAndWrittenAgainInItsShape_data()
    {
        QTest::addColumn<QString>("folder");
        const QDir all(QStringLiteral(QKVM_PROFILE_FIXTURES));
        for (const QString& version : published) {
            QTest::newRow(qPrintable(version)) << all.filePath(version);
        }
    }
    void everyRecordAVersionWroteIsFoundAndWrittenAgainInItsShape()
    {
        QFETCH(QString, folder);
        const QJsonObject shapes = jsonIn(folder + u"/credentials.json"_s);
        const QJsonObject secrets = jsonIn(folder + u"/secrets.json"_s);
        const QJsonObject records = shapes[u"credentials"_s].toObject();
        QVERIFY2(!records.isEmpty(), "credentials.json of a published version is missing");
        QCOMPARE(records.keys(), secrets.keys());
        // What that version wrote, put where this build looks: in its store, as it was written there.
        for (auto one = records.begin(); one != records.end(); ++one) {
            const testing::credentials::Record record = recordOf(one.value().toObject());
            QCOMPARE(record.blob,
                     QByteArray::fromBase64(secrets[one.key()].toObject()[u"bytes"_s].toString().toLatin1()));
            testing::credentials::kept().push_back(record);
        }
        // This build's own, with its own default of what its secrets are kept under.
        const std::unique_ptr<core::SecretStore> store = core::capturedWindowsSecrets();
        const base::Result<QStringList> names = store->names();
        QVERIFY2(names, "this build does not list what the version wrote");
        QStringList listed = names.value();
        listed.sort();
        QStringList expected = records.keys();
        expected.sort();
        QCOMPARE(listed, expected);
        for (auto one = records.begin(); one != records.end(); ++one) {
            const QByteArray blob = recordOf(one.value().toObject()).blob;
            const base::Result<std::optional<QByteArray>> read = store->read(one.key());
            QVERIFY2(read && read.value(), qPrintable(one.key()));
            QCOMPARE(read.value().value_or(QByteArray()), blob);
            QCOMPARE(store->kept(one.key()), blob);
        }
        // And what this build writes of the same is in the version's very shape: so the version reads it too (an
        // update undone).
        std::vector<DWORD> writeFlags;
        for (auto one = records.begin(); one != records.end(); ++one) {
            const testing::credentials::Record was = recordOf(one.value().toObject());
            testing::credentials::clear();
            QVERIFY(store->keep(one.key(), was.blob));
            const std::vector<DWORD>& flags = testing::credentials::asked().writeFlags;
            writeFlags.insert(writeFlags.end(), flags.begin(), flags.end());
            QCOMPARE(testing::credentials::kept().size(), std::size_t{1});
            const testing::credentials::Record& now = testing::credentials::kept().front();
            QCOMPARE(QString::fromStdWString(now.target), QString::fromStdWString(was.target));
            QCOMPARE(now.type, was.type);
            QCOMPARE(now.persist, was.persist);
            QCOMPARE(now.flags, was.flags);
            QCOMPARE(QString::fromStdWString(now.userName), QString::fromStdWString(was.userName));
            QCOMPARE(QString::fromStdWString(now.comment), QString::fromStdWString(was.comment));
            QCOMPARE(QString::fromStdWString(now.alias), QString::fromStdWString(was.alias));
            QCOMPARE(now.attributes, was.attributes);
            QCOMPARE(now.blob, was.blob);
        }
        // "Erase saved secrets" leaves none of the version's records behind.
        testing::credentials::clear();
        for (auto one = records.begin(); one != records.end(); ++one) {
            testing::credentials::kept().push_back(recordOf(one.value().toObject()));
        }
        QVERIFY(store->names());
        const QStringList all = records.keys();
        for (const QString& name : all) {
            QVERIFY2(store->read(name), qPrintable(name));
            QVERIFY2(!store->kept(name).isEmpty(), qPrintable(name));
        }
        for (const QString& name : all) {
            QVERIFY2(store->erase(name), qPrintable(name));
        }
        QVERIFY2(testing::credentials::kept().empty(), "an erase left a record the version wrote");
        // And every call is made as that version made it: the same filter, the same types, the same flags.
        const testing::credentials::Asked& asked = testing::credentials::asked();
        QStringList filters;
        for (const std::wstring& one : asked.filters) {
            filters.append(QString::fromStdWString(one));
        }
        filters.removeDuplicates();
        QStringList itsFilters;
        const QJsonArray listedBy = shapes[u"list_filters"_s].toArray();
        for (const auto& one : listedBy) {
            itsFilters.append(one.toString());
        }
        itsFilters.removeDuplicates();
        QVERIFY(!itsFilters.isEmpty());
        QCOMPARE(filters, itsFilters);
        const std::vector<std::pair<QString, QList<double>>> calls{
            {u"write_flags"_s, distinct(writeFlags)},        {u"list_flags"_s, distinct(asked.enumerateFlags)},
            {u"read_types"_s, distinct(asked.readTypes)},    {u"read_flags"_s, distinct(asked.readFlags)},
            {u"erase_types"_s, distinct(asked.deleteTypes)}, {u"erase_flags"_s, distinct(asked.deleteFlags)},
        };
        for (const auto& [field, now] : calls) {
            const QList<double> its = distinct(shapes[field]);
            QVERIFY2(!its.isEmpty(), qPrintable(field + u" is not in credentials.json"_s));
            QVERIFY2(now == its, qPrintable(u"%1: this build %2, the version %3"_s.arg(field, shown(now), shown(its))));
        }
    }

    // What is in memory follows Windows' own rules, which are what the records above are read by: a name is told
    // apart without regard to the case of its letters and by its type; a filter is a name or a beginning and "*".
    void theCredentialManagerInMemoryKeepsWindowsRules()
    {
        CREDENTIALW entry{};
        std::wstring target = L"QKVM Desktop: kvm:0a1b2c3d";
        QByteArray blob("a blob");
        entry.Type = CRED_TYPE_GENERIC;
        entry.TargetName = target.data();
        entry.CredentialBlobSize = static_cast<DWORD>(blob.size());
        entry.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
        entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
        QVERIFY(testing::credentials::write(&entry, 0));
        PCREDENTIALW read = nullptr;
        QVERIFY(testing::credentials::read(L"qkvm desktop: KVM:0A1B2C3D", CRED_TYPE_GENERIC, 0, &read));
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(read->CredentialBlob),
                            static_cast<qsizetype>(read->CredentialBlobSize)),
                 blob);
        testing::credentials::release(read);
        QVERIFY(!testing::credentials::read(target.c_str(), CRED_TYPE_DOMAIN_PASSWORD, 0, &read));
        QCOMPARE(GetLastError(), static_cast<DWORD>(ERROR_NOT_FOUND));
        DWORD count = 0;
        PCREDENTIALW* list = nullptr;
        QVERIFY(testing::credentials::enumerate(L"qkvm DESKTOP: *", 0, &count, &list));
        QCOMPARE(count, DWORD{1});
        testing::credentials::release(static_cast<void*>(list));
        QVERIFY(!testing::credentials::enumerate(L"QKVM Desktop:*x", 0, &count, &list));
        QVERIFY(!testing::credentials::enumerate(L"QKVM", 0, &count, &list));
        QVERIFY(testing::credentials::erase(L"QKVM DESKTOP: kvm:0a1b2c3d", CRED_TYPE_GENERIC, 0));
        QVERIFY(testing::credentials::kept().empty());
    }

    // And what Windows refuses, it refuses as Windows does (ERROR_INVALID_FLAGS): a flag Windows has not, a flag of a
    // read or an erase (reserved: zero), a filter together with CRED_ENUMERATE_ALL_CREDENTIALS. What Windows takes,
    // it does as Windows: every record by the name of its kind for CRED_ENUMERATE_ALL_CREDENTIALS, and the blob
    // already there for CRED_PRESERVE_CREDENTIAL_BLOB (whose own must then be empty).
    void whatWindowsRefusesTheOneInMemoryRefusesToo()
    {
        const auto refusedFor = [](BOOL said, DWORD why) { return said == FALSE && GetLastError() == why; };
        CREDENTIALW entry{};
        std::wstring target = L"QKVM Desktop: kvm:0a1b2c3d";
        QByteArray blob("a blob");
        entry.Type = CRED_TYPE_GENERIC;
        entry.TargetName = target.data();
        entry.CredentialBlobSize = static_cast<DWORD>(blob.size());
        entry.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
        entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
        QVERIFY(refusedFor(testing::credentials::write(&entry, 0x2), ERROR_INVALID_FLAGS));
        QVERIFY(
            refusedFor(testing::credentials::write(&entry, CRED_PRESERVE_CREDENTIAL_BLOB), ERROR_INVALID_PARAMETER));
        QVERIFY(testing::credentials::kept().empty());
        QVERIFY(testing::credentials::write(&entry, 0));
        entry.CredentialBlobSize = 0;
        entry.CredentialBlob = nullptr;
        entry.Persist = CRED_PERSIST_ENTERPRISE;
        QVERIFY(testing::credentials::write(&entry, CRED_PRESERVE_CREDENTIAL_BLOB));
        QCOMPARE(testing::credentials::kept().size(), std::size_t{1});
        QCOMPARE(testing::credentials::kept().front().blob, blob);
        QCOMPARE(testing::credentials::kept().front().persist, static_cast<DWORD>(CRED_PERSIST_ENTERPRISE));

        PCREDENTIALW read = nullptr;
        QVERIFY(
            refusedFor(testing::credentials::read(target.c_str(), CRED_TYPE_GENERIC, 1, &read), ERROR_INVALID_FLAGS));
        QVERIFY(read == nullptr);

        DWORD count = 0;
        PCREDENTIALW* list = nullptr;
        QVERIFY(refusedFor(
            testing::credentials::enumerate(L"QKVM Desktop: *", CRED_ENUMERATE_ALL_CREDENTIALS, &count, &list),
            ERROR_INVALID_FLAGS));
        QVERIFY(
            refusedFor(testing::credentials::enumerate(L"QKVM Desktop: *", 0x2, &count, &list), ERROR_INVALID_FLAGS));
        QVERIFY(refusedFor(testing::credentials::enumerate(nullptr, 0x2, &count, &list), ERROR_INVALID_FLAGS));
        QVERIFY(list == nullptr);
        QVERIFY(testing::credentials::enumerate(nullptr, CRED_ENUMERATE_ALL_CREDENTIALS, &count, &list));
        QCOMPARE(count, DWORD{1});
        QCOMPARE(QString::fromWCharArray(list[0]->TargetName), u"LegacyGeneric:target=QKVM Desktop: kvm:0a1b2c3d"_s);
        testing::credentials::release(static_cast<void*>(list));

        QVERIFY(refusedFor(testing::credentials::erase(target.c_str(), CRED_TYPE_GENERIC, 1), ERROR_INVALID_FLAGS));
        QCOMPARE(testing::credentials::kept().size(), std::size_t{1});
        QVERIFY(testing::credentials::erase(target.c_str(), CRED_TYPE_GENERIC, 0));
        QVERIFY(refusedFor(testing::credentials::write(&entry, CRED_PRESERVE_CREDENTIAL_BLOB), ERROR_NOT_FOUND));
        QVERIFY(testing::credentials::kept().empty());
    }
};

QTEST_GUILESS_MAIN(TestCredentialShapes)
#include "test_credential_shapes.moc"
