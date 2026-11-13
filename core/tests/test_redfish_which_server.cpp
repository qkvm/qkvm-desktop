#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QCryptographicHash>
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
#include <qkvm/core/sign_in_pacing.h>

#include "redfish_by_hand.h"
#include "redfish_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::RedfishDevice;

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
        // (test_which_server_answers.py:516) Compared before this very session: not refused, and nothing changed.
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        const QString both = markOf(aSystem) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, both), u"secret"_s, quick());
        const auto what = identified(device);
        QVERIFY2(what, what ? "" : what.error().message.c_str());
        QCOMPARE(device.machine(), both);
        // (It gives none now: what it is known by stays as it is.)
        QCOMPARE(what.value().number, std::nullopt);
    }

    void t14KeptNoneSaidAndItsControllerKnownByItsControllerAlone()
    {
        // (test_which_server_answers.py:414) Its computer gave no number when it was added.
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        RedfishDevice device(keptAs(bmc, kept), u"secret"_s, quick());
        QVERIFY(identified(device));
        // Whatever its computer says now, it is known by its controller.
        bmc.uuid = aSystem;
        RedfishDevice again(keptAs(bmc, kept), u"secret"_s, quick());
        QVERIFY(identified(again));
        QCOMPARE(again.machine(), kept);
        // Another server: refused before the password goes.
        bmc.beServer(bSystem, bRoot);
        const int logins = bmc.logins;
        RedfishDevice other(keptAs(bmc, kept), u"secret"_s, quick());
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
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        int signedRoots = 0;
        bmc.whenAsked = [&](const Asked& asked) {
            // (The root its controller's number is read from, with no session: after the computer was judged.)
            if (asked.method == "GET" && asked.path == "/redfish/v1/" && !asked.headers.contains("x-auth-token") &&
                ++signedRoots == 1) {
                bmc.beServer(bSystem, bRoot); // (B: another computer, another controller, none of A's sessions)
            }
        };
        RedfishDevice device(keptAs(bmc, markOf(aSystem)), u"secret"_s, quick());
        QSignalSpy known(&device, &RedfishDevice::knownAs);
        static_cast<void>(identified(device));
        QCOMPARE(device.machine(), markOf(aSystem));
        QCOMPARE(known.size(), 0);
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
        // (its controller compared again). The late answer is no less this controller's.
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        RedfishDevice device(keptAs(bmc, kept), u"secret"_s, quick());
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
        // controller's cannot be read just then. Nothing is taken, nor is it taken as one that gives none.
        RedfishByHand bmc;
        bmc.beServer(zero, aRoot);
        bmc.rootFails = 2; // (the first answers all the same, before the sign-in: it is there)
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        const auto first = identified(device);
        QCOMPARE(codeOf(first), std::string("bad_response"));
        QVERIFY(QString::fromStdString(first.error().message).contains(u"could not be told"_s));
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
};

QTEST_GUILESS_MAIN(TestRedfishWhichServer)
#include "test_redfish_which_server.moc"
