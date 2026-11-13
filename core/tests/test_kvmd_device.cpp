#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QTcpServer>
#include <QTest>
#include <QWebSocket>

#include <qkvm/core/kvmd_device.h>

#include "made_up_kvm.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmdDevice;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::good;
using core::testing::jpeg;
using core::testing::MadeUpKvm;
using core::testing::refusal;
using core::testing::sign;
using core::testing::StandIn;

namespace {

// Reproduce a received request whose send notification was not recorded. This
// only changes the reply's observation flag; the loopback exchange still occurs.
class UnreportedSend final : public QObject {
public:
    int forgotten = 0;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::DynamicPropertyChange && qobject_cast<QNetworkReply*>(watched) != nullptr &&
            static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "wentOut"_ba &&
            watched->property("wentOut").toBool()) {
            ++forgotten;
            watched->setProperty("wentOut", false);
        }
        return false;
    }
};

// While one of these is there, the program's lines trust what these makers of certificates signed,
// as they trust what the makers that Windows knows signed.
class VouchedFor {
public:
    explicit VouchedFor(const QString& makersFile) : m_before(QSslConfiguration::defaultConfiguration())
    {
        QSslConfiguration with = m_before;
        with.addCaCertificates(QSslCertificate::fromPath(makersFile, QSsl::Pem));
        QSslConfiguration::setDefaultConfiguration(with);
    }
    ~VouchedFor() { QSslConfiguration::setDefaultConfiguration(m_before); }
    VouchedFor(const VouchedFor&) = delete;
    VouchedFor& operator=(const VouchedFor&) = delete;
    VouchedFor(VouchedFor&&) = delete;
    VouchedFor& operator=(VouchedFor&&) = delete;

private:
    QSslConfiguration m_before;
};

} // namespace

// One KVM spoken to by the program itself, against a made-up one that answers as kvmd does.
class TestKvmdDevice : public QObject {
    Q_OBJECT

private:
    // The times a real KVM is given, made short.
    static KvmdDevice::Pace quick()
    {
        KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.holdMs = 400;
        pace.longestHoldMs = 1600;
        pace.refusedHoldMs = 1200;
        pace.signInRetryMs = 800;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        pace.restAfterMs = 900;
        // (The power board's: test_kvmd_board has what they hold.)
        pace.pressHeldMs = 100;
        pace.longPressHeldMs = 300;
        pace.boardAnswerMs = 1500;
        pace.seenWithinMs = 600;
        pace.seenEveryMs = 100;
        pace.boards = std::make_shared<core::BoardHolds>();
        return pace;
    }
    // The same, with far longer to be let in: for a test of what happens to a question that has
    // gone out. Under load, being let in has taken longer than quick() gives, and such a test
    // then heard "unreachable" of a KVM that was there, where it looks for "dropped".
    static KvmdDevice::Pace patient()
    {
        KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 10'000;
        return pace;
    }
    // As quick(), where a picture asked of a KVM nothing is known of yet waits for the first look's end, and then for
    // the picture itself, the events, however long they take, and not 400 ms each: what these cases look at is what
    // comes of them, and on the loaded slow cores each took longer (the fix round's item, 2 of 4 whole runs). (A case
    // that looks at one of those moments itself, aPictureWaitsOnlyAMomentFor... or aSlowKvmDoesNotHoldACardUp, keeps
    // its own.)
    static KvmdDevice::Pace waitingForTheFirstLook(KvmdDevice::Pace pace = quick())
    {
        pace.firstContactMs = 10'000;
        pace.patienceMs = 10'000;
        return pace;
    }

    static core::testing::Tls tls(const char* name)
    {
        const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
        return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
    }
    static QByteArray sha256Of(const char* name)
    {
        return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
    }

    // Asks, lets the program's loop run until the answer is there, and gives it.
    template <typename Answer>
    static base::Result<Answer> answerTo(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
    {
        QList<base::Result<Answer>> answers;
        ask([&](base::Result<Answer> answer) { answers.append(std::move(answer)); });
        if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 20000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return answers.takeFirst();
    }
    static base::Result<core::Snapshot> pictureOf(KvmdDevice& device, int width = 400)
    {
        return answerTo<core::Snapshot>([&](auto done) { device.askSnapshot(width, std::move(done)); });
    }
    static base::Result<core::Done> doneBy(KvmdDevice& device, const core::Action& what)
    {
        return answerTo<core::Done>([&](auto done) { device.act(what, std::move(done)); });
    }
    // Has the KVM looked at, and says how it is once that look is over.
    static core::Status lookedAt(KvmdDevice& device)
    {
        QSignalSpy over(&device, &KvmdDevice::looked);
        device.look();
        if (!over.wait(20000)) {
            core::Status never;
            never.state = u"the look never ended"_s;
            return never;
        }
        return device.status();
    }

private slots:
    void initTestCase()
    {
        // The device chooses Windows' own TLS, which is refused once TLS is in use: so one is
        // made before anything else touches TLS (the stand-ins do).
        const KvmdDevice first(KvmdDevice::Where{}, QString());
    }

    void qtSaysWhenAQuestionHasGoneOut()
    {
        // What the driver leans on: whether a question has gone out decides between "the KVM is
        // not there" and "it may have been done", and Qt is what says so.
        StandIn silent([](const Asked&) {
            Answered never;
            never.never = true;
            return never;
        });
        QNetworkAccessManager network;
        QNetworkRequest request(QUrl(silent.address() + u"/x"_s));
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        QNetworkReply* asked = network.get(request);
        QSignalSpy askedWent(asked, &QNetworkReply::requestSent);
        const bool getSaid = askedWent.wait(3000);
        request.setRawHeader("Content-Type"_ba, "text/plain"_ba);
        QNetworkReply* full = network.post(request, "abc"_ba);
        QSignalSpy fullWent(full, &QNetworkReply::requestSent);
        const bool fullSaid = fullWent.wait(3000);
        QVERIFY(getSaid);
        QVERIFY(fullSaid);
        // (Qt says it went as it is written; the stand-in has it a moment later.)
        QTRY_COMPARE(silent.asked.size(), 2);

        // With nothing to send along, Qt does not say it, although the question went. So the
        // driver puts no limit on such a question's being let in: it could not tell a button the
        // KVM holds for seconds from a KVM that is not there. (If this ever fails, Qt has mended
        // it, and the driver may treat all questions alike.)
        request.setRawHeader("Content-Type"_ba, "application/octet-stream"_ba);
        QNetworkReply* bare = network.post(request, QByteArray());
        QSignalSpy bareWent(bare, &QNetworkReply::requestSent);
        QVERIFY(!bareWent.wait(1500));
        QCOMPARE(silent.asked.size(), 3);

        // And Qt's other way of sending one, by the name of its kind, is no way: what stands
        // behind the question mark is then moved into the question, where a KVM does not look.
        StandIn noting([](const Asked&) { return Answered{200, "{}"_ba}; });
        QNetworkRequest named(QUrl(noting.address() + u"/api/atx/click?button=power"_s));
        named.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        named.setRawHeader("Content-Type"_ba, "application/octet-stream"_ba);
        QNetworkReply* byName = network.sendCustomRequest(named, "POST"_ba);
        QSignalSpy byNameOver(byName, &QNetworkReply::finished);
        QVERIFY(byNameOver.wait(5000));
        QCOMPARE(noting.asked.constLast().path, "/api/atx/click"_ba);
        QCOMPARE(noting.asked.constLast().body, "button=power"_ba);
        QNetworkReply* plainWay = network.post(named, QByteArray());
        QSignalSpy plainWayOver(plainWay, &QNetworkReply::finished);
        QVERIFY(plainWayOver.wait(5000));
        QCOMPARE(noting.asked.constLast().path, "/api/atx/click?button=power"_ba);
        QVERIFY(noting.asked.constLast().body.isEmpty());
    }

    void itIsSaidWhenAQuestionWentOnALineThatWasThereAlready()
    {
        // The same, where the line was made for an earlier question and kept: a real KVM keeps
        // its lines, and most questions go out on one that is there.
        MadeUpKvm kvm;
        kvm.stand->keepsLines = true;
        KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 400;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        const int lines = kvm.stand->lines;
        Answered late = good({{u"streamer"_s, QJsonObject{}}});
        late.afterMs = 1200;
        kvm.first["/api/hid/print"_ba].append(late);
        kvm.first["/api/streamer"_ba].append(late);
        QVERIFY(doneBy(device, core::Action::type(u"a long text"_s, false)).ok());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.asked("/api/hid/print"), 1);
        // (They did go out on lines that were there.)
        QCOMPARE(kvm.stand->lines, lines);
    }

    void anAnswerThatTakesLongerThanBeingLetInIsWaitedFor()
    {
        // To be let in a KVM has a few seconds; to answer, as long as what was asked takes it.
        // The two must not be taken for each other: a button the KVM holds for seconds is not
        // "the KVM is not there", which would have it pressed again.
        MadeUpKvm kvm;
        kvm.atx = true;
        KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 400;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        Answered late = good();
        late.afterMs = 1000;
        kvm.instead.insert("/api/atx/click"_ba, late);
        kvm.instead.insert("/api/hid/print"_ba, late);
        kvm.instead.insert("/api/streamer"_ba, good({{u"streamer"_s, QJsonObject{}}}));
        kvm.first["/api/streamer"_ba].append(late);
        QVERIFY(doneBy(device, core::Action::plain(u"atx_power_long"_s)).ok());
        QVERIFY(doneBy(device, core::Action::type(u"a long text"_s, false)).ok());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.asked("/api/atx/click"), 1);
        QCOMPARE(kvm.asked("/api/hid/print"), 1);
    }

    void itSignsInOnceAndCarriesThePass()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(device.status().state, u"connecting"_s);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);

        const Asked* signIn = kvm.last("/api/auth/login");
        QVERIFY(signIn != nullptr);
        QCOMPARE(signIn->method, "POST"_ba);
        QCOMPARE(signIn->body, "user=admin&passwd=secret"_ba);
        QCOMPARE(signIn->headers.value("content-type"), "application/x-www-form-urlencoded"_ba);
        QVERIFY(!signIn->headers.contains("cookie"));
        // Before it, whether it signs this program in, with nothing to sign in with: a password goes only on a line
        // shown to be up. Every question after it carries the pass, and asks for an answer that is not packed.
        QCOMPARE(kvm.stand->asked.first().path, "/api/auth/check"_ba);
        QVERIFY(!kvm.stand->asked.first().headers.contains("cookie"));
        for (qsizetype index = 0; index < kvm.stand->asked.size(); ++index) {
            const Asked& one = kvm.stand->asked.at(index);
            QCOMPARE(one.headers.value("accept-encoding"), "identity"_ba);
            if (index > 0 && one.path != "/api/auth/login") {
                QCOMPARE(one.headers.value("cookie"), "auth_token=pass1"_ba);
            }
        }
    }

    void aPasswordArrivesAsItWasTyped()
    {
        MadeUpKvm kvm;
        kvm.password = u"p+a s&s=w%rd#"_s + sign(0xE4) + sign(0x1F600);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
    }

    void theOtherMakersPassComesAsACookie()
    {
        MadeUpKvm kvm;
        kvm.passAsCookie = true;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.last("/api/streamer")->headers.value("cookie"), "auth_token=pass1"_ba);
    }

    void aRefusedPasswordIsSaidAndNotSentAgainForAWhile()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"not it"_s, quick());
        core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"auth"_s);
        QCOMPARE(status.errorCode, u"auth_failed"_s);
        QCOMPARE(status.errorMessage, u"Login rejected - check the username and password"_s);
        // Every look, every picture and whatever somebody asks for ends up at the same sign-in:
        // a KVM locks an account out, and the password is not hammered at it.
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(pictureOf(device).error().code, std::string("auth_failed"));
        QCOMPARE(doneBy(device, core::Action::plain(u"atx_power"_s)).error().code, std::string("auth_failed"));
        QCOMPARE(kvm.signIns, 1);
        // After a while it is tried once more.
        QTest::qWait(quick().refusedHoldMs + 100);
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(kvm.signIns, 2);
    }

    void aPasswordRefusedAgainWaitsLongerEachTime()
    {
        MadeUpKvm kvm;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice device(kvm.where(), u"not it"_s, pace);
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(kvm.signIns, 1);
        // The first wait, and it is offered once more: refused again.
        QTest::qWait(pace.refusedHoldMs + 100);
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(kvm.signIns, 2);
        // Now five times as long: after the first wait again it is not offered.
        QTest::qWait(pace.refusedHoldMs + 100);
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(kvm.signIns, 2);
        QTest::qWait(4 * pace.refusedHoldMs);
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(kvm.signIns, 3);
    }

    void aDriverMadeAnewWaitsForTheNoTheOneBeforeItHeard()
    {
        // The wall makes a KVM's driver anew whenever the KVM is changed: the new one is not a reason to
        // offer a refused password again at once.
        MadeUpKvm kvm;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        {
            KvmdDevice before(kvm.where(), u"not it"_s, pace);
            QCOMPARE(lookedAt(before).state, u"auth"_s);
        }
        QCOMPARE(kvm.signIns, 1);
        KvmdDevice anew(kvm.where(), u"not it"_s, pace);
        const core::Status status = lookedAt(anew);
        QCOMPARE(status.state, u"auth"_s);
        QCOMPARE(status.errorCode, u"auth_failed"_s);
        QCOMPARE(status.errorMessage, u"Login rejected - check the username and password"_s);
        QCOMPARE(kvm.signIns, 1);
        // Another password is another sign-in; and the refused one, typed anew by a person, is offered
        // at once (the boxes that sign in with a typed one say so: KvmdDevice::tryAfresh).
        KvmdDevice other(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(other).state, u"online"_s);
        QCOMPARE(kvm.signIns, 2);
        // A sign-in that was let in leaves nothing behind: one more driver with that password signs in
        // at once, and is not held back as if it had come to nothing.
        KvmdDevice again(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(again).state, u"online"_s);
        QCOMPARE(kvm.signIns, 3);
        KvmdDevice::tryAfresh(kvm.where(), u"not it"_s, pace);
        KvmdDevice typed(kvm.where(), u"not it"_s, pace);
        QCOMPARE(lookedAt(typed).state, u"auth"_s);
        QCOMPARE(kvm.signIns, 4);
    }

    void whatWaitedForASignInThatGotNoAnswerWasNeverSent()
    {
        // The sign-in went out and nothing answered it. What waited for it went nowhere, and is not told
        // so in words that say it may have been done; nor is a restart taken for done (a KVM that goes
        // down may never answer its own restart). Nothing is sent while the sign-in is held back.
        MadeUpKvm kvm;
        Answered silent;
        silent.never = true;
        kvm.instead.insert("/api/auth/login"_ba, silent);
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const auto first = doneBy(device, core::Action::plain(u"reboot"_s));
        QVERIFY(!first.ok());
        QCOMPARE(first.error().code, std::string("no_sign_in"));
        QCOMPARE(first.error().message,
                 std::string("The device does not sign qKVM in (Device did not respond in time). It is asked again "
                             "shortly."));
        QVERIFY(!KvmdDevice::mayHaveBeenDone(first.error()));
        const auto held = doneBy(device, core::Action::plain(u"reboot"_s));
        QVERIFY(!held.ok());
        QCOMPARE(held.error().code, std::string("no_sign_in"));
        QCOMPARE(kvm.asked("/api/auth/login"_ba), 1);
        QVERIFY(kvm.did.isEmpty());
        // What it came to replaced the hold it went out with (as long as it could be out): held as one that came to
        // nothing, and sent again after that wait.
        QTest::qWait(pace.signInRetryMs + 200);
        static_cast<void>(doneBy(device, core::Action::plain(u"reboot"_s)));
        QCOMPARE(kvm.asked("/api/auth/login"_ba), 2);
    }

    void aKvmThatDoesNotSignTheProgramInIsSaidSoUntilItIsGone()
    {
        // Held back after a sign-in that brought no pass, the card says so, and not that the KVM is away.
        // While it is held, nothing that needs a sign-in is asked, and whether the KVM is there at all is
        // asked now and then, with no password: once it is gone, it is offline.
        auto kvm = std::make_unique<MadeUpKvm>();
        kvm->signInStatus = 500;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.signInRetryMs = 60'000;   // held for all of it
        pace.stillThereEveryMs = 1500; // (room for a look that is "at once" on a slow machine)
        pace.holdMs = 3000;            // and away, once it is gone, for long enough to be asked of it
        pace.longestHoldMs = 3000;
        KvmdDevice device(kvm->where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"error"_s);
        // Looked at again at once: it answered of late, and is not asked whether it is there.
        const int checks = kvm->asked("/api/auth/check"_ba);
        QCOMPARE(lookedAt(device).state, u"error"_s);
        QCOMPARE(kvm->asked("/api/auth/check"_ba), checks);
        // Later it is, and it answers.
        QTest::qWait(pace.stillThereEveryMs + 100);
        QCOMPARE(lookedAt(device).state, u"error"_s);
        QCOMPARE(kvm->asked("/api/auth/check"_ba), checks + 1);
        QCOMPARE(kvm->signIns, 1);
        // What anybody may ask: no password, no session, nothing in it, in none of the ways a KVM signs one
        // in. (This driver was never let in and has no session to carry: that none goes is its own state's
        // doing, and this guards against one that is sent all the same.)
        const auto check =
            std::ranges::find_if(kvm->stand->asked, [](const Asked& one) { return one.path == "/api/auth/check"; });
        QVERIFY(check != kvm->stand->asked.end());
        QCOMPARE(check->method, "GET"_ba);
        QVERIFY(check->body.isEmpty());
        for (const auto& [name, value] : check->headers.asKeyValueRange()) {
            QVERIFY2(name != "cookie" && name != "authorization" && !name.startsWith("x-kvmd"), name.constData());
            QVERIFY2(!value.contains("secret"), name.constData());
        }
        // Gone: it hangs up on the one question it is asked while its sign-in is held, whether it is there.
        // (Its port stays held by this test: one that is let go of is given by Windows to whoever asks next,
        // another test program's server among them.)
        Answered hangsUp;
        hangsUp.hangUp = true;
        kvm->instead.insert("/api/auth/check"_ba, hangsUp);
        QTest::qWait(pace.stillThereEveryMs + 100);
        const core::Status gone = lookedAt(device);
        QCOMPARE(gone.state, u"offline"_s);
        QCOMPARE(gone.errorCode, u"dropped"_s);
        // And away as any KVM that is away: whoever wants its picture is told so, and nothing is asked of it
        // (counted once the loop has run: a question sent meanwhile is on its way for a while).
        const qsizetype asked = kvm->stand->asked.size();
        QCOMPARE(pictureOf(device).error().code, std::string("dropped"));
        QTest::qWait(pace.stillThereEveryMs);
        QCOMPARE(kvm->stand->asked.size(), asked);
    }

    void aHoldThatIsLiftedIsLiftedForTheDriverThatWasHeld()
    {
        // A driver keeps no time of its own for a hold the pacing keeps: once the password is typed afresh,
        // the driver that was held signs in with it at once, and does not wait out a time it noted itself.
        MadeUpKvm kvm;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.refusedHoldMs = 60'000;
        pace.signInRetryMs = 60'000;
        // Refused; then the password is made the KVM's own on the KVM, and typed afresh in a box.
        KvmdDevice refused(kvm.where(), u"the new one"_s, pace);
        QCOMPARE(lookedAt(refused).state, u"auth"_s);
        kvm.password = u"the new one"_s;
        KvmdDevice::tryAfresh(kvm.where(), u"the new one"_s, pace);
        QCOMPARE(lookedAt(refused).state, u"online"_s);
        // Held after an answer that brought no pass; the KVM mended, and the password typed afresh.
        kvm.signInStatus = 500;
        KvmdDevice held(kvm.where(), u"the new one"_s, pace);
        QCOMPARE(lookedAt(held).state, u"error"_s);
        kvm.signInStatus = 200;
        KvmdDevice::tryAfresh(kvm.where(), u"the new one"_s, pace);
        QCOMPARE(lookedAt(held).state, u"online"_s);
    }

    void aHoldIsSaidInTheWordsOfTheOneThatStands()
    {
        // A driver says its own words of a hold only while the hold it noted is the one that stands: once
        // another driver of the KVM has a sign-in out, it is said as that one.
        MadeUpKvm kvm;
        kvm.signInStatus = 500;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice first(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(first).errorMessage,
                 u"The device does not sign qKVM in (Login failed (HTTP 500)). It is asked again shortly."_s);
        // Its hold waited out, another driver of the KVM signs in, and nothing answers that one.
        QTest::qWait(pace.signInRetryMs + 100);
        Answered silent;
        silent.never = true;
        kvm.instead.insert("/api/auth/login"_ba, silent);
        KvmdDevice second(kvm.where(), u"secret"_s, pace);
        second.look();
        QTRY_COMPARE(kvm.asked("/api/auth/login"_ba), 2);
        const core::Status status = lookedAt(first);
        QCOMPARE(status.errorCode, u"no_sign_in"_s);
        QCOMPARE(status.errorMessage, u"The device does not sign qKVM in (a sign-in to it is under way, or the last "
                                      "one came to nothing). It is asked again shortly."_s);
        QCOMPARE(kvm.asked("/api/auth/login"_ba), 2);
    }

    void aNoThatTakesItsTimeIsStillANo()
    {
        // Some KVMs hold a no back on purpose: a sign-in's answer is waited for two and a half times as
        // long as any other's, and a no that comes after the usual wait is a no, and paced as one.
        MadeUpKvm kvm;
        Answered slowNo = refusal(403, u"ForbiddenError"_s);
        slowNo.afterMs = quick().quietMs * 7 / 4;
        kvm.instead.insert("/api/auth/login"_ba, slowNo);
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice device(kvm.where(), u"not it"_s, pace);
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"auth"_s);
        QCOMPARE(status.errorCode, u"auth_failed"_s);
        QCOMPARE(
            pace.pacing->times(core::SignInPacing::what(u"login", kvm.where().address, kvm.where().user, u"not it"_s)),
            1);
    }

    void aSignInThatGotNoAnswerAfterANoIsSaidAsTheNo()
    {
        // Refused, and the next sign-in, after its wait, gets no answer: nothing has shown meanwhile that the
        // password is right, and it is said as the refused password it was (the web's `_held_back`).
        MadeUpKvm kvm;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice device(kvm.where(), u"not it"_s, pace);
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QTest::qWait(pace.refusedHoldMs + 100);
        Answered silent;
        silent.never = true;
        kvm.instead.insert("/api/auth/login"_ba, silent);
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"auth"_s);
        QCOMPARE(status.errorCode, u"auth_failed"_s);
        QCOMPARE(kvm.asked("/api/auth/login"_ba), 2);
    }

    void aKvmThatRefusedThePasswordAndWentAwayIsOffline()
    {
        // While a refused password waits to be offered again (up to an hour), the KVM is asked now and then
        // whether it is there at all: once it is gone, its card says so, and not that the password was refused.
        auto kvm = std::make_unique<MadeUpKvm>();
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.refusedHoldMs = 60'000;
        pace.stillThereEveryMs = 300;
        KvmdDevice device(kvm->where(), u"not it"_s, pace);
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        // Gone: the one question it is asked while its password waits, whether it is there, gets no answer at
        // all (its port held by this test, as in aKvmThatDoesNotSignTheProgramInIsSaidSoUntilItIsGone, where
        // the KVM hangs up instead).
        Answered silent;
        silent.never = true;
        kvm->instead.insert("/api/auth/check"_ba, silent);
        QTest::qWait(pace.stillThereEveryMs + 100);
        const core::Status gone = lookedAt(device);
        QCOMPARE(gone.state, u"offline"_s);
        QCOMPARE(gone.errorCode, u"timeout"_s);
    }

    void aDriverLetGoOfWhileItsSignInIsOutLeavesItsMark()
    {
        // A sign-in is counted as it goes out: a driver let go of before the KVM answered it (the wall
        // made it anew, a window was closed) leaves its mark, and the one made in its place does not
        // send that password again as if it had never gone. Its line outlives it and hears what it comes to:
        // here nothing, its line giving up at its own end. The one made anew waits for that, is not said to be in
        // error meanwhile, and then is told the KVM does not sign it in: nothing more went.
        MadeUpKvm kvm;
        Answered silent;
        silent.never = true;
        kvm.instead.insert("/api/auth/login"_ba, silent);
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        QElapsedTimer watch;
        {
            KvmdDevice before(kvm.where(), u"secret"_s, pace);
            before.look();
            QTRY_COMPARE(kvm.asked("/api/auth/login"_ba), 1);
            watch.start();
        }
        KvmdDevice anew(kvm.where(), u"secret"_s, pace);
        QList<std::string> codes;
        anew.identify([&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        });
        QTest::qWait(2000);
        QVERIFY(codes.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 1, 15000);
        QCOMPARE(codes, QList<std::string>{"no_sign_in"});
        QVERIFY(watch.elapsed() >= pace.quietMs * 5 / 2 - 500); // (at its line's end: nothing heard for its wait)
        QCOMPARE(kvm.asked("/api/auth/login"_ba), 1);
    }

    void aDriverThatWaitsForOneLetGoOfIsLetInWhenTheKvmAnswersIt()
    {
        // (The pacing's second and third readings, M1.) The Edit box signs in, slowly; the wall's driver waits for it;
        // the person presses Cancel, and the box's driver is let go of with its sign-in out. Its line outlives it and
        // hears the yes: the wall's driver signs in itself then, not when that sign-in could no longer be out (a
        // Cancel, then Save again, waited 49.5 s for that).
        MadeUpKvm kvm;
        kvm.signInAfterMs = 1000;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        auto box = std::make_unique<KvmdDevice>(kvm.where(), u"secret"_s, pace);
        KvmdDevice wall(kvm.where(), u"secret"_s, pace);
        box->identify([](const base::Result<KvmdDevice::Identity>&) {});
        QTRY_COMPARE(kvm.signIns, 1);
        QElapsedTimer watch;
        watch.start();
        QList<std::string> codes;
        wall.identify([&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        });
        QTest::qWait(200);
        box.reset();
        QTest::qWait(400);
        QVERIFY(codes.isEmpty());
        QCOMPARE(kvm.signIns, 1);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 1, 15000);
        QCOMPARE(codes, QList<std::string>{"ok"});
        QCOMPARE(kvm.signIns, 2);
        QVERIFY2(watch.elapsed() < 4000, QByteArray::number(watch.elapsed()));
    }

    void theCertificateIsSaidOnceThoughTheSignInHasALineOfItsOwn()
    {
        // (The pacing's third reading.) The sign-in's own line shows the certificate too: said once for each driver, as
        // own_device.h has it (the wall keeps it beside its fingerprint once).
        const MadeUpKvm kvm(tls("first"));
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        KvmdDevice::Pace pace = patient();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice device(where, u"secret"_s, pace);
        QSignalSpy showed(&device, &KvmdDevice::showedItsCertificate);
        QVERIFY(answerTo<KvmdDevice::Identity>([&](auto done) { device.identify(std::move(done)); }).ok());
        QCOMPARE(showed.count(), 1);
        QVERIFY(lookedAt(device).state == u"online"_s);
        QCOMPARE(showed.count(), 1);
    }

    void aSignInHeldOpenPastItsTimeHoldsTheOtherToItsLinesEnd()
    {
        // (The pacing's third reading, M1.) The hold ended by the clock as long as the sign-in could be out, a moment
        // before its line gave up (Qt's timer for that is a coarse one, and late): a driver that waited woke at the
        // hold's end and sent the same password while the first was still on its line, two strikes. Out until its line
        // ends: here the KVM sends its answer a byte at a time, and the pacing's clock is past that time.
        MadeUpKvm kvm;
        Answered trickles = good({{u"token"_s, QString(10'000, u'x')}});
        trickles.byteEveryMs = 100;
        kvm.first["/api/auth/login"_ba] = {trickles};
        qint64 nowMs = 1'000'000;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>(2048, [&nowMs] { return nowMs; });
        KvmdDevice one(kvm.where(), u"secret"_s, pace);
        KvmdDevice other(kvm.where(), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        };
        one.identify(keep);
        QTRY_COMPARE(kvm.asked("/api/auth/login"_ba), 1);
        nowMs += core::SignInPacing::outFor(pace.connectWithinMs, pace.quietMs * 5 / 2, pace.transferMs) + 1000;
        other.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 20000);
        QCOMPARE(kvm.asked("/api/auth/login"_ba), 1);
        QCOMPARE(codes, (QList<std::string>{"no_sign_in", "no_sign_in"}));
    }

    void aDriverLetGoOfAfterSignInsThatCameToNothingHoldsNobodyOnceItIsAnswered()
    {
        // (The pacing's third reading, M2.) Three sign-ins in a row came to nothing, then the KVM is well again. The
        // wall's driver signs in and is let go of mid sign-in (a saved edit, a window closed): the hold it went out
        // with is 80, 160, up to 300 s. Its line hears the yes, and the box with the same password typed waits for that
        // alone, and is let in.
        MadeUpKvm kvm;
        kvm.signInAfterMs = 1000;
        qint64 nowMs = 1'000'000;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>(2048, [&nowMs] { return nowMs; });
        const core::SignInPacing::Mark mark =
            core::SignInPacing::what(u"login", kvm.where().address, kvm.where().user, u"secret"_s);
        const core::SignInPacing::Steps steps{
            pace.refusedHoldMs, pace.signInRetryMs,
            core::SignInPacing::outFor(pace.connectWithinMs, pace.quietMs * 5 / 2, pace.transferMs)};
        for (int one = 0; one < 5; ++one) {
            pace.pacing->unanswered(mark, steps, pace.pacing->sent(mark, steps));
            nowMs += 600'000;
        }
        auto wall = std::make_unique<KvmdDevice>(kvm.where(), u"secret"_s, pace);
        wall->identify([](const base::Result<KvmdDevice::Identity>&) {});
        QTRY_COMPARE(kvm.signIns, 1);
        // (Thirty times the wait after one that came to nothing, here: longer than twice the time it can be out.)
        QCOMPARE(pace.pacing->waitLeftMs(mark), 30 * pace.signInRetryMs);
        wall.reset();
        KvmdDevice::tryAfresh(kvm.where(), u"secret"_s, pace);
        KvmdDevice box(kvm.where(), u"secret"_s, pace);
        QList<std::string> codes;
        box.identify([&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        });
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 1, 10000);
        QCOMPARE(codes, QList<std::string>{"ok"});
        QCOMPARE(kvm.signIns, 2);
    }

    void threeDriversSignInOneAtATimeAndEachIsLetIn()
    {
        // (The pacing's second reading, L3.) Two wait for the first's yes; told at once, one of them sends first, and
        // the other waits again for that one (within its own time) rather than being told the KVM does not sign it in.
        MadeUpKvm kvm;
        kvm.signInAfterMs = 1000;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice one(kvm.where(), u"secret"_s, pace);
        KvmdDevice two(kvm.where(), u"secret"_s, pace);
        KvmdDevice three(kvm.where(), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        };
        one.identify(keep);
        QTRY_COMPARE(kvm.signIns, 1);
        two.identify(keep);
        three.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 3, 20000);
        QCOMPARE(codes, (QList<std::string>{"ok", "ok", "ok"}));
        QCOMPARE(kvm.signIns, 3);
    }

    void aWaiterToldAsAnotherHasJustSentWaitsAgain()
    {
        // (The pacing's second reading, L3.) Told at a yes, it finds another's sign-in out by the time it looks (one
        // that went first, before it was told): it waits again for that one, within its own time, and is then let in.
        MadeUpKvm kvm;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        const core::SignInPacing::Mark mark =
            core::SignInPacing::what(u"login", kvm.where().address, kvm.where().user, u"secret"_s);
        const core::SignInPacing::Steps steps{
            pace.refusedHoldMs, pace.signInRetryMs,
            core::SignInPacing::outFor(pace.connectWithinMs, pace.quietMs * 5 / 2, pace.transferMs)};
        pace.pacing->sent(mark, steps); // (another driver's, out)
        KvmdDevice wall(kvm.where(), u"secret"_s, pace);
        QList<std::string> codes;
        wall.identify([&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        });
        QTest::qWait(300);
        QVERIFY(codes.isEmpty());
        // Its yes, and at once another's goes out, before the one that waits is told.
        pace.pacing->accepted(mark);
        pace.pacing->sent(mark, steps);
        QTest::qWait(300);
        QVERIFY(codes.isEmpty());
        QCOMPARE(kvm.signIns, 0);
        pace.pacing->accepted(mark);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 1, 10000);
        QCOMPARE(codes, QList<std::string>{"ok"});
        QCOMPARE(kvm.signIns, 1);
    }

    void aPasswordTypedAnewWhileTheSameIsOutWaitsForIt()
    {
        // (The pacing's second reading, M2.) The wall's driver has the wrong password out; a person types the same one
        // in the Edit box and saves (tryAfresh): the box's sign-in waits for the one that is out, and its no is the
        // box's no too. One strike, not two.
        MadeUpKvm kvm;
        kvm.password = u"changed on the KVM"_s;
        kvm.signInAfterMs = 1500;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice wall(kvm.where(), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        };
        wall.identify(keep);
        QTRY_COMPARE(kvm.signIns, 1);
        KvmdDevice::tryAfresh(kvm.where(), u"secret"_s, pace);
        KvmdDevice box(kvm.where(), u"secret"_s, pace);
        box.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(codes, (QList<std::string>{"auth_failed", "auth_failed"}));
    }

    void aSignInThatNothingReachedKeepsNothing()
    {
        // Nothing listens there: the sign-in never went out, and the next one is not held back for it.
        const core::testing::NobodyThere nobody;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice::Where where;
        where.address = QUrl(nobody.address());
        where.user = u"admin"_s;
        KvmdDevice device(where, u"secret"_s, pace);
        QCOMPARE(lookedAt(device).errorCode, u"unreachable"_s);
        QCOMPARE(pace.pacing->waitLeftMs(core::SignInPacing::what(u"login", where.address, where.user, u"secret"_s)),
                 0);
    }

    void withoutAPasswordNothingIsSent()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), QString(), quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"auth"_s);
        QCOMPARE(status.errorCode, u"no_password"_s);
        QVERIFY(kvm.stand->asked.isEmpty());
    }

    void aSignInThatFailsOtherwiseIsNotHammeredEither()
    {
        MadeUpKvm kvm;
        kvm.signInStatus = 500;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        // It is there, and does not sign this program in: something to look at, and not a KVM that is
        // away, which "offline" would tell an alert (the web's `no_sign_in`).
        core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"no_sign_in"_s);
        QCOMPARE(status.errorMessage,
                 u"The device does not sign qKVM in (Login failed (HTTP 500)). It is asked again shortly."_s);
        QCOMPARE(lookedAt(device).state, u"error"_s);
        QCOMPARE(kvm.signIns, 1);
        kvm.signInStatus = 200;
        QTest::qWait(quick().signInRetryMs + 100);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.signIns, 2);
    }

    void aSignInThatBringsNoPassIsNone()
    {
        MadeUpKvm kvm;
        kvm.instead.insert("/api/auth/login"_ba, good({{u"token"_s, u"a pass\r\nX-More: 1"_s}}));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.errorCode, u"no_sign_in"_s);
        QCOMPARE(status.errorMessage, u"The device does not sign qKVM in (Login succeeded but the device returned "
                                      "no session token). It is asked again shortly."_s);
        // Nothing was asked with what it handed over. (The question asked first, and the sign-in.)
        QCOMPARE(kvm.stand->asked.size(), 2);
    }

    void aSessionTheKvmForgotIsSignedInToAnew()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        kvm.passes.clear(); // kvmd was started anew
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.last("/api/streamer")->headers.value("cookie"), "auth_token=pass2"_ba);
    }

    void aKvmHeardJustNowIsAskedNothingFirst()
    {
        // The question asked first where nothing of the KVM's was heard just now (aPasswordGoesOnlyOnALineShownToBeUp)
        // is not asked where something was: a pass it forgot is signed in for on the line its no came on.
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const auto askedFirst = [&kvm] {
            int found = 0;
            for (const core::testing::Asked& one : std::as_const(kvm.stand->asked)) {
                found += one.path.startsWith("/api/auth/check") && !one.headers.contains("cookie") ? 1 : 0;
            }
            return found;
        };
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(askedFirst(), 1);
        kvm.passes.clear(); // kvmd was started anew
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(askedFirst(), 1);
    }

    void aKvmThatRefusesTheSessionItGaveIsNotHammered()
    {
        MadeUpKvm kvm;
        kvm.instead.insert("/api/streamer"_ba, refusal(401, u"UnauthorizedError"_s));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"auth"_s);
        QCOMPARE(status.errorMessage, u"Device keeps rejecting the session"_s);
        // Signed in, refused, signed in once more, refused: and that is all.
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.asked("/api/streamer"), 2);
    }

    void howAKvmIsIsRead()
    {
        MadeUpKvm kvm;
        kvm.width = 2560;
        kvm.height = 1440;
        kvm.atx = true;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(status.errorCode.isEmpty());
        QCOMPARE(status.signal, std::optional<bool>(true));
        QCOMPARE(status.width, 2560);
        QCOMPARE(status.height, 1440);
        QCOMPARE(status.keyboard, std::optional<bool>(true));
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::optional<bool>(true));
        QCOMPARE(status.temperature, std::optional<double>(42.5));
        QCOMPARE(status.model, u"RM10"_s);
        QCOMPARE(status.version, u"V1.10.1 release2"_s);
        QVERIFY(status.ports.isEmpty());
        QVERIFY(status.lastSeenSecondsAgo.has_value());
        QVERIFY(*status.lastSeenSecondsAgo <= 5);
        QVERIFY(!device.isStale());
        QTRY_VERIFY(device.isStale());

        kvm.signal = false;
        QCOMPARE(lookedAt(device).state, u"no_signal"_s);
    }

    void whatAKvmIsAndThatItHasOneComputerIsAskedRarely()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        for (int look = 0; look < 4; ++look) {
            QCOMPARE(lookedAt(device).model, u"RM10"_s);
        }
        QCOMPARE(kvm.asked("/api/streamer"), 4);
        QCOMPARE(kvm.asked("/api/hid"), 4);
        QCOMPARE(kvm.asked("/api/upgrade/version"), 1);
        QCOMPARE(kvm.asked("/api/switch"), 1);
    }

    void aDetailThatDoesNotComeIsNotKnownAndTheKvmIsStillThere()
    {
        MadeUpKvm kvm;
        kvm.instead.insert("/api/hid"_ba, Answered{500, "oops"_ba});
        kvm.instead.insert("/api/atx"_ba, good({{u"enabled"_s, u"perhaps"_s}}));
        // (One of them breaks off, and after the others have come: nothing that answers after it
        // makes up for it.)
        Answered broken = good({{u"health"_s, QJsonObject{}}});
        broken.shortBy = 50;
        broken.afterMs = 300;
        kvm.instead.insert("/api/info"_ba, broken);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.keyboard.has_value());
        QVERIFY(!status.powerIsKnown);
        QVERIFY(!status.temperature.has_value());
        // And a picture of it is to be had: it does not count as away for that.
        QVERIFY(pictureOf(device).ok());
    }

    void aKvmThatHangsUpOnItsSignInIsLeftAloneForAWhile_data()
    {
        QTest::addColumn<bool>("unreported");
        QTest::newRow("normal notification") << false;
        QTest::newRow("missing notification") << true;
    }

    void aKvmThatHangsUpOnItsSignInIsLeftAloneForAWhile()
    {
        QFETCH(bool, unreported);
        MadeUpKvm kvm;
        Answered gone;
        gone.hangUp = true;
        kvm.instead.insert("/api/auth/login"_ba, gone);
        KvmdDevice::Pace pace = patient();
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.signInRetryMs = 3000; // held well past the pause it is looked at in, on a slow machine too
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        UnreportedSend missing;
        if (unreported) {
            QCoreApplication::instance()->installEventFilter(&missing);
        }
        const core::Status status = lookedAt(device);
        QCoreApplication::instance()->removeEventFilter(&missing);
        if (unreported) {
            qInfo() << "Request-sent observations suppressed:" << missing.forgotten;
        }
        QVERIFY(kvm.asked("/api/auth/login") > 0);
        // It is there (it answers what anybody may ask), and does not sign this program in.
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"no_sign_in"_s);
        QCOMPARE(status.errorMessage,
                 u"The device does not sign qKVM in (The device closed the connection). It is asked again shortly."_s);
        QCOMPARE(kvm.asked("/api/auth/check"), 2); // (asked first, before the sign-in; then whether it is there)
        const qsizetype asked = kvm.stand->asked.size();
        // A card is told at once what is known, and the KVM is not asked meanwhile.
        QElapsedTimer watch;
        watch.start();
        QCOMPARE(pictureOf(device).error().code, std::string("no_sign_in"));
        QCOMPARE(lookedAt(device).state, u"error"_s);
        QVERIFY(watch.elapsed() < pace.holdMs);
        QCOMPARE(kvm.stand->asked.size(), asked);
        // The sign-in went out and was answered by nothing: the password is not sent again at every look
        // (SignInPacing), and what stands is said meanwhile.
        QTest::qWait(pace.holdMs + 100);
        QCOMPARE(lookedAt(device).errorCode, u"no_sign_in"_s);
        QCOMPARE(kvm.stand->asked.size(), asked);
        QTest::qWait(pace.signInRetryMs - pace.holdMs);
        kvm.instead.clear();
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
    }

    void nobodyAtTheAddressIsUnreachable()
    {
        const core::testing::NobodyThere nobody;
        KvmdDevice::Where where;
        where.address = QUrl(nobody.address());
        KvmdDevice device(where, u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"offline"_s);
        QCOMPARE(status.errorCode, u"unreachable"_s);
        QCOMPARE(status.errorMessage, u"Device is unreachable"_s);
        const auto refused = doneBy(device, core::Action::plain(u"atx_power"_s));
        QCOMPARE(refused.error().code, std::string("unreachable"));
        // Nothing went out: that is a refusal, and not "it may have been done".
        QVERIFY(!KvmdDevice::mayHaveBeenDone(refused.error()));
    }

    void aKvmThatLetsNobodyInInTimeIsUnreachable()
    {
        // Something that takes the line and never speaks, where a KVM would agree on how to speak.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
        KvmdDevice::Where where;
        where.address = QUrl(u"https://127.0.0.1:%1"_s.arg(silent.serverPort()));
        KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 500;
        KvmdDevice device(where, u"secret"_s, pace);
        QElapsedTimer watch;
        watch.start();
        const core::Status status = lookedAt(device);
        QCOMPARE(status.errorCode, u"unreachable"_s);
        QVERIFY(watch.elapsed() < pace.connectWithinMs + pace.quietMs);
    }

    void aPictureComesWithWhetherTheComputerSendsOne()
    {
        MadeUpKvm kvm;
        kvm.picture = jpeg(480, 270, 3000);
        KvmdDevice device(kvm.where(), u"secret"_s, waitingForTheFirstLook());
        // Nothing is known of the KVM yet: the picture waits a moment for the first look.
        auto picture = pictureOf(device, 400);
        QVERIFY2(picture.ok(), picture.ok() ? "" : picture.error().message.c_str());
        QCOMPARE(picture.value().picture, kvm.picture);
        QVERIFY(picture.value().signal);
        // Asked small, as the card is, and never without kvmd's own picture in place of none.
        QCOMPARE(kvm.pictureAsks.size(), 1);
        QCOMPARE(kvm.pictureAsks.constLast().queryItemValue(u"allow_offline"_s), u"1"_s);
        QCOMPARE(kvm.pictureAsks.constLast().queryItemValue(u"preview"_s), u"1"_s);
        QCOMPARE(kvm.pictureAsks.constLast().queryItemValue(u"preview_max_width"_s), u"480"_s);

        kvm.signal = false;
        QTest::qWait(250);
        picture = pictureOf(device, 400);
        QVERIFY2(picture.ok(), picture.ok() ? "" : picture.error().message.c_str());
        QVERIFY(!picture.value().signal);
        QCOMPARE(kvm.pictureAsks.size(), 2);
    }

    void whatAPictureSaysStandsInWhereTheStateSaysNothing()
    {
        // One maker's KVM says nothing of its video while that rests ("streamer": null): the
        // first look finds no size, and whether the computer sends a picture is not known. A
        // picture that comes says so, and as captured it says its size: taken from it there and
        // then, and not left for the next look to find.
        MadeUpKvm kvm;
        kvm.restsUntilWatched = true;
        kvm.saysNothingOfAVideoThatRests = true;
        kvm.width = 1280;
        kvm.height = 720;
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 60'000; // (it stays awake, and what a picture said stays what is known)
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.signal.has_value());
        QCOMPARE(status.width, 0);
        QCOMPARE(status.height, 0);
        QCOMPARE(kvm.asked("/api/streamer"), 1);
        QCOMPARE(pictureOf(device).error().code, std::string("no_stream"));
        QTRY_VERIFY(kvm.stand->live.isConnected());
        QTest::qWait(pace.holdMs / 2 + 50);
        const QSignalSpy changed(&device, &KvmdDevice::statusChanged);

        // A card's small picture is one the KVM made anew, at another size than the computer
        // sends: it says whether the computer sends one, and nothing of how large that is.
        kvm.picture = jpeg(480, 270);
        QVERIFY(pictureOf(device, 400).ok());
        QCOMPARE(device.status().signal, std::optional<bool>(true));
        QCOMPARE(device.status().state, u"online"_s);
        QCOMPARE(device.status().width, 0);
        QCOMPARE(changed.count(), 1);
        // The picture as captured says its size too.
        kvm.picture = jpeg(1280, 720);
        QVERIFY(pictureOf(device, 1600).ok());
        QCOMPARE(device.status().width, 1280);
        QCOMPARE(device.status().height, 720);
        QCOMPARE(changed.count(), 2);
        // The same once more says nothing new; and a small one after it takes the size back from nobody.
        QTest::qWait(250);
        QVERIFY(pictureOf(device, 1600).ok());
        kvm.picture = jpeg(480, 270);
        QVERIFY(pictureOf(device, 400).ok());
        QCOMPARE(device.status().width, 1280);
        QCOMPARE(changed.count(), 2);
        // (All of that without another look.)
        QCOMPARE(kvm.asked("/api/streamer"), 1);

        // A picture the KVM sends in the place of one (the computer sends none) says that, and
        // its size is not the computer's.
        kvm.signal = false;
        kvm.picture = jpeg(640, 480);
        QTest::qWait(250);
        QVERIFY(pictureOf(device, 1600).ok());
        QCOMPARE(device.status().signal, std::optional<bool>(false));
        QCOMPARE(device.status().state, u"no_signal"_s);
        QCOMPARE(device.status().width, 1280);
        QCOMPARE(changed.count(), 3);
        kvm.signal = true;

        // Never the other way round: where the KVM's state says something, that is how it is.
        // Its video is awake by now, and the state says so; a picture of another size changes
        // nothing of it.
        kvm.width = 1920;
        kvm.height = 1080;
        status = lookedAt(device);
        QCOMPARE(status.signal, std::optional<bool>(true));
        QCOMPARE(status.state, u"online"_s);
        QCOMPARE(status.width, 1920);
        QCOMPARE(status.height, 1080);
        const qsizetype said = changed.count();
        kvm.picture = jpeg(1280, 720);
        kvm.signal = false;
        QTest::qWait(250);
        QVERIFY(pictureOf(device, 1600).ok());
        QCOMPARE(device.status().width, 1920);
        QCOMPARE(device.status().signal, std::optional<bool>(true));
        QCOMPARE(changed.count(), said);
    }

    void whatAPictureSaidStandsInOnlyWhileThePictureIsNew()
    {
        // A KVM whose state never says anything of its video: what its last picture said goes on
        // standing in at every look, for as long as pictures are asked for. Once nobody has asked
        // for one for as long as a KVM is kept awake, nothing is known of its video again: the
        // computer may have been switched off since.
        MadeUpKvm kvm;
        kvm.instead.insert("/api/streamer"_ba, good({{u"streamer"_s, QJsonValue::Null}}));
        kvm.picture = jpeg(1280, 720);
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 700;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QVERIFY(!lookedAt(device).signal.has_value());
        QVERIFY(pictureOf(device, 1600).ok());
        QCOMPARE(device.status().width, 1280);
        core::Status status = lookedAt(device);
        QCOMPARE(status.signal, std::optional<bool>(true));
        QCOMPARE(status.width, 1280);
        QCOMPARE(status.height, 720);
        QTest::qWait(pace.restAfterMs + 100);
        status = lookedAt(device);
        QVERIFY(!status.signal.has_value());
        QCOMPARE(status.width, 0);
        QCOMPARE(status.height, 0);

        // A picture that comes without kvmd's word on whether the computer sends one says
        // nothing of that: it is not taken for "it does".
        kvm.instead.insert("/api/streamer/snapshot"_ba, Answered(200, jpeg(1024, 768), "image/jpeg"_ba));
        QVERIFY(pictureOf(device, 1600).ok());
        QVERIFY(!device.status().signal.has_value());
        QCOMPARE(device.status().width, 1024);
        kvm.instead.remove("/api/streamer/snapshot"_ba);

        // Nor does what a picture said of one computer stand in for the next one on its screen.
        kvm.ports = {true, true};
        KvmdDevice::Pace patientPace = quick();
        patientPace.restAfterMs = 60'000;
        KvmdDevice withSeveral(kvm.where(), u"secret"_s, patientPace);
        QCOMPARE(lookedAt(withSeveral).ports.size(), 2);
        QVERIFY(pictureOf(withSeveral, 1600).ok());
        QCOMPARE(lookedAt(withSeveral).width, 1280);
        QVERIFY(doneBy(withSeveral, core::Action::show(1)).ok());
        status = lookedAt(withSeveral);
        QCOMPARE(status.shownPort, 1);
        QCOMPARE(status.width, 0);
        QVERIFY(!status.signal.has_value());
    }

    void theSizeAPictureSaidStandsInOnlyWhileAPictureAsCapturedIsNew()
    {
        // A KVM whose state never says anything of its video. One picture as it captured it said
        // how large the computer's picture is; after that only a card's small pictures come,
        // which say whether the computer sends one and nothing of its size. They do not keep
        // that size new: the computer may send another by now.
        MadeUpKvm kvm;
        kvm.instead.insert("/api/streamer"_ba, good({{u"streamer"_s, QJsonValue::Null}}));
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 900;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QVERIFY(!lookedAt(device).signal.has_value());
        kvm.picture = jpeg(1280, 720);
        QVERIFY(pictureOf(device, 1600).ok());
        QElapsedTimer sinceCaptured;
        sinceCaptured.start();
        QCOMPARE(device.status().width, 1280);
        QCOMPARE(device.status().signal, std::optional<bool>(true));
        // Small pictures, one every quarter of a second, for longer than a picture's word counts.
        kvm.picture = jpeg(480, 270);
        qsizetype small = 0;
        while (sinceCaptured.elapsed() < pace.restAfterMs + 300) {
            QTest::qWait(250);
            QVERIFY(pictureOf(device, 400).ok());
            ++small;
        }
        QCOMPARE(kvm.pictureAsks.size(), 1 + small);
        QVERIFY(kvm.pictureAsks.constLast().hasQueryItem(u"preview"_s));
        // Whether the computer sends a picture is as new as the last of them. How large that is,
        // nothing has said for too long.
        const core::Status status = lookedAt(device);
        QCOMPARE(status.signal, std::optional<bool>(true));
        QCOMPARE(status.state, u"online"_s);
        QCOMPARE(status.width, 0);
        QCOMPARE(status.height, 0);
        // A picture as captured says it anew.
        kvm.picture = jpeg(1024, 768);
        QTest::qWait(250);
        QVERIFY(pictureOf(device, 1600).ok());
        QCOMPARE(device.status().width, 1024);
        QCOMPARE(device.status().height, 768);
    }

    void whatWasSeenIsDroppedWhenTheKvmPutsAnotherComputerOnItsScreenByItself()
    {
        // Somebody presses the button on the KVM: this driver did not switch it, and notices at
        // its next look. What the last picture said, and the picture itself, were the other
        // computer's.
        MadeUpKvm kvm;
        kvm.instead.insert("/api/streamer"_ba, good({{u"streamer"_s, QJsonValue::Null}}));
        kvm.ports = {true, true};
        kvm.picture = jpeg(1280, 720);
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 60'000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).shownPort, 0);
        QVERIFY(pictureOf(device, 1600).ok());
        core::Status status = lookedAt(device);
        QCOMPARE(status.width, 1280);
        QCOMPARE(status.signal, std::optional<bool>(true));
        const qsizetype pictures = kvm.pictureAsks.size();

        kvm.shown = 1;
        status = lookedAt(device);
        QCOMPARE(status.shownPort, 1);
        QCOMPARE(status.width, 0);
        QCOMPARE(status.height, 0);
        QVERIFY(!status.signal.has_value());
        QCOMPARE(kvm.asked("/api/switch/set_active"), 0);
        // Nor is the picture of before handed out, however soon after it one is asked for.
        kvm.picture = jpeg(1024, 768);
        const auto picture = pictureOf(device, 1600);
        QVERIFY(picture.ok());
        QCOMPARE(picture.value().picture, kvm.picture);
        QCOMPARE(kvm.pictureAsks.size(), pictures + 1);
        QCOMPARE(device.status().width, 1024);

        // A switch this driver makes itself drops what was seen there and then, and the look
        // that follows finds the computer it knows of: what a picture has said of that one
        // meanwhile stays.
        QVERIFY(doneBy(device, core::Action::show(0)).ok());
        kvm.picture = jpeg(800, 600);
        QVERIFY(pictureOf(device, 1600).ok());
        QCOMPARE(device.status().width, 800);
        status = lookedAt(device);
        QCOMPARE(status.shownPort, 0);
        QCOMPARE(status.width, 800);
        QCOMPARE(status.signal, std::optional<bool>(true));
    }

    void aWidePictureIsAskedAsCaptured()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, waitingForTheFirstLook());
        QVERIFY(pictureOf(device, 1200).ok());
        QVERIFY(!kvm.pictureAsks.constLast().hasQueryItem(u"preview"_s));
        // And where the computer's own picture is no wider than the small size would be.
        kvm.width = 640;
        kvm.height = 480;
        QCOMPARE(lookedAt(device).width, 640);
        QTest::qWait(250); // (so that it is asked anew, and not the picture of a moment ago)
        QVERIFY(pictureOf(device, 600).ok());
        QCOMPARE(kvm.pictureAsks.size(), 2);
        QVERIFY(!kvm.pictureAsks.constLast().hasQueryItem(u"preview"_s));
        QVERIFY(pictureOf(device, 300).ok());
        QCOMPARE(kvm.pictureAsks.size(), 3);
        QCOMPARE(kvm.pictureAsks.constLast().queryItemValue(u"preview_max_width"_s), u"480"_s);
    }

    void whatIsNoPictureIsNotHandedOn_data()
    {
        QTest::addColumn<QByteArray>("sent");
        QTest::addColumn<QByteArray>("kind");
        QTest::addColumn<QString>("sentence");
        const QString none = u"The device sent something that is not a picture"_s;
        QTest::newRow("a page called a picture") << "<html><body>sign in</body></html>"_ba << "image/jpeg"_ba << none;
        QTest::newRow("a picture called a page") << jpeg(640, 360) << "text/html"_ba << none;
        QTest::newRow("a picture called a drawing") << jpeg(640, 360) << "image/svg+xml"_ba << none;
        QTest::newRow("nothing") << QByteArray(" ") << "image/jpeg"_ba << none;
        QTest::newRow("a few bytes that say they are enormous")
            << jpeg(60000, 60000) << "image/jpeg"_ba
            << u"The device sent a picture of 60000 by 60000 pixels, which is not shown"_s;
    }
    void whatIsNoPictureIsNotHandedOn()
    {
        QFETCH(QByteArray, sent);
        QFETCH(QByteArray, kind);
        QFETCH(QString, sentence);
        MadeUpKvm kvm;
        kvm.picture = sent;
        kvm.pictureKind = kind;
        // (Its first contact, the question asked before the sign-in among it, given time on a busy PC: this is about
        // what is no picture, not about how soon one comes.)
        KvmdDevice::Pace pace = quick();
        pace.firstContactMs = waitingForTheFirstLook().firstContactMs;
        pace.patienceMs = waitingForTheFirstLook().patienceMs;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const auto picture = pictureOf(device);
        QVERIFY(!picture.ok());
        QCOMPARE(picture.error().code, std::string("bad_response"));
        QCOMPARE(QString::fromStdString(picture.error().message), sentence);
        // The KVM is there all the same.
        QCOMPARE(device.status().state, u"online"_s);
    }

    void aPictureLargerThanIsTakenIsCutOff()
    {
        MadeUpKvm kvm;
        kvm.picture = jpeg(1920, 1080, core::kvmd::largestPictureBytes + 1000);
        KvmdDevice::Pace pace = patient();
        pace.firstContactMs = waitingForTheFirstLook().firstContactMs; // (as above)
        pace.patienceMs = waitingForTheFirstLook().patienceMs;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const auto picture = pictureOf(device);
        QVERIFY(!picture.ok());
        QCOMPARE(picture.error().code, std::string("dropped"));
        QCOMPARE(picture.error().message, std::string("The device sent far more data than this program accepts"));
    }

    void anAnswerLargerThanAKvmHasReasonToSendIsCutOff()
    {
        MadeUpKvm kvm;
        kvm.instead.insert("/api/streamer"_ba, good({{u"x"_s, QString(core::kvmd::largestReplyBytes + 1000, u'x')}}));
        KvmdDevice device(kvm.where(), u"secret"_s, patient());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"offline"_s);
        QCOMPARE(status.errorCode, u"dropped"_s);
    }

    void aKvmThatAnswersAByteAtATimeIsGivenUpOn()
    {
        // Something always comes, so nothing is ever quiet for too long: and still a KVM may not
        // keep a question open for as long as it likes.
        MadeUpKvm kvm;
        Answered dripping = good({{u"x"_s, QString(100'000, u'x')}});
        dripping.byteEveryMs = 100;
        kvm.instead.insert("/api/streamer"_ba, dripping);
        KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 500;
        pace.quietMs = 800;
        pace.transferMs = 800;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QElapsedTimer watch;
        watch.start();
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"offline"_s);
        QCOMPARE(status.errorCode, u"timeout"_s);
        QVERIFY(watch.elapsed() < 6000);
        // And the look after it is a look: nothing is left waiting for the one that was given up.
        kvm.instead.clear();
        QTest::qWait(pace.holdMs + 100);
        QCOMPARE(lookedAt(device).state, u"online"_s);
    }

    void anAnswerThatBreaksOffIsNoAnswer()
    {
        MadeUpKvm kvm;
        kvm.atx = true;
        KvmdDevice device(kvm.where(), u"secret"_s, patient());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        // Half a picture is not handed on as a picture.
        Answered half(200, jpeg(1920, 1080, 20'000), "image/jpeg"_ba);
        half.shortBy = 5000;
        kvm.instead.insert("/api/streamer/snapshot"_ba, half);
        const auto picture = pictureOf(device);
        QVERIFY(!picture.ok());
        QCOMPARE(picture.error().code, std::string("dropped"));
        // And an answer to something done that breaks off says nothing of whether it was done.
        Answered cut = good();
        cut.shortBy = 50;
        kvm.instead.insert("/api/atx/click"_ba, cut);
        const auto unknown = doneBy(device, core::Action::plain(u"atx_power"_s));
        QVERIFY(!unknown.ok());
        QCOMPARE(unknown.error().code, std::string("dropped"));
        QVERIFY(KvmdDevice::mayHaveBeenDone(unknown.error()));
        QCOMPARE(kvm.asked("/api/atx/click"), 1);
    }

    void theLineThatKeepsItAwakeGoesOnlyToTheKvmItself()
    {
        // The pass goes out with the asking for that line. So the asking does not go out to
        // whatever shows another certificate than the KVM is known by, also where Windows
        // would trust that certificate.
        class Open : public KvmdDevice {
        public:
            using KvmdDevice::keepAwake;
            using KvmdDevice::KvmdDevice;
        };
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        KvmdDevice::Where where = kvm->where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        Open device(where, u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok()); // signed in: it has a pass, and somebody is looking
        // To the KVM itself the line is made.
        device.keepAwake();
        QTRY_VERIFY(kvm->stand->live.isConnected());
        QCOMPARE(kvm->stand->live.headers.value("cookie"), "auth_token=pass1"_ba);

        // Something else answers at its address, with a certificate that Windows trusts.
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        StandIn other([](const Asked&) { return Answered{200, "{}"_ba}; }, tls("signed"), port);
        QVERIFY(other.isListening());
        Open again(where, u"secret"_s, quick());
        QCOMPARE(lookedAt(again).errorCode, u"cert_mismatch"_s);
        again.keepAwake(); // (it has no pass, and asks for nothing)
        QTest::qWait(500);
        QCOMPARE(other.live.lines, 0);
        // And the one that has a pass: its line had ended with the KVM, and is asked for anew.
        QTRY_VERIFY(!device.isKeptAwake());
        device.keepAwake();
        QTest::qWait(1000);
        QCOMPARE(other.live.lines, 0);
        QVERIFY(other.asked.isEmpty());
        QVERIFY(!device.isKeptAwake());
    }

    void aLineUnderWhichTheKvmRestsAllTheSameIsMadeAnew()
    {
        // The KVM lost its power, or a router forgot the line: nothing said so, the line looks
        // as it did, and the KVM that came back rests. The line is given up and asked for anew.
        MadeUpKvm kvm;
        kvm.restsUntilWatched = true;
        KvmdDevice::Pace pace = quick();
        pace.quietMs = 700;
        pace.restAfterMs = 20'000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QVERIFY(!pictureOf(device).ok());
        QTRY_VERIFY(kvm.stand->live.isConnected());
        QCOMPARE(kvm.stand->live.lines, 1);
        kvm.restsThoughWatched = true;
        // Not at once: a KVM takes a moment to wake under a line that is new.
        QTest::qWait(pace.holdMs / 2 + 50);
        QCOMPARE(pictureOf(device).error().code, std::string("no_stream"));
        QTest::qWait(200); // (a second line would have arrived by now)
        QCOMPARE(kvm.stand->live.lines, 1);
        QTest::qWait(pace.quietMs);
        QCOMPARE(pictureOf(device).error().code, std::string("no_stream"));
        QTRY_COMPARE(kvm.stand->live.lines, 2);
        kvm.restsThoughWatched = false;
        QTest::qWait(pace.holdMs / 2 + 50);
        QVERIFY(pictureOf(device).ok());
    }

    void aKvmThatAsksForAWindowsSignOnGetsNone()
    {
        // Asked to sign on in some ways, Qt would answer by itself with the account of whoever
        // is signed in to Windows, and send the question again, what it carries included.
        MadeUpKvm kvm;
        kvm.atx = true;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        for (const QByteArray& way : {"NTLM"_ba, "Negotiate"_ba, "Basic realm=kvm"_ba}) {
            Answered asks = refusal(401, u"UnauthorizedError"_s);
            asks.more = {"WWW-Authenticate: " + way};
            kvm.instead.insert("/api/atx/click"_ba, asks);
            const int before = kvm.asked("/api/atx/click");
            const auto refused = doneBy(device, core::Action::plain(u"atx_power"_s));
            QCOMPARE(refused.error().code, std::string("auth_failed"));
            QVERIFY(!KvmdDevice::mayHaveBeenDone(refused.error()));
            // Asked and refused, signed in anew, asked once more and refused: the driver's own
            // two, and nothing of Qt's after them.
            QCOMPARE(kvm.asked("/api/atx/click"), before + 2);
            QTest::qWait(quick().signInRetryMs + 100);
        }
        // The sign-in itself answered so is a refused password, and is not answered either.
        kvm.instead.clear();
        Answered asks = refusal(401, u"UnauthorizedError"_s);
        asks.more = {"WWW-Authenticate: NTLM"_ba};
        kvm.instead.insert("/api/auth/login"_ba, asks);
        kvm.passes.clear();
        const int signInsAsked = kvm.asked("/api/auth/login");
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(kvm.asked("/api/auth/login"), signInsAsked + 1);
        for (const Asked& one : std::as_const(kvm.stand->asked)) {
            QVERIFY(!one.headers.contains("authorization"));
        }
    }

    void theLineThatKeepsItAwakeSignsOnToNothingEither()
    {
        MadeUpKvm kvm;
        kvm.restsUntilWatched = true;
        kvm.stand->handsLinesOver = false; // the asking for the line is answered as a question
        kvm.stand->keepsLines = true;      // and the line is kept: a second asking would arrive
        Answered asks = refusal(401, u"UnauthorizedError"_s);
        asks.more = {"WWW-Authenticate: NTLM"_ba};
        kvm.instead.insert("/api/ws"_ba, asks);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QVERIFY(!pictureOf(device).ok());
        QTRY_VERIFY(kvm.asked("/api/ws") >= 1);
        QTest::qWait(700);
        // (Left to itself, Qt asks for the line again at once, with the beginning of a Windows
        // sign-on in it, which names this PC, and goes on asking for as long as it is answered so.)
        QCOMPARE(kvm.asked("/api/ws"), 1);
        for (const Asked& one : std::as_const(kvm.stand->asked)) {
            QVERIFY(!one.headers.contains("authorization"));
        }
        QTRY_VERIFY(!device.isKeptAwake());
    }

    void anAskingForTheLineThatIsNeverAnsweredIsGivenUp()
    {
        MadeUpKvm kvm;
        kvm.restsUntilWatched = true;
        kvm.stand->handsLinesOver = false;
        Answered silent;
        silent.never = true;
        kvm.instead.insert("/api/ws"_ba, silent);
        KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 500;
        pace.restAfterMs = 60'000; // (not let go of because nobody looks: given up because nobody answers)
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QVERIFY(!pictureOf(device).ok());
        QTRY_VERIFY(kvm.asked("/api/ws") == 1);
        QVERIFY(device.isKeptAwake());
        QTRY_VERIFY_WITH_TIMEOUT(!device.isKeptAwake(), 3000);
    }

    void anErrorInTheMiddleOfItLeavesOpenWhetherItWasDone()
    {
        // An error of the KVM's own, or of what stands before it: a long text may be typed on
        // while the answer to it was given up. That is no "no", and nothing is sent again for it.
        MadeUpKvm kvm;
        kvm.atx = true;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        for (const int status : {500, 502, 504}) {
            kvm.instead.insert("/api/hid/print"_ba, Answered{status, "<html>gateway</html>"_ba, "text/html"_ba});
            const auto unknown = doneBy(device, core::Action::type(u"a long text"_s, false));
            QVERIFY(!unknown.ok());
            QCOMPARE(unknown.error().code, std::string("action_unknown"));
            QVERIFY(KvmdDevice::mayHaveBeenDone(unknown.error()));
        }
        QCOMPARE(kvm.asked("/api/hid/print"), 3);
        // What the KVM says no to is a no.
        kvm.instead.insert("/api/hid/print"_ba, refusal(409, u"IsBusyError"_s, u"Busy"_s));
        const auto refused = doneBy(device, core::Action::type(u"a long text"_s, false));
        QCOMPARE(refused.error().code, std::string("action_failed"));
        QVERIFY(!KvmdDevice::mayHaveBeenDone(refused.error()));
    }

    void aButtonIsNotPressedTwiceOnAKeptLineEither()
    {
        // Over TLS and on a line that was there already, which is how most questions go.
        MadeUpKvm kvm(tls("first"));
        kvm.stand->keepsLines = true;
        kvm.atx = true;
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        KvmdDevice device(where, u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(doneBy(device, core::Action::plain(u"atx_power"_s)).ok());
        const int lines = kvm.stand->lines;
        Answered gone;
        gone.hangUp = true;
        kvm.instead.insert("/api/atx/click"_ba, gone);
        kvm.instead.insert("/api/hid/print"_ba, gone);
        auto unknown = doneBy(device, core::Action::plain(u"atx_reset"_s));
        QCOMPARE(unknown.error().code, std::string("dropped"));
        QVERIFY(KvmdDevice::mayHaveBeenDone(unknown.error()));
        QCOMPARE(kvm.asked("/api/atx/click"), 2);
        unknown = doneBy(device, core::Action::type(u"text"_s, true));
        QCOMPARE(unknown.error().code, std::string("dropped"));
        QVERIFY(KvmdDevice::mayHaveBeenDone(unknown.error()));
        QCOMPARE(kvm.asked("/api/hid/print"), 1);
        QVERIFY(kvm.stand->lines <= lines + 2);
    }

    void aRestingKvmIsWokenAndLetRestAgain()
    {
        MadeUpKvm kvm;
        kvm.restsUntilWatched = true;
        KvmdDevice device(kvm.where(), u"secret"_s, waitingForTheFirstLook());
        const auto resting = pictureOf(device);
        QVERIFY(!resting.ok());
        QCOMPARE(resting.error().code, std::string("no_stream"));
        QCOMPARE(resting.error().message, std::string("Video stream is idle - waking it up"));
        // It is watched as a browser would watch it, with the pass handed over both ways: it is GL.iNet's (its
        // answer to /api/upgrade/version, at the first look, said so), whose own page does so.
        QTRY_VERIFY(kvm.stand->live.isConnected());
        QCOMPARE(kvm.stand->live.path, "/api/ws?stream=1&auth_token=pass1"_ba);
        QCOMPARE(kvm.stand->live.headers.value("cookie"), "auth_token=pass1"_ba);
        QTest::qWait(quick().holdMs / 2 + 50);
        QVERIFY(pictureOf(device).ok());
        QCOMPARE(kvm.stand->live.lines, 1);
        // Nobody asks for a picture any more: it is not kept awake for nobody.
        QTRY_VERIFY_WITH_TIMEOUT(!kvm.stand->live.isConnected(), 5000);
    }

    void aSlowKvmDoesNotHoldACardUp()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        kvm.pictureAfterMs = quick().patienceMs + 300;
        QElapsedTimer watch;
        watch.start();
        const auto slow = pictureOf(device);
        QVERIFY(!slow.ok());
        QCOMPARE(slow.error().code, std::string("no_stream"));
        QCOMPARE(slow.error().message, std::string("The KVM is slow to answer"));
        QVERIFY(watch.elapsed() < quick().patienceMs + 250);
        // The question to the KVM carried on by itself, and what it brought goes to whoever asks
        // next: the KVM is not asked a second time for it.
        QTest::qWait(500);
        kvm.pictureAfterMs = 0;
        QVERIFY(pictureOf(device).ok());
        QCOMPARE(kvm.pictureAsks.size(), 1);
        // And is new only once.
        QTest::qWait(250);
        QVERIFY(pictureOf(device).ok());
        QCOMPARE(kvm.pictureAsks.size(), 2);
    }

    void aNewPictureIsNeverTheOneSomebodyHadBeforeAndJoinsOneThatIsUnderWay()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        // The picture that came, or why none did.
        const auto bytesOf = [](const base::Result<core::Snapshot>& came) {
            return came ? came.value().picture : QByteArray::fromStdString(came.error().code);
        };
        const auto newPictureOf = [&device] {
            return answerTo<core::Snapshot>([&](auto done) { device.askNewSnapshot(1600, std::move(done)); });
        };
        // A card's asking that follows closely on another is answered with the picture of that
        // one, and the KVM is not asked.
        kvm.picture = jpeg(1920, 1080, 100);
        QCOMPARE(bytesOf(pictureOf(device, 1600)), jpeg(1920, 1080, 100));
        kvm.picture = jpeg(1920, 1080, 200);
        QCOMPARE(bytesOf(pictureOf(device, 1600)), jpeg(1920, 1080, 100));
        QCOMPARE(kvm.pictureAsks.size(), 1);
        // Whoever asks for a new one is not handed that: the KVM is asked, each time.
        QCOMPARE(bytesOf(newPictureOf()), jpeg(1920, 1080, 200));
        kvm.picture = jpeg(1920, 1080, 300);
        QCOMPARE(bytesOf(newPictureOf()), jpeg(1920, 1080, 300));
        QCOMPARE(kvm.pictureAsks.size(), 3);
        QVERIFY(!kvm.pictureAsks.constLast().hasQueryItem(u"preview"_s));
        // And a card that asks closely behind is answered with what that brought.
        QCOMPARE(bytesOf(pictureOf(device, 1600)), jpeg(1920, 1080, 300));
        QCOMPARE(kvm.pictureAsks.size(), 3);

        // One that is under way is joined: the KVM, which takes a while over each now, is not
        // asked a second time at once, and both are handed what it brings.
        kvm.pictureAfterMs = 200;
        kvm.picture = jpeg(1920, 1080, 400);
        QTest::qWait(250);
        QList<base::Result<core::Snapshot>> card;
        QList<base::Result<core::Snapshot>> live;
        device.askSnapshot(1600, [&card](const base::Result<core::Snapshot>& came) { card.append(came); });
        QTRY_COMPARE(kvm.pictureAsks.size(), 4);
        device.askNewSnapshot(1600, [&live](const base::Result<core::Snapshot>& came) { live.append(came); });
        QTRY_VERIFY(card.size() == 1 && live.size() == 1);
        QCOMPARE(bytesOf(card.constFirst()), jpeg(1920, 1080, 400));
        QCOMPARE(bytesOf(live.constFirst()), jpeg(1920, 1080, 400));
        QTest::qWait(100);
        QCOMPARE(kvm.pictureAsks.size(), 4);
        QCOMPARE(kvm.mostPicturesAtOnce, 1);
        // (The made-up KVM does see two at once where there are two: pictures of two sizes are
        // two questions.)
        card.clear();
        live.clear();
        QTest::qWait(250);
        device.askSnapshot(400, [&card](const base::Result<core::Snapshot>& came) { card.append(came); });
        device.askNewSnapshot(1600, [&live](const base::Result<core::Snapshot>& came) { live.append(came); });
        QTRY_VERIFY(card.size() == 1 && live.size() == 1);
        QCOMPARE(kvm.pictureAsks.size(), 6);
        QCOMPARE(kvm.mostPicturesAtOnce, 2);
    }

    void whoeverAsksForANewPictureIsHandedOneNobodyHadOnlyWhileItHasJustCome()
    {
        // A KVM that takes longer over a picture as captured than whoever asks waits for it.
        MadeUpKvm kvm;
        KvmdDevice::Pace pace = quick();
        pace.freshMs = 1000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        const auto bytesOf = [](const base::Result<core::Snapshot>& came) {
            return came ? came.value().picture : QByteArray::fromStdString(came.error().code);
        };
        const auto newPictureOf = [&device] {
            return answerTo<core::Snapshot>([&](auto done) { device.askNewSnapshot(1600, std::move(done)); });
        };
        const int late = pace.patienceMs + 200;
        kvm.picture = jpeg(1920, 1080, 100);
        kvm.pictureAfterMs = late;
        QCOMPARE(bytesOf(newPictureOf()), "no_stream"_ba);
        // It lands a moment later. Asked for anew then, it is what is handed over: the newest
        // there is of a KVM that is slow, and the KVM is not asked a second time for it.
        QTest::qWait(late - pace.patienceMs + 100);
        kvm.picture = jpeg(1920, 1080, 200);
        kvm.pictureAfterMs = 0;
        QCOMPARE(bytesOf(newPictureOf()), jpeg(1920, 1080, 100));
        QCOMPARE(kvm.pictureAsks.size(), 1);
        // Again one that lands after whoever asked had given up, and nobody asks for a while:
        // longer than a picture that came says how the KVM is.
        kvm.picture = jpeg(1920, 1080, 300);
        kvm.pictureAfterMs = late;
        QCOMPARE(bytesOf(newPictureOf()), "no_stream"_ba);
        QCOMPARE(kvm.pictureAsks.size(), 2);
        QTest::qWait(late - pace.patienceMs + pace.freshMs + 300);
        // Whoever asks for a new one now is not handed that as new: it is thrown away, and the
        // KVM is asked.
        kvm.picture = jpeg(1920, 1080, 400);
        kvm.pictureAfterMs = 0;
        QCOMPARE(bytesOf(newPictureOf()), jpeg(1920, 1080, 400));
        QCOMPARE(kvm.pictureAsks.size(), 3);
    }

    void aPictureWaitsOnlyAMomentForAKvmNothingIsKnownOf()
    {
        MadeUpKvm kvm;
        Answered late = good();
        late.afterMs = quick().firstContactMs + 400;
        kvm.first["/api/auth/login"_ba].append(late);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QElapsedTimer watch;
        watch.start();
        const auto picture = pictureOf(device);
        QVERIFY(!picture.ok());
        QCOMPARE(picture.error().code, std::string("no_stream"));
        QCOMPARE(picture.error().message, std::string("Connecting to the KVM"));
        QVERIFY(watch.elapsed() < quick().firstContactMs + 300);
    }

    void somethingIsDoneToTheComputer()
    {
        MadeUpKvm kvm;
        kvm.atx = true;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QVERIFY(doneBy(device, core::Action::plain(u"atx_power"_s)).ok());
        QVERIFY(doneBy(device, core::Action::plain(u"atx_reset"_s)).ok());
        QVERIFY(doneBy(device, core::Action::press(u"ControlLeft,AltLeft,Delete"_s)).ok());
        QVERIFY(doneBy(device, core::Action::wake(u"AA:BB:CC:DD:EE:FF"_s)).ok());
        QVERIFY(doneBy(device, core::Action::plain(u"jiggler_on"_s)).ok());
        QVERIFY(doneBy(device, core::Action::plain(u"reboot"_s)).ok());
        const QList<QByteArray> all{
            "atx power"_ba, "atx reset"_ba, "keys ControlLeft,AltLeft,Delete"_ba, "wake AA:BB:CC:DD:EE:FF"_ba,
            "jiggler 1"_ba, "reboot"_ba};
        QCOMPARE(kvm.did, all);
        QCOMPARE(kvm.last("/api/atx/click")->method, "POST"_ba);
        QCOMPARE(kvm.last("/api/atx/click")->headers.value("cookie"), "auth_token=pass1"_ba);
    }

    void aTextIsTypedOnceAndAsItIs()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const QString text = u"ls -l | grep 'a&b=c+d' "_s + sign(0xE4) + sign(0x1F600);
        QVERIFY(doneBy(device, core::Action::type(text, true)).ok());
        QCOMPARE(kvm.did.size(), 1);
        QCOMPARE(kvm.did.constFirst(), "type " + text.toUtf8() + "\n");
        QCOMPARE(kvm.last("/api/hid/print")->path, "/api/hid/print?limit=0"_ba);
    }

    void whatAKvmWillNotDoIsSaidInItsWords()
    {
        MadeUpKvm kvm; // without a power board
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const auto refused = doneBy(device, core::Action::plain(u"atx_power"_s));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("action_failed"));
        QCOMPARE(refused.error().message, std::string("ATX is not available"));
        QVERIFY(!KvmdDevice::mayHaveBeenDone(refused.error()));
        // And what is none of a KVM's is not sent at all.
        const qsizetype asked = kvm.stand->asked.size();
        QCOMPARE(doneBy(device, core::Action::plain(u"power_off"_s)).error().code, std::string("invalid"));
        QCOMPARE(doneBy(device, core::Action::show(0)).error().code, std::string("invalid"));
        QCOMPARE(kvm.stand->asked.size(), asked);
    }

    void whatGotNoAnswerMayHaveBeenDoneAndIsNotSentAgain()
    {
        MadeUpKvm kvm;
        kvm.atx = true;
        const KvmdDevice::Pace pace = quick();
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        // The line falls away under it.
        Answered gone;
        gone.hangUp = true;
        kvm.instead.insert("/api/atx/click"_ba, gone);
        auto unknown = doneBy(device, core::Action::plain(u"atx_power"_s));
        QElapsedTimer since;
        since.start();
        QVERIFY(!unknown.ok());
        QCOMPARE(unknown.error().code, std::string("dropped"));
        QVERIFY(KvmdDevice::mayHaveBeenDone(unknown.error()));
        QCOMPARE(kvm.asked("/api/atx/click"), 1);
        // A power button pressed twice undoes the first press, and the board may take that one as late as now: a
        // press straight after it is refused, and the KVM is not asked (the web's BOARD_ANSWER, rule 23; this test
        // pressed again at once before). Also by a driver made anew for the same KVM.
        Answered silent;
        silent.never = true;
        kvm.instead.insert("/api/atx/click"_ba, silent);
        unknown = doneBy(device, core::Action::plain(u"atx_power"_s));
        QCOMPARE(unknown.error().code, std::string("busy"));
        QVERIFY(!KvmdDevice::mayHaveBeenDone(unknown.error()));
        KvmdDevice anew(kvm.where(), u"secret"_s, pace);
        QCOMPARE(doneBy(anew, core::Action::plain(u"atx_reset"_s)).error().code, std::string("busy"));
        QCOMPARE(kvm.asked("/api/atx/click"), 1);

        // After the hold, the KVM takes it and never answers.
        QTest::qWait(
            static_cast<int>(std::max<qint64>(0, pace.boardAnswerMs + pace.pressHeldMs + 100 - since.elapsed())));
        unknown = doneBy(device, core::Action::plain(u"atx_power"_s));
        QCOMPARE(unknown.error().code, std::string("timeout"));
        QCOMPARE(
            unknown.error().message,
            std::string("The KVM did not say whether its board pressed the button. It may have: look at the machine "
                        "before pressing again."));
        QVERIFY(KvmdDevice::mayHaveBeenDone(unknown.error()));
        QCOMPARE(kvm.asked("/api/atx/click"), 2);
        QCOMPARE(doneBy(device, core::Action::plain(u"atx_power"_s)).error().code, std::string("busy"));
        QCOMPARE(kvm.asked("/api/atx/click"), 2);
    }

    void anActionRefusedForItsSessionIsSentOnceMoreSignedIn()
    {
        MadeUpKvm kvm;
        kvm.atx = true;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        kvm.passes.clear();
        // Refused is not done: it is safe to send again, and is sent once.
        QVERIFY(doneBy(device, core::Action::plain(u"atx_power"_s)).ok());
        QCOMPARE(kvm.did, QList<QByteArray>{"atx power"_ba});
        QCOMPARE(kvm.signIns, 2);
    }

    void aQuestionThatChangesNothingIsAskedAgainWhenTheLineFell()
    {
        // Usually a line the KVM closed while it sat unused. Qt asks again by itself, a few
        // times, and only what changes nothing: the driver adds no asking of its own to that.
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, patient());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        Answered gone;
        gone.hangUp = true;
        kvm.first["/api/streamer"_ba].append(gone);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.asked("/api/streamer"), 3);
        // And not for ever.
        for (int times = 0; times < 20; ++times) {
            kvm.first["/api/streamer"_ba].append(gone);
        }
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"offline"_s);
        QCOMPARE(status.errorCode, u"dropped"_s);
        QVERIFY(kvm.asked("/api/streamer") <= 3 + 6);
        // A KVM that failed a look is left alone for a while, whatever asks: a card is told at once.
        const qsizetype asked = kvm.stand->asked.size();
        QCOMPARE(pictureOf(device).error().code, std::string("dropped"));
        QCOMPARE(lookedAt(device).state, u"offline"_s);
        QCOMPARE(kvm.stand->asked.size(), asked);
    }

    void aKvmThatNeverAnswersItsOwnRestartHasRestarted()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        Answered gone;
        gone.hangUp = true;
        kvm.instead.insert("/api/upgrade/reboot"_ba, gone);
        QVERIFY(doneBy(device, core::Action::plain(u"reboot"_s)).ok());
    }

    void anotherComputerIsPutOnItsScreen()
    {
        MadeUpKvm kvm;
        kvm.ports = {true, true, false};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        core::Status status = lookedAt(device);
        QCOMPARE(status.ports.size(), 3);
        QCOMPARE(status.shownPort, 0);
        QVERIFY(status.ports.at(2).empty);
        QVERIFY(pictureOf(device).ok());
        const qsizetype pictures = kvm.pictureAsks.size();

        QVERIFY(doneBy(device, core::Action::show(1)).ok());
        QCOMPARE(kvm.did, QList<QByteArray>{"port 1"_ba});
        // Said at once, and looked at anew.
        QCOMPARE(device.status().shownPort, 1);
        QVERIFY(device.isStale());
        // What was on its screen belongs to the other computer: no picture of before is handed out.
        QVERIFY(pictureOf(device).ok());
        QCOMPARE(kvm.pictureAsks.size(), pictures + 1);
        // One it has not is not asked of it.
        QCOMPARE(doneBy(device, core::Action::show(3)).error().code, std::string("invalid"));
        QCOMPARE(kvm.asked("/api/switch/set_active"), 1);
    }

    void afterAnotherComputerTheKeyboardIsNotKnownUntilItIsFound()
    {
        MadeUpKvm kvm;
        kvm.ports = {true, true};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).keyboard, std::optional<bool>(true));
        // As a real one: after the switch it says "no keyboard" until a key has been sent.
        QVERIFY(doneBy(device, core::Action::show(1)).ok());
        kvm.keyboard = false;
        QVERIFY(!lookedAt(device).keyboard.has_value());
        kvm.keyboard = true;
        QCOMPARE(lookedAt(device).keyboard, std::optional<bool>(true));
        // And a cable that is out later is that.
        kvm.keyboard = false;
        QCOMPARE(lookedAt(device).keyboard, std::optional<bool>(false));
    }

    void theComputersAKvmCanWake()
    {
        MadeUpKvm kvm;
        kvm.toWake = {QJsonObject{{u"mac"_s, u"AA:BB:CC:DD:EE:01"_s}, {u"name"_s, u"build server"_s}}};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        auto targets = answerTo<QList<core::WakeTarget>>([&](auto done) { device.askWakeTargets(std::move(done)); });
        QVERIFY(targets.ok());
        QCOMPARE(targets.value().size(), 1);
        QCOMPARE(targets.value().constFirst().name, u"build server"_s);
        // A KVM of the maker that keeps no such list has none.
        kvm.instead.insert("/api/wol/list"_ba, refusal(404, u"NotFoundError"_s));
        targets = answerTo<QList<core::WakeTarget>>([&](auto done) { device.askWakeTargets(std::move(done)); });
        QVERIFY(targets.ok());
        QVERIFY(targets.value().isEmpty());
    }

    void whoAKvmSaysItIs()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const auto who = answerTo<KvmdDevice::Identity>([&](auto done) { device.identify(std::move(done)); });
        QVERIFY(who.ok());
        QCOMPARE(who.value().model, u"RM10"_s);
        QCOMPARE(who.value().version, u"V1.10.1 release2"_s);
        QCOMPARE(who.value().hostname, u"glkvm"_s);

        // The other maker's has neither of the two answers, and is still a KVM.
        kvm.instead.insert("/api/upgrade/version"_ba, refusal(404, u"NotFoundError"_s));
        kvm.instead.insert("/api/system/get_hostname"_ba, refusal(404, u"NotFoundError"_s));
        const auto other = answerTo<KvmdDevice::Identity>([&](auto done) { device.identify(std::move(done)); });
        QVERIFY(other.ok());
        QVERIFY(other.value().model.isEmpty());
        QVERIFY(other.value().hostname.isEmpty());

        KvmdDevice wrong(kvm.where(), u"not it"_s, quick());
        const auto refused = answerTo<KvmdDevice::Identity>([&](auto done) { wrong.identify(std::move(done)); });
        QCOMPARE(refused.error().code, std::string("auth_failed"));
    }

    void aKvmThatSendsTheProgramElsewhereIsNotFollowed()
    {
        MadeUpKvm elsewhere;
        MadeUpKvm kvm;
        Answered moved(302, QByteArray());
        moved.more = {"Location: " + elsewhere.stand->address().toUtf8() + "/api/auth/login"};
        kvm.instead.insert("/api/auth/login"_ba, moved);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorMessage,
                 u"The device does not sign qKVM in (Login failed (HTTP 302)). It is asked again shortly."_s);
        // The password went to the KVM it was typed for, and to nothing else.
        QVERIFY(elsewhere.stand->asked.isEmpty());
    }

    void anAnswerThatIsPackedAllTheSameIsNotUnpacked()
    {
        MadeUpKvm kvm;
        Answered packed = good();
        packed.more = {"Content-Encoding: gzip"_ba};
        kvm.instead.insert("/api/streamer"_ba, packed);
        KvmdDevice device(kvm.where(), u"secret"_s, patient());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.errorCode, u"dropped"_s);
        QCOMPARE(status.errorMessage, u"The device packed its answer in a way this program does not unpack"_s);
    }

    void aKvmIsKnownByItsCertificate()
    {
        MadeUpKvm kvm(tls("first"));
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        KvmdDevice device(where, u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
        QVERIFY(device.shownCertificate().isEmpty());
    }

    void aKvmsOwnWronglyDatedCertificateIsTaken()
    {
        // As these KVMs make them: named "localhost", and good from 1970 to 1979.
        MadeUpKvm kvm(tls("comet"));
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("comet");
        KvmdDevice device(where, u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
    }

    void whatShowsAnotherCertificateHearsNothing()
    {
        MadeUpKvm kvm(tls("second"));
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        KvmdDevice device(where, u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"cert_mismatch"_s);
        QCOMPARE(status.errorMessage, u"Certificate does not match the one saved for this device"_s);
        QCOMPARE(device.shownCertificate(), sha256Of("second"));
        // No password, no session, not one question: and nothing done to it either.
        const auto refused = doneBy(device, core::Action::plain(u"atx_power"_s));
        QCOMPARE(refused.error().code, std::string("cert_mismatch"));
        QVERIFY(!KvmdDevice::mayHaveBeenDone(refused.error()));
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvm.signIns, 0);
    }

    void aCertificateNobodyVouchesForHearsNothing()
    {
        // Known by none: then Windows has to trust it, and Windows does not.
        MadeUpKvm kvm(tls("first"));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"cert_invalid"_s);
        QCOMPARE(status.errorMessage, u"The device's certificate is not trusted"_s);
        QCOMPARE(device.shownCertificate(), sha256Of("first"));
        QVERIFY(kvm.stand->asked.isEmpty());
    }

    void anotherCertificateOfTheSameSignerIsNotTheKvm()
    {
        // A certificate that somebody else signed, whom Windows does not know. What Windows
        // objects to in it names the certificate itself (its signer is not sent along): so it is
        // one to be known by, and what is passed over for it holds for no other.
        auto kvm = std::make_unique<MadeUpKvm>(tls("twin-one"));
        KvmdDevice::Where where = kvm->where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("twin-one");
        KvmdDevice device(where, u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);

        // Something else at its address, with another certificate of the same signer's, good for
        // the same name: that is not the KVM, and hears nothing.
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        StandIn other([](const Asked&) { return Answered{200, "{}"_ba}; }, tls("twin-two"), port);
        QVERIFY(other.isListening());
        QTest::qWait(quick().freshMs + 50);
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"cert_mismatch"_s);
        QCOMPARE(device.shownCertificate(), sha256Of("twin-two"));
        QVERIFY(other.asked.isEmpty());
        // (Where a signer's own certificate is sent along, Windows' objection names the signer,
        // and such a certificate is not one to be known by at all: the driver says so and lets
        // nothing through. The tests' own server sends none along, so that is not tried here.)
    }

    void aKvmThatWindowsVouchesForNeedsNoCertificateOfItsOwn()
    {
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        MadeUpKvm kvm(tls("signed"));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
    }

    void aSavedCertificateThatIsNoneIsSaidAndNothingIsTrustedInItsPlace()
    {
        MadeUpKvm kvm(tls("first"));
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = "cut short"_ba;
        KvmdDevice device(where, u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"cert_invalid"_s);
        QVERIFY(status.errorMessage.startsWith(u"The certificate saved for this device cannot be read."_s));
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvm.stand->notHttp, 0);
    }

    void whereWindowsVouchesForNobodyItObjectsToEveryCertificate()
    {
        // What the driver leans on for a KVM that is known by a certificate: its lines are set
        // up so that Windows has nobody to vouch for anybody, and so objects to every
        // certificate, also to one it would trust, which is the one this is tried with. The
        // driver's own look is then made for every line Qt makes, and not only for those a
        // question is waiting for.
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        MadeUpKvm kvm(tls("signed"));
        QUrl line = kvm.address();
        line.setScheme(u"wss"_s);
        line.setPath(u"/api/ws"_s);
        {
            // As Windows would have it: let in.
            QWebSocket trusting;
            QSignalSpy objections(&trusting, &QWebSocket::sslErrors);
            QSignalSpy in(&trusting, &QWebSocket::connected);
            trusting.open(line);
            QVERIFY(in.wait(10000));
            QVERIFY(objections.isEmpty());
        }
        const int linesBefore = kvm.stand->live.lines;
        QWebSocket careful;
        careful.setSslConfiguration(KvmdDevice::knownByItsCertificateAlone(careful.sslConfiguration()));
        QSignalSpy objections(&careful, &QWebSocket::sslErrors);
        QSignalSpy over(&careful, &QWebSocket::errorOccurred);
        careful.open(line);
        QVERIFY(over.wait(10000));
        QVERIFY(!objections.isEmpty());
        // Nobody said yes to it, and the asking never went out.
        QCOMPARE(kvm.stand->live.lines, linesBefore);
    }

    // The line for keyboard and mouse, as the driver hands it out.

private:
    using Console = std::unique_ptr<core::KvmdConsoleLine>;

    // Asks the driver for the line, lets the program's loop run until it has answered, and gives
    // what it handed out.
    static base::Result<Console> consoleOf(KvmdDevice& device)
    {
        std::optional<base::Result<Console>> made;
        device.openConsole([&made](base::Result<Console> line) { made.emplace(std::move(line)); });
        if (!QTest::qWaitFor([&made] { return made.has_value(); }, 20000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return std::move(*made);
    }

private slots:
    void theLineForKeyboardAndMouseIsHandedOutSignedInAndGoesOnlyToTheKvmItself()
    {
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        KvmdDevice::Where where = kvm->where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        KvmdDevice device(where, u"secret"_s, quick());
        // Nothing has been asked of the KVM yet: the driver signs in for the line, and asks
        // nothing else.
        base::Result<Console> made = consoleOf(device);
        QVERIFY2(made.ok(), made.ok() ? "" : made.error().message.c_str());
        const Console line = std::move(made).value();
        QVERIFY(line != nullptr);
        QCOMPARE(kvm->signIns, 1);
        QCOMPARE(kvm->stand->asked.size(), 2); // (the question asked first, and the sign-in)
        // Handed over as it is made: it opens by itself a turn later, and says when it is in.
        QVERIFY(!line->isOpen());
        const QSignalSpy opened(line.get(), &core::KvmdConsoleLine::opened);
        const QSignalSpy closed(line.get(), &core::KvmdConsoleLine::closed);
        QTRY_COMPARE(opened.count(), 1);
        QVERIFY(line->isOpen());
        QCOMPARE(kvm->stand->live.lines, 1);
        // Who asks is said as a cookie, and not in the address of the asking: nothing has said yet that the KVM is
        // GL.iNet's (theSessionGoesInTheAddressOnlyToAGlinetKvm). And this line watches nothing.
        QCOMPARE(kvm->stand->live.path, "/api/ws?stream=0"_ba);
        QCOMPARE(kvm->stand->live.headers.value("cookie"), "auth_token=pass1"_ba);
        QVERIFY(!kvm->stand->live.headers.contains("authorization"));
        QCOMPARE(line->pass(), "pass1"_ba);
        // It keeps no video awake, and nothing more was asked of the KVM for it.
        QVERIFY(!device.isKeptAwake());
        QCOMPARE(kvm->stand->asked.size(), 2);
        // The next one is made with the pass the driver has: it does not sign in for each.
        base::Result<Console> second = consoleOf(device);
        QVERIFY(second.ok());
        const QSignalSpy secondOpened(second.value().get(), &core::KvmdConsoleLine::opened);
        QTRY_COMPARE(secondOpened.count(), 1);
        QCOMPARE(kvm->signIns, 1);
        QCOMPARE(kvm->stand->live.lines, 2);
        second.value()->close();

        // Something else answers at the KVM's address, with a certificate that Windows trusts.
        // The line is known by what the driver is known by: the next one hears nothing, not a
        // byte of the asking, and so not the pass.
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        QTRY_COMPARE(closed.count(), 1);
        StandIn other([](const Asked&) { return Answered{200, "{}"_ba}; }, tls("signed"), port);
        QVERIFY(other.isListening());
        base::Result<Console> third = consoleOf(device);
        QVERIFY(third.ok());
        const QSignalSpy thirdOpened(third.value().get(), &core::KvmdConsoleLine::opened);
        const QSignalSpy thirdClosed(third.value().get(), &core::KvmdConsoleLine::closed);
        QTRY_COMPARE(thirdClosed.count(), 1);
        QCOMPARE(thirdClosed.at(0).at(0).toString(), u"cert_mismatch"_s);
        QTest::qWait(300);
        QCOMPARE(thirdOpened.count(), 0);
        QCOMPARE(other.live.lines, 0);
        QVERIFY(other.asked.isEmpty());
    }

    void theSessionGoesInTheAddressOnlyToAGlinetKvm()
    {
        // GL.iNet's own page sends the session in the address of its lines to /api/ws as well as in the cookie, and
        // so does this to a KVM known to be one. Stock kvmd writes the address of every request into its log, where the
        // session would stand for whoever reads it: to it, and to a KVM not known to be GL.iNet's, the cookie alone
        // (the owner's decision of 2026-11-05 on the outside review's F6: as the web, kvm.py `_line`).
        class Open : public KvmdDevice {
        public:
            using KvmdDevice::keepAwake;
            using KvmdDevice::KvmdDevice;
        };
        MadeUpKvm kvm;
        kvm.glinet = false;
        KvmdDevice::Pace pace = quick();
        pace.aboutFreshMs = 0; // (what it is, asked at every look)
        pace.restAfterMs = 30'000;
        Open device(kvm.where(), u"secret"_s, pace);
        const auto lineFor = [&device, &kvm] {
            base::Result<Console> made = consoleOf(device);
            if (!made.ok()) {
                return QByteArray("no line");
            }
            const QSignalSpy opened(made.value().get(), &core::KvmdConsoleLine::opened);
            if (!QTest::qWaitFor([&opened] { return opened.count() == 1; }, 5000)) {
                return QByteArray("not let in");
            }
            // (In the cookie, whatever the KVM is.)
            const QByteArray path = kvm.stand->live.headers.value("cookie") == "auth_token=pass1"_ba
                                        ? kvm.stand->live.path
                                        : QByteArray("no cookie");
            made.value()->close();
            return path;
        };
        // Nothing said yet: the cookie alone.
        QCOMPARE(lineFor(), "/api/ws?stream=0"_ba);
        // Stock kvmd (it answered 404 where GL.iNet's firmware answers): the cookie alone, on both lines.
        QCOMPARE(lookedAt(device).model, QString());
        QCOMPARE(lineFor(), "/api/ws?stream=0"_ba);
        device.keepAwake();
        QTRY_VERIFY(kvm.stand->live.isConnected() && kvm.stand->live.path.startsWith("/api/ws?stream=1"));
        QCOMPARE(kvm.stand->live.path, "/api/ws?stream=1"_ba);
        QCOMPARE(kvm.stand->live.headers.value("cookie"), "auth_token=pass1"_ba);
        // GL.iNet's: in both.
        kvm.glinet = true;
        QCOMPARE(lookedAt(device).model, u"RM10"_s);
        QCOMPARE(lineFor(), "/api/ws?stream=0&auth_token=pass1"_ba);
        // An answer that says neither leaves what was known.
        kvm.instead.insert("/api/upgrade/version"_ba, Answered{500, "oops"_ba});
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(lineFor(), "/api/ws?stream=0&auth_token=pass1"_ba);
        kvm.instead.insert("/api/upgrade/version"_ba, Answered{200, "[1, 2]"_ba});
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(lineFor(), "/api/ws?stream=0&auth_token=pass1"_ba);
        // And one that says now that it is stock kvmd is that from then on.
        kvm.instead.remove("/api/upgrade/version"_ba);
        kvm.glinet = false;
        QCOMPARE(lookedAt(device).model, QString());
        QCOMPARE(lineFor(), "/api/ws?stream=0"_ba);
    }

    void noLineForKeyboardAndMouseIsAskedForWhereTheSignInFails()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"not it"_s, quick());
        const base::Result<Console> made = consoleOf(device);
        QVERIFY(!made.ok());
        QCOMPARE(made.error().code, std::string("auth_failed"));
        QCOMPARE(made.error().message, std::string("Login rejected - check the username and password"));
        // No line was made, and none asked for: the sign-in, and nothing after it.
        QTest::qWait(300);
        QCOMPARE(kvm.stand->live.lines, 0);
        QCOMPARE(kvm.asked("/api/ws"), 0);
        QCOMPARE(kvm.stand->asked.size(), 2); // (the question asked first, and the sign-in)
        // Nor is the refused password sent again for the next asking.
        QVERIFY(!consoleOf(device).ok());
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(kvm.stand->asked.size(), 2);
        // And a driver that has no password asks nothing at all.
        KvmdDevice bare(kvm.where(), QString(), quick());
        const base::Result<Console> none = consoleOf(bare);
        QVERIFY(!none.ok());
        QCOMPARE(none.error().code, std::string("no_password"));
        QCOMPARE(kvm.stand->asked.size(), 2);
    }

    void aPassThatWasRefusedOnALineIsForgottenWhereTheDriverIsToldAndItIsStillTheOne()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
        // kvmd was started anew, and knows no pass it gave before. The driver cannot see that
        // while nothing is asked of the KVM: the line is refused, and the line is its holder's.
        kvm.passes.clear();
        kvm.stand->handsLinesOver = false; // (the asking for the line is answered as a question is)
        const auto refusedOn = [](const base::Result<Console>& made) {
            if (!made.ok()) {
                return u"no line was handed out"_s;
            }
            const QSignalSpy closed(made.value().get(), &core::KvmdConsoleLine::closed);
            return QTest::qWaitFor([&closed] { return closed.count() == 1; }, 5000) ? closed.at(0).at(0).toString()
                                                                                    : u"the line did not end"_s;
        };
        const base::Result<Console> first = consoleOf(device);
        QCOMPARE(refusedOn(first), u"auth_failed"_s);
        QCOMPARE(first.value()->pass(), "pass1"_ba);
        // Told of a pass that is not the one it holds, the driver forgets nothing: the next line
        // is asked for with the same pass, and nobody was signed in for it.
        device.passWasRefused("pass0"_ba);
        device.passWasRefused(QByteArray());
        const base::Result<Console> second = consoleOf(device);
        QCOMPARE(refusedOn(second), u"auth_failed"_s);
        QCOMPARE(second.value()->pass(), "pass1"_ba);
        QCOMPARE(kvm.signIns, 1);
        // Told which pass was refused, by whoever holds the line that ended for it: the driver
        // signs in anew for the next line, which is asked for with the new pass and let in.
        device.passWasRefused(second.value()->pass());
        kvm.stand->handsLinesOver = true;
        const base::Result<Console> third = consoleOf(device);
        QVERIFY(third.ok());
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(third.value()->pass(), "pass2"_ba);
        const QSignalSpy opened(third.value().get(), &core::KvmdConsoleLine::opened);
        QTRY_COMPARE(opened.count(), 1);
        QCOMPARE(kvm.stand->live.path, "/api/ws?stream=0&auth_token=pass2"_ba);
        QCOMPARE(kvm.stand->live.headers.value("cookie"), "auth_token=pass2"_ba);
        // An older line's word that comes late takes no newer pass away.
        device.passWasRefused("pass1"_ba);
        const base::Result<Console> fourth = consoleOf(device);
        QVERIFY(fourth.ok());
        QCOMPARE(fourth.value()->pass(), "pass2"_ba);
        QCOMPARE(kvm.signIns, 2);
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
        // Before the sign-in, where nothing of the KVM's was heard just now: whether it signs this program in, asked
        // with nothing to sign in with, under the guess that tells a KVM that is not there (DeviceAsker::whenUp). The
        // sign-in then goes as a question that does something: never "not there" on a guess, as it may have gone out
        // and been counted before Qt says it went.
        QFETCH(const int, line);
        MadeUpKvm kvm(line == 2 ? std::nullopt : std::optional<core::testing::Tls>(tls("first")));
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = line != 2;
        where.certificate = line != 2 ? sha256Of("first") : QByteArray();
        KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 4000; // (a TLS handshake under the address sanitizer on a busy PC)
        pace.quietMs = 3200;         // (the sign-in's own wait, two and a half times this, longer than its line's)
        if (line == 0) {
            QVERIFY(kvm.stand->letInOnlyAfter(60'000));
        } else if (line == 1) {
            // (Only the line after the question asked first: the one the sign-in goes on.)
            kvm.stand->beforeAnswer = [&kvm, slow = pace.connectWithinMs + 500, slowed = false]() mutable {
                const bool signIn = kvm.stand->asked.constLast().method == "POST";
                if (signIn || !std::exchange(slowed, true)) {
                    kvm.stand->letInOnlyAfter(signIn ? 0 : slow);
                }
            };
        } else {
            kvm.stand->closesSilently = true;
        }
        KvmdDevice device(where, u"secret"_s, pace);
        QElapsedTimer watch;
        watch.start();
        const auto who = answerTo<KvmdDevice::Identity>([&](auto done) { device.identify(std::move(done)); });
        kvm.stand->beforeAnswer = nullptr;
        if (line == 0) {
            QVERIFY(!who.ok());
            QCOMPARE(who.error().code, std::string("unreachable"));
            QVERIFY(watch.elapsed() < pace.connectWithinMs + pace.quietMs);
            QCOMPARE(kvm.signIns, 0);
            QVERIFY(!core::SignInPacing::program().standing(
                core::SignInPacing::what(u"login", where.address, where.user, u"secret"_s)));
            return;
        }
        QVERIFY2(who.ok(), who.ok() ? "" : who.error().message.c_str());
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(kvm.stand->count("/api/auth/login"), 1);
    }

    void aSecondDriverFindsTheHoldTheFirstsSignInLeftBehindItsQuestion()
    {
        // (The first question's reading.) Two drivers of one KVM (the wall's, and the Edit box's with the kept
        // password) ask their first question at once, answered a moment later; the first sends the password, which is
        // refused. The hold is looked at again just before a sign-in is counted: the second sends nothing (one strike,
        // not two).
        MadeUpKvm kvm;
        kvm.password = u"changed on the KVM"_s;
        Answered slowly = refusal(401, u"UnauthorizedError"_s);
        slowly.afterMs = 300;
        kvm.first["/api/auth/check"_ba] = {slowly, slowly};
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice one(kvm.where(), u"secret"_s, pace);
        KvmdDevice other(kvm.where(), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        };
        one.identify(keep);
        other.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(kvm.signIns, 1);
        // (The one that went is refused; the other waited for that no, and it is its no too.)
        QCOMPARE(codes, (QList<std::string>{"auth_failed", "auth_failed"}));
    }

    void aSecondDriverWaitsForASlowNoAndSendsNothing()
    {
        // (The pacing's reading, M1.) A sign-in can be out longer than the wait after one that came to nothing (a slow
        // controller's no came after twelve seconds; here 1.5 against 0.8): it holds every other as long as it can be
        // out. The other driver of the KVM (the wall's, while the Edit box signs in) sends the wrong password no second
        // time: it waits for that no, which is its no too.
        MadeUpKvm kvm;
        kvm.password = u"changed on the KVM"_s;
        kvm.signInAfterMs = 1500;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice one(kvm.where(), u"secret"_s, pace);
        KvmdDevice other(kvm.where(), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        };
        one.identify(keep);
        QTRY_COMPARE(kvm.signIns, 1);
        QTest::qWait(pace.signInRetryMs + 200);
        QVERIFY(codes.isEmpty());
        other.identify(keep);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(codes, (QList<std::string>{"auth_failed", "auth_failed"}));
    }

    void aSecondDriverWaitsForTheFirstsSignInAndThenSignsInItself()
    {
        // (The pacing's reading, L3.) The right password, a slow yes: the other driver, asking while it is out, is not
        // told the KVM "does not sign qKVM in" (the wall's card in error while the Edit box signs in). It waits for
        // that yes, as the web's second asker waits for the one sign-in, and then signs in itself: one at a time.
        MadeUpKvm kvm;
        kvm.signInAfterMs = 1200;
        KvmdDevice::Pace pace = quick();
        pace.pacing = std::make_shared<core::SignInPacing>();
        KvmdDevice one(kvm.where(), u"secret"_s, pace);
        KvmdDevice other(kvm.where(), u"secret"_s, pace);
        QList<std::string> codes;
        const auto keep = [&codes](const base::Result<KvmdDevice::Identity>& said) {
            codes.append(said.ok() ? std::string("ok") : said.error().code);
        };
        one.identify(keep);
        QTRY_COMPARE(kvm.signIns, 1);
        other.identify(keep);
        QTest::qWait(600);
        QVERIFY(codes.isEmpty());
        QCOMPARE(kvm.signIns, 1);
        QTRY_COMPARE_WITH_TIMEOUT(codes.size(), 2, 10000);
        QCOMPARE(codes, (QList<std::string>{"ok", "ok"}));
        QCOMPARE(kvm.signIns, 2);
    }

    void theQuestionAskedFirstAndTheSignInGoOnOneLine()
    {
        // Where the KVM keeps its lines, the sign-in goes on the very line the question before it was answered on.
        MadeUpKvm kvm;
        kvm.stand->keepsLines = true;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QVERIFY(answerTo<KvmdDevice::Identity>([&](auto done) { device.identify(std::move(done)); }).ok());
        const QList<core::testing::Asked>& asked = kvm.stand->asked;
        QVERIFY(asked.size() >= 2);
        QCOMPARE(asked.at(0).path, "/api/auth/check"_ba);
        QCOMPARE(asked.at(1).path, "/api/auth/login"_ba);
        QCOMPARE(asked.at(1).line, asked.at(0).line);
        // (The sign-in's line is its own, which can outlive the driver; what is asked after it goes on the driver's.)
        QCOMPARE(kvm.stand->lines, 2);
        for (qsizetype after = 2; after < asked.size(); ++after) {
            QVERIFY(asked.at(after).line != asked.at(0).line);
        }
    }
};

QTEST_GUILESS_MAIN(TestKvmdDevice)
#include "test_kvmd_device.moc"
