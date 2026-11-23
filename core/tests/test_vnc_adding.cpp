#include "signer_far_away.h"
#include "vnc_by_hand.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QSslCertificate>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <optional>

#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/own_sign_in.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/vnc_device.h>

#include "memory_secrets.h"

// Adding a VNC server to this PC's own wall, changing one and reviewing its certificate, against VNC servers by hand on
// this PC's own address: the web's tests/test_vnc.py at the boxes' end (KvmAdding, KvmEditing, KvmCertificateReview).
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

// A VNC server's driver's times, made short.
VncDevice::Pace quickVnc()
{
    VncDevice::Pace pace;
    pace.screen.handshakeMs = 3000;
    pace.screen.openingMs = 3000;
    pace.screen.lookPaceMs = 50;
    pace.screen.idlePaceMs = 1000;
    pace.screen.watchedForMs = 500;
    pace.screen.tickMs = 20;
    pace.screen.goodbyeWithinMs = 500;
    pace.statusWaitMs = 2000;
    pace.freshMs = 100;
    pace.errorHoldMs = 100;
    pace.backoffMostMs = 400;
    pace.badHoldMs = 300;
    pace.badHoldMostMs = 1200;
    pace.noPasswordHoldMs = 2000;
    return pace;
}

// This PC's own wall for one test, its settings in a folder that goes with it, its secrets in memory, and the adding,
// changing and reviewing boxes' ends over it, all with one pacing of sign-ins.
struct Own {
    Own() : settings(folder.filePath(u"settings.json"_s)), wall(settings, secrets, wallPaces) {}

    static DriverPaces paces()
    {
        KvmdDevice::Pace kvmd;
        kvmd.pacing = std::make_shared<SignInPacing>();
        DriverPaces made = DriverPaces::alike(kvmd);
        made.vnc = quickVnc();
        made.vnc.pacing = kvmd.pacing;
        return made;
    }
    KvmAdding::Pace addingPace() const
    {
        KvmAdding::Pace pace;
        pace.driver = wallPaces.kvmd;
        pace.vnc = wallPaces.vnc;
        return pace;
    }

    QTemporaryDir folder;
    Settings settings;
    MemorySecrets secrets;
    DriverPaces wallPaces = paces();
    LocalWall wall;
};

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

base::Result<KvmAdding::Added> addedBy(KvmAdding& adding, KvmAdding::Typed typed)
{
    return waitedFor<base::Result<KvmAdding::Added>>(
        [&adding, &typed](auto done) { adding.add(std::move(typed), std::move(done)); }, 30000,
        base::Error{"never", "Not answered"});
}

KvmEditing::Outcome changedBy(KvmEditing& editing, const QString& id, KvmEditing::Typed typed)
{
    return waitedFor<KvmEditing::Outcome>(
        [&editing, &id, &typed](auto done) { editing.change(id, std::move(typed), std::move(done)); }, 30000,
        KvmEditing::Outcome{base::Error{"never", "Not answered"}, {}, {}, {}});
}

// What a review reads of a KVM of the wall, and what a yes to a certificate comes to, waited for.
base::Result<KvmCertificateReview::Shown> shownBy(KvmCertificateReview& review, const QString& id)
{
    return waitedFor<base::Result<KvmCertificateReview::Shown>>(
        [&review, &id](auto done) { review.askCertificates(id, std::move(done)); }, 10000,
        base::Error{"never", "Not answered"});
}

base::Result<void> trustedBy(KvmCertificateReview& review, const QString& id, const QByteArray& presented)
{
    return waitedFor<base::Result<void>>(
        [&review, &id, &presented](auto done) {
            review.trust(id, presented, [done = std::move(done)](const KvmCertificateReview::Outcome& outcome) {
                done(outcome.result);
            });
        },
        10000, base::Error{"never", "Not answered"});
}

// The KVM of the wall with this id, as it is now; one with nothing in it where there is none.
OwnKvm onTheWall(const LocalWall& wall, const QString& id)
{
    for (const OwnKvm& kvm : wall.kvms()) {
        if (kvm.id == id) {
            return kvm;
        }
    }
    return {};
}

KvmAdding::Typed typedAt(const QString& address, const QString& password, const QString& group = {})
{
    KvmAdding::Typed typed;
    typed.address = address;
    typed.password = password;
    typed.group = group;
    return typed;
}

// The wall's word on a machine, asked until it says `state` or ten seconds are over.
QString stateOn(LocalWall& wall, const QString& id, const QString& state)
{
    QString now;
    for (int round = 0; round < 100 && now != state; ++round) {
        const base::Result<QHash<QString, Status>> said = waitedFor<base::Result<QHash<QString, Status>>>(
            [&wall](auto done) { wall.askStatus(std::move(done)); }, 3000, base::Error{"never", "Not answered"});
        if (said.ok()) {
            now = said.value().value(id).state;
        }
        QTest::qWait(100);
    }
    return now;
}

base::Result<vnc::Certificate> certificateAt(const QString& address)
{
    // (The context goes with this frame: an answer after it is never given.)
    QObject context;
    return waitedFor<base::Result<vnc::Certificate>>(
        [&address, &context](auto done) { vnc::askCertificate(address, 3000, &context, std::move(done)); }, 6000,
        base::Error{"never", "Not answered"});
}

} // namespace

class TestVncAdding : public QObject {
    Q_OBJECT

private slots:
    // (Windows' own TLS, chosen before anything else uses TLS.)
    void initTestCase() { QVERIFY(useWindowsOwnTls()); }

    void aVncServerIsAddedByItsAddressAndNamedAfterWhatItCallsItself()
    {
        // The web's test_a_vnc_server_is_added_by_its_address_and_shows_its_screen.
        Own own;
        VncByHand server;
        KvmAdding adding(own.wall, own.addingPace());
        const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(server.url(), u"secret"_s, u"Lab"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(added.value().name, u"stand-in VNC"_s);
        QCOMPARE(added.value().model, u"stand-in VNC"_s);
        QCOMPARE(added.value().version, u"RFB 3.8"_s);
        QVERIFY(added.value().overPlainHttp); // (vnc:// is in the clear, and said so)
        QVERIFY(added.value().certificate.isEmpty());
        QCOMPARE(own.wall.kvms().size(), 1);
        const OwnKvm kept = own.wall.kvms().constFirst();
        QCOMPARE(kept.kind, u"vnc"_s);
        QCOMPARE(kept.address, server.url());
        QCOMPARE(kept.group, u"Lab"_s);
        QVERIFY(own.secrets.all.contains(u"kvm:"_s + kept.id));
        QCOMPARE(server.passwordTries, 1);
        // Its card: online, with the password that was saved for it.
        QCOMPARE(stateOn(own.wall, kept.id, u"online"_s), u"online"_s);
        QCOMPARE(server.rejected, 0);
        // Written another way, the same server is on the wall already, and asked nothing.
        const int connections = server.connections;
        const base::Result<KvmAdding::Added> again =
            addedBy(adding, typedAt(u"VNC://127.0.0.1:%1/"_s.arg(server.port()), u"secret"_s));
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, std::string("duplicate"));
        QTest::qWait(200);
        QCOMPARE(server.connections, connections);
    }

    void aVncServerIsNotAddedWithAWrongPasswordNorWithoutOneItAsksFor()
    {
        // The web's test_a_wrong_password_is_said_and_not_offered_again_and_again and
        // test_a_server_that_wants_a_password_nobody_saved_is_said_as_that, at the adding.
        Own own;
        VncByHand server;
        KvmAdding adding(own.wall, own.addingPace());
        const base::Result<KvmAdding::Added> wrong = addedBy(adding, typedAt(server.url(), u"wrong"_s));
        QVERIFY(!wrong.ok());
        QCOMPARE(wrong.error().code, std::string("auth_failed"));
        QVERIFY(!QString::fromStdString(wrong.error().message).contains(u"wrong"_s));
        QCOMPARE(server.passwordTries, 1);
        const base::Result<KvmAdding::Added> none = addedBy(adding, typedAt(server.url(), QString()));
        QVERIFY(!none.ok());
        QCOMPARE(none.error().code, std::string("no_password"));
        QCOMPARE(server.passwordTries, 1); // (nothing was sent for a password that is not there)
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
        // One that asks for none is added without one.
        VncByHand open;
        open.password.reset();
        const base::Result<KvmAdding::Added> without = addedBy(adding, typedAt(open.url(), QString()));
        QVERIFY2(without.ok(), without.ok() ? "" : without.error().message.c_str());
        QCOMPARE(own.wall.kvms().size(), 1);
    }

    void overTlsItsCertificateIsReadWithNothingSentAndItIsKnownByIt()
    {
        // The web's test_over_tls_the_certificate_is_remembered...: shown, before anything of RFB, and pinned.
        Own own;
        VncByHand server(tlsOf(u"first"_s));
        const base::Result<vnc::Certificate> shown = certificateAt(server.url());
        QVERIFY2(shown.ok(), shown.ok() ? "" : shown.error().message.c_str());
        QCOMPARE(shown.value().sha256, sha256Of(u"first"_s));
        QCOMPARE(QCryptographicHash::hash(shown.value().der, QCryptographicHash::Sha256), sha256Of(u"first"_s));
        QVERIFY(!shown.value().trustedByWindows);
        QTest::qWait(200);
        QCOMPARE(server.bytesHeard, qint64{0}); // (not a byte of RFB, nor anything else)
        QVERIFY(server.versions.empty());
        // Added, it is known by that certificate from then on, and said so.
        KvmAdding adding(own.wall, own.addingPace());
        const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(server.url(), u"secret"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QVERIFY(!added.value().overPlainHttp);
        QCOMPARE(added.value().certificate, sha256Of(u"first"_s));
        const OwnKvm kept = own.wall.kvms().constFirst();
        QVERIFY(kept.knownByCertificate);
        QCOMPARE(kept.certificate, sha256Of(u"first"_s));
        QVERIFY(kept.address.startsWith(u"vncs://"_s));
        QCOMPARE(stateOn(own.wall, kept.id, u"online"_s), u"online"_s);
    }

    void askingACertificateWhileWindowsFetchesItsSignerDoesNotHoldTheInterfaceThread()
    {
        // Outside review 2.2: what a vncs:// server shows is judged by Windows as TLS is spoken, and where its
        // certificate names a signer to be fetched from an address that does not answer (an AIA fetch on a PC
        // without internet), Windows fetches it on the thread that judges the line, and waits. The box asks for the
        // certificate on a thread of its own, within its time: the interface thread goes on all the while, and the
        // box is answered in its time, whatever Windows still waits for.
        // (This program's own certificate and ports: signer_far_away.h.)
        const auto& stalls = stallsOfAVncCertificate;
        const SignerFarAway far(stalls);
        QVERIFY2(far.isListening(), qPrintable(far.said()));
        const TlsDoorElsewhere door(tlsOf(QString::fromLatin1(stalls.name)));
        QVERIFY(door.speaksWindowsOwnTls());
        const QString address = u"vncs://127.0.0.1:%1"_s.arg(QUrl(door.address()).port());
        QObject context;
        const auto came = std::make_shared<std::optional<base::Result<vnc::Certificate>>>();
        const Stillness still;
        QElapsedTimer since;
        since.start();
        vnc::askCertificate(address, 3000, &context,
                            [came](base::Result<vnc::Certificate> shown) { *came = std::move(shown); });
        QVERIFY2(QTest::qWaitFor([&far] { return far.asked() > 0; }, 60'000),
                 qPrintable(u"Windows did not fetch the signer here for this program: "_s + far.said()));
        QVERIFY2(far.waitedOn() > 0, qPrintable(far.said()));
        QVERIFY(QTest::qWaitFor([came] { return came->has_value(); }, 10'000));
        const qint64 answeredMs = since.elapsed();
        QTest::qWait(1000);
        QVERIFY2(still.longestMs() < 1000,
                 qPrintable(
                     u"The interface thread was held still for %1 ms (%2)."_s.arg(still.longestMs()).arg(far.said())));
        QVERIFY2(answeredMs < 3000 + 1000, qPrintable(u"Answered after %1 ms"_s.arg(answeredMs)));
        // (Nothing was shown in time: said as a server that did not answer in time, and nothing is kept of it.)
        QCOMPARE(came->value_or(base::Error{}).ok() ? std::string("ok") : came->value_or(base::Error{}).error().code,
                 std::string("unreachable"));
        qInfo("%s", qPrintable(far.said()));
    }

    void aVncsSessionWhileWindowsFetchesItsSignerDoesNotHoldTheInterfaceThread()
    {
        // (Outside review 2.2's other half.) The same of a vncs:// server's session: its line's TLS is spoken where
        // its line's socket is, and what Windows judges of it is judged there. The session's line is a DeviceLine, as
        // every device's line is (a kvmd console's, a live link's, a server's console), whose socket is on a thread of
        // its own: on the interface thread, Windows held it for 17 s on the build box (2026-11-18).
        // (test_device_line_tls: every kind of line so.)
        const auto& stalls = stallsOfAVncSession;
        const SignerFarAway far(stalls);
        QVERIFY2(far.isListening(), qPrintable(far.said()));
        const TlsDoorElsewhere door(tlsOf(QString::fromLatin1(stalls.name)));
        QVERIFY(door.speaksWindowsOwnTls());
        const QString address = u"vncs://127.0.0.1:%1"_s.arg(QUrl(door.address()).port());
        VncDevice::Pace pace = quickVnc();
        pace.pacing = std::make_shared<SignInPacing>();
        VncDevice device({address, false, {}}, u"secret"_s, pace);
        const Stillness still;
        device.look();
        QVERIFY2(QTest::qWaitFor([&far] { return far.asked() > 0; }, 60'000),
                 qPrintable(u"Windows did not fetch the signer here for this program: "_s + far.said()));
        QVERIFY2(far.waitedOn() > 0, qPrintable(far.said()));
        QTest::qWait(6000);
        QVERIFY2(still.longestMs() < 1000,
                 qPrintable(
                     u"The interface thread was held still for %1 ms (%2)."_s.arg(still.longestMs()).arg(far.said())));
        qInfo("%s", qPrintable(far.said()));
    }

    void aVncServerIsChangedInItsBoxAndSignedInToWithAPasswordTypedForIt()
    {
        // The web's edit of a VNC server: its name alone is saved with nothing asked; a password typed for it signs in
        // where it is, held to what it is known by; a wrong one is said, and nothing is saved.
        Own own;
        VncByHand server;
        KvmAdding adding(own.wall, own.addingPace());
        const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(server.url(), u"secret"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        const QString id = own.wall.kvms().constFirst().id;
        KvmEditing editing(own.wall, own.addingPace());
        QTest::qWait(300);
        const int tries = server.passwordTries;
        KvmEditing::Typed renamed;
        renamed.name = u"Build guest"_s;
        renamed.address = server.url();
        const KvmEditing::Outcome named = changedBy(editing, id, renamed);
        QVERIFY2(named.result.ok(), named.result.ok() ? "" : named.result.error().message.c_str());
        QVERIFY(!named.result.value().signedIn);
        QCOMPARE(own.wall.kvms().constFirst().name, u"Build guest"_s);
        // (Written without vnc://, its address is a VNC server's all the same: the kind stays.)
        KvmEditing::Typed typed;
        typed.address = u"127.0.0.1:%1"_s.arg(server.port());
        typed.password = u"secret"_s;
        const KvmEditing::Outcome signedIn = changedBy(editing, id, typed);
        QVERIFY2(signedIn.result.ok(), signedIn.result.ok() ? "" : signedIn.result.error().message.c_str());
        QVERIFY(signedIn.result.value().signedIn);
        QVERIFY(signedIn.result.value().overPlainHttp);
        QCOMPARE(own.wall.kvms().constFirst().address, server.url());
        QCOMPARE(own.wall.kvms().constFirst().kind, u"vnc"_s);
        QVERIFY(server.passwordTries > tries);
        typed.password = u"wrong"_s;
        const KvmEditing::Outcome refused = changedBy(editing, id, typed);
        QVERIFY(!refused.result.ok());
        QCOMPARE(refused.result.error().code, std::string("auth_failed"));
        QVERIFY(!QString::fromStdString(refused.result.error().message).contains(u"wrong"_s));
        QCOMPARE(own.secrets.all.value(u"kvm:"_s + id).contains("wrong"), false);
    }

    void aKvmIsNotMadeAVncServerByAChange()
    {
        Own own;
        // (At ports where nothing listens, which Windows gives nobody else meanwhile: outside review c9a-3, L3.)
        const NobodyThere itsPlace;
        const NobodyThere elsewhere;
        OwnKvm kvm;
        kvm.address = u"https://127.0.0.1:%1"_s.arg(itsPlace.port());
        kvm.kind = u"kvmd"_s;
        const base::Result<QString> id = own.wall.add(kvm, u"secret"_s);
        QVERIFY(id.ok());
        KvmEditing editing(own.wall, own.addingPace());
        KvmEditing::Typed typed;
        typed.address = u"vnc://127.0.0.1:%1"_s.arg(elsewhere.port());
        typed.password = u"secret"_s;
        const KvmEditing::Outcome refused = changedBy(editing, id.value(), typed);
        QVERIFY(!refused.result.ok());
        QCOMPARE(refused.result.error().code, std::string("other_family"));
        QCOMPARE(own.wall.kvms().constFirst().address, kvm.address);
    }

    void aKvmsBoxToldOfAVncSchemeWrittenAmissSaysOnlyWhatThatBoxTakes()
    {
        // Outside review fin-1, A-L3: in a KVM's own box, a VNC server's scheme written amiss ("vnc:8443", a KVM at a
        // host called "vnc") is refused as everywhere, and not with the sentence of the box that adds one, whose first
        // half (write a VNC server's in full) this box refuses as another family's. Told only a KVM's way.
        Own own;
        const NobodyThere itsPlace;
        OwnKvm kvm;
        kvm.address = u"https://127.0.0.1:%1"_s.arg(itsPlace.port());
        kvm.kind = u"kvmd"_s;
        const base::Result<QString> id = own.wall.add(kvm, u"secret"_s);
        QVERIFY(id.ok());
        KvmEditing editing(own.wall, own.addingPace());
        for (const QString& typed : {u"vnc:8443"_s, u"vncs:/box.lan"_s, u"VNC//box.lan:8443"_s}) {
            KvmEditing::Typed asked;
            asked.address = typed;
            asked.password = u"secret"_s;
            const KvmEditing::Outcome refused = changedBy(editing, id.value(), asked);
            QVERIFY2(!refused.result.ok(), qPrintable(typed));
            QCOMPARE(refused.result.error().code, std::string("invalid"));
            QCOMPARE(QString::fromStdString(refused.result.error().message),
                     u"This begins as a VNC server's address does, with vnc or vncs, but not with vnc:// or vncs://. "
                     "A KVM at a host of that name is written with https:// before it."_s);
            QCOMPARE(own.wall.kvms().constFirst().address, kvm.address);
        }
    }

    void aVncServersKeptPasswordStaysAtItsAddressWhateverShowsElsewhere()
    {
        // Outside review c9a-2, M2 (the builder's difference 4, not taken), the web's known_again, which is for
        // https:// alone: a VNC server's kept password never goes untyped to another address, vnc:// or vncs://, not
        // even where the very certificate it is known by shows there. One certificate is often not one VNC server: a
        // TLS proxy before several displays of one host, each with a password of its own, or one certificate on every
        // board of a maker. Refused in the web's words, with nothing sent there; typed again, the password goes there.
        const QString typeIt =
            u"There is nothing to know this device by at another address. Type its password to use it there."_s;
        Own own;
        VncByHand plain;
        KvmAdding adding(own.wall, own.addingPace());
        QVERIFY(addedBy(adding, typedAt(plain.url(), u"secret"_s)).ok());
        const QString plainId = own.wall.kvms().constFirst().id;
        KvmEditing editing(own.wall, own.addingPace());
        VncByHand elsewhere;
        KvmEditing::Typed moved;
        moved.address = elsewhere.url();
        const KvmEditing::Outcome needsOne = changedBy(editing, plainId, moved);
        QVERIFY(!needsOne.result.ok());
        QCOMPARE(needsOne.result.error().code, std::string("password_needed"));
        QCOMPARE(QString::fromStdString(needsOne.result.error().message), typeIt);
        QCOMPARE(elsewhere.connections, 0);
        moved.password = u"secret"_s;
        const KvmEditing::Outcome withOne = changedBy(editing, plainId, moved);
        QVERIFY2(withOne.result.ok(), withOne.result.ok() ? "" : withOne.result.error().message.c_str());
        QCOMPARE(onTheWall(own.wall, plainId).address, elsewhere.url());

        auto first = std::make_unique<VncByHand>(tlsOf(u"first"_s));
        QVERIFY(addedBy(adding, typedAt(first->url(), u"secret"_s)).ok());
        const QString pinnedId = own.wall.kvms().constLast().id;
        QCOMPARE(onTheWall(own.wall, pinnedId).certificate, sha256Of(u"first"_s));
        // Another port, the same certificate (another display behind one proxy): refused, and nothing goes there, not
        // even TLS's hello. (Every line counted as its connection is accepted, before a byte of TLS: a server over TLS
        // hands one on only after its handshake, so a hello that was sent and dropped went uncounted by it. Outside
        // review 2.8.)
        VncEitherWay same(tlsOf(u"first"_s));
        VncEitherWay other(tlsOf(u"second"_s));
        for (const VncEitherWay* there : {&same, &other}) {
            KvmEditing::Typed untyped;
            untyped.address = there->url();
            const KvmEditing::Outcome stays = changedBy(editing, pinnedId, untyped);
            QVERIFY(!stays.result.ok());
            QCOMPARE(stays.result.error().code, std::string("password_needed"));
            QCOMPARE(QString::fromStdString(stays.result.error().message), typeIt);
        }
        QTest::qWait(300);
        QCOMPARE(same.connections, 0);
        QCOMPARE(other.connections, 0);
        QCOMPARE(onTheWall(own.wall, pinnedId).address, first->url());
        // The wall holds to that by itself, whoever asks it.
        OwnKvm walled = onTheWall(own.wall, pinnedId);
        walled.address = same.url();
        const base::Result<void> byTheWall = own.wall.change(pinnedId, walled, std::nullopt);
        QVERIFY(!byTheWall.ok());
        QCOMPARE(byTheWall.error().code, std::string("password_needed"));
        QCOMPARE(onTheWall(own.wall, pinnedId).address, first->url());
        // Typed, the password goes there, and it is known by that certificate still.
        KvmEditing::Typed typed;
        typed.address = same.url();
        typed.password = u"secret"_s;
        const KvmEditing::Outcome withItsPassword = changedBy(editing, pinnedId, typed);
        QVERIFY2(withItsPassword.result.ok(),
                 withItsPassword.result.ok() ? "" : withItsPassword.result.error().message.c_str());
        QCOMPARE(onTheWall(own.wall, pinnedId).address, same.url());
        QCOMPARE(onTheWall(own.wall, pinnedId).certificate, sha256Of(u"first"_s));
        QVERIFY(same.behind.passwordTries >= 1);
        QVERIFY(same.overTls >= 1);
    }

    void twoDisplaysBehindOneCertificateAreBothOnTheWallWhereSomebodySaysSo()
    {
        // The outside review of the wave 2 desktop, 6.5: an stunnel with one certificate before vncs://h:5901 and
        // vncs://h:5902. The second display was refused as "the same device" when added, changed or taken from a file,
        // with nothing to answer: a certificate's twin was proven, and kept beside nothing. The web knows a VNC server
        // by its pinned certificate too, and asks: "Keep both" answers it (core.identity, keep_both). So here: said to
        // be the same device, in the words a "Keep both" answers, and kept beside it where somebody says so; each
        // display with its own password.
        Own own;
        VncByHand five(tlsOf(u"first"_s));
        VncByHand six(tlsOf(u"first"_s));
        six.password = std::string("another");
        KvmAdding adding(own.wall, own.addingPace());
        QVERIFY(addedBy(adding, typedAt(five.url(), u"secret"_s)).ok());
        const QString fiveId = own.wall.kvms().constFirst().id;
        const base::Result<KvmAdding::Added> asked = addedBy(adding, typedAt(six.url(), u"another"_s));
        QVERIFY(!asked.ok());
        QCOMPARE(asked.error().code, std::string("same_device"));
        QVERIFY2(LocalWall::saysItIsTheSame(asked.error()), asked.error().message.c_str());
        QCOMPARE(six.passwordTries, 0);
        QCOMPARE(own.wall.kvms().size(), 1);
        KvmAdding::Typed both = typedAt(six.url(), u"another"_s);
        both.keepBoth = true;
        const base::Result<KvmAdding::Added> kept = addedBy(adding, both);
        QVERIFY2(kept.ok(), kept.ok() ? "" : kept.error().message.c_str());
        QCOMPARE(own.wall.kvms().size(), 2);
        const OwnKvm second = own.wall.kvms().constLast();
        QVERIFY(second.keptBesideItsTwin);
        QCOMPARE(second.certificate, sha256Of(u"first"_s));
        QVERIFY(!onTheWall(own.wall, fiveId).keptBesideItsTwin);
        QCOMPARE(stateOn(own.wall, fiveId, u"online"_s), u"online"_s);
        QCOMPARE(stateOn(own.wall, second.id, u"online"_s), u"online"_s);
        QCOMPARE(five.rejected, 0);
        QCOMPARE(six.rejected, 0);
        // The first's certificate trusted anew is no twin's either (the second stays beside it); and a third display
        // behind it is asked again.
        VncByHand seven(tlsOf(u"first"_s));
        QCOMPARE(addedBy(adding, typedAt(seven.url(), u"secret"_s)).error().code, std::string("same_device"));
        // (A KVM over HTTPS is the one that shows its certificate, and proven so, as ever.)
        OwnKvm https;
        https.address = u"https://127.0.0.1:9"_s;
        https.knownByCertificate = true;
        https.certificate = QByteArray(32, 'c');
        OwnKvm alsoHttps = https;
        alsoHttps.address = u"https://127.0.0.1:10"_s;
        QVERIFY(LocalWall::sameCertificate(https, alsoHttps));
        alsoHttps.keptBesideItsTwin = true;
        QVERIFY(LocalWall::twinOf({https}, alsoHttps).has_value());

        // From a file: taken where that file's wall kept both, and asked of where it did not.
        Own other;
        const LocalWall::AsAFile asAFile = own.wall.asAFile(true);
        const QList<exchange::Device> devices = asAFile.content.devices.value_or(QList<exchange::Device>());
        QCOMPARE(devices.size(), 2);
        QList<LocalWall::FromAFile> fromTheFile;
        for (const exchange::Device& device : devices) {
            LocalWall::FromAFile one;
            one.kvm.address = device.address;
            one.kvm.kind = device.kind;
            one.kvm.knownByCertificate = true;
            one.kvm.certificate = device.certificate;
            one.kvm.keptBesideItsTwin = device.keepBoth;
            one.password = device.password;
            fromTheFile.append(one);
        }
        QVERIFY(!fromTheFile.constFirst().kvm.keptBesideItsTwin && fromTheFile.constLast().kvm.keptBesideItsTwin);
        QList<LocalWall::FromAFile> notKept = fromTheFile;
        notKept.last().kvm.keptBesideItsTwin = false;
        const QList<base::Result<QString>> refused = other.wall.takeFromAFile(notKept);
        QCOMPARE(refused.size(), 2);
        QVERIFY(refused.constFirst().ok());
        QCOMPARE(refused.constLast().ok() ? std::string() : refused.constLast().error().code,
                 std::string("same_device"));
        QVERIFY(LocalWall::saysItIsTheSame(refused.constLast().error()));
        Own third;
        const QList<base::Result<QString>> taken = third.wall.takeFromAFile(fromTheFile);
        QCOMPARE(taken.size(), 2);
        QVERIFY(taken.constFirst().ok() && taken.constLast().ok());
        QCOMPARE(third.wall.kvms().size(), 2);
    }

    void aVncServerThatHasNoPasswordIsMovedWithNoneTyped()
    {
        // Outside review c9a-2, L3, the web's `record.secret is not None`: a VNC server with no password kept (its row
        // says "No password: fine if it runs without one") is moved with none typed: signed in to there with none,
        // and saved with none. Where what answers there asks for a password, that is said, and it stays where it is.
        Own own;
        VncByHand open;
        open.password.reset();
        KvmAdding adding(own.wall, own.addingPace());
        QVERIFY(addedBy(adding, typedAt(open.url(), QString())).ok());
        const QString id = own.wall.kvms().constFirst().id;
        QVERIFY(!own.wall.marksAPasswordFor(id));
        KvmEditing editing(own.wall, own.addingPace());
        VncByHand there;
        there.password.reset();
        KvmEditing::Typed moved;
        moved.address = there.url();
        const KvmEditing::Outcome came = changedBy(editing, id, moved);
        QVERIFY2(came.result.ok(), came.result.ok() ? "" : came.result.error().message.c_str());
        QVERIFY(came.result.value().signedIn);
        QCOMPARE(onTheWall(own.wall, id).address, there.url());
        QVERIFY(there.handshakes >= 1);
        QVERIFY(!own.wall.marksAPasswordFor(id));
        VncByHand guarded;
        moved.address = guarded.url();
        const KvmEditing::Outcome asks = changedBy(editing, id, moved);
        QVERIFY(!asks.result.ok());
        QCOMPARE(asks.result.error().code, std::string("no_password"));
        QCOMPARE(guarded.passwordTries, 0);
        QCOMPARE(onTheWall(own.wall, id).address, there.url());
    }

    void aVncsServerMovedWithAPasswordTypedIsKnownOtherwiseThereOnlyAfterAYes()
    {
        // As a KVM over HTTPS: moved with a password typed to where another certificate shows, it is put before
        // somebody with nothing sent there; with a yes to that very one, signed in to there and known by it.
        Own own;
        VncByHand first(tlsOf(u"first"_s));
        KvmAdding adding(own.wall, own.addingPace());
        QVERIFY(addedBy(adding, typedAt(first.url(), u"secret"_s)).ok());
        const QString id = own.wall.kvms().constFirst().id;
        VncByHand second(tlsOf(u"second"_s));
        KvmEditing editing(own.wall, own.addingPace());
        KvmEditing::Typed moved;
        moved.address = second.url();
        moved.password = u"secret"_s;
        const KvmEditing::Outcome asked = changedBy(editing, id, moved);
        QVERIFY(!asked.result.ok());
        QCOMPARE(asked.result.error().code, std::string("certificate_differs"));
        QCOMPARE(asked.remembered, sha256Of(u"first"_s));
        QCOMPARE(asked.presented, sha256Of(u"second"_s));
        QVERIFY(!asked.onWindowsWord);
        QTest::qWait(200);
        QCOMPARE(second.bytesHeard, qint64{0});
        QCOMPARE(onTheWall(own.wall, id).address, first.url());
        moved.saidYesTo = asked.presented;
        const KvmEditing::Outcome yes = changedBy(editing, id, moved);
        QVERIFY2(yes.result.ok(), yes.result.ok() ? "" : yes.result.error().message.c_str());
        QVERIFY(yes.result.value().signedIn);
        QCOMPARE(yes.result.value().certificate, sha256Of(u"second"_s));
        QCOMPARE(onTheWall(own.wall, id).address, second.url());
        QCOMPARE(onTheWall(own.wall, id).certificate, sha256Of(u"second"_s));
        QVERIFY(second.passwordTries >= 1);
    }

    void aYesToACertificateIsForVncsAloneAndVncsIsNotTurnedIntoVncAtItsPlace()
    {
        // Outside review c9a-2, H1. A vncs:// server is not changed to vnc:// at its very host and port, with its
        // password typed or written without vnc:// (which a VNC server's address is read as): refused as a KVM is from
        // HTTPS to plain HTTP at its place, and nothing goes there. And a yes to a certificate is one for a vncs://
        // address alone: with one, a vnc:// address elsewhere is refused with nothing sent there, whatever it names.
        Own own;
        VncByHand atItsPlace;
        OwnKvm kvm;
        kvm.kind = u"vnc"_s;
        kvm.address = u"vncs://127.0.0.1:%1"_s.arg(atItsPlace.port());
        kvm.knownByCertificate = true;
        kvm.certificate = QByteArray(32, 'c');
        const base::Result<QString> id = own.wall.add(kvm, u"secret"_s);
        QVERIFY(id.ok());
        KvmEditing editing(own.wall, own.addingPace());
        for (const QString& typed : {atItsPlace.url(), u"127.0.0.1:%1"_s.arg(atItsPlace.port())}) {
            KvmEditing::Typed clear;
            clear.address = typed;
            clear.password = u"secret"_s;
            const KvmEditing::Outcome refused = changedBy(editing, id.value(), clear);
            QVERIFY(!refused.result.ok());
            QCOMPARE(refused.result.error().code, std::string("not_in_the_clear"));
            // (In its family's words: VNC authentication sends no password, but an answer it can be worked out from,
            // outside review c9a-3, L2.)
            const QString said = QString::fromStdString(refused.result.error().message);
            QVERIFY(said.contains(u"vncs://"_s));
            QVERIFY2(said.contains(u"its password worked out from what goes"_s), qPrintable(said));
            QVERIFY2(!said.contains(u"travel unencrypted"_s), qPrintable(said));
        }
        VncByHand elsewhere;
        KvmEditing::Typed yes;
        yes.address = elsewhere.url();
        yes.password = u"secret"_s;
        yes.saidYesTo = sha256Of(u"second"_s);
        const KvmEditing::Outcome noYesThere = changedBy(editing, id.value(), yes);
        QVERIFY(!noYesThere.result.ok());
        QCOMPARE(noYesThere.result.error().code, std::string("no_certificate"));
        QTest::qWait(300);
        QCOMPARE(atItsPlace.connections, 0);
        QCOMPARE(elsewhere.connections, 0);
        QCOMPARE(onTheWall(own.wall, id.value()).address, kvm.address);
        // (Nor is vnc:// at another port than its place a way round that: it is asked first, outside review c9a-3, M1.)
        yes.saidYesTo.clear();
        const KvmEditing::Outcome asked = changedBy(editing, id.value(), yes);
        QVERIFY(!asked.result.ok());
        QCOMPARE(asked.result.error().code, std::string("plain_http_there"));
        QTest::qWait(300);
        QCOMPARE(elsewhere.connections, 0);
        QCOMPARE(onTheWall(own.wall, id.value()).address, kvm.address);
    }

    void aVncsServerMovedIntoTheClearElsewhereGoesThereOnlyWithAYesToThatVeryAddress()
    {
        // Outside review c9a-3, M1, as the coordinator decided: the desktop's own KVM rule (from HTTPS to plain HTTP at
        // another place, a yes first), stricter than the web, which makes this a change like any other. A vncs://
        // server moved to vnc:// at another host or port, or to its own machine written another way, which its place
        // cannot see, with its password typed: nothing goes there, and it is handed back what it is known by and that
        // address whole, vnc:// and all, to be shown. A bare address is a vnc:// one. The yes counts for that very
        // address and no other; with it, the server is signed in to there in the clear, and saved so.
        Own own;
        VncByHand atItsPlace; // (what a line in the clear to its own host and port would reach)
        OwnKvm kvm;
        kvm.kind = u"vnc"_s;
        kvm.address = u"vncs://127.0.0.1:%1"_s.arg(atItsPlace.port());
        kvm.knownByCertificate = true;
        kvm.certificate = QByteArray(32, 'c');
        const base::Result<QString> id = own.wall.add(kvm, u"secret"_s);
        QVERIFY(id.ok());
        KvmEditing editing(own.wall, own.addingPace());
        VncByHand elsewhere;
        VncByHand other;
        const QString there = elsewhere.url();
        const auto movedTo = [&editing, &id](const QString& typed, const QString& yesAt, const QString& password) {
            KvmEditing::Typed moved;
            moved.address = typed;
            moved.password = password;
            moved.saidYesToPlainHttpAt = yesAt;
            return changedBy(editing, id.value(), moved);
        };
        struct Row {
            QString typed;
            QString yesAt;
        };
        const QString itsPort = QString::number(atItsPlace.port());
        const QList<Row> rows{
            {there, {}},                                            // another port
            {u"127.0.0.1:%1"_s.arg(elsewhere.port()), {}},          // bare, which is vnc://
            {u"VNC://LocalHost.:"_s + itsPort, {}},                 // its own machine, by name
            {u"[::ffff:127.0.0.1]:"_s + itsPort, {}},               // its own machine, by IPv6
            {other.url(), there},                                   // a yes for another port
            {u"vnc://localhost:%1"_s.arg(elsewhere.port()), there}, // a yes for another host
            {u"vnc://localhost:%1"_s.arg(elsewhere.port()), u"localhost:%1"_s.arg(elsewhere.port())}, // a yes as typed
        };
        for (const Row& row : rows) {
            const KvmEditing::Outcome came = movedTo(row.typed, row.yesAt, u"secret"_s);
            QVERIFY2(!came.result.ok(), qPrintable(row.typed));
            QCOMPARE(came.result.error().code, std::string("plain_http_there"));
            const base::Result<QString> whole = vnc::addressFrom(row.typed);
            QVERIFY(whole.ok());
            QVERIFY(whole.value().startsWith(u"vnc://"_s));
            QCOMPARE(came.presentedAt, whole.value());
            QCOMPARE(came.remembered, kvm.certificate);
            QVERIFY(came.presented.isEmpty());
            // In VNC's words, not plain HTTP's.
            const QString said = QString::fromStdString(came.result.error().message);
            QVERIFY2(said.contains(u"vnc://"_s) && !said.contains(u"HTTP"_s), qPrintable(said));
        }
        QCOMPARE(movedTo(u"127.0.0.1:%1"_s.arg(elsewhere.port()), {}, u"secret"_s).presentedAt, there);
        QTest::qWait(300);
        QCOMPARE(atItsPlace.connections, 0);
        QCOMPARE(elsewhere.connections, 0);
        QCOMPARE(other.connections, 0);
        QCOMPARE(onTheWall(own.wall, id.value()).address, kvm.address);
        QVERIFY(onTheWall(own.wall, id.value()).knownByCertificate);
        // With the yes to that very address (typed bare, as it is read): signed in to there in the clear, and saved so,
        // known by nothing.
        const KvmEditing::Outcome yes = movedTo(u"127.0.0.1:%1"_s.arg(elsewhere.port()), there, u"secret"_s);
        QVERIFY2(yes.result.ok(), yes.result.ok() ? "" : yes.result.error().message.c_str());
        QVERIFY(yes.result.value().signedIn);
        QVERIFY(yes.result.value().overPlainHttp);
        QVERIFY(yes.result.value().certificate.isEmpty());
        QCOMPARE(onTheWall(own.wall, id.value()).address, there);
        QVERIFY(!onTheWall(own.wall, id.value()).knownByCertificate);
        QVERIFY(elsewhere.passwordTries >= 1);
        QCOMPARE(other.connections, 0);
        QCOMPARE(atItsPlace.connections, 0);

        // One with no password kept, moved with none typed (outside review c9a-2, L3), asks the same, and goes with the
        // yes, with none.
        VncByHand open;
        open.password.reset();
        OwnKvm none = kvm;
        none.address = u"vncs://127.0.0.1:%1"_s.arg(other.port());
        const base::Result<QString> noneId = own.wall.add(none, QString());
        QVERIFY(noneId.ok());
        QVERIFY(!own.wall.marksAPasswordFor(noneId.value()));
        KvmEditing::Typed withNone;
        withNone.address = open.url();
        const KvmEditing::Outcome noneAsked = changedBy(editing, noneId.value(), withNone);
        QVERIFY(!noneAsked.result.ok());
        QCOMPARE(noneAsked.result.error().code, std::string("plain_http_there"));
        QCOMPARE(noneAsked.presentedAt, open.url());
        QTest::qWait(300);
        QCOMPARE(open.connections, 0);
        withNone.saidYesToPlainHttpAt = open.url();
        const KvmEditing::Outcome noneMoved = changedBy(editing, noneId.value(), withNone);
        QVERIFY2(noneMoved.result.ok(), noneMoved.result.ok() ? "" : noneMoved.result.error().message.c_str());
        QCOMPARE(onTheWall(own.wall, noneId.value()).address, open.url());
        QVERIFY(open.handshakes >= 1);
        QVERIFY(!own.wall.marksAPasswordFor(noneId.value()));
        QCOMPARE(other.connections, 0);
    }

    void theStandInAtOnePortTellsTlsFromTheClearByTheFirstByteAndCountsEveryLine()
    {
        // Outside review c9a-3, L3: VncEitherWay (vnc_by_hand.h), the settings screens' stand-in at one port for a VNC
        // server's yes, tells a line by its caller's first byte and by no timer. A caller in the clear is counted
        // whether it ends at once, speaks, or waits and ends later, and nothing greets it; TLS that begins after a
        // silence longer than the old stand-in's 400 ms is TLS, and reaches the server over TLS behind it.
        VncEitherWay there(tlsOf(u"second"_s));
        {
            // (Ended as soon as it is accepted, and not before: whether Windows still hands over a line that ended
            // before it was accepted is Windows' own, outside review c9a-4, L4.)
            QTcpSocket once;
            once.connectToHost(QHostAddress::LocalHost, there.port());
            QVERIFY(once.waitForConnected(5000));
            QTRY_COMPARE(there.connections, 1);
            once.abort();
        }
        QTRY_COMPARE(there.inTheClear, 1);
        QTcpSocket speaks;
        speaks.connectToHost(QHostAddress::LocalHost, there.port());
        QVERIFY(speaks.waitForConnected(5000));
        speaks.write("RFB 003.008\n");
        QTRY_COMPARE(there.inTheClear, 2);
        QTRY_COMPARE(there.clearBytesHeard, qint64{12});
        QTcpSocket silent;
        silent.connectToHost(QHostAddress::LocalHost, there.port());
        QVERIFY(silent.waitForConnected(5000));
        QTRY_COMPARE(there.connections, 3);
        QTest::qWait(1500);
        QCOMPARE(there.undecided(), 1);
        QCOMPARE(silent.bytesAvailable(), qint64{0}); // (greeted by nobody)
        silent.abort();
        QTRY_COMPARE(there.inTheClear, 3);
        QSslSocket late;
        late.setPeerVerifyMode(QSslSocket::VerifyNone);
        late.connectToHost(QHostAddress::LocalHost, there.port());
        QVERIFY(late.waitForConnected(5000));
        QTest::qWait(1500);
        late.startClientEncryption();
        QTRY_VERIFY_WITH_TIMEOUT(late.isEncrypted(), 10000);
        QCOMPARE(late.peerCertificate().digest(QCryptographicHash::Sha256), sha256Of(u"second"_s));
        QCOMPARE(there.overTls, 1);
        QCOMPARE(there.connections, 4);
        QCOMPARE(there.inTheClear, 3);
        QCOMPARE(there.undecided(), 0);
    }

    void aVncsServersCertificateIsLookedAtFromItsBoxAndTrustedAgain()
    {
        // The web's review of a certificate, at a vncs:// server's own host and port: read with nothing sent, shown
        // beside the one it is known by, and trusted from then on with a yes.
        Own own;
        auto first = std::make_unique<VncByHand>(tlsOf(u"first"_s));
        const quint16 port = first->port();
        KvmAdding adding(own.wall, own.addingPace());
        QVERIFY(addedBy(adding, typedAt(first->url(), u"secret"_s)).ok());
        const QString id = own.wall.kvms().constFirst().id;
        first.reset();
        VncByHand second(tlsOf(u"second"_s), port);
        KvmCertificateReview review(own.wall);
        const base::Result<KvmCertificateReview::Shown> shown = shownBy(review, id);
        QVERIFY2(shown.ok(), shown.ok() ? "" : shown.error().message.c_str());
        QCOMPARE(shown.value().remembered, sha256Of(u"first"_s));
        QCOMPARE(shown.value().presented, sha256Of(u"second"_s));
        QTest::qWait(200);
        QCOMPARE(second.passwordTries, 0);
        const base::Result<void> trusted = trustedBy(review, id, sha256Of(u"second"_s));
        QVERIFY2(trusted.ok(), trusted.ok() ? "" : trusted.error().message.c_str());
        QCOMPARE(onTheWall(own.wall, id).certificate, sha256Of(u"second"_s));
        QCOMPARE(stateOn(own.wall, id, u"online"_s), u"online"_s);
        // A plain VNC server shows none.
        VncByHand plain;
        QVERIFY(addedBy(adding, typedAt(plain.url(), u"secret"_s)).ok());
        const base::Result<KvmCertificateReview::Shown> none = shownBy(review, own.wall.kvms().constLast().id);
        QVERIFY(!none.ok());
        QCOMPARE(none.error().code, std::string("no_certificate"));
    }

    void aYesNeverTakesTheCertificateAnotherCardIsKnownByWhateverIsKeptBeside()
    {
        // Review cx6-1, M1, a side effect of 6.5: where either card was kept beside its twin, a yes to a new
        // certificate took a VNC card's kept password onto a line that showed another card's pinned certificate, and
        // the box did not say whose it was. Host h runs a TLS proxy before two displays (A, and D kept beside it), host
        // h2 one before B; DHCP swaps the two hosts' addresses. "Keep both" answers "are these two the same device?",
        // and never "may this card's password go to that card's certificate?": the box names the other card, the yes
        // is refused by the box's end and by the wall, and no password goes to a display that is not its card's.
        Own own;
        VncByHand five(tlsOf(u"first"_s));
        auto six = std::make_unique<VncByHand>(tlsOf(u"first"_s));
        six->password = std::string("another");
        auto other = std::make_unique<VncByHand>(tlsOf(u"second"_s));
        other->password = std::string("a third");
        KvmAdding adding(own.wall, own.addingPace());
        QVERIFY(addedBy(adding, typedAt(five.url(), u"secret"_s)).ok());
        const QString fiveId = own.wall.kvms().constLast().id;
        KvmAdding::Typed both = typedAt(six->url(), u"another"_s);
        both.keepBoth = true;
        QVERIFY(addedBy(adding, both).ok());
        const QString sixId = own.wall.kvms().constLast().id;
        QVERIFY(onTheWall(own.wall, sixId).keptBesideItsTwin);
        QVERIFY(addedBy(adding, typedAt(other->url(), u"a third"_s)).ok());
        const QString otherId = own.wall.kvms().constLast().id;
        QVERIFY(!onTheWall(own.wall, otherId).keptBesideItsTwin);
        // The swap: D's address reaches B's display, and B's address reaches D's.
        const quint16 sixPort = six->port();
        const quint16 otherPort = other->port();
        six.reset();
        other.reset();
        VncByHand bAtSix(tlsOf(u"second"_s), sixPort);
        bAtSix.password = std::string("a third");
        VncByHand dAtOther(tlsOf(u"first"_s), otherPort);
        dAtOther.password = std::string("another");
        KvmCertificateReview review(own.wall);
        struct Row {
            QString id;
            QByteArray presented;
            QString isOf;
        };
        const auto refusedEverywhere = [&](const Row& row) {
            const base::Result<KvmCertificateReview::Shown> shown = shownBy(review, row.id);
            QVERIFY2(shown.ok(), shown.ok() ? "" : shown.error().message.c_str());
            QCOMPARE(shown.value().presented, row.presented);
            QCOMPARE(shown.value().presentedIsOf, row.isOf);
            // (By what a VNC server's certificate says, not proven: review cx6-2, L2.)
            QVERIFY(shown.value().presentedSaysSo);
            const base::Result<void> yes = trustedBy(review, row.id, row.presented);
            QVERIFY(!yes.ok());
            QCOMPARE(yes.error().code, std::string("same_device"));
            // (The wall's own rule, whoever asks it.)
            const base::Result<void> byTheWall = own.wall.knowBy(row.id, row.presented);
            QVERIFY(!byTheWall.ok());
            QCOMPARE(byTheWall.error().code, std::string("same_device"));
            QVERIFY(onTheWall(own.wall, row.id).certificate != row.presented);
        };
        // D, kept beside A, is shown B's certificate; B, kept beside nothing, is shown A's and D's.
        refusedEverywhere({sixId, sha256Of(u"second"_s), otherId});
        refusedEverywhere({otherId, sha256Of(u"first"_s), fiveId});
        // The other way round: once A is gone, D alone is known by its certificate, and D is kept beside: B is not
        // taken onto it either.
        QVERIFY(own.wall.remove(fiveId).ok());
        refusedEverywhere({otherId, sha256Of(u"first"_s), sixId});
        QTest::qWait(1500);
        QCOMPARE(bAtSix.passwordTries, 0);
        QCOMPARE(dAtOther.passwordTries, 0);

        // The Change box: D moved, with its password typed, to where B's certificate shows is said to be B, whatever it
        // was kept beside before, and nothing goes there; kept both there, and its certificate said yes to, it goes.
        // (Review cx6-1, L5: "Keep both" in the Change box for a VNC server behind another's certificate.)
        VncByHand elsewhere(tlsOf(u"second"_s));
        elsewhere.password = std::string("another");
        KvmEditing editing(own.wall, own.addingPace());
        KvmEditing::Typed moved;
        moved.address = elsewhere.url();
        moved.password = u"another"_s;
        const KvmEditing::Outcome asked = changedBy(editing, sixId, moved);
        QVERIFY(!asked.result.ok());
        QCOMPARE(asked.result.error().code, std::string("same_device"));
        QVERIFY2(LocalWall::saysItIsTheSame(asked.result.error()), asked.result.error().message.c_str());
        QTest::qWait(200);
        QCOMPARE(elsewhere.passwordTries, 0);
        QCOMPARE(onTheWall(own.wall, sixId).address, u"vncs://127.0.0.1:%1"_s.arg(sixPort));
        moved.keepBoth = true;
        moved.saidYesTo = sha256Of(u"second"_s);
        const KvmEditing::Outcome kept = changedBy(editing, sixId, moved);
        QVERIFY2(kept.result.ok(), kept.result.ok() ? "" : kept.result.error().message.c_str());
        QCOMPARE(onTheWall(own.wall, sixId).address, elsewhere.url());
        QCOMPARE(onTheWall(own.wall, sixId).certificate, sha256Of(u"second"_s));
        QVERIFY(onTheWall(own.wall, sixId).keptBesideItsTwin);
        QVERIFY(elsewhere.passwordTries >= 1);
        QCOMPARE(elsewhere.rejected, 0);
    }

    void aCardThatComesWhereOnlyACardKeptBesideItsTwinHoldsTheCertificateIsNamedAndAskedOfAsATwinIs()
    {
        // Review cx6-2, L1, M1's other half: A and D behind one TLS proxy, D kept beside A; A removed, so that D alone
        // is known by that certificate. The Add box and the Change box passed D over as anybody's twin (it was kept
        // beside one): another VNC card went onto that certificate, unnamed and unasked, with the password typed
        // there. Now D is named, as a twin is, and nothing goes there without "Keep both" said in that very box.
        Own own;
        VncByHand five(tlsOf(u"first"_s));
        VncByHand six(tlsOf(u"first"_s));
        six.password = std::string("another");
        KvmAdding adding(own.wall, own.addingPace());
        QVERIFY(addedBy(adding, typedAt(five.url(), u"secret"_s)).ok());
        const QString fiveId = own.wall.kvms().constLast().id;
        KvmAdding::Typed both = typedAt(six.url(), u"another"_s);
        both.keepBoth = true;
        QVERIFY(addedBy(adding, both).ok());
        const QString sixId = own.wall.kvms().constLast().id;
        QVERIFY(onTheWall(own.wall, sixId).keptBesideItsTwin);
        QVERIFY(own.wall.remove(fiveId).ok());
        const QString sixAt = u"127.0.0.1:%1"_s.arg(six.port());
        // The Add box: a third display behind that certificate.
        VncByHand seven(tlsOf(u"first"_s));
        seven.password = std::string("a third");
        const base::Result<KvmAdding::Added> asked = addedBy(adding, typedAt(seven.url(), u"a third"_s));
        QVERIFY(!asked.ok());
        QCOMPARE(asked.error().code, std::string("same_device"));
        QVERIFY2(LocalWall::saysItIsTheSame(asked.error()), asked.error().message.c_str());
        QVERIFY2(QString::fromStdString(asked.error().message).contains(sixAt), asked.error().message.c_str());
        QTest::qWait(200);
        QCOMPARE(seven.passwordTries, 0);
        QCOMPARE(own.wall.kvms().size(), 1);
        KvmAdding::Typed keep = typedAt(seven.url(), u"a third"_s);
        keep.keepBoth = true;
        const base::Result<KvmAdding::Added> kept = addedBy(adding, keep);
        QVERIFY2(kept.ok(), kept.ok() ? "" : kept.error().message.c_str());
        const QString sevenId = own.wall.kvms().constLast().id;
        QVERIFY(onTheWall(own.wall, sevenId).keptBesideItsTwin);
        QCOMPARE(onTheWall(own.wall, sevenId).certificate, sha256Of(u"first"_s));
        // The Change box: a card of another certificate moved, with its password typed, to where that certificate
        // shows (a typo of a port, or a display by another name): named, and nothing goes there, until "Keep both".
        VncByHand other(tlsOf(u"second"_s));
        other.password = std::string("a fourth");
        QVERIFY(addedBy(adding, typedAt(other.url(), u"a fourth"_s)).ok());
        const QString otherId = own.wall.kvms().constLast().id;
        VncByHand eighth(tlsOf(u"first"_s));
        eighth.password = std::string("a fourth");
        KvmEditing editing(own.wall, own.addingPace());
        KvmEditing::Typed moved;
        moved.address = eighth.url();
        moved.password = u"a fourth"_s;
        const KvmEditing::Outcome refused = changedBy(editing, otherId, moved);
        QVERIFY(!refused.result.ok());
        QCOMPARE(refused.result.error().code, std::string("same_device"));
        QVERIFY2(LocalWall::saysItIsTheSame(refused.result.error()), refused.result.error().message.c_str());
        QTest::qWait(200);
        QCOMPARE(eighth.passwordTries, 0);
        QCOMPARE(onTheWall(own.wall, otherId).address, other.url());
        QCOMPARE(onTheWall(own.wall, otherId).certificate, sha256Of(u"second"_s));
        moved.keepBoth = true;
        moved.saidYesTo = sha256Of(u"first"_s);
        const KvmEditing::Outcome keptThere = changedBy(editing, otherId, moved);
        QVERIFY2(keptThere.result.ok(), keptThere.result.ok() ? "" : keptThere.result.error().message.c_str());
        QCOMPARE(onTheWall(own.wall, otherId).address, eighth.url());
        QVERIFY(onTheWall(own.wall, otherId).keptBesideItsTwin);
        QVERIFY(eighth.passwordTries >= 1);
    }
};

QTEST_GUILESS_MAIN(TestVncAdding)
#include "test_vnc_adding.moc"
