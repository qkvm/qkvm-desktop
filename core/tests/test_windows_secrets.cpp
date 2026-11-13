#include <memory>

#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QTest>
#include <QUuid>

#include <qkvm/core/secrets.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

// Windows Credential Manager itself, of whoever runs the test: under a name of the test's own, which
// no entry of the program has, and with everything taken out again at the end.
class TestWindowsSecrets : public QObject {
    Q_OBJECT

private:
    QString m_under = u"QKVM Desktop test %1"_s.arg(QCoreApplication::applicationPid());
    QStringList m_used;

    QString name(const QString& of)
    {
        m_used.append(of);
        return of;
    }

private slots:
    void textTypedIntoCmdkeyIsKeptAsItsUtf16Bytes()
    {
        const QString prefix = u"qKVM test typed record "_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto store = qkvm::core::windowsSecrets(prefix);
        const auto cleanup = qScopeGuard([&] { store->forget(u"app password"_s); });
        QByteArray json = "{\"v\":1,\"probe\":\"abc+/=\",\"locked\":false}";
        if (json.size() % 2 != 0)
            json.append(' ');
        QString typed;
        for (qsizetype at = 0; at < json.size(); at += 2)
            typed.append(QChar(char16_t(quint8(json[at]) | (quint8(json[at + 1]) << 8))));
        QProcess cmdkey;
        cmdkey.setProgram(u"cmdkey.exe"_s);
        cmdkey.setNativeArguments(u"/generic:\""_s + prefix + u": app password\" /user:probe /pass:"_s + typed);
        cmdkey.start();
        QVERIFY(cmdkey.waitForFinished(10000));
        QCOMPARE(cmdkey.exitCode(), 0);
        QCOMPARE(store->kept(u"app password"_s), json);
    }

    // P5 everythingKeptCanReadIsListed
    //   names() keeps a target only if it startsWith(prefix) case-sensitively
    //   (windows_secrets.cpp:86), while CredReadW/CredDeleteW match target names regardless of case
    //   (hypothesis). An entry differing only in case would then be readable by kept() but invisible
    //   to names(): it neither blocks first setup nor is erased by the reset.
    //   PASS (sound): kept() does not read it, or names() lists it.
    //   FAIL (defect): kept() returns "test bytes" and names() is empty.
    void everythingKeptCanReadIsListed()
    {
        const QString prefix = u"qKVM test case "_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto exact = qkvm::core::windowsSecrets(prefix);
        const auto other = qkvm::core::windowsSecrets(prefix.toUpper());
        const auto cleanup = qScopeGuard([&] {
            other->forget(u"orphan"_s);
            exact->forget(u"orphan"_s);
        });
        QVERIFY(other->keep(u"orphan"_s, "test bytes"));
        const QByteArray read = exact->kept(u"orphan"_s);
        const auto listed = exact->names();
        QVERIFY(listed);
        qInfo() << "read by other case:" << !read.isEmpty() << "listed:" << listed.value();
        QVERIFY(read.isEmpty() || listed.value().contains(u"orphan"_s));
    }

    void cleanup()
    {
        const std::unique_ptr<core::SecretStore> secrets = core::windowsSecrets(m_under);
        for (const QString& used : std::as_const(m_used)) {
            secrets->forget(used);
        }
        m_used.clear();
    }

    void windowsEnumerationAndErasureStayWithinTheirPrefix()
    {
        const QString prefix = u"qKVM test app lock "_s + QUuid::createUuid().toString();
        const auto store = qkvm::core::windowsSecrets(prefix);
        const auto neighbor = qkvm::core::windowsSecrets(prefix + u" neighbor"_s);
        const auto cleanup = qScopeGuard([&] {
            store->forget(u"orphan"_s);
            neighbor->forget(u"neighbor"_s);
        });
        QVERIFY(store->keep(u"orphan"_s, "test bytes"));
        QVERIFY(neighbor->keep(u"neighbor"_s, "test bytes"));
        const auto names = store->names();
        QVERIFY(names);
        QCOMPARE(names.value(), QStringList{u"orphan"_s});
        QVERIFY(store->erase(u"orphan"_s));
        QVERIFY(store->names().value().isEmpty());
        QCOMPARE(neighbor->kept(u"neighbor"_s), QByteArray("test bytes"));
    }
    void whatIsKeptIsThereForAnotherWhoAsksByTheSameName()
    {
        const QByteArray secret("a session's token \x01\xff with bytes that are no letters", 51);
        {
            const std::unique_ptr<core::SecretStore> secrets = core::windowsSecrets(m_under);
            QVERIFY(secrets->keep(name(u"session at http://192.168.1.10:8787/"_s), secret).ok());
        }
        const std::unique_ptr<core::SecretStore> again = core::windowsSecrets(m_under);
        QCOMPARE(again->kept(u"session at http://192.168.1.10:8787/"_s), secret);
        // Under another name there is nothing, and neither for a program of another name.
        QCOMPARE(again->kept(u"session at http://192.168.1.11:8787/"_s), QByteArray());
        QCOMPARE(core::windowsSecrets(m_under + u" other"_s)->kept(u"session at http://192.168.1.10:8787/"_s),
                 QByteArray());
    }

    void aNewSecretTakesThePlaceOfTheOld()
    {
        const std::unique_ptr<core::SecretStore> secrets = core::windowsSecrets(m_under);
        QVERIFY(secrets->keep(name(u"one"_s), "first").ok());
        QVERIFY(secrets->keep(u"one"_s, "second").ok());
        QCOMPARE(secrets->kept(u"one"_s), QByteArray("second"));
    }

    void whatIsForgottenIsGone()
    {
        const std::unique_ptr<core::SecretStore> secrets = core::windowsSecrets(m_under);
        QVERIFY(secrets->keep(name(u"one"_s), "a secret").ok());
        secrets->forget(u"one"_s);
        QCOMPARE(secrets->kept(u"one"_s), QByteArray());
        // Forgetting what was never kept is nothing.
        secrets->forget(u"never kept"_s);
    }

    // M-D1: read() tells "nothing kept under the name" (an empty optional) from what is kept. (That
    // Windows refuses a read cannot be brought about on a PC that is well; the stores of the tests
    // stand in for it.)
    void readTellsNothingKeptFromWhatIsKept()
    {
        const std::unique_ptr<core::SecretStore> secrets = core::windowsSecrets(m_under);
        const auto never = secrets->read(u"never kept"_s);
        QVERIFY(never);
        QVERIFY(!never.value().has_value());
        QVERIFY(secrets->keep(name(u"kept"_s), "a secret").ok());
        const auto kept = secrets->read(u"kept"_s);
        QVERIFY(kept);
        QCOMPARE(kept.value().value_or(QByteArray()), QByteArray("a secret"));
        secrets->forget(u"kept"_s);
        const auto gone = secrets->read(u"kept"_s);
        QVERIFY(gone);
        QVERIFY(!gone.value().has_value());
    }

    void nothingAndTooMuchAreNotKept()
    {
        const std::unique_ptr<core::SecretStore> secrets = core::windowsSecrets(m_under);
        QVERIFY(!secrets->keep(name(u"one"_s), QByteArray()).ok());
        QVERIFY(!secrets->keep(u"one"_s, QByteArray(core::largestSecretBytes + 1, 'x')).ok());
        QCOMPARE(secrets->kept(u"one"_s), QByteArray());
        QVERIFY(secrets->keep(u"one"_s, QByteArray(core::largestSecretBytes, 'x')).ok());
        QCOMPARE(secrets->kept(u"one"_s).size(), core::largestSecretBytes);
    }
};

QTEST_GUILESS_MAIN(TestWindowsSecrets)
#include "test_windows_secrets.moc"
