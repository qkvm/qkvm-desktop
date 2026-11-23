#include "vnc_by_hand.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QTest>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/own_sign_in.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/vnc_device.h>

#include "memory_secrets.h"

// A VNC server of this PC's own wall (kind "vnc"), against VNC servers by hand on this PC's own address: the web's
// tests/test_vnc.py and test_a_vnc_server_that_ends_every_connection.py, at the driver.
using namespace qkvm::core;
using namespace qkvm::core::testing;
using namespace Qt::StringLiterals;
namespace base = qkvm::base;

namespace {

Tls tlsOf(const QString& name)
{
    return {QStringLiteral(QKVM_TESTDATA "/tls/") + name + u".crt"_s,
            QStringLiteral(QKVM_TESTDATA "/tls/") + name + u".key"_s};
}

QByteArray sha256Of(const QString& name)
{
    const QList<QSslCertificate> certificates =
        QSslCertificate::fromPath(QStringLiteral(QKVM_TESTDATA "/tls/") + name + u".crt"_s, QSsl::Pem);
    return certificates.isEmpty() ? QByteArray() : certificates.first().digest(QCryptographicHash::Sha256);
}

// A VNC server by its address alone (in the clear, or known by Windows' trust for its name).
VncDevice::Where at(const QString& url)
{
    return {url, false, {}};
}

// The web's times, made short.
VncDevice::Pace quick(SignInPacing& pacing)
{
    VncDevice::Pace pace;
    pace.screen.handshakeMs = 3000;
    pace.screen.openingMs = 3000;
    pace.screen.lookPaceMs = 50;
    pace.screen.livePaceMs = 20;
    pace.screen.idlePaceMs = 1000;
    pace.screen.watchedForMs = 500;
    pace.screen.heartbeatMs = 1500;
    pace.screen.messageWithinMs = 1500;
    pace.screen.settledMs = 300;
    pace.screen.tickMs = 20;
    pace.screen.goodbyeWithinMs = 500;
    pace.screen.idleMs = 3000;
    pace.statusWaitMs = 2000;
    pace.freshMs = 100;
    pace.noPasswordHoldMs = 2000;
    pace.badHoldMs = 300;
    pace.badHoldMostMs = 1200;
    pace.errorHoldMs = 100;
    pace.backoffMostMs = 400;
    pace.cutLadderMs = {500, 1000, 2000};
    pace.servedLongMs = 60000;
    pace.signIns.refusedMs = 60000;
    pace.signIns.unansweredMs = 1000;
    pace.pacing = std::shared_ptr<SignInPacing>(&pacing, [](SignInPacing* /*the test's own*/) {});
    return pace;
}

// What a callback answers, waited for at the most `withinMs`; `never` where nothing came. The answer is kept by the
// callback and by the wait, never in the caller's frame: one that comes after the wait was given up on writes into
// nothing of a later call's (outside review 2.9).
template <typename T, typename Ask>
T waitedFor(Ask ask, int withinMs, T never)
{
    const auto came = std::make_shared<std::optional<T>>();
    ask([came](T one) { *came = std::move(one); });
    static_cast<void>(QTest::qWaitFor([came] { return came->has_value(); }, withinMs));
    return came->value_or(std::move(never));
}

// A look at its status, waited for, and what it says then.
Status lookedAt(VncDevice& device)
{
    device.look();
    // (What a callback writes into is its own, never this frame's: one that answers after the wait is over writes
    // into nothing of a later call's. Outside review 2.9.)
    const auto over = std::make_shared<bool>(false);
    device.afterLook(5000, [over](bool /*still*/) { *over = true; });
    static_cast<void>(QTest::qWaitFor([over] { return *over; }, 6000));
    return device.status();
}

// Looks every 100 ms until `until` holds or 10 s are over, as the wall's rounds do (and the cards' pictures with
// `pictures`); the status then.
Status lookUntil(VncDevice& device, const std::function<bool(const Status&)>& until, bool pictures = false)
{
    Status now = device.status();
    for (int round = 0; round < 100 && !until(now); ++round) {
        device.look();
        if (pictures) {
            device.askSnapshot(320, [](const base::Result<Snapshot>& /*made*/) {});
        }
        QTest::qWait(100);
        now = device.status();
    }
    return now;
}

base::Result<Snapshot> pictureOf(VncDevice& device)
{
    std::optional<base::Result<Snapshot>> last;
    for (int round = 0; round < 50; ++round) {
        // (A round's own: an answer to a round given up on writes into that round's, never into the next one's.)
        const auto made = std::make_shared<std::optional<base::Result<Snapshot>>>();
        device.askSnapshot(320, [made](base::Result<Snapshot> one) { *made = std::move(one); });
        static_cast<void>(QTest::qWaitFor([made] { return made->has_value(); }, 3000));
        last = *made;
        if (last && last->ok()) {
            return *last;
        }
        QTest::qWait(100);
    }
    return last.value_or(base::Error{"never", "No picture came"});
}

// An action's answer, as it comes: kept by whoever asks, and by the callback, so that one that answers after its wait
// was given up on writes into nothing of the caller's frame (outside review 2.9).
using ActAnswer = std::shared_ptr<std::optional<base::Result<Done>>>;

ActAnswer actFor(VncDevice& device, const Action& what)
{
    const auto said = std::make_shared<std::optional<base::Result<Done>>>();
    device.act(what, [said](base::Result<Done> how) { *said = std::move(how); });
    return said;
}

base::Result<Done> answerOf(const ActAnswer& said, int withinMs = 10000)
{
    static_cast<void>(QTest::qWaitFor([said] { return said->has_value(); }, withinMs));
    return said->value_or(base::Error{"never", "Not answered"});
}

base::Result<Done> actOn(VncDevice& device, const Action& what)
{
    return answerOf(actFor(device, what));
}

} // namespace

class TestVncDevice : public QObject {
    Q_OBJECT

private slots:
    // (Windows' own TLS, chosen before anything else uses TLS.)
    void initTestCase() { QVERIFY(useWindowsOwnTls()); }

    void howAVncAddressIsWritten()
    {
        // The web's test_how_a_vnc_address_is_written.
        const auto written = [](const QString& typed) {
            const base::Result<QString> made = vnc::addressFrom(typed);
            return made.ok() ? made.value() : u"(none)"_s;
        };
        QCOMPARE(written(u"192.168.1.20"_s), u"vnc://192.168.1.20"_s);
        QCOMPARE(written(u" 192.168.1.20:5900 "_s), u"vnc://192.168.1.20"_s);
        QCOMPARE(written(u"192.168.1.20:5901"_s), u"vnc://192.168.1.20:5901"_s);
        QCOMPARE(written(u"VNC://Box.lan.:5902"_s), u"vnc://box.lan:5902"_s);
        QCOMPARE(written(u"vncs://box.lan"_s), u"vncs://box.lan"_s);
        QCOMPARE(written(u"[::1]:5901"_s), u"vnc://[::1]:5901"_s);
        QCOMPARE(written(u"vnc://box.lan/some/path"_s), u"vnc://box.lan"_s);
        for (const QString& bad :
             {u""_s, u"a b"_s, u"box:0"_s, u"box:70000"_s, u"http://box"_s, u"user@box"_s, u"box:x"_s, u"vnc://"_s}) {
            QVERIFY2(!vnc::addressFrom(bad).ok(), qPrintable(bad));
            QVERIFY(
                !QString::fromStdString(vnc::addressFrom(bad).error().message).contains(bad.isEmpty() ? u"#"_s : bad));
        }
        // Outside review c9a-3, L1: a scheme written amiss, with one slash or none ("vncs:/10.0.0.6:5901", "vncs//box",
        // "vnc:/box"), in any case, is no address at all, and not one at a host called "vncs" or "vnc", which the web
        // makes of it, and which anyone on the network may answer for. Said so, with nothing of what was typed.
        const QString amiss = u"This begins as a VNC server's address does, with vnc or vncs, but not with vnc:// or "
                              "vncs://. Write either in full, with both slashes."_s;
        for (const QString& typed :
             {u"vncs:/10.0.0.6:5901"_s, u"vncs//box.lan"_s, u"vnc:/box.lan"_s, u"VNCS:/10.0.0.6"_s, u"Vnc:/box.lan"_s,
              u"vNcS//box.lan"_s, u" vnc:box.lan"_s, u"vnc:5901"_s, u"vncs:"_s, u"vnc//box.lan:5902"_s}) {
            const base::Result<QString> refused = vnc::addressFrom(typed);
            QVERIFY2(!refused.ok(), qPrintable(typed + u" -> "_s + (refused.ok() ? refused.value() : QString())));
            QCOMPARE(refused.error().code, std::string("invalid"));
            QCOMPARE(QString::fromStdString(refused.error().message), amiss);
            QVERIFY(vnc::schemeAmiss(typed));
        }
        // (A host of that name, or that begins so, is one all the same; and with its scheme written whole.)
        QCOMPARE(written(u"vnc"_s), u"vnc://vnc"_s);
        QCOMPARE(written(u"vncs"_s), u"vnc://vncs"_s);
        QCOMPARE(written(u"vncbox.lan:5901"_s), u"vnc://vncbox.lan:5901"_s);
        QCOMPARE(written(u"vnc.lan"_s), u"vnc://vnc.lan"_s);
        QCOMPARE(written(u"vnc://vnc:5901"_s), u"vnc://vnc:5901"_s);
        QCOMPARE(written(u"VNCS://vncs.lan"_s), u"vncs://vncs.lan"_s);
        for (const QString& typed : {u"vnc"_s, u"vncbox.lan:5901"_s, u"vnc://vnc:5901"_s, u"VNCS://box"_s}) {
            QVERIFY2(!vnc::schemeAmiss(typed), qPrintable(typed));
        }
        // Outside review c9a-4, L1: what Qt drops from a host or makes another letter of, as it reads a name (a soft
        // hyphen, a sign of no width, a mark of the byte order; letters of the full width), passed the refusal above,
        // which looks at the text as typed: Qt read "vncs:/10.0.0.6" behind one as a server at a host called "vncs",
        // with no port, and so did a scheme typed twice, also as the box that adds one makes it where VNC servers are
        // chosen ("vnc://" before what was typed). The host is held to what was typed, as the web's vnc_url holds it to
        // its device_url: letters of the twenty-six, digits, dots, hyphens and the underscore, or numbers written the
        // one way; and a colon with no port behind it is no port.
        const auto sign = [](char16_t number) { return QString(QChar(number)); };
        const QString fullWidth = sign(0xFF56) + sign(0xFF4E) + sign(0xFF43) + sign(0xFF53); // "vncs" of the full width
        const QStringList droppedOrMapped{sign(0x00AD) + u"vncs:/10.0.0.6:5901"_s,
                                          sign(0x200B) + u"vncs:/10.0.0.6:5901"_s,
                                          sign(0x2060) + u"vncs:/10.0.0.6"_s,
                                          sign(0xFEFF) + u"vncs:/10.0.0.6"_s,
                                          u"vn"_s + sign(0x00AD) + u"cs:/10.0.0.6"_s,
                                          fullWidth + u":/10.0.0.6"_s,
                                          u"vnc"_s + sign(0x017F) + u":/10.0.0.6"_s,
                                          u"vnc://vncs:/10.0.0.6:5901"_s,
                                          u"vncs://vnc:/10.0.0.6"_s,
                                          u"VNC://VNCS:/10.0.0.6"_s,
                                          u"box.lan:"_s,
                                          u"vncs://box.lan:"_s,
                                          sign(0x00AD) + u"vncs:5901"_s, // (no slash: a host "vncs" at 5901)
                                          sign(0xFEFF) + u"vnc:5901"_s,
                                          fullWidth + u":5901"_s};
        for (const QString& typed : droppedOrMapped) {
            // As typed, and as the box makes it where VNC servers are chosen.
            for (const QString& made : {typed, u"vnc://"_s + typed}) {
                const base::Result<QString> refused = vnc::addressFrom(made);
                QVERIFY2(!refused.ok(), qPrintable(made + u" -> "_s + (refused.ok() ? refused.value() : QString())));
                QCOMPARE(refused.error().code, std::string("invalid"));
            }
        }
        // Outside review fin-1, A-L1: a question mark or a cross, where Qt and the web end a host and its port, is no
        // part of a VNC server's address anywhere (it has no question and no place in a page to be asked). Read so, a
        // colon before one was a host with no port ("vncs:" of "vnc://vncs:?/10.0.0.6": a server at a host called
        // "vncs"), and a port behind one was dropped ("10.0.0.6?:5901" was 10.0.0.6 at 5900). Refused, as typed and as
        // the box makes it where VNC servers are chosen, with nothing of what was typed (stricter than the web, which
        // reads them so).
        for (const QString& typed :
             {u"vnc://vncs:?/10.0.0.6"_s, u"vnc://vncs:?/10.0.0.6:5901"_s, u"vnc://vncs:?x"_s, u"vnc://vncs:#5901"_s,
              u"vncs://box:#"_s, u"vncs://box.lan:?x"_s, u"10.0.0.6?:5901"_s, u"10.0.0.6#5901"_s, u"10.0.0.6#:5901"_s,
              u"vnc://10.0.0.6?:5901"_s, u"box.lan?"_s, u"box.lan:5901#"_s, u"[::1]?:5901"_s, u"vnc://box.lan/a?b"_s}) {
            for (const QString& made : {typed, u"vnc://"_s + typed}) {
                const base::Result<QString> refused = vnc::addressFrom(made);
                QVERIFY2(!refused.ok(), qPrintable(made + u" -> "_s + (refused.ok() ? refused.value() : QString())));
                QCOMPARE(refused.error().code, std::string("invalid"));
                QCOMPARE(refused.error().message, std::string("Not the address of a VNC server"));
            }
        }
        // Stricter than the web's vnc_url in more than that, as SPEC lists it (Add a KVM): a tab, a line's end or
        // another sign of control anywhere, which the web drops, or strips at the ends, and reads on past; a name Qt
        // will not take (a part that begins or ends with a hyphen, one longer than 63 signs, two dots together), which
        // the web takes; and what Qt reads as numbers, which the web takes for a name ("0b0" is 0.0.0.0 to Qt, "123" is
        // 0.0.0.123). Each is refused here.
        for (const QString& typed : {u"10.0.0.6"_s + sign(0x09) + u":5901"_s, u"10.0.0.6"_s + sign(0x0A) + u":5901"_s,
                                     u"10.0."_s + sign(0x0D) + u"0.6"_s, sign(0x1F) + u"10.0.0.6"_s,
                                     sign(0x01) + u"box.lan"_s, u"box.lan"_s + sign(0x1C), u"-box.lan"_s, u"box-.lan"_s,
                                     QString(64, u'a') + u".lan"_s, u"a..b"_s, u"0b0"_s, u"123"_s}) {
            const base::Result<QString> made = vnc::addressFrom(typed);
            QVERIFY2(!made.ok(), qPrintable(typed + u" -> "_s + (made.ok() ? made.value() : QString())));
        }
        // A name of letters beyond the twenty-six, typed so, is refused as the web refuses it; written in its xn-- form
        // it is one. Numbers are written the one way, as a KVM's are (and as the web has them): "127.1" and "0x7f.1",
        // which Qt takes for 127.0.0.1, are none.
        for (const QString& typed :
             {u"m"_s + sign(0xFC) + u"nchen.lan"_s, u"127.1"_s, u"0x7f.0.0.1:5901"_s, u"010.1.1.1"_s, u"box.1"_s}) {
            QVERIFY2(!vnc::addressFrom(typed).ok(), qPrintable(typed));
        }
        QCOMPARE(written(u"xn--mnchen-3ya.lan:5901"_s), u"vnc://xn--mnchen-3ya.lan:5901"_s);
        QCOMPARE(written(u"vncs://[0:0:0:0:0:0:0:1]:5901"_s), u"vncs://[::1]:5901"_s);
        QCOMPARE(written(u"[::FFFF:127.0.0.1]"_s), u"vnc://[::ffff:127.0.0.1]"_s);
        QCOMPARE(written(u"vnc_box.lan:05901"_s), u"vnc://vnc_box.lan:5901"_s);
        QCOMPARE(written(u"10.0.0.6:5901/"_s), u"vnc://10.0.0.6:5901"_s);
        const vnc::Place plain = vnc::placeOf(u"vnc://box.lan"_s);
        QCOMPARE(plain.host, u"box.lan"_s);
        QCOMPARE(plain.port, 5900);
        QVERIFY(!plain.tls);
        const vnc::Place secure = vnc::placeOf(u"vncs://[::1]:5901"_s);
        QCOMPARE(secure.host, u"::1"_s);
        QCOMPARE(secure.port, 5901);
        QVERIFY(secure.tls);
        QCOMPARE(vnc::lineAddress(u"vncs://[::1]:5901"_s), QUrl(u"https://[::1]:5901"_s));
        QCOMPARE(vnc::lineAddress(u"vnc://box.lan"_s), QUrl(u"http://box.lan:5900"_s));
        QVERIFY(vnc::namesOne(u" VNCS://box"_s));
        QVERIFY(!vnc::namesOne(u"https://box"_s));
    }

    void aVncServerShowsItsScreenAndSaysWhatItIs()
    {
        SignInPacing pacing;
        VncByHand server;
        VncDevice device(at(server.url()), u"secret"_s, quick(pacing));
        const Status said = lookUntil(device, [](const Status& one) { return one.state == u"online"_s; });
        QCOMPARE(said.state, u"online"_s);
        QCOMPARE(said.signal, std::optional<bool>(true));
        QCOMPARE(said.width, 160);
        QCOMPARE(said.height, 100);
        QCOMPARE(said.model, u"stand-in VNC"_s);
        QCOMPARE(said.version, u"RFB 3.8"_s);
        QCOMPARE(said.capabilities, std::optional<QStringList>(VncDevice::can()));
        QCOMPARE(VncDevice::can(), (QStringList{u"screen"_s, u"keys"_s, u"text"_s, u"live"_s, u"control"_s}));
        const base::Result<Snapshot> picture = pictureOf(device);
        QVERIFY2(picture.ok(), picture.ok() ? "" : picture.error().message.c_str());
        QVERIFY(!picture.value().picture.isEmpty()); // (the card's still, as a server's screen has one)
        QCOMPARE(server.passwordTries, 1);
        QVERIFY(server.shared == std::vector<int>{1});
        // The password the server took is noted so: nothing stands for it any more (no sign-in out, no hold).
        QVERIFY(!pacing.standing(SignInPacing::what(u"login", QUrl(server.url()), {}, u"secret"_s)).has_value());
        QCOMPARE(server.connections, 1); // (the looks and the pictures share the one connection)
    }

    void aServersNameIsOneLineOfWhatCanBeShownCutAtAWholeCharacter()
    {
        // Outside review c9a-2, L5, the web's kvm.plain: a server's name as one line of what can be shown, each
        // character whole. One beyond the first plane (two UTF-16 units) is kept, as the web keeps what Python finds
        // printable; a format character (a bidi control) is a blank; and where the name is cut (80 for its name, 40
        // for the model its card shows) it is cut before such a character, never through it.
        const QString picture = QString::fromStdU32String(std::u32string(1, char32_t{0x1F5A5}));
        const auto identityOf = [](VncDevice& device) {
            return waitedFor<base::Result<VncDevice::Identity>>(
                [&device](auto done) { device.identify(std::move(done)); }, 15000,
                base::Error{"never", "Not answered"});
        };
        const struct {
            QString given;
            QString name;
            QString model;
        } cases[] = {
            {u"Rack "_s + picture + u" "_s + QChar(char16_t{0x202E}) + u"evil"_s, u"Rack "_s + picture + u" evil"_s,
             u"Rack "_s + picture + u" evil"_s},
            {QString(39, u'a') + picture, QString(39, u'a') + picture, QString(39, u'a')},
            {QString(79, u'b') + picture + u"c"_s, QString(79, u'b'), QString(40, u'b')},
        };
        for (const auto& one : cases) {
            SignInPacing pacing;
            VncByHand server;
            server.name = one.given.toStdString();
            VncDevice device(at(server.url()), u"secret"_s, quick(pacing));
            const base::Result<VncDevice::Identity> said = identityOf(device);
            QVERIFY2(said.ok(), said.ok() ? "" : said.error().message.c_str());
            QCOMPARE(said.value().model, one.name);
            const Status now = lookUntil(device, [](const Status& status) { return status.state == u"online"_s; });
            QCOMPARE(now.model, one.model);
        }
    }

    void aWrongPasswordIsSaidAndNotOfferedAgainAndAgain()
    {
        SignInPacing pacing;
        VncByHand server;
        VncDevice device(at(server.url()), u"wrong"_s, quick(pacing));
        const Status said = lookUntil(device, [](const Status& one) { return one.state == u"auth"_s; });
        QCOMPARE(said.state, u"auth"_s);
        QCOMPARE(said.errorCode, u"auth_failed"_s);
        QCOMPARE(said.errorMessage, u"Login rejected - check the username and password"_s);
        QVERIFY(!said.errorMessage.contains(u"wrong"_s));
        // Looked at and asked for pictures again and again, it is not sent again meanwhile.
        for (int round = 0; round < 15; ++round) {
            device.look();
            device.askSnapshot(320, [](const base::Result<Snapshot>& /*made*/) {});
            QTest::qWait(100);
        }
        QCOMPARE(server.passwordTries, 1);
        QCOMPARE(device.status().errorCode, u"auth_failed"_s);
        // Another driver of it (an edit made a new one) holds back the same.
        VncDevice again(at(server.url()), u"wrong"_s, quick(pacing));
        QCOMPARE(lookedAt(again).errorCode, u"auth_failed"_s);
        QTest::qWait(300);
        QCOMPARE(server.passwordTries, 1);
    }

    void twoDriversOfOneServerNeverBothSendItsPassword()
    {
        // Outside review 2.4: "one password at a time" holds when the answer goes, and not only when the way in is
        // made. The wall's driver and the Change box's (two drivers of one server, with one password) both make their
        // way in before either has sent: the first whose challenge comes answers it, and the other sends nothing while
        // that one is out (and, the password refused, nothing during its wait): a wrong password is tried once, not
        // twice at once.
        SignInPacing pacing;
        VncByHand server;
        VncDevice wall(at(server.url()), u"wrong"_s, quick(pacing));
        VncDevice box(at(server.url()), u"wrong"_s, quick(pacing));
        wall.look();
        box.look();
        QTRY_VERIFY_WITH_TIMEOUT(server.connections >= 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.rejected >= 1, 5000);
        for (int round = 0; round < 20; ++round) {
            wall.look();
            box.look();
            QTest::qWait(100);
        }
        QCOMPARE(server.passwordTries, 1);
        QCOMPARE(server.rejected, 1);
        QCOMPARE(lookedAt(wall).errorCode, u"auth_failed"_s);
        QCOMPARE(lookedAt(box).errorCode, u"auth_failed"_s);
    }

    void aServerThatWantsAPasswordNobodySavedIsSaidAsThat()
    {
        SignInPacing pacing;
        VncByHand server;
        VncDevice device(at(server.url()), QString(), quick(pacing));
        const Status said = lookUntil(device, [](const Status& one) { return one.state == u"auth"_s; });
        QCOMPARE(said.errorCode, u"no_password"_s);
        QCOMPARE(said.errorMessage, u"No password saved for this device"_s);
        QCOMPARE(server.passwordTries, 0); // (nothing was sent for a password that is not there)
        // ...and it is not connected to again at every look meanwhile (asked again after its hold, two seconds here).
        const int connections = server.connections;
        for (int round = 0; round < 10; ++round) {
            device.look();
            QTest::qWait(100);
        }
        QCOMPARE(server.connections, connections);
    }

    void aServerWhosePasswordWaitsIsAskedWhetherItIsThereAndIsOfflineWhenGone_data()
    {
        QTest::addColumn<QString>("password");
        QTest::addColumn<QString>("code");
        QTest::newRow("a rejected password") << u"wrong"_s << u"auth_failed"_s;
        QTest::newRow("none saved") << QString() << u"no_password"_s;
    }

    void aServerWhosePasswordWaitsIsAskedWhetherItIsThereAndIsOfflineWhenGone()
    {
        // Outside review L1, the web's _gone_meanwhile_vnc: while a password waits (rejected, or none saved), whether
        // the server is there at all is asked by its greeting every STILL_THERE_EVERY, with nothing sent; gone, it is
        // offline, and back, the password waits as it did.
        QFETCH(QString, password);
        QFETCH(QString, code);
        SignInPacing pacing;
        auto server = std::make_unique<VncByHand>();
        const quint16 port = server->port();
        VncDevice::Pace pace = quick(pacing);
        pace.stillThereEveryMs = 600;
        pace.noPasswordHoldMs = 60000;
        VncDevice device(at(server->url()), password, pace);
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"auth"_s; }).errorCode, code);
        const int tries = password.isEmpty() ? 0 : 1;
        QCOMPARE(server->passwordTries, tries);
        const qint64 heard = server->bytesHeard;
        const int connections = server->connections;
        // Looked at for a while: asked by its greeting more than once, and sent nothing.
        static_cast<void>(lookUntil(
            device, [&server, connections](const Status& /*now*/) { return server->connections >= connections + 2; }));
        QVERIFY(server->connections >= connections + 2);
        // ...at most every STILL_THERE_EVERY (0.6 s here), and not at every look.
        const int asked = server->connections;
        for (int round = 0; round < 15; ++round) {
            device.look();
            QTest::qWait(100);
        }
        QVERIFY2(server->connections - asked <= 4, qPrintable(QString::number(server->connections - asked)));
        QCOMPARE(server->bytesHeard, heard);
        QCOMPARE(server->passwordTries, tries);
        QCOMPARE(device.status().errorCode, code);
        server.reset();
        const Status gone = lookUntil(device, [](const Status& one) { return one.state == u"offline"_s; });
        QCOMPARE(gone.state, u"offline"_s);
        QCOMPARE(gone.errorCode, u"unreachable"_s);
        server = std::make_unique<VncByHand>(std::nullopt, port);
        QCOMPARE(server->port(), port);
        const Status back = lookUntil(device, [](const Status& one) { return one.state == u"auth"_s; });
        QCOMPARE(back.errorCode, code);
        QCOMPARE(server->passwordTries, 0);
        QCOMPARE(server->bytesHeard, qint64{0});
    }

    void aCertificateThatChangedWhileAPasswordWaitsIsSaidAsTheCertificatesError()
    {
        // Outside review c9a-2, L1 (the builder's difference 3, not taken), the web's _gone_meanwhile_vnc: while a
        // password waits, a greeting whose line shows another certificate than the server is known by is no server
        // gone. It is said as the screen says it ("cert_mismatch", "error", whose card offers "Review certificate"),
        // and not a byte goes to what shows it.
        SignInPacing pacing;
        auto first = std::make_unique<VncByHand>(tlsOf(u"first"_s));
        const quint16 port = first->port();
        VncDevice::Pace pace = quick(pacing);
        pace.stillThereEveryMs = 600;
        VncDevice device({first->url(), true, sha256Of(u"first"_s)}, u"wrong"_s, pace);
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"auth"_s; }).errorCode,
                 u"auth_failed"_s);
        first.reset();
        VncByHand other(tlsOf(u"second"_s), port);
        QCOMPARE(other.port(), port);
        const Status said = lookUntil(device, [](const Status& one) { return one.errorCode == u"cert_mismatch"_s; });
        QCOMPARE(said.errorCode, u"cert_mismatch"_s);
        QCOMPARE(said.state, u"error"_s);
        QCOMPARE(said.errorMessage, u"Certificate does not match the one saved for this device"_s);
        QTest::qWait(300);
        QCOMPARE(other.passwordTries, 0);
        QCOMPARE(other.bytesHeard, qint64{0});
    }

    void aSessionRefusedBeforeItsLineIsNoHearingOfTheServer()
    {
        // The second reading's note on the builder's difference 2: only a session that opened its line has heard the
        // server. One whose password the pacing held back by itself (another driver's refusal of that very password)
        // asks the greeting at its next look, and not a whole STILL_THERE_EVERY later.
        SignInPacing pacing;
        auto server = std::make_unique<VncByHand>();
        VncDevice::Pace pace = quick(pacing);
        pace.stillThereEveryMs = 30000;
        VncDevice first(at(server->url()), u"wrong"_s, pace);
        QCOMPARE(lookUntil(first, [](const Status& one) { return one.state == u"auth"_s; }).errorCode,
                 u"auth_failed"_s);
        QCOMPARE(server->passwordTries, 1);
        const int connections = server->connections;
        VncDevice second(at(server->url()), u"wrong"_s, pace);
        QCOMPARE(lookUntil(second, [](const Status& one) { return one.state == u"auth"_s; }).errorCode,
                 u"auth_failed"_s);
        QCOMPARE(server->passwordTries, 1); // (held back: nothing went)
        static_cast<void>(lookUntil(
            second, [&server, connections](const Status& /*now*/) { return server->connections > connections; }));
        QVERIFY(server->connections > connections);
        QCOMPARE(server->passwordTries, 1);
    }

    void duringAWaitBetweenEndsInARowAServerThatIsGoneIsOffline()
    {
        // Outside review c9a-2, L2, the web's STILL_THERE_IN_A_WAIT: during a wait between ends in a row, whether the
        // server is there at all is asked by its greeting every so often (two minutes; here 0.6 s), with nothing sent;
        // gone, it is offline, and not said to end every connection until the rung is over.
        SignInPacing pacing;
        auto server = std::make_unique<VncByHand>();
        server->endsAfterMs = 600;
        VncDevice::Pace pace = quick(pacing);
        pace.cutLadderMs = {20000, 40000, 60000};
        pace.stillThereInAWaitMs = 600;
        VncDevice device(at(server->url()), u"secret"_s, pace);
        const Status waiting = lookUntil(
            device, [](const Status& one) { return one.state == u"error"_s && one.errorCode == u"bad_response"_s; },
            true);
        QCOMPARE(waiting.state, u"error"_s);
        QVERIFY2(waiting.errorMessage.startsWith(u"The VNC server ends every connection"_s),
                 qPrintable(waiting.errorMessage));
        // Asked by its greeting while it waits, and sent nothing for that.
        const int connections = server->connections;
        const qint64 heard = server->bytesHeard;
        static_cast<void>(lookUntil(
            device, [&server, connections](const Status& /*now*/) { return server->connections > connections; }));
        QVERIFY(server->connections > connections);
        QCOMPARE(server->bytesHeard, heard);
        QCOMPARE(device.status().state, u"error"_s);
        server.reset();
        const Status gone = lookUntil(device, [](const Status& one) { return one.state == u"offline"_s; });
        QCOMPARE(gone.state, u"offline"_s);
        QCOMPARE(gone.errorCode, u"unreachable"_s);
    }

    void aServerThatAsksForNoPasswordIsShownWithoutOne()
    {
        SignInPacing pacing;
        VncByHand server;
        server.password.reset();
        VncDevice device(at(server.url()), QString(), quick(pacing));
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
    }

    void aServerThatGoesAwayIsOfflineAndBackWhenItReturns()
    {
        SignInPacing pacing;
        auto server = std::make_unique<VncByHand>();
        const quint16 port = server->port();
        const QString url = server->url();
        VncDevice device(at(url), u"secret"_s, quick(pacing));
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
        server.reset();
        const Status gone = lookUntil(device, [](const Status& one) { return one.state == u"offline"_s; });
        QCOMPARE(gone.state, u"offline"_s);
        server = std::make_unique<VncByHand>(std::nullopt, port);
        QCOMPARE(server->port(), port);
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
    }

    void keysAndCtrlAltDelReachTheServerAndNothingElseIsOffered()
    {
        SignInPacing pacing;
        VncByHand server;
        VncDevice device(at(server.url()), u"secret"_s, quick(pacing));
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
        QVERIFY(actOn(device, Action::press(u"MetaLeft,KeyL"_s)).ok());
        QVERIFY(!actOn(device, Action::press(u"not keys!"_s)).ok());
        // A key there is none of, in the web's words (outside review L4).
        const base::Result<Done> noKey = actOn(device, Action::press(u"Lang1"_s));
        QVERIFY(!noKey.ok());
        QCOMPARE(noKey.error().message, std::string("There is no Lang1 key to send to a VNC server"));
        QVERIFY(actOn(device, Action::plain(u"ctrl_alt_del"_s)).ok());
        for (const QString& other : {u"atx_power"_s, u"reboot"_s, u"jiggler_on"_s, u"power_on"_s}) {
            const base::Result<Done> refused = actOn(device, Action::plain(other));
            QVERIFY2(!refused.ok(), qPrintable(other));
            QCOMPARE(refused.error().code, std::string("invalid"));
        }
        QVERIFY(!actOn(device, Action::wake(u"AA:BB:CC:DD:EE:FF"_s)).ok());
        const base::Result<QList<WakeTarget>> listed =
            waitedFor<base::Result<QList<WakeTarget>>>([&device](auto done) { device.askWakeTargets(std::move(done)); },
                                                       5000, base::Error{"never", "Not answered"});
        QVERIFY(listed.ok() && listed.value().isEmpty());
        QTRY_COMPARE(server.keys.size(), std::size_t{4 + 6});
        const std::vector<std::pair<std::uint32_t, bool>> expected{
            {0xFFEB, true}, {0x6C, true},   {0x6C, false},  {0xFFEB, false}, // Super_L, l
            {0xFFE3, true}, {0xFFE9, true}, {0xFFFF, true}, {0xFFFF, false}, {0xFFE9, false}, {0xFFE3, false}};
        QVERIFY(server.keys == expected);
    }

    void textIsTypedByItsKeysymsWithNoShiftOfThisProgramsOwn()
    {
        // The server's keyboard layout makes each keysym a key: no US Shift around a capital or a sign (the web's
        // text_events with us_shift False), and what no keysym says is refused, by its place, before anything goes.
        SignInPacing pacing;
        VncByHand server;
        VncDevice device(at(server.url()), u"secret"_s, quick(pacing));
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
        QVERIFY(actOn(device, Action::type(u"Hi!"_s, true)).ok());
        QTRY_COMPARE(server.keys.size(), std::size_t{8});
        const std::vector<std::pair<std::uint32_t, bool>> typed{{0x48, true},   {0x48, false},  {0x69, true},
                                                                {0x69, false},  {0x21, true},   {0x21, false},
                                                                {0xFF0D, true}, {0xFF0D, false}};
        QVERIFY(server.keys == typed);
        QVERIFY(actOn(device, Action::type(u"caf"_s + QChar(0xE9) + u" "_s + QChar(0x20AC), false)).ok());
        QTRY_COMPARE(server.keys.size(), std::size_t{8 + 12});
        std::vector<std::uint32_t> down;
        for (std::size_t at = 8; at < server.keys.size(); ++at) {
            if (server.keys.at(at).second) {
                down.push_back(server.keys.at(at).first);
            }
        }
        QVERIFY(down == (std::vector<std::uint32_t>{0x63, 0x61, 0x66, 0xE9, 0x20, 0x01000000 + 0x20AC}));
        const base::Result<Done> empty = actOn(device, Action::type(QString(), false));
        QVERIFY(!empty.ok());
        QCOMPARE(empty.error().code, std::string("invalid"));
        const base::Result<Done> control = actOn(device, Action::type(u"ab"_s + QChar(0x01) + u"secret"_s, false));
        QVERIFY(!control.ok());
        QCOMPARE(control.error().code, std::string("invalid"));
        QCOMPARE(control.error().message, std::string("Character 3 of the text cannot be typed on a VNC server"));
        QTest::qWait(200);
        QCOMPARE(server.keys.size(), std::size_t{8 + 12}); // (nothing of a refused text went)
        // Half a character by itself (outside review L3): refused by its place, as the web's keysym_of refuses it; a
        // pair of halves is one character, a keysym of its own.
        const base::Result<Done> half = actOn(device, Action::type(u"ab"_s + QChar(0xD800) + u"c"_s, false));
        QVERIFY(!half.ok());
        QCOMPARE(half.error().message, std::string("Character 3 of the text cannot be typed on a VNC server"));
        QVERIFY(actOn(device, Action::type(QString(QChar(0xD83D)) + QChar(0xDE00), false)).ok());
        QTRY_COMPARE(server.keys.size(), std::size_t{8 + 12 + 2});
        QCOMPARE(server.keys.at(8 + 12).first, std::uint32_t{0x01000000 + 0x1F600});
    }

    void anEnterAfterATextIsPressedInTheTextsOwnTyping()
    {
        // Outside review 2.3: Enter after a text belongs to the text's own typing, in its turn of the keyboard, as the
        // web types it (the text with a line's end after it): never a typing of its own after it, which would wait
        // behind whatever was asked for meanwhile, and ask for the keyboard anew, and be given a new session's where
        // the text was lost on a line that fell. Here a key asked for just after the text comes after its Enter.
        SignInPacing pacing;
        VncByHand server;
        VncDevice device(at(server.url()), u"secret"_s, quick(pacing));
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
        const ActAnswer typed = actFor(device, Action::type(u"ls"_s, true));
        const ActAnswer pressed = actFor(device, Action::press(u"KeyA"_s));
        QVERIFY(answerOf(typed).ok());
        QVERIFY(answerOf(pressed).ok());
        QTRY_COMPARE(server.keys.size(), std::size_t{8});
        const std::vector<std::pair<std::uint32_t, bool>> inTurn{{0x6C, true},  {0x6C, false},  {0x73, true},
                                                                 {0x73, false}, {0xFF0D, true}, {0xFF0D, false},
                                                                 {0x61, true},  {0x61, false}};
        QVERIFY(server.keys == inTurn);
    }

    void aTextCutShortByItsLineIsNeverEnteredOnTheNextSession()
    {
        // Outside review 2.3, the line that falls: a text of sixteen characters is a piece of its own (32 key events),
        // and its Enter the next. The server ends the connection once the text's piece has come: the typing ends there
        // ("dropped", which may have been done), and its Enter is pressed on no session, the next one least of all.
        SignInPacing pacing;
        VncByHand server;
        server.hangsUpAfterKeys = 32;
        VncDevice::Pace pace = quick(pacing);
        pace.screen.piecePauseMs = 300; // (the fall is heard between the two pieces)
        VncDevice device(at(server.url()), u"secret"_s, pace);
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
        const base::Result<Done> typed = actOn(device, Action::type(u"0123456789abcdef"_s, true));
        QVERIFY(!typed.ok());
        QVERIFY2(VncDevice::mayHaveBeenDone(typed.error()), typed.error().code.c_str());
        // The next session comes, and nothing of the text goes on it.
        lookUntil(device, [&server](const Status& /*one*/) { return server.handshakes >= 2; });
        QVERIFY2(server.handshakes >= 2, qPrintable(QString::number(server.handshakes)));
        QTest::qWait(500);
        for (std::size_t at = 0; at < server.keys.size(); ++at) {
            QVERIFY2(server.keys.at(at).first != 0xFF0D,
                     qPrintable(u"An Enter on connection %1"_s.arg(server.keysOn.at(at))));
            QCOMPARE(server.keysOn.at(at), 1);
        }
    }

    void aTypingUnderWayStopsWhereItStandsWhenItIsToGoNoFurther()
    {
        // The outside review of the wave 2 desktop, 6.18: the lock stopped new actions and the rest of a run, and not
        // an action already handed to the driver: a long text, and its Enter, were typed to the end after Lock.
        // Stopped, the typing ends where it stands, with nothing down, no Enter after it, and its action said
        // "stopped"; and the wall stops every one of its drivers' (as the lock asks it).
        SignInPacing pacing;
        VncByHand server;
        VncDevice::Pace pace = quick(pacing);
        pace.screen.keysAtOnce = 2;
        pace.screen.piecePauseMs = 50;
        VncDevice device(at(server.url()), u"secret"_s, pace);
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
        const QString text(40, u'x');
        std::optional<base::Result<Done>> said;
        device.act(Action::type(text, true), [&said](base::Result<Done> how) { said = std::move(how); });
        QTRY_VERIFY(server.keys.size() >= 4);
        device.stopTyping();
        QTRY_VERIFY(said.has_value());
        const base::Result<Done> stopped = said.value_or(Done{});
        QVERIFY(!stopped.ok());
        QCOMPARE(stopped.error().code, std::string("stopped"));
        QTest::qWait(300);
        QVERIFY2(server.keys.size() < 2 * static_cast<std::size_t>(text.size()),
                 qPrintable(QString::number(server.keys.size())));
        QVERIFY(std::ranges::none_of(server.keys, [](const auto& key) { return key.first == 0xFF0D; }));
        QVERIFY(server.stillDown().empty());
        // And typed again, it goes whole.
        QVERIFY(actOn(device, Action::type(u"ok"_s, false)).ok());

        // The wall's, for every driver it holds.
        VncByHand walled;
        const QTemporaryDir folder;
        Settings settings(folder.filePath(u"settings.json"_s));
        testing::MemorySecrets secrets;
        KvmdDevice::Pace kvmd;
        kvmd.pacing = std::make_shared<SignInPacing>();
        DriverPaces paces = DriverPaces::alike(kvmd);
        paces.vnc = pace;
        LocalWall wall(settings, secrets, paces);
        OwnKvm kvm;
        kvm.kind = u"vnc"_s;
        kvm.address = walled.url();
        const base::Result<QString> id = wall.add(kvm, u"secret"_s);
        QVERIFY(id.ok());
        std::optional<base::Result<Done>> wallSaid;
        wall.act(id.value(), Action::type(text, true),
                 [&wallSaid](base::Result<Done> how) { wallSaid = std::move(how); });
        QTRY_VERIFY_WITH_TIMEOUT(walled.keys.size() >= 4, 10000);
        wall.stopEveryTyping();
        QTRY_VERIFY(wallSaid.has_value());
        const base::Result<Done> wallStopped = wallSaid.value_or(Done{});
        QVERIFY(!wallStopped.ok());
        QCOMPARE(wallStopped.error().code, std::string("stopped"));
        QTest::qWait(300);
        QVERIFY(walled.keys.size() < 2 * static_cast<std::size_t>(text.size()));
        QVERIFY(std::ranges::none_of(walled.keys, [](const auto& key) { return key.first == 0xFF0D; }));
    }

    void itsScreenLiveWithItsKeysAndPointer()
    {
        SignInPacing pacing;
        VncByHand server;
        VncDevice device(at(server.url()), u"secret"_s, quick(pacing));
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
        const std::unique_ptr<OwnLive> own = device.makeLive();
        const std::unique_ptr<Live> live = own->open(Live::Need{});
        int pictures = 0;
        QObject::connect(live.get(), &Live::pictureCame, live.get(), [&pictures] { ++pictures; });
        QTRY_VERIFY_WITH_TIMEOUT(pictures > 0, 5000);
        live->askControl(true);
        QTRY_VERIFY_WITH_TIMEOUT(live->hasControl(), 5000);
        live->key(0x04, true); // a
        live->key(0x04, false);
        live->pointerAt(16384, 16384);
        QTRY_VERIFY(std::ranges::find(server.keys, std::pair<std::uint32_t, bool>{0x61, false}) != server.keys.end());
        QVERIFY(std::ranges::find(server.keys, std::pair<std::uint32_t, bool>{0x61, true}) != server.keys.end());
        QTRY_VERIFY(!server.pointers.empty());
        QVERIFY(std::abs(server.pointers.back().x - 80) <= 1 && std::abs(server.pointers.back().y - 50) <= 1);
        // Taken away: the live ends in those words.
        QSignalSpy closed(live.get(), &Live::closed);
        own->end(base::Error{"removed", "The VNC server was taken off the wall"});
        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(server.stillDown().empty());
    }

    void aVncServerOfTheSettingsIsOnTheWallWithItsSavedPassword()
    {
        // A profile holding a VNC server (as settings.json and Credential Manager keep one: SPEC section 11) opens
        // with it on this PC's own wall: its driver is a VNC server's, with the password saved for it, and its RFB
        // greeting is the asking (nothing else is asked at its address first).
        VncByHand server;
        const QTemporaryDir folder;
        OwnKvm kvm;
        kvm.id = u"5c0ffee1"_s;
        kvm.kind = u"vnc"_s;
        kvm.name = u"Build box"_s;
        kvm.address = server.url();
        kvm.askedWhatItIs = false;
        Remembered what;
        what.kvms = {kvm};
        what.kvmPasswordsKept = {kvm.id};
        Settings settings(folder.filePath(u"settings.json"_s));
        QVERIFY(settings.remember(what).ok());
        testing::MemorySecrets secrets;
        QVERIFY(secrets
                    .keep(u"kvm:5c0ffee1"_s,
                          "QKVM-KVM-2\n" + server.url().toUtf8().toPercentEncoding(":/[]") + "\nnone\nsecret")
                    .ok());
        KvmdDevice::Pace kvmd;
        kvmd.pacing = std::make_shared<SignInPacing>();
        LocalWall wall(settings, secrets, DriverPaces::alike(kvmd));
        QString state;
        for (int round = 0; round < 100 && state != u"online"_s; ++round) {
            const base::Result<QHash<QString, Status>> said = waitedFor<base::Result<QHash<QString, Status>>>(
                [&wall](auto done) { wall.askStatus(std::move(done)); }, 3000, base::Error{"never", "Not answered"});
            if (said.ok()) {
                state = said.value().value(kvm.id).state;
            }
            QTest::qWait(100);
        }
        QCOMPARE(state, u"online"_s);
        QCOMPARE(server.passwordTries, 1);
        QCOMPARE(server.rejected, 0);
    }

    void aVncServerIsAtItsOwnAddressAndNoKvmIsWhereItIs()
    {
        // Outside review M1: a VNC server is one place with another only where it is the very same address (the web
        // refuses a VNC server only where the same url is on the wall), and never one with a KVM of its host, which
        // may be the same machine's web page beside its VNC server (a PiKVM's kvmd-vnc).
        const auto at = [](const QString& address, const QString& kind) {
            OwnKvm kvm;
            kvm.address = address;
            kvm.kind = kind;
            return kvm;
        };
        QVERIFY(!LocalWall::atOnePlace(at(u"vnc://box.lan"_s, u"vnc"_s), at(u"https://box.lan"_s, u"kvmd"_s)));
        QVERIFY(!LocalWall::atOnePlace(at(u"https://box.lan"_s, u"kvmd"_s), at(u"vnc://box.lan"_s, u"vnc"_s)));
        QVERIFY(!LocalWall::atOnePlace(at(u"vnc://box.lan:443"_s, u"vnc"_s), at(u"https://box.lan"_s, u"kvmd"_s)));
        QVERIFY(!LocalWall::atOnePlace(at(u"vnc://box.lan"_s, u"vnc"_s), at(u"http://box.lan:5900"_s, u"kvmd"_s)));
        QVERIFY(!LocalWall::atOnePlace(at(u"vnc://box.lan"_s, u"vnc"_s), at(u"vncs://box.lan"_s, u"vnc"_s)));
        QVERIFY(!LocalWall::atOnePlace(at(u"vnc://box.lan"_s, u"vnc"_s), at(u"vnc://box.lan:5901"_s, u"vnc"_s)));
        QVERIFY(LocalWall::atOnePlace(at(u"vnc://box.lan:5901"_s, u"vnc"_s), at(u"vnc://box.lan:5901"_s, u"vnc"_s)));
        // The places a file of two thousand is laid out against: the same.
        QVERIFY(!LocalWall::atOnePlace(LocalWall::placeOf(at(u"vnc://box.lan"_s, u"vnc"_s)),
                                       LocalWall::placeOf(at(u"https://box.lan"_s, u"kvmd"_s))));
        QVERIFY(LocalWall::atOnePlace(LocalWall::placeOf(at(u"vncs://box.lan"_s, u"vnc"_s)),
                                      LocalWall::placeOf(at(u"vncs://box.lan"_s, u"vnc"_s))));
        // A wall that holds a VNC server at a host takes a KVM at that host.
        const QTemporaryDir folder;
        OwnKvm vncServer = at(u"vnc://127.0.0.1"_s, u"vnc"_s);
        vncServer.id = u"5c0ffee1"_s;
        Remembered what;
        what.kvms = {vncServer};
        Settings settings(folder.filePath(u"settings.json"_s));
        QVERIFY(settings.remember(what).ok());
        testing::MemorySecrets secrets;
        KvmdDevice::Pace kvmd;
        kvmd.pacing = std::make_shared<SignInPacing>();
        LocalWall wall(settings, secrets, DriverPaces::alike(kvmd));
        OwnKvm page = at(u"https://127.0.0.1"_s, u"kvmd"_s);
        const base::Result<QString> added = wall.add(page, u"a password"_s);
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(wall.kvms().size(), 2);
    }

    void aVncsServerOfTheWallHasItsCertificateKeptForTheWallsFile()
    {
        // Outside review M2: what its driver's line was let in by is kept beside its fingerprint, for the next file.
        VncByHand server(tlsOf(u"first"_s));
        const QTemporaryDir folder;
        OwnKvm kvm;
        kvm.id = u"5c0ffee1"_s;
        kvm.kind = u"vnc"_s;
        kvm.address = server.url();
        kvm.knownByCertificate = true;
        kvm.certificate = sha256Of(u"first"_s);
        kvm.askedWhatItIs = true;
        Remembered what;
        what.kvms = {kvm};
        what.kvmPasswordsKept = {kvm.id};
        Settings settings(folder.filePath(u"settings.json"_s));
        QVERIFY(settings.remember(what).ok());
        testing::MemorySecrets secrets;
        QVERIFY(secrets
                    .keep(u"kvm:5c0ffee1"_s, "QKVM-KVM-2\n" + server.url().toUtf8().toPercentEncoding(":/[]") +
                                                 "\nsha256:" + kvm.certificate.toHex() + "\nsecret")
                    .ok());
        KvmdDevice::Pace kvmd;
        kvmd.pacing = std::make_shared<SignInPacing>();
        DriverPaces paces = DriverPaces::alike(kvmd);
        LocalWall wall(settings, secrets, paces);
        for (int round = 0; round < 150 && settings.remembered().kvmCertificates.value(kvm.id).isEmpty(); ++round) {
            wall.askStatus([](const base::Result<QHash<QString, Status>>& /*all*/) {});
            QTest::qWait(100);
        }
        QVERIFY(settings.remembered().kvmCertificates.value(kvm.id).startsWith(u"-----BEGIN CERTIFICATE-----"_s));
    }

    void aServerThatDoesNotAnswerInTimeIsSaidAsTheWebSaysIt()
    {
        // Outside review L4: what takes a connection and never finishes TLS did not respond in time (the web's
        // _connect_error: "timeout"), where a port nobody listens at is unreachable.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        QList<QTcpSocket*> held;
        QObject::connect(&silent, &QTcpServer::newConnection, &silent, [&silent, &held] {
            while (QTcpSocket* line = silent.nextPendingConnection()) {
                held.append(line); // (taken, and never answered)
            }
        });
        SignInPacing pacing;
        VncDevice::Pace pace = quick(pacing);
        pace.screen.openingMs = 800;
        VncDevice device(at(u"vncs://127.0.0.1:%1"_s.arg(silent.serverPort())), u"secret"_s, pace);
        const Status said = lookUntil(device, [](const Status& one) { return one.state == u"offline"_s; });
        QCOMPARE(said.errorCode, u"timeout"_s);
        QCOMPARE(said.errorMessage, u"Device did not respond in time"_s);
    }

    void aVncServerOnTheWallHasNoConsolePageToOpen()
    {
        // Outside review M4: its address is no web page, and nothing hands vnc:// to Windows to open (the web shows a
        // console link of a VNC server's only where one was typed). Its card is a machine with a picture of its own.
        const QTemporaryDir folder;
        // (At ports where nothing listens, which Windows gives nobody else meanwhile, and not at 5901 and 5902, where a
        // VNC server of this PC's own may be: outside review c9a-3, L3.)
        const testing::NobodyThere one;
        const testing::NobodyThere other;
        OwnKvm vncServer;
        vncServer.id = u"5c0ffee1"_s;
        vncServer.kind = u"vnc"_s;
        vncServer.address = u"vncs://127.0.0.1:%1"_s.arg(one.port());
        OwnKvm inTheClear = vncServer;
        inTheClear.id = u"5c0ffee2"_s;
        inTheClear.address = u"vnc://127.0.0.1:%1"_s.arg(other.port());
        Remembered what;
        what.kvms = {vncServer, inTheClear};
        Settings settings(folder.filePath(u"settings.json"_s));
        QVERIFY(settings.remember(what).ok());
        testing::MemorySecrets secrets;
        KvmdDevice::Pace kvmd;
        kvmd.pacing = std::make_shared<SignInPacing>();
        LocalWall wall(settings, secrets, DriverPaces::alike(kvmd));
        const base::Result<Wall> asked = waitedFor<base::Result<Wall>>(
            [&wall](auto done) { wall.askWall(std::move(done)); }, 5000, base::Error{"never", "Not answered"});
        QVERIFY(asked.ok());
        QCOMPARE(asked.value().machines.size(), 2);
        for (const Machine& machine : asked.value().machines) {
            QVERIFY2(machine.page.isEmpty(), qPrintable(machine.page.toString()));
            QVERIFY(machine.hasPicture);
            // (Spoken to in the clear, as its card says of a KVM over plain HTTP: the vnc:// one alone.)
            QCOMPARE(machine.plain, machine.id == inTheClear.id);
        }
    }

    void overTlsTheCertificateItIsKnownByIsHeldAndAnotherIsSentNothing()
    {
        SignInPacing pacing;
        auto first = std::make_unique<VncByHand>(tlsOf(u"first"_s));
        const quint16 port = first->port();
        const QString url = first->url();
        QVERIFY(url.startsWith(u"vncs://"_s));
        VncDevice device({url, true, sha256Of(u"first"_s)}, u"secret"_s, quick(pacing));
        const QSignalSpy showed(&device, &OwnDevice::showedItsCertificate);
        QCOMPARE(lookUntil(device, [](const Status& one) { return one.state == u"online"_s; }).state, u"online"_s);
        QCOMPARE(first->passwordTries, 1);
        // The certificate itself it is known by, told once, for the wall as a file (outside review M2).
        QCOMPARE(showed.size(), 1);
        QCOMPARE(QCryptographicHash::hash(showed.first().first().toByteArray(), QCryptographicHash::Sha256),
                 sha256Of(u"first"_s));
        first.reset();
        VncByHand other(tlsOf(u"second"_s), port);
        const Status refused = lookUntil(device, [](const Status& one) { return one.errorCode == u"cert_mismatch"_s; });
        QCOMPARE(refused.errorCode, u"cert_mismatch"_s);
        QCOMPARE(refused.state, u"error"_s);
        QTest::qWait(300);
        QCOMPARE(other.passwordTries, 0);
        QCOMPARE(other.bytesHeard, qint64{0}); // (it never got as far as the protocol)
        QVERIFY(other.versions.empty());
    }

    void overTlsOnWindowsTrustACertificateItDoesNotTrustIsSentNothing()
    {
        // Outside review L5: a vncs:// server known by Windows' trust for its name (no certificate of its own saved),
        // whose certificate Windows does not trust: "cert_invalid", and not a byte of RFB, the password with it, goes.
        SignInPacing pacing;
        VncByHand server(tlsOf(u"first"_s));
        VncDevice device(at(server.url()), u"secret"_s, quick(pacing));
        const Status refused = lookUntil(device, [](const Status& one) { return one.errorCode == u"cert_invalid"_s; });
        QCOMPARE(refused.errorCode, u"cert_invalid"_s);
        QCOMPARE(refused.errorMessage, u"The device's certificate is not trusted"_s);
        QCOMPARE(refused.state, u"error"_s);
        QTest::qWait(300);
        QCOMPARE(server.passwordTries, 0);
        QCOMPARE(server.bytesHeard, qint64{0});
        QVERIFY(server.versions.empty());
    }

    void aServerThatSendsWhatIsNotTakenIsLeftAloneLongerEachTime()
    {
        // One that lets this program in and then sends what it does not take: left alone a rung each time, the count
        // begun anew at every handshake (the web's REFUSALS_COUNT_TILL_SETTLED False).
        SignInPacing pacing;
        VncByHand garbling;
        garbling.garbles = true;
        VncDevice garbled(at(garbling.url()), u"secret"_s, quick(pacing));
        const Status garbledSaid =
            lookUntil(garbled, [](const Status& one) { return one.errorCode == u"bad_response"_s; });
        QCOMPARE(garbledSaid.state, u"error"_s);
        QVERIFY2(garbledSaid.errorMessage.startsWith(u"The VNC server sent what qKVM does not take: "_s),
                 qPrintable(garbledSaid.errorMessage));
        // One that refuses before it lets this program in: twice as long each time, to its ceiling (the web's
        // _left_alone, 15 s to four minutes; here 0.2 s to 0.4, so that the ceiling shows, outside review L5: without
        // it, 0.2, 0.4, 0.8, 1.6, 3.2 and 6.4 s were some six connections in ten seconds).
        VncByHand server;
        server.offers = std::vector<std::uint8_t>{19, 16};
        VncDevice::Pace capped = quick(pacing);
        capped.badHoldMs = 200;
        capped.badHoldMostMs = 400;
        VncDevice device(at(server.url()), u"secret"_s, capped);
        const Status said = lookUntil(device, [](const Status& one) { return one.errorCode == u"bad_response"_s; });
        QCOMPARE(said.state, u"error"_s);
        QVERIFY2(said.errorMessage.startsWith(u"The VNC server does not let qKVM in: "_s),
                 qPrintable(said.errorMessage));
        // 0.2 s, then 0.4 and 0.4 (its ceiling here): some twenty connections in ten seconds, never one a look.
        const int before = server.connections;
        lookUntil(device, [](const Status& /*one*/) { return false; }); // (looked at for ten seconds)
        const int after = server.connections - before;
        QVERIFY2(after >= 12 && after <= 26, qPrintable(QString::number(after)));
    }

    void aServerThatEndsEveryConnectionIsSaidToDoSoAndAViewerHasItAtOnce()
    {
        // Outside review M3, the web's _cut_said and in_a_run: from its second end in a row on, a wait says "error"
        // with its sentence, and from its third on, so does the connection between two waits; never "offline", which
        // an alert would report as gone and back at every end. And whoever has it live during a wait has it at once.
        // (Outside review c9a-2, L6: the stand-in ends 1.5 s after its ServerInit, which this program hears some time
        // after that and is let in a little after it sent it, so its sentence says about 1 or 2 s; and a connection has
        // 1.2 s past its handshake to have served, settledMs 0.3 s, on a loaded PC. Fourteen seconds hold the third end
        // and the connection after it with room.)
        SignInPacing pacing;
        VncByHand server;
        server.endsAfterMs = 1500;
        VncDevice::Pace pace = quick(pacing);
        pace.cutLadderMs = {1500, 3000, 6000};
        VncDevice device(at(server.url()), u"secret"_s, pace);
        QElapsedTimer clock;
        clock.start();
        QStringList offlineLate;
        bool saidInAWait = false;
        bool saidWhileIn = false;
        const QRegularExpression said(u"^The VNC server ends every connection after about [12] s: "_s);
        while (clock.elapsed() < 14000) {
            device.look();
            device.askSnapshot(320, [](const base::Result<Snapshot>& /*made*/) {});
            QTest::qWait(100);
            const Status now = device.status();
            if (clock.elapsed() > 3000 && now.state == u"offline"_s) {
                offlineLate.append(now.errorMessage);
            }
            if (now.state == u"error"_s) {
                QVERIFY2(said.match(now.errorMessage).hasMatch(), qPrintable(now.errorMessage));
                (device.screen().letIn() ? saidWhileIn : saidInAWait) = true;
            }
        }
        QVERIFY2(offlineLate.isEmpty(), qPrintable(offlineLate.join(u" | "_s)));
        QVERIFY(saidInAWait);
        QVERIFY(saidWhileIn);
        // During a wait, a live has it at once, whatever the rung.
        QTRY_VERIFY_WITH_TIMEOUT(!device.screen().letIn() && device.status().state == u"error"_s, 10000);
        const int connections = server.connections;
        const std::unique_ptr<OwnLive> own = device.makeLive();
        const std::unique_ptr<Live> live = own->open(Live::Need{});
        QTRY_VERIFY_WITH_TIMEOUT(server.connections > connections, 1000);
    }

    void aServerThatEndsEveryConnectionIsConnectedToLessAndLessOften()
    {
        // Watched (its pictures asked for), each connection answers and lasts past `settledMs` (300 ms): it served. The
        // first end is connected to again at once; then after 0.5, 1 and 2 s, each wait the ladder's and no shorter
        // one of a session that failed. Each connection lasts a second (outside review 2.10: 400 ms against 300 left
        // a loaded runner a tenth of a second, which turned sessions that served into early failures): the waits
        // between them are measured, and waited for, not counted in a window of time.
        SignInPacing pacing;
        VncByHand server;
        server.endsAfterMs = 1000;
        VncDevice device(at(server.url()), u"secret"_s, quick(pacing));
        lookUntil(device, [&server](const Status& /*one*/) { return server.connections >= 5; }, true);
        lookUntil(device, [&server](const Status& /*one*/) { return server.connections >= 5; }, true);
        QVERIFY2(server.connections >= 5, qPrintable(QString::number(server.connections)));
        QTRY_VERIFY_WITH_TIMEOUT(server.endedAtMs.size() >= 4, 5000);
        // From each end to the next connection: at once, then the ladder's rungs (less a little for the clocks).
        const std::vector<qint64> rungs{0, 500, 1000, 2000};
        QString waits;
        for (std::size_t one = 0; one < rungs.size(); ++one) {
            waits += QString::number(server.connectedAtMs.at(one + 1) - server.endedAtMs.at(one)) + u" "_s;
        }
        for (std::size_t one = 0; one < rungs.size(); ++one) {
            const qint64 waited = server.connectedAtMs.at(one + 1) - server.endedAtMs.at(one);
            QVERIFY2(waited >= rungs.at(one) - 50, qPrintable(u"The waits: "_s + waits));
            if (one == 0) {
                QVERIFY2(waited < 400, qPrintable(u"The waits: "_s + waits)); // (at once: less than the first rung)
            }
        }
        // Each with its password: the fifth a moment after it was counted as a connection, as its line's socket's
        // thread speaks RFB to it a few turns of the two threads' loops later (outside review 2.2), which nothing above
        // waits for.
        QTRY_VERIFY_WITH_TIMEOUT(server.passwordTries >= 5, 3000);
    }
};

QTEST_GUILESS_MAIN(TestVncDevice)
#include "test_vnc_device.moc"
