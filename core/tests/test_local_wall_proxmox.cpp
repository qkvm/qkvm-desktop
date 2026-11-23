#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/own_sign_in.h>
#include <qkvm/core/proxmox_adding.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wall_exchange.h>

#include "memory_secrets.h"
#include "proxmox_by_hand.h"
#include "proxmox_device_tests.h"
#include "stand_in.h"
#include "vnc_by_hand.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
namespace core = qkvm::core;
using namespace qkvm::core::testing;
using core::LocalWall;
using core::OwnKvm;
using core::ProxmoxAdding;
using Guest = ProxmoxByHand::Guest;

namespace {

core::DriverPaces quickPaces()
{
    core::DriverPaces paces;
    paces.proxmox = quickProxmox();
    paces.proxmox.places = std::make_shared<core::ProxmoxDevice::Places>();
    paces.proxmox.confirmEveryMs = 50;
    paces.proxmox.followUpMs.clear();
    return paces;
}

template <typename Answer>
base::Result<Answer> waitedFor(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
{
    return answerOf<Answer>(ask);
}

// The edit box's times: short, and a pacing of the test's own.
core::KvmEditing::Pace editingPace()
{
    core::KvmEditing::Pace pace;
    pace.driver.connectWithinMs = 1500;
    pace.driver.quietMs = 2000;
    pace.driver.transferMs = 2000;
    pace.driver.holdMs = 200;
    pace.driver.longestHoldMs = 800;
    pace.driver.freshMs = 0;
    pace.driver.pacing = std::make_shared<core::SignInPacing>();
    return pace;
}

core::KvmEditing::Outcome edited(LocalWall& wall, const QString& id, const core::KvmEditing::Typed& typed)
{
    core::KvmEditing editing(wall, editingPace());
    std::optional<core::KvmEditing::Outcome> told;
    editing.change(id, typed, [&told](const core::KvmEditing::Outcome& outcome) { told = outcome; });
    static_cast<void>(QTest::qWaitFor([&told] { return told.has_value(); }, 30'000));
    return told.value_or(
        core::KvmEditing::Outcome{base::Error{"never", "The change was never answered."}, {}, {}, {}, false});
}

ProxmoxAdding::Typed typedFor(const ProxmoxByHand& pve, const QString& after = {})
{
    return ProxmoxAdding::Typed{pve.address().toString() + after, pve.tokenId, pve.secret, u"lab"_s};
}

base::Result<ProxmoxAdding::Offered> listed(ProxmoxAdding& adding, const ProxmoxAdding::Typed& typed)
{
    return waitedFor<ProxmoxAdding::Offered>(
        [&](std::function<void(base::Result<ProxmoxAdding::Offered>)> done) { adding.list(typed, std::move(done)); });
}

base::Result<ProxmoxAdding::Added> added(ProxmoxAdding& adding, const ProxmoxAdding::Typed& typed,
                                         const ProxmoxAdding::Offered& at, qint64 vmid)
{
    return waitedFor<ProxmoxAdding::Added>([&](std::function<void(base::Result<ProxmoxAdding::Added>)> done) {
        adding.add(typed, at, vmid, std::move(done));
    });
}

QHash<QString, core::Status> statusOf(LocalWall& wall)
{
    return answerOf<QHash<QString, core::Status>>(
               [&wall](std::function<void(base::Result<QHash<QString, core::Status>>)> done) {
                   wall.askStatus(std::move(done));
               })
        .value();
}

// Whether the wall keeps the certificate itself of each of its cards: once its driver has shown it, at its first look
// (which asking how the cards are has every driver take).
bool everyCertificateKept(LocalWall& wall)
{
    return !statusOf(wall).isEmpty() && std::ranges::all_of(wall.kvms(), [&wall](const OwnKvm& kvm) {
        return !wall.certificateOf(kvm.id).isEmpty();
    });
}

QString numberOf(const Guest& guest)
{
    return u"proxmox:"_s +
           QString::fromLatin1(
               QCryptographicHash::hash(guest.uuid.toLower().toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// A VNC server's times beside a guest's: short, its sign-ins counted with the guests'.
core::DriverPaces bothFamilies()
{
    core::DriverPaces paces = quickPaces();
    paces.vnc.screen.handshakeMs = 3000;
    paces.vnc.screen.openingMs = 3000;
    paces.vnc.screen.lookPaceMs = 50;
    paces.vnc.screen.idlePaceMs = 1000;
    paces.vnc.pacing = paces.proxmox.pacing;
    paces.kvmd.pacing = paces.proxmox.pacing;
    return paces;
}

// What came of a change, by its code ("ok" where it was done).
std::string codeOfTheChange(const core::KvmEditing::Outcome& outcome)
{
    return outcome.result ? std::string("ok") : outcome.result.error().code;
}

// A wall's card of guest 100 of `pve`, added as a person adds it; its id. Nothing looks at its card (the wall's driver
// of it has signed in nowhere).
QString cardOf(LocalWall& wall, const ProxmoxByHand& pve)
{
    ProxmoxAdding adding(wall, quickPaces());
    const auto offered = listed(adding, typedFor(pve));
    if (!offered) {
        return {};
    }
    const auto made = added(adding, typedFor(pve), offered.value(), 100);
    return made ? made.value().id : QString();
}

QJsonArray kvmsIn(const QString& file)
{
    QFile settings(file);
    if (!settings.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(settings.readAll()).object().value("kvms"_L1).toArray();
}

// Whether the published 0.1.2's reading of the settings (core/src/settings.cpp kvmFrom at 6a389a7c, the 0.1.x head)
// keeps this entry as it stands where its family is none it speaks to: an id of eight of 0-9 and a-f, and an address of
// HTTP or HTTPS that is exactly as kvm::addressFrom writes it (that code is this build's still: kvm_probe.cpp).
bool keptAsItStandsBy012(const QJsonObject& entry)
{
    static const QRegularExpression idShape(u"\\A[0-9a-f]{8}\\z"_s);
    const QString address = entry.value("address"_L1).toString();
    const QUrl url(address, QUrl::StrictMode);
    const base::Result<QUrl> asWritten = core::kvm::addressFrom(address);
    return idShape.match(entry.value("id"_L1).toString()).hasMatch() && url.isValid() && !url.host().isEmpty() &&
           url.userInfo().isEmpty() && (url.scheme() == "https"_L1 || url.scheme() == "http"_L1) && asWritten &&
           asWritten.value().toString(QUrl::FullyEncoded) == address;
}

} // namespace

// Proxmox guests on this PC's own wall (c9b, wave 2): added from what a token lists (ProxmoxAdding), each a card of
// its own with its driver (LocalWall: the family "proxmox"), its stored shape one the published 0.1.2 keeps as it
// stands, and the number it gives for itself kept with it (rule 30). Ported from the web's tests of adding a guest
// (../app/tests/test_proxmox.py of web 0.3.1).
class TestLocalWallProxmox : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { const core::ProxmoxDevice first(core::ProxmoxDevice::Where{}, QString(), quickProxmox()); }

    // The web's test_a_guest_is_added_and_shows_its_screen, its adding: named after the guest, its Proxmox known by
    // the certificate it showed first (Windows does not trust the stand-in's), the number it gives kept, the token in
    // the secret store and nowhere else, its card online with a screen.
    void aGuestIsAddedAndItsCardSaysIt()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typedFor(pve));
        QVERIFY2(offered, offered ? "" : offered.error().message.c_str());
        QCOMPARE(offered.value().guests.size(), 1);
        QVERIFY(offered.value().knownByCertificate);
        QCOMPARE(offered.value().certificate, certificateOf());
        QVERIFY(!offered.value().named);
        const auto made = added(adding, typedFor(pve), offered.value(), 100);
        QVERIFY2(made, made ? "" : made.error().message.c_str());
        QCOMPARE(made.value().name, u"web"_s);
        QCOMPARE(made.value().model, u"VM 100 on pve1"_s);
        const OwnKvm& kvm = wall.kvms().value(0);
        QCOMPARE(kvm.kind, u"proxmox"_s);
        QCOMPARE(kvm.address, pve.address().toString());
        QCOMPARE(kvm.node, u"pve1"_s);
        QCOMPARE(kvm.vmid, qint64{100});
        QCOMPARE(kvm.user, pve.tokenId);
        QCOMPARE(kvm.group, u"lab"_s);
        QCOMPARE(kvm.machine, numberOf(pve.guests.value(100)));
        QVERIFY(kvm.knownByCertificate);
        QVERIFY(wall.marksAPasswordFor(kvm.id));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(kvm.id).state, u"online"_s, 10'000);
        QVERIFY(statusOf(wall).value(kvm.id).hasScreen);
        // The token's secret is in no file of the settings.
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QVERIFY(!written.readAll().contains(pve.secret.toUtf8()));
        QVERIFY(pve.inTheClear.isEmpty());
    }

    // The web's test_two_guests_of_one_proxmox_are_two_machines_and_one_guest_twice_is_one, and
    // test_the_guests_a_token_may_see_are_listed_to_pick_from: what the list says of a guest on the wall.
    void twoGuestsOfOneProxmoxAreTwoCardsAndOneGuestTwiceIsOne()
    {
        ProxmoxByHand pve(proxmoxTls());
        Guest db;
        db.name = u"db"_s;
        db.status = u"stopped"_s;
        pve.guests.insert(101, db);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typedFor(pve));
        QVERIFY(offered);
        QVERIFY(added(adding, typedFor(pve), offered.value(), 100));
        QVERIFY(added(adding, typedFor(pve), offered.value(), 101));
        QCOMPARE(wall.kvms().size(), 2);
        // The same guest again: at its place, and by another address of its Proxmox (its number says so).
        const auto again = added(adding, typedFor(pve), offered.value(), 100);
        QVERIFY(!again);
        QCOMPARE(again.error().code, std::string("duplicate"));
        ProxmoxAdding::Typed elsewhere = typedFor(pve);
        elsewhere.address = u"https://localhost:%1/nodes/pve1/qemu/100"_s.arg(pve.address().port());
        const auto there = listed(adding, elsewhere);
        QVERIFY2(there, there ? "" : there.error().message.c_str());
        const auto twice = added(adding, elsewhere, there.value(), 100);
        QVERIFY(!twice);
        QCOMPARE(twice.error().code, std::string("same_device"));
        QCOMPARE(wall.kvms().size(), 2);
        // The list says which are on the wall; a new VM under a card's number is not that card's.
        const auto relisted = listed(adding, typedFor(pve));
        QVERIFY(relisted);
        QVERIFY(relisted.value().onTheWall.contains(100) && relisted.value().onTheWall.contains(101));
        pve.deleteGuest(100);
        Guest fresh;
        fresh.name = u"fresh"_s;
        pve.makeGuest(fresh);
        const auto after = listed(adding, typedFor(pve));
        QVERIFY(after);
        QVERIFY(!after.value().onTheWall.contains(100));
        QCOMPARE(after.value().newUnder.value(100), u"\"web\""_s);
    }

    // The web's test_what_is_no_token_is_sent_nowhere and test_a_wrong_token_is_a_no..., the list's part.
    void noTokenIsSentNowhereAndAWrongOneIsANo()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        ProxmoxAdding::Typed notAToken = typedFor(pve);
        notAToken.tokenId = u"root@pam"_s;
        QCOMPARE(listed(adding, notAToken).error().code, std::string("invalid"));
        QCOMPARE(pve.stand->lines, 0);
        // Not even a line: a listener that counts every caller, at the address typed.
        QTcpServer anybody;
        QVERIFY(anybody.listen(QHostAddress::LocalHost));
        int callers = 0;
        connect(&anybody, &QTcpServer::newConnection, &anybody, [&callers] { ++callers; });
        notAToken.address = u"https://127.0.0.1:%1"_s.arg(anybody.serverPort());
        QCOMPARE(listed(adding, notAToken).error().code, std::string("invalid"));
        QTest::qWait(300);
        QCOMPARE(callers, 0);
        ProxmoxAdding::Typed wrong = typedFor(pve);
        wrong.secret = u"00000000-0000-0000-0000-000000000000"_s;
        QCOMPARE(listed(adding, wrong).error().code, std::string("auth_failed"));
        QCOMPARE(pve.refused, 1);
        QVERIFY(wall.kvms().isEmpty());
        // A guest the address names that the token does not see.
        const auto unseen = listed(adding, typedFor(pve, u"/102"_s));
        QVERIFY(!unseen);
        QVERIFY(unseen.error().message.find("sees no virtual machine 102") != std::string::npos);
        const auto named = listed(adding, typedFor(pve, u"/100"_s));
        QVERIFY(named);
        QVERIFY(named.value().named.has_value());
        QCOMPARE(named.value().named.value_or(qkvm::core::proxmox::Listed{}).vmid, qint64{100});
    }

    // Outside review of 0.2.0, 3.2: what answers the unsigned question of a Proxmox's API is taken for a Proxmox only
    // where it refuses it 401, as a real one refuses every question without a token, or says its version. A 404, a
    // login page, a redirect, an error, an answer of another shape, a 401 that asks for a sign-in of its own, is
    // somebody else (another HTTPS service at the address typed): refused before the token goes, on a line whose
    // certificate is taken on first use as on any other.
    void onlyWhatSaysItIsAProxmoxIsSentTheToken_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("type");
        QTest::addColumn<QByteArray>("more");
        QTest::newRow("not-found") << 404 << QByteArray("Not Found") << QByteArray("text/plain") << QByteArray();
        QTest::newRow("a-login-page") << 200 << QByteArray("<!doctype html><form>Sign in</form>")
                                      << QByteArray("text/html") << QByteArray();
        QTest::newRow("a-redirect") << 302 << QByteArray() << QByteArray("text/html") << QByteArray();
        QTest::newRow("an-error") << 500 << QByteArray(R"({"data":null})") << QByteArray("application/json")
                                  << QByteArray();
        QTest::newRow("no-version") << 200 << QByteArray(R"({"data":{"release":"9.2"}})")
                                    << QByteArray("application/json") << QByteArray();
        QTest::newRow("a-version-of-another-kind")
            << 200 << QByteArray(R"({"data":{"version":9.2}})") << QByteArray("application/json") << QByteArray();
        QTest::newRow("forbidden") << 403 << QByteArray(R"({"data":null})") << QByteArray("application/json")
                                   << QByteArray();
        // (Review cx3-1, L2: a 401 that asks for a sign-in of its own is somebody else's: Proxmox's API asks for none,
        // and a token in the Authorization header answers no Basic, Digest or other challenge.)
        QTest::newRow("a-cameras-sign-in") << 401 << QByteArray() << QByteArray("text/html")
                                           << QByteArray(R"(WWW-Authenticate: Basic realm="IP Camera")");
        QTest::newRow("a-switchs-digest")
            << 401 << QByteArray() << QByteArray("text/html")
            << QByteArray(R"(WWW-Authenticate: Digest realm="switch", nonce="6a2b3c4d", qop="auth")");
        QTest::newRow("a-windows-sign-in")
            << 401 << QByteArray() << QByteArray("text/html") << QByteArray("WWW-Authenticate: Negotiate");
    }
    void onlyWhatSaysItIsAProxmoxIsSentTheToken()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QByteArray, type);
        QFETCH(QByteArray, more);
        QList<qkvm::core::testing::Asked> heard;
        StandIn other(
            [&](const qkvm::core::testing::Asked& asked) {
                heard.append(asked);
                Answered said(status, body, type);
                if (status == 302) {
                    said.more = {"Location: https://127.0.0.1/api2/json/version"};
                }
                if (!more.isEmpty()) {
                    said.more = {more};
                }
                return said;
            },
            proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const ProxmoxAdding::Typed typed{other.address(), u"wall@pve!qkvm"_s, u"0b9e4f2a-5c1d-4e8f-9a3b-7d6c5e4f3a2b"_s,
                                         u"lab"_s};
        const auto refused = listed(adding, typed);
        QCOMPARE(codeOf(refused), std::string("bad_response"));
        QVERIFY2(refused.error().message.find("does not say it is Proxmox VE") != std::string::npos,
                 refused.error().message.c_str());
        QVERIFY(!heard.isEmpty()); // (asked, on the line held to what it showed)
        for (const qkvm::core::testing::Asked& one : std::as_const(heard)) {
            QVERIFY2(!one.headers.contains("authorization"), one.path.constData());
            QVERIFY(!one.path.contains(typed.secret.toLatin1()));
        }
        QVERIFY(wall.kvms().isEmpty());
    }

    // ...and what says its version to that question is one (a Proxmox whose API answers it unsigned): the token goes
    // with the sign-in, on a line held to the certificate it showed. A real one, which refuses it 401, is asked it on
    // such a line (Windows does not trust the stand-in's certificate: taken on first use) before the token goes.
    void whatSaysItsVersionOrRefusesTheQuestionIsAProxmox()
    {
        QList<qkvm::core::testing::Asked> heard;
        StandIn versioned(
            [&](const qkvm::core::testing::Asked& asked) {
                heard.append(asked);
                if (asked.path == "/api2/json/version") {
                    return Answered(200, R"({"data":{"version":"9.2.21","release":"9.2"}})");
                }
                return Answered(404, {});
            },
            proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const ProxmoxAdding::Typed typed{versioned.address(), u"wall@pve!qkvm"_s,
                                         u"0b9e4f2a-5c1d-4e8f-9a3b-7d6c5e4f3a2b"_s, u"lab"_s};
        QVERIFY(!listed(adding, typed)); // (its list of guests is no list)
        QVERIFY(!heard.isEmpty() && !heard.first().headers.contains("authorization"));
        QVERIFY(std::ranges::any_of(
            heard, [](const qkvm::core::testing::Asked& one) { return one.headers.contains("authorization"); }));
        ProxmoxByHand pve(proxmoxTls());
        QVERIFY(listed(adding, typedFor(pve)));
        QCOMPARE(pve.unsignedAsked, QStringList{u"GET /version"_s});
        QCOMPARE(pve.refused, 0);
    }

    // Outside review of 0.2.0, 3.1: a Proxmox whose guests are on the wall is known by what their cards are known by,
    // and by no certificate it shows on first use. Its address shows another one now (a machine in the middle): the
    // guests a token sees there are listed only once a card of it was said yes to that one, and nothing reaches it
    // before (not even the unsigned question: the line is held to the cards' certificate). Where its cards are known by
    // two (one said yes to the new one, the other not yet), each is tried, and the one it shows is taken.
    void aProxmoxWhoseGuestsAreOnTheWallIsKnownByWhatTheyAreKnownBy()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        Guest db;
        db.name = u"db"_s;
        pve->guests.insert(101, db);
        const auto port = static_cast<quint16>(pve->address().port());
        const QString webUuid = pve->guests.value(100).uuid;
        const QString dbUuid = pve->guests.value(101).uuid;
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typedFor(*pve));
        QVERIFY(offered);
        const auto first = added(adding, typedFor(*pve), offered.value(), 100);
        QVERIFY2(first, first ? "" : first.error().message.c_str());
        pve.reset();
        ProxmoxByHand middle(proxmoxTls("second"), port);
        middle.guests[100].uuid = webUuid;
        middle.guests.insert(101, db);
        middle.guests[101].uuid = dbUuid;
        const auto refused = listed(adding, typedFor(middle));
        QCOMPARE(codeOf(refused), std::string("cert_mismatch"));
        QVERIFY2(refused.error().message.find("than its guests on the wall are known by") != std::string::npos,
                 refused.error().message.c_str());
        QVERIFY(middle.requests.isEmpty());
        QVERIFY(middle.unsignedAsked.isEmpty());
        QCOMPARE(middle.refused, 0);
        QCOMPARE(wall.kvms().size(), 1);
        // Said yes to on its card: listed there, known by it, and a guest of it added so.
        const base::Result<void> yes = wall.knowBy(first.value().id, certificateOf("second"));
        QVERIFY2(yes, yes ? "" : yes.error().message.c_str());
        const auto now = listed(adding, typedFor(middle));
        QVERIFY2(now, now ? "" : now.error().message.c_str());
        QVERIFY(now.value().knownByCertificate);
        QCOMPARE(now.value().certificate, certificateOf("second"));
        QVERIFY(now.value().onTheWall.contains(100));
        const auto second = added(adding, typedFor(middle), now.value(), 101);
        QVERIFY2(second, second ? "" : second.error().message.c_str());
        QCOMPARE(wall.kvms().value(1).certificate, certificateOf("second"));
        // Its first card back on the first certificate: both ways are tried, and the one it shows is taken.
        const base::Result<void> back = wall.knowBy(first.value().id, certificateOf());
        QVERIFY2(back, back ? "" : back.error().message.c_str());
        const auto either = listed(adding, typedFor(middle));
        QVERIFY2(either, either ? "" : either.error().message.c_str());
        QCOMPARE(either.value().certificate, certificateOf("second"));
    }

    // Review cx3-1, L3 (outside review of 0.2.0, 3.1, at the add): an add from a listing made before guests of that
    // Proxmox came onto the wall is held to what their cards are known by, as a listing is. One box lists it while it
    // shows one certificate (taken on first use: no guest of it on the wall then); another lists it while it shows
    // another, taken on first use too, and adds a guest; it shows the first again. The first box's add is refused, and
    // nothing reaches what answers: its guests are listed again first.
    void anAddFromAnEarlierListingIsHeldToTheCardsOnTheWallSince()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        Guest db;
        db.name = u"db"_s;
        pve->guests.insert(101, db);
        const auto port = static_cast<quint16>(pve->address().port());
        const QMap<qint64, Guest> guests = pve->guests;
        const ProxmoxAdding::Typed typed = typedFor(*pve);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding first(wall, quickPaces());
        ProxmoxAdding second(wall, quickPaces());
        const auto earlier = listed(first, typed);
        QVERIFY2(earlier, earlier ? "" : earlier.error().message.c_str());
        QCOMPARE(earlier.value().certificate, certificateOf());
        pve.reset();
        auto other = std::make_unique<ProxmoxByHand>(proxmoxTls("second"), port);
        other->guests = guests;
        const auto later = listed(second, typed);
        QVERIFY2(later, later ? "" : later.error().message.c_str());
        QCOMPARE(later.value().certificate, certificateOf("second"));
        const auto onTheWall = added(second, typed, later.value(), 101);
        QVERIFY2(onTheWall, onTheWall ? "" : onTheWall.error().message.c_str());
        other.reset();
        ProxmoxByHand again(proxmoxTls(), port);
        again.guests = guests;
        const auto refused = added(first, typed, earlier.value(), 100);
        QCOMPARE(codeOf(refused), std::string("cert_mismatch"));
        QVERIFY(again.requests.isEmpty());
        QVERIFY(again.unsignedAsked.isEmpty());
        QCOMPARE(wall.kvms().size(), 1);
        // Listed again, it is known by what the card is known by: refused there too, until a yes on the card.
        QCOMPARE(codeOf(listed(first, typed)), std::string("cert_mismatch"));
    }

    // Outside review of 0.2.0, 3.2, on every road the token takes (review cx3-1, H1): it goes only where what answers
    // the unsigned question of its Proxmox's API, on the very line the token is to go on, says it is a Proxmox; asked
    // right before every sign-in. Here somebody else answers there (a 404), showing the certificate the guest is known
    // by or one that was said yes to: nothing it hears carries the token, and what came of it says why. At an add, from
    // a listing made a moment before:
    void theTokenGoesOnlyToAProxmoxAtAnAdd()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        const auto port = static_cast<quint16>(pve->address().port());
        const ProxmoxAdding::Typed typed = typedFor(*pve);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typed);
        QVERIFY2(offered, offered ? "" : offered.error().message.c_str());
        pve.reset();
        SomebodyElse other("first", port);
        const auto made = added(adding, typed, offered.value(), 100);
        QCOMPARE(codeOf(made), std::string("bad_response"));
        QVERIFY2(made.error().message.find("does not say it is Proxmox VE") != std::string::npos,
                 made.error().message.c_str());
        QVERIFY(!other.heard.isEmpty());
        QVERIFY(!other.heardTheToken(typed.secret));
        QVERIFY(wall.kvms().isEmpty());
    }

    // ...at an edit to a new address: where it shows the certificate the guest is known by (the way a Proxmox known
    // by Windows' word goes silently, which the stand-in cannot be), with its secret typed or with the one kept; and
    // where it shows another, said yes to.
    void theTokenGoesOnlyToAProxmoxAtAnEditToANewAddress()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        const QString id = cardOf(wall, pve);
        QVERIFY(!id.isEmpty());
        const QString before = wall.kvms().value(0).address;
        core::KvmEditing::Typed typed;
        typed.name = u"web"_s;
        typed.user = pve.tokenId;
        typed.password = pve.secret;
        typed.group = u"lab"_s;
        {
            SomebodyElse other("first");
            typed.address = other.address();
            core::KvmEditing::Outcome outcome = edited(wall, id, typed);
            QCOMPARE(codeOfTheChange(outcome), std::string("bad_response"));
            QVERIFY(!other.heard.isEmpty());
            QVERIFY(!other.heardTheToken(pve.secret));
            // (With the secret that is kept: the same.)
            typed.password.clear();
            outcome = edited(wall, id, typed);
            QVERIFY(!outcome.result);
            QVERIFY(!other.heardTheToken(pve.secret));
            QCOMPARE(wall.kvms().value(0).address, before);
        }
        SomebodyElse other("second");
        typed.address = other.address();
        typed.password = pve.secret;
        core::KvmEditing::Outcome outcome = edited(wall, id, typed);
        QCOMPARE(codeOfTheChange(outcome), std::string("certificate_differs"));
        typed.saidYesTo = outcome.presented;
        outcome = edited(wall, id, typed);
        QCOMPARE(codeOfTheChange(outcome), std::string("bad_response"));
        QVERIFY(!other.heardTheToken(pve.secret));
        QCOMPARE(wall.kvms().value(0).address, before);
    }

    // ...at an edit at its own address, with its secret typed:
    void theTokenGoesOnlyToAProxmoxAtAnEditAtItsAddress()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        const auto port = static_cast<quint16>(pve->address().port());
        const QString secret = pve->secret;
        const QString tokenId = pve->tokenId;
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        const QString id = cardOf(wall, *pve);
        QVERIFY(!id.isEmpty());
        pve.reset();
        SomebodyElse other("first", port);
        core::KvmEditing::Typed typed;
        typed.name = u"web"_s;
        typed.address = wall.kvms().value(0).address;
        typed.user = tokenId;
        typed.password = secret;
        typed.group = u"lab"_s;
        const core::KvmEditing::Outcome outcome = edited(wall, id, typed);
        QCOMPARE(codeOfTheChange(outcome), std::string("bad_response"));
        QVERIFY(!other.heard.isEmpty());
        QVERIFY(!other.heardTheToken(secret));
    }

    // ...after a yes on its card to another certificate (its Proxmox's address handed to another machine):
    void theTokenGoesOnlyToAProxmoxAfterAYesToAnotherCertificate()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        const auto port = static_cast<quint16>(pve->address().port());
        const QString secret = pve->secret;
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        const QString id = cardOf(wall, *pve);
        QVERIFY(!id.isEmpty());
        pve.reset();
        SomebodyElse other("second", port);
        const base::Result<void> yes = wall.knowBy(id, certificateOf("second"));
        QVERIFY2(yes, yes ? "" : yes.error().message.c_str());
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).errorCode, u"bad_response"_s, 10'000);
        QVERIFY2(statusOf(wall).value(id).errorMessage.contains(u"does not say it is Proxmox VE"_s),
                 qPrintable(statusOf(wall).value(id).errorMessage));
        QVERIFY(!other.heard.isEmpty());
        QVERIFY(!other.heardTheToken(secret));
    }

    // ...for guests from a file (the outside review's 6.2 too): the wall's own file of two guests of one Proxmox,
    // sealed with their token, laid out and taken on another wall by the Import box's road (WallExchange); and one
    // handed to a wall as from a file that says it was asked already. Somebody else answers at its address by then,
    // showing the certificate the file names: at look after look, and after a start of the program with them on the
    // wall, nothing it hears carries the token, and every card says why.
    void theTokenGoesOnlyToAProxmoxFromAFile()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        Guest db;
        db.name = u"db"_s;
        pve->guests.insert(101, db);
        const auto port = static_cast<quint16>(pve->address().port());
        const QString secret = pve->secret;
        const QString passphrase = u"a passphrase of ours"_s;
        QByteArray file;
        {
            const QTemporaryDir before;
            core::Settings settings(before.filePath(u"settings.json"_s));
            MemorySecrets secrets;
            LocalWall wall(settings, secrets, quickPaces());
            ProxmoxAdding adding(wall, quickPaces());
            const auto offered = listed(adding, typedFor(*pve));
            QVERIFY(offered);
            for (const qint64 vmid : {qint64{100}, qint64{101}}) {
                const auto made = added(adding, typedFor(*pve), offered.value(), vmid);
                QVERIFY2(made, made ? "" : made.error().message.c_str());
            }
            // (Each certificate itself is kept once its driver has shown it: at its first look.)
            QTRY_VERIFY_WITH_TIMEOUT(everyCertificateKept(wall), 10'000);
            const auto sealed = core::exchange::write(wall.asAFile(true).content, passphrase);
            QVERIFY2(sealed, sealed ? "" : sealed.error().message.c_str());
            file = sealed.value().file(core::exchange::NotInIt::wasLookedAt);
        }
        pve.reset();
        SomebodyElse other("first", port);
        const QTemporaryDir folder;
        const QString path = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        QStringList ids;
        {
            core::Settings settings(path);
            LocalWall wall(settings, secrets, quickPaces());
            core::WallExchange exchange(wall);
            const auto laidOut = answerOf<core::WallExchange::LaidOut>(
                [&](std::function<void(base::Result<core::WallExchange::LaidOut>)> done) {
                    exchange.layOut(file, passphrase, &exchange, std::move(done));
                });
            QVERIFY2(laidOut, laidOut ? "" : laidOut.error().message.c_str());
            const auto& rows = laidOut.value().devices(core::WallExchange::BesideTheList::wasLookedAt);
            QCOMPARE(rows.size(), 2);
            const QList<core::WallExchange::Took> took =
                exchange.take(laidOut.value().number(), {rows.at(0).place, rows.at(1).place});
            QCOMPARE(took.size(), 2);
            for (const core::WallExchange::Took& one : took) {
                QVERIFY2(one.came, one.came ? "" : one.came.error().message.c_str());
            }
            QCOMPARE(wall.kvms().size(), 2);
            ids = {wall.kvms().value(0).id, wall.kvms().value(1).id};
            for (const QString& id : std::as_const(ids)) {
                QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).errorCode, u"bad_response"_s, 10'000);
            }
            // Look after look, past the hold each time.
            for (int look = 0; look < 4; ++look) {
                QTest::qWait(400);
                const QHash<QString, core::Status> now = statusOf(wall);
                QCOMPARE(now.value(ids.at(0)).errorCode, u"bad_response"_s);
                QCOMPARE(now.value(ids.at(1)).errorCode, u"bad_response"_s);
            }
            QVERIFY(other.heard.size() >= 4);
            QVERIFY(!other.heardTheToken(secret));
        }
        // A start of the program with them on the wall.
        {
            const qsizetype heard = other.heard.size();
            core::Settings settings(path);
            LocalWall wall(settings, secrets, quickPaces());
            QCOMPARE(wall.kvms().size(), 2);
            for (const QString& id : std::as_const(ids)) {
                QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).errorCode, u"bad_response"_s, 10'000);
            }
            QVERIFY(other.heard.size() > heard);
            QVERIFY(!other.heardTheToken(secret));
        }
        // One handed in as from a file that says it was asked: what the wall keeps of that is its own, and its driver
        // asks all the same.
        core::Settings settings(folder.filePath(u"another.json"_s));
        MemorySecrets more;
        LocalWall wall(settings, more, quickPaces());
        LocalWall::FromAFile one;
        one.kvm.kind = u"proxmox"_s;
        one.kvm.address = other.address();
        one.kvm.user = u"wall@pve!qkvm"_s;
        one.kvm.node = u"pve1"_s;
        one.kvm.vmid = 100;
        one.kvm.knownByCertificate = true;
        one.kvm.certificate = certificateOf();
        one.kvm.askedWhatItIs = true;
        one.kvm.name = u"web"_s;
        one.password = secret;
        const QList<base::Result<QString>> took = wall.takeFromAFile({one});
        QCOMPARE(took.size(), 1);
        QVERIFY(took.first());
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(took.first().value()).errorCode, u"bad_response"_s, 10'000);
        QVERIFY(!other.heardTheToken(secret));
    }

    // ...at a start of the program, its cards on the wall already: each guest's driver asks on its own line, the one
    // that would go on with the other's sign-in too.
    void theTokenGoesOnlyToAProxmoxAtAStartOfTheProgram()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        Guest db;
        db.name = u"db"_s;
        pve->guests.insert(101, db);
        const auto port = static_cast<quint16>(pve->address().port());
        const QString secret = pve->secret;
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quickPaces());
            ProxmoxAdding adding(wall, quickPaces());
            const auto offered = listed(adding, typedFor(*pve));
            QVERIFY(offered);
            QVERIFY(added(adding, typedFor(*pve), offered.value(), 100));
            QVERIFY(added(adding, typedFor(*pve), offered.value(), 101));
        }
        pve.reset();
        SomebodyElse other("first", port);
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quickPaces());
        QCOMPARE(wall.kvms().size(), 2);
        const QStringList ids{wall.kvms().value(0).id, wall.kvms().value(1).id};
        for (const QString& id : ids) {
            QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).errorCode, u"bad_response"_s, 10'000);
        }
        QVERIFY(other.heard.size() >= 2);
        QVERIFY(!other.heardTheToken(secret));
    }

    // The stored shape (SPEC.md section 11.2, settled with the coordinator): an entry of `kvms`, kind "proxmox", the
    // address its Proxmox's API as kvm::addressFrom writes it, node, vmid, the number it gives for itself, the token's
    // ID as its user; read back as it was written; and one the published 0.1.2 keeps as it stands (its family is none
    // 0.1.2 speaks to): shown by its rule, and by this build's own reading of an entry of a family it does not speak
    // to.
    void theStoredShapeIsReadBackAndKeptBy012()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QString id;
        OwnKvm stored;
        {
            core::Settings settings(file);
            MemorySecrets secrets;
            LocalWall wall(settings, secrets, quickPaces());
            ProxmoxAdding adding(wall, quickPaces());
            const auto offered = listed(adding, typedFor(pve));
            QVERIFY(offered);
            const auto made = added(adding, typedFor(pve), offered.value(), 100);
            QVERIFY(made);
            id = made.value().id;
            stored = wall.kvms().value(0);
        }
        const QJsonArray kvms = kvmsIn(file);
        QCOMPARE(kvms.size(), 1);
        const QJsonObject entry = kvms.at(0).toObject();
        QCOMPARE(entry.value("kind"_L1).toString(), u"proxmox"_s);
        QCOMPARE(entry.value("node"_L1).toString(), u"pve1"_s);
        QCOMPARE(entry.value("vmid"_L1).toInteger(), qint64{100});
        QCOMPARE(entry.value("machine"_L1).toString(), numberOf(pve.guests.value(100)));
        QCOMPARE(entry.value("user"_L1).toString(), pve.tokenId);
        QCOMPARE(entry.value("secret_kept"_L1), QJsonValue(true));
        QVERIFY(keptAsItStandsBy012(entry));
        // Read back unchanged.
        core::Settings again(file);
        QCOMPARE(again.remembered().kvms.value(0), stored);
        QVERIFY(again.remembered().kvmsItDoesNotRead.isEmpty());
        // As an entry of a family a build does not speak to (what 0.1.2 makes of "proxmox"): kept as it stands, and
        // written back so.
        QJsonObject unknown = entry;
        unknown.insert(u"kind"_s, u"a-family-of-later"_s);
        QJsonObject whole = QJsonDocument::fromJson([&file] {
                                QFile kept(file);
                                return kept.open(QIODevice::ReadOnly) ? kept.readAll() : QByteArray();
                            }())
                                .object();
        whole.insert(u"kvms"_s, QJsonArray{unknown});
        {
            QFile rewritten(file);
            QVERIFY(rewritten.open(QIODevice::WriteOnly | QIODevice::Truncate));
            rewritten.write(QJsonDocument(whole).toJson());
        }
        core::Settings older(file);
        QVERIFY(older.remembered().kvms.isEmpty());
        QCOMPARE(older.remembered().kvmsItDoesNotRead, QJsonArray{unknown});
        QVERIFY(older.remember(older.remembered()));
        QCOMPARE(kvmsIn(file), QJsonArray{unknown});
        // An entry whose number is none of a guest's (out of a qint64's reach, a fraction, below 100, beyond nine
        // digits) is not read, and is kept as it stands: never cast to a number first (review c9b-1, L7).
        for (const double vmid : {1e300, -1e300, 100.5, 99.0, 1e10, 9.3e18, -5.0}) {
            QJsonObject odd = entry;
            odd.insert(u"vmid"_s, vmid);
            whole.insert(u"kvms"_s, QJsonArray{odd});
            {
                QFile rewritten(file);
                QVERIFY(rewritten.open(QIODevice::WriteOnly | QIODevice::Truncate));
                rewritten.write(QJsonDocument(whole).toJson());
            }
            const core::Settings read(file);
            QVERIFY2(read.remembered().kvms.isEmpty(), qPrintable(QString::number(vmid)));
            QCOMPARE(read.remembered().kvmsItDoesNotRead.size(), 1);
        }
        // And an entry that says "proxmox" in another shape (a node that is none) is not read, and kept as it stands.
        QJsonObject odd = entry;
        odd.insert(u"node"_s, u"not a node"_s);
        whole.insert(u"kvms"_s, QJsonArray{odd});
        {
            QFile rewritten(file);
            QVERIFY(rewritten.open(QIODevice::WriteOnly | QIODevice::Truncate));
            rewritten.write(QJsonDocument(whole).toJson());
        }
        core::Settings oddly(file);
        QVERIFY(oddly.remembered().kvms.isEmpty());
        QCOMPARE(oddly.remembered().kvmsItDoesNotRead, QJsonArray{odd});
    }

    // Review c9b-3, M1: the boot order a guest's driver replaced and has not put back yet, in the guest's stored data
    // (SPEC.md section 11.2, `boot_order_was`): read back as it was written, written back the same, one the published
    // 0.1.2 keeps as it stands; in any other shape the entry is not read, and is kept as it stands; the wall's own to
    // keep (none taken from an adding, and every change keeps it).
    void aBootOrderFromBeforeIsStoredWithTheGuest()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        QString id;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quickPaces());
            ProxmoxAdding adding(wall, quickPaces());
            const auto offered = listed(adding, typedFor(pve));
            QVERIFY(offered);
            const auto made = added(adding, typedFor(pve), offered.value(), 100);
            QVERIFY(made);
            id = made.value().id;
            // None handed in is taken.
            OwnKvm handed = wall.kvms().value(0);
            handed.vmid = 101;
            handed.machine.clear();
            handed.bootOrderWas = core::BootOrderWas{u"scsi0"_s, u"ide2;scsi0"_s, 101, QString()};
            const base::Result<QString> other = wall.add(handed, pve.secret);
            QVERIFY2(other, other ? "" : other.error().message.c_str());
            QVERIFY(!wall.kvms().value(1).bootOrderWas);
            QVERIFY(wall.remove(other.value()).ok());
            // ...nor from a file of the wall.
            const QList<base::Result<QString>> taken =
                wall.takeFromAFile({LocalWall::FromAFile{handed, pve.secret, {}}});
            QVERIFY(taken.size() == 1 && taken.front());
            QVERIFY(!wall.kvms().value(1).bootOrderWas);
            QVERIFY(wall.remove(taken.front().value()).ok());
        }
        const core::BootOrderWas was{u"scsi0;ide2;net0"_s, u"ide2;scsi0;net0"_s, 100, numberOf(pve.guests.value(100))};
        QJsonObject entry = kvmsIn(file).at(0).toObject();
        QVERIFY(!entry.contains(u"boot_order_was"_s));
        const QJsonObject stored{
            {u"order"_s, was.order}, {u"written"_s, was.written}, {u"vmid"_s, was.vmid}, {u"machine"_s, was.machine}};
        entry.insert(u"boot_order_was"_s, stored);
        QJsonObject whole = QJsonDocument::fromJson([&file] {
                                QFile kept(file);
                                return kept.open(QIODevice::ReadOnly) ? kept.readAll() : QByteArray();
                            }())
                                .object();
        const auto rewrite = [&file, &whole](const QJsonObject& one) {
            whole.insert(u"kvms"_s, QJsonArray{one});
            QFile rewritten(file);
            return rewritten.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                   rewritten.write(QJsonDocument(whole).toJson()) > 0;
        };
        QVERIFY(rewrite(entry));
        QVERIFY(keptAsItStandsBy012(entry));
        {
            core::Settings settings(file);
            QCOMPARE(settings.remembered().kvms.size(), 1);
            QCOMPARE(settings.remembered().kvms.value(0).bootOrderWas, std::optional<core::BootOrderWas>(was));
            QVERIFY(settings.remember(settings.remembered()));
            QCOMPARE(kvmsIn(file).at(0).toObject().value("boot_order_was"_L1).toObject(), stored);
            // Every change keeps it: a new name and group, whatever was handed in.
            LocalWall wall(settings, secrets, quickPaces());
            OwnKvm renamed = wall.kvms().value(0);
            renamed.name = u"web, renamed"_s;
            renamed.group = u"other"_s;
            renamed.bootOrderWas.reset();
            QVERIFY(wall.change(id, renamed, std::nullopt));
            QCOMPARE(wall.kvms().value(0).name, u"web, renamed"_s);
            QCOMPARE(wall.kvms().value(0).bootOrderWas, std::optional<core::BootOrderWas>(was));
        }
        QCOMPARE(kvmsIn(file).at(0).toObject().value("boot_order_was"_L1).toObject(), stored);
        // In any other shape: the entry is not read, and is kept as it stands.
        entry = kvmsIn(file).at(0).toObject();
        const QList<std::pair<QString, QJsonValue>> odd{
            {u"order"_s, 5},
            {u"order"_s, u""_s},
            {u"order"_s, u"cdn"_s},
            {u"written"_s, u"ide2;ide2"_s},
            {u"written"_s, u"ide2;SCSI0"_s},
            {u"vmid"_s, 99},
            {u"vmid"_s, 100.5},
            {u"machine"_s, u"not a number"_s},
            {u"machine"_s, QJsonValue::Null},
        };
        for (const auto& [key, value] : odd) {
            QJsonObject wrong = stored;
            wrong.insert(key, value);
            QJsonObject one = entry;
            one.insert(u"boot_order_was"_s, wrong);
            QVERIFY(rewrite(one));
            const core::Settings read(file);
            QVERIFY2(read.remembered().kvms.isEmpty(), qPrintable(key));
            QCOMPARE(read.remembered().kvmsItDoesNotRead, QJsonArray{one});
        }
        for (const QJsonValue& notOne : {QJsonValue(u"scsi0;ide2"_s), QJsonValue(QJsonArray{}), QJsonValue(true)}) {
            QJsonObject one = entry;
            one.insert(u"boot_order_was"_s, notOne);
            QVERIFY(rewrite(one));
            const core::Settings read(file);
            QVERIFY(read.remembered().kvms.isEmpty());
            QCOMPARE(read.remembered().kvmsItDoesNotRead, QJsonArray{one});
        }
    }

    // Review c9b-4, L3: a guest whose entry this build keeps as it stands (its boot order from before in a later
    // build's shape) stands at its own place, its Proxmox's and its number there: the same guest is not added a second
    // time, from the box nor from a file of the wall; another guest of that Proxmox is, and so is a KVM of another
    // family at the Proxmox's port, which is no guest's place. One whose number is no guest's stands at no guest's
    // place.
    void aGuestKeptAsItStandsHasItsOwnPlace()
    {
        ProxmoxByHand pve(proxmoxTls());
        Guest db;
        db.name = u"db"_s;
        db.status = u"stopped"_s;
        pve.guests.insert(101, db);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        OwnKvm guest;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quickPaces());
            ProxmoxAdding adding(wall, quickPaces());
            const auto offered = listed(adding, typedFor(pve));
            QVERIFY(offered);
            QVERIFY(added(adding, typedFor(pve), offered.value(), 100));
            guest = wall.kvms().value(0);
        }
        QJsonObject whole = QJsonDocument::fromJson([&file] {
                                QFile kept(file);
                                return kept.open(QIODevice::ReadOnly) ? kept.readAll() : QByteArray();
                            }())
                                .object();
        const auto keptAs = [&file, &whole](const QJsonObject& one) {
            whole.insert(u"kvms"_s, QJsonArray{one});
            QFile rewritten(file);
            return rewritten.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                   rewritten.write(QJsonDocument(whole).toJson()) > 0;
        };
        QJsonObject entry = kvmsIn(file).at(0).toObject();
        entry.insert(u"boot_order_was"_s, u"in a later build's shape"_s);
        QVERIFY(keptAs(entry));
        {
            core::Settings settings(file);
            QVERIFY(settings.remembered().kvms.isEmpty());
            QCOMPARE(settings.remembered().kvmsItDoesNotRead.size(), 1);
            LocalWall wall(settings, secrets, quickPaces());
            ProxmoxAdding adding(wall, quickPaces());
            const auto offered = listed(adding, typedFor(pve));
            QVERIFY(offered);
            const auto again = added(adding, typedFor(pve), offered.value(), 100);
            QVERIFY(!again);
            QCOMPARE(again.error().code, std::string("duplicate"));
            QVERIFY2(again.error().message.starts_with("A KVM that another version of qKVM added is kept at this"),
                     again.error().message.c_str());
            const QList<base::Result<QString>> taken =
                wall.takeFromAFile({LocalWall::FromAFile{guest, pve.secret, {}}});
            QVERIFY(taken.size() == 1 && !taken.front());
            QVERIFY(wall.kvms().isEmpty());
            // Another guest of that Proxmox, and a kvmd at its port.
            QVERIFY(added(adding, typedFor(pve), offered.value(), 101));
            OwnKvm kvmd;
            kvmd.kind = u"kvmd"_s;
            kvmd.name = u"Rack"_s;
            kvmd.address = pve.address().toString();
            kvmd.knownByCertificate = true;
            kvmd.certificate = certificateOf();
            const base::Result<QString> there = wall.add(kvmd, u"secret"_s);
            QVERIFY2(there, there ? "" : there.error().message.c_str());
            QCOMPARE(wall.kvms().size(), 2);
        }
        // Its number in no guest's shape: at no guest's place, nor at its Proxmox's port.
        entry.insert(u"vmid"_s, u"100"_s);
        QVERIFY(keptAs(entry));
        core::Settings settings(file);
        QCOMPARE(settings.remembered().kvmsItDoesNotRead.size(), 1);
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typedFor(pve));
        QVERIFY(offered);
        QVERIFY(added(adding, typedFor(pve), offered.value(), 100));
        OwnKvm kvmd;
        kvmd.kind = u"kvmd"_s;
        kvmd.address = pve.address().toString();
        kvmd.knownByCertificate = true;
        kvmd.certificate = certificateOf();
        QVERIFY(wall.add(kvmd, u"secret"_s));
    }

    // Its buttons through the wall, and whether what came of one may have been done is its family's word.
    void itsButtonsGoThroughTheWall()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typedFor(pve));
        QVERIFY(offered);
        const auto made = added(adding, typedFor(pve), offered.value(), 100);
        QVERIFY(made);
        const auto reset = answerOf<core::Done>([&](std::function<void(base::Result<core::Done>)> done) {
            wall.act(made.value().id, core::Action::plain(u"atx_reset"_s), std::move(done));
        });
        QVERIFY2(reset, reset ? "" : reset.error().message.c_str());
        QCOMPARE(pve.tasks.value(0).type, u"qmreset"_s);
        pve.powerAnswer = 596;
        const auto lost = answerOf<core::Done>([&](std::function<void(base::Result<core::Done>)> done) {
            wall.act(made.value().id, core::Action::plain(u"atx_reset"_s), std::move(done));
        });
        QVERIFY(!lost);
        QVERIFY(wall.mayHaveBeenDone(made.value().id, lost.error()));
    }

    // The web's test_a_tile_that_remembers_no_number_takes_the_first_it_sees_and_keeps_it, on the wall: a guest taken
    // from a file with no number is known by the first it gives, kept with its card for its next driver.
    void aGuestFromAFileWithNoNumberKeepsTheFirstItGives()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        LocalWall::FromAFile one;
        one.kvm.kind = u"proxmox"_s;
        one.kvm.address = pve.address().toString();
        one.kvm.user = pve.tokenId;
        one.kvm.node = u"pve1"_s;
        one.kvm.vmid = 100;
        one.kvm.knownByCertificate = true;
        one.kvm.certificate = certificateOf();
        one.kvm.name = u"web"_s;
        one.password = pve.secret;
        const QList<base::Result<QString>> took = wall.takeFromAFile({one});
        QCOMPARE(took.size(), 1);
        QVERIFY(took.first());
        const QString id = took.first().value();
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).state, u"online"_s, 10'000);
        QTRY_COMPARE_WITH_TIMEOUT(wall.kvms().value(0).machine, numberOf(pve.guests.value(100)), 5000);
    }

    // Review c9b-1, M1: every guest of one Proxmox shows its certificate, and they are guests of their own: a file with
    // two of them is taken whole, and so is one beside another of its Proxmox on the wall. One guest twice is one.
    void twoGuestsOfOneProxmoxFromAFileAreBothTaken()
    {
        ProxmoxByHand pve(proxmoxTls());
        Guest db;
        db.name = u"db"_s;
        db.status = u"stopped"_s;
        pve.guests.insert(101, db);
        Guest mail;
        mail.name = u"mail"_s;
        pve.guests.insert(102, mail);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        const auto fromAFile = [&pve](qint64 vmid) {
            LocalWall::FromAFile one;
            one.kvm.kind = u"proxmox"_s;
            one.kvm.address = pve.address().toString();
            one.kvm.user = pve.tokenId;
            one.kvm.node = u"pve1"_s;
            one.kvm.vmid = vmid;
            one.kvm.knownByCertificate = true;
            one.kvm.certificate = certificateOf();
            one.kvm.machine = numberOf(pve.guests.value(vmid));
            one.kvm.name = pve.guests.value(vmid).name;
            one.password = pve.secret;
            return one;
        };
        QList<base::Result<QString>> took = wall.takeFromAFile({fromAFile(100), fromAFile(101)});
        QCOMPARE(took.size(), 2);
        QVERIFY2(took.at(0), took.at(0) ? "" : took.at(0).error().message.c_str());
        QVERIFY2(took.at(1), took.at(1) ? "" : took.at(1).error().message.c_str());
        took = wall.takeFromAFile({fromAFile(102)});
        QVERIFY2(took.first(), took.first() ? "" : took.first().error().message.c_str());
        QCOMPARE(wall.kvms().size(), 3);
        const QString mailId = took.first().value();
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(mailId).state, u"online"_s, 10'000);
        took = wall.takeFromAFile({fromAFile(101)});
        QCOMPARE(codeOf(took.first()), std::string("duplicate"));
        QCOMPARE(wall.kvms().size(), 3);
    }

    // Review cx3-2, M1: a wall's two guests of one Proxmox go into its file, sealed and plain, and come out of it whole
    // on an empty wall (CHANGELOG: in the web app's format), each laid out and taken at a place of its own, with its
    // node, its number, the number it gives for itself, its token's ID and what its Proxmox is known by. From a sealed
    // file each comes with a record of its own of the token's secret (SPEC 11.4: one for each guest, whatever they
    // share), and its card shows its own guest; from a plain one, with none to be typed.
    void twoGuestsOfOneProxmoxGoIntoTheWallsFileAndComeOutWhole()
    {
        ProxmoxByHand pve(proxmoxTls());
        Guest db;
        db.name = u"db"_s;
        pve.guests.insert(101, db);
        const QString passphrase = u"a passphrase of ours"_s;
        const QTemporaryDir before;
        core::Settings settings(before.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typedFor(pve));
        QVERIFY(offered);
        for (const qint64 vmid : {qint64{100}, qint64{101}}) {
            const auto made = added(adding, typedFor(pve), offered.value(), vmid);
            QVERIFY2(made, made ? "" : made.error().message.c_str());
        }
        QCOMPARE(wall.kvms().size(), 2);
        // (Each certificate itself is kept once its driver has shown it: at its first look.)
        QTRY_VERIFY_WITH_TIMEOUT(everyCertificateKept(wall), 10'000);
        for (const bool sealed : {true, false}) {
            const auto written = core::exchange::write(wall.asAFile(sealed).content, sealed ? passphrase : QString());
            QVERIFY2(written, written ? "" : written.error().message.c_str());
            const QTemporaryDir after;
            core::Settings fresh(after.filePath(u"settings.json"_s));
            MemorySecrets more;
            LocalWall empty(fresh, more, quickPaces());
            core::WallExchange exchange(empty);
            const auto laidOut = answerOf<core::WallExchange::LaidOut>(
                [&](std::function<void(base::Result<core::WallExchange::LaidOut>)> done) {
                    exchange.layOut(written.value().file(core::exchange::NotInIt::wasLookedAt),
                                    sealed ? passphrase : QString(), &exchange, std::move(done));
                });
            QVERIFY2(laidOut, laidOut ? "" : laidOut.error().message.c_str());
            const auto& rows = laidOut.value().devices(core::WallExchange::BesideTheList::wasLookedAt);
            QCOMPARE(rows.size(), 2);
            QList<qsizetype> places;
            for (const core::WallExchange::Offered& row : rows) {
                QVERIFY2(row.canBeTaken, qPrintable(row.whyNot));
                places.append(row.place);
            }
            QCOMPARE(places, (QList<qsizetype>{1, 2}));
            const QList<core::WallExchange::Took> took = exchange.take(laidOut.value().number(), places);
            QCOMPARE(took.size(), 2);
            for (const core::WallExchange::Took& one : took) {
                QVERIFY2(one.came, one.came ? "" : one.came.error().message.c_str());
            }
            QCOMPARE(empty.kvms().size(), 2);
            for (qsizetype at = 0; at < 2; ++at) {
                const OwnKvm was = wall.kvms().value(at);
                const OwnKvm is = empty.kvms().value(at);
                QCOMPARE(is.kind, u"proxmox"_s);
                QCOMPARE(is.name, was.name);
                QCOMPARE(is.address, pve.address().toString());
                QCOMPARE(is.node, u"pve1"_s);
                QCOMPARE(is.vmid, was.vmid);
                QCOMPARE(is.user, pve.tokenId);
                QCOMPARE(is.group, u"lab"_s);
                QCOMPARE(is.machine, numberOf(pve.guests.value(was.vmid)));
                QVERIFY(is.knownByCertificate);
                QCOMPARE(is.certificate, certificateOf());
                QCOMPARE(empty.certificateOf(is.id), wall.certificateOf(was.id));
                QCOMPARE(empty.marksAPasswordFor(is.id), sealed);
                const QByteArray record = more.all.value(u"kvm:"_s + is.id);
                if (sealed) {
                    QVERIFY(record.startsWith("QKVM-KVM-2\n" + pve.address().toString().toUtf8() + '\n'));
                    QVERIFY(record.endsWith('\n' + pve.secret.toUtf8()));
                } else {
                    QVERIFY(record.isEmpty());
                }
            }
            QCOMPARE(empty.kvms().value(0).vmid, qint64{100});
            QCOMPARE(empty.kvms().value(1).vmid, qint64{101});
            if (sealed) {
                for (const OwnKvm& kvm : empty.kvms()) {
                    QTRY_COMPARE_WITH_TIMEOUT(statusOf(empty).value(kvm.id).state, u"online"_s, 10'000);
                }
                QCOMPARE(statusOf(empty).value(empty.kvms().value(0).id).model, u"VM 100 on pve1"_s);
                QCOMPARE(statusOf(empty).value(empty.kvms().value(1).id).model, u"VM 101 on pve1"_s);
            }
        }
    }

    // Review c9b-1, M2: a guest whose Proxmox shows another certificate now (its certificates made anew), which
    // somebody looked at and said yes to, is asked by its driver on lines held to that one, as before: no KVM's probe
    // tells a Proxmox, and its driver asks what answers before every sign-in. Its card works again, and so after a
    // restart.
    void aGuestWhoseNewCertificateIsSaidYesToWorksAgain()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        const quint16 port = static_cast<quint16>(pve->address().port());
        const QString uuid = pve->guests.value(100).uuid;
        std::unique_ptr<ProxmoxByHand> renewed;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quickPaces());
            ProxmoxAdding adding(wall, quickPaces());
            const auto offered = listed(adding, typedFor(*pve));
            QVERIFY(offered);
            const auto made = added(adding, typedFor(*pve), offered.value(), 100);
            QVERIFY2(made, made ? "" : made.error().message.c_str());
            const QString id = made.value().id;
            QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).state, u"online"_s, 10'000);
            pve.reset();
            renewed = std::make_unique<ProxmoxByHand>(proxmoxTls("second"), port);
            renewed->guests[100].uuid = uuid;
            QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).errorCode, u"cert_mismatch"_s, 10'000);
            const base::Result<void> yes = wall.knowBy(id, certificateOf("second"));
            QVERIFY2(yes, yes ? "" : yes.error().message.c_str());
            QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).state, u"online"_s, 10'000);
            QVERIFY(wall.kvms().value(0).askedWhatItIs);
            QCOMPARE(wall.kvms().value(0).certificate, certificateOf("second"));
            QVERIFY(!renewed->requests.isEmpty());
        }
        QString id;
        {
            core::Settings settings(file);
            LocalWall again(settings, secrets, quickPaces());
            id = again.kvms().value(0).id;
            QTRY_COMPARE_WITH_TIMEOUT(statusOf(again).value(id).state, u"online"_s, 10'000);
        }
        // (And one whose settings say it was not asked, as a build before this one wrote it: asked by its driver all
        // the same.)
        {
            core::Settings settings(file);
            core::Remembered before = settings.remembered();
            before.kvms[0].askedWhatItIs = false;
            QVERIFY(settings.remember(before));
        }
        core::Settings older(file);
        QVERIFY(!older.remembered().kvms.value(0).askedWhatItIs);
        LocalWall fromThen(older, secrets, quickPaces());
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(fromThen).value(id).state, u"online"_s, 10'000);
    }

    // Review c9b-1, M3: the guest was made anew under its number (another SMBIOS UUID), and its card says so. An edit
    // with its token's secret typed takes the virtual machine that has the number now, as the card's words say it does
    // (the web's identify, and its server's update_device); one with the kept secret does not, and changes nothing.
    void anEditWithTheSecretTypedTakesTheGuestThatHasTheNumberNow()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typedFor(pve));
        QVERIFY(offered);
        const auto made = added(adding, typedFor(pve), offered.value(), 100);
        QVERIFY2(made, made ? "" : made.error().message.c_str());
        const QString id = made.value().id;
        const QString before = wall.kvms().value(0).machine;
        pve.deleteGuest(100);
        Guest rebuilt;
        rebuilt.name = u"web"_s;
        QCOMPARE(pve.makeGuest(rebuilt), qint64{100});
        const QString now = numberOf(pve.guests.value(100));
        QVERIFY(now != before);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).errorCode, u"another_machine"_s, 10'000);
        core::KvmEditing::Typed typed;
        typed.name = u"web"_s;
        typed.address = wall.kvms().value(0).address;
        typed.user = pve.tokenId;
        typed.group = u"lab"_s;
        // With the kept secret: what it is known by stays (a name or a group alone is saved; a sign-in is held to the
        // number kept, and refused).
        core::KvmEditing::Outcome outcome = edited(wall, id, typed);
        QVERIFY(outcome.result || outcome.result.error().code == "another_machine");
        QCOMPARE(wall.kvms().value(0).machine, before);
        // With it typed: whoever types it says "this is it now".
        typed.password = pve.secret;
        outcome = edited(wall, id, typed);
        QVERIFY2(outcome.result, outcome.result ? "" : outcome.result.error().message.c_str());
        QCOMPARE(wall.kvms().value(0).machine, now);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).state, u"online"_s, 10'000);
    }

    // Review c9b-1, M5: a token's ID is 64 characters at the most everywhere, as the web's (_a_line(64)): one of 64 is
    // added, and read back whole at the next start; one longer is refused as it is typed, in the words for any user
    // name, and nothing is sent.
    void aTokensIdIsSixtyFourCharactersAtTheMost()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.tokenId = QString(40, u'u') + u"@pve!"_s + QString(19, u't');
        QCOMPARE(pve.tokenId.size(), 64);
        QVERIFY(core::proxmox::isTokenId(pve.tokenId));
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quickPaces());
            ProxmoxAdding adding(wall, quickPaces());
            const auto offered = listed(adding, typedFor(pve));
            QVERIFY2(offered, offered ? "" : offered.error().message.c_str());
            const auto made = added(adding, typedFor(pve), offered.value(), 100);
            QVERIFY2(made, made ? "" : made.error().message.c_str());
        }
        const core::Settings again(file);
        QCOMPARE(again.remembered().kvms.value(0).user, pve.tokenId);
        ProxmoxByHand longer(proxmoxTls());
        longer.tokenId = QString(41, u'u') + u"@pve!"_s + QString(19, u't');
        QVERIFY(!core::proxmox::isTokenId(longer.tokenId));
        core::Settings settings(folder.filePath(u"other.json"_s));
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto refused = listed(adding, typedFor(longer));
        QCOMPARE(codeOf(refused), std::string("invalid"));
        QVERIFY2(refused.error().message.find("64 characters at the most") != std::string::npos,
                 refused.error().message.c_str());
        QCOMPARE(longer.stand->lines, 0);
    }

    // Review c9b-1, L4: its Proxmox has a new address, where it shows another certificate. An edit that types the new
    // address and the token's secret moves the guest there once the certificate shown there was said yes to (the
    // probe asks there what it asks anywhere, with nothing of the token; the token goes with the sign-in alone).
    void aGuestIsMovedToItsProxmoxsNewAddressWithItsSecretTyped()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typedFor(pve));
        QVERIFY(offered);
        const auto made = added(adding, typedFor(pve), offered.value(), 100);
        QVERIFY2(made, made ? "" : made.error().message.c_str());
        const QString id = made.value().id;
        ProxmoxByHand moved(proxmoxTls("second"));
        moved.guests[100].uuid = pve.guests.value(100).uuid;
        core::KvmEditing::Typed typed;
        typed.name = u"web"_s;
        typed.address = moved.address().toString();
        typed.user = pve.tokenId;
        typed.password = pve.secret;
        typed.group = u"lab"_s;
        core::KvmEditing::Outcome outcome = edited(wall, id, typed);
        QCOMPARE(codeOf(outcome.result), std::string("certificate_differs"));
        QCOMPARE(outcome.presented, certificateOf("second"));
        QVERIFY(moved.requests.isEmpty()); // (nothing with the token went before the yes)
        typed.saidYesTo = outcome.presented;
        outcome = edited(wall, id, typed);
        QVERIFY2(outcome.result, outcome.result ? "" : outcome.result.error().message.c_str());
        QCOMPARE(wall.kvms().value(0).address, moved.address().toString());
        QCOMPARE(wall.kvms().value(0).certificate, certificateOf("second"));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).state, u"online"_s, 10'000);
        // Not to plain HTTP.
        typed.address = u"http://127.0.0.1:%1"_s.arg(moved.address().port());
        typed.saidYesTo.clear();
        outcome = edited(wall, id, typed);
        QVERIFY(!outcome.result);
        QCOMPARE(wall.kvms().value(0).address, moved.address().toString());
    }

    // Review cx3-3, M1, at an edit: a guest's Proxmox typed in the box that changes its card is read as the Add box
    // reads it, and as the web reads it: without a port, at Proxmox's own 8006 (so here at the place of the other
    // card's guest, a guest of the same number at 8006, which is refused before anything is asked), HTTPS alone; with
    // its port written, 443 too, at that port (the box shows it so: test_ui_own_wall_proxmox), and a guest of a Proxmox
    // at 443 stays where it is when it is only named anew.
    void aGuestsProxmoxTypedAtAnEditIsReadAsTheAddBoxReadsIt_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("code");
        QTest::newRow("no port") << u"127.0.0.1"_s << u"duplicate"_s;
        QTest::newRow("https and no port") << u"https://127.0.0.1"_s << u"duplicate"_s;
        QTest::newRow("in the clear") << u"http://127.0.0.1:8007"_s << u"invalid"_s;
        QTest::newRow("its port written") << u"127.0.0.1:443"_s << u"ok"_s;
        QTest::newRow("its port written, in full") << u"https://127.0.0.1:443/"_s << u"ok"_s;
    }

    void aGuestsProxmoxTypedAtAnEditIsReadAsTheAddBoxReadsIt()
    {
        QFETCH(QString, typed);
        QFETCH(QString, code);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const auto guest = [](const QString& id, const QString& address) {
            return QJsonObject{{u"id"_s, id},
                               {u"kind"_s, u"proxmox"_s},
                               {u"name"_s, u"web"_s},
                               {u"address"_s, address},
                               {u"node"_s, u"pve1"_s},
                               {u"vmid"_s, 100},
                               {u"user"_s, u"wall@pve!qkvm"_s}};
        };
        {
            QFile written(file);
            QVERIFY(written.open(QIODevice::WriteOnly));
            written.write(
                QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{guest(u"0a0b0c0d"_s, u"https://127.0.0.1"_s),
                                                                 guest(u"0e0f0a0b"_s, u"https://127.0.0.1:8006"_s)}}})
                    .toJson());
        }
        core::Settings settings(file);
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        QCOMPARE(wall.kvms().size(), 2);
        core::KvmEditing::Typed edit;
        edit.name = u"behind a proxy"_s;
        edit.address = typed;
        edit.user = u"wall@pve!qkvm"_s;
        const core::KvmEditing::Outcome outcome = edited(wall, u"0a0b0c0d"_s, edit);
        QCOMPARE(QString::fromStdString(codeOfTheChange(outcome)), code);
        if (code == u"invalid"_s) {
            QVERIFY2(outcome.result.error().message.find("spoken over HTTPS only") != std::string::npos,
                     outcome.result.error().message.c_str());
        }
        QCOMPARE(wall.kvms().value(0).address, u"https://127.0.0.1"_s);
        QCOMPARE(wall.kvms().value(0).name, code == u"ok"_s ? u"behind a proxy"_s : u"web"_s);
        QCOMPARE(wall.kvms().value(1).address, u"https://127.0.0.1:8006"_s);
    }

    // The web's test_a_guest_goes_into_an_export_file_and_comes_out_of_it and
    // test_the_number_a_guest_gives_goes_into_an_export_file_and_comes_out_of_it: in the web's file format, its url the
    // web's (https://host:8006/nodes/<node>/qemu/<vmid>), the token's ID as its user name, its secret as its password
    // in a sealed file and none in a plain one, the number it gives for itself as its machine; read back as it went.
    void aGuestGoesIntoAFileAndComesOutOfIt()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        ProxmoxAdding adding(wall, quickPaces());
        const auto offered = listed(adding, typedFor(pve));
        QVERIFY(offered);
        QVERIFY(added(adding, typedFor(pve), offered.value(), 100));
        // (Its certificate itself is kept once its driver has shown it: at its first look.)
        QTRY_VERIFY_WITH_TIMEOUT(!statusOf(wall).isEmpty() && !wall.certificateOf(wall.kvms().value(0).id).isEmpty(),
                                 10'000);
        const QString url = pve.address().toString() + u"/nodes/pve1/qemu/100"_s;
        // Plain: no secret in it.
        const LocalWall::AsAFile plain = wall.asAFile(false);
        const auto plainFile = core::exchange::write(plain.content, QString());
        QVERIFY2(plainFile, plainFile ? "" : plainFile.error().message.c_str());
        const QByteArray text = plainFile.value().file(core::exchange::NotInIt::wasLookedAt);
        const QJsonObject device =
            QJsonDocument::fromJson(text).object().value("devices"_L1).toArray().at(0).toObject();
        QCOMPARE(device.value("url"_L1).toString(), url);
        QCOMPARE(device.value("kind"_L1).toString(), u"proxmox"_s);
        QCOMPARE(device.value("username"_L1).toString(), pve.tokenId);
        QCOMPARE(device.value("machine"_L1).toString(), numberOf(pve.guests.value(100)));
        QVERIFY(!text.contains(pve.secret.toUtf8()));
        // Sealed, with its secret; read back as it went.
        const LocalWall::AsAFile sealed = wall.asAFile(true);
        const auto sealedFile = core::exchange::write(sealed.content, u"a passphrase of ours"_s);
        QVERIFY(sealedFile);
        const auto read = core::exchange::read(sealedFile.value().file(core::exchange::NotInIt::wasLookedAt),
                                               u"a passphrase of ours"_s);
        QVERIFY2(read, read ? "" : read.error().message.c_str());
        const core::exchange::Device back = read.value().devices.value_or(QList<core::exchange::Device>{}).value(0);
        QCOMPARE(back.kind, u"proxmox"_s);
        QCOMPARE(back.address, pve.address().toString());
        QCOMPARE(back.node, u"pve1"_s);
        QCOMPARE(back.vmid, qint64{100});
        QCOMPARE(back.user, pve.tokenId);
        QCOMPARE(back.password, pve.secret);
        QCOMPARE(back.machine, numberOf(pve.guests.value(100)));
        QCOMPARE(back.tls, u"pinned"_s);
        QCOMPARE(back.certificate, certificateOf());
        // A guest's url that names no guest leaves the device out.
        QByteArray odd = text;
        odd.replace(url.toUtf8(), pve.address().toString().toUtf8());
        const auto oddRead = core::exchange::read(odd, QString());
        QVERIFY(oddRead);
        QVERIFY(oddRead.value().devices.value_or(QList<core::exchange::Device>{}).isEmpty());
        // Taken from the file on another wall: the same guest, its number kept, which refuses another VM under it.
        const QTemporaryDir another;
        core::Settings otherSettings(another.filePath(u"settings.json"_s));
        MemorySecrets otherSecrets;
        LocalWall otherWall(otherSettings, otherSecrets, quickPaces());
        core::WallExchange exchange(otherWall);
        const auto laidOut = answerOf<core::WallExchange::LaidOut>(
            [&](std::function<void(base::Result<core::WallExchange::LaidOut>)> done) {
                exchange.layOut(sealedFile.value().file(core::exchange::NotInIt::wasLookedAt),
                                u"a passphrase of ours"_s, &exchange, std::move(done));
            });
        QVERIFY2(laidOut, laidOut ? "" : laidOut.error().message.c_str());
        const auto& rows = laidOut.value().devices(core::WallExchange::BesideTheList::wasLookedAt);
        QCOMPARE(rows.size(), 1);
        QVERIFY2(rows.at(0).canBeTaken, qPrintable(rows.at(0).verdict));
        QCOMPARE(rows.at(0).verdict, u"It can be taken, as a virtual machine on Proxmox VE."_s);
        const QList<core::WallExchange::Took> took = exchange.take(laidOut.value().number(), {rows.at(0).place});
        QCOMPARE(took.size(), 1);
        QVERIFY(took.first().came);
        const OwnKvm& taken = otherWall.kvms().value(0);
        QCOMPARE(taken.kind, u"proxmox"_s);
        QCOMPARE(taken.node, u"pve1"_s);
        QCOMPARE(taken.vmid, qint64{100});
        QCOMPARE(taken.machine, numberOf(pve.guests.value(100)));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(otherWall).value(taken.id).state, u"online"_s, 10'000);
        pve.deleteGuest(100);
        Guest other;
        other.name = u"prod-db"_s;
        other.uptime = 5;
        pve.makeGuest(other);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(otherWall).value(taken.id).errorCode, u"another_machine"_s, 10'000);
    }

    // After the rebase onto the VNC servers (desktop 0.2.0 has both families): a wall that holds a VNC server and a
    // Proxmox guest at once keeps each in its own family on every road. Added side by side, two cards, neither at the
    // other's place; each card's `can` its family's; each one's word on what may have been done its family's, and of
    // a machine no longer on the wall every family's (the VNC servers' among them); each changed in its own box; both
    // into the wall's file, as the web writes them, and out of it on another wall.
    void aVncServerAndAGuestShareOneWall()
    {
        ProxmoxByHand pve(proxmoxTls());
        VncByHand server;
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        const core::DriverPaces paces = bothFamilies();
        LocalWall wall(settings, secrets, paces);
        core::KvmAdding::Pace addingPace;
        addingPace.driver = paces.kvmd;
        addingPace.vnc = paces.vnc;
        core::KvmAdding kvmAdding(wall, addingPace);
        core::KvmAdding::Typed vncTyped;
        vncTyped.address = server.url();
        vncTyped.password = u"secret"_s;
        vncTyped.group = u"lab"_s;
        const auto vncAdded =
            waitedFor<core::KvmAdding::Added>([&](std::function<void(base::Result<core::KvmAdding::Added>)> done) {
                kvmAdding.add(vncTyped, std::move(done));
            });
        QVERIFY2(vncAdded, vncAdded ? "" : vncAdded.error().message.c_str());
        ProxmoxAdding adding(wall, paces);
        const auto offered = listed(adding, typedFor(pve));
        QVERIFY(offered);
        // (The VNC server is no guest of the Proxmox's, and the list says nothing of it.)
        QVERIFY(offered.value().onTheWall.isEmpty() && offered.value().newUnder.isEmpty());
        const auto guestAdded = added(adding, typedFor(pve), offered.value(), 100);
        QVERIFY2(guestAdded, guestAdded ? "" : guestAdded.error().message.c_str());
        QCOMPARE(wall.kvms().size(), 2);
        const OwnKvm vnc = wall.kvms().at(0);
        const OwnKvm guest = wall.kvms().at(1);
        QCOMPARE(vnc.kind, u"vnc"_s);
        QCOMPARE(vnc.address, server.url());
        QCOMPARE(guest.kind, u"proxmox"_s);
        QCOMPARE(guest.vmid, qint64{100});
        QVERIFY(!LocalWall::atOnePlace(vnc, guest));
        QVERIFY(secrets.all.contains(u"kvm:"_s + vnc.id) && secrets.all.contains(u"kvm:"_s + guest.id));
        // Each card by its family: the VNC server's screen and keys, the guest's power and media too.
        QTRY_VERIFY_WITH_TIMEOUT(statusOf(wall).value(vnc.id).state == u"online"_s &&
                                     statusOf(wall).value(guest.id).state == u"online"_s,
                                 15'000);
        const QStringList vncCan = statusOf(wall).value(vnc.id).capabilities.value_or(QStringList());
        const QStringList guestCan = statusOf(wall).value(guest.id).capabilities.value_or(QStringList());
        QVERIFY2(vncCan.contains(u"screen"_s) && vncCan.contains(u"keys"_s), qPrintable(vncCan.join(u',')));
        QVERIFY2(!vncCan.contains(u"power"_s) && !vncCan.contains(u"media"_s), qPrintable(vncCan.join(u',')));
        QVERIFY2(guestCan.contains(u"power"_s) && guestCan.contains(u"media"_s) && guestCan.contains(u"screen"_s),
                 qPrintable(guestCan.join(u',')));
        // What may have been done: each by its family's word; of a machine that is gone, by every family's.
        const base::Error stopped{"stopped", "Stopped on the way"};
        const base::Error timeout{"timeout", "Device did not respond in time"};
        QVERIFY(wall.mayHaveBeenDone(vnc.id, stopped));
        QVERIFY(!wall.mayHaveBeenDone(guest.id, stopped));
        QVERIFY(wall.mayHaveBeenDone(guest.id, timeout));
        QVERIFY(!wall.mayHaveBeenDone(vnc.id, timeout));
        QVERIFY(wall.mayHaveBeenDone(u"0badf00d"_s, stopped));
        QVERIFY(wall.mayHaveBeenDone(u"0badf00d"_s, timeout));
        // Each changed in its own box: named anew, and nothing else of it.
        core::KvmEditing::Pace editPace = editingPace();
        editPace.vnc = paces.vnc;
        const auto renamed = [&wall, &editPace](const OwnKvm& one, const QString& name) {
            core::KvmEditing editing(wall, editPace);
            core::KvmEditing::Typed typed;
            typed.name = name;
            typed.address = one.address;
            typed.user = one.user;
            typed.group = one.group;
            std::optional<core::KvmEditing::Outcome> told;
            editing.change(one.id, typed, [&told](const core::KvmEditing::Outcome& outcome) { told = outcome; });
            static_cast<void>(QTest::qWaitFor([&told] { return told.has_value(); }, 30'000));
            return told && told->result.ok();
        };
        QVERIFY(renamed(vnc, u"Build box"_s));
        QVERIFY(renamed(guest, u"Build VM"_s));
        QCOMPARE(wall.kvms().at(0).name, u"Build box"_s);
        QCOMPARE(wall.kvms().at(0).kind, u"vnc"_s);
        QCOMPARE(wall.kvms().at(0).address, server.url());
        QCOMPARE(wall.kvms().at(1).name, u"Build VM"_s);
        QCOMPARE(wall.kvms().at(1).kind, u"proxmox"_s);
        QCOMPARE(wall.kvms().at(1).node, u"pve1"_s);
        QCOMPARE(wall.kvms().at(1).vmid, qint64{100});
        // Into the wall's file, as the web writes both: the VNC server by its vnc:// url, the guest by its own.
        const LocalWall::AsAFile sealed = wall.asAFile(true);
        const auto sealedFile = core::exchange::write(sealed.content, u"a passphrase of ours"_s);
        QVERIFY2(sealedFile, sealedFile ? "" : sealedFile.error().message.c_str());
        const QByteArray file = sealedFile.value().file(core::exchange::NotInIt::wasLookedAt);
        const auto read = core::exchange::read(file, u"a passphrase of ours"_s);
        QVERIFY2(read, read ? "" : read.error().message.c_str());
        const QList<core::exchange::Device> devices = read.value().devices.value_or(QList<core::exchange::Device>{});
        QCOMPARE(devices.size(), 2);
        QCOMPARE(devices.at(0).kind, u"vnc"_s);
        QCOMPARE(devices.at(0).address, server.url());
        QCOMPARE(devices.at(0).password, u"secret"_s);
        QCOMPARE(devices.at(1).kind, u"proxmox"_s);
        QCOMPARE(devices.at(1).vmid, qint64{100});
        QCOMPARE(devices.at(1).password, pve.secret);
        // Out of it on another wall: each as its family, both taken.
        const QTemporaryDir another;
        core::Settings otherSettings(another.filePath(u"settings.json"_s));
        MemorySecrets otherSecrets;
        LocalWall otherWall(otherSettings, otherSecrets, paces);
        core::WallExchange exchange(otherWall);
        const auto laidOut = answerOf<core::WallExchange::LaidOut>(
            [&](std::function<void(base::Result<core::WallExchange::LaidOut>)> done) {
                exchange.layOut(file, u"a passphrase of ours"_s, &exchange, std::move(done));
            });
        QVERIFY2(laidOut, laidOut ? "" : laidOut.error().message.c_str());
        const auto& rows = laidOut.value().devices(core::WallExchange::BesideTheList::wasLookedAt);
        QCOMPARE(rows.size(), 2);
        QVERIFY2(rows.at(0).canBeTaken && rows.at(1).canBeTaken,
                 qPrintable(rows.at(0).verdict + u" / "_s + rows.at(1).verdict));
        QCOMPARE(rows.at(0).verdict, u"It can be taken, as a VNC server."_s);
        QCOMPARE(rows.at(1).verdict, u"It can be taken, as a virtual machine on Proxmox VE."_s);
        const QList<core::WallExchange::Took> took =
            exchange.take(laidOut.value().number(), {rows.at(0).place, rows.at(1).place});
        QCOMPARE(took.size(), 2);
        QVERIFY(took.at(0).came && took.at(1).came);
        QCOMPARE(otherWall.kvms().size(), 2);
        QCOMPARE(otherWall.kvms().at(0).kind, u"vnc"_s);
        QCOMPARE(otherWall.kvms().at(0).address, server.url());
        QCOMPARE(otherWall.kvms().at(1).kind, u"proxmox"_s);
        QCOMPARE(otherWall.kvms().at(1).vmid, qint64{100});
        QTRY_VERIFY_WITH_TIMEOUT(statusOf(otherWall).value(otherWall.kvms().at(0).id).state == u"online"_s &&
                                     statusOf(otherWall).value(otherWall.kvms().at(1).id).state == u"online"_s,
                                 15'000);
    }
};

QTEST_GUILESS_MAIN(TestLocalWallProxmox)
#include "test_local_wall_proxmox.moc"
