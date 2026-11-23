#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTest>
#include <QTimer>

#include <qkvm/core/redfish_answers.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/setup_overrides.h>
#include <qkvm/core/sign_in_pacing.h>

#include "redfish_by_hand.h"
#include "redfish_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::RedfishDevice;
using qkvm::core::ServerPlace;

namespace {

QByteArray placeSaid(const ServerPlace& at)
{
    return (at.place + (at.amongSeveral ? u" (among several)"_s : u" (alone)"_s)).toUtf8();
}

} // namespace

// A server's management controller spoken to by the program itself over Redfish (redfish_device.h),
// against a stand-in that answers as the web's does (redfish_by_hand.h): which server answers at its address (1a-iv).
// Ported from the web's own tests where they are named (../app/tests).
class TestRedfishWhichServer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The driver chooses Windows' own TLS, which is refused once TLS is in use: so one is made before
        // anything else touches TLS.
        const RedfishDevice first(RedfishDevice::Where{}, QString(), quick());
    }

    void t1NothingKeptTakesBothNumbers()
    {
        // (test_which_server_answers.py:291) Trust on first use, as for a certificate.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        const QString both = markOf(aSystem) + digestOf(aRoot);
        QCOMPARE(what.value().number, std::optional<QString>(markOf(aSystem)));
        QCOMPARE(device.machine(), both);
        QTRY_COMPARE(known.size(), 1);
        QCOMPARE(known.first().at(0).toString(), QString());
        QCOMPARE(known.first().at(1).toString(), both);
        // (Its root read with no session before the password, as before every sign-in, with nothing kept to compare it
        // with; its number taken from that read, under the session the sign-in brought.)
        QCOMPARE(bmc.rootsUnsigned, 1);
        QCOMPARE(bmc.stand->asked.at(1).method, "POST"_ba);
        // Asked again: known, and not taken again.
        QVERIFY(identified(device));
        QCOMPARE(known.size(), 1);
    }

    void t2NothingKeptAComputerWithoutANumberIsKnownByItsController()
    {
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY(what);
        QCOMPARE(what.value().number, std::optional<QString>(QString()));
        QCOMPARE(device.machine(), QString(qkvm::core::redfish::saidNone) + digestOf(aRoot));
    }

    void t3NothingKeptAControllerWithoutANumberIsKnownByItsComputer()
    {
        // (test_which_server_answers.py:375)
        RedfishByHand bmc;
        bmc.beServer(aSystem, QJsonValue());
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY(what);
        QCOMPARE(what.value().number, std::optional<QString>(markOf(aSystem)));
        QCOMPARE(device.machine(), markOf(aSystem));
    }

    void t4NothingKeptAndNoNumbersAtAll()
    {
        // (test_twins.py:139: two whose maker gave them no number are two.) Nothing to know it by: nothing taken.
        RedfishByHand bmc;
        bmc.beServer(zero, QJsonValue());
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        const auto what = identified(device);
        QVERIFY(what);
        QCOMPARE(what.value().number, std::optional<QString>(QString()));
        QCOMPARE(device.machine(), QString());
        QCOMPARE(known.size(), 0);
    }

    void t5NothingKeptAndWhichServerCannotBeToldJustThen()
    {
        // (test_which_server_answers.py:678) Its computer gives no number, and its controller's manager answers an
        // error of the moment: nothing is taken, nor is it taken as one that gives none; the next asking takes it.
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        bmc.managerFails = 1;
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        const auto first = identified(device);
        QCOMPARE(codeOf(first), std::string("bad_response"));
        QVERIFY(QString::fromStdString(first.error().message).contains(u"could not be told"_s));
        QCOMPARE(device.machine(), QString());
        const auto then = identified(device);
        QVERIFY(then);
        QCOMPARE(then.value().number, std::optional<QString>(QString()));
        QCOMPARE(device.machine(), QString(qkvm::core::redfish::saidNone) + digestOf(aRoot));
    }

    void t6KeptItsComputerAloneItLearnsItsController()
    {
        // (test_which_server_answers.py:266, its second half) A card that kept its computer's number alone: its
        // controller's is taken the first time the computer is found to be this one, under that session.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem)), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        const auto what = identified(device);
        QVERIFY(what);
        QCOMPARE(what.value().number, std::optional<QString>(markOf(aSystem)));
        QCOMPARE(device.machine(), markOf(aSystem) + digestOf(aRoot));
        QTRY_COMPARE(known.size(), 1);
        QCOMPARE(known.first().at(0).toString(), markOf(aSystem));
        QCOMPARE(known.first().at(1).toString(), markOf(aSystem) + digestOf(aRoot));
    }

    void t7KeptItsComputerAloneAnotherServerIsRefused()
    {
        RedfishByHand bmc;
        bmc.beServer(bSystem, bRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem)), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("another_machine"));
        QCOMPARE(what.error().message, RedfishDevice::anotherServer().message);
        QCOMPARE(device.machine(), markOf(aSystem));
        QCOMPARE(known.size(), 0);
        // (Nothing kept to compare the controller with: the password went; nothing else did after the computer.)
        QCOMPARE(bmc.logins, 1);
        QCOMPARE(bmc.requests.last(), u"GET "_s + bmc.system());
    }

    void t8KeptItsComputerAloneOneThatSaysNoneIsRefused()
    {
        // (test_which_server_answers.py:436) Not shown as this one, nor done to.
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem)), u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("another_machine"));
        QCOMPARE(what.error().message, RedfishDevice::noServerNumber().message);
    }

    void t9KeptBothTheSameServerIsTaken()
    {
        // (test_which_server_answers.py:133: asked before the password goes)
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        const QString both = markOf(aSystem) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, both), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(what.value().number, std::optional<QString>(markOf(aSystem)));
        QCOMPARE(known.size(), 0);
        // Its root asked with no session, before the password went.
        QCOMPARE(bmc.rootsUnsigned, 1);
        QCOMPARE(bmc.stand->asked.first().path, "/redfish/v1/"_ba);
        QVERIFY(!bmc.stand->asked.first().headers.contains("x-auth-token"));
        QCOMPARE(bmc.stand->asked.at(1).method, "POST"_ba);
    }

    void t10KeptBothAnotherControllerIsSentNothing()
    {
        // (test_which_server_answers.py:86) Neither the password nor anything else.
        RedfishByHand bmc;
        bmc.beServer(bSystem, bRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("another_machine"));
        QCOMPARE(what.error().message, RedfishDevice::anotherController().message);
        QCOMPARE(bmc.logins, 0);
        QVERIFY(bmc.requests.isEmpty());
        QCOMPARE(bmc.stand->asked.size(), 1);
        // Asked again: asked again first, and refused again.
        QCOMPARE(codeOf(identified(device)), std::string("another_machine"));
        QCOMPARE(bmc.rootsUnsigned, 2);
        QCOMPARE(bmc.logins, 0);
    }

    void t11KeptBothAControllerThatSaysNoneIsSentNothing()
    {
        // (test_which_server_answers.py:362)
        RedfishByHand bmc;
        bmc.beServer(aSystem, QJsonValue());
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("another_machine"));
        QCOMPARE(what.error().message, RedfishDevice::noControllerNumber().message);
        QCOMPARE(bmc.logins, 0);
    }

    void t12KeptBothTheSameControllerNumberAnotherComputerIsRefused()
    {
        // (test_which_server_answers.py:113) Another computer behind a controller whose root says this card's number:
        // the password goes, and nothing else; its computer is asked.
        RedfishByHand bmc;
        bmc.beServer(bSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("another_machine"));
        QCOMPARE(what.error().message, RedfishDevice::anotherServer().message);
        QCOMPARE(bmc.logins, 1);
    }

    void t13KeptBothAComputerThatStopsGivingItsNumberIsKnownByItsController()
    {
        // (test_which_server_answers.py:516) Compared before this very session, at the place its computer was found at,
        // which a card added under 0.2.0 keeps: not refused, and nothing changed. (One that keeps no place, as the
        // web's cards, is refused until its computer gives its number: w7, w10; outside review of 0.2.0's Redfish
        // fixes, round 2, L1.)
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        const QString both = markOf(aSystem) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, both, ServerPlace{bmc.system(), false}), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(device.machine(), both);
        // (It gives none now: what it is known by stays as it is.)
        QCOMPARE(what.value().number, std::nullopt);
    }

    void t14KeptNoneSaidAndItsControllerKnownByItsControllerAlone()
    {
        // (test_which_server_answers.py:414) Its computer gave no number when it was added, where its computer was
        // taken, which a card added under 0.2.0 keeps. (One that keeps no place, as the web's cards, passes nothing:
        // w8.)
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        const ServerPlace takenAt{bmc.system(), false};
        RedfishDevice device(keptAs(bmc, kept, takenAt), u"secret"_s, quick());
        QVERIFY(identified(device));
        // Its computer gives a number of its own now: whether it is the card's, started since, or another put in its
        // place, cannot be told, and it is not taken for the card's until the person says so on the card, or the card
        // is changed with its password typed (outside review 2, 3.4, and the owner's decision of 2026-11-22: w16;
        // before: whatever its computer said, it was known by its controller).
        bmc.uuid = aSystem;
        RedfishDevice again(keptAs(bmc, kept, takenAt), u"secret"_s, quick());
        const auto numbered = identified(again);
        QCOMPARE(codeOf(numbered), std::string("another_machine"));
        QCOMPARE(numbered.error().message, RedfishDevice::asksOnItsCard().message);
        QCOMPARE(again.machine(), kept);
        // Another server: refused before the password goes.
        bmc.beServer(bSystem, bRoot);
        const int logins = bmc.logins;
        RedfishDevice other(keptAs(bmc, kept, takenAt), u"secret"_s, quick());
        const auto what = identified(other);
        QCOMPARE(codeOf(what), std::string("another_machine"));
        QCOMPARE(what.error().message, RedfishDevice::anotherController().message);
        QCOMPARE(bmc.logins, logins);
    }

    void t15KeptWhatIsOfNoServersShapeIsNoServer()
    {
        // (redfish.py:381-386) Taken whole for the computer's: no number a server gives equals it.
        for (const QString& kept :
             {u"jetkvm:"_s + digestOf(aSystem), QString(qkvm::core::redfish::saidNone), markOf(aSystem) + u"0"_s}) {
            RedfishByHand bmc;
            bmc.beServer(aSystem, aRoot);
            RedfishDevice device(keptAs(bmc, kept), u"secret"_s, quick());
            const auto what = identified(device);
            QCOMPARE(codeOf(what), std::string("another_machine"));
            QCOMPARE(what.error().message, RedfishDevice::anotherServer().message);
            bmc.beServer(zero, aRoot);
            RedfishDevice none(keptAs(bmc, kept), u"secret"_s, quick());
            QCOMPARE(identified(none).error().message, RedfishDevice::noServerNumber().message);
        }
    }

    void t16AnotherServerAndThenThisOneAgain()
    {
        // (test_which_server_answers.py:86, its end) This card's server answers there again: its own again.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        const QString both = markOf(aSystem) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, both), u"secret"_s, quick());
        QVERIFY(identified(device));
        bmc.beServer(bSystem, bRoot);
        QCOMPARE(codeOf(identified(device)), std::string("another_machine"));
        bmc.beServer(aSystem, aRoot);
        const auto again = identified(device);
        QVERIFY2(again, again ? "" : again.error().message.c_str());
        QCOMPARE(again.value().number, std::optional<QString>(markOf(aSystem)));
        QCOMPARE(device.machine(), both);
    }

    void t17ANewSessionIsComparedAgainAndTheRightServerTaken()
    {
        // (test_which_server_answers.py:337) The session ran out: signed in again, the controller's number asked first.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QVERIFY(identified(device));
        bmc.sessions.clear();
        QVERIFY(identified(device));
        QCOMPARE(bmc.logins, 2);
        QCOMPARE(bmc.rootsUnsigned, 2);
    }

    void t18ARootThatIsNoneJustThenSendsNothing_data()
    {
        QTest::addColumn<bool>("in200");
        QTest::newRow("503") << false;
        QTest::newRow("an error said with 200") << true;
    }

    void t18ARootThatIsNoneJustThenSendsNothing()
    {
        // (redfish.py:1066-1068) Not a service root just then: nothing is judged by that, and nothing sent.
        QFETCH(bool, in200);
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.rootFails = 1;
        bmc.rootFailsIn200 = in200;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("bad_response"));
        QVERIFY(QString::fromStdString(what.error().message).contains(u"which is read before its password goes"_s));
        QCOMPARE(bmc.logins, 0);
        QVERIFY(identified(device));
    }

    void t18bWithNoControllersNumberKeptARootThatIsNoneOfRedfishsSendsNothing_data()
    {
        QTest::addColumn<QString>("kept");
        QTest::addColumn<int>("status");
        const QString server = markOf(aSystem);
        QTest::newRow("nothing kept, 401 (a camera's sign-in)") << QString() << 401;
        QTest::newRow("nothing kept, 404") << QString() << 404;
        QTest::newRow("nothing kept, 200 with no RedfishVersion") << QString() << 200;
        QTest::newRow("nothing kept, 503") << QString() << 503;
        QTest::newRow("its computer's alone, 401") << server << 401;
        QTest::newRow("its computer's alone, 200 with no RedfishVersion") << server << 200;
    }

    void t18bWithNoControllersNumberKeptARootThatIsNoneOfRedfishsSendsNothing()
    {
        // (Outside review of 0.2.0, area 5, HIGH 5.1.) No number of its controller is kept to compare its root with
        // (a card that kept none, or its computer's alone): what answers the root read before the password is still to
        // be a Redfish service, or the password does not go. A camera's sign-in page that took the address (401), a
        // page that is not there, a JSON with no RedfishVersion (before: "nothing to compare", and the password was
        // posted to it). Said as the root's moment, and asked again at the next asking.
        QFETCH(QString, kept);
        QFETCH(int, status);
        RedfishByHand bmc;
        bmc.beServer(aSystem, QJsonValue());
        bmc.rootIsNone = 1;
        bmc.rootIsNoneWith = status;
        RedfishDevice device(keptAs(bmc, kept), u"secret"_s, quick());
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("bad_response"));
        QVERIFY(QString::fromStdString(what.error().message).contains(u"which is read before its password goes"_s));
        QCOMPARE(bmc.rootIsNone, 0);
        QCOMPARE(bmc.logins, 0);
        QVERIFY(bmc.loginBodies.isEmpty());
        const auto then = identified(device);
        QVERIFY2(then, then ? "" : then.error().message.c_str());
        QCOMPARE(bmc.logins, 1);
    }

    void t19ARootOnALineClosedWhileItSatUnusedIsAskedOnceMore()
    {
        // (test_which_server_answers.py:722) Not taken for a controller that is gone: asked once more (Qt does
        // that of a GET itself: test_device_asker.cpp).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.stand->keepsLines = true;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QVERIFY(identified(device));
        bmc.sessions.clear();
        bmc.rootHangsUp = 1;
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(bmc.rootsUnsigned, 3);
    }

    void t20AControllersNumberIsTakenOnlyUnderTheSessionItWasReadUnder()
    {
        // (test_which_server_answers.py:569, the second review's L3) Kept: its computer's number alone. A new lease
        // between the computer's read and the controller's: another controller's number is not taken for this one.
        // (Outside review of 0.2.0, area 5, MEDIUM 5.11: this test once switched to B at the first root read with no
        // session, which became the sign-in's own; B was refused by its computer's number, and the race it names was
        // never reached.) Here A's computer is read under A's session and answered late; meanwhile the address passes
        // to B, and a look signs in to B. A's late answer is judged A's, and its controller's number is then asked:
        // the open root, which B answers now, and B's manager under B's session. Neither is taken for A's controller:
        // they are not of the session A's computer was found under.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        int systems = 0;
        int rootsAfter = -1;
        bool judgedLate = false;
        RedfishDevice device(keptAs(bmc, markOf(aSystem)), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        bmc.systemTakesMs = 900;
        bmc.whenAsked = [&](const Asked& asked) {
            // (The second reading of its computer under A's session: the one it is judged by. The first finds where it
            // keeps things.)
            if (asked.method != "GET" || asked.path != bmc.system().toLatin1() || ++systems != 2) {
                return;
            }
            QTimer::singleShot(0, &bmc, [&] {
                bmc.beServer(bSystem, bRoot); // (B: another computer, another controller, none of A's sessions)
                bmc.systemTakesMs = 0;
                rootsAfter = bmc.rootsUnsigned;
                device.look(); // (under A's session, refused by B: signed in anew, to B)
            });
        };
        QObject::connect(&device, &RedfishDevice::looked, &device, [&] { judgedLate = systems >= 2; });
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(what.value().number, std::optional<QString>(markOf(aSystem))); // (A's late answer, judged A's)
        QVERIFY(judgedLate);                                                    // (the look met B first)
        QCOMPARE(bmc.logins, 2);                                                // (A's sign-in, then B's)
        // The race reached: after B took the address, its open root was read for B's sign-in and once more, for the
        // controller's number under A's session.
        QCOMPARE(bmc.rootsUnsigned - rootsAfter, 2);
        QTest::qWait(200);
        QCOMPARE(device.machine(), markOf(aSystem));
        QCOMPARE(known.size(), 0);
        bmc.whenAsked = nullptr;
    }

    void t21AControllersNumberIsNotTakenWhereItsManagerNamesAnother()
    {
        // (test_which_server_answers.py:607) The open root says one number, the manager under the session another.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.entryUuid = QJsonValue(bRoot);
        bmc.managerNamesItself = false;
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY(what);
        QCOMPARE(what.value().number, std::optional<QString>(markOf(aSystem)));
        QCOMPARE(device.machine(), markOf(aSystem));
    }

    void t22AControllerWhoseManagerNamesItselfByItsServicesNumberIsKnownByIt()
    {
        // (test_which_server_answers.py:663) Its manager says nothing of the service it hosts, and names itself by
        // its service root's number (as a real AMI board's does).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.entryUuid = QJsonValue();
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY(what);
        QCOMPARE(what.value().number, std::optional<QString>(markOf(aSystem)));
        QCOMPARE(device.machine(), markOf(aSystem) + digestOf(aRoot));
    }

    void t23AnAnswerUnderTheSessionBeforeIsStillThisControllers()
    {
        // (test_which_server_answers.py:534, the second review's L1) Known by its controller alone. Its computer's
        // read is answered late, under a session the controller lets go of meanwhile; another asking signs in anew
        // (its controller compared again). The late answer is no less this controller's. (Where its computer was taken
        // kept with it, as a card added under 0.2.0 keeps it.)
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, kept, ServerPlace{bmc.system(), false}), u"secret"_s, quick());
        QVERIFY(identified(device)); // (where it keeps things is known from here on)
        bmc.systemTakesMs = 800;
        bool late = false;
        bmc.whenAsked = [&](const Asked& asked) {
            if (asked.method == "GET" && asked.path == bmc.system().toLatin1()) {
                late = true;
            }
        };
        QList<base::Result<RedfishDevice::Identity>> answers;
        device.identify([&](base::Result<RedfishDevice::Identity> said) { answers.append(std::move(said)); });
        QTRY_VERIFY(late);
        bmc.sessions.clear(); // (the controller lets that session go: the read it took is answered all the same)
        device.identify([&](base::Result<RedfishDevice::Identity> said) { answers.append(std::move(said)); });
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 2, 20000);
        for (const auto& said : std::as_const(answers)) {
            QVERIFY2(said, said ? "" : said.error().message.c_str());
        }
        QCOMPARE(bmc.logins, 2);
    }

    void b1TheRootAndTheSignInGoOnOneLine()
    {
        // Two controllers at one address that show one certificate are not asked one each: the root it is compared
        // by and the password go on the line kept for both.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.stand->keepsLines = true;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QVERIFY(identified(device));
        int root = 0;
        int login = 0;
        for (const Asked& one : std::as_const(bmc.stand->asked)) {
            if (one.method == "GET" && one.path == "/redfish/v1/" && !one.headers.contains("x-auth-token")) {
                root = one.line;
            }
            if (one.method == "POST") {
                login = one.line;
            }
        }
        QVERIFY(root > 0);
        QCOMPARE(login, root);
    }

    void b2ARootThatDoesNotAnswerSendsNothing_data()
    {
        QTest::addColumn<int>("hangsUp");
        QTest::addColumn<int>("takesMs");
        QTest::addColumn<QByteArray>("code");
        // Hung up on more often than Qt asks a GET again; slower than its patience: no root, no password.
        QTest::newRow("hung up on twice") << 2 << 0 << "dropped"_ba;
        QTest::newRow("slower than its patience") << 0 << 2500 << "timeout"_ba;
    }

    void b2ARootThatDoesNotAnswerSendsNothing()
    {
        QFETCH(int, hangsUp);
        QFETCH(int, takesMs);
        QFETCH(QByteArray, code);
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.rootHangsUp = hangsUp;
        bmc.rootUnsignedTakesMs = takesMs;
        RedfishDevice::Pace pace = quick();
        pace.rootUnsignedMs = 1000;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        QCOMPARE(QByteArray::fromStdString(codeOf(identified(device))), code);
        QCOMPARE(bmc.logins, 0);
        for (const Asked& one : std::as_const(bmc.stand->asked)) {
            QVERIFY(one.method != "POST");
        }
    }

    void b3AControllerThatShowsItsNumberOnlyUnderASessionIsKnownByItsComputer()
    {
        // (Unlike the web, which reads the root under the session: a number never shown to anybody could never be
        // compared before the password goes, and the server would be refused for good.)
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.rootUuidSignedOnly = true;
        RedfishDevice first(keptAs(bmc, QString()), u"secret"_s, quick());
        QVERIFY(identified(first));
        QCOMPARE(first.machine(), markOf(aSystem));
        RedfishDevice again(keptAs(bmc, first.machine()), u"secret"_s, quick());
        const auto what = identified(again);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(again.machine(), markOf(aSystem));
    }

    void t24NothingKeptAndItsControllerUnreadableItIsKnownByItsComputer()
    {
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.managerFails = 1;
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        QVERIFY(identified(device));
        QCOMPARE(device.machine(), markOf(aSystem));
    }

    void t25KeptItsComputerAloneAndItsControllerUnreadableNothingIsLearnt()
    {
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.managerFails = 1;
        RedfishDevice device(keptAs(bmc, markOf(aSystem)), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        QVERIFY(identified(device));
        QTest::qWait(100);
        QCOMPARE(device.machine(), markOf(aSystem));
        QCOMPARE(known.size(), 0);
    }

    void t26WhereItKeepsThingsIsFoundAnewAfterAnotherServer()
    {
        // Another server found at its address: where things are is looked up anew, for whichever server answers
        // next (here this one, its board laid out as another make lays it out).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QVERIFY(identified(device));
        bmc.beServer(bSystem, aRoot);
        QCOMPARE(codeOf(identified(device)), std::string("another_machine"));
        bmc.becomeLayout(RedfishByHand::Layout::OpenBmc);
        bmc.beServer(aSystem, aRoot);
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QVERIFY(bmc.requests.contains(u"GET /redfish/v1/Systems/system"_s));
    }

    void t27ASessionUnderWhichItsNumbersWereTakenCountsAsCompared()
    {
        // Taken at first use under a session; its computer says none a moment later, under that same session: known
        // by its controller, compared there.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        QVERIFY(identified(device));
        bmc.uuid = zero;
        QVERIFY(identified(device));
        QCOMPARE(device.machine(), markOf(aSystem) + digestOf(aRoot));
    }

    void t28ASessionUnderWhichItsControllersNumberWasLearntCountsAsCompared()
    {
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem)), u"secret"_s, quick());
        QVERIFY(identified(device));
        QCOMPARE(device.machine(), markOf(aSystem) + digestOf(aRoot));
        bmc.uuid = zero;
        QVERIFY(identified(device));
    }

    void t29TwoAskingsAtOnceTakeItOnce()
    {
        // Nothing kept, asked twice at once: taken by the first, judged by that for the second (and said once).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        QList<base::Result<RedfishDevice::Identity>> answers;
        for (int twice = 0; twice < 2; ++twice) {
            device.identify([&](base::Result<RedfishDevice::Identity> said) { answers.append(std::move(said)); });
        }
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 2, 20000);
        QVERIFY(answers.at(0) && answers.at(1));
        QTest::qWait(100);
        QCOMPARE(known.size(), 1);
        QCOMPARE(device.machine(), markOf(aSystem) + digestOf(aRoot));
    }

    void t30TwoAskingsAtOnceLearnItsControllerOnce()
    {
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem)), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        QList<base::Result<RedfishDevice::Identity>> answers;
        for (int twice = 0; twice < 2; ++twice) {
            device.identify([&](base::Result<RedfishDevice::Identity> said) { answers.append(std::move(said)); });
        }
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 2, 20000);
        QTest::qWait(100);
        QCOMPARE(known.size(), 1);
    }

    void t31WhoeverIsToldItIsKnownMayLetGoOfTheDriverThenAndThere()
    {
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        auto device = std::make_unique<RedfishDevice>(keptAs(bmc, QString()), u"secret"_s, quick());
        QObject::connect(device.get(), &RedfishDevice::knownAs, this, [&device] { device.reset(); });
        device->identify([](const base::Result<RedfishDevice::Identity>&) {});
        QTRY_VERIFY_WITH_TIMEOUT(!device, 20000);
    }

    void t32ItsVersionIsTakenOnlyUnderTheSessionItsServerWasJudgedUnder()
    {
        // A session signed in anew between the judge and the controller's version (for another question: here, a
        // look) may be another controller's: its version is not taken for this one's.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QSignalSpy looked(&device, &RedfishDevice::looked);
        bool once = false;
        bmc.systemTakesMs = 400;
        bmc.whenAsked = [&](const Asked& asked) {
            if (once || asked.path != bmc.system().toLatin1()) {
                return;
            }
            once = true;
            QTimer::singleShot(0, &bmc, [&] {
                bmc.sessions.clear(); // (the session it came under forgotten: the look signs in anew)
                bmc.firmware = u"9.99"_s;
                device.look();
            });
        };
        const auto what = identified(device);
        QVERIFY(what);
        QCOMPARE(what.value().version, QString());
        QCOMPARE(bmc.logins, 2);
        QVERIFY(!looked.isEmpty() || looked.wait(5000));
        bmc.whenAsked = nullptr;
    }

    void t33NothingKeptAndItsOpenRootUnreadableWhichServerCannotBeToldJustThen()
    {
        // As t5, the root anybody may read failing instead of its manager: its computer gives no number, and its
        // controller's cannot be read just then. Nothing is taken, nor is it taken as one that gives none. (Since the
        // outside review of 0.2.0, 5.1, the root read before the password is that very read, and a root that is none
        // then sends nothing: what is read for the controller's number is the root the sign-in read.)
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        bmc.rootFails = 1;
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        const auto first = identified(device);
        QCOMPARE(codeOf(first), std::string("bad_response"));
        QVERIFY(QString::fromStdString(first.error().message).contains(u"which is read before its password goes"_s));
        QCOMPARE(bmc.logins, 0);
        QCOMPARE(device.machine(), QString());
        const auto then = identified(device);
        QVERIFY(then);
        QCOMPARE(device.machine(), QString(qkvm::core::redfish::saidNone) + digestOf(aRoot));
    }

    void t34NothingKeptItsVersionIsTakenOnlyUnderTheSessionItsServerWasJudgedUnder()
    {
        // As t32, with nothing kept: the controller's number is asked under the session after the computer was read
        // (its manager), and a session signed in anew meanwhile (for a look) is the one its version would be read
        // under. Its version is taken only under the session its computer was judged under.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        QSignalSpy looked(&device, &RedfishDevice::looked);
        int systems = 0;
        bmc.systemTakesMs = 400;
        bmc.whenAsked = [&](const Asked& asked) {
            // (The second reading of its computer: the one it is judged by. The first finds where it keeps things.)
            if (asked.path != bmc.system().toLatin1() || ++systems != 2) {
                return;
            }
            QTimer::singleShot(0, &bmc, [&] {
                bmc.sessions.clear(); // (the session it came under forgotten: the look signs in anew)
                bmc.firmware = u"9.99"_s;
                bmc.systemTakesMs = 1200;
                device.look();
            });
        };
        const auto what = identified(device);
        QVERIFY(what);
        QCOMPARE(what.value().version, QString());
        QCOMPARE(device.machine(), markOf(aSystem));
        QCOMPARE(bmc.logins, 2);
        QVERIFY(!looked.isEmpty() || looked.wait(5000));
        bmc.whenAsked = nullptr;
    }

    // --- a controller that lists several computers (outside review of 0.2.0, area 5, HIGH 5.2) ----------------------

    void w1APressGoesToTheCardsComputerWhereverItIsListed()
    {
        // Its controller lists another node first, which gives no number while it is off. The press goes to the
        // computer the card was added for, found by its number among them (before: to whichever was listed first,
        // which passed as the card's for its controller's number).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.power = u"Off"_s;
        bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
        bmc.othersFirst = true;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
        QCOMPARE(bmc.others.at(0).power, u"Off"_s);
    }

    void w2AComputerOfNoNumberAmongSeveralIsNeverTheCards_data()
    {
        QTest::addColumn<QString>("kept");
        QTest::addColumn<bool>("ownGivesNone");
        // Its computer's number kept, and it is not listed: of two that give none, neither is it.
        QTest::newRow("its computer's number kept") << markOf(aSystem) + digestOf(aRoot) << false;
        // Known by its controller alone (its computer gave none when it was added): which of several is its own
        // cannot be told, and none is pressed.
        QTest::newRow("known by its controller alone")
            << QString(qkvm::core::redfish::saidNone) + digestOf(aRoot) << true;
    }

    void w2AComputerOfNoNumberAmongSeveralIsNeverTheCards()
    {
        QFETCH(QString, kept);
        QFETCH(bool, ownGivesNone);
        RedfishByHand bmc;
        bmc.beServer(ownGivesNone ? QJsonValue(zero) : QJsonValue(aSystem), aRoot);
        bmc.power = u"Off"_s;
        bmc.ownListed = ownGivesNone;
        bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}, {u"Node3"_s, QJsonValue(), u"Off"_s}};
        bmc.othersFirst = true;
        RedfishDevice device(keptAs(bmc, kept), u"secret"_s, quick());
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY(!done);
        QCOMPARE(done.error().code, std::string("another_machine"));
        QVERIFY(bmc.actions.isEmpty());
        QCOMPARE(device.machine(), kept);
    }

    void w3ANodeOfNoNumberThatTookTheCardsPlaceAloneIsNotTheCards()
    {
        // Its computer was found by its number at its place; then its node leaves, and another, which gives no number
        // while it is off, is the only one listed, at another place: not taken for the card's (before: its controller's
        // number compared, and listed alone, it was pressed).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice::Pace pace = quick();
        pace.placesFreshMs = 0; // (where it keeps things, found anew at every asking)
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        QVERIFY(identified(device));
        bmc.ownListed = false;
        bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY(!done);
        QCOMPARE(done.error().code, std::string("another_machine"));
        QVERIFY(bmc.actions.isEmpty());
    }

    void w4TheCardsComputerListedAloneAndGivingNoNumberWhileOffIsPressedStill()
    {
        // As before: a controller that learns its computer's number from the server's firmware says none while the
        // server is off; listed alone where it always was, its controller's number compared, it is the card's.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QVERIFY(identified(device));
        bmc.uuid = zero;
        bmc.power = u"Off"_s;
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
    }

    void w5NothingKeptAmongSeveralAFirstOfNoNumberIsNotTaken()
    {
        // Added where its controller lists several computers, the first of which gives no number: which is meant cannot
        // be told by any number, and nothing is taken (before: "known by its controller alone", and any of them would
        // have passed as this card's from then on).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.others = {{u"Node2"_s, QJsonValue(), u"On"_s}};
        bmc.othersFirst = true;
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("bad_response"));
        QVERIFY(QString::fromStdString(what.error().message).contains(u"several computers"_s));
        QCOMPARE(device.machine(), QString());
        QTest::qWait(100);
        QCOMPARE(known.size(), 0);
    }

    // --- where the card's computer is, kept with the card (outside review of 0.2.0's Redfish fixes, M1) -------------

    void p1WhereItsComputerIsFoundIsSaidBesideItsNumbers()
    {
        // Nothing kept: taken at first use, and with it where its computer is, and that its controller lists it alone.
        // Said a turn of the loop later (foundAt), after the numbers it is known by (knownAs), so that the wall keeps
        // both with the card.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        QStringList order;
        QObject::connect(&device, &RedfishDevice::knownAs, &device, [&order] { order.append(u"knownAs"_s); });
        QObject::connect(&device, &RedfishDevice::foundAt, &device, [&order] { order.append(u"foundAt"_s); });
        QSignalSpy found(&device, &RedfishDevice::foundAt);
        QVERIFY(identified(device));
        QVERIFY2(device.serverPlace() == (ServerPlace{bmc.system(), false}), placeSaid(device.serverPlace()));
        QTRY_COMPARE(found.size(), 1);
        QCOMPARE(found.first().at(0).toString(), markOf(aSystem) + digestOf(aRoot));
        QCOMPARE(found.first().at(1).toString(), bmc.system());
        QCOMPARE(found.first().at(2).toBool(), false);
        QCOMPARE(order, (QStringList{u"knownAs"_s, u"foundAt"_s}));
        // Asked again: nothing new, and nothing said.
        QVERIFY(identified(device));
        QTest::qWait(100);
        QCOMPARE(found.size(), 1);
        // Found by its number among several its controller lists: where, and that it lists several.
        bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
        bmc.othersFirst = true;
        RedfishDevice among(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QSignalSpy foundAmong(&among, &RedfishDevice::foundAt);
        QVERIFY(identified(among));
        QVERIFY2(among.serverPlace() == (ServerPlace{bmc.system(), true}), placeSaid(among.serverPlace()));
        QTRY_COMPARE(foundAmong.size(), 1);
        QCOMPARE(foundAmong.first().at(2).toBool(), true);
    }

    void p2AControllerSeenToListSeveralIsKeptSoByAReadItRefuses()
    {
        // Its controller's number compared, and its computer gives no number among several: refused (it cannot be told
        // which is the card's), and that its controller lists several is kept all the same, for every later read.
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
        const QString kept = markOf(aSystem) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, kept), u"secret"_s, quick());
        QSignalSpy found(&device, &RedfishDevice::foundAt);
        const auto what = identified(device);
        QCOMPARE(codeOf(what), std::string("another_machine"));
        QVERIFY2(device.serverPlace() == (ServerPlace{QString(), true}), placeSaid(device.serverPlace()));
        QTRY_COMPARE(found.size(), 1);
        QCOMPARE(found.first().at(0).toString(), kept);
        QCOMPARE(found.first().at(2).toBool(), true);
    }

    void w6ANodeOfNoNumberIsNotTheCardsByADriverMadeAnew_data()
    {
        QTest::addColumn<bool>("amongSeveral"); // the card's computer found by its number among several
        QTest::addColumn<bool>("atItsPlace");   // the other node then listed alone at the card's own place
        QTest::newRow("found alone, another node at another place") << false << false;
        QTest::newRow("found among several, another node at another place") << true << false;
        QTest::newRow("found among several, another node at the card's own place") << true << true;
    }

    void w6ANodeOfNoNumberIsNotTheCardsByADriverMadeAnew()
    {
        // The triage's scenario of 5.2 across a start of the program, or a change of the card: its computer found by
        // its number, the driver that found it gone; then its node is pulled, and another, which gives no number while
        // it is off, is the only one listed. The driver made anew with what the one before found (as the wall keeps it
        // with the card) presses nothing there (before: what was found was the driver's own, gone with it, and any node
        // listed alone passed).
        QFETCH(bool, amongSeveral);
        QFETCH(bool, atItsPlace);
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        if (amongSeveral) {
            bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
        }
        const QString kept = markOf(aSystem) + digestOf(aRoot);
        ServerPlace found;
        {
            RedfishDevice first(keptAs(bmc, kept), u"secret"_s, quick());
            QVERIFY(identified(first));
            found = first.serverPlace();
        }
        QVERIFY2(found == (ServerPlace{bmc.system(), amongSeveral}), placeSaid(found));
        if (atItsPlace) {
            // (Another node in the card's own slot: listed at its place, alone, and giving no number while it is off.)
            bmc.others.clear();
            bmc.uuid = zero;
            bmc.power = u"Off"_s;
        } else {
            bmc.ownListed = false;
            bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
        }
        RedfishDevice device(keptAs(bmc, kept, found), u"secret"_s, quick());
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY(!done);
        QCOMPARE(done.error().code, std::string("another_machine"));
        QCOMPARE(done.error().message, RedfishDevice::notWhereItsComputerIs().message);
        QVERIFY(bmc.actions.isEmpty());
    }

    void w7TheCardsOnlyComputerOffAndGivingNoNumberIsPressedByADriverMadeAnewAndFromBefore()
    {
        // w4 across a start of the program (the owner's boards, one computer each): found by its number where it always
        // is, then off and giving none. The driver made anew with what the one before found presses it. The usual way
        // is no harder. (Another node put in the card's own slot on a controller never seen to list two, off and giving
        // no number, is this very case, and cannot be told from it: SPEC 8.)
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        const QString kept = markOf(aSystem) + digestOf(aRoot);
        ServerPlace found;
        {
            RedfishDevice first(keptAs(bmc, kept), u"secret"_s, quick());
            QVERIFY(identified(first));
            found = first.serverPlace();
        }
        QVERIFY2(found == (ServerPlace{bmc.system(), false}), placeSaid(found));
        bmc.uuid = zero;
        bmc.power = u"Off"_s;
        RedfishDevice device(keptAs(bmc, kept, found), u"secret"_s, quick());
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
        // A card from before 0.2.0 (or from a file of the wall), which keeps no place: off and giving no number, it is
        // not pressed until its computer has given its number at a read, which keeps where it is (outside review of
        // 0.2.0's Redfish fixes, round 2, L1: one listed alone passed for it until then, w10's other node too). Its
        // card says how past that; switched on another way, it is found where it is, and pressed there off and giving
        // none from then on.
        bmc.actions.clear();
        bmc.power = u"Off"_s;
        {
            RedfishDevice before(keptAs(bmc, kept), u"secret"_s, quick());
            QSignalSpy foundBefore(&before, &RedfishDevice::foundAt);
            const auto refused = doneBy(before, u"power_on"_s);
            QVERIFY(!refused);
            QCOMPARE(refused.error().code, std::string("another_machine"));
            QCOMPARE(refused.error().message, RedfishDevice::whereItsComputerIsNotKnownYet().message);
            QVERIFY(bmc.actions.isEmpty());
            QVERIFY2(before.serverPlace() == ServerPlace{}, placeSaid(before.serverPlace()));
            QTest::qWait(100);
            QCOMPARE(foundBefore.size(), 0);
            bmc.uuid = aSystem;
            bmc.power = u"On"_s;
            QVERIFY(identified(before));
            QVERIFY2(before.serverPlace() == (ServerPlace{bmc.system(), false}), placeSaid(before.serverPlace()));
            QTRY_COMPARE(foundBefore.size(), 1);
            found = before.serverPlace();
        }
        bmc.uuid = zero;
        bmc.power = u"Off"_s;
        RedfishDevice after(keptAs(bmc, kept, found), u"secret"_s, quick());
        const auto pressed = doneBy(after, u"power_on"_s);
        QVERIFY2(pressed, pressed ? "" : pressed.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
    }

    void w8ACardKnownByItsControllerAlonePressesOnlyTheComputerWhereItWasTaken()
    {
        // Its computer gave no number when it was added: known by its controller alone, and by where its computer was
        // taken. Another computer listed alone at another place (its node pulled, another in), under the card's own
        // controller, is not the card's (before: any computer it listed alone passed for it).
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        QString kept;
        ServerPlace found;
        {
            RedfishDevice first(keptAs(bmc, QString()), u"secret"_s, quick());
            QVERIFY(identified(first));
            kept = first.machine();
            found = first.serverPlace();
        }
        QCOMPARE(kept, QString(qkvm::core::redfish::saidNone) + digestOf(aRoot));
        QVERIFY2(found == (ServerPlace{bmc.system(), false}), placeSaid(found));
        bmc.ownListed = false;
        bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
        RedfishDevice device(keptAs(bmc, kept, found), u"secret"_s, quick());
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY(!done);
        QCOMPARE(done.error().message, RedfishDevice::notWhereItsComputerIs().message);
        QVERIFY(bmc.actions.isEmpty());
        // Its own back where it was taken: its own again.
        bmc.ownListed = true;
        bmc.others.clear();
        bmc.power = u"Off"_s;
        RedfishDevice back(keptAs(bmc, kept, found), u"secret"_s, quick());
        const auto again = doneBy(back, u"power_on"_s);
        QVERIFY2(again, again ? "" : again.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
        // A card of it from before 0.2.0 (or from a file of the wall), which keeps no place: no number tells which
        // computer it was added for, and none passes for it, its own where it was neither, nor is a place taken
        // (outside review of 0.2.0's Redfish fixes, round 2, L1: the first that passed took the place, another node
        // that stood in for its own too). Its card says to change it and type its password, which takes the server that
        // answers then.
        bmc.actions.clear();
        bmc.power = u"Off"_s;
        RedfishDevice before(keptAs(bmc, kept), u"secret"_s, quick());
        QSignalSpy foundBefore(&before, &RedfishDevice::foundAt);
        const auto refused = doneBy(before, u"power_on"_s);
        QVERIFY(!refused);
        QCOMPARE(refused.error().code, std::string("another_machine"));
        QCOMPARE(refused.error().message, RedfishDevice::whereItWasTakenIsNotKnown().message);
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY2(before.serverPlace() == ServerPlace{}, placeSaid(before.serverPlace()));
        QTest::qWait(100);
        QCOMPARE(foundBefore.size(), 0);
        QCOMPARE(before.machine(), kept);
    }

    void w9AComputerListedBeforeTheCardsThatCannotBeReadIsPassedOver()
    {
        // (Outside review of 0.2.0's Redfish fixes, L5 a.) A node listed before the card's answers a read of it with an
        // error of its own: passed over, and the card's, next in the list, found by its number and pressed (before: the
        // search ended with that node's error, and nothing of the card's own could be looked at or pressed). Where none
        // of them gives the card's number, that error is what is said, and nothing is pressed.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.power = u"Off"_s;
        bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s, 503}};
        bmc.othersFirst = true;
        const QString kept = markOf(aSystem) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, kept), u"secret"_s, quick());
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
        QVERIFY(bmc.requests.contains(u"GET /redfish/v1/Systems/Node2"_s));
        bmc.ownListed = false;
        bmc.others.append({u"Node3"_s, QJsonValue(), u"Off"_s});
        RedfishDevice none(keptAs(bmc, kept), u"secret"_s, quick());
        const auto refused = doneBy(none, u"power_on"_s);
        QVERIFY(!refused);
        QVERIFY2(
            QString::fromStdString(refused.error().message).contains(u"/redfish/v1/Systems/Node2 failed (HTTP 503)"_s),
            refused.error().message.c_str());
        QVERIFY(bmc.requests.contains(u"GET /redfish/v1/Systems/Node3"_s));
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
    }

    void w10ACardThatKeepsNoPlacePressesNoNodeOfNoNumberWhereItsOwnIsPulled()
    {
        // The triage's scenario of 5.2 for a card that keeps no place (from before 0.2.0, from a file of the wall, or
        // one that went back to 0.1.2, which drops it, and came up again: outside review of 0.2.0's Redfish fixes,
        // round 2, L1 a). Its controller lists two computers, and the card's node A is pulled before any read under
        // this version gave its number. Node B, left alone, off and giving no number while it is off, is the only one
        // listed: nothing is pressed there, run after run (before: listed alone, its controller's number compared, it
        // passed in every run, A never there again to give its number), and nothing of B is kept for the card.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.ownListed = false;
        bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
        const QString kept = markOf(aSystem) + digestOf(aRoot);
        for (int run = 0; run < 2; ++run) {
            RedfishDevice device(keptAs(bmc, kept), u"secret"_s, quick());
            QSignalSpy found(&device, &RedfishDevice::foundAt);
            const auto done = doneBy(device, u"power_on"_s);
            QVERIFY(!done);
            QCOMPARE(done.error().code, std::string("another_machine"));
            QCOMPARE(done.error().message, RedfishDevice::whereItsComputerIsNotKnownYet().message);
            QVERIFY(bmc.actions.isEmpty());
            QCOMPARE(bmc.others.at(0).power, u"Off"_s);
            QVERIFY2(device.serverPlace() == ServerPlace{}, placeSaid(device.serverPlace()));
            QTest::qWait(100);
            QCOMPARE(found.size(), 0);
            QCOMPARE(device.machine(), kept);
        }
    }

    // --- the outside review's second pass of 0.2.0 (outside review 2, 3.1 to 3.4) ---------------------------------

    void w11ACommandGoesOnlyToTheComputerItWasDecidedFor()
    {
        // (Outside review 2, 3.1.) Its controller names two computers by their slots: the card's at 1, another at 2. A
        // restart of the card's is decided at 1 and answered 401 (its session ran out); in that moment the chassis
        // numbers its nodes anew, the card's at 2 and the other at 1, and where it keeps things is found anew. Signed
        // in again, the card's computer is found at 2 and judged there: the restart decided for 1 goes nowhere, and
        // says so (before: it went to 1, the other node). Pressed again, it is decided at 2 and done there.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.ownListed = false;
        bmc.others = {{u"1"_s, QJsonValue(aSystem), u"On"_s}, {u"2"_s, QJsonValue(bSystem), u"On"_s}};
        RedfishDevice::Pace pace = quick();
        pace.placesFreshMs = 0; // (where it keeps things lapses in that moment: found anew at every asking)
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        bool renumbered = false;
        bmc.whenAsked = [&bmc, &renumbered](const Asked& asked) {
            if (!renumbered && asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                renumbered = true;
                bmc.sessions.clear();
                std::swap(bmc.others[0].uuid, bmc.others[1].uuid);
            }
        };
        const auto done = doneBy(device, u"atx_reset"_s);
        QVERIFY(renumbered);
        QVERIFY(!done);
        QCOMPARE(done.error().code, std::string("action_failed"));
        QVERIFY2(QString::fromStdString(done.error().message).startsWith(u"Nothing was sent"_s),
                 done.error().message.c_str());
        QVERIFY2(QString::fromStdString(done.error().message).contains(u"another place"_s),
                 done.error().message.c_str());
        QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
        const auto again = doneBy(device, u"atx_reset"_s);
        QVERIFY2(again, again ? "" : again.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system 2 ForceRestart"_s});
        bmc.whenAsked = nullptr;
    }

    void w12TwoComputersThatGiveTheCardsNumberAreNeitherTheCards()
    {
        // (Outside review 2, 3.2.) Two nodes of one controller give the same number, as boards whose maker gave them
        // one for all may (a number of that kind that qKVM does not know of: AMI's own is no number, w15). Which is the
        // card's cannot be told: nothing is switched on, whichever of them is listed first, the card says so, and where
        // its computer is stays as it was (before: the first that gave it was taken, and the card's place moved to it).
        const QString placeholder = aSystem;
        for (const bool ownFirst : {true, false}) {
            RedfishByHand bmc;
            bmc.beServer(placeholder, aRoot);
            bmc.power = u"Off"_s;
            bmc.others = {{u"Node2"_s, QJsonValue(placeholder), u"Off"_s}};
            bmc.othersFirst = !ownFirst;
            const QString kept = markOf(placeholder) + digestOf(aRoot);
            const ServerPlace foundAt{bmc.system(), true};
            RedfishDevice device(keptAs(bmc, kept, foundAt), u"secret"_s, quick());
            QSignalSpy found(&device, &RedfishDevice::foundAt);
            const auto done = doneBy(device, u"power_on"_s);
            QVERIFY(!done);
            QCOMPARE(done.error().code, std::string("another_machine"));
            QCOMPARE(done.error().message, RedfishDevice::sameNumberTwice().message);
            QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
            QCOMPARE(bmc.power, u"Off"_s);
            QCOMPARE(bmc.others.at(0).power, u"Off"_s);
            const qkvm::core::Status status = lookedAt(device);
            QCOMPARE(status.errorMessage.toStdString(), RedfishDevice::sameNumberTwice().message);
            QVERIFY2(device.serverPlace() == foundAt, placeSaid(device.serverPlace()));
            QTest::qWait(100);
            QCOMPARE(found.size(), 0);
            QCOMPARE(device.machine(), kept);
        }
        // Found by its number before, among others that give numbers of their own; within the ten minutes where it
        // keeps things counts as known, another of them gives the card's number too. The press reads the list anew,
        // and nothing is done (before: the press went on the place a look found).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.power = u"Off"_s;
        bmc.others = {{u"Node2"_s, QJsonValue(bSystem), u"Off"_s}};
        const QString kept = markOf(aSystem) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, kept), u"secret"_s, quick());
        QVERIFY(identified(device));
        bmc.others[0].uuid = aSystem;
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY(!done);
        QCOMPARE(done.error().message, RedfishDevice::sameNumberTwice().message);
        QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
    }

    void w13APressThatRestsOnItsComputerListedAloneReadsTheListAnew()
    {
        // (Outside review 2, 3.3.) Its computer found by its number, alone, at its place. Within the ten minutes where
        // it keeps things counts as known, another node comes in, and the one at the card's place is off and gives no
        // number. The press reads its controller's list anew: two are listed, and nothing is switched on (before:
        // "listed alone" was taken from the list of the look before, and that computer was switched on).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        const QString kept = markOf(aSystem) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, kept, ServerPlace{bmc.system(), false}), u"secret"_s, quick());
        QVERIFY(identified(device));
        bmc.uuid = zero;
        bmc.power = u"Off"_s;
        bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY(!done);
        QCOMPARE(done.error().code, std::string("another_machine"));
        QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
        QCOMPARE(bmc.power, u"Off"_s);
        // (And that its controller lists several is kept from that read, as from any: p2.)
        QVERIFY2(device.serverPlace() == (ServerPlace{bmc.system(), true}), placeSaid(device.serverPlace()));
    }

    void w14ACardKnownByItsControllerAloneTakesNoComputerThatGivesANumber_data()
    {
        QTest::addColumn<QString>("power");
        QTest::newRow("on") << u"On"_s;
        QTest::newRow("off") << u"Off"_s;
    }

    void w14ACardKnownByItsControllerAloneTakesNoComputerThatGivesANumber()
    {
        // (Outside review 2, 3.4.) Its computer gave no number when it was added: known by its controller alone, and by
        // where its computer was taken. The computer there gives a number of its own now, on or off: another node put
        // in the card's slot, or its own started since, which nothing tells apart. Nothing is done to it, and the card
        // asks whether it is its server (before: whatever its computer said, it passed for the card's; w16 for the
        // Yes). Changed with its password typed, the card takes the server that answers, by its number.
        QFETCH(QString, power);
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        const ServerPlace takenAt{bmc.system(), false};
        bmc.uuid = bSystem;
        bmc.power = power;
        RedfishDevice device(keptAs(bmc, kept, takenAt), u"secret"_s, quick());
        const auto done = doneBy(device, power == u"On"_s ? u"power_off"_s : u"power_on"_s);
        QVERIFY(!done);
        QCOMPARE(done.error().code, std::string("another_machine"));
        QCOMPARE(done.error().message, RedfishDevice::asksFirst().message);
        QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
        QCOMPARE(bmc.power, power);
        const qkvm::core::Status asking = lookedAt(device);
        QVERIFY(asking.asksItsServer);
        QVERIFY2(asking.errorMessage.startsWith(QString::fromStdString(RedfishDevice::asksWhetherItsServer().message)),
                 qPrintable(asking.errorMessage));
        QCOMPARE(device.machine(), kept);
        RedfishDevice typed(keptAs(bmc, QString()), u"secret"_s, quick());
        QVERIFY(identified(typed));
        QCOMPARE(typed.machine(), markOf(bSystem) + digestOf(aRoot));
        QVERIFY2(typed.serverPlace() == takenAt, placeSaid(typed.serverPlace()));
    }

    void w15ALoneComputerThatGivesASharedNumberIsNotTheCards_data()
    {
        QTest::addColumn<QString>("number");    // what both nodes give
        QTest::addColumn<bool>("cardsAnswers"); // the card's node is listed, and answers an error of its own
        const QString placeholder = u"03000200-0400-0500-0006-000700080009"_s;
        QTest::newRow("AMI's placeholder, the card's node pulled") << placeholder << false;
        QTest::newRow("AMI's placeholder, the card's node unwell") << placeholder << true;
        QTest::newRow("AMI's placeholder in SMBIOS byte order, the card's node pulled")
            << u"00020003-0004-0005-0006-000700080009"_s << false;
        QTest::newRow("another shared number, the card's node unwell") << aSystem << true;
    }

    void w15ALoneComputerThatGivesASharedNumberIsNotTheCards()
    {
        // (Outside review 2, 3.2, its second half.) Two nodes of one chassis give one number, and the card was added
        // for one of them. That node is pulled for service, or answers an error of its own just then: the other is the
        // only one that gives the number. AMI's placeholder is no number at all, so the other is held to where the
        // card's computer was found, and is not the card's; and where the one that gives the number is at another place
        // than the card's computer was found at, while a node could not be read, what that node answered is said, and
        // nothing is done (before: the other was the card's, its place taken, and it was switched on).
        QFETCH(QString, number);
        QFETCH(bool, cardsAnswers);
        RedfishByHand bmc;
        bmc.beServer(number, aRoot);
        bmc.others = {{u"Node2"_s, QJsonValue(number), u"Off"_s}};
        const QString kept = markOf(number) + digestOf(aRoot);
        const ServerPlace foundAt{bmc.system(), true};
        if (cardsAnswers) {
            bmc.systemUnwell = true;
        } else {
            bmc.ownListed = false;
        }
        RedfishDevice device(keptAs(bmc, kept, foundAt), u"secret"_s, quick());
        QSignalSpy found(&device, &RedfishDevice::foundAt);
        const auto done = doneBy(device, u"power_on"_s);
        QVERIFY(!done);
        QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
        QCOMPARE(bmc.others.at(0).power, u"Off"_s);
        QVERIFY2(device.serverPlace().place == bmc.system(), placeSaid(device.serverPlace()));
        QCOMPARE(device.machine(), kept);
    }

    void w16ACardKnownByItsControllerAloneAsksOnceWhetherTheComputerThatGivesANumberIsItsServer()
    {
        // (The owner's decision of 2026-11-22.) Added while its server was off and gave no number: known by its
        // controller alone, where its computer was taken. The server is switched on, and gives its number. The card
        // asks whether that is the server it was added for, says what that computer says of itself, and offers its two
        // answers alone (review cx2-2, M1): looks go on, nothing else (no power command, no console). Yes, of the
        // number it asked about (L1), takes that number for the card, with no password typed, and its buttons work
        // again. A computer that gives another number then is another server. (Before: refused for good, until changed
        // with its password typed.)
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        bmc.power = u"Off"_s;
        bmc.hostname = u"db-01.lan"_s;
        QString kept;
        ServerPlace takenAt;
        {
            RedfishDevice added(keptAs(bmc, QString()), u"secret"_s, quick());
            QVERIFY(identified(added));
            kept = added.machine();
            takenAt = added.serverPlace();
        }
        QCOMPARE(kept, QString(qkvm::core::redfish::saidNone) + digestOf(aRoot));
        bmc.uuid = aSystem;
        bmc.power = u"On"_s;
        RedfishDevice device(keptAs(bmc, kept, takenAt), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        const qkvm::core::Status asking = lookedAt(device);
        QVERIFY(asking.asksItsServer);
        QCOMPARE(asking.errorCode, u"another_machine"_s);
        QVERIFY2(asking.errorMessage.startsWith(QString::fromStdString(RedfishDevice::asksWhetherItsServer().message)),
                 qPrintable(asking.errorMessage));
        QVERIFY2(asking.errorMessage.contains(u"SRV-2200"_s) && asking.errorMessage.contains(u"db-01.lan"_s),
                 qPrintable(asking.errorMessage));
        QCOMPARE(asking.capabilities, std::optional<QStringList>(QStringList{u"its_server"_s, u"not_its_server"_s}));
        QVERIFY(!asking.hasScreen);
        QCOMPARE(asking.askedAbout, markOf(aSystem));
        // Nothing goes to it but looks, until it is answered: the question put away leaves it so.
        const auto refused = doneBy(device, u"power_off"_s);
        QVERIFY(!refused);
        QCOMPARE(refused.error().message, RedfishDevice::asksFirst().message);
        QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
        QVERIFY(lookedAt(device).asksItsServer);
        QCOMPARE(device.machine(), kept);
        // A Yes of another number than the one asked about (a box older than the question) takes nothing.
        QVERIFY(!answered(device, u"its_server"_s, markOf(bSystem)));
        QCOMPARE(device.machine(), kept);
        // Yes: known by its number from now on, where it was taken, and pressed.
        const auto yes = answered(device, u"its_server"_s, asking.askedAbout);
        QVERIFY2(yes, yes ? "" : yes.error().message.c_str());
        const QString both = markOf(aSystem) + digestOf(aRoot);
        QCOMPARE(device.machine(), both);
        QTRY_COMPARE(known.size(), 1);
        QCOMPARE(known.first().at(0).toString(), kept);
        QCOMPARE(known.first().at(1).toString(), both);
        QVERIFY2(device.serverPlace() == takenAt, placeSaid(device.serverPlace()));
        const qkvm::core::Status after = lookedAt(device);
        QVERIFY(!after.asksItsServer);
        QCOMPARE(after.state, u"online"_s);
        const auto pressed = doneBy(device, u"power_off"_s);
        QVERIFY2(pressed, pressed ? "" : pressed.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system GracefulShutdown"_s});
        // Another number at its place from then on: another server, refused as any is.
        bmc.uuid = bSystem;
        bmc.power = u"Off"_s;
        const auto other = doneBy(device, u"power_on"_s);
        QVERIFY(!other);
        QCOMPARE(other.error().message, RedfishDevice::anotherServer().message);
        QCOMPARE(bmc.actions, QStringList{u"system GracefulShutdown"_s});
        // A card that asks nothing takes nothing by a Yes.
        QVERIFY(!answered(device, u"its_server"_s, markOf(bSystem)));
        QCOMPARE(device.machine(), both);
    }

    void w17AYesTakesOnlyTheNumberThatWasAskedAbout()
    {
        // (The owner's decision of 2026-11-22.) The card asked about the computer that gave one number; before the Yes
        // came, another computer stands at its place, giving another. The Yes takes neither, and the card asks again
        // about the one that is there now; and a Yes of the number it asked about before takes nothing then either,
        // though a look asked about the new one meanwhile (review cx2-2, L1).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, kept, ServerPlace{bmc.system(), false}), u"secret"_s, quick());
        const qkvm::core::Status asking = lookedAt(device);
        QVERIFY(asking.asksItsServer);
        bmc.uuid = bSystem;
        QVERIFY(!answered(device, u"its_server"_s, asking.askedAbout));
        QCOMPARE(device.machine(), kept);
        QCOMPARE(lookedAt(device).askedAbout, markOf(bSystem));
        QVERIFY(!answered(device, u"its_server"_s, asking.askedAbout));
        QCOMPARE(device.machine(), kept);
        QVERIFY(bmc.actions.isEmpty());
    }

    void w18ANoKeepsTheCardRefusingThatComputerOnOrOff()
    {
        // (The owner's decision of 2026-11-22, "No keeps refusing"; review cx2-2, M2.) The card asks about the computer
        // that gives a number at its place, and is told No. From then on it refuses that place: the computer giving its
        // number, and once it gives none (switched off), in this run and the next (as the wall keeps what its driver
        // found: among several, which no computer that gives no number passes). Changed with its password typed while
        // the server gives its number, the card takes it by that number.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        const ServerPlace takenAt{bmc.system(), false};
        ServerPlace afterNo;
        {
            RedfishDevice device(keptAs(bmc, kept, takenAt), u"secret"_s, quick());
            QSignalSpy found(&device, &RedfishDevice::foundAt);
            const qkvm::core::Status asking = lookedAt(device);
            QVERIFY(asking.asksItsServer);
            const auto no = answered(device, u"not_its_server"_s, asking.askedAbout);
            QVERIFY2(no, no ? "" : no.error().message.c_str());
            QTRY_COMPARE(found.size(), 1);
            QCOMPARE(found.first().at(0).toString(), kept);
            QCOMPARE(found.first().at(2).toBool(), true);
            afterNo = device.serverPlace();
            QVERIFY2(afterNo.amongSeveral, placeSaid(afterNo));
            const qkvm::core::Status refusing = lookedAt(device);
            QVERIFY(!refusing.asksItsServer);
            QCOMPARE(refusing.errorMessage.toStdString(), RedfishDevice::elsewhereWithANumber().message);
            QVERIFY(!doneBy(device, u"power_off"_s));
            bmc.uuid = zero;
            bmc.power = u"Off"_s;
            const auto off = doneBy(device, u"power_on"_s);
            QVERIFY(!off);
            QCOMPARE(off.error().message, RedfishDevice::notWhereItsComputerIs().message);
            QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
        }
        RedfishDevice next(keptAs(bmc, kept, afterNo), u"secret"_s, quick());
        QVERIFY(!doneBy(next, u"power_on"_s));
        QVERIFY(bmc.actions.isEmpty());
        bmc.uuid = aSystem;
        bmc.power = u"On"_s;
        RedfishDevice typed(keptAs(bmc, QString()), u"secret"_s, quick());
        QVERIFY(identified(typed));
        QCOMPARE(typed.machine(), markOf(aSystem) + digestOf(aRoot));
        QVERIFY2(typed.serverPlace() == takenAt, placeSaid(typed.serverPlace()));
    }

    void w19ACardThatKeptAMakersPlaceholderAsItsNumberIsKnownByItsControllerAlone()
    {
        // (Review cx2-2, M3.) A card of qKVM 0.1 kept AMI's placeholder as its computer's number, beside its
        // controller's, and no place. That placeholder is no number now: the card is read as one known by its
        // controller alone. It keeps no place, so its words are those of such a card, whose one way past (its password
        // typed) works, and is then pressed; one that keeps a place goes as one known by its controller alone there.
        const QString placeholder = u"03000200-0400-0500-0006-000700080009"_s;
        RedfishByHand bmc;
        bmc.beServer(placeholder, aRoot);
        bmc.power = u"Off"_s;
        const QString kept = markOf(placeholder) + digestOf(aRoot);
        RedfishDevice old(keptAs(bmc, kept), u"secret"_s, quick());
        const auto refused = doneBy(old, u"power_on"_s);
        QVERIFY(!refused);
        QCOMPARE(refused.error().message, RedfishDevice::whereItWasTakenIsNotKnown().message);
        QVERIFY(bmc.actions.isEmpty());
        RedfishDevice typed(keptAs(bmc, QString()), u"secret"_s, quick());
        QVERIFY(identified(typed));
        QCOMPARE(typed.machine(), QString(qkvm::core::redfish::saidNone) + digestOf(aRoot));
        RedfishDevice placed(keptAs(bmc, kept, ServerPlace{bmc.system(), false}), u"secret"_s, quick());
        const auto pressed = doneBy(placed, u"power_on"_s);
        QVERIFY2(pressed, pressed ? "" : pressed.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
    }

    void w20ANoSaidOnceItsCardStoppedAskingIsTakenAllTheSame()
    {
        // (Review cx2-3, M-a.) The card asks about the computer that gives a number at its place, and its box stands
        // open. Meanwhile that computer goes off and gives none: a look passes it as the card's own, and the card asks
        // no more. The No said then, of the number the box was shown, is taken all the same: that place is refused from
        // then on (among several), and Power on is refused, with nothing pressed (before: "asks nothing just now", and
        // the card took that computer, off, as its own). A No of a number this card never asked about changes nothing,
        // and says so.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, kept, ServerPlace{bmc.system(), false}), u"secret"_s, quick());
        QSignalSpy found(&device, &RedfishDevice::foundAt);
        const qkvm::core::Status asking = lookedAt(device);
        QVERIFY(asking.asksItsServer);
        QCOMPARE(asking.askedAbout, markOf(aSystem));
        bmc.uuid = zero;
        bmc.power = u"Off"_s;
        const qkvm::core::Status off = lookedAt(device);
        QVERIFY(!off.asksItsServer);
        QVERIFY2(off.errorCode.isEmpty(), qPrintable(off.errorMessage));
        const auto stray = answered(device, u"not_its_server"_s, markOf(bSystem));
        QVERIFY(!stray);
        QVERIFY2(!device.serverPlace().amongSeveral, placeSaid(device.serverPlace()));
        const auto no = answered(device, u"not_its_server"_s, asking.askedAbout);
        QVERIFY2(no, no ? "" : no.error().message.c_str());
        QTRY_COMPARE(found.size(), 1);
        QCOMPARE(found.first().at(0).toString(), kept);
        QCOMPARE(found.first().at(2).toBool(), true);
        QVERIFY2(device.serverPlace().amongSeveral, placeSaid(device.serverPlace()));
        const auto on = doneBy(device, u"power_on"_s);
        QVERIFY(!on);
        QCOMPARE(on.error().message, RedfishDevice::notWhereItsComputerIs().message);
        QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
    }

    void w21WhileItsCardAsksAFailedLookShowsNothingOfTheServerBefore()
    {
        // (Review cx2-3, L-c.) A look of the card's own server, giving no number, shows it as it is; then the computer
        // there gives one, and the card asks. A look that fails then, within the moment a failed look leaves the last
        // good one standing, does not show that good one (online, with its power buttons and without the question):
        // it says what failed.
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        RedfishDevice::Pace pace = quick();
        pace.hiccupMs = 60000;
        RedfishDevice device(keptAs(bmc, kept, ServerPlace{bmc.system(), false}), u"secret"_s, pace);
        const qkvm::core::Status before = lookedAt(device);
        QCOMPARE(before.state, u"online"_s);
        QVERIFY(!before.asksItsServer);
        bmc.uuid = aSystem;
        QVERIFY(lookedAt(device).asksItsServer);
        bmc.systemUnwell = true;
        const qkvm::core::Status failed = lookedAt(device);
        QVERIFY2(failed.state != u"online"_s, qPrintable(failed.state));
        QVERIFY(!failed.capabilities.value_or(QStringList()).contains(u"power_on"_s));
        QVERIFY(!failed.server.has_value());
    }

    void w22ANoteOfAStartIntoBiosSetupStandsWhileItsCardAsks()
    {
        // (Review cx2-3, L-f; cx2-2, L2.) A start into BIOS setup left a note on the card's server: its override may be
        // set. The card asks about the computer at its place, which reads its override Disabled: the note stands, since
        // nothing of that computer is taken for the card's server's until the card is answered. The same read of the
        // card's own server (giving no number, as when it was added) lets the note go.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.bootTargets = QJsonArray{u"None"_s, u"Pxe"_s, u"BiosSetup"_s};
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        RedfishDevice::Pace pace = quick();
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>();
        const RedfishDevice::Where where = keptAs(bmc, kept, ServerPlace{bmc.system(), false});
        static_cast<void>(pace.overrides->mayBeSet(where.address, kept, QDateTime::currentDateTimeUtc().addSecs(-60)));
        QVERIFY(pace.overrides->standing(where.address, kept).has_value());
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        {
            RedfishDevice device(where, u"secret"_s, pace);
            QVERIFY(lookedAt(device).asksItsServer);
            QVERIFY(lookedAt(device).asksItsServer);
            QVERIFY(pace.overrides->standing(where.address, kept).has_value());
        }
        bmc.uuid = zero;
        RedfishDevice own(where, u"secret"_s, pace);
        QVERIFY(!lookedAt(own).asksItsServer);
        QVERIFY(!pace.overrides->standing(where.address, kept).has_value());
    }

private:
    // (The answers held apart from this call: a press it gave up on is told what became of it after this has returned,
    // by its driver's end at the latest; as test_redfish_setup's.)
    static base::Result<qkvm::core::Done> doneBy(RedfishDevice& device, const QString& kind)
    {
        return answered(device, kind, QString());
    }
    // ...and the answer of a card's question, of the number it was asked about (Action::text).
    static base::Result<qkvm::core::Done> answered(RedfishDevice& device, const QString& kind, const QString& number)
    {
        const auto answers = std::make_shared<QList<base::Result<qkvm::core::Done>>>();
        qkvm::core::Action what = qkvm::core::Action::plain(kind);
        what.text = number;
        device.act(what, [answers](base::Result<qkvm::core::Done> answer) { answers->append(std::move(answer)); });
        if (!QTest::qWaitFor([&answers] { return !answers->isEmpty(); }, 30000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return answers->takeFirst();
    }
};

QTEST_GUILESS_MAIN(TestRedfishWhichServer)
#include "test_redfish_which_server.moc"
