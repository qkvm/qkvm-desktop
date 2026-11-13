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
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/wall_exchange.h>

#include "exchange_by_hand.h"
#include "memory_secrets.h"
#include "redfish_by_hand.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmAdding;
using core::KvmEditing;
using core::LocalWall;
using core::OwnKvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::MemorySecrets;
using core::testing::RedfishByHand;
using core::testing::StandIn;

namespace {

using Layout = RedfishByHand::Layout;

core::testing::Tls tls(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

QByteArray sha256Of(const char* name)
{
    return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
}

// A number as a controller's driver keeps it (redfish::serverNumber, redfish::controllerNumber): the first 32
// hexadecimal digits of the SHA-256 of its small letters, the computer's behind "server:".
QString digestOf(const QJsonValue& number)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(number.toString().toLower().toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}
QString markOf(const QJsonValue& number)
{
    return u"server:"_s + digestOf(number);
}
QString bothOf(const RedfishByHand& bmc)
{
    return markOf(bmc.uuid) + digestOf(bmc.rootUuid);
}

// A board whose maker set no number for its computer.
const QString zero = u"00000000-0000-0000-0000-000000000000"_s;

// The times a kvmd's driver keeps, made short; with room to be let in over TLS on a busy PC. Every family's are
// alike these (DriverPaces::alike), a controller's by the web's ratios.
core::KvmdDevice::Pace quickKvmd()
{
    core::KvmdDevice::Pace pace;
    pace.connectWithinMs = 5000;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.freshMs = 200;
    pace.firstContactMs = 400;
    pace.patienceMs = 400;
    // (A pacing of the test's own: nothing of one test's sign-ins reaches another's.)
    pace.pacing = std::make_shared<core::SignInPacing>();
    return pace;
}

// This PC's own wall for one test: its settings in a folder that goes with the test, its secrets in memory,
// and the goodbyes of its controllers' drivers its own, to be counted.
struct OwnWall {
    // Over the settings in `in` (none: its own), with the secrets `kept` there from the start (as Windows keeps them
    // from one run to the next).
    explicit OwnWall(const QString& in = {}, const QHash<QString, QByteArray>& kept = {})
        : file(in.isEmpty() ? folder.filePath(u"settings.json"_s) : in),
          goodbyes(std::make_shared<core::ControllerGoodbyes>()), drivers(driversWith(goodbyes)), settings(file),
          wall(settings, withKept(secrets, kept), drivers)
    {}

    static MemorySecrets& withKept(MemorySecrets& secrets, const QHash<QString, QByteArray>& kept)
    {
        secrets.all = kept;
        return secrets;
    }

    static core::DriverPaces driversWith(const std::shared_ptr<core::ControllerGoodbyes>& goodbyes)
    {
        core::DriverPaces paces = core::DriverPaces::alike(quickKvmd());
        paces.redfish.goodbyes = goodbyes;
        paces.redfish.signOutWithinMs = 1000;
        // (A look that fails says so at once, unless a test is about that: as the driver's own tests.)
        paces.redfish.hiccupMs = 0;
        return paces;
    }

    // The boxes' times: the probe's the program's own (as the boxes' tests have them), the drivers' this wall's.
    KvmAdding::Pace boxes() const
    {
        KvmAdding::Pace pace;
        pace.probe = {};
        pace.driver = drivers.kvmd;
        pace.redfish = drivers.redfish;
        return pace;
    }

    QTemporaryDir folder;
    QString file;
    std::shared_ptr<core::ControllerGoodbyes> goodbyes;
    core::DriverPaces drivers;
    core::Settings settings;
    MemorySecrets secrets;
    LocalWall wall;
};

KvmAdding::Typed typedAt(const RedfishByHand& bmc, const QString& name = {})
{
    KvmAdding::Typed typed;
    typed.address = bmc.address().toString();
    typed.password = u"secret"_s;
    typed.name = name;
    return typed;
}

base::Result<KvmAdding::Added> addedBy(KvmAdding& adding, const KvmAdding::Typed& typed)
{
    QList<base::Result<KvmAdding::Added>> told;
    adding.add(typed, [&told](const base::Result<KvmAdding::Added>& result) { told.append(result); });
    if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 60000)) {
        return base::Error{"never", "The adding never ended."};
    }
    return told.constFirst();
}

KvmEditing::Outcome changedBy(KvmEditing& editing, const QString& id, const KvmEditing::Typed& typed)
{
    QList<KvmEditing::Outcome> told;
    editing.change(id, typed, [&told](KvmEditing::Outcome outcome) { told.append(std::move(outcome)); });
    if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 60000)) {
        return KvmEditing::Outcome{base::Error{"never", "The change never ended."}, {}, {}, {}, false};
    }
    return told.constFirst();
}

template <typename T>
std::string codeOf(const base::Result<T>& result)
{
    return result.ok() ? std::string("(ok)") : result.error().code;
}

std::optional<OwnKvm> kvmOf(const LocalWall& wall, const QString& id)
{
    for (const OwnKvm& kvm : wall.kvms()) {
        if (kvm.id == id) {
            return kvm;
        }
    }
    return std::nullopt;
}

// What stands in the change box for a KVM as it is on the wall.
KvmEditing::Typed asItIs(const LocalWall& wall, const QString& id)
{
    const OwnKvm kvm = kvmOf(wall, id).value_or(OwnKvm{});
    KvmEditing::Typed typed;
    typed.name = kvm.name;
    typed.address = kvm.address;
    typed.user = kvm.user;
    typed.group = kvm.group;
    return typed;
}

template <typename Answer>
base::Result<Answer> answerTo(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
{
    QList<base::Result<Answer>> answers;
    ask([&](base::Result<Answer> answer) { answers.append(std::move(answer)); });
    if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 30000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

QHash<QString, core::Status> statusOf(LocalWall& wall)
{
    const base::Result<QHash<QString, core::Status>> all =
        answerTo<QHash<QString, core::Status>>([&](auto done) { wall.askStatus(std::move(done)); });
    return all.ok() ? all.value() : QHash<QString, core::Status>();
}

// How it is once it has been looked at: asked until it says `state`, or for a while.
core::Status cameTo(LocalWall& wall, const QString& id, const QString& state)
{
    core::Status status;
    static_cast<void>(QTest::qWaitFor(
        [&] {
            status = statusOf(wall).value(id);
            return status.state == state;
        },
        30000));
    return status;
}

// A wall's file: a server's controller as a file of the wall holds one, at its stand-in's address, known by the
// certificate it shows, its password with it, and kept by these numbers (`machine`; the web's).
core::exchange::Device inAFile(const RedfishByHand& bmc, const QString& machine, const char* certificate = "first")
{
    core::exchange::Device device;
    device.address = bmc.address().toString();
    device.name = u"db-01"_s;
    device.user = u"admin"_s;
    device.password = u"secret"_s;
    device.kind = u"redfish"_s;
    device.tls = u"pinned"_s;
    QFile pem(tls(certificate).certificateFile);
    const std::optional<core::exchange::KeptCertificate> kept =
        core::exchange::certificateFrom(pem.open(QIODevice::ReadOnly) ? QString::fromLatin1(pem.readAll()) : QString());
    device.certificatePem = kept ? kept->pem : QString();
    device.certificate = kept ? kept->sha256 : QByteArray();
    device.machine = machine;
    return device;
}

QByteArray fileOf(const QList<core::exchange::Device>& devices)
{
    core::exchange::Content content;
    content.devices = devices;
    const base::Result<core::exchange::Written> written =
        core::exchange::writeWith(content, core::SecretBytes::utf8Of(core::testing::thePassphrase()),
                                  core::exchange::Sealing{core::testing::quickCost, core::exchange::windowsDerive()});
    return written ? core::testing::textOf(written.value()) : QByteArray();
}

// A file laid out by the wall's exchange, and the places of all its devices taken: what came of each.
QList<core::WallExchange::Took> takenFrom(core::WallExchange& exchange, const QByteArray& file,
                                          QList<core::WallExchange::Offered>* rows = nullptr)
{
    QObject asker;
    QList<base::Result<core::WallExchange::LaidOut>> told;
    exchange.layOut(file, core::testing::thePassphrase(), &asker,
                    [&told](const base::Result<core::WallExchange::LaidOut>& result) { told.append(result); });
    if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 30000) || !told.constFirst()) {
        return {};
    }
    const QList<core::WallExchange::Offered> offered =
        told.constFirst().value().devices(core::WallExchange::BesideTheList::wasLookedAt);
    if (rows != nullptr) {
        *rows = offered;
    }
    QList<qsizetype> places;
    for (const core::WallExchange::Offered& row : offered) {
        places.append(row.place);
    }
    return exchange.take(told.constFirst().value().number(), places);
}

// Never Basic, never a cookie: whatever the test, no question that reached the controller carried either.
bool signsOnToNothing(const RedfishByHand& bmc)
{
    return bmc.authorizations == 0 && bmc.cookies == 0;
}

} // namespace

// A server's management controller on this PC's own wall (M7 1b-i): added in the Add box, known by the numbers
// its computer and its controller give for themselves (never by the certificate its maker may have put on every
// board), its card, its buttons' words, and its sessions given back at the program's end. Against RedfishByHand.
class TestWallServers : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void theProgramsTimesForAControllerAreTheWebs()
    {
        // A controller's driver on the wall and in the boxes keeps the web's times (redfish.py), made by the web's
        // ratios from a kvmd's (DriverPaces::alike): so a test's short kvmd is short for it too.
        const core::RedfishDevice::Pace web;
        const core::RedfishDevice::Pace made = core::DriverPaces::alike(core::KvmdDevice::Pace{}).redfish;
        QCOMPARE(made.connectWithinMs, web.connectWithinMs);
        QCOMPARE(made.quietMs, web.quietMs);
        QCOMPARE(made.transferMs, web.transferMs);
        QCOMPARE(made.signInQuietMs, web.signInQuietMs);
        QCOMPARE(made.rootUnsignedMs, web.rootUnsignedMs);
        QCOMPARE(made.holdMs, web.holdMs);
        QCOMPARE(made.longestHoldMs, web.longestHoldMs);
        QCOMPARE(made.refusedHoldMs, web.refusedHoldMs);
        QCOMPARE(made.signInRetryMs, web.signInRetryMs);
        QCOMPARE(made.freshMs, web.freshMs);
        QCOMPARE(made.retryLookMs, web.retryLookMs);
        QCOMPARE(made.hiccupMs, web.hiccupMs);
        QCOMPARE(made.faultHoldMs, web.faultHoldMs);
        QCOMPARE(made.stillThereEveryMs, web.stillThereEveryMs);
        QCOMPARE(made.signOutWithinMs, web.signOutWithinMs);
        // The boxes' and the wall's: the same.
        QCOMPARE(KvmAdding::Pace{}.drivers().redfish.rootUnsignedMs, web.rootUnsignedMs);
        // The Add box waits for a controller's root as its driver waits for it before a sign-in.
        QCOMPARE(KvmAdding::Pace{}.probe.rootQuietMs, web.rootUnsignedMs);
        // A test's short kvmd: short for a controller too, in the same ratios.
        const core::RedfishDevice::Pace shortened = OwnWall::driversWith(nullptr).redfish;
        QCOMPARE(shortened.quietMs, quickKvmd().quietMs * 5 / 2);
        QCOMPARE(shortened.rootUnsignedMs, shortened.quietMs * 2);
        QCOMPARE(shortened.freshMs, quickKvmd().freshMs * 4);
    }

    void aControllerIsAddedAndItsServerKeptByItsNumbers()
    {
        // W1 (the web's test_twins.py, test_a_server_is_known_by_its_own_number): "redfish", named by the name its
        // computer goes by, its model and its controller's version, known by the certificate it showed, its
        // numbers kept; the sign-in of the box given back, and the wall's driver holding one session.
        OwnWall own;
        RedfishByHand bmc(Layout::Ami, tls("first"));
        bmc.hostname = u"db-01"_s;
        KvmAdding adding(own.wall, own.boxes());
        const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(bmc));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(added.value().name, u"db-01"_s);
        QCOMPARE(added.value().model, u"SRV-2200"_s);
        QCOMPARE(added.value().version, u"1.42.0"_s);
        QCOMPARE(added.value().certificate, sha256Of("first"));
        QVERIFY(!added.value().overPlainHttp);
        const OwnKvm kvm = kvmOf(own.wall, added.value().id).value_or(OwnKvm{});
        QCOMPARE(kvm.kind, u"redfish"_s);
        QCOMPARE(kvm.address, bmc.address().toString());
        QVERIFY(kvm.knownByCertificate);
        QCOMPARE(kvm.certificate, sha256Of("first"));
        QCOMPARE(kvm.number, markOf(bmc.uuid));
        QCOMPARE(kvm.machine, bothOf(bmc));
        QCOMPARE(cameTo(own.wall, kvm.id, u"online"_s).state, u"online"_s);
        QTRY_COMPARE_WITH_TIMEOUT(bmc.signedOut, 1, 10000);
        QCOMPARE(bmc.logins, 2);
        QCOMPARE(bmc.sessions.size(), 1);
        QVERIFY(signsOnToNothing(bmc));
        // Kept so from one run to the next.
        QVERIFY(own.settings.writings() >= 1);
        const core::Settings again(own.file);
        QCOMPARE(again.remembered().kvms.size(), 1);
        QCOMPARE(again.remembered().kvms.constFirst().machine, bothOf(bmc));
        QCOMPARE(again.remembered().kvms.constFirst().kind, u"redfish"_s);

        // One whose computer calls itself nothing is named by where it is.
        RedfishByHand nameless(Layout::OpenBmc, tls("second"));
        const base::Result<KvmAdding::Added> byWhere = addedBy(adding, typedAt(nameless));
        QVERIFY2(byWhere.ok(), byWhere.ok() ? "" : byWhere.error().message.c_str());
        QCOMPARE(byWhere.value().name, nameless.address().authority());
    }

    void theSameServerAtAnotherAddressIsNotASecondCard()
    {
        // W2: by the number its computer gives, which is said ("This says it is the same device as ..."), and refused
        // whatever is typed: the owner's word (M7 Q2), a server has one card on this PC's wall, and the web's "keep
        // both" is not offered. The sign-in that found it out is given back. Another server of the make is another
        // server; two whose maker gave them no number are two.
        OwnWall own;
        RedfishByHand one(Layout::Ami, tls("first"));
        one.hostname = u"db-01"_s;
        RedfishByHand again(Layout::Ami, tls("first"));
        again.beServer(one.uuid, one.rootUuid);
        KvmAdding adding(own.wall, own.boxes());
        QVERIFY(addedBy(adding, typedAt(one)).ok());
        KvmAdding::Typed typed = typedAt(again);
        for (const bool keepBoth : {false, true}) {
            typed.keepBoth = keepBoth;
            const base::Result<KvmAdding::Added> refused = addedBy(adding, typed);
            QCOMPARE(codeOf(refused), std::string("same_device"));
            QCOMPARE(QString::fromStdString(refused.error().message),
                     u"This says it is the same device as \"db-01\", which is on the wall already (127.0.0.1:%1). A "
                     "server has one card on this PC's wall."_s.arg(one.address().port()));
            QVERIFY(!LocalWall::saysItIsTheSame(refused.error()));
        }
        QCOMPARE(own.wall.kvms().size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(again.signedOut, again.logins, 10000);
        QVERIFY(again.sessions.isEmpty());
        // Another server of the same make, with the same certificate: another server.
        RedfishByHand sameMake(Layout::Ami, tls("first"));
        QVERIFY(addedBy(adding, typedAt(sameMake)).ok());
        // Two whose maker never gave their computers a number are not taken for one.
        RedfishByHand unsetA(Layout::Ami, tls("first"));
        unsetA.uuid = zero;
        RedfishByHand unsetB(Layout::Ami, tls("first"));
        unsetB.uuid = zero;
        QVERIFY(addedBy(adding, typedAt(unsetA)).ok());
        QVERIFY(addedBy(adding, typedAt(unsetB)).ok());
        QCOMPARE(own.wall.kvms().size(), 4);
        QVERIFY(signsOnToNothing(one) && signsOnToNothing(again));
    }

    void aServersPasswordDoesNotFollowItToAnotherControllerOfItsCertificate()
    {
        // W3 (the web's test_a_server_is_not_known_again_by_a_certificate_its_maker_may_have_put_on_every_board): two
        // controllers of one make showing the one certificate they left the factory with. Both are added; the first
        // moved to the second's address without a password is "password_needed", by the wall and by the change box,
        // and the second hears no sign-in. Whoever types the password says "this is the server, at this address".
        OwnWall own;
        RedfishByHand a(Layout::Ami, tls("first"));
        RedfishByHand b(Layout::Ami, tls("first"));
        KvmAdding adding(own.wall, own.boxes());
        const base::Result<KvmAdding::Added> first = addedBy(adding, typedAt(a, u"db-01"_s));
        const base::Result<KvmAdding::Added> second = addedBy(adding, typedAt(b, u"db-02"_s));
        QVERIFY2(first.ok(), first.ok() ? "" : first.error().message.c_str());
        QVERIFY2(second.ok(), second.ok() ? "" : second.error().message.c_str());
        QVERIFY(LocalWall::twinOf({kvmOf(own.wall, first.value().id).value_or(OwnKvm{})},
                                  kvmOf(own.wall, second.value().id).value_or(OwnKvm{})) == std::nullopt);
        QVERIFY(own.wall.remove(second.value().id).ok());
        const QString id = first.value().id;
        const int heard = b.logins;
        OwnKvm moved = kvmOf(own.wall, id).value_or(OwnKvm{});
        moved.address = b.address().toString();
        QCOMPARE(codeOf(own.wall.change(id, moved, std::nullopt)), std::string("password_needed"));
        QCOMPARE(codeOf(own.wall.signerIn(id, b.address(), u"admin"_s, own.drivers)), std::string("password_needed"));
        KvmEditing editing(own.wall, own.boxes());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = b.address().toString();
        QCOMPARE(codeOf(changedBy(editing, id, typed).result), std::string("password_needed"));
        QTest::qWait(300);
        QCOMPARE(b.logins, heard);
        QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).address, a.address().toString());
        // Typed: the server that answers there is the card's from now on.
        typed.password = u"secret"_s;
        const KvmEditing::Outcome typedIn = changedBy(editing, id, typed);
        QVERIFY2(typedIn.result.ok(), typedIn.result.ok() ? "" : typedIn.result.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).address, b.address().toString());
        QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).machine, bothOf(b));
        QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).number, markOf(b.uuid));
        QVERIFY(signsOnToNothing(a) && signsOnToNothing(b));
    }

    void aServersCardSaysHowItIsAndThatItHasNoPicture()
    {
        // W4: looked at by its driver, its card says how the server is (Status::server), and the wall says it has no
        // picture of a KVM's (Machine::hasPicture; its own screen, where its console can be had, is the status's
        // `screen`: test_wall_server_screens), and its page is its controller's own, plain (the owner's word, M7 Q6:
        // no pass in an address). One with no console that qKVM reads has no picture of its screen either.
        OwnWall own;
        RedfishByHand bmc(Layout::Ami, tls("first"));
        bmc.ownConsole = false;
        KvmAdding adding(own.wall, own.boxes());
        const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(bmc, u"db-01"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        const core::Status status = cameTo(own.wall, added.value().id, u"online"_s);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(status.server.has_value());
        QCOMPARE(status.power, std::optional<bool>(true));
        QVERIFY(!status.hasPicture);
        const base::Result<core::Wall> wall =
            answerTo<core::Wall>([&](auto done) { own.wall.askWall(std::move(done)); });
        QVERIFY(wall.ok());
        QCOMPARE(wall.value().machines.size(), 1);
        const core::Machine& machine = wall.value().machines.constFirst();
        QCOMPARE(machine.kind, u"redfish"_s);
        QVERIFY(!machine.hasPicture);
        QCOMPARE(machine.page, bmc.address());
        base::Result<core::Snapshot> picture = base::Error{"never", "Not asked."};
        QVERIFY(QTest::qWaitFor(
            [&] {
                picture = answerTo<core::Snapshot>(
                    [&](auto done) { own.wall.askSnapshot(added.value().id, 320, 0, std::move(done)); });
                return picture.ok() || picture.error().code != "no_stream";
            },
            20000));
        QCOMPARE(codeOf(picture), std::string("no_picture"));
        QVERIFY(!statusOf(own.wall).value(added.value().id).hasScreen);
        QCOMPARE(bmc.webLogins, 0);
    }

    void whatMayHaveBeenDoneToAServerIsItsDriversWord()
    {
        // W5: a press that went out and whose line fell under it ("dropped") may have been done, as a controller's
        // driver has it; and of a machine that is not on the wall, by the word of every family.
        OwnWall own;
        OwnKvm server;
        server.address = u"https://192.0.2.7"_s;
        server.kind = u"redfish"_s;
        const base::Result<QString> id = own.wall.add(server, u"secret"_s);
        QVERIFY(id.ok());
        const base::Error dropped{"dropped", "The line fell after it went out."};
        QCOMPARE(own.wall.mayHaveBeenDone(id.value(), dropped), core::RedfishDevice::mayHaveBeenDone(dropped));
        QVERIFY(own.wall.mayHaveBeenDone(id.value(), dropped));
        QVERIFY(own.wall.mayHaveBeenDone(id.value(), base::Error{"timeout", ""}));
        QVERIFY(!own.wall.mayHaveBeenDone(id.value(), base::Error{"unreachable", ""}));
        // (Its driver's word, and not a kvmd's, which says so of an action it does not know.)
        const base::Error unknown{"action_unknown", "No such action."};
        QVERIFY(!own.wall.mayHaveBeenDone(id.value(), unknown));
        QVERIFY(own.wall.mayHaveBeenDone(u"0badc0de"_s, dropped));
        QVERIFY(own.wall.mayHaveBeenDone(u"0badc0de"_s, unknown));
    }

    void aServerItsDriverTookIsKeptWithItsCard()
    {
        // A card that keeps no mark yet (one put on the wall without one): its driver takes the server that answers
        // at its first look (trust on first use), and the wall keeps that with the card, by itself, from one run to
        // the next. A box that was open over it meanwhile is not told it was changed: nobody changed it.
        OwnWall own;
        RedfishByHand bmc(Layout::Ami, tls("first"));
        OwnKvm server;
        server.address = bmc.address().toString();
        server.kind = u"redfish"_s;
        server.name = u"db-01"_s;
        server.knownByCertificate = true;
        server.certificate = sha256Of("first");
        const base::Result<QString> id = own.wall.add(server, u"secret"_s);
        QVERIFY(id.ok());
        KvmEditing::Typed typed = asItIs(own.wall, id.value());
        typed.seen =
            KvmEditing::Seen{kvmOf(own.wall, id.value()).value_or(OwnKvm{}), own.wall.secretWrites(id.value())};
        QVERIFY(kvmOf(own.wall, id.value()).value_or(OwnKvm{}).machine.isEmpty());
        QCOMPARE(cameTo(own.wall, id.value(), u"online"_s).state, u"online"_s);
        QTRY_COMPARE_WITH_TIMEOUT(kvmOf(own.wall, id.value()).value_or(OwnKvm{}).machine, bothOf(bmc), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(core::Settings(own.file).remembered().kvms.value(0).machine, bothOf(bmc), 10000);
        typed.name = u"db-01, renamed"_s;
        KvmEditing editing(own.wall, own.boxes());
        const KvmEditing::Outcome renamed = changedBy(editing, id.value(), typed);
        QVERIFY2(renamed.result.ok(), renamed.result.ok() ? "" : renamed.result.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id.value()).value_or(OwnKvm{}).machine, bothOf(bmc));
        // And no change sets a mark of its own where no password was typed for it.
        OwnKvm altered = kvmOf(own.wall, id.value()).value_or(OwnKvm{});
        altered.machine = u"server:0123456789abcdef0123456789abcdef"_s;
        QVERIFY(own.wall.change(id.value(), altered, std::nullopt).ok());
        QCOMPARE(kvmOf(own.wall, id.value()).value_or(OwnKvm{}).machine, bothOf(bmc));
    }

    void anotherServerAtItsAddressIsSentNothing()
    {
        // W7 (the web's test_which_server_answers.py, test_a_server_that_took_the_address_is_sent_neither_the_password_
        // nor_anything_pressed): its card says so, nothing pressed goes there, and no password where its controller's
        // number was kept. Its own server back there, its buttons are its own again.
        OwnWall own;
        RedfishByHand bmc(Layout::Ami, tls("first"));
        KvmAdding adding(own.wall, own.boxes());
        const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(bmc, u"db-01"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        const QString id = added.value().id;
        QCOMPARE(cameTo(own.wall, id, u"online"_s).state, u"online"_s);
        const QJsonValue system = bmc.uuid;
        const QJsonValue root = bmc.rootUuid;
        const int logins = bmc.logins;
        bmc.anotherServer();
        const core::Status status = cameTo(own.wall, id, u"error"_s);
        QCOMPARE(status.errorCode, u"another_machine"_s);
        const base::Result<core::Done> pressed = answerTo<core::Done>(
            [&](auto done) { own.wall.act(id, core::Action::plain(u"power_on"_s), std::move(done)); });
        QCOMPARE(codeOf(pressed), std::string("another_machine"));
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY(bmc.changes.isEmpty());
        QCOMPARE(bmc.logins, logins);
        bmc.beServer(system, root);
        QCOMPARE(cameTo(own.wall, id, u"online"_s).state, u"online"_s);
        const base::Result<core::Done> again = answerTo<core::Done>(
            [&](auto done) { own.wall.act(id, core::Action::plain(u"atx_power_long"_s), std::move(done)); });
        QVERIFY2(again.ok(), again.ok() ? "" : again.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system ForceOff"_s});
        QVERIFY(signsOnToNothing(bmc));
    }

    void theNextRunHoldsItsDriverToTheNumbersKept()
    {
        // W7 across a start of the program: the wall's driver, made from the settings, is held to the numbers kept with
        // the card. Another server at its address by then is said so, and is not sent the password (its controller's
        // number told it before).
        QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        RedfishByHand bmc(Layout::Ami, tls("first"));
        QString id;
        QHash<QString, QByteArray> kept;
        {
            OwnWall own(file);
            KvmAdding adding(own.wall, own.boxes());
            const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(bmc, u"db-01"_s));
            QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
            id = added.value().id;
            kept = own.secrets.all;
        }
        bmc.anotherServer();
        const int logins = bmc.logins;
        OwnWall next(file, kept);
        QCOMPARE(cameTo(next.wall, id, u"error"_s).errorCode, u"another_machine"_s);
        QCOMPARE(bmc.logins, logins);
        QVERIFY(signsOnToNothing(bmc));
    }

    void aKvmThatSaysNothingIsGivenUpOnInAKvmsTime()
    {
        // The wall waits for a controller's service root as its driver does (W9), with an asking of its own for that
        // family; a KVM's asking is not made to wait as long: one whose address takes the line and says nothing is
        // given up on in a KVM's time, as before.
        OwnWall own;
        StandIn silent([](const Asked& /*asked*/) {
            Answered never(200, "");
            never.never = true;
            return never;
        });
        LocalWall::FromAFile fromAFile;
        fromAFile.kvm.address = silent.address();
        fromAFile.kvm.kind = u"kvmd"_s;
        fromAFile.kvm.name = u"Rack"_s;
        QElapsedTimer watch;
        watch.start();
        const QList<base::Result<QString>> taken = own.wall.takeFromAFile({fromAFile});
        QCOMPARE(taken.size(), 1);
        QVERIFY2(taken.constFirst().ok(), taken.constFirst().ok() ? "" : taken.constFirst().error().message.c_str());
        const QString id = taken.constFirst().value();
        core::Status status;
        QVERIFY(QTest::qWaitFor(
            [&] {
                status = statusOf(own.wall).value(id);
                return status.state != "connecting"_L1;
            },
            20000));
        QVERIFY2(watch.elapsed() < own.drivers.redfish.quietMs,
                 qPrintable(u"given up on after %1 ms"_s.arg(watch.elapsed())));
        QVERIFY(!silent.asked.isEmpty());
    }

    void theChangeBoxTakesAnotherServerOnlyWhereItsPasswordIsTyped()
    {
        // W8 (the web's test_an_edit_takes_the_server_that_answers_now_only_where_its_password_is_typed): signed in to
        // with the kept password (under another user name), another server at its address is "another_machine",
        // nothing is saved and no password goes; typed, the server that answers now is the card's.
        OwnWall own;
        RedfishByHand bmc(Layout::Ami, tls("first"));
        KvmAdding adding(own.wall, own.boxes());
        const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(bmc, u"db-01"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        const QString id = added.value().id;
        const QString kept = bothOf(bmc);
        QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).machine, kept);
        bmc.anotherServer();
        const int logins = bmc.logins;
        KvmEditing editing(own.wall, own.boxes());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.user = u"operator"_s;
        QCOMPARE(codeOf(changedBy(editing, id, typed).result), std::string("another_machine"));
        QCOMPARE(bmc.logins, logins);
        QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).machine, kept);
        QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).user, u"admin"_s);
        // A new name alone asks nothing, and keeps the server it was added for.
        typed = asItIs(own.wall, id);
        typed.name = u"db-01, renamed"_s;
        QVERIFY(changedBy(editing, id, typed).result.ok());
        QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).machine, kept);
        // Typed: "this is the server".
        typed.name = u"db-02"_s;
        typed.password = u"secret"_s;
        const KvmEditing::Outcome typedIn = changedBy(editing, id, typed);
        QVERIFY2(typedIn.result.ok(), typedIn.result.ok() ? "" : typedIn.result.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).machine, bothOf(bmc));
        QCOMPARE(cameTo(own.wall, id, u"online"_s).state, u"online"_s);
        const base::Result<core::Done> pressed = answerTo<core::Done>(
            [&](auto done) { own.wall.act(id, core::Action::plain(u"atx_power_long"_s), std::move(done)); });
        QVERIFY2(pressed.ok(), pressed.ok() ? "" : pressed.error().message.c_str());
        QCOMPARE(bmc.actions, QStringList{u"system ForceOff"_s});
    }

    void aControllerSlowToShowItsRootIsOneAllTheSame()
    {
        // W9: a controller's service root, read with no session, is waited for as its driver waits for it before a
        // sign-in (twice as long as an answer: the web's ROOT_WAIT_TIMES), in the Add box and in the wall's asking. An
        // older board took nine to twenty seconds; here the test's times, in the same ratio.
        QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        RedfishByHand bmc(Layout::Ami);
        bmc.rootUnsignedTakesMs = 2500;
        QString id;
        QHash<QString, QByteArray> kept;
        {
            OwnWall own(file);
            KvmAdding::Pace pace = own.boxes();
            pace.probe = core::KvmProbe::Pace::beforeASignInWith(1500, 1500, 1500, 5000);
            KvmAdding adding(own.wall, pace);
            const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(bmc, u"db-01"_s));
            QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
            id = added.value().id;
            QCOMPARE(kvmOf(own.wall, id).value_or(OwnKvm{}).kind, u"redfish"_s);
            kept = own.secrets.all;
        }
        // Over plain HTTP it is asked again in every run, before its password goes anywhere. (The store is what stays
        // from one run to the next.)
        OwnWall next(file);
        next.secrets.all = kept;
        QCOMPARE(next.wall.kvms().size(), 1);
        QVERIFY(!next.wall.wasAskedWhatItIs(id));
        QCOMPARE(cameTo(next.wall, id, u"online"_s).state, u"online"_s);
        QVERIFY(next.wall.wasAskedWhatItIs(id));
    }

    void aServerOfAFileIsAskedFirstAndKeptByTheNumbersTheFileKeeps()
    {
        // M7 1b-ii (the web's test_which_server_answers.py, test_a_file_carries_the_numbers_a_server_is_known_by): the
        // file carries the numbers its server is kept by, and its card is held to them. Laid out as a server's
        // controller; taken as one that was not asked what it is, and asked first (its service root alone, nothing in
        // the question that says who asks) before its password goes; then signed in to and judged.
        OwnWall own;
        RedfishByHand bmc(Layout::Ami, tls("first"));
        core::WallExchange exchange(own.wall,
                                    core::exchange::Sealing{core::testing::quickCost, core::exchange::windowsDerive()});
        QList<core::WallExchange::Offered> rows;
        const QList<core::WallExchange::Took> took = takenFrom(exchange, fileOf({inAFile(bmc, bothOf(bmc))}), &rows);
        QCOMPARE(rows.size(), 1);
        QVERIFY(rows.constFirst().canBeTaken);
        QCOMPARE(rows.constFirst().verdict, u"It can be taken, as a server's management controller (BMC)."_s);
        QCOMPARE(took.size(), 1);
        QVERIFY2(took.constFirst().came.ok(),
                 took.constFirst().came.ok() ? "" : took.constFirst().came.error().message.c_str());
        const QString id = took.constFirst().came.value();
        const OwnKvm kvm = kvmOf(own.wall, id).value_or(OwnKvm{});
        QCOMPARE(kvm.kind, u"redfish"_s);
        QCOMPARE(kvm.machine, bothOf(bmc));
        QVERIFY(kvm.knownByCertificate);
        QCOMPARE(kvm.certificate, sha256Of("first"));
        QCOMPARE(cameTo(own.wall, id, u"online"_s).state, u"online"_s);
        // Asked first: what came before the first sign-in was read by anybody, and said nothing of who asks.
        const auto signIn =
            std::ranges::find_if(bmc.stand->asked, [](const Asked& one) { return one.method == "POST"; });
        QVERIFY(signIn != bmc.stand->asked.cend());
        QVERIFY(signIn != bmc.stand->asked.cbegin());
        for (auto one = bmc.stand->asked.cbegin(); one != signIn; ++one) {
            QCOMPARE(one->method, QByteArray("GET"));
            QVERIFY(one->body.isEmpty() && !one->headers.contains("x-auth-token"));
        }
        QVERIFY(signsOnToNothing(bmc));
    }

    void aFileNamingAnotherServersNumbersSendsItNothing()
    {
        // The file names the numbers of another server than the one that answers there now: its card says so, and
        // that server is not sent the password (its controller's number told it before), nor anything else.
        OwnWall own;
        RedfishByHand bmc(Layout::Ami, tls("first"));
        RedfishByHand somewhereElse(Layout::Ami, tls("first"));
        core::WallExchange exchange(own.wall,
                                    core::exchange::Sealing{core::testing::quickCost, core::exchange::windowsDerive()});
        const QList<core::WallExchange::Took> took = takenFrom(exchange, fileOf({inAFile(bmc, bothOf(somewhereElse))}));
        QCOMPARE(took.size(), 1);
        QVERIFY(took.constFirst().came.ok());
        const QString id = took.constFirst().came.value();
        QCOMPARE(cameTo(own.wall, id, u"error"_s).errorCode, u"another_machine"_s);
        QCOMPARE(bmc.logins, 0);
        QVERIFY(bmc.loginBodies.isEmpty());
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY(signsOnToNothing(bmc));
    }

    void twoServersOfAFileWithOneCertificateAreBothTaken()
    {
        // The maker's one certificate on two boards makes no twins of them in a file either: both are taken, each held
        // to its own numbers. (A JetKVM's or a kvmd's twin in a file is by its certificate first: test_wall_exchange.)
        OwnWall own;
        RedfishByHand a(Layout::Ami, tls("first"));
        RedfishByHand b(Layout::Ami, tls("first"));
        core::WallExchange exchange(own.wall,
                                    core::exchange::Sealing{core::testing::quickCost, core::exchange::windowsDerive()});
        core::exchange::Device second = inAFile(b, bothOf(b));
        second.name = u"db-02"_s;
        // (And a KVM known by that certificate after them: no twin of theirs either, the controllers' certificate
        // being nobody's to be known by.)
        core::exchange::Device kvm = inAFile(a, QString());
        kvm.address = u"https://127.0.0.1:9"_s;
        kvm.name = u"Rack"_s;
        kvm.kind = u"kvmd"_s;
        QList<core::WallExchange::Offered> rows;
        const QList<core::WallExchange::Took> took =
            takenFrom(exchange, fileOf({inAFile(a, bothOf(a)), second, kvm}), &rows);
        QCOMPARE(rows.size(), 3);
        QVERIFY(rows.at(0).canBeTaken && rows.at(1).canBeTaken && rows.at(2).canBeTaken);
        QCOMPARE(took.size(), 3);
        QVERIFY(took.at(0).came.ok() && took.at(1).came.ok() && took.at(2).came.ok());
        QCOMPARE(own.wall.kvms().size(), 3);
        QCOMPARE(kvmOf(own.wall, took.at(1).came.value()).value_or(OwnKvm{}).machine, bothOf(b));
        QVERIFY(own.wall.remove(took.at(2).came.value()).ok());
        // And the wall's own file names each by its own numbers, keeps neither beside the other, and is read back so.
        const LocalWall::AsAFile asAFile = own.wall.asAFile(false);
        const QList<core::exchange::Device> written = asAFile.content.devices.value_or(QList<core::exchange::Device>());
        QCOMPARE(written.size(), 2);
        QCOMPARE(written.at(0).machine, bothOf(a));
        QCOMPARE(written.at(1).machine, bothOf(b));
        QVERIFY(!written.at(0).keepBoth && !written.at(1).keepBoth);
        const base::Result<core::exchange::Content> again =
            core::exchange::readWith(fileOf(written), core::SecretBytes::utf8Of(core::testing::thePassphrase()),
                                     core::exchange::windowsDerive());
        QVERIFY(again.ok());
        const QList<core::exchange::Device> read = again.value().devices.value_or(QList<core::exchange::Device>());
        QCOMPARE(read.size(), 2);
        QCOMPARE(read.at(1).machine, bothOf(b));
        // And a server after a KVM known by that certificate in a file: no twin of that KVM either, laid out or taken.
        RedfishByHand c(Layout::Ami, tls("first"));
        core::exchange::Device third = inAFile(c, bothOf(c));
        third.name = u"db-03"_s;
        QList<core::WallExchange::Offered> after;
        const QList<core::WallExchange::Took> tookAfter = takenFrom(exchange, fileOf({kvm, third}), &after);
        QCOMPARE(after.size(), 2);
        QVERIFY(after.at(0).canBeTaken && after.at(1).canBeTaken);
        QCOMPARE(tookAfter.size(), 2);
        QVERIFY(tookAfter.at(0).came.ok() && tookAfter.at(1).came.ok());
        QCOMPARE(own.wall.kvms().size(), 4);
    }

    void aServerOfAFileIsNeverKeptBesideItsTwin()
    {
        // A server has one card on this PC's wall (the owner's word, M7 Q2), whatever a file says: one that says by its
        // computer's number that it is a server of the wall is not taken, though the file says to keep both.
        OwnWall own;
        RedfishByHand bmc(Layout::Ami, tls("first"));
        KvmAdding adding(own.wall, own.boxes());
        QVERIFY(addedBy(adding, typedAt(bmc, u"db-01"_s)).ok());
        RedfishByHand again(Layout::Ami, tls("second"));
        again.beServer(bmc.uuid, bmc.rootUuid);
        core::exchange::Device twin = inAFile(again, bothOf(again), "second");
        twin.ident = markOf(bmc.uuid);
        twin.keepBoth = true;
        core::WallExchange exchange(own.wall,
                                    core::exchange::Sealing{core::testing::quickCost, core::exchange::windowsDerive()});
        QList<core::WallExchange::Offered> rows;
        const QList<core::WallExchange::Took> took = takenFrom(exchange, fileOf({twin}), &rows);
        QCOMPARE(rows.size(), 1);
        QVERIFY(!rows.constFirst().canBeTaken);
        QCOMPARE(rows.constFirst().whyNot, u"same_device"_s);
        QCOMPARE(took.size(), 1);
        QCOMPARE(codeOf(took.constFirst().came), std::string("same_device"));
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(again.logins, 0);
    }

    void atTheProgramsEndEverySessionIsGivenBackAndNoneTaken()
    {
        // W10 (the reading of 1a-v, L4): the wall's drivers and a box's sign-in that is still out are let go of before
        // the goodbyes are waited for; their sessions come back within the goodbyes' time, and nothing signs in
        // meanwhile (no look of the wall's makes a driver or asks).
        OwnWall own;
        RedfishByHand bmc(Layout::Ami, tls("first"));
        KvmAdding adding(own.wall, own.boxes());
        const base::Result<KvmAdding::Added> added = addedBy(adding, typedAt(bmc, u"db-01"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(cameTo(own.wall, added.value().id, u"online"_s).state, u"online"_s);
        QTRY_COMPARE_WITH_TIMEOUT(bmc.sessions.size(), 1, 10000);
        // A box's sign-in to another, out when the program ends.
        RedfishByHand other(Layout::Ami, tls("second"));
        other.loginTakesMs = 600;
        auto box = std::make_unique<KvmAdding>(own.wall, own.boxes());
        box->add(typedAt(other), [](const base::Result<KvmAdding::Added>&) {});
        QVERIFY(QTest::qWaitFor([&other] { return other.logins == 1; }, 20000));
        // The program ends: as main.cpp does it at aboutToQuit.
        own.wall.letGoOfEveryDriver();
        box->giveUp();
        box.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const int logins = bmc.logins;
        QElapsedTimer watch;
        watch.start();
        own.goodbyes->finish(own.drivers.redfish.signOutWithinMs + 2000);
        QVERIFY(watch.elapsed() < own.drivers.redfish.signOutWithinMs + 2000);
        QVERIFY(bmc.sessions.isEmpty());
        QVERIFY(other.sessions.isEmpty());
        QCOMPARE(bmc.logins, logins);
        // And nothing of the wall asks again afterwards, nor is anything put on it.
        QVERIFY(statusOf(own.wall).isEmpty());
        OwnKvm late;
        late.address = other.address().toString();
        late.kind = u"redfish"_s;
        QCOMPARE(codeOf(own.wall.add(late, u"secret"_s)), std::string("stopped"));
        QTest::qWait(500);
        QCOMPARE(bmc.logins, logins);
        QVERIFY(bmc.sessions.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestWallServers)
#include "test_wall_servers.moc"
