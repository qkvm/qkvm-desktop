#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTest>
#include <QTimer>

#include <qkvm/core/redfish_answers.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/sign_in_pacing.h>

#include "redfish_by_hand.h"
#include "redfish_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::RedfishDevice;
using qkvm::core::SignInPacing;

// A server's management controller spoken to by the program itself over Redfish (redfish_device.h),
// against a stand-in that answers as the web's does (redfish_by_hand.h): its sign-in, its sessions and its sign-out,
// and what it is (1a-iii). Ported from the web's own tests where they are named (../app/tests).
class TestRedfishSignIn : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The driver chooses Windows' own TLS, which is refused once TLS is in use: so one is made before
        // anything else touches TLS.
        const RedfishDevice first(RedfishDevice::Where{}, QString(), quick());
    }

    void itSignsInOnceWithASessionAndCarriesItsToken()
    {
        // (R1; test_redfish.py:96) One sign-in, its user and password as JSON; every read under the token it
        // handed out, in its own header, as it was sent (a token in mixed case is another in small letters);
        // never a cookie, never an Authorization.
        RedfishByHand bmc;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        QVERIFY(identified(device));
        QCOMPARE(bmc.logins, 1);
        const QJsonObject sent = QJsonDocument::fromJson(bmc.loginBodies.value(0)).object();
        QCOMPARE(sent, (QJsonObject{{u"UserName"_s, u"admin"_s}, {u"Password"_s, u"secret"_s}}));
        // (Before it, the root anybody may read: a password goes only on a line shown to be up.)
        QCOMPARE(bmc.stand->asked.first().path, "/redfish/v1/"_ba);
        QCOMPARE(bmc.stand->asked.at(1).headers.value("content-type"), "application/json"_ba);
        QVERIFY(identified(device));
        QCOMPARE(bmc.logins, 1);
        // (Its computer read twice the first time: where it keeps things, then judged; once after, where it keeps
        // things being known.)
        QCOMPARE(bmc.asked(u"GET "_s + bmc.system()), 3);
        const QByteArray token = bmc.sessions.keys().value(0);
        QVERIFY(token.endsWith("Ab+/="));
        int underIt = 0;
        for (const Asked& one : std::as_const(bmc.stand->asked)) {
            if (one.method == "POST") {
                QVERIFY(!one.headers.contains("x-auth-token"));
                continue;
            }
            if (!one.headers.contains("x-auth-token")) {
                // (Only its root, which anybody may read, goes with no session: for its controller's number.)
                QCOMPARE(one.path, "/redfish/v1/"_ba);
                continue;
            }
            QCOMPARE(one.headers.value("x-auth-token"), token);
            QCOMPARE(one.headers.value("accept"), "application/json"_ba);
            ++underIt;
        }
        QCOMPARE(underIt, static_cast<int>(bmc.requests.size()));
        QVERIFY(signsOnToNothing(bmc));
        // (Its root before the sign-in, as before every one, its controller's number taken from it: nothing was kept.)
        QCOMPARE(bmc.rootsUnsigned, 1);
    }

    void aRefusedPasswordIsSaidAndNotOfferedAgainBeforeItsTime()
    {
        // (R2; test_redfish.py:678; test_a_password_a_device_rejects_again_and_again.py:99) A controller locks
        // an account after a few wrong passwords.
        RedfishByHand bmc;
        bmc.password = u"changed on the controller"_s;
        const RedfishDevice::Pace pace = quick();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("auth_failed"));
        QCOMPARE(what.error().message, std::string("Login rejected - check the username and password"));
        QCOMPARE(bmc.logins, 1);
        // Meanwhile it is not offered at all, however often it is asked, nor by a driver made anew.
        for (int again = 0; again < 3; ++again) {
            QCOMPARE(codeOf(identified(device)), std::string("auth_failed"));
        }
        RedfishDevice anew(at(bmc), u"secret"_s, pace);
        QCOMPARE(codeOf(identified(anew)), std::string("auth_failed"));
        QCOMPARE(bmc.logins, 1);
        // After its time, once more; refused again, it waits longer (five times as long).
        QTest::qWait(pace.refusedHoldMs + 100);
        QCOMPARE(codeOf(identified(device)), std::string("auth_failed"));
        QCOMPARE(bmc.logins, 2);
        QTest::qWait(pace.refusedHoldMs + 100);
        QCOMPARE(codeOf(identified(device)), std::string("auth_failed"));
        QCOMPARE(bmc.logins, 2);
        // A password a person has just typed is tried as for the first time; set right on the controller,
        // it signs in, and a no after that is the first again.
        bmc.password = u"secret"_s;
        RedfishDevice::tryAfresh(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(device));
        QCOMPARE(bmc.logins, 3);
        QVERIFY(signsOnToNothing(bmc));
    }

    void aSignInTurnedAwayForAnotherReasonIsNotRepeatedAtEveryLook_data()
    {
        QTest::addColumn<int>("status");
        // Every session it has is given out; a stand-in of another make's that takes a sign-in it does not
        // understand as a bad question (test_controllers.py:629); one that says yes and gives no token.
        QTest::newRow("503") << 503;
        QTest::newRow("400") << 400;
        QTest::newRow("204") << 204;
        QTest::newRow("500") << 500;
    }

    void aSignInTurnedAwayForAnotherReasonIsNotRepeatedAtEveryLook()
    {
        // (R3; test_redfish.py:734) The password has nothing to do with it, and is not sent again each time.
        QFETCH(int, status);
        RedfishByHand bmc;
        bmc.loginStatus = status;
        const RedfishDevice::Pace pace = quick();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("no_sign_in"));
        QVERIFY(QString::fromStdString(what.error().message).contains(QString::number(status)));
        for (int again = 0; again < 3; ++again) {
            QCOMPARE(codeOf(identified(device)), std::string("no_sign_in"));
        }
        QCOMPARE(bmc.logins, 1);
        // Nothing was asked under a session it never gave.
        QVERIFY(bmc.requests.isEmpty());
        // Not a no: the password is offered again after the shorter wait, and taken once the controller has a
        // session to give.
        bmc.loginStatus = 201;
        QTest::qWait(pace.signInRetryMs + 100);
        QVERIFY(identified(device));
        QCOMPARE(bmc.logins, 2);
    }

    void aControllerWithEverySessionGivenOutTurnsTheNextAway()
    {
        // (The stand-in has four.) A fifth driver is turned away, and says why.
        RedfishByHand bmc;
        std::vector<std::unique_ptr<RedfishDevice>> four;
        RedfishDevice::Pace pace = quick();
        for (int index = 0; index < 4; ++index) {
            four.push_back(std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace));
            QVERIFY(identified(*four.back()));
        }
        RedfishDevice fifth(at(bmc), u"secret"_s, pace);
        QCOMPARE(codeOf(identified(fifth)), std::string("no_sign_in"));
        QCOMPARE(bmc.sessions.size(), 4);
        // Let go of, each gives its session back.
        four.clear();
        QTRY_COMPARE(bmc.sessions.size(), 0);
        QCOMPARE(bmc.signedOut, 4);
        QTRY_COMPARE(pace.goodbyes->underWay(), 0);
    }

    void aDriverLetGoOfGivesItsSessionBack()
    {
        // (R4; test_redfish.py:96) A controller has only so many: none is left behind.
        RedfishByHand bmc;
        const RedfishDevice::Pace pace = quick();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(*device));
        QCOMPARE(bmc.sessions.size(), 1);
        const QByteArray token = bmc.sessions.keys().value(0);
        device.reset();
        QTRY_COMPARE(bmc.signedOut, 1);
        QVERIFY(bmc.sessions.isEmpty());
        const Asked goodbye = bmc.stand->asked.last();
        QCOMPARE(goodbye.method, "DELETE"_ba);
        QCOMPARE(goodbye.path, "/redfish/v1/SessionService/Sessions/1"_ba);
        QCOMPARE(goodbye.headers.value("x-auth-token"), token);
        QTRY_COMPARE(pace.goodbyes->underWay(), 0);
        QCOMPARE(pace.goodbyes->given(), 1);
        // A sign-in that was only tried, and found no computer to know the controller by, is given back too.
        bmc.systemLink = u"/somewhere/else"_s;
        device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        const auto what = identified(*device);
        QCOMPARE(codeOf(what), std::string("bad_response"));
        QCOMPARE(what.error().message, std::string("The controller lists no computer"));
        device.reset();
        QTRY_COMPARE(bmc.signedOut, 2);
        QVERIFY(bmc.sessions.isEmpty());
        QCOMPARE(bmc.logins, 2);
        QVERIFY(signsOnToNothing(bmc));
    }

    void aControllerThatDawdlesOverTheSignOutHoldsNothingUp()
    {
        // (R4; test_redfish.py:121) It answers a byte at a time, and never to its end: no single wait for the
        // next piece runs out. The driver goes at once; its goodbye is let go of in its own time.
        RedfishByHand bmc;
        bmc.dawdles = true;
        const RedfishDevice::Pace pace = quick();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(*device));
        QElapsedTimer watch;
        watch.start();
        device.reset();
        QVERIFY(watch.elapsed() < 200);
        QTRY_COMPARE(bmc.signedOut, 1);
        QCOMPARE(pace.goodbyes->underWay(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(pace.goodbyes->underWay(), 0, pace.signOutWithinMs + 1500);
        // Within its time in all, and not before it.
        QVERIFY(watch.elapsed() >= pace.signOutWithinMs);
        QVERIFY(watch.elapsed() < pace.signOutWithinMs + 1000);
    }

    void noSessionNoGoodbye()
    {
        // (redfish.py:640) None where it had none: it was never signed in to, or its sign-in was refused.
        RedfishByHand bmc;
        const RedfishDevice::Pace pace = quick();
        {
            const RedfishDevice never(at(bmc), u"secret"_s, pace);
        }
        bmc.password = u"another"_s;
        {
            RedfishDevice refused(at(bmc), u"secret"_s, pace);
            QCOMPARE(codeOf(identified(refused)), std::string("auth_failed"));
        }
        QTest::qWait(300);
        QCOMPARE(pace.goodbyes->given(), 0);
        QCOMPARE(bmc.signedOut, 0);
        QCOMPARE(bmc.stand->asked.size(), 2); // the root asked first, the sign-in, and nothing after it
    }

    void aControllerThatWentAwayIsNotSaidGoodbyeTo()
    {
        // (redfish.py:640) Where it was away the last time it was asked, nothing is sent after it.
        RedfishByHand bmc;
        const RedfishDevice::Pace pace = quick();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(*device));
        bmc.stand->takeNoMore();
        QCOMPARE(codeOf(identified(*device)), std::string("unreachable"));
        device.reset();
        QTest::qWait(300);
        QCOMPARE(pace.goodbyes->given(), 0);
    }

    void aSessionsPlaceIsOnlyEverOneOfItsOwn_data()
    {
        QTest::addColumn<int>("location");
        QTest::addColumn<bool>("signedOut");
        // Named with another host before it: the path alone, on this controller (the web's urlsplit). Named
        // outside /redfish/v1, or not at all: nowhere to sign out, and nothing is sent anywhere.
        QTest::newRow("its own") << static_cast<int>(RedfishByHand::LocationIs::Own) << true;
        QTest::newRow("another host") << static_cast<int>(RedfishByHand::LocationIs::AnotherHost) << true;
        QTest::newRow("outside Redfish") << static_cast<int>(RedfishByHand::LocationIs::OutsideRedfish) << false;
        QTest::newRow("none") << static_cast<int>(RedfishByHand::LocationIs::None) << false;
    }

    void aSessionsPlaceIsOnlyEverOneOfItsOwn()
    {
        // (R5)
        QFETCH(int, location);
        QFETCH(bool, signedOut);
        RedfishByHand bmc;
        bmc.locationIs = static_cast<RedfishByHand::LocationIs>(location);
        const RedfishDevice::Pace pace = quick();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(*device));
        const qsizetype before = bmc.stand->asked.size();
        device.reset();
        if (signedOut) {
            QTRY_COMPARE(bmc.signedOut, 1);
            QCOMPARE(bmc.stand->asked.last().path, "/redfish/v1/SessionService/Sessions/1"_ba);
        } else {
            QTest::qWait(500);
            QCOMPARE(bmc.stand->asked.size(), before);
            QCOMPARE(pace.goodbyes->given(), 0);
        }
    }

    void aTokenThatIsNoTokenIsNoSignIn_data()
    {
        QTest::addColumn<QByteArray>("token");
        QTest::newRow("a blank") << " "_ba;
        QTest::newRow("513 signs") << QByteArray(513, 'a');
        QTest::newRow("a blank inside") << "abc def"_ba;
        QTest::newRow("what ends a cookie") << "abc;def"_ba;
        QTest::newRow("a quote") << "abc\"def"_ba;
    }

    void aTokenThatIsNoTokenIsNoSignIn()
    {
        // (R6) It goes back in a header with every question: one that could end the line, or say something
        // else there, is no session, and nothing is sent under it.
        QFETCH(QByteArray, token);
        RedfishByHand bmc;
        bmc.tokenIs = token;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("no_sign_in"));
        QVERIFY(QString::fromStdString(what.error().message).contains(u"HTTP 201"_s));
        QCOMPARE(bmc.stand->asked.size(), 2); // (the root asked first, and the sign-in)
        QVERIFY(bmc.requests.isEmpty());
        // (And 512 signs of what may be in one are one.)
        RedfishByHand longest;
        longest.tokenIs = QByteArray(512, 'a');
        RedfishDevice taken(at(longest), u"secret"_s, quick());
        QVERIFY(identified(taken));
    }

    void whatAControllerSaysItIs()
    {
        // (R7; test_redfish.py:63) It has no host name to go by.
        RedfishByHand bmc;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(what.value().model, u"SRV-2200"_s);
        QCOMPARE(what.value().version, u"1.42.0"_s);
        QCOMPARE(what.value().hostname, QString());
        QCOMPARE(what.value().number, std::optional<QString>(markOf(bmc.uuid)));
        QCOMPARE(device.machine(), markOf(bmc.uuid) + digestOf(bmc.rootUuid));
        // Its root, its computers, the computer and its chassis (where its sensors are), which it is (the computer
        // judged; the controller its computer names, under the session, against the root anybody may read: nothing
        // was kept of it), and the controller's version.
        QCOMPARE(bmc.requests,
                 (QStringList{u"GET /redfish/v1"_s, u"GET /redfish/v1/Systems"_s, u"GET "_s + bmc.system(),
                              u"GET /redfish/v1/Chassis/Self"_s, u"GET "_s + bmc.system(), u"GET "_s + bmc.manager(),
                              u"GET "_s + bmc.manager()}));
        QCOMPARE(bmc.rootsUnsigned, 1); // (before the sign-in, and its controller's number taken from that read)
        // A name it goes by; a computer that names no controller of its own: the first the root lists.
        bmc.hostname = u"db-01"_s;
        bmc.managerNamed = false;
        RedfishDevice anew(at(bmc), u"secret"_s, quick());
        what = identified(anew);
        QCOMPARE(what.value().hostname, u"db-01"_s);
        QCOMPARE(what.value().version, u"1.42.0"_s);
        QVERIFY(bmc.requests.contains(u"GET /redfish/v1/Managers"_s));
        // The other layout.
        RedfishByHand openbmc(RedfishByHand::Layout::OpenBmc);
        RedfishDevice other(at(openbmc), u"secret"_s, quick());
        what = identified(other);
        QVERIFY(what);
        QCOMPARE(what.value().version, u"1.42.0"_s);
        QVERIFY(openbmc.requests.contains(u"GET /redfish/v1/Systems/system"_s));
        QVERIFY(openbmc.requests.contains(u"GET /redfish/v1/Managers/bmc"_s));
        QVERIFY(signsOnToNothing(bmc) && signsOnToNothing(openbmc));
    }

    void whatAControllerSaysOfItselfIsNotTakenAtFaceValue()
    {
        // (R7; test_redfish.py:766) A name nobody filled in, a name that is one line of script, a version
        // that is no text; a number that is none (its maker set none: test_twins.py:139).
        RedfishByHand bmc;
        bmc.model = u"To Be Filled By O.E.M."_s;
        bmc.manufacturer = u"Rack\nMaker <script>"_s + QString(300, u'x');
        bmc.hostname = u"Default string"_s;
        bmc.firmware = QJsonObject{{u"not"_s, u"a version"_s}};
        bmc.uuid = u"00000000-0000-0000-0000-000000000000"_s;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY(what);
        QVERIFY(what.value().model.startsWith(u"Rack Maker <script>"_s));
        QVERIFY(what.value().model.size() <= 60);
        QCOMPARE(what.value().version, QString());
        QCOMPARE(what.value().hostname, QString());
        // (Its computer gave none: known by its controller's number alone.)
        QCOMPARE(what.value().number, std::optional<QString>(QString()));
        QCOMPARE(device.machine(), QString(qkvm::core::redfish::saidNone) + digestOf(bmc.rootUuid));
        // Its controller not well just then: what it is all the same, without its version (and known by its
        // computer's number: which server answers is told by that, t5 has the computer that gives none).
        bmc.firmware = u"1.42.0"_s;
        bmc.uuid = RedfishByHand::fresh();
        bmc.managerFails = 5;
        RedfishDevice again(at(bmc), u"secret"_s, quick());
        const auto without = identified(again);
        QVERIFY(without);
        QCOMPARE(without.value().version, QString());
    }

    void withoutAPasswordNothingIsSent()
    {
        // (R8) A controller is asked nothing without a session: not its root, not anything.
        RedfishByHand bmc;
        RedfishDevice device(at(bmc), QString(), quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("no_password"));
        QCOMPARE(what.error().message, std::string("No password saved for this device"));
        QTest::qWait(300);
        QVERIFY(bmc.stand->asked.isEmpty());
        QCOMPARE(bmc.stand->lines, 0);
    }

    void aControllerThatRefusesTheSessionItGaveIsNotHammered()
    {
        // (R9; redfish.py:762-776) Refused once: signed in once more. Refused again: it says so, and is not
        // signed in to again for a while.
        RedfishByHand bmc;
        bmc.refusesSessions = true;
        const RedfishDevice::Pace pace = quick();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("auth_failed"));
        QCOMPARE(what.error().message, std::string("The controller keeps rejecting the session"));
        QCOMPARE(bmc.logins, 2);
        for (int again = 0; again < 3; ++again) {
            QCOMPARE(identified(device).error().message, std::string("The controller keeps rejecting the session"));
        }
        QCOMPARE(bmc.logins, 2);
        QTest::qWait(pace.signInRetryMs + 100);
        bmc.refusesSessions = false;
        QVERIFY(identified(device));
        QCOMPARE(bmc.logins, 3);
        // A session it forgot (it restarted) is signed in to anew, once.
        bmc.sessions.clear();
        QVERIFY(identified(device));
        QCOMPARE(bmc.logins, 4);
    }

    void aDriverLetGoOfWhileItsSignInIsOutLeavesItsMark()
    {
        // Counted as it goes out: a driver made anew does not offer the password again at once. It waits for what the
        // one that is out comes to (heard by the goodbyes, its line's), and that no is its no too.
        RedfishByHand bmc;
        bmc.password = u"another"_s;
        bmc.loginTakesMs = 800;
        const RedfishDevice::Pace pace = quick();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        device->identify([](const base::Result<RedfishDevice::Identity>&) {});
        QTRY_COMPARE(bmc.logins, 1);
        device.reset();
        RedfishDevice anew(at(bmc), u"secret"_s, pace);
        QCOMPARE(codeOf(identified(anew)), std::string("auth_failed"));
        QCOMPARE(bmc.logins, 1);
    }

    void overHttpsEveryLineIsHeldToItsCertificate()
    {
        // The sign-in goes on a line of its own: held to the certificate the controller is known by, as every
        // other question is. Another certificate: the password goes nowhere.
        RedfishByHand bmc(RedfishByHand::Layout::Ami, tls("first"));
        RedfishDevice::Where where = at(bmc);
        where.knownByCertificate = true;
        where.certificate = sha256Of("second");
        RedfishDevice::Pace pace = quick();
        pace.connectWithinMs = 5000; // (a TLS handshake under the address sanitizer on a busy PC)
        RedfishDevice other(where, u"secret"_s, pace);
        QCOMPARE(codeOf(identified(other)), std::string("cert_mismatch"));
        QVERIFY(bmc.stand->asked.isEmpty());
        QCOMPARE(bmc.logins, 0);
        // Its own: signed in, asked, and the certificate it showed said once, of both its lines.
        where.certificate = sha256Of("first");
        RedfishDevice known(where, u"secret"_s, pace);
        QSignalSpy showed(&known, &RedfishDevice::showedItsCertificate);
        const auto what = identified(known);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(showed.size(), 1);
        QCOMPARE(QCryptographicHash::hash(showed.first().first().toByteArray(), QCryptographicHash::Sha256),
                 sha256Of("first"));
        QCOMPARE(bmc.logins, 1);
    }

    void aSessionsPlaceIsOneNameUnderTheSessions_data()
    {
        QTest::addColumn<int>("location");
        QTest::addColumn<bool>("bodyNamesIt");
        QTest::addColumn<bool>("signedOut");
        // A DELETE anywhere else could end something else: an account, for one. Where its Location is no such
        // place, the place its answer names is taken, under the same rule; and none where neither is one.
        QTest::newRow("an account") << static_cast<int>(RedfishByHand::LocationIs::Account) << false << false;
        QTest::newRow("an account, its answer naming its place")
            << static_cast<int>(RedfishByHand::LocationIs::Account) << true << true;
        QTest::newRow("deeper than one name") << static_cast<int>(RedfishByHand::LocationIs::Deeper) << false << false;
        QTest::newRow("none, its answer naming its place")
            << static_cast<int>(RedfishByHand::LocationIs::None) << true << true;
    }

    void aSessionsPlaceIsOneNameUnderTheSessions()
    {
        QFETCH(int, location);
        QFETCH(bool, bodyNamesIt);
        QFETCH(bool, signedOut);
        RedfishByHand bmc;
        bmc.locationIs = static_cast<RedfishByHand::LocationIs>(location);
        bmc.bodyNamesItsPlace = bodyNamesIt;
        const RedfishDevice::Pace pace = quick();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(*device));
        const qsizetype before = bmc.stand->asked.size();
        device.reset();
        if (signedOut) {
            QTRY_COMPARE(bmc.signedOut, 1);
            QCOMPARE(bmc.stand->asked.last().path, "/redfish/v1/SessionService/Sessions/1"_ba);
        } else {
            QTest::qWait(500);
            QCOMPARE(bmc.stand->asked.size(), before);
            QCOMPARE(pace.goodbyes->given(), 0);
        }
    }

    void aSessionLetGoOfAfterARefusalIsGivenBackAndTheHoldOutlivesItsDriver()
    {
        // An older board refuses a resource the account may not read (401) and keeps the session: signed in once
        // more, refused again. Both sessions given back; and a driver made anew does not sign in again at once
        // (two clicks of a box would fill the controller): the hold is the pacing's.
        RedfishByHand bmc;
        bmc.unauthorizedAt = bmc.system();
        const RedfishDevice::Pace pace = quick();
        auto first = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        QCOMPARE(identified(*first).error().message, std::string("The controller keeps rejecting the session"));
        QCOMPARE(bmc.logins, 2);
        QTRY_COMPARE(bmc.signedOut, 2);
        QVERIFY(bmc.sessions.isEmpty());
        first.reset();
        RedfishDevice second(at(bmc), u"secret"_s, pace);
        QCOMPARE(codeOf(identified(second)), std::string("no_sign_in"));
        QCOMPARE(bmc.logins, 2);
        // Its time over, and the controller let it read: in.
        bmc.unauthorizedAt.clear();
        QTest::qWait(pace.signInRetryMs + 100);
        QVERIFY(identified(second));
        QCOMPARE(bmc.logins, 3);
    }

    void aLateSignInThatCameToNothingDoesNotHoldAPasswordAnotherDriverWasLetInWith()
    {
        // A driver let go of mid sign-in; another is let in with the same password meanwhile; then the first one's
        // line runs out of time. The password was proven: nothing holds it.
        RedfishByHand bmc;
        bmc.loginsNever = 1;
        RedfishDevice::Pace pace = quick();
        pace.signInQuietMs = 1000;
        auto gone = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        gone->identify([](const base::Result<RedfishDevice::Identity>&) {});
        QTRY_COMPARE(bmc.logins, 1);
        gone.reset();
        const SignInPacing::Mark mark = SignInPacing::what(u"login", bmc.address(), u"admin"_s, u"secret"_s);
        pace.pacing->accepted(mark); // (as the driver made anew would be let in, its own sign-in counted out)
        RedfishDevice anew(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(anew));
        QCOMPARE(bmc.logins, 2);
        QTRY_COMPARE_WITH_TIMEOUT(pace.goodbyes->underWay(), 0, 5000);
        QVERIFY(!pace.pacing->standing(mark));
        RedfishDevice third(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(third));
        QCOMPARE(bmc.logins, 3);
    }

    void atTheProgramsEndEverySessionIsGivenBackWithinItsTime()
    {
        // (server.py:1098, core.py:544: the web waits for each device's close at its shutdown.) While the loop
        // still runs, the goodbyes are waited for: every one given back, or cut at the bound.
        RedfishByHand one;
        RedfishByHand two;
        const RedfishDevice::Pace pace = quick();
        auto first = std::make_unique<RedfishDevice>(at(one), u"secret"_s, pace);
        auto second = std::make_unique<RedfishDevice>(at(two), u"secret"_s, pace);
        QVERIFY(identified(*first) && identified(*second));
        first.reset();
        second.reset();
        QElapsedTimer watch;
        watch.start();
        pace.goodbyes->finish(3000);
        QVERIFY(watch.elapsed() < 2000);
        QCOMPARE(pace.goodbyes->underWay(), 0);
        QCOMPARE(one.signedOut + two.signedOut, 2);
        // With nothing out, at once.
        watch.restart();
        pace.goodbyes->finish(3000);
        QVERIFY(watch.elapsed() < 100);
        // One that never answers: cut at the bound. (Given back after the end was waited for, it says that it may
        // never be: here the loop goes on.)
        RedfishByHand silent;
        silent.signOutNever = true;
        auto third = std::make_unique<RedfishDevice>(at(silent), u"secret"_s, pace);
        QVERIFY(identified(*third));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"given back as the program ends"_s));
        third.reset();
        watch.restart();
        pace.goodbyes->finish(400);
        QVERIFY(watch.elapsed() >= 350 && watch.elapsed() < 900); // (its own time is 1 s: finish's is shorter)
        QCOMPARE(silent.signedOut, 1);
    }

    void aSignInIsWaitedForItsOwnTime()
    {
        // (The web's WAITS x SIGN_IN_PATIENCE: twenty seconds for every family's sign-in, made short here.)
        // Answered within it: in, though that is longer than any other answer is waited for.
        RedfishDevice::Pace pace = quick();
        pace.signInQuietMs = 2500;
        RedfishByHand bmc;
        bmc.loginTakesMs = 1900;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(device));
        // Answered after it: a sign-in that came to nothing, held back as one, and not sent again at once.
        RedfishByHand late;
        late.loginTakesMs = 3300;
        RedfishDevice slow(at(late), u"secret"_s, pace);
        QCOMPARE(codeOf(identified(slow)), std::string("no_sign_in"));
        QCOMPARE(codeOf(identified(slow)), std::string("no_sign_in"));
        QCOMPARE(late.logins, 1);
        QVERIFY(late.requests.isEmpty());
        // What it came to replaced the hold it went out with (as long as it could be out): held as one that came to
        // nothing, and sent again after that wait.
        QTest::qWait(pace.signInRetryMs + 200);
        QCOMPARE(codeOf(identified(slow)), std::string("no_sign_in"));
        QCOMPARE(late.logins, 2);
    }

    void aSignInThatIsOutWhenItsDriverGoesIsGivenBackWhenItComes()
    {
        // A controller has four sessions or so, and keeps one it forgot for half an hour: a box closed during a
        // slow sign-in, a few times over, would shut the owner out of their own controller's page.
        RedfishByHand bmc;
        bmc.loginTakesMs = 600;
        const RedfishDevice::Pace pace = quick();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        device->identify([](const base::Result<RedfishDevice::Identity>&) {});
        QTRY_COMPARE(bmc.logins, 1);
        device.reset();
        QCOMPARE(pace.goodbyes->underWay(), 1);
        // It comes after the driver went: the session it brought is given back, and nothing is left.
        QTRY_COMPARE(bmc.signedOut, 1);
        QVERIFY(bmc.sessions.isEmpty());
        QCOMPARE(bmc.stand->asked.last().method, "DELETE"_ba);
        QCOMPARE(bmc.stand->asked.last().path, "/redfish/v1/SessionService/Sessions/1"_ba);
        QTRY_COMPARE(pace.goodbyes->underWay(), 0);
        QCOMPARE(pace.goodbyes->given(), 1);
        // And the pacing was told it was let in: nothing stands against the password.
        const SignInPacing::Mark mark = SignInPacing::what(u"login", bmc.address(), u"admin"_s, u"secret"_s);
        QVERIFY(!pace.pacing->standing(mark));
        QVERIFY(signsOnToNothing(bmc));
    }

    void aSignInThatIsOutWhenItsDriverGoesIsCountedWhenItIsRefused()
    {
        // Refused after the driver went: nothing to give back, and the no is noted where every driver of the
        // controller sees it, as the driver would have noted it.
        RedfishByHand bmc;
        bmc.password = u"changed on the controller"_s;
        bmc.loginTakesMs = 600;
        const RedfishDevice::Pace pace = quick();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        device->identify([](const base::Result<RedfishDevice::Identity>&) {});
        QTRY_COMPARE(bmc.logins, 1);
        device.reset();
        QTRY_COMPARE(pace.goodbyes->underWay(), 0);
        const SignInPacing::Mark mark = SignInPacing::what(u"login", bmc.address(), u"admin"_s, u"secret"_s);
        QCOMPARE(pace.pacing->times(mark), 1);
        QCOMPARE(pace.pacing->silent(mark), 0);
        QCOMPARE(pace.goodbyes->given(), 0);
        QCOMPARE(bmc.signedOut, 0);
        QCOMPARE(bmc.stand->asked.size(), 2); // (the root asked first, and the sign-in)
        RedfishDevice anew(at(bmc), u"secret"_s, pace);
        const auto what = identified(anew);
        QCOMPARE(codeOf(what), std::string("auth_failed"));
        QCOMPARE(what.error().message, std::string("Login rejected - check the username and password"));
        QCOMPARE(bmc.logins, 1);
    }

    void aSignInThatIsOutWhenItsDriverGoesIsLetGoOfInItsOwnTime()
    {
        // Never answered: its line goes when the sign-in's own time is up, nothing is heard after it, and the
        // pacing holds it as one that came to neither yes nor no (counted once).
        RedfishByHand bmc;
        bmc.loginNever = true;
        RedfishDevice::Pace pace = quick();
        pace.signInQuietMs = 1000;
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        device->identify([](const base::Result<RedfishDevice::Identity>&) {});
        QTRY_COMPARE(bmc.logins, 1);
        device.reset();
        QCOMPARE(pace.goodbyes->underWay(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(pace.goodbyes->underWay(), 0, pace.signInQuietMs + 3000);
        QCOMPARE(bmc.stand->asked.size(), 2); // (the root asked first, and the sign-in)
        QCOMPARE(pace.goodbyes->given(), 0);
        const SignInPacing::Mark mark = SignInPacing::what(u"login", bmc.address(), u"admin"_s, u"secret"_s);
        QCOMPARE(pace.pacing->silent(mark), 1);
        QCOMPARE(pace.pacing->times(mark), 0);
        QVERIFY(pace.pacing->waitLeftMs(mark) > 0);
    }

    void aLineTheControllerClosedWithoutAWordIsNoSignInThatCameToNothing_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("in the clear") << false;
        QTest::newRow("over TLS") << true;
    }

    void aLineTheControllerClosedWithoutAWordIsNoSignInThatCameToNothing()
    {
        // A controller that closes the line right after its root, without saying it would (no "Connection: close"):
        // the line looks kept. The password goes once, a session comes, and nothing is held back. (Qt finds the line
        // closed before the sign-in goes: the close comes with the root's answer.)
        QFETCH(const bool, encrypted);
        RedfishByHand bmc(RedfishByHand::Layout::Ami, encrypted ? std::optional<Tls>(tls("first")) : std::nullopt);
        bmc.beServer(aSystem, aRoot);
        bmc.stand->closesSilently = true;
        RedfishDevice::Pace pace = quick();
        pace.connectWithinMs = 5000; // (a TLS handshake under the address sanitizer on a busy PC)
        RedfishDevice::Where where = keptAs(bmc, markOf(aSystem) + digestOf(aRoot));
        where.knownByCertificate = encrypted;
        where.certificate = encrypted ? sha256Of("first") : QByteArray();
        RedfishDevice device(where, u"secret"_s, pace);
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(bmc.logins, 1);
        QCOMPARE(bmc.sessions.size(), 1);
        QVERIFY(!pace.pacing->standing(SignInPacing::what(u"login", bmc.address(), u"admin"_s, u"secret"_s)));
        QVERIFY(bmc.stand->lines > 1);
    }

    void aSignInThatNothingReachedKeepsNothing()
    {
        // Nothing listens there: the sign-in never went out, and the next one is not held back for it.
        const NobodyThere nobody;
        RedfishDevice::Where where;
        where.address = QUrl(nobody.address());
        const RedfishDevice::Pace pace = quick();
        RedfishDevice device(where, u"secret"_s, pace);
        QCOMPARE(codeOf(identified(device)), std::string("unreachable"));
        const SignInPacing::Mark mark = SignInPacing::what(u"login", where.address, where.user, u"secret"_s);
        QCOMPARE(pace.pacing->waitLeftMs(mark), 0);
        QVERIFY(!pace.pacing->standing(mark));
    }

    void r2ASessionRefusedAgainAndAgainIsHeldLongerEachTime_data()
    {
        QTest::addColumn<bool>("everywhere");
        QTest::newRow("refused everywhere but its root") << true;
        QTest::newRow("its computer refused to the account") << false;
    }

    void r2ASessionRefusedAgainAndAgainIsHeldLongerEachTime()
    {
        // It signs this program in and refuses the session everywhere but at its root (which Redfish has it answer
        // whatever the session), or keeps its computer from the account (an operator's): not signed in to again for a
        // while, twice as long each time it does it again; over once a session of it is honoured, its computer read
        // and judged under it. (Neither its root nor a list says so.)
        QFETCH(const bool, everywhere);
        RedfishByHand bmc;
        const auto refuse = [&bmc, everywhere](bool refusing) {
            bmc.refusesSessions = refusing && everywhere;
            bmc.unauthorizedAt = refusing && !everywhere ? bmc.system() : QString();
        };
        refuse(true);
        bmc.mostSessions = 100; // (it refuses their sign-outs too)
        RedfishDevice::Pace pace = quick();
        pace.signInRetryMs = 400;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        const std::string refused("The controller keeps rejecting the session");
        QCOMPARE(identified(device).error().message, refused);
        QCOMPARE(bmc.logins, 2);
        QTest::qWait(500);
        QCOMPARE(identified(device).error().message, refused);
        QCOMPARE(bmc.logins, 4);
        // Twice as long now.
        QTest::qWait(500);
        QCOMPARE(identified(device).error().message, refused);
        QCOMPARE(bmc.logins, 4);
        QTest::qWait(400);
        QCOMPARE(identified(device).error().message, refused);
        QCOMPARE(bmc.logins, 6);
        // Honoured: over. Refused again later, it begins again at the first wait. (Then a session signed in for the
        // question, and refused at once, is not signed in for it again.)
        refuse(false);
        QTest::qWait(1700);
        QVERIFY(identified(device));
        QCOMPARE(bmc.logins, 7);
        refuse(true);
        QCOMPARE(identified(device).error().message, refused);
        QCOMPARE(bmc.logins, 8);
        QTest::qWait(500);
        QCOMPARE(identified(device).error().message, refused);
        QCOMPARE(bmc.logins, 9);
    }

    void aSessionMadeForTheQuestionAndRefusedAtOnceIsNotMadeAgainForIt()
    {
        // It did not run out: the controller refuses it. Signed in again for that question, it was two sign-ins at
        // every round. (A session it held, refused, is signed in for once more: that one may have run out.)
        RedfishByHand bmc;
        bmc.mostSessions = 100;
        RedfishDevice::Pace pace = quick();
        pace.signInRetryMs = 400;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(device));
        QCOMPARE(bmc.logins, 1);
        bmc.refusesSessions = true;
        const std::string refused("The controller keeps rejecting the session");
        QCOMPARE(identified(device).error().message, refused);
        QCOMPARE(bmc.logins, 2);
        QTest::qWait(500);
        QCOMPARE(identified(device).error().message, refused);
        QCOMPARE(bmc.logins, 3);
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
        // As a kvmd's: the root anybody may read, read on the sign-in's own line before every sign-in (where nothing is
        // kept to compare it with, too), under the guess; then the sign-in, never "not there" on a guess.
        QFETCH(const int, line);
        RedfishByHand bmc(RedfishByHand::Layout::Ami, line == 2 ? std::nullopt : std::optional<Tls>(tls("first")));
        RedfishDevice::Where where = at(bmc);
        where.knownByCertificate = line != 2;
        where.certificate = line != 2 ? sha256Of("first") : QByteArray();
        RedfishDevice::Pace pace = quick();
        pace.connectWithinMs = 4000; // (a TLS handshake under the address sanitizer on a busy PC)
        pace.signInQuietMs = 8000;   // (the sign-in's own wait, longer than its line's)
        if (line == 0) {
            QVERIFY(bmc.stand->letInOnlyAfter(60'000));
        } else if (line == 1) {
            // (Only the line after the question asked first: the one the sign-in goes on.)
            bmc.stand->beforeAnswer = [&bmc, slow = pace.connectWithinMs + 500, slowed = false]() mutable {
                const bool signIn = bmc.stand->asked.constLast().method == "POST";
                if (signIn || !std::exchange(slowed, true)) {
                    bmc.stand->letInOnlyAfter(signIn ? 0 : slow);
                }
            };
        } else {
            bmc.stand->closesSilently = true;
        }
        RedfishDevice device(where, u"secret"_s, pace);
        QElapsedTimer watch;
        watch.start();
        const auto what = identified(device);
        bmc.stand->beforeAnswer = nullptr;
        if (line == 0) {
            QCOMPARE(codeOf(what), std::string("unreachable"));
            QVERIFY(watch.elapsed() < pace.connectWithinMs + pace.quietMs);
            QCOMPARE(bmc.logins, 0);
            QVERIFY(!pace.pacing->standing(SignInPacing::what(u"login", bmc.address(), u"admin"_s, u"secret"_s)));
            return;
        }
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(bmc.logins, 1);
    }

    void twoDriversSigningInAtOnceSendThePasswordOneAtATime()
    {
        // The hold is looked at again just before the sign-in is counted (its root, read first, is a round trip): two
        // drivers of one controller (the wall's and a box's) never have the password out at once. The other waits for
        // what the first comes to (as the web's second asker waits for the one sign-in), and after a yes signs in
        // itself; it is not told the controller "does not sign qKVM in" meanwhile.
        RedfishByHand bmc;
        bmc.loginTakesMs = 1000;
        const RedfishDevice::Pace pace = quick();
        RedfishDevice one(at(bmc), u"secret"_s, pace);
        RedfishDevice other(at(bmc), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<RedfishDevice::Identity>& said) { codes.append(codeOf(said)); };
        one.identify(keep);
        other.identify(keep);
        QTRY_COMPARE(bmc.logins, 1);
        QTest::qWait(500);
        QVERIFY(codes.isEmpty());
        QCOMPARE(bmc.logins, 1);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(codes, (QList<std::string>{"ok", "ok"}));
        QCOMPARE(bmc.logins, 2);
    }

    void threeDriversSignInOneAtATimeAndEachIsLetIn()
    {
        // (The pacing's second reading, L3.) Two wait for the first's yes; told at once, one of them sends first, and
        // the other waits again for that one (within its own time) rather than being told the controller does not sign
        // it in.
        RedfishByHand bmc;
        bmc.loginTakesMs = 1000;
        const RedfishDevice::Pace pace = quick();
        RedfishDevice one(at(bmc), u"secret"_s, pace);
        RedfishDevice two(at(bmc), u"secret"_s, pace);
        RedfishDevice three(at(bmc), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<RedfishDevice::Identity>& said) { codes.append(codeOf(said)); };
        one.identify(keep);
        QTRY_COMPARE(bmc.logins, 1);
        two.identify(keep);
        three.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 3, 20000);
        QCOMPARE(codes, (QList<std::string>{"ok", "ok", "ok"}));
        QCOMPARE(bmc.logins, 3);
    }

    void threeDriversOnASlowBoardAreEachLetIn()
    {
        // (The pacing's third reading, L4.) The slow board answers its root in 12 to 20 s, and each driver told reads
        // it again before it looks: the third, which waited for the first and then for the second, is not cut off by a
        // time counted from its first wait while the second's sign-in is out (here a root of 7 s, and a sign-in that
        // can be out 6.75).
        RedfishByHand bmc;
        bmc.loginTakesMs = 1000;
        bmc.rootUnsignedTakesMs = 7000;
        RedfishDevice::Pace pace = quick();
        pace.rootUnsignedMs = 15'000;
        RedfishDevice one(at(bmc), u"secret"_s, pace);
        RedfishDevice two(at(bmc), u"secret"_s, pace);
        RedfishDevice three(at(bmc), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<RedfishDevice::Identity>& said) { codes.append(codeOf(said)); };
        one.identify(keep);
        two.identify(keep);
        three.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 3, 60'000);
        QCOMPARE(codes, (QList<std::string>{"ok", "ok", "ok"}));
        QCOMPARE(bmc.logins, 3);
    }

    void aWaiterToldAsAnotherHasJustSentWaitsAgain()
    {
        // (The pacing's second reading, L3, as a kvmd's.) Told at a yes, it finds another's sign-in out by the time it
        // looks: it waits again for that one, within its own time, and is then let in.
        RedfishByHand bmc;
        const RedfishDevice::Pace pace = quick();
        const SignInPacing::Mark mark = SignInPacing::what(u"login", bmc.address(), u"admin"_s, u"secret"_s);
        const SignInPacing::Steps steps{
            pace.refusedHoldMs, pace.signInRetryMs,
            SignInPacing::outFor(pace.connectWithinMs, pace.signInQuietMs, pace.transferMs)};
        pace.pacing->sent(mark, steps); // (another driver's, out)
        RedfishDevice wall(at(bmc), u"secret"_s, pace);
        QList<std::string> codes;
        wall.identify([&codes](const base::Result<RedfishDevice::Identity>& said) { codes.append(codeOf(said)); });
        QTest::qWait(300);
        QVERIFY(codes.isEmpty());
        // Its yes, and at once another's goes out, before the one that waits is told.
        pace.pacing->accepted(mark);
        pace.pacing->sent(mark, steps);
        QTest::qWait(300);
        QVERIFY(codes.isEmpty());
        QCOMPARE(bmc.logins, 0);
        pace.pacing->accepted(mark);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 1, 10000);
        QCOMPARE(codes, QList<std::string>{"ok"});
        QCOMPARE(bmc.logins, 1);
    }

    void aSecondDriverWaitsForASlowNoAndSendsNothing()
    {
        // (The pacing's reading, M1.) The slow board answered in 12 to 20 seconds: a no slower than the wait after a
        // sign-in that came to nothing (here 1.5 against 0.8). The other driver sends the wrong password no second
        // time: a controller locks an account after a few.
        RedfishByHand bmc;
        bmc.password = u"changed on the controller"_s;
        bmc.loginTakesMs = 1500;
        const RedfishDevice::Pace pace = quick();
        RedfishDevice one(at(bmc), u"secret"_s, pace);
        RedfishDevice other(at(bmc), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<RedfishDevice::Identity>& said) { codes.append(codeOf(said)); };
        one.identify(keep);
        QTRY_COMPARE(bmc.logins, 1);
        QTest::qWait(pace.signInRetryMs + 200);
        QVERIFY(codes.isEmpty());
        other.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(bmc.logins, 1);
        QCOMPARE(codes, (QList<std::string>{"auth_failed", "auth_failed"}));
    }

    void whatMayHaveBeenDone()
    {
        // (The web's server: "timeout" and "dropped" may have been done; what never went out was not.)
        for (const char* code : {"timeout", "dropped"}) {
            QVERIFY(RedfishDevice::mayHaveBeenDone(base::Error{code, ""}));
        }
        for (const char* code : {"unreachable", "cert_mismatch", "cert_invalid", "no_sign_in", "auth_failed",
                                 "no_password", "another_machine", "invalid", "not_yet", "action_failed"}) {
            QVERIFY(!RedfishDevice::mayHaveBeenDone(base::Error{code, ""}));
        }
    }
};

QTEST_GUILESS_MAIN(TestRedfishSignIn)
#include "test_redfish_sign_in.moc"
