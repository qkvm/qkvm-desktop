#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QTimer>

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

namespace {

// Asks the driver for a way into its console, and waits for the answer.
base::Result<RedfishDevice::ConsoleWay> wayOf(RedfishDevice& device)
{
    QList<base::Result<RedfishDevice::ConsoleWay>> answers;
    device.consoleWay([&](base::Result<RedfishDevice::ConsoleWay> answer) { answers.append(std::move(answer)); });
    if (!answers.isEmpty()) {
        return base::Error{"at_once", "The answer came before the call came back."};
    }
    if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 30000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

std::string codeOf(const base::Result<RedfishDevice::ConsoleWay>& what)
{
    return what ? std::string("ok") : what.error().code;
}

SignInPacing::Mark webOf(const RedfishByHand& bmc)
{
    return SignInPacing::what(u"web", bmc.address(), u"admin"_s, u"secret"_s);
}

// Short times, and which server answers asked through its moments quickly.
RedfishDevice::Pace brisk()
{
    RedfishDevice::Pace pace = quick();
    pace.questionAgainMs = 100;
    return pace;
}

} // namespace

// A server's management controller spoken to by the program itself over Redfish (redfish_device.h),
// against a stand-in that answers as the web's does (redfish_by_hand.h): the way into its console, its
// own web page signed in to and the pass it hands out (3a). Ported from the web's own tests where they
// are named (../app/tests).
class TestRedfishConsole : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The driver chooses Windows' own TLS, which is refused once TLS is in use: so one is made before
        // anything else touches TLS.
        const RedfishDevice first(RedfishDevice::Where{}, QString(), quick());
    }

    void c1TheWayInIsItsWebPagesSessionAndAPassForOneConsoleSession()
    {
        // (redfish.py:1655-1760) Its web page signed in to with the form it takes, the pass asked for with that page's
        // session (its cookie and the token beside it), and what the console needs handed on; that page's session given
        // back when the way is done with, once.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        const auto way = wayOf(device);
        QVERIFY2(way, way ? "" : way.error().message.c_str());
        QCOMPARE(bmc.webLogins, 1);
        QCOMPARE(bmc.webLoginBodies, QList<QByteArray>{"username=admin&password=secret"_ba});
        QCOMPARE(way.value().address, bmc.address());
        QVERIFY(way.value().cookie.startsWith("QSESSIONID="));
        QVERIFY(bmc.webSessions.contains(way.value().cookie.mid(11)));
        QVERIFY(bmc.consolePasses.contains(way.value().pass));
        QCOMPARE(way.value().ownAddress, "127.0.0.1"_ba);
        QCOMPARE(way.value().controllerAddress, "127.0.0.1"_ba);
        QVERIFY(!way.value().webSession.isEmpty());
        QCOMPARE(way.value().completeFirst, std::optional<bool>(false));
        // Whether it has a console, asked with nothing (and so the question asked first before the password goes);
        // the pass, with that page's session.
        QCOMPARE(bmc.tokenAsks, 2);
        const Asked* pass = nullptr;
        for (const Asked& one : std::as_const(bmc.stand->asked)) {
            if (one.path == "/api/kvm/token" && one.headers.contains("cookie")) {
                pass = &one;
            }
        }
        QVERIFY(pass != nullptr);
        QCOMPARE(pass->headers.value("cookie"), way.value().cookie);
        QCOMPARE(pass->headers.value("x-csrftoken"), bmc.webSessions.value(way.value().cookie.mid(11)));
        QVERIFY(!pace.pacing->standing(webOf(bmc)));
        // Never Basic; and a cookie only to its web page, which signs in so.
        QCOMPARE(bmc.authorizations, 0);
        for (const Asked& one : std::as_const(bmc.stand->asked)) {
            QVERIFY(!one.headers.contains("cookie") || one.path.startsWith("/api/"));
        }
        // Done with: given back, once.
        way.value().done();
        QTRY_COMPARE(bmc.webSignedOut, 1);
        QVERIFY(bmc.webSessions.isEmpty());
        way.value().done();
        QTest::qWait(300);
        QCOMPARE(pace.goodbyes->given(), 1);
    }

    void c2ItsFirmwareSaysWhetherAConsoleMayReconnect()
    {
        // Asked once a driver: where it says so, the console is greeted "the connection is complete" first.
        RedfishByHand bmc;
        bmc.reconnects = true;
        RedfishDevice device(at(bmc), u"secret"_s, brisk());
        auto way = wayOf(device);
        QVERIFY(way);
        QCOMPARE(way.value().completeFirst, std::optional<bool>(true));
        way.value().done();
        way = wayOf(device);
        QVERIFY(way);
        QCOMPARE(way.value().completeFirst, std::optional<bool>(true));
        QCOMPARE(bmc.stand->count("/api/configuration/project"), 1);
        // (Whether it has a console, asked once a driver; and each way's pass, with its session.)
        QCOMPARE(bmc.tokenAsks, 3);
        way.value().done();
        // One that says nothing of what it can do: no.
        RedfishByHand silent;
        silent.saysWhatItCan = false;
        RedfishDevice other(at(silent), u"secret"_s, brisk());
        const auto plain = wayOf(other);
        QVERIFY(plain);
        QCOMPARE(plain.value().completeFirst, std::optional<bool>(false));
        plain.value().done();
    }

    void c3AControllerWithoutSuchAConsoleIsNotOfferedThePassword()
    {
        RedfishByHand bmc;
        bmc.ownConsole = false;
        RedfishDevice device(at(bmc), u"secret"_s, brisk());
        const auto none = wayOf(device);
        QCOMPARE(codeOf(none), std::string("no_picture"));
        QCOMPARE(none.error().message, std::string("This controller has no console that qKVM reads yet"));
        QCOMPARE(bmc.webLogins, 0);
        QCOMPARE(bmc.logins, 0);
        // Known from then on.
        QCOMPARE(codeOf(wayOf(device)), std::string("no_picture"));
        QCOMPARE(bmc.tokenAsks, 1);
    }

    void c4ItsWebPageRefusingThePasswordIsANoOfItsOwnRoad()
    {
        // (redfish.py:1643-1660) A no there counts against the same account: not offered again before its time. The
        // controller's own road is another, and goes on; a password typed anew is tried again on both.
        RedfishByHand bmc;
        bmc.webLoginStatus = 401;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        const auto refused = wayOf(device);
        QCOMPARE(codeOf(refused), std::string("no_picture"));
        QCOMPARE(refused.error().message,
                 std::string("The controller's web page did not sign this user in (HTTP 401)"));
        QCOMPARE(pace.pacing->times(webOf(bmc)), 1);
        const auto again = wayOf(device);
        QCOMPARE(codeOf(again), std::string("no_picture"));
        QCOMPARE(again.error().message,
                 std::string("The controller's web page rejected the saved login: it is not offered again yet"));
        QCOMPARE(bmc.webLogins, 1);
        QVERIFY(identified(device));
        RedfishDevice::tryAfresh(at(bmc), u"secret"_s, pace);
        bmc.webLoginStatus = 0;
        const auto now = wayOf(device);
        QVERIFY2(now, now ? "" : now.error().message.c_str());
        QCOMPARE(bmc.webLogins, 2);
        now.value().done();
    }

    void c5AnAnswerOfItsWebPageThatSaysNeitherIsNoNo()
    {
        RedfishByHand bmc;
        bmc.webLoginStatus = 503;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QCOMPARE(wayOf(device).error().message,
                 std::string("The controller's web page did not sign this user in (HTTP 503)"));
        QCOMPARE(pace.pacing->times(webOf(bmc)), 0);
        QCOMPARE(codeOf(wayOf(device)), std::string("no_sign_in"));
        QCOMPARE(bmc.webLogins, 1);
    }

    void c6WhichServerIsAskedBeforeItsWebPageIsOfferedThePassword_data()
    {
        QTest::addColumn<bool>("itsControllerToo");
        QTest::newRow("another controller") << true;
        QTest::newRow("its controller's number the same") << false;
    }

    void c6WhichServerIsAskedBeforeItsWebPageIsOfferedThePassword()
    {
        // (test_which_server_answers.py:133) The new lease comes before the screen is asked for: its web page is not
        // offered the password, and no pass is asked for.
        QFETCH(bool, itsControllerToo);
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, brisk());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.beServer(bSystem, itsControllerToo ? bRoot : aRoot);
        const auto refused = wayOf(device);
        QCOMPARE(codeOf(refused), std::string("another_machine"));
        QCOMPARE(bmc.webLogins, 0);
        QCOMPARE(bmc.logins, itsControllerToo ? 1 : 2); // (its controller's number asked first, where it is another)
        QVERIFY(bmc.consolePasses.isEmpty());
    }

    void c7WhichServerUnreadableJustThenOffersItsWebPageNothing()
    {
        // (test_which_server_answers.py:153) Its computer answers an error of its own: asked through a few times, then
        // said; its web page not offered the password on the strength of nothing. Then as ever.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, brisk());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.systemUnwell = true;
        const qsizetype before = bmc.asked(u"GET "_s + bmc.system());
        QCOMPARE(codeOf(wayOf(device)), std::string("bad_response"));
        QCOMPARE(bmc.asked(u"GET "_s + bmc.system()) - before, 3);
        QVERIFY(bmc.tokenAsks > 0);
        QCOMPARE(bmc.webLogins, 0);
        bmc.systemUnwell = false;
        const auto way = wayOf(device);
        QVERIFY(way);
        way.value().done();
    }

    void c8ANewLeaseBetweenItsWebSignInAndThePassHandsNoPassOn()
    {
        // (test_which_server_answers.py:171) The other server's web page signs this program in: asked again which
        // server answers before the pass, and that page's session given back. Nothing of its console is handed on.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, brisk());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "POST" && asked.path == "/api/session" && bmc.uuid == QJsonValue(aSystem)) {
                bmc.beServer(bSystem, aRoot);
            }
        };
        const auto refused = wayOf(device);
        QCOMPARE(codeOf(refused), std::string("another_machine"));
        QCOMPARE(bmc.webLogins, 1);
        QVERIFY(bmc.consolePasses.isEmpty());
        QTRY_COMPARE(bmc.webSignedOut, 1);
        bmc.whenAsked = nullptr;
    }

    void c9AConsoleThatMayDarkenTheServersMonitorIsLeftAlone_data()
    {
        QTest::addColumn<QJsonValue>("automaticOff");
        QTest::addColumn<QJsonValue>("localMonitorOff");
        QTest::addColumn<QString>("why"); // nothing: it is opened
        const QString darkens = u"The controller is set to switch the server's own monitor off while its console is "
                                "open, so qKVM leaves the console alone"_s;
        const QString unsaid =
            u"The controller does not say whether it switches the server's own monitor off while its "
            "console is open, so qKVM leaves the console alone"_s;
        const QJsonValue none(QJsonValue::Undefined);
        // Both said, and both a plain no: opened.
        QTest::newRow("both a plain no, as a real board says it") << QJsonValue(0) << QJsonValue(0) << QString();
        QTest::newRow("both a plain no in other words") << QJsonValue(u" Off"_s) << QJsonValue(false) << QString();
        QTest::newRow("both a plain no, in more words")
            << QJsonValue(u"disabled"_s) << QJsonValue(u"no"_s) << QString();
        // Anything else: left alone, and the card says why.
        QTest::newRow("both a yes") << QJsonValue(1) << QJsonValue(1) << darkens;
        QTest::newRow("the first a yes") << QJsonValue(1) << QJsonValue(0) << darkens;
        QTest::newRow("the second a yes") << QJsonValue(0) << QJsonValue(1) << darkens;
        QTest::newRow("a yes said as a word") << QJsonValue(u"on"_s) << QJsonValue(0) << darkens;
        QTest::newRow("a yes said as true") << QJsonValue(false) << QJsonValue(true) << darkens;
        QTest::newRow("a number that is not nought") << QJsonValue(0) << QJsonValue(0.5) << darkens;
        QTest::newRow("a word that cannot be read") << QJsonValue(QJsonArray{}) << QJsonValue(0) << darkens;
        // The owner's decision of 2026-11-05: a word that is not said at all is not a no.
        QTest::newRow("automatic_off not said") << none << QJsonValue(0) << unsaid;
        QTest::newRow("local_monitor_off not said") << QJsonValue(0) << none << unsaid;
        QTest::newRow("neither said") << none << none << unsaid;
        QTest::newRow("said as nothing") << QJsonValue(QJsonValue::Null) << QJsonValue(0) << unsaid;
        QTest::newRow("one not said, the other a yes") << none << QJsonValue(1) << darkens;
    }

    void c9AConsoleThatMayDarkenTheServersMonitorIsLeftAlone()
    {
        // (test_redfish.py:1329, and the owner's decision of 2026-11-05) Whoever stands at the server would see that:
        // not "only looking" any more. The console is opened only where both words are said, and both are a plain no.
        QFETCH(QJsonValue, automaticOff);
        QFETCH(QJsonValue, localMonitorOff);
        QFETCH(QString, why);
        RedfishByHand bmc;
        bmc.automaticOff = automaticOff;
        bmc.localMonitorOff = localMonitorOff;
        RedfishDevice device(at(bmc), u"secret"_s, brisk());
        const auto way = wayOf(device);
        if (why.isEmpty()) {
            QVERIFY2(way.ok(), way.ok() ? "" : way.error().message.c_str());
            QVERIFY(bmc.consolePasses.contains(way.value().pass));
            return;
        }
        QCOMPARE(codeOf(way), std::string("no_picture"));
        QCOMPARE(QString::fromStdString(way.error().message), why);
        QTRY_COMPARE(bmc.webSignedOut, 1);
    }

    void c10AWebSignInOutWhenItsDriverGoesIsGivenBackWhenItComes()
    {
        // (redfish.py:1675-1702) The controller makes the session when it answers, wanted or not: given back.
        RedfishByHand bmc;
        bmc.webSignInTakesMs = 600;
        const RedfishDevice::Pace pace = brisk();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        QList<base::Result<RedfishDevice::ConsoleWay>> answers;
        device->consoleWay(
            [&answers](base::Result<RedfishDevice::ConsoleWay> said) { answers.append(std::move(said)); });
        QTRY_COMPARE(bmc.webLogins, 1);
        device.reset();
        QTRY_COMPARE(answers.size(), 1);
        QCOMPARE(codeOf(answers.first()), std::string("stopped"));
        QTRY_COMPARE(bmc.webSignedOut, 1);
        QVERIFY(bmc.webSessions.isEmpty());
        QVERIFY(!pace.pacing->standing(webOf(bmc)));
    }

    void c12WhatItsWebPageHandsOutIsTakenOnlyInItsShape_data()
    {
        QTest::addColumn<QString>("token");
        QTest::addColumn<bool>("noCookie");
        QTest::addColumn<QString>("pass");
        QTest::newRow("a token that is none") << u"a token"_s << false << QString();
        QTest::newRow("no cookie") << QString() << true << QString();
        QTest::newRow("a pass that is none") << QString() << false << u"a pass"_s;
    }

    void c12WhatItsWebPageHandsOutIsTakenOnlyInItsShape()
    {
        // (redfish.py:197-198, HANDED and CONSOLE_PASS) What goes back to it in a header, and what goes into a message
        // of its console: none that could end the one or say something else in the other. A sign-in that hands out none
        // in its shape is no sign-in (and no no); a pass that is none is no way in, and the session is given back.
        QFETCH(QString, token);
        QFETCH(bool, noCookie);
        QFETCH(QString, pass);
        RedfishByHand bmc;
        bmc.webTokenIs = token;
        bmc.webGivesNoCookie = noCookie;
        bmc.passIs = pass;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        const auto none = wayOf(device);
        QCOMPARE(codeOf(none), std::string("no_picture"));
        if (pass.isNull()) {
            QCOMPARE(none.error().message,
                     std::string("The controller's web page did not sign this user in (HTTP 200)"));
            QCOMPARE(pace.pacing->times(webOf(bmc)), 0);
            QVERIFY(bmc.consolePasses.isEmpty());
        } else {
            QCOMPARE(none.error().message, std::string("The controller gave no pass for its console (HTTP 200)"));
            QTRY_COMPARE(bmc.webSignedOut, 1);
        }
    }

    void c13ThePasswordGoesToItsWebPageOnlyAsFormEncoded()
    {
        // What would end a field of the form, or begin another, goes escaped: the password is the password.
        RedfishByHand bmc;
        bmc.password = u"a&b=c%d+e\nf"_s;
        RedfishDevice device(at(bmc), bmc.password, brisk());
        const auto way = wayOf(device);
        QVERIFY2(way, way ? "" : way.error().message.c_str());
        QCOMPARE(bmc.webLoginBodies, QList<QByteArray>{"username=admin&password=a%26b%3Dc%25d%2Be%0Af"_ba});
        way.value().done();
    }

    void c14ACookieThatIsNoneIsNoSignIn_data()
    {
        QTest::addColumn<QByteArray>("cookie");
        QTest::newRow("a backslash") << "ab\\cd"_ba;
        QTest::newRow("a sign that is not ASCII") << QByteArray("ab\xe9"
                                                                "cd");
    }

    void c14ACookieThatIsNoneIsNoSignIn()
    {
        // (HANDED) What goes back to it in a header: none that could say something else there.
        QFETCH(QByteArray, cookie);
        RedfishByHand bmc;
        bmc.webCookieIs = cookie;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        const auto none = wayOf(device);
        QCOMPARE(codeOf(none), std::string("no_picture"));
        QCOMPARE(none.error().message, std::string("The controller's web page did not sign this user in (HTTP 200)"));
        QVERIFY(bmc.consolePasses.isEmpty());
    }

    void c15ASlowNoOfItsWebPageIsHeardBeforeAnotherDriverSendsThePassword()
    {
        // (The pacing's reading, M1.) Two drivers of one controller (the wall's and a box's) ask for its console, the
        // second once the wait after a web sign-in that came to nothing is over (as the program's ten seconds) and the
        // first's is still out: its no is slow (the slow board's 12 to 20 seconds; here 1.5 against 0.8). The hold
        // lasts as long as that sign-in can be out: the password goes once, and the no is the other's too.
        RedfishByHand bmc;
        bmc.webLoginStatus = 401;
        bmc.webSignInTakesMs = 1500;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice one(at(bmc), u"secret"_s, pace);
        RedfishDevice other(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(one));
        QVERIFY(identified(other));
        QList<base::Result<RedfishDevice::ConsoleWay>> answers;
        const auto keep = [&answers](base::Result<RedfishDevice::ConsoleWay> said) { answers.append(std::move(said)); };
        one.consoleWay(keep);
        QTRY_COMPARE(bmc.webLogins, 1);
        QTest::qWait(pace.signInRetryMs + 200);
        QVERIFY(answers.isEmpty());
        other.consoleWay(keep);
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 2, 10000);
        QCOMPARE(bmc.webLogins, 1);
        QStringList said;
        for (const auto& answer : std::as_const(answers)) {
            QCOMPARE(codeOf(answer), std::string("no_picture"));
            said.append(QString::fromStdString(answer.error().message));
        }
        said.sort();
        QCOMPARE(said,
                 (QStringList{u"The controller's web page did not sign this user in (HTTP 401)"_s,
                              u"The controller's web page rejected the saved login: it is not offered again yet"_s}));
    }

    void c15bAnotherDriverWaitsForItsWebPagesYesAndThenSignsInItself()
    {
        // (The pacing's reading, L3.) The other driver, asking while the first's web sign-in is out, is not told the
        // controller "does not sign qKVM in": it waits for that yes, and signs in itself after. One at a time.
        RedfishByHand bmc;
        bmc.webSignInTakesMs = 1200;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice one(at(bmc), u"secret"_s, pace);
        RedfishDevice other(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(one));
        QVERIFY(identified(other));
        QList<base::Result<RedfishDevice::ConsoleWay>> answers;
        const auto keep = [&answers](base::Result<RedfishDevice::ConsoleWay> said) { answers.append(std::move(said)); };
        one.consoleWay(keep);
        QTRY_COMPARE(bmc.webLogins, 1);
        other.consoleWay(keep);
        QTest::qWait(600);
        QVERIFY(answers.isEmpty());
        QCOMPARE(bmc.webLogins, 1);
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 2, 10000);
        for (const auto& said : std::as_const(answers)) {
            QVERIFY2(said, said ? "" : said.error().message.c_str());
            said.value().done();
        }
        QCOMPARE(bmc.webLogins, 2);
        QTRY_COMPARE(bmc.webSignedOut, 2);
    }

    void c15dTwoDriversAskingAtOnceSignInToItsWebPageOneAtATime()
    {
        // Both ask at once, and both find nothing held: the hold is looked at again just before the sign-in is counted
        // (its root, read first on that page's line, is a round trip). The other waits for the first's yes, then signs
        // in itself.
        RedfishByHand bmc;
        bmc.webSignInTakesMs = 1000;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice one(at(bmc), u"secret"_s, pace);
        RedfishDevice other(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(one));
        QVERIFY(identified(other));
        QList<base::Result<RedfishDevice::ConsoleWay>> answers;
        const auto keep = [&answers](base::Result<RedfishDevice::ConsoleWay> said) { answers.append(std::move(said)); };
        one.consoleWay(keep);
        other.consoleWay(keep);
        QTRY_COMPARE(bmc.webLogins, 1);
        QTest::qWait(500);
        QVERIFY(answers.isEmpty());
        QCOMPARE(bmc.webLogins, 1);
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 2, 10000);
        for (const auto& said : std::as_const(answers)) {
            QVERIFY2(said, said ? "" : said.error().message.c_str());
            said.value().done();
        }
        QCOMPARE(bmc.webLogins, 2);
        QTRY_COMPARE(bmc.webSignedOut, 2);
    }

    void c15cOneDriverHasOneWebSignInOutAtATime()
    {
        // (The pacing's reading, M2.) Two askings of one driver: never two of its web page's sign-ins out at once, not
        // even where the pacing holds nothing (here it is cleared). The second waits for what the first comes to, and
        // is asked through again after; each way's session given back.
        RedfishByHand bmc;
        bmc.webSignInTakesMs = 1000;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(device));
        QList<base::Result<RedfishDevice::ConsoleWay>> answers;
        const auto keep = [&answers](base::Result<RedfishDevice::ConsoleWay> said) { answers.append(std::move(said)); };
        device.consoleWay(keep);
        QTRY_COMPARE(bmc.webLogins, 1);
        pace.pacing->clear();
        device.consoleWay(keep);
        QTest::qWait(600);
        QCOMPARE(bmc.webLogins, 1);
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 2, 10000);
        for (const auto& said : std::as_const(answers)) {
            QVERIFY2(said, said ? "" : said.error().message.c_str());
            said.value().done();
        }
        QCOMPARE(bmc.webLogins, 2);
        QTRY_COMPARE(bmc.webSignedOut, 2);
    }

    void c15eThreeDriversSignInToItsWebPageOneAtATimeAndEachIsLetIn()
    {
        // (The pacing's second reading, L3.) The one told second of two that waited for the first's yes waits again for
        // the one that went first, rather than being told the controller does not sign it in.
        RedfishByHand bmc;
        bmc.webSignInTakesMs = 1000;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice one(at(bmc), u"secret"_s, pace);
        RedfishDevice two(at(bmc), u"secret"_s, pace);
        RedfishDevice three(at(bmc), u"secret"_s, pace);
        QVERIFY(identified(one) && identified(two) && identified(three));
        QList<base::Result<RedfishDevice::ConsoleWay>> answers;
        const auto keep = [&answers](base::Result<RedfishDevice::ConsoleWay> said) { answers.append(std::move(said)); };
        one.consoleWay(keep);
        QTRY_COMPARE(bmc.webLogins, 1);
        two.consoleWay(keep);
        three.consoleWay(keep);
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 3, 20000);
        for (const auto& said : std::as_const(answers)) {
            QVERIFY2(said, said ? "" : said.error().message.c_str());
            said.value().done();
        }
        QCOMPARE(bmc.webLogins, 3);
        QTRY_COMPARE(bmc.webSignedOut, 3);
    }

    void c16AWebSignInThatNothingAnswersIsHeldAndNotSentAgainAtOnce()
    {
        RedfishByHand bmc;
        bmc.webLoginNever = true;
        RedfishDevice::Pace pace = brisk();
        pace.signInQuietMs = 800;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QCOMPARE(codeOf(wayOf(device)), std::string("timeout"));
        QVERIFY(pace.pacing->waitLeftMs(webOf(bmc)) > 0);
        QCOMPARE(pace.pacing->times(webOf(bmc)), 0);
        QCOMPARE(codeOf(wayOf(device)), std::string("no_sign_in"));
        QCOMPARE(bmc.webLogins, 1);
        // What it came to replaced the hold it went out with (as long as it could be out): held as one that came to
        // nothing, and sent again after that wait.
        QTest::qWait(pace.signInRetryMs + 200);
        QCOMPARE(codeOf(wayOf(device)), std::string("timeout"));
        QCOMPARE(bmc.webLogins, 2);
    }

    void c16bAWebSignInAnsweredWithNeitherIsHeldAsSuchAndNotAsLongAsItWentOutWith()
    {
        // Every session of its web page given out (503): no word on the password. Held as one that came to nothing, and
        // sent again after that wait, not after the time it could have been out.
        RedfishByHand bmc;
        bmc.webLoginStatus = 503;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QCOMPARE(codeOf(wayOf(device)), std::string("no_picture"));
        QCOMPARE(codeOf(wayOf(device)), std::string("no_sign_in"));
        QCOMPARE(pace.pacing->times(webOf(bmc)), 0);
        QCOMPARE(bmc.webLogins, 1);
        QTest::qWait(pace.signInRetryMs + 200);
        QCOMPARE(codeOf(wayOf(device)), std::string("no_picture"));
        QCOMPARE(bmc.webLogins, 2);
    }

    void c17ASessionHeldWhileThePassIsOutIsGivenBackWhenItsDriverGoes()
    {
        RedfishByHand bmc;
        bmc.tokenTakesMs = 600;
        const RedfishDevice::Pace pace = brisk();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        QList<base::Result<RedfishDevice::ConsoleWay>> answers;
        device->consoleWay(
            [&answers](base::Result<RedfishDevice::ConsoleWay> said) { answers.append(std::move(said)); });
        QTRY_COMPARE(bmc.tokenAsks, 2); // (the pass, asked with the session)
        device.reset();
        QTRY_COMPARE(answers.size(), 1);
        QCOMPARE(codeOf(answers.first()), std::string("stopped"));
        QTRY_COMPARE(bmc.webSignedOut, 1);
    }

    void c18AWayLetGoOfWithoutItsDoneGivesItsSessionBack()
    {
        RedfishByHand bmc;
        RedfishDevice device(at(bmc), u"secret"_s, brisk());
        {
            const auto way = wayOf(device);
            QVERIFY(way);
        }
        QTRY_COMPARE(bmc.webSignedOut, 1);
    }

    void c19ItsWebPagesOwnLineIsAskedWhichControllerItIsBeforeThePasswordGoes()
    {
        // The question about the server goes on the line of the controller's own road; the password to its web page on
        // a line of that page's. Its root, on that line, is compared first: another controller there hears nothing.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, brisk());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bool once = false;
        bmc.whenAsked = [&](const Asked& asked) {
            if (!once && asked.method == "GET" && asked.path == bmc.system().toLatin1()) {
                once = true;
                QTimer::singleShot(0, &bmc, [&bmc] { bmc.beServer(bSystem, bRoot); }); // (after its answer)
            }
        };
        QCOMPARE(codeOf(wayOf(device)), std::string("another_machine"));
        QCOMPARE(bmc.webLogins, 0);
        bmc.whenAsked = nullptr;
    }

    void c11NoPasswordNothingAsked()
    {
        RedfishByHand bmc;
        RedfishDevice device(at(bmc), QString(), brisk());
        QCOMPARE(codeOf(wayOf(device)), std::string("no_password"));
        QVERIFY(bmc.stand->asked.isEmpty());
    }

    void c20AWayDoneWithOnAnotherThreadIsGivenBackThroughItsGoodbyesDoor()
    {
        // (The pacing's reading, L5.) A console's screen may live on a thread of its own, and its way is done with
        // there: given back on the goodbyes' own loop, through a door their going closes under the same lock.
        RedfishByHand bmc;
        const RedfishDevice::Pace pace = brisk();
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        const auto way = wayOf(device);
        QVERIFY2(way, way ? "" : way.error().message.c_str());
        std::thread([&way] { way.value().done(); }).join();
        QTRY_COMPARE(bmc.webSignedOut, 1);
        QCOMPARE(pace.goodbyes->given(), 1);
    }

    void c21AWayDoneWithAfterItsGoodbyesAreGoneSaysSo()
    {
        // Its goodbyes gone (the program has ended): nothing is handed to them, and it is said that the session was not
        // given back.
        RedfishByHand bmc;
        RedfishDevice::Pace pace = brisk();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        const auto way = wayOf(*device);
        QVERIFY2(way, way ? "" : way.error().message.c_str());
        device.reset();
        pace.goodbyes.reset(); // (the last owner of them)
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"web page session was not given back"_s));
        way.value().done();
        QTest::qWait(200);
        QCOMPARE(bmc.webSignedOut, 0);
    }

    void c23AGoodbyeHandedInWhileTheEndIsWaitedForSaysSo()
    {
        // (The pacing's third reading.) The end was noted only after the drain: a goodbye handed in between was queued,
        // never warned of, and never said. It is noted first, under the door's lock, and one handed in while the end is
        // waited for says that it may never be said.
        RedfishByHand silent;
        silent.signOutNever = true;
        const RedfishDevice::Pace pace = brisk();
        auto device = std::make_unique<RedfishDevice>(at(silent), u"secret"_s, pace);
        QVERIFY(identified(*device));
        device.reset(); // (its session given back, on a line that is never answered: the end waits for it)
        QCOMPARE(pace.goodbyes->underWay(), 1);
        const std::shared_ptr<qkvm::core::ControllerGoodbyes::Door> door = pace.goodbyes->door();
        const qkvm::core::DeviceAsker::Where where{silent.address(), false, QByteArray()};
        QTimer::singleShot(100, [door, where] { door->leaveWebPage(where, 500, "QSESSIONID=s\nt"_ba, 500); });
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"given back as the program ends"_s));
        pace.goodbyes->finish(3000);
    }

    void c22AtTheProgramsEndWhatCameThroughTheDoorIsSaidFirst()
    {
        // A way done with on another thread just before the program's end is waited for: queued still, nothing of it
        // out, the end was waited for at once without it. It is said first, and waited for.
        RedfishByHand bmc;
        const RedfishDevice::Pace pace = brisk();
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        const auto way = wayOf(*device);
        QVERIFY2(way, way ? "" : way.error().message.c_str());
        std::thread([&way] { way.value().done(); }).join();
        pace.goodbyes->finish(3000);
        QCOMPARE(bmc.webSignedOut, 1);
        QCOMPARE(pace.goodbyes->underWay(), 0);
        // After the end was waited for, a goodbye says that it may never be said (the loop is ending): a way's, and the
        // controller's own session as its driver goes.
        const auto other = wayOf(*device);
        QVERIFY2(other, other ? "" : other.error().message.c_str());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"given back as the program ends"_s));
        other.value().done();
        QTRY_COMPARE(bmc.webSignedOut, 2); // (here the loop goes on)
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"given back as the program ends"_s));
        device.reset();
        QTRY_COMPARE(bmc.signedOut, 1);
    }
};

QTEST_GUILESS_MAIN(TestRedfishConsole)
#include "test_redfish_console.moc"
