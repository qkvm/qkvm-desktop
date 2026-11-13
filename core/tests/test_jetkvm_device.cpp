#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/device_line.h>
#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>

#include "jetkvm_by_hand.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using qkvm::core::JetKvmDevice;
using qkvm::core::SignInPacing;
using qkvm::core::testing::Answered;
using qkvm::core::testing::JetKvmByHand;
using qkvm::core::testing::StandIn;
using qkvm::core::testing::Tls;

namespace {

// Times made short, and a pacing of the test's own: nothing of one test's sign-ins reaches another's.
JetKvmDevice::Pace quick()
{
    JetKvmDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.refusedHoldMs = 1200;
    pace.signInRetryMs = 800;
    pace.freshMs = 200;
    pace.lineWithinMs = 3000;
    pace.versionWithinMs = 1500;
    pace.pacing = std::make_shared<SignInPacing>();
    return pace;
}

JetKvmDevice::Where at(const JetKvmByHand& jet)
{
    JetKvmDevice::Where where;
    where.address = jet.address();
    return where;
}

Tls tls(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

QByteArray sha256Of(const char* name)
{
    return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
}

QString digestOf(const QString& number)
{
    return u"jetkvm:"_s +
           QString::fromLatin1(QCryptographicHash::hash(number.toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

template <typename Answer>
base::Result<Answer> answerTo(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
{
    QList<base::Result<Answer>> answers;
    ask([&](base::Result<Answer> answer) { answers.append(std::move(answer)); });
    if (!answers.isEmpty()) {
        return base::Error{"at_once", "The answer came before the call came back."};
    }
    if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 20000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

base::Result<JetKvmDevice::Identity> identified(JetKvmDevice& device)
{
    return answerTo<JetKvmDevice::Identity>([&](auto done) { device.identify(std::move(done)); });
}

qkvm::core::Status lookedAt(JetKvmDevice& device)
{
    QSignalSpy over(&device, &JetKvmDevice::looked);
    device.look();
    if (!over.wait(20000)) {
        qkvm::core::Status never;
        never.state = u"the look never ended"_s;
        return never;
    }
    return device.status();
}

std::string codeOf(const base::Result<JetKvmDevice::Identity>& what)
{
    return what ? std::string("ok") : what.error().code;
}

} // namespace

class TestJetKvmDevice : public QObject {
    Q_OBJECT

private slots:
    // (Windows' own TLS chosen before any stand-in makes a line: as the program chooses it at its start.)
    void initTestCase() { QVERIFY(qkvm::core::useWindowsOwnTls()); }

    void anAdditionNeverTakesANumberItCouldNotRead()
    {
        // From the reading of the twins (L1): GET /device answered 500 is no number read, and not "none". And as the
        // web's identify (jetkvm.py, _same_jetkvm with `taking`: an addition, an edit with the password typed): which
        // JetKVM answers could not be told, and nothing is taken. Its signalling line is not asked for.
        JetKvmByHand jet;
        jet.deviceFails = true;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("bad_response"));
        QCOMPARE(what.error().message, std::string("Which JetKVM answers at this address could not be told: The "
                                                   "JetKVM did not say which it is (HTTP 500)"));
        QCOMPARE(jet.lineAskings, 0);
        QVERIFY(device.machine().isEmpty());
        // An older one, which has no GET /device, gives none: it is what it is, with no number.
        jet.deviceFails = false;
        jet.hasNoDevicePage = true;
        const auto older = identified(device);
        QVERIFY2(older, older ? "" : older.error().message.c_str());
        QCOMPARE(older.value().number, std::optional<QString>(QString()));
        QVERIFY(device.machine().isEmpty());
    }

    // --- which JetKVM answers (J5, the web's _same_jetkvm and _judge) --------------------------------------------

    void anotherJetKvmAtItsAddressIsSaidAndItsLineIsNotAskedFor_data()
    {
        QTest::addColumn<int>("instead"); // 0: another number; 1: none fit to be one; 2: no GET /device at all
        QTest::newRow("another number") << 0;
        QTest::newRow("none fit to be one") << 1;
        QTest::newRow("an older one with no /device") << 2;
    }

    void anotherJetKvmAtItsAddressIsSaidAndItsLineIsNotAskedFor()
    {
        // After a new lease, another JetKVM answers at the address of the one this driver is known by: asked under the
        // session, after the sign-in, it gives another number (or none, where one is known). "another_machine" in the
        // web's words; its signalling line is not asked for, so no offer goes and no viewer is taken; a look says so;
        // and nothing is taken in place of what is known.
        QFETCH(int, instead);
        JetKvmByHand jet;
        JetKvmDevice::Where where = at(jet);
        where.machine = digestOf(jet.deviceId.toString());
        if (instead == 0) {
            jet.deviceId = u"another-one-0001"_s;
        } else if (instead == 1) {
            jet.deviceId = u"short"_s;
        } else {
            jet.hasNoDevicePage = true;
        }
        JetKvmDevice device(where, u"secret"_s, quick());
        QSignalSpy known(&device, &JetKvmDevice::knownAs);
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("another_machine"));
        QCOMPARE(what.error().message,
                 (instead == 0 ? JetKvmDevice::anotherJetKvm() : JetKvmDevice::noJetKvmNumber()).message);
        QCOMPARE(jet.signIns, 1);
        // (Asked first with no session over plain HTTP, and refused there by one with a password: the merged
        // review, 4.)
        QCOMPARE(jet.asked("/device"), 2);
        QCOMPARE(jet.lineAskings, 0);
        QVERIFY(device.another().has_value());
        const qkvm::core::Status looked = lookedAt(device);
        QCOMPARE(looked.state, u"error"_s);
        QCOMPARE(looked.errorCode, u"another_machine"_s);
        QCOMPARE(looked.errorMessage.toStdString(), what.error().message);
        QCOMPARE(device.machine(), where.machine);
        QTest::qWait(50);
        QCOMPARE(known.size(), 0);
    }

    void theFirstNumberSeenIsTakenAndHeldToFromThen()
    {
        // Where no number is known (a card from a file without it, one added before it was kept): the first one seen is
        // taken, as a certificate is at first use, and said. From then on it is held to.
        JetKvmByHand jet;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        QSignalSpy known(&device, &JetKvmDevice::knownAs);
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        const QString its = digestOf(jet.deviceId.toString());
        QCOMPARE(what.value().number, std::optional<QString>(its));
        QCOMPARE(device.machine(), its);
        QTRY_COMPARE(known.size(), 1);
        QCOMPARE(known.first().at(0).toUrl(), jet.address());
        QCOMPARE(known.first().at(1).toString(), QString());
        QCOMPARE(known.first().at(2).toString(), its);
        jet.deviceId = u"another-one-0002"_s;
        QCOMPARE(codeOf(identified(device)), std::string("another_machine"));
        QCOMPARE(device.machine(), its);
        QCOMPARE(known.size(), 1);
        QCOMPARE(jet.linesIn, 1);
    }

    void aNumberThatCouldNotBeAskedWhereOneIsKnownIsTheAskingsError()
    {
        // Where one is known, a question that came to nothing is no pass: the asking's error, and no line.
        JetKvmByHand jet;
        jet.deviceFails = true;
        JetKvmDevice::Where where = at(jet);
        where.machine = digestOf(jet.deviceId.toString());
        JetKvmDevice device(where, u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("bad_response"));
        QCOMPARE(what.error().message, std::string("The JetKVM did not say which it is (HTTP 500)"));
        QCOMPARE(jet.lineAskings, 0);
        QVERIFY(!device.another().has_value());
        QCOMPARE(device.machine(), where.machine);
    }

    void theRightJetKvmAnsweringAgainEndsIt()
    {
        JetKvmByHand jet;
        const QJsonValue its = jet.deviceId;
        JetKvmDevice::Where where = at(jet);
        where.machine = digestOf(its.toString());
        JetKvmDevice device(where, u"secret"_s, quick());
        jet.deviceId = u"another-one-0003"_s;
        QCOMPARE(codeOf(identified(device)), std::string("another_machine"));
        QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
        jet.deviceId = its;
        const auto back = identified(device);
        QVERIFY2(back, back ? "" : back.error().message.c_str());
        QCOMPARE(back.value().number, std::optional<QString>(where.machine));
        QVERIFY(!device.another().has_value());
        const qkvm::core::Status looked = lookedAt(device);
        QCOMPARE(looked.state, u"online"_s);
        QVERIFY(looked.errorCode.isEmpty());
        QCOMPARE(jet.linesIn, 1);
    }

    void itsNumberIsAskedUnderTheSessionItsLineIsAskedWith()
    {
        // Under the session the line is then asked with: somebody signed in at its own page since, and the number's
        // question is refused (401) as its line would be: signed in to once more, and both asked under the new one.
        JetKvmByHand jet;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        QVERIFY(identified(device));
        jet.signInElsewhere();
        const auto again = identified(device);
        QVERIFY2(again, again ? "" : again.error().message.c_str());
        QCOMPARE(jet.signIns, 2);
        QCOMPARE(jet.lineCookies, (QList<QByteArray>{"authToken=jet.1"_ba, "authToken=jet.3"_ba}));
        QList<QByteArray> numberCookies;
        for (const auto& one : jet.stand->asked) {
            if (one.path == "/device") {
                numberCookies.append(one.headers.value("cookie"));
            }
        }
        // (The third refused: the session somebody else's sign-in replaced. And before each sign-in, over plain HTTP,
        // asked with no session, which one with a password refuses: the merged review, 4.)
        QCOMPARE(numberCookies, (QList<QByteArray>{QByteArray(), "authToken=jet.1"_ba, "authToken=jet.1"_ba,
                                                   QByteArray(), "authToken=jet.3"_ba}));
    }

    void oneWithNoPasswordSavedThatAsksForOneNowMayBeAnother()
    {
        // As the web's _rejected: a JetKVM that let anybody in when it was added, and asks for a password now, may be
        // its own (its owner switched one on) or another: said so, and nothing is sent to it but the question of which
        // it is.
        JetKvmByHand jet;
        JetKvmDevice::Where where = at(jet);
        where.machine = digestOf(jet.deviceId.toString());
        JetKvmDevice device(where, QString(), quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("no_password"));
        QVERIFY(what.error().message.starts_with("The JetKVM at this address asks for a password now, and this card "
                                                 "has none saved: it may be another JetKVM"));
        QCOMPARE(jet.signIns, 0);
        QCOMPARE(jet.lineAskings, 0);
    }

    void aPlainJetKvmIsAskedWhichItIsBeforeItsPasswordGoes_data()
    {
        QTest::addColumn<bool>("itsOwn"); // the card's own JetKVM, now without a password; or another
        QTest::newRow("another, without a password") << false;
        QTest::newRow("its own, now without a password") << true;
    }

    void aPlainJetKvmIsAskedWhichItIsBeforeItsPasswordGoes()
    {
        // The merged review's MEDIUM 4 (2026-11-06), as the web's _login and _number_to_anybody: over plain HTTP the
        // password goes in the clear to whatever answers. After a new lease a JetKVM without a password (out of its
        // box, or set so) answers at the card's address, and says which it is to anybody. Asked that first, with no
        // session: another one is "another_machine" and is sent no password; the card's own is used as one that lets
        // anybody in, with no password sent either.
        QFETCH(bool, itsOwn);
        JetKvmByHand jet;
        jet.password.clear();
        JetKvmDevice::Where where = at(jet);
        where.machine = itsOwn ? digestOf(jet.deviceId.toString()) : digestOf(u"the-cards-own-0001"_s);
        JetKvmDevice device(where, u"secret"_s, quick());
        const auto what = identified(device);
        if (itsOwn) {
            QVERIFY2(what, what ? "" : what.error().message.c_str());
            QCOMPARE(what.value().number, std::optional<QString>(where.machine));
            QCOMPARE(jet.linesIn, 1);
            QCOMPARE(jet.asked("/device"), 2);
            // From then on one that needs no session: asked which it is before its line only (as any connection).
            QVERIFY(identified(device));
            QCOMPARE(jet.asked("/device"), 3);
        } else {
            QCOMPARE(codeOf(what), std::string("another_machine"));
            QCOMPARE(what.error().message, JetKvmDevice::anotherJetKvm().message);
            QCOMPARE(jet.lineAskings, 0);
            // (Judged there: it is asked nothing more.)
            QCOMPARE(jet.asked("/device"), 1);
        }
        QCOMPARE(jet.signIns, 0);
        QVERIFY(jet.signInBodies.isEmpty());
        // (Asked with nothing that signs in.)
        QVERIFY(jet.asked("/device") >= 1);
        for (const auto& one : jet.stand->asked) {
            QVERIFY(one.body.isEmpty());
            QVERIFY(!one.headers.contains("cookie"));
        }
    }

    void aPlainJetKvmWithAPasswordIsSignedInToAfterItsQuestionAsBefore()
    {
        // Where it says none so (one with a password answers 401), the sign-in goes, and its number is asked under the
        // session after it, as before.
        JetKvmByHand jet;
        JetKvmDevice::Where where = at(jet);
        where.machine = digestOf(jet.deviceId.toString());
        JetKvmDevice device(where, u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(jet.signIns, 1);
        QList<QByteArray> numberCookies;
        for (const auto& one : jet.stand->asked) {
            if (one.path == "/device") {
                numberCookies.append(one.headers.value("cookie"));
            }
        }
        QCOMPARE(numberCookies, (QList<QByteArray>{QByteArray(), "authToken=jet.1"_ba}));
        QCOMPARE(jet.stand->asked.first().path, QByteArray("/device"));
    }

    // --- its other way (M6 1b-iii) -----------------------------------------------------------------------

    void aJetKvmOverPlainHttpIsAskedNowAndThenWhetherItOffersHttps()
    {
        // As the web's (jetkvm.py, _look_the_other_way): at a look, and then no more often than wayEveryMs, its
        // other way is asked the JetKVM's question alone, with nothing that says who asks; a JetKVM there is
        // told, with what it showed on that line.
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        JetKvmDevice::Pace pace = quick();
        pace.wayEveryMs = 1500;
        pace.otherWay = [from = plain.address(), to = secure.address()](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        JetKvmDevice device(at(plain), u"secret"_s, pace);
        QList<QList<QUrl>> offered;
        QList<QByteArray> shown;
        QObject::connect(&device, &JetKvmDevice::offersHttps, &device,
                         [&](const QUrl& from, const QUrl& to, const qkvm::core::KvmProbe::Found& found) {
                             offered.append({from, to});
                             shown.append(found.shown);
                         });
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QTRY_COMPARE_WITH_TIMEOUT(offered.size(), 1, 10000);
        QCOMPARE(offered.constFirst(), (QList<QUrl>{plain.address(), secure.address()}));
        QCOMPARE(shown.constFirst(), sha256Of("first"));
        QCOMPARE(secure.asked("/device/status"), 1);
        for (const auto& one : std::as_const(secure.stand->asked)) {
            QCOMPARE(one.path, "/device/status"_ba);
            QVERIFY(one.headers.value("cookie").isEmpty());
        }
        // Looked at again soon: not asked again.
        for (int look = 0; look < 3; ++look) {
            QTest::qWait(pace.freshMs + 50);
            QCOMPARE(lookedAt(device).state, u"online"_s);
        }
        QCOMPARE(secure.asked("/device/status"), 1);
        // Once its pace has gone by: asked again.
        QTest::qWait(pace.wayEveryMs);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QTRY_COMPARE_WITH_TIMEOUT(offered.size(), 2, 10000);
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(secure.signIns, 0);
    }

    void anHttpsJetKvmThatAnswersOnlyThePlainWayIsSaidSoAndNeverFollowed_data()
    {
        QTest::addColumn<QString>("password");
        QTest::newRow("with a password") << u"secret"_s;
        QTest::newRow("without one") << QString();
    }

    void anHttpsJetKvmThatAnswersOnlyThePlainWayIsSaidSoAndNeverFollowed()
    {
        // As the web's (test_a_jetkvm_whose_https_mode_is_switched_off): its HTTPS gone for longer than its
        // patience, the plain way is asked, and a JetKVM there is said on its card, in this program's words
        // (it does not change a KVM from HTTPS to plain HTTP at its place). Never followed there. Answering
        // again, it is said no more.
        QFETCH(QString, password);
        JetKvmByHand plain;
        plain.password = password;
        auto secure = std::make_unique<JetKvmByHand>(tls("first"));
        secure->password = password;
        const quint16 port = secure->stand->serverPort();
        JetKvmDevice::Pace pace = quick();
        pace.holdMs = 50;
        pace.longestHoldMs = 100;
        pace.wayWhenDownMs = 200;
        pace.wayPatienceMs = 600;
        pace.otherWay = [from = secure->address(), to = plain.address()](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        JetKvmDevice::Where where = at(*secure);
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        JetKvmDevice device(where, password, pace);
        int offered = 0;
        QObject::connect(&device, &JetKvmDevice::offersHttps, &device,
                         [&offered](const QUrl&, const QUrl&, const qkvm::core::KvmProbe::Found&) { ++offered; });
        const qkvm::core::Status first = lookedAt(device);
        QVERIFY2(first.state == u"online"_s, qPrintable(first.errorCode + u": "_s + first.errorMessage));
        secure->stand->takeNoMore();
        // Away: offline, and the plain way not asked yet.
        const qkvm::core::Status away = lookedAt(device);
        QCOMPARE(away.state, u"offline"_s);
        QVERIFY(plain.stand->asked.isEmpty());
        // Away for longer than its patience: asked the plain way, and said so.
        QVERIFY(QTest::qWaitFor([&] { return lookedAt(device).errorCode == u"plain_only"_s; }, 20000));
        const qkvm::core::Status said = device.status();
        QCOMPARE(said.state, u"error"_s);
        // (As the web's two sentences: what would travel unencrypted, or that nothing would be checked.)
        QCOMPARE(said.errorMessage,
                 password.isEmpty()
                     ? u"This JetKVM no longer answers over HTTPS, only over plain HTTP. Nothing would be checked or "
                       "encrypted there: to use it that way, take it off this wall and add it again as %1."_s.arg(
                           plain.address().toString())
                     : u"This JetKVM no longer answers over HTTPS, only over plain HTTP. Its password would travel "
                       "unencrypted there: to allow that, take it off this wall and add it again as %1, and type its "
                       "password again."_s.arg(plain.address().toString()));
        QCOMPARE(offered, 0);
        QCOMPARE(plain.signIns, 0);
        for (const auto& one : std::as_const(plain.stand->asked)) {
            QCOMPARE(one.path, "/device/status"_ba);
            QVERIFY(one.headers.value("cookie").isEmpty());
        }
        // Back over HTTPS at its address: online, and plain-only no more.
        JetKvmByHand back(tls("first"), port);
        back.password = password;
        QVERIFY(back.stand->isListening());
        QVERIFY(QTest::qWaitFor([&] { return lookedAt(device).state == u"online"_s; }, 20000));
        QVERIFY(device.status().errorCode.isEmpty());
        QCOMPARE(offered, 0);
        // (What was said of it is over: away once more, it is away, until its patience is over again.)
        back.stand->takeNoMore();
        QCOMPARE(lookedAt(device).state, u"offline"_s);
    }

    void aJetKvmThatIsStartingUpIsNotSaidToAnswerOnlyThePlainWayBeforeItsPatience()
    {
        // In every restart a JetKVM serves plain HTTP at once and HTTPS a little later (the web's WAY_PATIENCE,
        // test_a_jetkvm_that_is_starting_up_is_not_taken_to_have_given_up_https): before it has been away that
        // long, the plain way is not asked; after that, no more often than wayWhenDownMs.
        JetKvmByHand plain;
        plain.password.clear();
        JetKvmByHand secure(tls("first"));
        secure.password.clear();
        JetKvmDevice::Pace pace = quick();
        pace.holdMs = 50;
        pace.longestHoldMs = 100;
        pace.wayWhenDownMs = 1500;
        pace.wayPatienceMs = 2500;
        pace.otherWay = [from = secure.address(), to = plain.address()](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        JetKvmDevice::Where where = at(secure);
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        JetKvmDevice device(where, QString(), pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        secure.stand->takeNoMore();
        QElapsedTimer sinceDown;
        sinceDown.start();
        QCOMPARE(lookedAt(device).state, u"offline"_s);
        while (sinceDown.elapsed() < 2000) {
            QCOMPARE(lookedAt(device).errorCode == u"plain_only"_s, false);
            QTest::qWait(50);
        }
        QCOMPARE(plain.asked("/device/status"), 0);
        // Looked at often: asked once its patience is over, and no more often than its pace from then on.
        while (sinceDown.elapsed() < 8000) {
            lookedAt(device);
            QTest::qWait(50);
        }
        const int asked = plain.asked("/device/status");
        const qint64 most = (sinceDown.elapsed() - pace.wayPatienceMs) / pace.wayWhenDownMs + 1;
        QVERIFY2(asked >= 1 && asked <= most,
                 (QByteArray::number(asked) + " of " + QByteArray::number(most)).constData());
        QCOMPARE(device.status().errorCode, u"plain_only"_s);
    }

    void itsPasswordGoesAsJsonAndItsOneSessionIsKept()
    {
        JetKvmByHand jet;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(what.value().model, u"JetKVM"_s);
        QCOMPARE(what.value().version, u"0.4.8"_s);
        QCOMPARE(what.value().number, std::optional<QString>(digestOf(jet.deviceId.toString())));
        QCOMPARE(jet.signInBodies, QList<QByteArray>{R"({"password":"secret"})"_ba});
        // Its signalling line and its number, asked with the session it gave.
        QCOMPARE(jet.lineCookies, QList<QByteArray>{"authToken=jet.1"_ba});
        QCOMPARE(jet.linesIn, 1);
        // (Over plain HTTP its number asked first with no session, answered 401 by one with a password: the merged
        // review, 4.)
        QCOMPARE(jet.asked("/device"), 2);
        // Asked again: with the session it has (a sign-in would sign out whoever is signed in to it).
        QVERIFY(identified(device));
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(jet.lineCookies.last(), "authToken=jet.1"_ba);
    }

    void aJetKvmWithNoPasswordSavedIsAskedWithNothing()
    {
        JetKvmByHand jet;
        jet.password.clear();
        JetKvmDevice device(at(jet), QString(), quick());
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(what.value().version, u"0.4.8"_s);
        QCOMPARE(what.value().number, std::optional<QString>(digestOf(jet.deviceId.toString())));
        QCOMPARE(jet.signIns, 0);
        QCOMPARE(jet.lineCookies, QList<QByteArray>{"(none)"_ba});
    }

    void aJetKvmThatRunsWithoutAPasswordTakesNoneThatIsSaved()
    {
        // Nothing is needed, and nothing is sent to say who asks. Over plain HTTP it says which it is to anybody, asked
        // first: the password saved is not sent at all (it was, and it answered 400, before the merged review, 4).
        JetKvmByHand jet;
        jet.password.clear();
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        QVERIFY(identified(device));
        QVERIFY(identified(device));
        QCOMPARE(jet.signIns, 0);
        QCOMPARE(jet.lineCookies, (QList<QByteArray>{"(none)"_ba, "(none)"_ba}));
    }

    void aJetKvmWithNoPasswordSavedThatAsksForOneSaysSo()
    {
        JetKvmByHand jet;
        JetKvmDevice device(at(jet), QString(), quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("no_password"));
        QCOMPARE(what.error().message, std::string("This JetKVM asks for a password and none is saved"));
        QCOMPARE(jet.signIns, 0);
    }

    void aRefusedPasswordIsSaidInTheJetKvmsWordsAndIsNotSentAgainForAWhile()
    {
        JetKvmByHand jet;
        const JetKvmDevice::Pace pace = quick();
        JetKvmDevice device(at(jet), u"not it"_s, pace);
        const auto refused = identified(device);
        QCOMPARE(codeOf(refused), std::string("auth_failed"));
        QCOMPARE(refused.error().message, std::string("Login rejected - check the password"));
        QCOMPARE(pace.pacing->times(SignInPacing::what(u"login", jet.address(), QString(), u"not it"_s)), 1);
        QCOMPARE(codeOf(identified(device)), std::string("auth_failed"));
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(jet.lineAskings, 0);
        // A driver made anew waits as well.
        JetKvmDevice anew(at(jet), u"not it"_s, pace);
        QCOMPARE(codeOf(identified(anew)), std::string("auth_failed"));
        QCOMPARE(jet.signIns, 1);
        // Typed afresh by a person, it is sent at once.
        jet.password = u"not it"_s;
        JetKvmDevice::tryAfresh(at(jet), u"not it"_s, pace);
        QVERIFY(identified(anew));
        QCOMPARE(jet.signIns, 2);
    }

    void aJetKvmThatWasNeverSetUpSaysSoForAsLongAsItIsHeld_data()
    {
        QTest::addColumn<QString>("password");
        QTest::newRow("a password saved") << u"secret"_s;
        QTest::newRow("none saved") << QString();
    }

    void aJetKvmThatWasNeverSetUpSaysSoForAsLongAsItIsHeld()
    {
        // Out of its box it lets nobody in until somebody has chosen, at its own page, how it is signed in to.
        QFETCH(QString, password);
        JetKvmByHand jet;
        jet.isSetup = false;
        JetKvmDevice device(at(jet), password, quick());
        const auto refused = identified(device);
        QCOMPARE(codeOf(refused), std::string("not_set_up"));
        QCOMPARE(refused.error().message, std::string("This JetKVM has not been set up yet. Open its own page once and "
                                                      "choose how it is signed in to; then it can be added here."));
        // Asked again: said so again, and not that its password is wrong; nothing sent for it meanwhile.
        QCOMPARE(codeOf(identified(device)), std::string("not_set_up"));
        QCOMPARE(jet.signIns, password.isEmpty() ? 0 : 1);
        // (Whether it was set up is asked with no session.)
        QVERIFY(jet.asked("/device/status") > 0);
        for (const auto& one : jet.stand->asked) {
            QVERIFY(one.path != "/device/status" || !one.headers.contains("cookie"));
        }
    }

    void aJetKvmThatShutsTheAddressOutAfterANoSaysSoInItsWords()
    {
        // A no, and once that wait is over, the address shut out (429): said as what stands, not as a no.
        JetKvmByHand jet;
        qint64 nowMs = 1'000'000;
        JetKvmDevice::Pace pace = quick();
        pace.pacing = std::make_shared<SignInPacing>(2048, [&nowMs] { return nowMs; });
        JetKvmDevice device(at(jet), u"not it"_s, pace);
        QCOMPARE(identified(device).error().message, std::string("Login rejected - check the password"));
        nowMs += pace.refusedHoldMs;
        Answered shut(429, R"({"error":"Too many requests"})");
        shut.more = {"Retry-After: 120"};
        jet.signInInstead = shut;
        const std::string refusing = "The JetKVM is refusing logins after too many wrong passwords - try again later";
        QCOMPARE(identified(device).error().message, refusing);
        QCOMPARE(identified(device).error().message, refusing);
        QCOMPARE(jet.signIns, 2);
    }

    void aSignInTheJetKvmHangsUpOnIsHeldBackAsNoWordOnThePassword()
    {
        JetKvmByHand jet;
        Answered gone;
        gone.hangUp = true;
        jet.signInInstead = gone;
        JetKvmDevice::Pace pace = quick();
        pace.signInRetryMs = 3000; // (room for the second asking within the hold)
        JetKvmDevice device(at(jet), u"secret"_s, pace);
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("no_sign_in"));
        QCOMPARE(what.error().message,
                 std::string("The device does not sign qKVM in (The device closed the connection). It is asked again "
                             "shortly."));
        QCOMPARE(codeOf(identified(device)), std::string("no_sign_in"));
        QCOMPARE(jet.signIns, 1);
    }

    void aSignallingLineThatSaysNothingIsAwayAndLeftAlone()
    {
        JetKvmByHand jet;
        jet.linesSayNothing = true;
        JetKvmDevice::Pace pace = quick();
        pace.lineWithinMs = 800;
        pace.holdMs = 3000;
        pace.longestHoldMs = 3000;
        JetKvmDevice device(at(jet), u"secret"_s, pace);
        QCOMPARE(codeOf(identified(device)), std::string("timeout"));
        // Away for a while on the road that connects, as any JetKVM that is away: its line is not asked for meanwhile.
        const int lines = jet.lineAskings;
        bool came = false;
        qkvm::base::Result<qkvm::core::DeviceLine*> held = qkvm::base::Error{"never", "Never answered."};
        device.openSignalling([&came, &held](const qkvm::base::Result<qkvm::core::DeviceLine*>& opened) {
            held = opened;
            came = true;
        });
        QTRY_VERIFY(came);
        QVERIFY(!held);
        QCOMPARE(held.error().code, std::string("timeout"));
        QCOMPARE(jet.lineAskings, lines);
        // And a look, which nothing of that road holds (the web's probe, from the second reading of c7ec58c, A: it was
        // held, and said "offline" for a JetKVM whose HTTP answers), asks the plain way, and says what it found.
        const int asked = jet.asked("/device/status");
        const auto status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(status.errorCode.isEmpty());
        QCOMPARE(jet.asked("/device/status"), asked + 1);
    }

    void aWordBeforeItsVersionIsLetGo()
    {
        JetKvmByHand jet;
        jet.saysAWordFirst = true;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(what.value().version, u"0.4.8"_s);
    }

    void aJetKvmThatShutsTheAddressOutIsLeftAloneForAsLongAsItSays_data()
    {
        QTest::addColumn<QByteArray>("retryAfter");
        QTest::addColumn<qint64>("waitMs");
        // (quick(): the first wait after a no is 1.2 s; an hour at the most, whatever it says.)
        QTest::newRow("two minutes") << "120"_ba << qint64{120'000};
        QTest::newRow("less than the first wait after a no") << "0.5"_ba << qint64{1200};
        QTest::newRow("no number") << "soon"_ba << qint64{1200};
        QTest::newRow("for ever") << "inf"_ba << qint64{3'600'000};
        QTest::newRow("more than an hour") << "86400"_ba << qint64{3'600'000};
    }

    void aJetKvmThatShutsTheAddressOutIsLeftAloneForAsLongAsItSays()
    {
        QFETCH(QByteArray, retryAfter);
        QFETCH(qint64, waitMs);
        JetKvmByHand jet;
        Answered shut(429, R"({"error":"Too many requests"})");
        shut.more = {"Retry-After: " + retryAfter};
        jet.signInInstead = shut;
        qint64 nowMs = 1'000'000;
        JetKvmDevice::Pace pace = quick();
        pace.pacing = std::make_shared<SignInPacing>(2048, [&nowMs] { return nowMs; });
        JetKvmDevice device(at(jet), u"secret"_s, pace);
        const auto refused = identified(device);
        QCOMPARE(codeOf(refused), std::string("auth_failed"));
        QCOMPARE(refused.error().message,
                 std::string("The JetKVM is refusing logins after too many wrong passwords - try again later"));
        const auto mark = SignInPacing::what(u"login", jet.address(), QString(), u"secret"_s);
        QCOMPARE(pace.pacing->waitLeftMs(mark), waitMs);
        // No word on the password.
        QCOMPARE(pace.pacing->times(mark), 0);
        // Said so again meanwhile, in its own words, and nothing sent.
        QCOMPARE(identified(device).error().message,
                 std::string("The JetKVM is refusing logins after too many wrong passwords - try again later"));
        QCOMPARE(jet.signIns, 1);
    }

    void aSignInThatLetsItNowhereIsNoWordOnThePassword_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("cookie");
        QTest::newRow("an error of its own") << 500 << QByteArray();
        QTest::newRow("in, with no pass") << 200 << QByteArray();
        QTest::newRow("in, with a pass that is none") << 200 << "authToken=a pass\r\nX: 1"_ba;
        QTest::newRow("in, with two passes") << 200 << "authToken=jet.a\r\nSet-Cookie: authToken=jet.b"_ba;
    }

    void aSignInThatLetsItNowhereIsNoWordOnThePassword()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, cookie);
        JetKvmByHand jet;
        Answered nowhere(status, "{}");
        if (!cookie.isEmpty()) {
            nowhere.more = {"Set-Cookie: " + cookie};
        }
        jet.signInInstead = nowhere;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("no_sign_in"));
        QCOMPARE(what.error().message,
                 u"The device does not sign qKVM in (Login failed (HTTP %1)). It is asked again shortly."_s.arg(status)
                     .toStdString());
        QCOMPARE(jet.lineAskings, 0);
    }

    void aSessionTakenAtItsOwnPageIsSignedInToOnceMore()
    {
        JetKvmByHand jet;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        QVERIFY(identified(device));
        jet.signInElsewhere();
        const auto again = identified(device);
        QVERIFY2(again, again ? "" : again.error().message.c_str());
        QCOMPARE(jet.signIns, 2);
        // (The session it had is refused, and the one it signs in for then is taken. Refused since J5 by the question
        // of which JetKVM it is, which goes first under it: its line is asked for under the new one alone.)
        QCOMPARE(jet.lineCookies, (QList<QByteArray>{"authToken=jet.1"_ba, "authToken=jet.3"_ba}));
    }

    void itsHttpsIsLookedAtBeforeEverySignInWhereThatIsAsked()
    {
        // From the reading of 2g/2h (M1): where the wall asks it (lookAtItsHttpsBeforeEverySignIn), a plain JetKVM's
        // HTTPS at its own host is looked at on every sign-in of its driver, before its password goes in the clear:
        // the first, and the one anew where its session is refused (the viewer's road). Where what that shows is
        // refused (the wall's word: another KVM of the wall is known by that certificate), nothing goes, the sign-in
        // is that refusal, and what was shown is said (httpsShowed).
        JetKvmByHand jet;
        JetKvmByHand secure(tls("first"));
        const qkvm::core::testing::NobodyThere nobody;
        bool twinThere = false;
        JetKvmDevice::Pace pace = quick();
        pace.otherWay = [from = jet.address(), to = secure.address(),
                         off = QUrl(u"https://127.0.0.1:%1"_s.arg(nobody.port())), &twinThere](const QUrl& address) {
            return address == from ? std::optional<QUrl>(twinThere ? to : off) : std::nullopt;
        };
        JetKvmDevice device(at(jet), u"secret"_s, pace);
        QList<QByteArray> looked;
        QList<QUrl> from;
        device.lookAtItsHttpsBeforeEverySignIn(
            [&looked, &from](const QUrl& where,
                             const qkvm::core::KvmProbe::Found& found) -> std::optional<base::Error> {
                looked.append(found.shown);
                from.append(where);
                if (found.shown == sha256Of("first")) {
                    return base::Error{"same_device", "This is the same device as another of the wall"};
                }
                return std::nullopt;
            });
        const QSignalSpy showed(&device, &JetKvmDevice::httpsShowed);
        QVERIFY(identified(device));
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(looked.size(), 1);
        QVERIFY(showed.isEmpty());
        // Its session taken elsewhere, and its address handed to another of the wall meanwhile.
        twinThere = true;
        jet.signInElsewhere();
        const auto refused = identified(device);
        QCOMPARE(codeOf(refused), std::string("same_device"));
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(secure.signIns, 0);
        QCOMPARE(looked.size(), 2);
        QCOMPARE(looked.constLast(), sha256Of("first"));
        // (Handed where the driver speaks to it: the plain address whose own HTTPS that is.)
        QCOMPARE(from, (QList<QUrl>{jet.address(), jet.address()}));
        QCOMPARE(showed.size(), 1);
        // Nothing asked of it, nothing looked at: signed in as ever.
        JetKvmDevice plainly(at(jet), u"secret"_s, pace);
        QVERIFY(identified(plainly));
        QCOMPARE(jet.signIns, 2);
    }

    void aSessionThatIsRefusedOnceMoreIsSaidAndNotAskedForAWhile()
    {
        JetKvmByHand jet;
        jet.refuseLines = 2;
        JetKvmDevice::Pace pace = quick();
        pace.signInRetryMs = 3000; // (room for three askings and a driver made anew within the hold)
        JetKvmDevice device(at(jet), u"secret"_s, pace);
        const auto refused = identified(device);
        QCOMPARE(codeOf(refused), std::string("auth_failed"));
        QCOMPARE(refused.error().message, std::string("The JetKVM keeps rejecting the session"));
        QCOMPARE(jet.signIns, 2);
        QCOMPARE(jet.lineAskings, 2);
        QCOMPARE(codeOf(identified(device)), std::string("auth_failed"));
        QCOMPARE(jet.signIns, 2);
        QCOMPARE(jet.lineAskings, 2);
        // A driver made anew waits as well: the hold is not this driver's own.
        JetKvmDevice anew(at(jet), u"secret"_s, pace);
        QCOMPARE(codeOf(identified(anew)), std::string("no_sign_in"));
        QCOMPARE(jet.signIns, 2);
        // After a while, asked again.
        QTest::qWait(pace.signInRetryMs + 100);
        QVERIFY(identified(device));
    }

    void aSessionRefusedOnceMoreIsHeldBeforeTheJetKvmIsAskedAnythingElse()
    {
        // Held as soon as its line refuses the session a second time: a driver let go of while it asks whether
        // the JetKVM was set up leaves the hold behind, and the one made in its place does not sign in at once.
        JetKvmByHand jet;
        jet.refuseLines = 2;
        // (Slow only where it is asked whether it was set up after the refusal, the first time it is asked that: over
        // plain HTTP what is asked first, before each sign-in, is its number, since the merged review's 4.)
        jet.stand->beforeAnswer = [&jet] {
            if (jet.asked("/device/status") == 1) {
                jet.statusAfterMs = 1500;
            }
        };
        JetKvmDevice::Pace pace = quick();
        pace.signInRetryMs = 5000;
        {
            JetKvmDevice before(at(jet), u"secret"_s, pace);
            before.identify([](const base::Result<JetKvmDevice::Identity>&) {});
            QTRY_COMPARE(jet.asked("/device/status"), 1);
        }
        JetKvmDevice anew(at(jet), u"secret"_s, pace);
        QCOMPARE(codeOf(identified(anew)), std::string("no_sign_in"));
        QCOMPARE(jet.signIns, 2);
        QCOMPARE(jet.lineAskings, 2);
        jet.stand->beforeAnswer = nullptr;
    }

    void aNoHeardWhileARefusedSessionIsAskedAboutKeepsItsOwnWords()
    {
        // One asking's session is refused once more, and while it asks whether the JetKVM was set up (a slow
        // answer) the hold runs out, and another asking of the same driver signs in and is refused its
        // password. That no keeps its own words: the first asking's sentence is for the hold it noted, which
        // is not the one that stands any more.
        JetKvmByHand jet;
        jet.refuseLines = 2;
        jet.stand->beforeAnswer = [&jet] { // (as above: slow only after the refusal)
            if (jet.asked("/device/status") == 1) {
                jet.statusAfterMs = 1200;
            }
        };
        JetKvmDevice::Pace pace = quick();
        pace.signInRetryMs = 300;
        JetKvmDevice device(at(jet), u"secret"_s, pace);
        QList<base::Result<JetKvmDevice::Identity>> first;
        device.identify([&first](const base::Result<JetKvmDevice::Identity>& what) { first.append(what); });
        QTRY_COMPARE(jet.asked("/device/status"), 1);
        jet.password = u"another"_s;
        QTest::qWait(pace.signInRetryMs + 100);
        QVERIFY(first.isEmpty());
        QList<base::Result<JetKvmDevice::Identity>> second;
        device.identify([&second](const base::Result<JetKvmDevice::Identity>& what) { second.append(what); });
        QTRY_COMPARE(first.size(), 1);
        QTRY_COMPARE(second.size(), 1);
        QCOMPARE(jet.signIns, 3);
        QCOMPARE(first.constFirst().error().message, std::string("The JetKVM keeps rejecting the session"));
        QCOMPARE(second.constFirst().error().message, std::string("Login rejected - check the password"));
        jet.stand->beforeAnswer = nullptr;
    }

    void aNoKeepsItsWordsWhileAnotherAskingsSessionIsRefusedMeanwhile()
    {
        // (Carried in from increment 1's readings.) One asking signs in once more and is refused its password, and
        // while it asks whether the JetKVM was set up (a slow answer), another asking's session is refused once more
        // and noted as a hold of its own. The first asking's waiters are told what came of their own sign-in, the
        // password refused, and not the other asking's words. (Before: they were told whatever words stood by the
        // time the JetKVM answered, "keeps rejecting the session".)
        JetKvmByHand jet;
        jet.refuseLines = 3;
        jet.lineRefusedAfterMs = {0, 1000, 0}; // the second asking's second line is refused a second late
        jet.stand->beforeAnswer = [&jet] {     // (slow only once the third sign-in, the refused one, went out)
            if (jet.signIns >= 3) {
                jet.statusAfterMs = 1500;
            }
        };
        JetKvmDevice::Pace pace = quick();
        pace.signInRetryMs = 5000;
        JetKvmDevice device(at(jet), u"secret"_s, pace);
        QList<base::Result<JetKvmDevice::Identity>> other;
        device.identify([&other](const base::Result<JetKvmDevice::Identity>& what) { other.append(what); });
        QTRY_COMPARE(jet.lineAskings, 2);
        QCOMPARE(jet.signIns, 2);
        jet.password = u"another"_s;
        QList<base::Result<JetKvmDevice::Identity>> first;
        device.identify([&first](const base::Result<JetKvmDevice::Identity>& what) { first.append(what); });
        QTRY_COMPARE_WITH_TIMEOUT(first.size(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(other.size(), 1, 10000);
        QCOMPARE(jet.signIns, 3);
        QCOMPARE(jet.lineAskings, 3);
        QCOMPARE(first.constFirst().error().message, std::string("Login rejected - check the password"));
        QCOMPARE(other.constFirst().error().message, std::string("The JetKVM keeps rejecting the session"));
        jet.stand->beforeAnswer = nullptr;
    }

    void aVersionNotSaidInTimeIsNone()
    {
        JetKvmByHand jet;
        JetKvmDevice::Pace pace = quick();
        pace.versionWithinMs = 300;
        JetKvmDevice device(at(jet), u"secret"_s, pace);
        QCOMPARE(identified(device).value().version, u"0.4.8"_s);
        jet.saysItsVersion = false;
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QVERIFY(what.value().version.isEmpty());
        QCOMPARE(what.value().number, std::optional<QString>(digestOf(jet.deviceId.toString())));
        // What it said before is the version it is known to run: a look goes on saying that.
        QCOMPARE(lookedAt(device).version, u"0.4.8"_s);
    }

    void whatIsNoNumberFitToKnowItByIsNone_data()
    {
        QTest::addColumn<QJsonValue>("given");
        QTest::addColumn<bool>("fit");
        QTest::newRow("its own") << QJsonValue(u"9a8b7c6d5e4f3a2b"_s) << true;
        QTest::newRow("eight signs") << QJsonValue(u"9a8b7c6d"_s) << true;
        QTest::newRow("too short") << QJsonValue(u"9a8b7c6"_s) << false;
        QTest::newRow("too long") << QJsonValue(QString(65, u'9') + u"abc"_s) << false;
        QTest::newRow("too few signs") << QJsonValue(u"abababababab"_s) << false;
        QTest::newRow("a number") << QJsonValue(123456789) << false;
        QTest::newRow("none") << QJsonValue() << false;
        // (Counted in signs: four beyond the first 65536 are four, though written in eight halves.)
        const std::array<char32_t, 4> far{0x1F600, 0x1F601, 0x1F602, 0x1F923};
        QTest::newRow("four signs beyond the first plane")
            << QJsonValue(QString::fromUcs4(far.data(), far.size())) << false;
    }

    void whatIsNoNumberFitToKnowItByIsNone()
    {
        QFETCH(QJsonValue, given);
        QFETCH(bool, fit);
        JetKvmByHand jet;
        jet.deviceId = given;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(what.value().number, std::optional<QString>(fit ? digestOf(given.toString()) : QString()));
        // (Nothing of the number itself is kept.)
        QVERIFY(given.toString().isEmpty() || !what.value().number.value_or(QString()).contains(given.toString()));
    }

    void aLookAsksWhatAnybodyMayAndTakesNobodysPicture()
    {
        JetKvmByHand jet;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        const auto status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QCOMPARE(status.model, u"JetKVM"_s);
        QCOMPARE(jet.asked("/device/status"), 1);
        QCOMPARE(jet.signIns, 0);
        QCOMPARE(jet.lineAskings, 0);
        QVERIFY(!jet.stand->asked.first().headers.contains("cookie"));
        // Its version, once it said it.
        QVERIFY(identified(device));
        QCOMPARE(lookedAt(device).version, u"0.4.8"_s);
        // (Every look and every question whether it is set up: with no session, after one was given too.)
        for (const auto& one : jet.stand->asked) {
            QVERIFY(one.path != "/device/status" || !one.headers.contains("cookie"));
        }
    }

    void aJetKvmThatIsNotThereIsOfflineAndLeftAloneForAWhile()
    {
        const qkvm::core::testing::NobodyThere nobody;
        JetKvmDevice::Where where;
        where.address = QUrl(nobody.address());
        JetKvmDevice device(where, u"secret"_s, quick());
        const auto status = lookedAt(device);
        QCOMPARE(status.state, u"offline"_s);
        QCOMPARE(status.errorCode, u"unreachable"_s);
        QElapsedTimer watch;
        watch.start();
        QCOMPARE(lookedAt(device).errorCode, u"unreachable"_s);
        QVERIFY(watch.elapsed() < quick().holdMs);
    }

    // What a JetKVM cannot do is refused before anything goes to it, as the web's act refuses it (what it does is
    // test_jetkvm_actions.cpp and test_jetkvm_keys.cpp; its picture its viewer's, test_jetkvm_live.cpp).
    void whatAJetKvmCannotDoIsRefusedBeforeAnythingGoes()
    {
        JetKvmByHand jet;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        for (const char* kind : {"locate_on", "power_on", "port", "fingerbot_power"}) {
            const auto done = answerTo<qkvm::core::Done>(
                [&](auto then) { device.act(qkvm::core::Action::plain(QString::fromLatin1(kind)), std::move(then)); });
            QCOMPARE(done.error().code, std::string("invalid"));
        }
        QVERIFY(jet.stand->asked.isEmpty());
    }

    void overHttpsItIsKnownByItsCertificateAndHearsNothingElse()
    {
        JetKvmByHand jet(tls("first"));
        JetKvmDevice::Where where = at(jet);
        where.knownByCertificate = true;
        where.certificate = StandIn::certificateIn(tls("second").certificateFile).digest(QCryptographicHash::Sha256);
        JetKvmDevice::Pace pace = quick();
        pace.connectWithinMs = 5000; // (a TLS handshake under the address sanitizer on a busy PC)
        JetKvmDevice other(where, u"secret"_s, pace);
        QCOMPARE(codeOf(identified(other)), std::string("cert_mismatch"));
        QVERIFY(jet.signInBodies.isEmpty());
        where.certificate = StandIn::certificateIn(tls("first").certificateFile).digest(QCryptographicHash::Sha256);
        JetKvmDevice known(where, u"secret"_s, pace);
        const auto what = identified(known);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(jet.linesIn, 1);
    }

    void onlyWhatGotNoAnswerInTimeMayHaveBeenDone()
    {
        // (A JetKVM's "dropped" was not sent: what goes to it goes on a line that is there.)
        QVERIFY(JetKvmDevice::mayHaveBeenDone(base::Error{"timeout", "Device did not respond in time"}));
        for (const char* code : {"dropped", "unreachable", "cert_mismatch", "no_sign_in", "not_yet", "auth_failed"}) {
            QVERIFY2(!JetKvmDevice::mayHaveBeenDone(base::Error{code, "said"}), code);
        }
    }

    void aDriverLetGoOfWhileItsSignInIsOutLeavesItsMark()
    {
        // (As a kvmd's.) Its line outlives it and hears what it comes to: here nothing, the line giving up at its own
        // end. The driver made anew sends nothing meanwhile, is not said to be in error, and is then told the JetKVM
        // does not sign it in: nothing more went.
        JetKvmByHand jet;
        Answered silent;
        silent.never = true;
        jet.signInInstead = silent;
        const JetKvmDevice::Pace pace = quick();
        QElapsedTimer watch;
        {
            JetKvmDevice before(at(jet), u"secret"_s, pace);
            before.identify([](const base::Result<JetKvmDevice::Identity>&) {});
            QTRY_COMPARE(jet.signIns, 1);
            watch.start();
        }
        JetKvmDevice anew(at(jet), u"secret"_s, pace);
        QList<std::string> codes;
        anew.identify([&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); });
        QTest::qWait(2000);
        QVERIFY(codes.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 1, 15000);
        QCOMPARE(codes, QList<std::string>{"no_sign_in"});
        QVERIFY(watch.elapsed() >= pace.quietMs * 5 / 2 - 500); // (at its line's end: nothing heard for its wait)
        QCOMPARE(jet.signIns, 1);
    }

    void aDriverThatWaitsForOneLetGoOfIsLetInWhenTheJetKvmAnswersIt()
    {
        // (The pacing's second and third readings, M1, as a kvmd's.) The Edit box signs in slowly, the wall's driver
        // waits, Cancel lets the box's driver go: its line hears the yes, and the wall's driver signs in itself then,
        // not when that sign-in could no longer be out.
        JetKvmByHand jet;
        jet.signInAfterMs = 1000;
        const JetKvmDevice::Pace pace = quick();
        auto box = std::make_unique<JetKvmDevice>(at(jet), u"secret"_s, pace);
        JetKvmDevice wall(at(jet), u"secret"_s, pace);
        box->identify([](const base::Result<JetKvmDevice::Identity>&) {});
        QTRY_COMPARE(jet.signIns, 1);
        QElapsedTimer watch;
        watch.start();
        QList<std::string> codes;
        wall.identify([&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); });
        QTest::qWait(200);
        box.reset();
        QTest::qWait(400);
        QVERIFY(codes.isEmpty());
        QCOMPARE(jet.signIns, 1);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 1, 15000);
        QCOMPARE(codes, QList<std::string>{"ok"});
        QCOMPARE(jet.signIns, 2);
        QVERIFY2(watch.elapsed() < 4000, QByteArray::number(watch.elapsed()));
    }

    void aSignInHeldOpenPastItsTimeHoldsTheOtherToItsLinesEnd()
    {
        // (The pacing's third reading, M1, as a kvmd's.) Out until its line ends, whatever the pacing's clock says:
        // here the JetKVM sends its answer a byte at a time, and the clock is past the time the sign-in could be out. A
        // JetKVM locks out after five: no second strike while the first is on its line.
        JetKvmByHand jet;
        jet.password = u"changed on the JetKVM"_s;
        Answered trickles(401, QByteArray(10'000, 'x'));
        trickles.byteEveryMs = 100;
        jet.signInInstead = trickles;
        qint64 nowMs = 1'000'000;
        JetKvmDevice::Pace pace = quick();
        pace.pacing = std::make_shared<SignInPacing>(2048, [&nowMs] { return nowMs; });
        JetKvmDevice one(at(jet), u"secret"_s, pace);
        JetKvmDevice other(at(jet), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); };
        one.identify(keep);
        QTRY_COMPARE(jet.signIns, 1);
        nowMs += SignInPacing::outFor(pace.connectWithinMs, pace.quietMs * 5 / 2, pace.transferMs) + 1000;
        other.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 20000);
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(codes, (QList<std::string>{"no_sign_in", "no_sign_in"}));
    }

    void aDriverLetGoOfAfterSignInsThatCameToNothingHoldsNobodyOnceItIsAnswered()
    {
        // (The pacing's third reading, M2, as a kvmd's.) After a run of sign-ins that came to nothing, the hold the
        // next went out with is long; its driver let go of, its line hears the yes, and the box with the same password
        // typed waits for that alone.
        JetKvmByHand jet;
        jet.signInAfterMs = 1000;
        qint64 nowMs = 1'000'000;
        JetKvmDevice::Pace pace = quick();
        pace.pacing = std::make_shared<SignInPacing>(2048, [&nowMs] { return nowMs; });
        const SignInPacing::Mark mark = SignInPacing::what(u"login", jet.address(), QString(), u"secret"_s);
        const SignInPacing::Steps steps{
            pace.refusedHoldMs, pace.signInRetryMs,
            SignInPacing::outFor(pace.connectWithinMs, pace.quietMs * 5 / 2, pace.transferMs)};
        for (int one = 0; one < 5; ++one) {
            pace.pacing->unanswered(mark, steps, pace.pacing->sent(mark, steps));
            nowMs += 600'000;
        }
        auto wall = std::make_unique<JetKvmDevice>(at(jet), u"secret"_s, pace);
        wall->identify([](const base::Result<JetKvmDevice::Identity>&) {});
        QTRY_COMPARE(jet.signIns, 1);
        QCOMPARE(pace.pacing->waitLeftMs(mark), 30 * pace.signInRetryMs);
        wall.reset();
        JetKvmDevice::tryAfresh(at(jet), u"secret"_s, pace);
        JetKvmDevice box(at(jet), u"secret"_s, pace);
        QList<std::string> codes;
        box.identify([&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); });
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 1, 10000);
        QCOMPARE(codes, QList<std::string>{"ok"});
        QCOMPARE(jet.signIns, 2);
    }

    void threeDriversSignInOneAtATimeAndEachIsLetIn()
    {
        // (The pacing's second reading, L3, as a kvmd's.) The one told second of two that waited waits again for the
        // one that went first, rather than being told the JetKVM does not sign it in.
        JetKvmByHand jet;
        jet.signInAfterMs = 1000;
        const JetKvmDevice::Pace pace = quick();
        JetKvmDevice one(at(jet), u"secret"_s, pace);
        JetKvmDevice two(at(jet), u"secret"_s, pace);
        JetKvmDevice three(at(jet), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); };
        one.identify(keep);
        QTRY_COMPARE(jet.signIns, 1);
        two.identify(keep);
        three.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 3, 20000);
        QCOMPARE(codes, (QList<std::string>{"ok", "ok", "ok"}));
        QCOMPARE(jet.signIns, 3);
    }

    void aWaiterToldAsAnotherHasJustSentWaitsAgain()
    {
        // (The pacing's second reading, L3, as a kvmd's.) Told at a yes, it finds another's sign-in out by the time it
        // looks: it waits again for that one, within its own time, and is then let in.
        JetKvmByHand jet;
        const JetKvmDevice::Pace pace = quick();
        const SignInPacing::Mark mark = SignInPacing::what(u"login", jet.address(), QString(), u"secret"_s);
        const SignInPacing::Steps steps{
            pace.refusedHoldMs, pace.signInRetryMs,
            SignInPacing::outFor(pace.connectWithinMs, pace.quietMs * 5 / 2, pace.transferMs)};
        pace.pacing->sent(mark, steps); // (another driver's, out)
        JetKvmDevice wall(at(jet), u"secret"_s, pace);
        QList<std::string> codes;
        wall.identify([&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); });
        QTest::qWait(300);
        QVERIFY(codes.isEmpty());
        // Its yes, and at once another's goes out, before the one that waits is told.
        pace.pacing->accepted(mark);
        pace.pacing->sent(mark, steps);
        QTest::qWait(300);
        QVERIFY(codes.isEmpty());
        QCOMPARE(jet.signIns, 0);
        pace.pacing->accepted(mark);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 1, 10000);
        QCOMPARE(codes, QList<std::string>{"ok"});
        QCOMPARE(jet.signIns, 1);
    }

    void aPasswordTypedAnewWhileTheSameIsOutWaitsForIt()
    {
        // (The pacing's second reading, M2.) The wall's driver has the wrong password out; a person types the same one
        // in the Edit box and saves (tryAfresh): the box waits for the one that is out. A JetKVM locks out after five:
        // one strike from that save, not two.
        JetKvmByHand jet;
        jet.password = u"changed on the JetKVM"_s;
        jet.signInAfterMs = 1500;
        const JetKvmDevice::Pace pace = quick();
        JetKvmDevice wall(at(jet), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); };
        wall.identify(keep);
        QTRY_COMPARE(jet.signIns, 1);
        JetKvmDevice::tryAfresh(at(jet), u"secret"_s, pace);
        JetKvmDevice box(at(jet), u"secret"_s, pace);
        box.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(codes, (QList<std::string>{"auth_failed", "auth_failed"}));
    }

    void aSignInThatNothingAnsweredIsHeldAsSuchAndNotAsLongAsItWentOutWith()
    {
        // What it came to replaces the hold it went out with (as long as it could be out): held as one that came to
        // nothing, and sent again after that wait.
        JetKvmByHand jet;
        Answered silent;
        silent.never = true;
        jet.signInInstead = silent;
        const JetKvmDevice::Pace pace = quick();
        JetKvmDevice device(at(jet), u"secret"_s, pace);
        QCOMPARE(codeOf(identified(device)), std::string("no_sign_in"));
        QCOMPARE(codeOf(identified(device)), std::string("no_sign_in"));
        QCOMPARE(jet.signIns, 1);
        QTest::qWait(pace.signInRetryMs + 200);
        QCOMPARE(codeOf(identified(device)), std::string("no_sign_in"));
        QCOMPARE(jet.signIns, 2);
    }

    void aPasswordGoesOnlyOnALineShownToBeUp_data()
    {
        QTest::addColumn<int>("line");
        // A line never let in: said not there within the guess, and nothing sent. The sign-in's own line let in only
        // after the guess, the question before it answered: sent once, and waited for. The line closed without a word
        // after that question: sent once, on a line of its own, and never "not there".
        QTest::newRow("never let in") << 0;
        QTest::newRow("its sign-in's line let in late") << 1;
        QTest::newRow("closed without a word") << 2;
    }

    void aPasswordGoesOnlyOnALineShownToBeUp()
    {
        // As a kvmd's: whether it was set up, asked first under the guess where nothing of the JetKVM's was heard just
        // now; then the sign-in, never "not there" on a guess. (A driver is made first: it chooses Windows' own TLS,
        // which a TLS stand-in made before it would leave it no way to.)
        QFETCH(const int, line);
        const JetKvmDevice first(JetKvmDevice::Where{}, QString(), quick());
        JetKvmByHand jet(line == 2 ? std::nullopt : std::optional<Tls>(tls("first")));
        JetKvmDevice::Where where = at(jet);
        where.knownByCertificate = line != 2;
        where.certificate =
            line != 2 ? StandIn::certificateIn(tls("first").certificateFile).digest(QCryptographicHash::Sha256)
                      : QByteArray();
        JetKvmDevice::Pace pace = quick();
        pace.connectWithinMs = 4000; // (a TLS handshake under the address sanitizer on a busy PC)
        pace.quietMs = 3200;         // (the sign-in's own wait, two and a half times this, longer than its line's)
        pace.lineWithinMs = 5000;
        if (line == 0) {
            QVERIFY(jet.stand->letInOnlyAfter(60'000));
        } else if (line == 1) {
            // (Only the line after the question asked first: the one the sign-in goes on.)
            jet.stand->beforeAnswer = [&jet, slow = pace.connectWithinMs + 500, slowed = false]() mutable {
                const bool signIn = jet.stand->asked.constLast().method == "POST";
                if (signIn || !std::exchange(slowed, true)) {
                    jet.stand->letInOnlyAfter(signIn ? 0 : slow);
                }
            };
        } else {
            jet.stand->closesSilently = true;
        }
        JetKvmDevice device(where, u"secret"_s, pace);
        QElapsedTimer watch;
        watch.start();
        const auto what = identified(device);
        jet.stand->beforeAnswer = nullptr;
        if (line == 0) {
            QCOMPARE(codeOf(what), std::string("unreachable"));
            QVERIFY(watch.elapsed() < pace.connectWithinMs + pace.quietMs);
            QCOMPARE(jet.signIns, 0);
            QVERIFY(!pace.pacing->standing(SignInPacing::what(u"login", where.address, QString(), u"secret"_s)));
            return;
        }
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(jet.signIns, 1);
    }

    void aSecondDriverFindsTheHoldTheFirstsSignInLeftBehindItsQuestion()
    {
        // (The first question's reading, as a kvmd's.) A JetKVM locks out after five wrong passwords: one strike, not
        // two.
        JetKvmByHand jet;
        jet.password = u"changed on the JetKVM"_s;
        jet.statusAfterMs = 300;
        const JetKvmDevice::Pace pace = quick();
        JetKvmDevice one(at(jet), u"secret"_s, pace);
        JetKvmDevice other(at(jet), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); };
        one.identify(keep);
        other.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(jet.signIns, 1);
        // (The other waited for that no, and it is its no too.)
        QCOMPARE(codes, (QList<std::string>{"auth_failed", "auth_failed"}));
    }

    void aSecondDriverWaitsForASlowNoAndSendsNothing()
    {
        // (The pacing's reading, M1, as a kvmd's.) Its no slower than the wait after one that came to nothing: the
        // other driver sends nothing while the sign-in can be out, and the no is its no too. (Five strikes lock it.)
        JetKvmByHand jet;
        jet.password = u"changed on the JetKVM"_s;
        jet.signInAfterMs = 1500;
        const JetKvmDevice::Pace pace = quick();
        JetKvmDevice one(at(jet), u"secret"_s, pace);
        JetKvmDevice other(at(jet), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); };
        one.identify(keep);
        QTRY_COMPARE(jet.signIns, 1);
        QTest::qWait(pace.signInRetryMs + 200);
        QVERIFY(codes.isEmpty());
        other.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(codes, (QList<std::string>{"auth_failed", "auth_failed"}));
    }

    void aSecondDriverWaitsForTheFirstsSignInAndThenSignsInItself()
    {
        // (The pacing's reading, L3, as a kvmd's.) A slow yes: the other is not told "does not sign qKVM in" while it
        // is out; it waits, and signs in itself after.
        JetKvmByHand jet;
        jet.signInAfterMs = 1200;
        const JetKvmDevice::Pace pace = quick();
        JetKvmDevice one(at(jet), u"secret"_s, pace);
        JetKvmDevice other(at(jet), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<JetKvmDevice::Identity>& said) { codes.append(codeOf(said)); };
        one.identify(keep);
        QTRY_COMPARE(jet.signIns, 1);
        other.identify(keep);
        QTest::qWait(600);
        QVERIFY(codes.isEmpty());
        QCOMPARE(jet.signIns, 1);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(codes, (QList<std::string>{"ok", "ok"}));
        QCOMPARE(jet.signIns, 2);
    }

    void theQuestionAskedFirstAndTheSignInGoOnOneLine()
    {
        // (Over plain HTTP that question is its number, with no session: the merged review, 4.)
        JetKvmByHand jet;
        jet.stand->keepsLines = true;
        JetKvmDevice device(at(jet), u"secret"_s, quick());
        QVERIFY(identified(device));
        const QList<qkvm::core::testing::Asked>& asked = jet.stand->asked;
        QVERIFY(asked.size() >= 2);
        QCOMPARE(asked.at(0).path, "/device"_ba);
        QVERIFY(!asked.at(0).headers.contains("cookie"));
        QCOMPARE(asked.at(1).path, "/auth/login-local"_ba);
        QCOMPARE(asked.at(1).line, asked.at(0).line);
    }
};

QTEST_GUILESS_MAIN(TestJetKvmDevice)
#include "test_jetkvm_device.moc"
