#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/proxmox_device.h>
#include <qkvm/core/sign_in_pacing.h>

#include "proxmox_by_hand.h"
#include "proxmox_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::Action;
using qkvm::core::Done;
using qkvm::core::ProxmoxDevice;
using qkvm::core::Status;

namespace {

base::Result<qkvm::core::proxmox::Listing> listed(ProxmoxDevice& device)
{
    return answerOf<qkvm::core::proxmox::Listing>(
        [&device](std::function<void(base::Result<qkvm::core::proxmox::Listing>)> done) {
            device.listing(std::move(done));
        });
}

} // namespace

// A virtual machine on Proxmox VE spoken to by the program itself through Proxmox's API (proxmox_device.h), against a
// stand-in that answers as the web's does (proxmox_by_hand.h): its token, how it is, what it is, and the guests a
// token may see. Ported from the web's own tests where they are named (../app/tests/test_proxmox.py of web 0.3.1).
class TestProxmoxDevice : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The driver chooses Windows' own TLS, which is refused once TLS is in use: so one is made before anything
        // else touches TLS (the stand-in's server among them).
        const ProxmoxDevice first(ProxmoxDevice::Where{}, QString(), quickProxmox());
    }

    // The web's test_a_guest_is_added_and_shows_its_screen, its card: as a server is said, with its power, its
    // processors and memory, and Proxmox's version; the token in a header of every question, never in an address.
    void aGuestIsLookedAtAsTheWebLooksAtIt()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(guestOf(pve), pve.secret, quickProxmox());
        QSignalSpy shown(&device, &ProxmoxDevice::showedItsCertificate);
        const Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.hasPicture);
        QCOMPARE(status.power, std::optional<bool>(true));
        QCOMPARE(status.model, u"VM 100 on pve1"_s);
        QCOMPARE(status.version, u"Proxmox VE 9.2.21"_s);
        QVERIFY(status.server.has_value());
        const qkvm::core::ServerReadings readings = status.server.value_or(qkvm::core::ServerReadings{});
        QCOMPARE(readings.processors, 2);
        QCOMPARE(readings.memoryGib, std::optional<double>(2.0));
        // (And its virtual media, the media step's tests; and while it runs, its screen through its console: the
        // console step's tests.)
        QCOMPARE(status.capabilities,
                 std::optional<QStringList>(QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"media"_s,
                                                        u"screen"_s, u"keys"_s, u"text"_s, u"live"_s, u"control"_s}));
        // (Its settings after its state: which virtual machine has its number, rule 30, at its first look.)
        QCOMPARE(pve.requests, (QStringList{u"GET /version"_s, u"GET /nodes/pve1/qemu/100/status/current"_s,
                                            u"GET /nodes/pve1/qemu/100/config"_s}));
        QCOMPARE(pve.refused, 0);
        QVERIFY(pve.inTheClear.isEmpty());
        QCOMPARE(shown.size(), 1);
        // The next look carries the token again, and signs in no more: Proxmox has no session for a token.
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
        QCOMPARE(pve.asked(u"GET /nodes/pve1/qemu/100/status/current"_s), 2);
    }

    // The web's test_a_stopped_guest_is_said_off_and_no_console_is_asked_for, its card.
    void aStoppedGuestIsSaidOff()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests.insert(101, ProxmoxByHand::Guest{u"db"_s, u"pve1"_s, u"stopped"_s});
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, quickProxmox());
        const Status status = lookedAt(device);
        QCOMPARE(status.state, u"no_signal"_s);
        QCOMPARE(status.power, std::optional<bool>(false));
        QCOMPARE(status.model, u"VM 101 on pve1"_s);
    }

    // What it is, for a person who adds it (the web's `about`).
    void whatAGuestIsIsSaid()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(guestOf(pve), pve.secret, quickProxmox());
        const base::Result<ProxmoxDevice::Identity> found = identified(device);
        QVERIFY2(found, found ? "" : found.error().message.c_str());
        QCOMPARE(found.value().model, u"VM 100 on pve1"_s);
        QCOMPARE(found.value().version, u"Proxmox VE 9.2.21"_s);
        QCOMPARE(found.value().hostname, u"web"_s);
    }

    // The web's test_a_wrong_token_is_a_no_and_is_not_offered_again_and_again: two guests of one Proxmox with a wrong
    // token are one no, and the token goes out no more for either of them, whatever they ask.
    void aWrongTokenIsANoForEveryGuestOfItsProxmox()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests.insert(101, ProxmoxByHand::Guest{u"db"_s});
        ProxmoxDevice::Pace pace = quickProxmox();
        const QString wrong = u"00000000-0000-0000-0000-000000000000"_s;
        ProxmoxDevice first(guestOf(pve, 100), wrong, pace);
        ProxmoxDevice second(guestOf(pve, 101), wrong, pace);
        QCOMPARE(lookedAt(first).state, u"auth"_s);
        QCOMPARE(first.status().errorMessage, QString::fromStdString(ProxmoxDevice::tokenRefused().message));
        const Status other = lookedAt(second);
        QCOMPARE(other.state, u"auth"_s);
        QCOMPARE(lookedAt(first).state, u"auth"_s);
        QCOMPARE(pve.refused, 1);
        QCOMPARE(pve.refusedPaths, QStringList{u"GET /version"_s});
        // A person types it again: tried at once, whatever was said to it before.
        ProxmoxDevice::tryAfresh(guestOf(pve, 100), wrong, pace);
        QCOMPARE(lookedAt(first).state, u"auth"_s);
        QCOMPARE(pve.refused, 2);
        // The right one lets it in.
        ProxmoxDevice right(guestOf(pve, 100), pve.secret, pace);
        QCOMPARE(lookedAt(right).state, u"online"_s);
    }

    // The web's test_what_is_no_token_is_sent_nowhere: what is no token's shape goes nowhere at all, not a line.
    void whatIsNoTokenIsSentNowhere()
    {
        ProxmoxByHand pve(proxmoxTls());
        for (const auto& [user, secret] :
             std::vector<std::pair<QString, QString>>{{u"root@pam"_s, pve.secret},
                                                      {pve.tokenId, u"a password"_s},
                                                      {pve.tokenId + u"\r\nX: y"_s, pve.secret},
                                                      {pve.tokenId, pve.secret + u"\r\n"_s}}) {
            ProxmoxDevice::Where where = guestOf(pve);
            where.user = user;
            ProxmoxDevice device(where, secret, quickProxmox());
            const Status status = lookedAt(device);
            QCOMPARE(status.errorCode, u"invalid"_s);
            QCOMPARE(status.state, u"error"_s);
        }
        ProxmoxDevice none(guestOf(pve), QString(), quickProxmox());
        QCOMPARE(lookedAt(none).state, u"auth"_s);
        QCOMPARE(none.status().errorCode, u"no_password"_s);
        QVERIFY(pve.requests.isEmpty());
        QCOMPARE(pve.refused, 0);
        QCOMPARE(pve.stand->lines, 0);
    }

    // The web's test_many_guests_with_the_right_token_looking_at_once_are_all_online_at_their_first_look and
    // test_many_guests_on_one_token_at_a_start_go_on_together_once_the_token_is_taken: twelve guests of one Proxmox at
    // a wall's start, one sign-in among them, every one online at its first look.
    void manyGuestsOfOneTokenSignInOnce()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.versionTakesMs = 300;
        for (qint64 vmid = 101; vmid < 112; ++vmid) {
            pve.guests.insert(vmid, ProxmoxByHand::Guest{u"vm%1"_s.arg(vmid)});
        }
        ProxmoxDevice::Pace pace = quickProxmox();
        std::vector<std::unique_ptr<ProxmoxDevice>> devices;
        std::vector<std::unique_ptr<QSignalSpy>> over;
        for (qint64 vmid = 100; vmid < 112; ++vmid) {
            devices.push_back(std::make_unique<ProxmoxDevice>(guestOf(pve, vmid), pve.secret, pace));
            over.push_back(std::make_unique<QSignalSpy>(devices.back().get(), &ProxmoxDevice::looked));
        }
        for (const auto& device : devices) {
            device->look();
        }
        QVERIFY(QTest::qWaitFor(
            [&over] { return std::ranges::all_of(over, [](const auto& spy) { return !spy->isEmpty(); }); }, 20000));
        for (const auto& device : devices) {
            QCOMPARE(device->status().state, u"online"_s);
            QCOMPARE(device->status().version, u"Proxmox VE 9.2.21"_s);
        }
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
        // (Each asked on its own line first what answers there: outside review of 0.2.0, 3.2.)
        QCOMPARE(pve.unsignedAsked.size(), 12);
    }

    // Review cx3-2, L3: the unsigned question, which a real Proxmox answers three seconds late (401 "No ticket", held
    // back as its every 401 is), is waited for as the sign-in is, and not for an ordinary question's quiet alone.
    void theUnsignedQuestionIsWaitedForAsTheSignInIs()
    {
        ProxmoxByHand pve(proxmoxTls());
        const ProxmoxDevice::Pace pace = quickProxmox();
        QVERIFY(pace.signInQuietMs > pace.quietMs + 1000);
        // (Later than an ordinary question's quiet, and well within the sign-in's.)
        pve.refuseAfterMs = pace.quietMs + 1000;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        const Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QCOMPARE(pve.unsignedAsked, QStringList{u"GET /version"_s});
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
    }

    // Outside review of 0.2.0, 3.2, on every road (review cx3-1, H1): the unsigned question of its API is asked right
    // before every sign-in, a sign-in again after a 401 among them. Here what answers it is somebody else by then: the
    // token goes out no more, and the card says why.
    void aSignInAgainAfterA401AsksWhatAnswersFirst()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = quickProxmox();
        pace.holdMs = 5000;
        pace.longestHoldMs = 10'000;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(pve.unsignedAsked, QStringList{u"GET /version"_s});
        bool once = false;
        pve.refuses = [&pve, &once](const QString& said) {
            if (said != u"GET /nodes/pve1/qemu/100/status/current"_s || std::exchange(once, true)) {
                return false;
            }
            pve.unsignedStatus = 404;
            return true;
        };
        const Status status = lookedAt(device);
        QCOMPARE(status.errorCode, u"bad_response"_s);
        QVERIFY2(status.errorMessage.contains(u"does not say it is Proxmox VE"_s), qPrintable(status.errorMessage));
        QCOMPARE(pve.unsignedAsked.size(), 2);
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
        // Not asked again at the next look, before its hold is over: said as it was.
        QCOMPARE(lookedAt(device).errorCode, u"bad_response"_s);
        QCOMPARE(pve.unsignedAsked.size(), 2);
    }

    // ...and in a follow to another node of its cluster (the guest moved, and the list of its Proxmox's guests answered
    // 401): nothing more goes with the token, not the list, not its settings on the new node.
    void aFollowToAnotherNodeAsksWhatAnswersBeforeItSignsInAgain()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes = {u"pve1"_s, u"pve2"_s};
        ProxmoxDevice::Pace pace = quickProxmox();
        pace.places = std::make_shared<ProxmoxDevice::Places>();
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        pve.guests[100].node = u"pve2"_s;
        bool once = false;
        pve.refuses = [&pve, &once](const QString& said) {
            if (said != u"GET /cluster/resources"_s || std::exchange(once, true)) {
                return false;
            }
            pve.unsignedStatus = 404;
            return true;
        };
        const Status status = lookedAt(device);
        QCOMPARE(status.errorCode, u"bad_response"_s);
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
        QCOMPARE(pve.asked(u"GET /cluster/resources"_s), 0);
        QVERIFY(std::ranges::none_of(pve.requests, [](const QString& one) { return one.contains(u"/pve2/"_s); }));
    }

    // ...and a guest that goes on with another guest's sign-in with the token (taken while it waited) asks on its own
    // line first: the other's went on the other's.
    void aGuestThatGoesOnWithAnothersSignInAsksFirstOnItsOwnLine()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests.insert(101, ProxmoxByHand::Guest{u"db"_s});
        pve.versionTakesMs = 1000;
        ProxmoxDevice::Pace pace = quickProxmox();
        ProxmoxDevice first(guestOf(pve, 100), pve.secret, pace);
        ProxmoxDevice second(guestOf(pve, 101), pve.secret, pace);
        QSignalSpy firstOver(&first, &ProxmoxDevice::looked);
        first.look();
        QTRY_COMPARE_WITH_TIMEOUT(pve.asked(u"GET /version"_s), 1, 5000);
        QCOMPARE(pve.unsignedAsked.size(), 1);
        QCOMPARE(lookedAt(second).state, u"online"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!firstOver.isEmpty(), 5000);
        QCOMPARE(first.status().state, u"online"_s);
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
        QCOMPARE(pve.unsignedAsked.size(), 2);
        // What it asked counted for that taking alone: its own sign-in after a 401 a moment later asks again, and
        // somebody else answering by then is sent nothing.
        pve.versionTakesMs = 0;
        bool once = false;
        pve.refuses = [&pve, &once](const QString& said) {
            if (said != u"GET /nodes/pve1/qemu/101/status/current"_s || std::exchange(once, true)) {
                return false;
            }
            pve.unsignedStatus = 404;
            return true;
        };
        QCOMPARE(lookedAt(second).errorCode, u"bad_response"_s);
        QCOMPARE(pve.unsignedAsked.size(), 3);
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
    }

    // Review cx3-2, L1: once contact with what answers at its Proxmox's address failed (nothing there, another
    // certificate shown; for a question that is vital or not), the sign-in no longer stands for what answers there: it
    // is asked what it is before the token goes again, and somebody else that comes up there with a certificate the
    // card accepts hears nothing of the token. (No answer in time is no such failure: review cx3-3, L2, below.)
    void afterContactFailedWhatAnswersIsAskedBeforeTheTokenGoesAgain_data()
    {
        QTest::addColumn<QString>("how");
        QTest::newRow("nothing there") << u"nothing there"_s;
        QTest::newRow("another certificate") << u"another certificate"_s;
        QTest::newRow("nothing there, to a question that is not vital") << u"not vital"_s;
    }

    void afterContactFailedWhatAnswersIsAskedBeforeTheTokenGoesAgain()
    {
        QFETCH(QString, how);
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        const auto port = static_cast<quint16>(pve->address().port());
        const QString secret = pve->secret;
        const ProxmoxDevice::Pace pace = quickProxmox();
        ProxmoxDevice device(guestOf(*pve), secret, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        // (Read by a question that is not vital: the number a guest gives for itself, as an add asks it.)
        const auto number = [&device] {
            return answerOf<QString>([&device](std::function<void(base::Result<QString>)> done) {
                device.numberOfGuest(u"pve1"_s, 100, std::move(done));
            });
        };
        // (Nothing there: refused, or the line found closed. Where Qt first tries the line it kept, and then a new one
        // to a port nobody listens at, which Windows takes its time to refuse, a question may end in no answer in time
        // first, which is no failure of contact: asked again past its hold until it is one.)
        const auto awayAfterAll = [&pace](const std::function<std::string()>& ask) {
            std::string code = ask();
            for (int again = 0; code == "timeout" && again < 5; ++again) {
                QTest::qWait(pace.longestHoldMs + 200);
                code = ask();
            }
            return code;
        };
        const auto away = [](const std::string& code) { return code == "unreachable" || code == "dropped"; };
        if (how == u"another certificate"_s) {
            pve.reset();
            const SomebodyElse second("second", port);
            QCOMPARE(lookedAt(device).errorCode, u"cert_mismatch"_s);
            QVERIFY(second.heard.isEmpty());
        } else if (how == u"not vital"_s) {
            QCOMPARE(codeOf(number()), std::string("ok"));
            pve.reset();
            const std::string code = awayAfterAll([&number] { return codeOf(number()); });
            QVERIFY2(away(code), code.c_str());
        } else {
            pve.reset();
            const std::string code = awayAfterAll([&device] { return lookedAt(device).errorCode.toStdString(); });
            QVERIFY2(away(code), code.c_str());
        }
        const SomebodyElse other("first", port);
        QTest::qWait(pace.longestHoldMs + 200); // (past the hold after a failure)
        if (how == u"not vital"_s) {
            QCOMPARE(codeOf(number()), std::string("bad_response"));
        } else {
            const Status status = lookedAt(device);
            QCOMPARE(status.errorCode, u"bad_response"_s);
            QVERIFY2(status.errorMessage.contains(u"does not say it is Proxmox VE"_s), qPrintable(status.errorMessage));
        }
        QVERIFY(!other.heard.isEmpty());
        QVERIFY(!other.heardTheToken(secret));
    }

    // Review cx3-3, L3: the sign-in again after a failure of contact holds for every guest of that Proxmox, and not
    // only for the one whose question failed: another guest's driver of it, signed in before and asking nothing
    // meanwhile, asks what answers there before its token goes again too.
    void afterOneGuestLostContactEveryGuestOfItsProxmoxAsksFirst()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests.insert(101, ProxmoxByHand::Guest{u"db"_s});
        const ProxmoxDevice::Pace pace = quickProxmox();
        ProxmoxDevice first(guestOf(pve, 100), pve.secret, pace);
        ProxmoxDevice second(guestOf(pve, 101), pve.secret, pace);
        QCOMPARE(lookedAt(first).state, u"online"_s);
        QCOMPARE(lookedAt(second).state, u"online"_s);
        QCOMPARE(pve.unsignedAsked.size(), 2);
        bool lost = true;
        pve.answering = [&lost](const QString& said, Answered& answer) {
            // (Nothing back at all for the first guest's look, asked again by Qt on a new line or not.)
            answer.hangUp = lost && said == u"GET /nodes/pve1/qemu/100/status/current"_s;
        };
        const QString code = lookedAt(first).errorCode;
        lost = false;
        QVERIFY2(code == u"dropped"_s || code == u"unreachable"_s, qPrintable(code));
        QCOMPARE(lookedAt(second).state, u"online"_s);
        QCOMPARE(pve.unsignedAsked.size(), 3);
    }

    // Outside review 2, 4.2: a failure of contact is one of any question, the unsigned question of its API and the
    // sign-in among them. Another guest's driver of that Proxmox, which signs in after the first did, gets nothing back
    // to its unsigned question, or to its sign-in: every guest of that Proxmox asks what answers there before its token
    // goes again. Somebody else answers there by then, under the certificate the cards accept: it hears no token.
    void aFailedAskingOrSignInOfAnotherGuestHasEveryGuestAskFirst_data()
    {
        QTest::addColumn<bool>("signIn");
        QTest::newRow("its unsigned question") << false;
        QTest::newRow("its sign-in") << true;
    }
    void aFailedAskingOrSignInOfAnotherGuestHasEveryGuestAskFirst()
    {
        QFETCH(bool, signIn);
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        pve->guests.insert(101, ProxmoxByHand::Guest{u"db"_s});
        const auto port = static_cast<quint16>(pve->address().port());
        const QString secret = pve->secret;
        const ProxmoxDevice::Pace pace = quickProxmox();
        ProxmoxDevice first(guestOf(*pve, 100), secret, pace);
        QCOMPARE(lookedAt(first).state, u"online"_s);
        ProxmoxDevice second(guestOf(*pve, 101), secret, pace);
        if (signIn) {
            // (Nothing back at all, asked again by Qt on a new line or not.)
            pve->answering = [](const QString& said, Answered& answer) { answer.hangUp = said == u"GET /version"_s; };
        } else {
            pve->unsignedHangsUp = true;
        }
        const qsizetype signInsBefore = pve->asked(u"GET /version"_s);
        const QString code = lookedAt(second).errorCode;
        QVERIFY2(code == u"dropped"_s || code == u"unreachable"_s, qPrintable(code));
        QCOMPARE(pve->asked(u"GET /version"_s) > signInsBefore, signIn);
        pve.reset();
        const SomebodyElse other("first", port);
        QTest::qWait(pace.longestHoldMs + 200); // (past the hold after a failure)
        const Status status = lookedAt(first);
        QVERIFY2(status.state != u"online"_s, qPrintable(status.state));
        QVERIFY(!other.heardTheToken(secret));
    }

    // ...and a sign-in that was out when contact was lost (another guest's question got nothing back after this one's
    // unsigned question was answered, and before its sign-in was) stood for what answered before that: its driver asks
    // again, and signs in again, before its token goes with the question that waited for that sign-in.
    void aSignInOutWhenContactWasLostStandsForWhatAnsweredBefore()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests.insert(101, ProxmoxByHand::Guest{u"db"_s});
        ProxmoxDevice::Pace pace = quickProxmox();
        pace.signInQuietMs = 10'000; // (its sign-ins are held five seconds: room for the first's look on a loaded PC)
        ProxmoxDevice first(guestOf(pve, 100), pve.secret, pace);
        QCOMPARE(lookedAt(first).state, u"online"_s);
        ProxmoxDevice second(guestOf(pve, 101), pve.secret, pace);
        pve.versionTakesMs = 5000;
        qsizetype askedBeforeItsLook = -1;
        pve.answering = [&pve, &askedBeforeItsLook](const QString& said, Answered& answer) {
            answer.hangUp = said == u"GET /nodes/pve1/qemu/100/status/current"_s;
            if (said == u"GET /nodes/pve1/qemu/101/status/current"_s && askedBeforeItsLook < 0) {
                askedBeforeItsLook = pve.unsignedAsked.size();
            }
        };
        QSignalSpy secondOver(&second, &ProxmoxDevice::looked);
        second.look();
        // Its sign-in is out (its unsigned question answered): the first guest's look gets nothing back meanwhile.
        QTRY_COMPARE_WITH_TIMEOUT(pve.asked(u"GET /version"_s), 2, 10'000);
        const QString code = lookedAt(first).errorCode;
        QVERIFY2(code == u"dropped"_s || code == u"unreachable"_s, qPrintable(code));
        QVERIFY(secondOver.isEmpty()); // (its sign-in still out)
        QTRY_VERIFY_WITH_TIMEOUT(!secondOver.isEmpty(), 30'000);
        QCOMPARE(second.status().state, u"online"_s);
        // Its look went with the token only after it asked again: the first guest's unsigned question, its own before
        // its sign-in, and its own after the loss of contact; and a sign-in after that.
        QCOMPARE(askedBeforeItsLook, qsizetype{3});
        QCOMPARE(pve.asked(u"GET /version"_s), 3);
    }

    // Fresh review of 4.2's fix, M1: a certificate refused is a failure of contact for the guests known the same way
    // alone (by that pin, or by Windows' trust). Its Proxmox's certificate renewed, one guest's card said yes to the
    // new one and another's not yet: the other's refusals at TLS, look after look, leave the first signed in, asking
    // nothing again, and its press goes. (Nothing there, or a line fallen, is one for every guest of that Proxmox
    // still: the tests above.)
    void aCertificateRefusedForOneGuestLeavesTheOthersSignedIn()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests.insert(101, ProxmoxByHand::Guest{u"db"_s});
        const ProxmoxDevice::Pace pace = quickProxmox();
        ProxmoxDevice renewed(guestOf(pve, 100), pve.secret, pace);
        ProxmoxDevice::Where stale = guestOf(pve, 101);
        stale.certificate = certificateOf("second"); // (the one its Proxmox showed before)
        ProxmoxDevice notYet(stale, pve.secret, pace);
        QCOMPARE(lookedAt(renewed).state, u"online"_s);
        const qsizetype unsignedBefore = pve.unsignedAsked.size();
        const int signInsBefore = pve.asked(u"GET /version"_s);
        for (int round = 0; round < 5; ++round) {
            QCOMPARE(lookedAt(notYet).errorCode, u"cert_mismatch"_s);
            QCOMPARE(lookedAt(renewed).state, u"online"_s);
            QTest::qWait(pace.longestHoldMs + 200); // (past the other's hold: it is asked again each time)
        }
        QCOMPARE(pve.unsignedAsked.size(), unsignedBefore);
        QCOMPARE(pve.asked(u"GET /version"_s), signInsBefore);
        const base::Result<Done> reset = answerOf<Done>([&renewed](std::function<void(base::Result<Done>)> done) {
            renewed.act(Action::plain(u"atx_reset"_s), std::move(done));
        });
        QVERIFY2(reset, reset ? "" : reset.error().message.c_str());
        QCOMPARE(pve.asked(u"POST /nodes/pve1/qemu/100/status/reset"_s), 1);
        QCOMPARE(pve.unsignedAsked.size(), unsignedBefore);
    }

    // ...and it is one for every guest known that same way: two guests known by one certificate, another shown to the
    // second (somebody else at the address), and then the first certificate again, by somebody else still. The first
    // guest asks what answers before its token goes again, and hears that it is no Proxmox.
    void aCertificateRefusedForOneGuestSignsOutTheOthersKnownTheSameWay()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        pve->guests.insert(101, ProxmoxByHand::Guest{u"db"_s});
        const auto port = static_cast<quint16>(pve->address().port());
        const QString secret = pve->secret;
        const ProxmoxDevice::Pace pace = quickProxmox();
        ProxmoxDevice first(guestOf(*pve, 100), secret, pace);
        ProxmoxDevice second(guestOf(*pve, 101), secret, pace);
        QCOMPARE(lookedAt(first).state, u"online"_s);
        QCOMPARE(lookedAt(second).state, u"online"_s);
        pve.reset();
        {
            const SomebodyElse another("second", port);
            QCOMPARE(lookedAt(second).errorCode, u"cert_mismatch"_s);
            QVERIFY(another.heard.isEmpty());
        }
        const SomebodyElse other("first", port);
        const Status status = lookedAt(first);
        QCOMPARE(status.errorCode, u"bad_response"_s);
        QVERIFY(!other.heard.isEmpty());
        QVERIFY(!other.heardTheToken(secret));
    }

    // Review cx3-3, L2 and L3: a question that got no answer in time on a line let in by what the Proxmox is known by
    // (a real Proxmox writes its answer, status line and all, once it is ready: late where it is busy), or one whose
    // answer began and was refused for its size, is no failure of contact: the token went on that very line, and asking
    // what answers there would keep it from nobody. The next question carries it with no unsigned question and no
    // sign-in first.
    void anAnswerTooLateOrTooLargeIsNoFailureOfContact_data()
    {
        QTest::addColumn<QString>("how");
        QTest::addColumn<QString>("code");
        QTest::newRow("its status line too late") << u"late"_s << u"timeout"_s;
        QTest::newRow("said to be too large") << u"large"_s << u"dropped"_s;
    }
    void anAnswerTooLateOrTooLargeIsNoFailureOfContact()
    {
        QFETCH(QString, how);
        QFETCH(QString, code);
        ProxmoxByHand pve(proxmoxTls());
        const ProxmoxDevice::Pace pace = quickProxmox();
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(pve.unsignedAsked.size(), 1);
        bool once = false;
        pve.answering = [&how, &once, &pace](const QString& said, Answered& answer) {
            if (said != u"GET /nodes/pve1/qemu/100/status/current"_s || std::exchange(once, true)) {
                return;
            }
            if (how == u"late"_s) {
                answer.afterMs = pace.connectWithinMs + pace.quietMs + pace.transferMs + 2000;
            } else {
                answer.moreToCome = 3 * 1024 * 1024; // (more than a Proxmox's answer may be: 2 MB)
            }
        };
        QCOMPARE(lookedAt(device).errorCode, code);
        QTest::qWait(pace.longestHoldMs + 200); // (past the hold after a failure)
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(pve.unsignedAsked.size(), 1);
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
    }

    // The web's test_a_token_made_anew_just_after_the_first_guest_was_let_in_is_one_no_for_all, in part: a question
    // answered 401 after the sign-in is signed in to once more, and a no then is a no.
    void aTokenTakenBackIsSignedInToOnceMore()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.secretAfterVersion = u"11111111-2222-3333-4444-555555555555"_s;
        ProxmoxDevice device(guestOf(pve), u"0b9e4f2a-5c1d-4e8f-9a3b-7d6c5e4f3a2b"_s, quickProxmox());
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(pve.refusedPaths, (QStringList{u"GET /nodes/pve1/qemu/100/status/current"_s, u"GET /version"_s}));
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(pve.refused, 2);
    }

    // The web's `relogged`: a Proxmox that takes the token for its version and refuses it for everything else is not
    // sent it at every look meanwhile.
    void aTokenTakenAndRefusedAfterIsHeldBack()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.hostile = u"refuses_after"_s;
        ProxmoxDevice device(guestOf(pve), pve.secret, quickProxmox());
        const Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorMessage, u"Proxmox takes the token and refuses the request after it"_s);
        QCOMPARE(pve.asked(u"GET /version"_s), 2);
        QCOMPARE(pve.refused, 2);
        QCOMPARE(lookedAt(device).state, u"error"_s);
        QCOMPARE(lookedAt(device).errorMessage, u"Proxmox takes the token and refuses the request after it"_s);
        QCOMPARE(pve.asked(u"GET /version"_s), 2);
        QCOMPARE(pve.refused, 2);
    }

    // The web's test_the_guests_a_token_may_see_are_listed_to_pick_from: not the one the token may not see, nor a
    // container, which is counted.
    void theGuestsATokenMaySeeAreListed()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests.insert(101, ProxmoxByHand::Guest{u"db"_s, u"pve1"_s, u"stopped"_s});
        ProxmoxByHand::Guest hidden{u"hidden"_s};
        hidden.maySee = false;
        pve.guests.insert(102, hidden);
        pve.containers = {300};
        ProxmoxDevice::Where where = guestOf(pve);
        where.node.clear();
        where.vmid = 0;
        ProxmoxDevice asker(where, pve.secret, quickProxmox());
        const auto listing = listed(asker);
        QVERIFY2(listing, listing ? "" : listing.error().message.c_str());
        const QList<qkvm::core::proxmox::Listed> expected{{100, u"pve1"_s, u"web"_s, u"running"_s},
                                                          {101, u"pve1"_s, u"db"_s, u"stopped"_s}};
        QCOMPARE(listing.value().guests, expected);
        QCOMPARE(listing.value().containers, 1);
        // Some sixteen megabytes of guests are more than is read.
        pve.hostile = u"huge_list"_s;
        QVERIFY(!listed(asker));
    }

    // The web's test_an_answer_nested_beyond_reading_is_no_answer, and what is no Proxmox: neither yes nor no, and the
    // token not sent again at the next look.
    void whatSaysItIsNoProxmoxIsNoSignIn()
    {
        for (const QString& how : {u"deep"_s, u"not_proxmox"_s}) {
            ProxmoxByHand pve(proxmoxTls());
            pve.hostile = how;
            ProxmoxDevice device(guestOf(pve), pve.secret, quickProxmox());
            const Status status = lookedAt(device);
            QCOMPARE(status.state, u"error"_s);
            QCOMPARE(status.errorCode, u"no_sign_in"_s);
            QCOMPARE(lookedAt(device).errorCode, u"no_sign_in"_s);
            QCOMPARE(pve.asked(u"GET /version"_s), 1);
        }
    }

    // The web's test_a_proxmox_that_says_the_token_back_does_not_put_it_in_the_log_or_on_the_tile.
    void theTokenSaidBackIsNotOnTheCard()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.hostile = u"echo"_s;
        ProxmoxDevice device(guestOf(pve), pve.secret, quickProxmox());
        const Status status = lookedAt(device);
        QCOMPARE(status.errorMessage, u"Proxmox answered look at this guest with HTTP 503: upstream failed; the "
                                      u"request had Authorization: (the token)"_s);
        pve.hostile = u"echo_spelled"_s;
        const Status spelt = lookedAt(device);
        QVERIFY(!spelt.errorMessage.contains(u"0b9e"_s));
        QVERIFY(spelt.errorMessage.endsWith(QString(qkvm::core::proxmox::saidTheToken)));
        QVERIFY(!spelt.errorMessage.contains(pve.tokenId));
    }

    // Its Proxmox is known by its certificate: one that shows another is sent nothing, the token least of all.
    void anotherCertificateIsSentNothing()
    {
        ProxmoxByHand pve(proxmoxTls("second"));
        ProxmoxDevice device(guestOf(pve), pve.secret, quickProxmox());
        const Status status = lookedAt(device);
        QCOMPARE(status.errorCode, u"cert_mismatch"_s);
        QCOMPARE(status.state, u"error"_s);
        QVERIFY(pve.requests.isEmpty());
        QCOMPARE(pve.refused, 0);
    }

    // A Proxmox that is not there is offline, and left alone a while.
    void aProxmoxNotThereIsOffline()
    {
        NobodyThere nobody;
        ProxmoxDevice::Where where;
        where.address = QUrl(u"https://127.0.0.1:%1"_s.arg(nobody.port()));
        where.node = u"pve1"_s;
        where.vmid = 100;
        where.user = u"wall@pve!qkvm"_s;
        ProxmoxDevice device(where, u"0b9e4f2a-5c1d-4e8f-9a3b-7d6c5e4f3a2b"_s, quickProxmox());
        const Status status = lookedAt(device);
        QCOMPARE(status.state, u"offline"_s);
        QCOMPARE(status.errorCode, u"unreachable"_s);
    }

    // A guest asked on a node it is not on, or one the token may not see, says so in Proxmox's words.
    void aGuestNotThereSaysSo()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxByHand::Guest hidden{u"hidden"_s};
        hidden.maySee = false;
        pve.guests.insert(102, hidden);
        ProxmoxDevice unseen(guestOf(pve, 102), pve.secret, quickProxmox());
        const Status status = lookedAt(unseen);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorMessage,
                 u"Proxmox does not let the token look at this guest: Permission check failed (/vms/102, VM.Audit)"_s);
    }
};

QTEST_GUILESS_MAIN(TestProxmoxDevice)
#include "test_proxmox_device.moc"
