#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <qkvm/core/awake_line.h>
#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/kvmd_console_line.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/kvmd_stream.h>
#include <qkvm/core/live_link.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/server_client.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/tls.h>

#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmdConsoleLine;
using core::KvmdDevice;
using core::KvmdStream;
using core::KvmProbe;
using core::RedfishDevice;
using core::ServerClient;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::MadeUpKvm;
using core::testing::StandIn;

namespace {

// Something that takes whoever comes to its port, in the clear, and counts them: a caller that
// began to speak TLS to it has come all the same, and is counted, and hung up on.
struct Listener {
    StandIn stand{[](const Asked&) { return Answered(404, "{}"_ba); }};

    // The same place, named as one that is spoken to over TLS.
    QUrl encrypted(const QString& scheme = u"https"_s) const
    {
        return QUrl(u"%1://127.0.0.1:%2"_s.arg(scheme).arg(stand.serverPort()));
    }
    // Whether nothing at all has come to it, some moments after it might have.
    bool heardNothing() const
    {
        QTest::qWait(250);
        return stand.lines == 0 && stand.notHttp == 0 && stand.asked.isEmpty() && stand.live.lines == 0;
    }
};

// The times a real KVM is given, made short.
KvmdDevice::Pace quick()
{
    KvmdDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.freshMs = 200;
    pace.firstContactMs = 400;
    pace.patienceMs = 400;
    return pace;
}

// Asks, lets the program's loop run until the answer is there, and gives it.
template <typename Answer>
base::Result<Answer> answerTo(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
{
    QList<base::Result<Answer>> answers;
    ask([&](base::Result<Answer> answer) { answers.append(std::move(answer)); });
    if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 20000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

std::string codeOf(const base::Error& error)
{
    return error.code;
}

} // namespace

// The program where what it speaks over TLS is not Windows' own (tls.h): something chose another
// before the program's first speaker was made, which is also what a copy of the program without
// Windows' own TLS comes to. Each of the five speakers refuses an HTTPS address before anything
// goes out, in one code and one sentence; plain HTTP is spoken as ever. In a process of its own,
// since the choice is made once for a whole process.
class TestTlsOtherFirst : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Whichever other TLS this PC's Qt has: OpenSSL where it finds one (which is the case
        // the choice is there for: Qt would take it by itself), else the part that only reads
        // certificates. Chosen, and in use from when the first socket is made. (Where there is
        // none, nothing here can be shown, and no test of this program runs. Qt Test ends that
        // as a success; ctest is told to call it "did not run" by the line this skips with:
        // core/CMakeLists.txt, which has to change with that line.)
        const QList<QString> all = QSslSocket::availableBackends();
        QString other;
        for (const QString& name : all) {
            if (name != u"schannel"_s && (other.isEmpty() || name == u"openssl"_s)) {
                other = name;
            }
        }
        if (other.isEmpty()) {
            QSKIP("This PC's Qt has no TLS but Windows' own: another cannot be chosen first.");
        }
        QVERIFY(QSslSocket::setActiveBackend(other));
        const QSslSocket inUse;
        QCOMPARE(QSslSocket::activeBackend(), other);
        qInfo("The other TLS in use here: %s", qPrintable(other));
        // The program's own choice comes too late, and Qt says so.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"another backend is already in use"_s));
        QVERIFY(!core::useWindowsOwnTls());
    }

    void theDriverOfAKvmRefusesAnHttpsAddressBeforeAnythingGoesOut()
    {
        const Listener listener;
        KvmdDevice::Where where;
        where.address = listener.encrypted();
        KvmdDevice device(where, u"secret"_s, quick());
        // Signing in, which is where the password would go.
        const auto who = answerTo<KvmdDevice::Identity>([&](auto done) { device.identify(std::move(done)); });
        QVERIFY(!who.ok());
        QCOMPARE(codeOf(who.error()), std::string(core::tlsDamaged));
        QCOMPARE(who.error().message, std::string(core::tlsDamagedSentence));
        // A look: said on its card as something to be looked at, and not as a KVM that is away.
        QSignalSpy looked(&device, &KvmdDevice::looked);
        device.look();
        QVERIFY(looked.wait(20000));
        QCOMPARE(device.status().state, u"error"_s);
        QCOMPARE(device.status().errorCode, QLatin1StringView(core::tlsDamaged));
        QCOMPARE(device.status().errorMessage, QLatin1StringView(core::tlsDamagedSentence));
        // A picture, something done to it, its stream, and the line for keyboard and mouse.
        const auto picture = answerTo<core::Snapshot>([&](auto done) { device.askSnapshot(400, std::move(done)); });
        QVERIFY(!picture.ok());
        QCOMPARE(codeOf(picture.error()), std::string(core::tlsDamaged));
        const auto done = answerTo<core::Done>(
            [&](auto told) { device.act(core::Action::press(u"ControlLeft,AltLeft,Delete"_s), std::move(told)); });
        QVERIFY(!done.ok());
        QCOMPARE(codeOf(done.error()), std::string(core::tlsDamaged));
        QVERIFY(!KvmdDevice::mayHaveBeenDone(done.error()));
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const QSignalSpy ended(stream.get(), &KvmdStream::ended);
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(ended.value(0).value(0).toString(), QLatin1StringView(core::tlsDamaged));
        QList<std::string> noConsole;
        device.openConsole([&noConsole](const base::Result<std::unique_ptr<KvmdConsoleLine>>& made) {
            noConsole.append(made ? std::string("a line was made") : made.error().code);
        });
        QTRY_COMPARE(noConsole.size(), 1);
        QCOMPARE(noConsole.constFirst(), std::string(core::tlsDamaged));
        QVERIFY(!device.isKeptAwake());
        QVERIFY(listener.heardNothing());
    }

    void theDriverOfAServersControllerRefusesAnHttpsAddressBeforeAnythingGoesOut()
    {
        // Its password least of all; and a look says so at once, as something to be looked at: no good look ever
        // comes before it (the choice is the program's, from before anything is asked), so none is left standing.
        const Listener listener;
        RedfishDevice::Where where;
        where.address = listener.encrypted();
        RedfishDevice::Pace pace;
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.goodbyes = std::make_shared<core::ControllerGoodbyes>();
        RedfishDevice device(where, u"secret"_s, pace);
        const auto who = answerTo<RedfishDevice::Identity>([&](auto done) { device.identify(std::move(done)); });
        QVERIFY(!who.ok());
        QCOMPARE(codeOf(who.error()), std::string(core::tlsDamaged));
        QSignalSpy looked(&device, &RedfishDevice::looked);
        device.look();
        QVERIFY(looked.wait(20000));
        QCOMPARE(device.status().state, u"error"_s);
        QCOMPARE(device.status().errorCode, QLatin1StringView(core::tlsDamaged));
        QCOMPARE(device.status().errorMessage, QLatin1StringView(core::tlsDamagedSentence));
        QVERIFY(listener.heardNothing());
    }

    void theDriverOfAKvmSpeaksPlainHttpAsEver()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const auto who = answerTo<KvmdDevice::Identity>([&](auto done) { device.identify(std::move(done)); });
        QVERIFY2(who.ok(), who.ok() ? "" : who.error().code.c_str());
        QCOMPARE(kvm.signIns, 1);
    }

    void theQuestionOfWhatAnswersThereIsNotAskedOfAnHttpsAddress()
    {
        const Listener listener;
        KvmProbe probe;
        QList<KvmProbe::Found> told;
        probe.ask(listener.encrypted(), [&told](const KvmProbe::Found& found) { told.append(found); });
        // (A turn of the loop later, as everything a probe says.)
        QVERIFY(told.isEmpty());
        QTRY_COMPARE(told.size(), 1);
        QCOMPARE(told.constFirst().kind, KvmProbe::Kind::NotAsked);
        QVERIFY(told.constFirst().shown.isEmpty());
        QVERIFY(!told.constFirst().trustedByWindows);
        QVERIFY(listener.heardNothing());

        // And whoever adds a KVM there is told why not, with the one sentence; its password is
        // kept nowhere.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        core::LocalWall wall(settings, secrets, quick());
        core::KvmAdding adding(wall);
        core::KvmAdding::Typed typed;
        typed.address = listener.encrypted().toString();
        typed.password = u"secret"_s;
        const auto added = answerTo<core::KvmAdding::Added>([&](auto done) { adding.add(typed, std::move(done)); });
        QVERIFY(!added.ok());
        QCOMPARE(codeOf(added.error()), std::string(core::tlsDamaged));
        QCOMPARE(added.error().message, std::string(core::tlsDamagedSentence));
        QVERIFY(wall.kvms().isEmpty());
        QVERIFY(secrets.all.isEmpty());
        QVERIFY(listener.heardNothing());
        // Nor is the certificate of a KVM that is on the wall read anew: the same sentence.
        core::OwnKvm own;
        own.address = listener.encrypted().toString();
        const base::Result<QString> id = wall.add(own, u"secret"_s);
        QVERIFY(id.ok());
        core::KvmCertificateReview review(wall);
        const auto shown = answerTo<core::KvmCertificateReview::Shown>(
            [&](auto done) { review.askCertificates(id.value(), std::move(done)); });
        QVERIFY(!shown.ok());
        QCOMPARE(codeOf(shown.error()), std::string(core::tlsDamaged));
        QVERIFY(listener.heardNothing());

        // A plain HTTP address is asked as ever.
        MadeUpKvm kvm;
        told.clear();
        probe.ask(kvm.address(), [&told](const KvmProbe::Found& found) { told.append(found); });
        QTRY_COMPARE(told.size(), 1);
        QCOMPARE(told.constFirst().kind, KvmProbe::Kind::Kvmd);
    }

    void aQkvmServerAtAnHttpsAddressIsNotNamed()
    {
        const Listener listener;
        ServerClient client;
        const base::Result<void> taken = client.setAddress(listener.encrypted().toString());
        QVERIFY(!taken.ok());
        QCOMPARE(codeOf(taken.error()), std::string(core::tlsDamaged));
        QCOMPARE(taken.error().message, std::string(core::tlsDamagedSentence));
        // No server is named by that: a call has nowhere to go, and nothing goes there.
        QVERIFY(client.address().isEmpty());
        QVERIFY(!client.isEncrypted());
        const auto health = answerTo<core::Health>([&](auto done) { client.askHealth(std::move(done)); });
        QVERIFY(!health.ok());
        QVERIFY(listener.heardNothing());

        // One that was named in the clear stays the one that is spoken to, and is spoken to as ever.
        StandIn plain([](const Asked&) {
            return Answered(200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"], "locked": false})"_ba);
        });
        QVERIFY(client.setAddress(plain.address()).ok());
        QVERIFY(!client.setAddress(listener.encrypted().toString()).ok());
        QCOMPARE(client.address(), QUrl(plain.address() + u'/'));
        const auto again = answerTo<core::Health>([&](auto done) { client.askHealth(std::move(done)); });
        QVERIFY2(again.ok(), again.ok() ? "" : again.error().code.c_str());
        QCOMPARE(plain.count("/healthz"), 1);
        QVERIFY(listener.heardNothing());
    }

    void aLineToAKvmIsNotAskedForAtAnHttpsAddress()
    {
        const Listener listener;
        KvmdConsoleLine::Where where;
        where.address = listener.encrypted();
        where.query = "stream=0";
        where.cookie = "auth_token=pass1";
        KvmdConsoleLine line(where);
        const QSignalSpy opened(&line, &KvmdConsoleLine::opened);
        const QSignalSpy closed(&line, &KvmdConsoleLine::closed);
        // (Said a turn of the loop later, when whoever made it can hear it.)
        QVERIFY(closed.isEmpty());
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(closed.value(0).value(0).toString(), QLatin1StringView(core::tlsDamaged));
        QCOMPARE(closed.value(0).value(1).toString(), QLatin1StringView(core::tlsDamagedSentence));
        QVERIFY(!closed.value(0).value(2).toBool());
        QVERIFY(opened.isEmpty());
        QVERIFY(!line.isOpen());
        // Nor the line that keeps a KVM's video awake, which carries the pass as well.
        core::AwakeLine::Where awakeAt;
        awakeAt.address = listener.encrypted();
        awakeAt.asking = "/api/ws?stream=1&auth_token=pass1";
        awakeAt.cookie = "auth_token=pass1";
        core::AwakeLine awake(awakeAt, 1500);
        const QSignalSpy ended(&awake, &core::AwakeLine::ended);
        awake.open();
        QTRY_COMPARE(ended.count(), 1);
        QVERIFY(!awake.isIn());
        QVERIFY(listener.heardNothing());
    }

    void aMachineIsNotHadLiveOverALineThatWouldBeEncrypted()
    {
        const Listener listener;
        QUrl where = listener.encrypted(u"wss"_s);
        where.setPath(u"/api/v1/devices/a/live"_s);
        core::LiveLink link(QNetworkRequest(where), "p4ss-for-one-line_0123456789", core::Live::Need{}, {},
                            core::LiveLink::Watch{});
        QList<QStringList> closed;
        connect(&link, &core::Live::closed, this,
                [&closed](const QString& code, const QString& message) { closed.append({code, message}); });
        QVERIFY(closed.isEmpty());
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(closed.constFirst(),
                 (QStringList{QString::fromLatin1(core::tlsDamaged), QString::fromLatin1(core::tlsDamagedSentence)}));
        QVERIFY(!link.isOpen());
        QVERIFY(listener.heardNothing());
    }
};

QTEST_GUILESS_MAIN(TestTlsOtherFirst)

#include "test_tls_other_first.moc"
