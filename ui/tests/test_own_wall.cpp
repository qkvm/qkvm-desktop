#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QMetaProperty>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/server_client.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/wiped.h>

#include "add_kvm_view.h"
#include "edit_kvm_view.h"
#include "jetkvm_by_hand.h"
#include "machine_menu.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "own_files_view.h"
#include "own_wall.h"
#include "redfish_by_hand.h"
#include "wall_cards.h"

#ifdef _DEBUG
// Microsoft's C runtime in its debug form, whose heap the test program and Qt's debug libraries share:
// it shows a hook of the program's every block of memory before the block is given back.
#include <crtdbg.h>
#endif

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::JetKvmByHand;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;
using core::testing::RedfishByHand;
using core::testing::StandIn;

namespace {

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

#ifdef _DEBUG
// What is looked for in every block of memory that is given back while one of these is there: the
// bytes of a secret. A block that still holds them as it goes was given back without being written
// over, and leaves the secret in memory that nobody holds. (The runtime's hook is one for the whole
// program: what other threads give back meanwhile is looked through too, and holds nothing of a
// secret this test made for itself. Where the hook is in the middle of a block, it keeps a copy of
// what it looks for in a place of its own, which is never given back.)
class WhatIsGivenBack {
public:
    explicit WhatIsGivenBack(QByteArrayView secret)
    {
        Looking& now = looking();
        const qsizetype size = std::min<qsizetype>(secret.size(), static_cast<qsizetype>(now.bytes.size()));
        std::copy_n(secret.data(), size, now.bytes.begin());
        now.found.store(0);
        now.size.store(size);
        now.before = _CrtSetAllocHook(&WhatIsGivenBack::look);
    }
    ~WhatIsGivenBack()
    {
        Looking& now = looking();
        _CrtSetAllocHook(now.before);
        now.size.store(0);
    }
    WhatIsGivenBack(const WhatIsGivenBack&) = delete;
    WhatIsGivenBack& operator=(const WhatIsGivenBack&) = delete;
    WhatIsGivenBack(WhatIsGivenBack&&) = delete;
    WhatIsGivenBack& operator=(WhatIsGivenBack&&) = delete;

    // How many blocks that were given back (or moved, as one that grows is) held it.
    static int holdingIt() { return looking().found.load(); }

private:
    struct Looking {
        std::array<char, 256> bytes{};
        std::atomic<qsizetype> size{0};
        std::atomic<int> found{0};
        _CRT_ALLOC_HOOK before = nullptr;
    };
    static Looking& looking()
    {
        static Looking one;
        return one;
    }
    static int __cdecl look(int what, void* block, size_t size, int use, long request, const unsigned char* file,
                            int line)
    {
        Looking& now = looking();
        const qsizetype length = now.size.load();
        if (length > 0 && block != nullptr && (what == _HOOK_FREE || what == _HOOK_REALLOC) &&
            (_BLOCK_TYPE(use) == _NORMAL_BLOCK || _BLOCK_TYPE(use) == _CLIENT_BLOCK)) {
            const size_t held = _msize_dbg(block, _BLOCK_TYPE(use));
            const auto* first = static_cast<const char*>(block);
            const auto* last = first + (held == static_cast<size_t>(-1) ? 0 : held);
            if (std::search(first, last, now.bytes.data(), now.bytes.data() + length) != last) {
                now.found.fetch_add(1);
            }
        }
        // (And it is given back, or moved, as it was to be.)
        return now.before != nullptr ? now.before(what, block, size, use, request, file, line) : 1;
    }
};

// Secrets kept as Windows keeps them: a copy of its own of what it is handed, and none of that shared
// with whoever handed it; written over when it lets it go (forgotten, or kept anew in its place); and
// what it hands out is a copy of that, the caller's. (A store in memory that keeps what it is handed
// as it is shares the caller's own bytes, and what the caller writes over is then the store's.)
class SecretsOfTheirOwn final : public core::SecretStore {
public:
    SecretsOfTheirOwn() = default;
    SecretsOfTheirOwn(const SecretsOfTheirOwn&) = delete;
    SecretsOfTheirOwn& operator=(const SecretsOfTheirOwn&) = delete;
    SecretsOfTheirOwn(SecretsOfTheirOwn&&) = delete;
    SecretsOfTheirOwn& operator=(SecretsOfTheirOwn&&) = delete;
    ~SecretsOfTheirOwn() override
    {
        for (QByteArray& one : m_all) {
            core::wipeInPlace(one);
        }
    }

    base::Result<void> keep(const QString& name, const QByteArray& secret) override
    {
        if (refuses) {
            return base::Error{"secrets", "Windows did not keep it."};
        }
        letGo(name);
        m_all.insert(name, QByteArray(secret.constData(), secret.size()));
        return {};
    }
    QByteArray kept(const QString& name) override
    {
        const auto found = m_all.constFind(name);
        return found == m_all.constEnd() ? QByteArray() : QByteArray(found->constData(), found->size());
    }
    void forget(const QString& name) override { letGo(name); }

    bool refuses = false;

private:
    void letGo(const QString& name)
    {
        const auto found = m_all.find(name);
        if (found != m_all.end()) {
            core::wipeInPlace(*found);
            m_all.erase(found);
        }
    }

    QHash<QString, QByteArray> m_all;
};
#endif

} // namespace

// This PC's own wall as the program holds it, and the boxes that add a KVM to it and change one of
// its KVMs, against made-up KVMs: what a box asks for is done, and that box is told what came of it.
class TestOwnWall : public QObject {
    Q_OBJECT

private:
    static core::KvmAdding::Pace quick()
    {
        core::KvmAdding::Pace pace;
        // Room to be let in (5 s), for the probe and the driver: a TLS handshake under the address sanitizer
        // on a busy PC takes longer than 1.5 s, and a test of where a password goes is not to find the HTTPS way
        // gone for that. And the quiet as long: it is Qt's limit on a transfer, which runs from the asking, and
        // so is one on being let in too (the first line of a run under the sanitizer on a slow core was let in
        // after 1.6 s, and with a quiet of 1.5 s nothing ever answered: Qt's timeout before the line was up).
        pace.probe.connectWithinMs = 5000;
        pace.probe.quietMs = 5000;
        pace.probe.withinMs = 10'000;
        pace.driver.connectWithinMs = 5000;
        pace.driver.quietMs = 5000;
        pace.driver.transferMs = 1500;
        pace.driver.holdMs = 400;
        pace.driver.freshMs = 200;
        return pace;
    }

    static core::testing::Tls tls(const char* name)
    {
        const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
        return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
    }

    // A KVM that answers the sign-in only after this long.
    static void makeSlowToSignIn(MadeUpKvm& kvm, int afterMs)
    {
        Answered late = core::testing::good({{u"token"_s, u"pass1"_s}});
        late.afterMs = afterMs;
        kvm.first["/api/auth/login"_ba].append(late);
    }

    // And one that says what it is only after this long: its certificate is read by asking that.
    static void makeSlowToSayWhatItIs(MadeUpKvm& kvm, int afterMs)
    {
        Answered late = core::testing::refusal(401, u"UnauthorizedError"_s);
        late.afterMs = afterMs;
        kvm.instead.insert("/api/auth/check"_ba, late);
    }

    static QByteArray sha256Of(const char* name)
    {
        return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
    }

    // This PC's own wall for one test of the box that changes a KVM: its settings in a folder that
    // goes with the test, its secrets in memory.
    // Two addresses that are one JetKVM's two ways (core::KvmAdding::Pace::otherWay): a stand-in's are two
    // ports of this PC, where a real one's are ports 80 and 443 of one host.
    static std::function<std::optional<QUrl>(const QUrl&)> twoWays(const QUrl& one, const QUrl& other)
    {
        return [one, other](const QUrl& address) -> std::optional<QUrl> {
            if (address == one) {
                return other;
            }
            if (address == other) {
                return one;
            }
            return std::nullopt;
        };
    }

    struct Held {
        Held() : Held(quick()) {}
        explicit Held(const core::KvmAdding::Pace& pace)
            : settings(folder.filePath(u"settings.json"_s)), own(settings, secrets, pace)
        {}

        // Puts a made-up KVM on the wall as it stands there once it was added, with the password
        // the made-up KVMs have, known by the certificate of that name where one is named; and
        // gives the wall's name for it.
        QString put(const MadeUpKvm& kvm, const QString& name, const char* certificate = nullptr,
                    const QString& group = {})
        {
            core::OwnKvm known;
            known.address = kvm.stand->address();
            known.name = name;
            known.group = group;
            known.knownByCertificate = certificate != nullptr;
            known.certificate = certificate != nullptr ? sha256Of(certificate) : QByteArray();
            const base::Result<QString> id = own.wall().add(known, u"secret"_s);
            return id.ok() ? id.value() : QString();
        }
        core::OwnKvm kvm(const QString& id)
        {
            const QList<core::OwnKvm>& all = own.wall().kvms();
            const auto known = std::ranges::find_if(all, [&id](const core::OwnKvm& one) { return one.id == id; });
            return known == all.end() ? core::OwnKvm{} : *known;
        }
        // What the store holds for a KVM of the wall.
        QByteArray secretOf(const QString& id) const { return secrets.all.value(core::LocalWall::secretOf(id)); }

        QTemporaryDir folder;
        core::Settings settings;
        MemorySecrets secrets;
        ui::OwnWall own;
    };

    // Saves what stands in a box as it stands there, under this name and with this password.
    static void saveAs(ui::EditKvmView& box, const QString& name, const QString& password = {})
    {
        box.save(name, box.address(), box.user(), password, box.group());
    }

    // Somebody looks at a box, as its screens say it of one that is open in a window that is in
    // sight and is the active one: only then does the moment run after which it takes a yes.
    static void lookAt(ui::EditKvmView& box) { box.setLookedAt(true); }

    // Whether nothing a box shows, or keeps to show, has this in it.
    static bool showsNothingOf(const ui::EditKvmView& box, const QString& what)
    {
        const QMetaObject* shown = box.metaObject();
        for (int index = 0; index < shown->propertyCount(); ++index) {
            if (shown->property(index).read(&box).toString().contains(what)) {
                return false;
            }
        }
        return true;
    }

private slots:
    void initTestCase()
    {
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void atTheProgramsEndEveryControllersSessionIsGivenBack()
    {
        // M7 1b-i (the reading of 1a-v, L4), as main.cpp ends the program: a server's controller on the wall, looked
        // at, and an Add box's sign-in to another that is still out; each is handed back its session within the
        // goodbyes' time, and nothing of the wall signs in again.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        const auto goodbyes = std::make_shared<core::ControllerGoodbyes>();
        core::KvmAdding::Pace pace = quick();
        core::RedfishDevice::Pace server = pace.drivers().redfish;
        server.goodbyes = goodbyes;
        server.signOutWithinMs = 1000;
        server.hiccupMs = 0;
        pace.redfish = server;
        ui::OwnWall own(settings, secrets, pace);
        RedfishByHand bmc(RedfishByHand::Layout::Ami, tls("first"));
        core::OwnKvm controller;
        controller.address = bmc.address().toString();
        controller.kind = u"redfish"_s;
        controller.knownByCertificate = true;
        controller.certificate = sha256Of("first");
        QVERIFY(own.wall().add(controller, u"secret"_s).ok());
        QVERIFY(QTest::qWaitFor(
            [&] {
                own.wall().askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
                return bmc.sessions.size() == 1;
            },
            20000));
        RedfishByHand other(RedfishByHand::Layout::Ami, tls("second"));
        other.loginTakesMs = 800;
        ui::AddKvmView box;
        own.serve(box);
        box.add(other.address().toString(), QString(), u"secret"_s, QString());
        QVERIFY(QTest::qWaitFor([&other] { return other.logins == 1; }, 20000));
        own.letGoOfEverything();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const int logins = bmc.logins;
        goodbyes->finish(4000);
        QVERIFY(bmc.sessions.isEmpty());
        QVERIFY(other.sessions.isEmpty());
        QCOMPARE(bmc.logins, logins);
        QCOMPARE(other.logins, 1);
    }

    void aServerOnThisPcsWallHasAServersCardAndMenu()
    {
        // M7 1c (the plan: the card, the menu, "no new code expected", proven on this PC's wall): a controller's card
        // shows how its server is (the server's panel), "Powered off" where its controller says it is off; its menu is
        // a server's, as the web's server tiles have it (rule 17's checks are its driver's): power on or shut down by
        // what it is, hold power, reset, restart the controller, the light; and no keys or text where it has no console
        // that qKVM reads.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::KvmAdding::Pace pace = quick();
        core::RedfishDevice::Pace server = pace.drivers().redfish;
        server.goodbyes = std::make_shared<core::ControllerGoodbyes>();
        server.hiccupMs = 0;
        pace.redfish = server;
        ui::OwnWall own(settings, secrets, pace);
        RedfishByHand bmc(RedfishByHand::Layout::Ami, tls("first"));
        bmc.ownConsole = false;
        core::OwnKvm controller;
        controller.address = bmc.address().toString();
        controller.kind = u"redfish"_s;
        controller.name = u"db-01"_s;
        controller.knownByCertificate = true;
        controller.certificate = sha256Of("first");
        const base::Result<QString> id = own.wall().add(controller, u"secret"_s);
        QVERIFY(id.ok());
        const auto lookedAt = [&](const QString& state) {
            std::optional<core::Status> found;
            static_cast<void>(QTest::qWaitFor(
                [&] {
                    own.wall().askStatus([&](const base::Result<QHash<QString, core::Status>>& all) {
                        if (all && all.value().contains(id.value())) {
                            found = all.value().value(id.value());
                        }
                    });
                    QTest::qWait(50);
                    return found && found->state == state;
                },
                20000));
            return found.value_or(core::Status{});
        };
        std::optional<core::Machine> machine;
        own.wall().askWall([&](const base::Result<core::Wall>& wall) {
            if (wall && !wall.value().machines.isEmpty()) {
                machine = wall.value().machines.constFirst();
            }
        });
        QTRY_VERIFY(machine.has_value());
        const auto rowsOf = [](const ui::Menu& menu) {
            QStringList ids;
            for (const ui::MenuSection& section : menu.sections) {
                for (const ui::MenuRow& row : section.rows) {
                    ids.append(row.id);
                }
            }
            return ids;
        };
        const core::Status on = lookedAt(u"online"_s);
        QCOMPARE(on.state, u"online"_s);
        const ui::WallTile card = ui::cardFor(*machine, &on, 0, false);
        QVERIFY(!card.server.isEmpty());
        QCOMPARE(card.server[u"power"_s].toString(), u"on"_s);
        ui::MenuOf of;
        of.machine = *machine;
        of.status = on;
        const QStringList running = rowsOf(ui::menuFor(of));
        for (const QString& one :
             {u"power:power_off"_s, u"power:atx_power_long"_s, u"power:atx_reset"_s, u"reboot"_s, u"light:on"_s}) {
            QVERIFY2(running.contains(one), qPrintable(one + u" in "_s + running.join(u", "_s)));
        }
        QVERIFY(!running.contains(u"power:power_on"_s));
        QVERIFY(!running.contains(u"sendKeys"_s) && !running.contains(u"typeText"_s));
        bmc.power = u"Off"_s;
        const core::Status off = lookedAt(u"no_signal"_s);
        QCOMPARE(off.state, u"no_signal"_s);
        QCOMPARE(ui::cardFor(*machine, &off, 0, false).label, u"Powered off"_s);
        of.status = off;
        const QStringList stopped = rowsOf(ui::menuFor(of));
        QVERIFY(stopped.contains(u"power:power_on"_s));
        QVERIFY(!stopped.contains(u"power:power_off"_s));
    }

    void aJetKvmThatSaysItIsOneOfTheWallMayBeKeptBesideItFromEitherBox()
    {
        // From the reading of the twins (L3), as on the page: refused for what it says by its number, the box offers to
        // keep both, and keeping both adds it (or saves it) beside the other.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        ui::AddKvmView box;
        ui::OwnWall own(settings, secrets, quick());
        own.serve(box);
        JetKvmByHand here;
        JetKvmByHand there;
        there.deviceId = here.deviceId;
        box.add(here.address().toString(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.problem().isEmpty());
        QVERIFY(!box.mayKeepBoth());
        box.addKeepingBoth(there.address().toString(), QString(), u"secret"_s, QString());
        QCOMPARE(own.wall().kvms().size(), 1); // (not offered yet: nothing asked)
        box.add(there.address().toString(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.problem().startsWith(u"This says it is the same device as "_s));
        QVERIFY(box.mayKeepBoth());
        box.addKeepingBoth(there.address().toString(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY2(box.problem().isEmpty(), qPrintable(box.problem()));
        QVERIFY(!box.mayKeepBoth());
        QCOMPARE(own.wall().kvms().size(), 2);
        QVERIFY(own.wall().kvms().at(1).keptBesideItsTwin);
        // The change box: a third card, which says when signed in to anew that it is the first.
        JetKvmByHand third;
        third.deviceId = here.deviceId;
        core::OwnKvm jet;
        jet.address = third.address().toString();
        jet.kind = u"jetkvm"_s;
        const QString thirdId = own.wall().add(jet, u"secret"_s).value();
        ui::EditKvmView edit;
        own.serve(edit);
        edit.openFor(thirdId, false);
        edit.save(edit.name(), edit.address(), edit.user(), u"secret"_s, edit.group());
        QTRY_VERIFY_WITH_TIMEOUT(!edit.busy(), 20000);
        QVERIFY(edit.problem().startsWith(u"This says it is the same device as "_s));
        QVERIFY(edit.mayKeepBoth());
        edit.saveKeepingBoth(edit.name(), edit.address(), edit.user(), u"secret"_s, edit.group());
        QTRY_VERIFY_WITH_TIMEOUT(!edit.busy(), 20000);
        QVERIFY2(edit.problem().isEmpty(), qPrintable(edit.problem()));
        const auto kept =
            std::ranges::find_if(own.wall().kvms(), [&](const core::OwnKvm& one) { return one.id == thirdId; });
        QVERIFY(kept != own.wall().kvms().end());
        QVERIFY(kept->keptBesideItsTwin);
    }

    void whatTheBoxAsksForIsAddedAndTheBoxIsTold()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        ui::AddKvmView box;
        ui::OwnWall own(settings, secrets, quick());
        own.serve(box);
        const MadeUpKvm kvm;

        QVERIFY(!box.busy());
        const QSignalSpy asked(&box, &ui::AddKvmView::addAsked);
        box.add(kvm.stand->address(), QString(), u"secret"_s, u"Rack 2"_s);
        // It says at once that it is at it, and takes no second one meanwhile: that is not even
        // passed on.
        QVERIFY(box.busy());
        box.add(u"192.168.1.20"_s, QString(), u"secret"_s, QString());
        QCOMPARE(asked.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.problem().isEmpty());
        // The KVM's own name for itself is a maker's word for every one of its kind: it is called
        // by where it is. And what it says it is, and that it is spoken to in the clear.
        const QString host = u"127.0.0.1:%1"_s.arg(kvm.stand->serverPort());
        QCOMPARE(box.added(), host + u" added (RM10, over plain HTTP)"_s);
        // The plain way shows no certificate: it is known by none.
        QCOMPARE(box.certificate(), QString());
        QCOMPARE(box.count(), 1);
        QCOMPARE(own.wall().kvms().size(), 1);
        QCOMPARE(own.wall().kvms().constFirst().group, u"Rack 2"_s);
        QCOMPARE(secrets.all.size(), 1);

        // The box is closed: nothing of it is said any more.
        box.reset();
        QVERIFY(box.added().isEmpty());
        QCOMPARE(box.count(), 0);
    }

    void theCertificateAKvmIsKnownByIsShownToBeCompared()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        ui::AddKvmView box;
        ui::OwnWall own(settings, secrets, quick());
        own.serve(box);
        const MadeUpKvm kvm(tls("comet"));

        box.add(kvm.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(box.count(), 1);
        // As the page writes one: capitals, a colon between every two. And it is the one the wall
        // knows the KVM by.
        const QByteArray shown =
            StandIn::certificateIn(tls("comet").certificateFile).digest(QCryptographicHash::Sha256);
        QCOMPARE(box.certificate(), core::ServerClient::written(shown));
        QCOMPARE(box.certificate().size(), 95);
        QCOMPARE(own.wall().kvms().constFirst().certificate, shown);
        QVERIFY(!box.added().contains(u"plain HTTP"_s));

        // It goes with the box, and is not said of the next one, which shows none.
        const MadeUpKvm plain;
        box.add(plain.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.count(), 2);
        QCOMPARE(box.certificate(), QString());
        // Nor is it said any more once the box was closed.
        const MadeUpKvm another(tls("first"));
        box.add(another.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.count(), 3);
        QCOMPARE(box.certificate().size(), 95);
        QVERIFY(box.certificate() != core::ServerClient::written(shown));
        box.reset();
        QCOMPARE(box.certificate(), QString());
    }

    void whatIsNotAddedIsSaidInTheKvmsOrTheProgramsWords()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        ui::AddKvmView box;
        ui::OwnWall own(settings, secrets, quick());
        own.serve(box);
        const MadeUpKvm kvm;

        box.add(kvm.stand->address(), QString(), u"not it"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), u"Login rejected - check the username and password"_s);
        QVERIFY(box.added().isEmpty());
        QCOMPARE(box.count(), 0);
        QVERIFY(secrets.all.isEmpty());

        // The next try says nothing of the last one while it is at it.
        box.add(u"not an address at all"_s, QString(), u"secret"_s, QString());
        QVERIFY(box.problem().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.problem().startsWith(u"Not a usable device address"_s));
    }

    void eachWindowsBoxIsToldOfItsOwnKvmAndOfNoOther()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        ui::AddKvmView first;
        ui::AddKvmView second;
        ui::OwnWall own(settings, secrets, quick());
        own.serve(first);
        own.serve(second);
        MadeUpKvm slow;
        makeSlowToSignIn(slow, 500);
        const MadeUpKvm other;

        first.add(slow.stand->address(), QString(), u"secret"_s, QString());
        QVERIFY(first.busy());
        QVERIFY(!second.busy());
        // One KVM is added at a time, whichever window asks: the second box is told to wait, and
        // what it asked for is not done later behind its back.
        second.add(other.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY(!second.busy());
        QCOMPARE(second.problem(), u"Another KVM is being added at this moment. Wait for it to finish."_s);
        QVERIFY(first.busy());
        QVERIFY(first.problem().isEmpty());
        // Nor does a window that goes meanwhile, with its box, take the first one's KVM with it.
        {
            ui::AddKvmView goes;
            own.serve(goes);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!first.busy(), 20000);
        QCOMPARE(first.count(), 1);
        QVERIFY(first.problem().isEmpty());
        // What came of the first is said in the first, and nothing of it in the second.
        QVERIFY(second.added().isEmpty());
        QCOMPARE(second.count(), 0);
        QCOMPARE(second.problem(), u"Another KVM is being added at this moment. Wait for it to finish."_s);
        QCOMPARE(own.wall().kvms().size(), 1);
        QCOMPARE(other.signIns, 0);

        // And now the second has its turn.
        second.add(other.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 20000);
        QCOMPARE(second.count(), 1);
        QCOMPARE(first.count(), 1);
        QCOMPARE(own.wall().kvms().size(), 2);
    }

    void aBoxThatWasToldToWaitDoesNotTakeTheOtherOnesKvm()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        ui::AddKvmView first;
        ui::AddKvmView second;
        ui::OwnWall own(settings, secrets, quick());
        own.serve(first);
        own.serve(second);
        MadeUpKvm slow;
        makeSlowToSignIn(slow, 1500);
        const MadeUpKvm other;

        first.add(slow.stand->address(), QString(), u"secret"_s, QString());
        QTRY_COMPARE(slow.asked("/api/auth/login"), 1);
        second.add(other.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY(!second.busy());
        QVERIFY(!second.problem().isEmpty());
        // The KVM that is being asked is still the first box's to give up, and it does.
        QVERIFY(first.busy());
        first.giveUp();
        QTRY_VERIFY_WITH_TIMEOUT(!first.busy(), 400);
        QCOMPARE(first.count(), 0);
        QCOMPARE(first.problem(), QString());
        QTest::qWait(1800);
        QVERIFY(own.wall().kvms().isEmpty());
    }

    void aKvmThatIsBeingAskedIsGivenUpByItsBox()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        ui::AddKvmView box;
        ui::OwnWall own(settings, secrets, quick());
        own.serve(box);
        MadeUpKvm slow;
        makeSlowToSignIn(slow, 800);

        // Nothing is being asked: nothing is given up.
        const QSignalSpy asked(&box, &ui::AddKvmView::giveUpAsked);
        box.giveUp();
        QCOMPARE(asked.count(), 0);

        box.add(slow.stand->address(), QString(), u"secret"_s, QString());
        QTRY_COMPARE(slow.asked("/api/auth/login"), 1);
        box.giveUp();
        QCOMPARE(asked.count(), 1);
        // Over at once for whoever looks at the box: not busy, nothing to complain of, nothing added.
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 400);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(box.added(), QString());
        QCOMPARE(box.count(), 0);
        // And the KVM's late answer adds nothing.
        QTest::qWait(1200);
        QVERIFY(own.wall().kvms().isEmpty());
        QVERIFY(secrets.all.isEmpty());

        // The same box adds the next one as ever.
        const MadeUpKvm next;
        box.add(next.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.count(), 1);
        QCOMPARE(own.wall().kvms().size(), 1);
    }

    void aBoxThatGoesWhileItsKvmIsBeingAskedHasGivenItUp()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        ui::AddKvmView stays;
        ui::OwnWall own(settings, secrets, quick());
        own.serve(stays);
        MadeUpKvm slow;
        makeSlowToSignIn(slow, 600);
        {
            auto goes = std::make_unique<ui::AddKvmView>();
            own.serve(*goes);
            goes->add(slow.stand->address(), QString(), u"secret"_s, QString());
            QTRY_COMPARE(slow.asked("/api/auth/login"), 1);
        }
        // Nothing is added behind anybody's back, and the next box does not wait for it.
        const MadeUpKvm next;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                if (!stays.busy() && stays.count() == 0) {
                    stays.add(next.stand->address(), QString(), u"secret"_s, QString());
                }
                return stays.count() == 1;
            }(),
            20000);
        QTest::qWait(900);
        QCOMPARE(own.wall().kvms().size(), 1);
        QCOMPARE(own.wall().kvms().constFirst().address, next.stand->address());
        QCOMPARE(secrets.all.size(), 1);
    }

    // --- the box that changes a KVM of the wall ---------------------------------------------------

    void theBoxIsOpenedForAKvmOfTheWallWithTheKvmAsItIsThere()
    {
        Held held;
        const MadeUpKvm plain;
        const MadeUpKvm encrypted(tls("first"));
        const QString nas = held.put(plain, u"NAS"_s, nullptr, u"Rack 2"_s);
        const QString unnamed = held.put(encrypted, QString(), "first");
        ui::EditKvmView box;
        held.own.serve(box);
        const QSignalSpy opened(&box, &ui::EditKvmView::opened);

        // A machine that is not on the wall opens nothing.
        box.openFor(u"00000000"_s, false);
        box.openFor(QString(), true);
        QCOMPARE(opened.count(), 0);
        QCOMPARE(box.machine(), QString());

        // One that is spoken to in the clear: its address is typed back with "http://" before it,
        // and that is said. It is known by no certificate.
        box.openFor(nas, false);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(box.machine(), nas);
        QCOMPARE(box.called(), u"NAS"_s);
        QCOMPARE(box.name(), u"NAS"_s);
        QCOMPARE(box.address(), plain.stand->address());
        QVERIFY(box.address().startsWith(u"http://127.0.0.1:"_s));
        QCOMPARE(box.user(), u"admin"_s);
        QCOMPARE(box.group(), u"Rack 2"_s);
        QCOMPARE(box.certificate(), QString());
        QVERIFY(box.isOverPlainHttp());
        QVERIFY(!box.isHttps());
        QVERIFY(!box.isForReview());
        QVERIFY(!box.busy());
        QVERIFY(!box.isReviewed());
        // Closed: nothing of it is shown or kept.
        box.closed();
        QCOMPARE(box.machine(), QString());
        QCOMPARE(box.called(), QString());
        QCOMPARE(box.address(), QString());
        QVERIFY(!box.isOverPlainHttp());

        // One over HTTPS: "https://" is not written, and what it is known by is, as the page
        // writes one. It has no name, and is called by where it is.
        box.openFor(unnamed, false);
        QCOMPARE(opened.count(), 2);
        const QString host = u"127.0.0.1:%1"_s.arg(encrypted.stand->serverPort());
        QCOMPARE(box.address(), host);
        QCOMPARE(box.called(), host);
        QCOMPARE(box.name(), QString());
        QCOMPARE(box.certificate(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.certificate().size(), 95);
        QVERIFY(box.isHttps());
        QVERIFY(!box.isOverPlainHttp());
        // And saved as it stands there, it is the address it has: nothing is changed.
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        saveAs(box, box.name());
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(changed.isEmpty());
        QCOMPARE(held.kvm(unnamed).address, encrypted.stand->address());
        box.closed();

        // Opened for its certificate alone, a KVM that is spoken to in the clear gets the box as
        // it is: it has no certificate to look at, and is asked nothing.
        box.openFor(nas, true);
        QCOMPARE(opened.count(), 3);
        QVERIFY(!box.isForReview());
        QVERIFY(!box.busy());
        QTest::qWait(150);
        QVERIFY(plain.stand->asked.isEmpty());
        QVERIFY(encrypted.stand->asked.isEmpty());
    }

    void aKvmIsRenamedFromItsBoxWithNothingAskedOfIt()
    {
        Held held;
        const MadeUpKvm kvm;
        const QString id = held.put(kvm, u"NAS"_s, nullptr, u"Rack 2"_s);
        ui::EditKvmView box;
        held.own.serve(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy asked(&box, &ui::EditKvmView::saveAsked);

        box.save(u"Storage"_s, box.address(), box.user(), QString(), u"Rack 3"_s);
        // It says at once that it is at it, and what at; and takes nothing else meanwhile: that
        // is not even passed on.
        QVERIFY(box.busy());
        QCOMPARE(box.asked(), u"saving"_s);
        box.save(u"Another name"_s, box.address(), box.user(), QString(), QString());
        box.remove();
        box.review();
        QCOMPARE(asked.count(), 1);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), u"saved"_s);
        QCOMPARE(done.at(0).at(1).toString(), u"\"Storage\""_s);
        QVERIFY(!box.busy());
        QCOMPARE(box.problem(), QString());
        QCOMPARE(held.kvm(id).name, u"Storage"_s);
        QCOMPARE(held.kvm(id).group, u"Rack 3"_s);
        // The KVM was asked nothing for it, and its password is the one it was.
        QTest::qWait(150);
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(held.secretOf(id), core::testing::savedFor("secret", kvm.stand->address()));

        // With no name typed for it, it keeps the one it has.
        box.closed();
        box.openFor(id, false);
        QCOMPARE(box.name(), u"Storage"_s);
        box.save(QString(), box.address(), box.user(), QString(), box.group());
        QTRY_COMPARE(done.count(), 2);
        QCOMPARE(done.at(1).at(1).toString(), u"\"Storage\""_s);
        QCOMPARE(held.kvm(id).name, u"Storage"_s);
    }

    void aNewPasswordIsSignedInWithBeforeItIsSaved()
    {
        Held held;
        MadeUpKvm kvm;
        const QString id = held.put(kvm, u"NAS"_s);
        ui::EditKvmView box;
        held.own.serve(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        // (How often it had been signed in to when the wall said that it has changed.)
        int signedInBeforeItWasSaved = -1;
        connect(&held.own.wall(), &core::LocalWall::changed, &box,
                [&signedInBeforeItWasSaved, &kvm] { signedInBeforeItWasSaved = kvm.signIns; });
        // Its password was changed on the KVM itself, and is typed here.
        kvm.password = u"another one"_s;
        saveAs(box, box.name(), u"another one"_s);
        QVERIFY(box.busy());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
        QCOMPARE(done.at(0).at(0).toString(), u"saved"_s);
        QCOMPARE(done.at(0).at(1).toString(), u"\"NAS\""_s);
        QCOMPARE(signedInBeforeItWasSaved, 1);
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(kvm.last("/api/auth/login")->body, "user=admin&passwd=another%20one"_ba);
        // Kept in the place of the one before, with the secrets; and nowhere in the box.
        QCOMPARE(held.secrets.all.size(), 1);
        QCOMPARE(held.secretOf(id), core::testing::savedFor("another one", kvm.stand->address()));
        QVERIFY(showsNothingOf(box, u"another one"_s));
    }

    void aPasswordThatIsRefusedChangesNothingAndIsSaidInTheKvmsWords()
    {
        Held held;
        const MadeUpKvm kvm;
        const QString id = held.put(kvm, u"NAS"_s, nullptr, u"Rack 2"_s);
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView box;
        held.own.serve(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);

        box.save(u"Another name"_s, box.address(), box.user(), u"not it"_s, u"Rack 9"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), u"Login rejected - check the username and password"_s);
        QCOMPARE(done.count(), 0);
        // Nothing of it: not the name either. The box shows the KVM as it is, and the password
        // that was typed is nowhere in it.
        QCOMPARE(held.kvm(id), before);
        QVERIFY(changed.isEmpty());
        QCOMPARE(held.secretOf(id), core::testing::savedFor("secret", kvm.stand->address()));
        QCOMPARE(box.name(), u"NAS"_s);
        QVERIFY(showsNothingOf(box, u"not it"_s));

        // The next try says nothing of the last one while it is at it; and with the right one
        // all of it is saved.
        box.save(u"Another name"_s, box.address(), box.user(), u"secret"_s, u"Rack 9"_s);
        QVERIFY(box.problem().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
        QCOMPARE(held.kvm(id).name, u"Another name"_s);
        QCOMPARE(held.kvm(id).group, u"Rack 9"_s);
    }

    void whatIsRefusedIsSaidInTheSentenceItComesWith()
    {
        Held held;
        const MadeUpKvm plain;
        const MadeUpKvm elsewhere;
        const MadeUpKvm encrypted(tls("first"));
        const QString nas = held.put(plain, u"NAS"_s);
        const QString build = held.put(encrypted, u"Build server"_s, "first");
        const core::OwnKvm nasBefore = held.kvm(nas);
        const core::OwnKvm buildBefore = held.kvm(build);
        ui::EditKvmView box;
        held.own.serve(box);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);

        // A KVM that is known by nothing is known nowhere else: at a new address its password
        // has to be typed. Nothing is asked there for it.
        box.openFor(nas, false);
        box.save(box.name(), elsewhere.stand->address(), box.user(), QString(), box.group());
        QTRY_VERIFY(!box.busy());
        QCOMPARE(box.problem(),
                 u"There is nothing to know this device by at another address. Type its password to use it there."_s);
        QTest::qWait(150);
        QVERIFY(elsewhere.stand->asked.isEmpty());
        // Nor is it put where another KVM of the wall is, or at an address that is none.
        box.save(box.name(), encrypted.stand->address(), box.user(), u"secret"_s, box.group());
        QTRY_VERIFY(!box.busy());
        QCOMPARE(box.problem(), u"This device is already on the wall"_s);
        box.save(box.name(), u"not an address at all"_s, box.user(), QString(), box.group());
        QTRY_VERIFY(!box.busy());
        QVERIFY(box.problem().startsWith(u"Not a usable device address"_s));
        box.closed();

        // One that is on the wall over HTTPS is not changed to plain HTTP at the place it is at,
        // with its password typed for it or without.
        box.openFor(build, false);
        box.save(box.name(), u"http://"_s + box.address(), box.user(), u"secret"_s, box.group());
        QTRY_VERIFY(!box.busy());
        QCOMPARE(box.problem(),
                 u"This device is on the wall over HTTPS. It is not changed to plain HTTP at the same address, where "
                 "its password would travel unencrypted. To use it that way, remove it from the wall and add it "
                 "again."_s);
        QCOMPARE(done.count(), 0);
        QVERIFY(changed.isEmpty());
        QCOMPARE(held.kvm(nas), nasBefore);
        QCOMPARE(held.kvm(build), buildBefore);
        QTest::qWait(150);
        QVERIFY(encrypted.stand->asked.isEmpty());
        box.closed();

        // With its password typed, the first is signed in to at the new address, and is there.
        box.openFor(nas, false);
        box.save(box.name(), elsewhere.stand->address(), box.user(), u"secret"_s, box.group());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
        QCOMPARE(held.kvm(nas).address, elsewhere.stand->address());
        QCOMPARE(elsewhere.signIns, 1);
        QCOMPARE(plain.signIns, 0);
    }

    void aKvmIsTakenOffTheWallFromItsBoxAndItsPasswordIsForgotten()
    {
        Held held;
        const MadeUpKvm kvm;
        const MadeUpKvm other;
        const QString id = held.put(kvm, u"NAS"_s);
        const QString stays = held.put(other, u"Build server"_s);
        ui::EditKvmView box;
        ui::EditKvmView second;
        held.own.serve(box);
        held.own.serve(second);
        // (Two windows have their box open for it.)
        box.openFor(id, false);
        second.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy doneInTheOther(&second, &ui::EditKvmView::done);
        QCOMPARE(held.secrets.all.size(), 2);

        box.remove();
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), u"removed"_s);
        QCOMPARE(done.at(0).at(1).toString(), u"NAS"_s);
        QVERIFY(!box.busy());
        QCOMPARE(box.asked(), u"removing"_s);
        // The KVM and its password are gone, and the other one is as it was.
        QCOMPARE(held.own.wall().kvms().size(), 1);
        QCOMPARE(held.own.wall().kvms().constFirst().id, stays);
        QCOMPARE(held.secrets.all.size(), 1);
        QVERIFY(held.secretOf(id).isEmpty());
        QCOMPARE(core::Settings(held.folder.filePath(u"settings.json"_s)).remembered().kvms.size(), 1);
        // The KVM itself is not touched.
        QTest::qWait(150);
        QVERIFY(kvm.stand->asked.isEmpty());

        // The other window's box is told nothing of its being done, but that its KVM went; and it
        // asks nothing more of that KVM (aBoxWhoseKvmWentAsksNothingMore).
        QCOMPARE(doneInTheOther.count(), 0);
        QVERIFY(second.isGone());
        second.remove();
        QVERIFY(!second.busy());
        QVERIFY(second.problem().isEmpty());
        saveAs(second, u"Storage"_s);
        QVERIFY(!second.busy());
        QVERIFY(second.problem().isEmpty());
        QCOMPARE(doneInTheOther.count(), 0);
        QCOMPARE(held.own.wall().kvms().size(), 1);
    }

    void aBoxIsOpenedForTheQuestionBeforeAKvmIsTakenOffAndForThatAlone()
    {
        Held held;
        const MadeUpKvm kvm;
        const MadeUpKvm other;
        const QString id = held.put(kvm, u"NAS"_s);
        const QString stays = held.put(other, u"Build server"_s);
        ui::EditKvmView box;
        held.own.serve(box);
        const QSignalSpy opened(&box, &ui::EditKvmView::opened);
        const QSignalSpy done(&box, &ui::EditKvmView::done);

        // A list's "Remove": the box, for that KVM, at its question alone.
        box.openToRemove(id);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(box.machine(), id);
        QVERIFY(box.removesOnly());
        QVERIFY(!box.isForReview());
        // Nothing is taken off by opening it.
        QCOMPARE(held.own.wall().kvms().size(), 2);
        // Closed: it is that no more.
        box.closed();
        QVERIFY(!box.removesOnly());

        // Opened the other ways, it is the box with its lines, as ever.
        box.openFor(id, false);
        QCOMPARE(opened.count(), 2);
        QVERIFY(!box.removesOnly());
        box.closed();
        // A machine that is not on the wall opens nothing; and the box that opens next for another
        // way has nothing of that wish.
        box.openToRemove(u"0000beef"_s);
        QCOMPARE(opened.count(), 2);
        box.openFor(stays, false);
        QCOMPARE(opened.count(), 3);
        QCOMPARE(box.machine(), stays);
        QVERIFY(!box.removesOnly());
        box.closed();

        // Said yes to, it is taken off as from the box's own question.
        box.openToRemove(id);
        QVERIFY(box.removesOnly());
        box.remove();
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), u"removed"_s);
        QCOMPARE(held.own.wall().kvms().size(), 1);
        QCOMPARE(held.own.wall().kvms().constFirst().id, stays);
        QVERIFY(held.secretOf(id).isEmpty());
        box.closed();
        QVERIFY(!box.removesOnly());
    }

    void theCertificateAKvmIsKnownByIsLookedAtBesideTheOneItShows()
    {
        Held held;
        const MadeUpKvm kvm(tls("first"));
        const MadeUpKvm plain;
        const QString id = held.put(kvm, u"Build server"_s, "first");
        const QString nas = held.put(plain, u"NAS"_s);
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView box;
        held.own.serve(box);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        const QSignalSpy trustAsked(&box, &ui::EditKvmView::trustAsked);
        box.openFor(id, false);

        // Before anything was read there is nothing to say yes to, and nothing to go back from.
        box.trust();
        box.closeReview();
        QCOMPARE(trustAsked.count(), 0);
        QVERIFY(!box.busy());

        box.review();
        QVERIFY(box.busy());
        QCOMPARE(box.asked(), u"looking"_s);
        QVERIFY(!box.isReviewed());
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QVERIFY(!box.busy());
        QCOMPARE(box.problem(), QString());
        // The same KVM as before: the two are the same, each as the page writes one.
        QCOMPARE(box.remembered(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.presented(), box.remembered());
        QCOMPARE(box.presented().size(), 95);
        QVERIFY(box.isSame());
        // It was asked what it is, with nothing that says who asks: no password went anywhere.
        QTest::qWait(150);
        QCOMPARE(kvm.signIns, 0);
        QVERIFY(!kvm.stand->asked.isEmpty());
        QVERIFY(std::ranges::all_of(kvm.stand->asked, [](const core::testing::Asked& one) {
            return one.method == "GET" && one.body.isEmpty() && !one.headers.contains("cookie");
        }));
        // Looking changes nothing; and once they were looked at, they are shown no longer.
        QCOMPARE(held.kvm(id), before);
        QVERIFY(changed.isEmpty());
        box.closeReview();
        QVERIFY(!box.isReviewed());
        QCOMPARE(box.remembered(), QString());
        QCOMPARE(box.presented(), QString());
        QVERIFY(!box.isSame());
        QCOMPARE(box.machine(), id);
        box.closed();

        // A KVM that is spoken to in the clear has none, and is asked nothing.
        box.openFor(nas, false);
        box.review();
        QTRY_VERIFY(!box.busy());
        QCOMPARE(box.problem(), u"This device is reached over plain HTTP, where there is no certificate"_s);
        QVERIFY(!box.isReviewed());
        QVERIFY(plain.stand->asked.isEmpty());
    }

    void aKvmThatShowsAnotherCertificateIsKnownByItOnceTheBoxSaidYes()
    {
        Held held;
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        const QString id = held.put(*kvm, u"Build server"_s, "first");
        // After a factory reset it has another certificate, at the address it had.
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        const MadeUpKvm reset(tls("second"), port);
        QVERIFY(reset.stand->isListening());
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        const QSignalSpy opened(&box, &ui::EditKvmView::opened);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);

        // As its card's "Review certificate" opens the box: for the certificate alone, which is
        // read at once.
        held.own.openFor(box, id, true);
        QCOMPARE(opened.count(), 1);
        QVERIFY(box.isForReview());
        QVERIFY(box.busy());
        QCOMPARE(box.asked(), u"looking"_s);
        QCOMPARE(box.certificate(), core::kvm::fingerprint(sha256Of("first")));
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QCOMPARE(box.remembered(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("second")));
        QVERIFY(!box.isSame());
        // Nothing is trusted by looking.
        QCOMPARE(held.kvm(id).certificate, sha256Of("first"));
        QVERIFY(changed.isEmpty());

        // The yes: for exactly the certificate that was shown.
        const QSignalSpy trustAsked(&box, &ui::EditKvmView::trustAsked);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.trust();
        QCOMPARE(trustAsked.count(), 1);
        QCOMPARE(trustAsked.at(0).at(0).toString(), id);
        QCOMPARE(trustAsked.at(0).at(1).toByteArray(), sha256Of("second"));
        QVERIFY(box.busy());
        QCOMPARE(box.asked(), u"trusting"_s);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
        QCOMPARE(done.at(0).at(0).toString(), u"trusted"_s);
        QCOMPARE(done.at(0).at(1).toString(), u"\"Build server\""_s);
        QVERIFY(!box.busy());
        QCOMPARE(changed.count(), 1);
        QVERIFY(held.kvm(id).knownByCertificate);
        QCOMPARE(held.kvm(id).certificate, sha256Of("second"));
        // Until then nothing of a password went to it; and the kept one is saved for that
        // certificate from now on.
        QCOMPARE(reset.signIns, 0);
        QCOMPARE(held.secretOf(id), core::testing::savedFor("secret", reset.stand->address(), sha256Of("second")));
        // Opened anew, the box says what it is known by now.
        box.closed();
        box.openFor(id, false);
        QCOMPARE(box.certificate(), core::kvm::fingerprint(sha256Of("second")));
    }

    void whatIsTrustedIsTheCertificateTheBoxShowedAndNeverOneReadAgainInBetween()
    {
        Held held;
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        const QString id = held.put(*kvm, u"Build server"_s, "first");
        const core::OwnKvm before = held.kvm(id);
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        auto second = std::make_unique<MadeUpKvm>(tls("second"), port);
        QVERIFY(second->stand->isListening());
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy trustAsked(&box, &ui::EditKvmView::trustAsked);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        box.openFor(id, false);
        box.review();
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("second")));
        QVERIFY(!box.yesWasToAnother());

        // While somebody reads the two, something else again takes its place at the address.
        second.reset();
        const MadeUpKvm third(tls("comet"), port);
        QVERIFY(third.stand->isListening());
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.trust();
        QCOMPARE(trustAsked.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        // Nobody has seen what it shows now: nothing is trusted. (This had the box keep the two
        // it had read, with a line under them that the device "now presents a different
        // certificate", and its yes took the next click at once, to the one that was shown no
        // longer: turned round. What it shows now is what the box shows, and it says that this
        // is another than the one the yes was given to.)
        QCOMPARE(done.count(), 0);
        QCOMPARE(held.kvm(id), before);
        QVERIFY(changed.isEmpty());
        QVERIFY(box.isReviewed());
        QCOMPARE(box.remembered(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("comet")));
        QVERIFY(box.yesWasToAnother());
        QCOMPARE(box.problem(), QString());
        // And no yes is passed on for longer than the usual moment.
        QVERIFY(!box.mayTrust());
        box.trust();
        QTest::qWait(ui::EditKvmView::trustRestsMs + 300);
        QVERIFY(!box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 1);
        QCOMPARE(third.signIns, 0);

        // Once it was looked at for that long, the third can be said yes to.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 8000);
        box.trust();
        QCOMPARE(trustAsked.count(), 2);
        QCOMPARE(trustAsked.at(1).at(1).toByteArray(), sha256Of("comet"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
        QCOMPARE(held.kvm(id).certificate, sha256Of("comet"));
    }

    void aSecondBoxIsToldToWaitAndEachBoxIsToldOfItsOwnAndOfNoOther()
    {
        Held held;
        MadeUpKvm slow;
        makeSlowToSignIn(slow, 500);
        const MadeUpKvm other;
        const QString first = held.put(slow, u"NAS"_s);
        const QString second = held.put(other, u"Build server"_s);
        ui::EditKvmView one;
        ui::EditKvmView two;
        held.own.serve(one);
        held.own.serve(two);
        one.openFor(first, false);
        two.openFor(second, false);
        const QSignalSpy doneInOne(&one, &ui::EditKvmView::done);
        const QSignalSpy doneInTwo(&two, &ui::EditKvmView::done);
        // What one window's box is opened for is nothing to the other's.
        QCOMPARE(one.machine(), first);
        QCOMPARE(two.machine(), second);

        saveAs(one, u"Storage"_s, u"secret"_s);
        QVERIFY(one.busy());
        QVERIFY(!two.busy());
        // One KVM is changed at a time, whichever window asks: the second box is told to wait,
        // and what it asked for is not done later behind its back.
        saveAs(two, u"Database"_s);
        QTRY_VERIFY(!two.busy());
        QCOMPARE(two.problem(), u"Another KVM is being changed at this moment. Wait for it to finish."_s);
        QVERIFY(one.busy());
        QVERIFY(one.problem().isEmpty());
        // Nor does a window that goes meanwhile, with its box, take the first one's change with it.
        {
            ui::EditKvmView goes;
            held.own.serve(goes);
            goes.openFor(second, false);
        }
        QTRY_COMPARE_WITH_TIMEOUT(doneInOne.count(), 1, 20000);
        QCOMPARE(doneInOne.at(0).at(1).toString(), u"\"Storage\""_s);
        // What came of the first is said in the first, and nothing of it in the second.
        QCOMPARE(doneInTwo.count(), 0);
        QCOMPARE(two.problem(), u"Another KVM is being changed at this moment. Wait for it to finish."_s);
        QCOMPARE(two.name(), u"Build server"_s);
        QCOMPARE(held.kvm(second).name, u"Build server"_s);
        QCOMPARE(other.signIns, 0);

        // And now the second has its turn.
        saveAs(two, u"Database"_s);
        QVERIFY(two.problem().isEmpty());
        QTRY_COMPARE(doneInTwo.count(), 1);
        QCOMPARE(doneInOne.count(), 1);
        QCOMPARE(held.kvm(second).name, u"Database"_s);
        QCOMPARE(held.kvm(first).name, u"Storage"_s);
    }

    void oneCertificateIsLookedAtATimeWhicheverBoxAsks()
    {
        Held held;
        MadeUpKvm slow(tls("first"));
        makeSlowToSayWhatItIs(slow, 500);
        const MadeUpKvm other(tls("second"));
        const QString first = held.put(slow, u"NAS"_s, "first");
        const QString second = held.put(other, u"Build server"_s, "second");
        ui::EditKvmView one;
        ui::EditKvmView two;
        held.own.serve(one);
        held.own.serve(two);
        one.openFor(first, false);
        two.openFor(second, false);

        one.review();
        two.review();
        QTRY_VERIFY(!two.busy());
        QCOMPARE(two.problem(), u"A certificate is being looked at at this moment. Wait for it to finish."_s);
        QVERIFY(!two.isReviewed());
        QVERIFY(one.busy());
        // A change is not held up by it: what changes a KVM is not what reads a certificate.
        const QSignalSpy doneInTwo(&two, &ui::EditKvmView::done);
        saveAs(two, u"Database"_s);
        QTRY_COMPARE(doneInTwo.count(), 1);
        QVERIFY(one.busy());
        QTRY_VERIFY_WITH_TIMEOUT(one.isReviewed(), 20000);
        QVERIFY(one.isSame());
        // The first one's two certificates are shown in the first, and nothing of them in the second.
        QVERIFY(!two.isReviewed());
        QCOMPARE(two.presented(), QString());
        QTest::qWait(150);
        QVERIFY(other.stand->asked.isEmpty());
        // And now the second has its turn. (Its box closed when it was saved, as the screens
        // close it, and is opened again: a look is asked over the KVM as the box was filled
        // with it, and that was before it was renamed.)
        two.closed();
        two.openFor(second, false);
        two.review();
        QTRY_VERIFY_WITH_TIMEOUT(two.isReviewed(), 20000);
        QCOMPARE(two.presented(), core::kvm::fingerprint(sha256Of("second")));
        QCOMPARE(one.presented(), core::kvm::fingerprint(sha256Of("first")));
    }

    void aBoxThatGoesWhileItsChangeIsUnderWayHasGivenItUp()
    {
        Held held;
        MadeUpKvm slow;
        makeSlowToSignIn(slow, 600);
        const QString id = held.put(slow, u"NAS"_s);
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView stays;
        held.own.serve(stays);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        {
            auto goes = std::make_unique<ui::EditKvmView>();
            held.own.serve(*goes);
            goes->openFor(id, false);
            saveAs(*goes, u"Another name"_s, u"another one"_s);
            QTRY_COMPARE(slow.asked("/api/auth/login"), 1);
        }
        // The next box does not wait for it: its own change is under way at once.
        stays.openFor(id, false);
        const QSignalSpy done(&stays, &ui::EditKvmView::done);
        saveAs(stays, u"Storage"_s);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(stays.problem(), QString());
        QCOMPARE(changed.count(), 1);
        // And nothing is changed behind anybody's back when the KVM answers at last: neither the
        // name nor the password that went with the box.
        QTest::qWait(1000);
        QCOMPARE(changed.count(), 1);
        core::OwnKvm now = before;
        now.name = u"Storage"_s;
        QCOMPARE(held.kvm(id), now);
        QCOMPARE(held.secretOf(id), core::testing::savedFor("secret", slow.stand->address()));
    }

    void aBoxThatGoesWhileItsCertificateIsBeingTrustedHasGivenItUp()
    {
        Held held;
        MadeUpKvm kvm(tls("second"));
        const QString id = held.put(kvm, u"Build server"_s, "first");
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView stays;
        held.own.serve(stays);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        {
            auto goes = std::make_unique<ui::EditKvmView>();
            held.own.serve(*goes);
            lookAt(*goes);
            held.own.openFor(*goes, id, true);
            QTRY_VERIFY_WITH_TIMEOUT(goes->isReviewed(), 20000);
            QVERIFY(!goes->isSame());
            // The yes is on its way when the window is closed over it.
            makeSlowToSayWhatItIs(kvm, 600);
            const qsizetype askedBefore = kvm.stand->asked.size();
            QTRY_VERIFY_WITH_TIMEOUT(goes->mayTrust(), 5000);
            goes->trust();
            QTRY_VERIFY(kvm.stand->asked.size() > askedBefore);
        }
        // The next box does not wait for it.
        stays.openFor(id, false);
        stays.review();
        QVERIFY(stays.busy());
        QTRY_VERIFY_WITH_TIMEOUT(stays.isReviewed(), 20000);
        QVERIFY(!stays.isSame());
        // And nothing was trusted behind anybody's back.
        QTest::qWait(400);
        QCOMPARE(held.kvm(id), before);
        QVERIFY(changed.isEmpty());
    }

    void aBoxWhoseSignInWaitsForAnEarlierOneSaysSoAndIsNeverToldItIsAskedAgain()
    {
        // (The pacing's third reading, L6.) Save says it is checking for as long as its sign-in waits for an earlier
        // one to that KVM (another window's, the wall's own): the box is told so, and says it. Where that came to
        // nothing, the box is told the KVM does not sign qKVM in, and not that it is asked again shortly: a box never
        // asks again.
        Held held;
        MadeUpKvm kvm;
        const QString id = held.put(kvm, u"NAS"_s);
        ui::EditKvmView box;
        held.own.serve(box);
        const QSignalSpy said(&box, &ui::EditKvmView::signInWaitsForAnEarlierOne);
        box.openFor(id, false);
        const core::SignInPacing::Mark mark =
            core::SignInPacing::what(u"login", QUrl(kvm.stand->address()), u"admin"_s, u"secret"_s);
        const core::SignInPacing::Steps steps{60'000, 10'000, 20'000};
        const core::SignInPacing::Sent earlier = core::SignInPacing::program().sent(mark, steps);
        saveAs(box, u"Another name"_s, u"secret"_s);
        QTRY_VERIFY(box.waitsForAnotherSignIn());
        QVERIFY(box.busy());
        QCOMPARE(said.count(), 1);
        QCOMPARE(kvm.asked("/api/auth/login"), 0);
        core::SignInPacing::program().unanswered(mark, steps, earlier);
        QTRY_VERIFY(!box.busy());
        QVERIFY(!box.waitsForAnotherSignIn());
        QVERIFY2(box.problem().contains(u"does not sign qKVM in"_s), qPrintable(box.problem()));
        QVERIFY2(!box.problem().contains(u"asked again"_s), qPrintable(box.problem()));
        QCOMPARE(kvm.asked("/api/auth/login"), 0);
        QCOMPARE(said.count(), 1);
        core::SignInPacing::program().accepted(mark);
    }

    void whatABoxAskedIsGivenUpByItAndByItsClosing()
    {
        Held held;
        MadeUpKvm slow;
        const QString id = held.put(slow, u"NAS"_s);
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView box;
        ui::EditKvmView other;
        held.own.serve(box);
        held.own.serve(other);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy giveUpAsked(&box, &ui::EditKvmView::giveUpAsked);
        box.openFor(id, false);
        other.openFor(id, false);

        // Nothing is being asked: nothing is given up.
        box.giveUp();
        QCOMPARE(giveUpAsked.count(), 0);

        // With the button that says so: over at once for whoever looks at the box, with nothing
        // to complain of, and the box stays as it was opened.
        makeSlowToSignIn(slow, 600);
        saveAs(box, u"Another name"_s, u"secret"_s);
        QTRY_COMPARE(slow.asked("/api/auth/login"), 1);
        box.giveUp();
        QCOMPARE(giveUpAsked.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 400);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(box.machine(), id);
        QCOMPARE(box.name(), u"NAS"_s);

        // By its closing, as when the wall is left under it. A box that was told to wait for
        // that change a moment before is told that nothing came of what it asked: the one that
        // would have told it to wait is gone. (The sign-in given up above goes on, on a line of its
        // own, and the same password typed again waits for its answer alone: one at a time to a KVM.)
        makeSlowToSignIn(slow, 600);
        saveAs(box, u"Another name"_s, u"secret"_s);
        QTRY_COMPARE(slow.asked("/api/auth/login"), 2);
        saveAs(other, u"Storage"_s);
        QVERIFY(other.busy());
        box.closed();
        QCOMPARE(giveUpAsked.count(), 2);
        QVERIFY(!box.busy());
        QCOMPARE(box.machine(), QString());
        QVERIFY(!other.busy());
        QCOMPARE(other.problem(), QString());

        // Nothing came of either, also once the KVM has answered; and nobody was told anything.
        QTest::qWait(1000);
        QCOMPARE(done.count(), 0);
        QVERIFY(changed.isEmpty());
        QCOMPARE(held.kvm(id), before);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(other.problem(), QString());

        // A box that was told to wait, and is closed before it hears so, hears nothing afterwards.
        makeSlowToSignIn(slow, 400);
        const QSignalSpy doneInTheOther(&other, &ui::EditKvmView::done);
        saveAs(other, u"Storage"_s, u"secret"_s);
        box.openFor(id, false);
        saveAs(box, u"Another name"_s);
        QVERIFY(box.busy());
        box.closed();
        QVERIFY(!box.busy());
        QTRY_COMPARE_WITH_TIMEOUT(doneInTheOther.count(), 1, 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(held.kvm(id).name, u"Storage"_s);

        // The same box changes the next one as ever.
        box.openFor(id, false);
        saveAs(box, u"Spare"_s);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(held.kvm(id).name, u"Spare"_s);
    }

    void aPasswordTypedForANewAddressGoesToNoOtherCertificateBeforeTheBoxShowedTheTwo()
    {
        Held held;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm other(tls("second"));
        const QString id = held.put(here, u"Build server"_s, "first");
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        const QString there = u"127.0.0.1:%1"_s.arg(other.stand->serverPort());

        // Another address, and its password typed with it: what answers there shows another
        // certificate than the KVM is known by.
        const QSignalSpy saveAsked(&box, &ui::EditKvmView::saveAsked);
        const QSignalSpy trustAsked(&box, &ui::EditKvmView::trustAsked);
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        // (First of all, while the two have only just come: a yes that comes at once is not
        // passed on; nor is this the yes of a review.)
        QVERIFY(!box.mayTrust());
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        box.trust();
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(trustAsked.count(), 0);
        QVERIFY(!box.busy());
        QCOMPARE(done.count(), 0);
        QCOMPARE(other.signIns, 0);
        QCOMPARE(held.kvm(id), before);
        QVERIFY(changed.isEmpty());
        // No yes went with that saving. The box has the two to show, each whole as the page
        // writes one, and the address the second was shown at; nothing is complained of.
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(saveAsked.at(0).at(6).toByteArray(), QByteArray());
        QVERIFY(box.differs());
        QVERIFY(!box.isReviewed());
        QCOMPARE(box.remembered(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("second")));
        QCOMPARE(box.presented().size(), 95);
        QCOMPARE(box.newAddress(), there);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(box.machine(), id);
        // Nothing but what a look at a certificate asks has gone there.
        QTest::qWait(150);
        QCOMPARE(other.stand->asked.size(), 3);
        QVERIFY(std::ranges::all_of(other.stand->asked, [](const core::testing::Asked& one) {
            return one.method == "GET" && one.body.isEmpty() && !one.headers.contains("cookie");
        }));

        // The way back to the lines: the two are shown no longer, and nothing was trusted.
        box.closeReview();
        QVERIFY(!box.differs());
        QCOMPARE(box.presented(), QString());
        QCOMPARE(box.newAddress(), QString());
        QCOMPARE(box.machine(), id);
        // Saved again as it is, it is refused again: no yes is remembered from before.
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.differs());
        QCOMPARE(other.signIns, 0);

        // The yes, once the two have been there for a moment: it hands on the very certificate
        // the box shows, with what was typed.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        QCOMPARE(saveAsked.count(), 3);
        QCOMPARE(saveAsked.at(2).at(2).toString(), there);
        QCOMPARE(saveAsked.at(2).at(4).toString(), u"secret"_s);
        QCOMPARE(saveAsked.at(2).at(6).toByteArray(), sha256Of("second"));
        QVERIFY(box.busy());
        QCOMPARE(box.asked(), u"saving"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        // Signed in to there, once, and known by that certificate; and the box says what it is
        // known by now (it does not just close).
        QCOMPARE(other.signIns, 1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(held.kvm(id).address, other.stand->address());
        QCOMPARE(held.kvm(id).certificate, sha256Of("second"));
        QCOMPARE(held.secretOf(id), core::testing::savedFor("secret", other.stand->address(), sha256Of("second")));
        QVERIFY(!box.differs());
        QCOMPARE(done.count(), 0);
        QCOMPARE(box.savedAs(), u"\"Build server\""_s);
        QCOMPARE(box.certificate(), core::kvm::fingerprint(sha256Of("second")));
        QCOMPARE(box.address(), there);
        QVERIFY(showsNothingOf(box, u"secret"_s));
    }

    void aYesInTheBoxIsToTheCertificateItShowedAndWhatIsShownInItsPlaceByThenIsShownAnew()
    {
        Held held;
        const MadeUpKvm here(tls("first"));
        auto second = std::make_unique<MadeUpKvm>(tls("second"));
        const quint16 port = second->stand->serverPort();
        const QString there = u"127.0.0.1:%1"_s.arg(port);
        const QString id = held.put(here, u"Build server"_s, "first");
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy saveAsked(&box, &ui::EditKvmView::saveAsked);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(box.differs(), 20000);
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("second")));
        QVERIFY(!box.yesWasToAnother());
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);

        // While somebody reads the two, something else again takes its place at that address.
        second.reset();
        const MadeUpKvm third(tls("comet"), port);
        QVERIFY(third.stand->isListening());
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        QCOMPARE(saveAsked.count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        // Nobody has seen what it shows now: nothing is saved, the box shows that one beside the
        // one the KVM is known by, and says that it is another than the one the yes was given
        // to. (This had the second swapped for the third with nothing said, and the yes took
        // the next click after its usual moment: the box read as one whose click had not
        // taken. It rests for longer now, and says why.)
        QVERIFY(box.differs());
        QCOMPARE(box.remembered(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("comet")));
        QVERIFY(box.yesWasToAnother());
        QVERIFY(!box.mayTrust());
        QCOMPARE(third.signIns, 0);
        QCOMPARE(held.kvm(id), before);
        QVERIFY(changed.isEmpty());
        QCOMPARE(done.count(), 0);
        QTest::qWait(ui::EditKvmView::trustRestsMs + 300);
        QVERIFY(!box.mayTrust());
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        QCOMPARE(saveAsked.count(), 2);
        QCOMPARE(third.signIns, 0);

        // A yes with a password that the KVM refuses: said at the lines, in the KVM's words, and
        // the two are shown no longer. Nothing is changed, and the yes is not kept.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 8000);
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"not it"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), u"Login rejected - check the username and password"_s);
        QVERIFY(!box.differs());
        QCOMPARE(box.presented(), QString());
        QCOMPARE(third.signIns, 1);
        QCOMPARE(held.kvm(id), before);
        QVERIFY(showsNothingOf(box, u"not it"_s));
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.differs());
        QCOMPARE(third.signIns, 1);
        QCOMPARE(held.kvm(id), before);
    }

    void aYesIsNotPassedOnInTheFirstMomentAfterTheTwoCertificatesHaveCome()
    {
        // (The box by itself: what it is told here is what whoever changes the KVM tells it.)
        ui::EditKvmView box;
        lookAt(box);
        const QSignalSpy trustAsked(&box, &ui::EditKvmView::trustAsked);
        const QSignalSpy saveAsked(&box, &ui::EditKvmView::saveAsked);
        ui::EditKvmView::Shown kvm;
        kvm.id = u"0a1b2c3d"_s;
        kvm.called = u"NAS"_s;
        kvm.https = true;
        const QByteArray known = sha256Of("first");
        const QByteArray one = sha256Of("second");
        const QByteArray another = sha256Of("comet");
        box.show(kvm, false);
        QVERIFY(!box.mayTrust());

        // The two of a review come, and they differ.
        box.began(u"looking"_s);
        QElapsedTimer since;
        since.start();
        box.reviewed(known, one);
        QVERIFY(!box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        // (About a second; never less than most of one.)
        QVERIFY2(since.elapsed() >= ui::EditKvmView::trustRestsMs - 100, qPrintable(QString::number(since.elapsed())));
        QVERIFY(ui::EditKvmView::trustRestsMs >= 800 && ui::EditKvmView::trustRestsMs <= 1500);
        box.trust();
        QCOMPARE(trustAsked.count(), 1);
        QCOMPARE(trustAsked.at(0).at(1).toByteArray(), one);
        // A yes that was refused or given up leaves the two as they are: it is taken again.
        box.began(u"trusting"_s);
        box.refused(u"The device now presents a different certificate than the one shown"_s);
        QVERIFY(box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 2);
        // What it would be said to is another: the moment begins anew.
        box.began(u"trusting"_s);
        box.gaveUp();
        box.closeReview();
        QVERIFY(!box.mayTrust());
        box.began(u"looking"_s);
        box.reviewed(known, another);
        QVERIFY(!box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.trust();
        QCOMPARE(trustAsked.count(), 3);
        QCOMPARE(trustAsked.at(2).at(1).toByteArray(), another);
        box.began(u"trusting"_s);
        box.gaveUp();
        box.closeReview();

        // So with the two of a saving that was refused over them; and again when what is shown
        // at that address is another by the time of the yes.
        box.began(u"saving"_s);
        box.differing(known, one, u"192.168.1.30"_s);
        QVERIFY(!box.mayTrust());
        box.saveKnownByWhatIsShown(u"NAS"_s, u"192.168.1.30"_s, u"admin"_s, u"secret"_s, QString());
        QCOMPARE(saveAsked.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(u"NAS"_s, u"192.168.1.30"_s, u"admin"_s, u"secret"_s, QString());
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(saveAsked.at(0).at(6).toByteArray(), one);
        box.began(u"saving"_s);
        box.differing(known, another, u"192.168.1.30"_s);
        QVERIFY(!box.mayTrust());
        box.saveKnownByWhatIsShown(u"NAS"_s, u"192.168.1.30"_s, u"admin"_s, u"secret"_s, QString());
        QCOMPARE(saveAsked.count(), 1);
        // And where nothing is shown, there is nothing to say yes to.
        box.closeReview();
        QTest::qWait(ui::EditKvmView::trustRestsMs + 200);
        QVERIFY(!box.mayTrust());
        box.saveKnownByWhatIsShown(u"NAS"_s, u"192.168.1.30"_s, u"admin"_s, u"secret"_s, QString());
        box.trust();
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(trustAsked.count(), 3);
    }

    void aYesToPlainHttpAtTheWayChosenIsTakenWithTheAddressTheBoxThenShows()
    {
        // Typed with a port and nothing before it, for a JetKVM spoken to over HTTPS, where only the plain way
        // answers: that way is chosen, and the box shows what is given up there, and the address chosen in its
        // line, so that the yes it gives goes with that address (it took one only with "http://" typed).
        JetKvmByHand before(tls("first"));
        JetKvmByHand plain;
        core::KvmAdding::Pace pace = quick();
        Held held(pace);
        core::OwnKvm jet;
        jet.address = before.address().toString();
        jet.name = u"Desk"_s;
        jet.kind = u"jetkvm"_s;
        jet.knownByCertificate = true;
        jet.certificate = sha256Of("first");
        const QString id = held.own.wall().add(jet, u"secret"_s).value();
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        const QSignalSpy retyped(&box, &ui::EditKvmView::addressRetyped);
        const QString typed = u"127.0.0.1:%1"_s.arg(plain.stand->serverPort());
        box.save(box.name(), typed, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.isPlainHttpThere());
        QCOMPARE(box.newAddress(), typed);
        QCOMPARE(retyped.count(), 1);
        QCOMPARE(retyped.constLast().constFirst().toString(), plain.address().toString());
        QCOMPARE(plain.signIns, 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), retyped.constLast().constFirst().toString(), box.user(), u"secret"_s,
                                   box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(held.kvm(id).address, plain.address().toString());
        QCOMPARE(plain.signIns, 1);
    }

    void aYesToPlainHttpAndThenToTheCertificateOverHttpsEndWhereTheBoxShowsTheAddressChosen()
    {
        // Typed "http://", for a JetKVM spoken to over HTTPS and known by one certificate, where that JetKVM's
        // HTTPS side shows another: the yes to plain HTTP there, then the HTTPS way chosen beside it, and the yes
        // to its certificate, given with the address the box then shows. (The two yeses took turns for ever.)
        JetKvmByHand before(tls("first"));
        JetKvmByHand plain;
        JetKvmByHand secure(tls("second"));
        core::KvmAdding::Pace pace = quick();
        pace.otherWay = twoWays(plain.address(), secure.address());
        Held held(pace);
        core::OwnKvm jet;
        jet.address = before.address().toString();
        jet.name = u"Desk"_s;
        jet.kind = u"jetkvm"_s;
        jet.knownByCertificate = true;
        jet.certificate = sha256Of("first");
        const QString id = held.own.wall().add(jet, u"secret"_s).value();
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        const QSignalSpy retyped(&box, &ui::EditKvmView::addressRetyped);
        box.save(box.name(), plain.address().toString(), box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.isPlainHttpThere());
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), plain.address().toString(), box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        // HTTPS was chosen beside it, and shows another certificate than it is known by: that is put before
        // whoever sits there, with the address chosen in the box's line.
        QVERIFY(box.differs());
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("second")));
        const QString chosen = u"127.0.0.1:%1"_s.arg(secure.stand->serverPort());
        QVERIFY(!retyped.isEmpty());
        QCOMPARE(retyped.constLast().constFirst().toString(), chosen);
        QCOMPARE(secure.signIns, 0);
        QCOMPARE(plain.signIns, 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), retyped.constLast().constFirst().toString(), box.user(), u"secret"_s,
                                   box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(held.kvm(id).address, secure.address().toString());
        QCOMPARE(held.kvm(id).certificate, sha256Of("second"));
        QCOMPARE(secure.signIns, 1);
        QCOMPARE(plain.signIns, 0);
    }

    void aJetKvmTypedPlainWhereItOffersHttpsIsAddedOverHttpsAndTheBoxShowsItsCertificate()
    {
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        core::KvmAdding::Pace pace = quick();
        pace.otherWay = twoWays(plain.address(), secure.address());
        Held held(pace);
        ui::AddKvmView box;
        held.own.serve(box);
        box.add(plain.address().toString(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        const QString host = u"127.0.0.1:%1"_s.arg(secure.stand->serverPort());
        QCOMPARE(box.added(), host + u" added (JetKVM)"_s);
        QCOMPARE(box.certificate(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(held.own.wall().kvms().constFirst().address, secure.address().toString());
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(secure.signIns, 1);
    }

    void aJetKvmThatAnswersOverHttpsNowIsSavedThereAndItsBoxSaysSo()
    {
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        core::KvmAdding::Pace pace = quick();
        pace.otherWay = twoWays(plain.address(), secure.address());
        Held held(pace);
        core::OwnKvm jet;
        jet.address = plain.address().toString();
        jet.name = u"Desk"_s;
        jet.kind = u"jetkvm"_s;
        const QString id = held.own.wall().add(jet, u"secret"_s).value();
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        QVERIFY(!box.isOntoHttps());
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        // Its password typed anew, at the address it has: it goes over HTTPS, where the JetKVM answers now,
        // and the box stays on what it is known by from here on, and says so.
        saveAs(box, box.name(), u"secret"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(done.count(), 0);
        QCOMPARE(box.savedAs(), u"\"Desk\""_s);
        QVERIFY(box.isOntoHttps());
        QVERIFY(box.isHttps());
        QCOMPARE(box.certificate(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.address(), u"127.0.0.1:%1"_s.arg(secure.stand->serverPort()));
        QCOMPARE(held.kvm(id).address, secure.address().toString());
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(secure.signIns, 1);
        // The next thing that is asked says nothing of it any more.
        saveAs(box, u"Desk 2"_s);
        QVERIFY(!box.isOntoHttps());
    }

    void aSaveThatChangesWhatAKvmIsKnownByLeavesTheBoxOpenOnWhatItIsKnownByNow()
    {
        Held held;
        const MadeUpKvm plain;
        const MadeUpKvm encrypted(tls("first"));
        const MadeUpKvm elsewhere;
        const QString id = held.put(plain, u"NAS"_s);
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy filledAnew(&box, &ui::EditKvmView::filledAnew);
        QCOMPARE(box.certificate(), QString());

        // From the plain way to HTTPS, somewhere else. It is known by none, and what answers
        // there shows a certificate: that one is put before whoever sits there first, beside
        // "none yet", and nothing but the question what it is has gone there. (This had it
        // signed in to at once, and known by whatever answered: turned round.)
        const QString there = u"127.0.0.1:%1"_s.arg(encrypted.stand->serverPort());
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.differs());
        QCOMPARE(box.remembered(), QString());
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("first")));
        QVERIFY(!box.isOnWindowsWord());
        QCOMPARE(box.newAddress(), there);
        QCOMPARE(encrypted.signIns, 0);
        QVERIFY(!held.kvm(id).knownByCertificate);
        // With the yes it is signed in to, and known by that one from now on.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(encrypted.signIns, 1);
        QCOMPARE(held.kvm(id).certificate, sha256Of("first"));
        // The box does not just close: it shows that certificate, whole, and says that the KVM
        // was saved. Its lines are the KVM as it is now.
        QCOMPARE(done.count(), 0);
        QCOMPARE(filledAnew.count(), 1);
        QCOMPARE(box.machine(), id);
        QCOMPARE(box.savedAs(), u"\"NAS\""_s);
        QCOMPARE(box.certificate(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.certificate().size(), 95);
        QCOMPARE(box.address(), u"127.0.0.1:%1"_s.arg(encrypted.stand->serverPort()));
        QVERIFY(box.isHttps());
        QVERIFY(!box.isOverPlainHttp());
        QVERIFY(showsNothingOf(box, u"secret"_s));

        // A saving that changes neither closes the box, as ever: from the box that stayed, too,
        // which saves over the KVM as it is now.
        saveAs(box, u"Storage"_s);
        QCOMPARE(box.savedAs(), QString());
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), u"saved"_s);
        QCOMPARE(done.at(0).at(1).toString(), u"\"Storage\""_s);
        QCOMPARE(filledAnew.count(), 1);
        box.closed();

        // And to a place that is spoken to in the clear: that it is, is said in the box that
        // stays, and it is known by no certificate any more.
        box.openFor(id, false);
        QVERIFY(!box.isOverPlainHttp());
        box.save(box.name(), elsewhere.stand->address(), box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        // (With the yes to that: this had the password sent there at once, with nobody asked.
        // Turned round; what goes there without the yes is in
        // aPasswordTypedForPlainHttpElsewhereGoesThereOnlyOnceTheBoxSaidYesToThat.)
        QVERIFY(box.isPlainHttpThere());
        QCOMPARE(elsewhere.signIns, 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), elsewhere.stand->address(), box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(done.count(), 1);
        QCOMPARE(box.savedAs(), u"\"Storage\""_s);
        QVERIFY(box.isOverPlainHttp());
        QVERIFY(!box.isHttps());
        QCOMPARE(box.certificate(), QString());
        QCOMPARE(box.address(), elsewhere.stand->address());
        QVERIFY(!held.kvm(id).knownByCertificate);
        // Closed, nothing of it is said any more.
        box.closed();
        QCOMPARE(box.savedAs(), QString());
    }

    void aCancelInTheTurnInWhichItWasSavedIsToldThatItWasSaved()
    {
        Held held;
        const MadeUpKvm kvm;
        const QString id = held.put(kvm, u"NAS"_s);
        ui::EditKvmView box;
        held.own.serve(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);

        // A name alone is on the wall before the saving has come back: "Cancel" in that turn
        // finds the box still waiting to be told.
        saveAs(box, u"Storage"_s);
        QVERIFY(box.busy());
        QCOMPARE(held.kvm(id).name, u"Storage"_s);
        box.giveUp();
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), u"saved"_s);
        QCOMPARE(done.at(0).at(1).toString(), u"\"Storage\""_s);
        QVERIFY(!box.busy());
        QCOMPARE(box.problem(), QString());
    }

    void aCancelInTheTurnInWhichACertificateWasTrustedIsToldThatItWasTrusted()
    {
        Held held;
        const MadeUpKvm kvm(tls("second"));
        const QString id = held.put(kvm, u"Build server"_s, "first");
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        held.own.openFor(box, id, true);
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy giveUpAsked(&box, &ui::EditKvmView::giveUpAsked);
        // The wall says that it has changed while the certificate is remembered: "Cancel" at
        // that very moment.
        connect(&held.own.wall(), &core::LocalWall::changed, &box, [&box] { box.giveUp(); });
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.trust();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
        QCOMPARE(giveUpAsked.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), u"trusted"_s);
        QCOMPARE(done.at(0).at(1).toString(), u"\"Build server\""_s);
        QCOMPARE(held.kvm(id).certificate, sha256Of("second"));
        QVERIFY(!box.busy());
        QCOMPARE(box.problem(), QString());
    }

    void aBoxThatWasOpenedBeforeAnotherBoxChangedItsKvmDoesNotSaveOverThat()
    {
        Held held;
        MadeUpKvm kvm;
        const QString id = held.put(kvm, u"NAS"_s, nullptr, u"Rack 2"_s);
        ui::EditKvmView first;
        ui::EditKvmView second;
        held.own.serve(first);
        held.own.serve(second);
        first.openFor(id, false);
        second.openFor(id, false);
        const QSignalSpy doneInTheFirst(&first, &ui::EditKvmView::done);
        const QSignalSpy doneInTheSecond(&second, &ui::EditKvmView::done);
        const QSignalSpy filledAnew(&first, &ui::EditKvmView::filledAnew);

        // The second window renames it.
        saveAs(second, u"Storage"_s);
        QTRY_COMPARE(doneInTheSecond.count(), 1);
        // The first, open since before that, changes the group: its lines are what they were
        // when it was opened, the old name among them.
        first.save(first.name(), first.address(), first.user(), QString(), u"Rack 9"_s);
        QTRY_VERIFY(!first.busy());
        QCOMPARE(doneInTheFirst.count(), 0);
        QCOMPARE(held.kvm(id).name, u"Storage"_s);
        QCOMPARE(held.kvm(id).group, u"Rack 2"_s);
        QCOMPARE(first.problem(), u"This device was changed meanwhile. Look at it again. The fields were filled in "
                                  "again with what is saved now."_s);
        // And the box shows the KVM as it is now, its lines to be filled anew.
        QCOMPARE(filledAnew.count(), 1);
        QCOMPARE(first.machine(), id);
        QCOMPARE(first.name(), u"Storage"_s);
        QCOMPARE(first.called(), u"Storage"_s);
        QCOMPARE(first.group(), u"Rack 2"_s);
        // Typed over that, the group is saved, and the name stays what the other window made it.
        first.save(first.name(), first.address(), first.user(), QString(), u"Rack 9"_s);
        QTRY_COMPARE(doneInTheFirst.count(), 1);
        QCOMPARE(held.kvm(id).name, u"Storage"_s);
        QCOMPARE(held.kvm(id).group, u"Rack 9"_s);
        first.closed();

        // So with a password that the other window saved meanwhile, of which nothing shows.
        first.openFor(id, false);
        second.openFor(id, false);
        kvm.password = u"another one"_s;
        saveAs(second, second.name(), u"another one"_s);
        QTRY_COMPARE_WITH_TIMEOUT(doneInTheSecond.count(), 2, 20000);
        saveAs(first, u"NAS again"_s);
        QTRY_VERIFY(!first.busy());
        QCOMPARE(doneInTheFirst.count(), 1);
        QVERIFY(first.problem().startsWith(u"This device was changed meanwhile."_s));
        QCOMPARE(filledAnew.count(), 2);
        QCOMPARE(held.kvm(id).name, u"Storage"_s);
        // And a KVM that the other window took off the wall: the box is told that it went, and
        // there is none to show in its place, and nothing is saved over it.
        first.closed();
        first.openFor(id, false);
        second.openFor(id, false);
        second.remove();
        QVERIFY(first.isGone());
        saveAs(first, u"NAS again"_s);
        QVERIFY(!first.busy());
        QVERIFY(first.problem().isEmpty());
        QCOMPARE(filledAnew.count(), 2);
        QVERIFY(held.own.wall().kvms().isEmpty());
    }

    void afterAYesToWhatIsShownNoLongerTheBoxSaysSoAndItsYesRestsForLonger()
    {
        // (The box by itself: what it is told here is what whoever changes the KVM tells it.)
        ui::EditKvmView box;
        lookAt(box);
        const QSignalSpy trustAsked(&box, &ui::EditKvmView::trustAsked);
        const QSignalSpy saveAsked(&box, &ui::EditKvmView::saveAsked);
        ui::EditKvmView::Shown kvm;
        kvm.id = u"0a1b2c3d"_s;
        kvm.called = u"NAS"_s;
        kvm.https = true;
        const QByteArray known = sha256Of("first");
        const QByteArray one = sha256Of("second");
        const QByteArray another = sha256Of("comet");
        const QString at = u"192.168.1.30"_s;
        const auto sayYesToSaving = [&] { box.saveKnownByWhatIsShown(u"NAS"_s, at, u"admin"_s, u"secret"_s, {}); };
        box.show(kvm, false);
        QVERIFY(ui::EditKvmView::trustRestsAnewMs >= 2 * ui::EditKvmView::trustRestsMs);

        // The two of a saving that was refused over them: shown for the first time, they are
        // nothing a yes was given to.
        box.began(u"saving"_s);
        box.differing(known, one, at);
        QVERIFY(!box.yesWasToAnother());
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        sayYesToSaving();
        QCOMPARE(saveAsked.count(), 1);
        // The same two again: not another, and the usual moment.
        box.began(u"saving"_s);
        box.differing(known, one, at);
        QVERIFY(!box.yesWasToAnother());
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), ui::EditKvmView::trustRestsAnewMs - 500);
        sayYesToSaving();
        QCOMPARE(saveAsked.count(), 2);
        // What is shown there by then is a third: the box says that it is another than the one
        // the yes was given to, and passes no yes on for the usual moment and more.
        box.began(u"saving"_s);
        QElapsedTimer since;
        since.start();
        box.differing(known, another, at);
        QVERIFY(box.differs());
        QVERIFY(box.yesWasToAnother());
        QCOMPARE(box.presented(), core::kvm::fingerprint(another));
        QVERIFY(!box.mayTrust());
        sayYesToSaving();
        QTest::qWait(ui::EditKvmView::trustRestsMs + 300);
        QVERIFY(!box.mayTrust());
        sayYesToSaving();
        QCOMPARE(saveAsked.count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 8000);
        QVERIFY2(since.elapsed() >= ui::EditKvmView::trustRestsAnewMs - 100,
                 qPrintable(QString::number(since.elapsed())));
        // It says so until a yes is given to those two; and that yes is to the third.
        QVERIFY(box.yesWasToAnother());
        sayYesToSaving();
        QCOMPARE(saveAsked.count(), 3);
        QCOMPARE(saveAsked.at(2).at(6).toByteArray(), another);
        QVERIFY(!box.yesWasToAnother());
        box.began(u"saving"_s);
        box.gaveUp();
        QVERIFY(!box.yesWasToAnother());
        box.closeReview();
        // Windows vouches there now, where a certificate was said yes to: another thing, too.
        box.began(u"saving"_s);
        box.differing(known, one, at);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        sayYesToSaving();
        box.began(u"saving"_s);
        box.differing(known, one, at, true);
        QVERIFY(box.isOnWindowsWord());
        QVERIFY(box.yesWasToAnother());
        QVERIFY(!box.mayTrust());
        box.closeReview();
        QVERIFY(!box.isOnWindowsWord());

        // So with the two of a look at its certificate, where the yes was to one that the KVM
        // shows no longer.
        box.began(u"looking"_s);
        box.reviewed(known, one);
        QVERIFY(!box.yesWasToAnother());
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.trust();
        QCOMPARE(trustAsked.count(), 1);
        box.began(u"trusting"_s);
        since.restart();
        box.reviewed(known, another);
        QVERIFY(box.isReviewed());
        QVERIFY(box.yesWasToAnother());
        QCOMPARE(box.presented(), core::kvm::fingerprint(another));
        QCOMPARE(box.problem(), QString());
        QVERIFY(!box.mayTrust());
        QTest::qWait(ui::EditKvmView::trustRestsMs + 300);
        QVERIFY(!box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 8000);
        QVERIFY2(since.elapsed() >= ui::EditKvmView::trustRestsAnewMs - 100,
                 qPrintable(QString::number(since.elapsed())));
        box.trust();
        QCOMPARE(trustAsked.count(), 2);
        QCOMPARE(trustAsked.at(1).at(1).toByteArray(), another);
        // Looked at anew, they are nothing a yes was given to.
        box.began(u"trusting"_s);
        box.gaveUp();
        box.closeReview();
        box.began(u"looking"_s);
        box.reviewed(known, another);
        QVERIFY(!box.yesWasToAnother());
    }

    void theMomentBeforeAYesRunsOnlyWhileTheTwoAreLookedAt()
    {
        // The two arrive when the KVM has answered. Somebody saved and went to another program
        // meanwhile: the moment in which a yes takes no click does not pass with nobody looking,
        // or the click that brings the window back would be a yes.
        ui::EditKvmView box;
        const QSignalSpy trustAsked(&box, &ui::EditKvmView::trustAsked);
        const QSignalSpy saveAsked(&box, &ui::EditKvmView::saveAsked);
        ui::EditKvmView::Shown kvm;
        kvm.id = u"0a1b2c3d"_s;
        kvm.called = u"NAS"_s;
        kvm.https = true;
        const QByteArray known = sha256Of("first");
        const QByteArray shown = sha256Of("second");
        box.show(kvm, false);
        QVERIFY(!box.isLookedAt());

        // Nobody has said that the box is looked at: however long the two are there, no yes.
        box.began(u"looking"_s);
        box.reviewed(known, shown);
        QTest::qWait(ui::EditKvmView::trustRestsMs + 400);
        QVERIFY(!box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 0);
        // Looked at: the moment begins then.
        QElapsedTimer since;
        since.start();
        box.setLookedAt(true);
        QVERIFY(!box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        QVERIFY2(since.elapsed() >= ui::EditKvmView::trustRestsMs - 100, qPrintable(QString::number(since.elapsed())));
        // Not looked at any more: no yes there and then, and none while that lasts.
        box.setLookedAt(false);
        QVERIFY(!box.mayTrust());
        box.trust();
        QTest::qWait(ui::EditKvmView::trustRestsMs + 400);
        QVERIFY(!box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 0);
        // Looked at again: from the beginning.
        since.restart();
        box.setLookedAt(true);
        QVERIFY(!box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        QVERIFY2(since.elapsed() >= ui::EditKvmView::trustRestsMs - 100, qPrintable(QString::number(since.elapsed())));
        box.trust();
        QCOMPARE(trustAsked.count(), 1);
        box.began(u"trusting"_s);
        box.gaveUp();
        box.closeReview();

        // So with the two of a saving that was refused over them, which come while nobody looks.
        box.setLookedAt(false);
        box.began(u"saving"_s);
        box.differing(known, shown, u"192.168.1.30"_s);
        QTest::qWait(ui::EditKvmView::trustRestsMs + 400);
        QVERIFY(!box.mayTrust());
        box.saveKnownByWhatIsShown(u"NAS"_s, u"192.168.1.30"_s, u"admin"_s, u"secret"_s, QString());
        QCOMPARE(saveAsked.count(), 0);
        box.setLookedAt(true);
        QVERIFY(!box.mayTrust());
        box.saveKnownByWhatIsShown(u"NAS"_s, u"192.168.1.30"_s, u"admin"_s, u"secret"_s, QString());
        QCOMPARE(saveAsked.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(u"NAS"_s, u"192.168.1.30"_s, u"admin"_s, u"secret"_s, QString());
        QCOMPARE(saveAsked.count(), 1);
    }

    void aYesIsOneOnlyForWhatItsBoxWasLastToldToShowAndOnce()
    {
        // The screens hold the box's view, and can send its signals as they can call a
        // function. Whoever changes the KVM takes a yes only for what it has told this box to
        // show, while the box shows it.
        Held held;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm other(tls("second"));
        const MadeUpKvm elsewhere(tls("second"));
        const QString id = held.put(here, u"Build server"_s, "first");
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        const QString there = u"127.0.0.1:%1"_s.arg(other.stand->serverPort());
        const QString another = u"127.0.0.1:%1"_s.arg(elsewhere.stand->serverPort());
        const auto sendByHand = [&](const QString& address, const QByteArray& certificate, const QString& password) {
            emit box.saveAsked(id, box.name(), address, box.user(), password, box.group(), certificate, false);
            return QTest::qWaitFor([&box] { return !box.busy(); }, 20000);
        };

        // The signal is sent by hand, with the certificate that is shown at that address, before
        // the box was told to show anything: no yes. The two are put on the screen, and what
        // answers there was asked what it is and no more.
        QVERIFY(sendByHand(there, sha256Of("second"), u"secret"_s));
        QVERIFY(box.differs());
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("second")));
        QCOMPARE(other.signIns, 0);
        QCOMPARE(held.kvm(id), before);
        // While they have only just come: no yes, though it is the very certificate.
        QVERIFY(!box.mayTrust());
        QVERIFY(sendByHand(there, sha256Of("second"), u"secret"_s));
        QVERIFY(box.differs());
        QCOMPARE(other.signIns, 0);
        // Once they were looked at for a moment: none with a certificate that was never shown,
        // and none for another address than the one it was shown at, where the same is shown.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        QVERIFY(sendByHand(there, sha256Of("comet"), u"secret"_s));
        QVERIFY(box.differs());
        QCOMPARE(other.signIns, 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        QVERIFY(sendByHand(another, sha256Of("second"), u"secret"_s));
        QVERIFY(box.differs());
        QCOMPARE(box.newAddress(), another);
        QCOMPARE(elsewhere.signIns, 0);
        QCOMPARE(other.signIns, 0);
        box.closeReview();

        // The yes as the box gives it is taken, once: with a password the KVM refuses, it is
        // signed in to and nothing is saved. Sent by hand after that, the same certificate for
        // the same address is no yes any more: the box does not show it, and it was used.
        box.save(box.name(), there, box.user(), u"not it"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(box.differs(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"not it"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), u"Login rejected - check the username and password"_s);
        QVERIFY(!box.differs());
        QCOMPARE(other.signIns, 1);
        QVERIFY(sendByHand(there, sha256Of("second"), u"secret"_s));
        QVERIFY(box.differs());
        QCOMPARE(other.signIns, 1);
        QCOMPARE(held.kvm(id), before);
        QVERIFY(changed.isEmpty());
        QCOMPARE(done.count(), 0);
        // And the box's own yes to what it shows now saves it.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(other.signIns, 2);
        QCOMPARE(held.kvm(id).certificate, sha256Of("second"));
        box.closed();

        // So with the yes to the certificate a KVM shows at the address it has: sent by hand
        // for one that was never shown, nothing is asked of the KVM and nothing is trusted.
        auto was = std::make_unique<MadeUpKvm>(tls("first"));
        const QString reset = held.put(*was, u"Database"_s, "first");
        const quint16 port = was->stand->serverPort();
        was.reset();
        const MadeUpKvm now(tls("comet"), port);
        QVERIFY(now.stand->isListening());
        const core::OwnKvm resetBefore = held.kvm(reset);
        box.openFor(reset, false);
        emit box.trustAsked(reset, sha256Of("comet"));
        QVERIFY(!box.busy());
        QTest::qWait(300);
        QVERIFY(now.stand->asked.isEmpty());
        QCOMPARE(held.kvm(reset), resetBefore);
        // Shown, and looked at for a moment: not with another certificate than the one shown;
        // and the box's own yes is taken.
        box.review();
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        const qsizetype asked = now.stand->asked.size();
        emit box.trustAsked(reset, sha256Of("second"));
        QVERIFY(!box.busy());
        QTest::qWait(300);
        QCOMPARE(now.stand->asked.size(), asked);
        QCOMPARE(held.kvm(reset), resetBefore);
        box.trust();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
        QCOMPARE(held.kvm(reset).certificate, sha256Of("comet"));
    }

    void whatWasDoneInTheTurnInWhichItsBoxWasClosedIsSaidForItsWindowToTell()
    {
        // The window goes out of sight between the wall's saving and the turn in which the box
        // would be told: the box is closed from outside, and what does the saving is ended with
        // it. It was saved all the same, and that is said.
        Held held;
        const MadeUpKvm kvm;
        const MadeUpKvm encrypted(tls("first"));
        MadeUpKvm elsewhere;
        const QString id = held.put(kvm, u"NAS"_s);
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy asItClosed(&box, &ui::EditKvmView::doneAsItClosed);
        const auto said = [&asItClosed](qsizetype which) {
            const QList<QVariant> one = asItClosed.value(which);
            return u"%1|%2|%3|%4|%5"_s.arg(one.value(0).toString(), one.value(1).toString(),
                                           one.value(2).toBool() ? u"known"_s : u""_s, one.value(3).toString(),
                                           one.value(4).toBool() ? u"plain"_s : u""_s);
        };

        // A name alone is on the wall before the saving has come back: closed in that turn.
        box.openFor(id, false);
        saveAs(box, u"Storage"_s);
        QVERIFY(box.busy());
        QCOMPARE(held.kvm(id).name, u"Storage"_s);
        box.close();
        QCOMPARE(asItClosed.count(), 1);
        QCOMPARE(said(0), u"saved|\"Storage\"|||"_s);
        QVERIFY(!box.busy());
        QCOMPARE(box.machine(), QString());
        // And the box itself is told nothing after that: it is closed.
        QTest::qWait(200);
        QCOMPARE(done.count(), 0);
        QCOMPARE(asItClosed.count(), 1);

        // One that is signed in to first, closed at the moment the wall says that it has
        // changed. It was moved to where a certificate is shown, with the yes to that one: the
        // box would have stayed open on what the KVM is known by now, so that is said with it.
        const QString there = u"127.0.0.1:%1"_s.arg(encrypted.stand->serverPort());
        box.openFor(id, false);
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(box.differs(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        const auto atTheWallsWord = connect(&held.own.wall(), &core::LocalWall::changed, &box, [&box] { box.close(); });
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_COMPARE_WITH_TIMEOUT(asItClosed.count(), 2, 20000);
        QCOMPARE(said(1), u"saved|\"Storage\"|known|%1|"_s.arg(core::kvm::fingerprint(sha256Of("first"))));
        QCOMPARE(held.kvm(id).certificate, sha256Of("first"));
        // And to a place that is spoken to in the clear: that it is, is said with it. (With the
        // yes to that: this had the password sent there with nobody asked. Turned round.)
        box.openFor(id, false);
        box.save(box.name(), elsewhere.stand->address(), box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(box.isPlainHttpThere(), 20000);
        QCOMPARE(asItClosed.count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), elsewhere.stand->address(), box.user(), u"secret"_s, box.group());
        QTRY_COMPARE_WITH_TIMEOUT(asItClosed.count(), 3, 20000);
        QCOMPARE(said(2), u"saved|\"Storage\"|known||plain"_s);
        disconnect(atTheWallsWord);
        QTest::qWait(200);
        QCOMPARE(done.count(), 0);
        QCOMPARE(asItClosed.count(), 3);

        // A change that the wall had not taken when its box was closed is not saved, and
        // nothing is said of it.
        makeSlowToSignIn(elsewhere, 600);
        const core::OwnKvm before = held.kvm(id);
        box.openFor(id, false);
        saveAs(box, u"Another name"_s, u"secret"_s);
        QTRY_COMPARE(elsewhere.asked("/api/auth/login"), 2);
        QVERIFY(box.busy());
        box.close();
        QTest::qWait(900);
        QCOMPARE(asItClosed.count(), 3);
        QCOMPARE(held.kvm(id), before);

        // A certificate that was trusted in the turn in which its box was closed.
        const MadeUpKvm reset(tls("second"));
        const QString build = held.put(reset, u"Build server"_s, "first");
        held.own.openFor(box, build, true);
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        connect(&held.own.wall(), &core::LocalWall::changed, &box, [&box] { box.close(); });
        box.trust();
        QTRY_COMPARE_WITH_TIMEOUT(asItClosed.count(), 4, 20000);
        QCOMPARE(said(3), u"trusted|\"Build server\"|||"_s);
        QCOMPARE(held.kvm(build).certificate, sha256Of("second"));
        QTest::qWait(200);
        QCOMPARE(done.count(), 0);
        QCOMPARE(asItClosed.count(), 4);
    }

    void aKvmThatWasAddedInTheTurnInWhichItsBoxWasClosedIsSaidForItsWindowToTell()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        ui::AddKvmView box;
        ui::OwnWall own(settings, secrets, quick());
        own.serve(box);
        const MadeUpKvm kvm(tls("comet"));
        const QSignalSpy asItClosed(&box, &ui::AddKvmView::addedAsItClosed);

        // The window goes out of sight at the moment the wall says that it has changed: the
        // box is closed from outside, with the KVM on the wall and the box not told yet.
        const auto atTheWallsWord = connect(&own.wall(), &core::LocalWall::changed, &box, [&box] { box.close(); });
        box.add(kvm.stand->address(), QString(), u"secret"_s, QString());
        QTRY_COMPARE_WITH_TIMEOUT(asItClosed.count(), 1, 20000);
        disconnect(atTheWallsWord);
        // What the box would have said, and the certificate it would have shown with it.
        const QString host = u"127.0.0.1:%1"_s.arg(kvm.stand->serverPort());
        QCOMPARE(asItClosed.at(0).at(0).toString(), host + u" added (RM10)"_s);
        QCOMPARE(asItClosed.at(0).at(1).toString(), core::ServerClient::written(sha256Of("comet")));
        QCOMPARE(own.wall().kvms().size(), 1);
        // The box itself says nothing of it: it is closed, and begins anew when it is opened.
        QVERIFY(!box.busy());
        QCOMPARE(box.added(), QString());
        QCOMPARE(box.certificate(), QString());
        QCOMPARE(box.count(), 0);

        // One that was given up by the closing, before the wall had it: nothing is said, and
        // nothing is added.
        MadeUpKvm slow;
        makeSlowToSignIn(slow, 600);
        box.add(slow.stand->address(), QString(), u"secret"_s, QString());
        QTRY_COMPARE(slow.asked("/api/auth/login"), 1);
        box.close();
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 400);
        QTest::qWait(900);
        QCOMPARE(asItClosed.count(), 1);
        QCOMPARE(own.wall().kvms().size(), 1);
        QCOMPARE(box.problem(), QString());
        // And the next one is told in the box, as ever.
        const MadeUpKvm next;
        box.add(next.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.count(), 1);
        QCOMPARE(asItClosed.count(), 1);
        QCOMPARE(own.wall().kvms().size(), 2);
    }

    void whatShowsTheCertificateOfAnotherKvmOfTheWallIsSaidInTheBoxAndTakesNoYes()
    {
        // Two KVMs of the wall have changed places: what answers at the second's address shows
        // the certificate the first is known by.
        Held held;
        const MadeUpKvm first(tls("first"));
        auto second = std::make_unique<MadeUpKvm>(tls("second"));
        const QString build = held.put(first, u"Build server"_s, "first");
        const QString database = held.put(*second, u"Database"_s, "second");
        const core::OwnKvm before = held.kvm(database);
        const quint16 port = second->stand->serverPort();
        second.reset();
        const MadeUpKvm atTheSeconds(tls("first"), port);
        QVERIFY(atTheSeconds.stand->isListening());
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        const QSignalSpy trustAsked(&box, &ui::EditKvmView::trustAsked);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        held.own.openFor(box, database, true);
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QCOMPARE(box.remembered(), core::kvm::fingerprint(sha256Of("second")));
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("first")));
        // The box is told whose it is, before anybody compares; and takes no yes to it, however
        // long the two are looked at: the wall would refuse it.
        QCOMPARE(box.presentedIsCalled(), u"\"Build server\""_s);
        QTest::qWait(ui::EditKvmView::trustRestsMs + 400);
        QVERIFY(!box.mayTrust());
        box.trust();
        QCOMPARE(trustAsked.count(), 0);
        QVERIFY(!box.busy());
        QCOMPARE(box.problem(), QString());
        QCOMPARE(held.kvm(database), before);
        QVERIFY(changed.isEmpty());
        // Back at the lines, nothing of it is said any more; and a KVM that shows a certificate
        // of its own is nobody else's.
        box.closed();
        QCOMPARE(box.presentedIsCalled(), QString());
        box.openFor(build, false);
        box.review();
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QCOMPARE(box.presentedIsCalled(), QString());
        QVERIFY(box.isSame());
    }

    void aBoxWhoseSavingCouldNotBeWrittenDownSavesAtItsNextTryWhatItStillHolds()
    {
        Held held;
        MadeUpKvm kvm;
        const QString id = held.put(kvm, u"NAS"_s);
        ui::EditKvmView box;
        held.own.serve(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy filledAnew(&box, &ui::EditKvmView::filledAnew);
        // Its password was changed on the KVM itself, and is typed here; the store does not
        // take it.
        kvm.password = u"another one"_s;
        held.secrets.refuses = true;
        saveAs(box, u"Storage"_s, u"another one"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY2(box.problem().startsWith(u"The change to this device could not be saved ("_s),
                 qPrintable(box.problem()));
        QCOMPARE(done.count(), 0);
        QCOMPARE(held.kvm(id).name, u"NAS"_s);
        // Nobody changed the KVM meanwhile: the same box saves it at its next try, and its
        // lines were not filled anew in between (what was typed into them would be gone).
        held.secrets.refuses = false;
        saveAs(box, u"Storage"_s, u"another one"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(filledAnew.count(), 0);
        QCOMPARE(done.count(), 1);
        QCOMPARE(held.kvm(id).name, u"Storage"_s);
        QCOMPARE(held.secretOf(id), core::testing::savedFor("another one", kvm.stand->address()));
    }

    void theAddressInTheBoxIsTheOneThatIsSaved()
    {
        // A KVM under a name that begins "xn--": one that was added so. (It is never asked
        // anything here: a name alone is saved without that, and it is known by no certificate,
        // so that its kept password goes to no address that is read otherwise.)
        Held held;
        core::OwnKvm known;
        known.address = u"https://xn--mnchen-3ya.de"_s;
        known.name = u"Far away"_s;
        const base::Result<QString> put = held.own.wall().add(known, u"secret"_s);
        QVERIFY(put.ok());
        const QString& id = put.value();
        ui::EditKvmView box;
        held.own.serve(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        // The address the box shows is written as it is saved, and is read back as that address.
        QCOMPARE(box.address(), u"xn--mnchen-3ya.de"_s);
        const base::Result<QUrl> read = core::kvm::addressFrom(box.address());
        QVERIFY2(read.ok(), qPrintable(box.address()));
        QCOMPARE(read.value().toString(QUrl::FullyEncoded), known.address);
        // So with nothing changed it is saved as it is, and so is another name alone.
        saveAs(box, box.name());
        QTRY_VERIFY(!box.busy());
        QCOMPARE(box.problem(), QString());
        QCOMPARE(done.count(), 1);
        box.closed();
        box.openFor(id, false);
        saveAs(box, u"Nearer"_s);
        QTRY_VERIFY(!box.busy());
        QCOMPARE(box.problem(), QString());
        QCOMPARE(done.count(), 2);
        QCOMPARE(held.kvm(id).name, u"Nearer"_s);
        QCOMPARE(held.kvm(id).address, known.address);
    }

    void aPasswordTypedForPlainHttpElsewhereGoesThereOnlyOnceTheBoxSaidYesToThat()
    {
        Held held;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm plain;
        const MadeUpKvm elsewhere;
        const QString id = held.put(here, u"Build server"_s, "first");
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy saveAsked(&box, &ui::EditKvmView::saveAsked);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        const QString there = plain.stand->address();
        const QString at = u"127.0.0.1:%1"_s.arg(plain.stand->serverPort());

        // An address that is spoken to in the clear, and its password typed with it: the box
        // shows what the KVM is known by beside that address, which shows nothing; nothing is
        // complained of, and not one line was made to that address.
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.differs());
        QVERIFY(box.isPlainHttpThere());
        QVERIFY(!box.isOnWindowsWord());
        QVERIFY(!box.yesWasToAnother());
        QCOMPARE(box.remembered(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.presented(), QString());
        QCOMPARE(box.newAddress(), at);
        QCOMPARE(box.problem(), QString());
        QTest::qWait(150);
        QCOMPARE(plain.stand->lines, 0);
        QCOMPARE(held.kvm(id), before);
        // The yes rests as the others do: none at once.
        QVERIFY(!box.mayTrust());
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        QCOMPARE(saveAsked.count(), 1);
        // Once it was looked at for its moment, a saving that is not the box's yes is none: the
        // wish as the lines' own button sends it, sent by hand, differs in nothing but that.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        emit box.saveAsked(id, box.name(), there, box.user(), u"secret"_s, box.group(), QByteArray(), false);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.differs() && box.isPlainHttpThere());
        QTest::qWait(150);
        QCOMPARE(plain.stand->lines, 0);
        // Nor is the yes for that address one for another that is spoken to in the clear:
        // that one is put before whoever sits there in its turn.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), elsewhere.stand->address(), box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.differs() && box.isPlainHttpThere());
        QCOMPARE(box.newAddress(), u"127.0.0.1:%1"_s.arg(elsewhere.stand->serverPort()));
        QTest::qWait(150);
        QCOMPARE(elsewhere.stand->lines, 0);
        QCOMPARE(plain.stand->lines, 0);
        QCOMPARE(held.kvm(id), before);
        QVERIFY(changed.isEmpty());

        // The way back to the lines, and saved again as it is: refused again, no yes is kept.
        box.closeReview();
        QVERIFY(!box.differs() && !box.isPlainHttpThere());
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(box.differs(), 20000);
        QCOMPARE(box.newAddress(), at);
        QCOMPARE(plain.stand->lines, 0);
        // The box's yes: asked what answers there, signed in to once, and saved. The box stays,
        // and says that the KVM is spoken to in the clear now.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        const qsizetype asked = saveAsked.count();
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        QCOMPARE(saveAsked.count(), asked + 1);
        QCOMPARE(saveAsked.constLast().at(6).toByteArray(), QByteArray());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(plain.signIns, 1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(held.kvm(id).address, plain.stand->address());
        QVERIFY(!held.kvm(id).knownByCertificate);
        QCOMPARE(held.secretOf(id), core::testing::savedFor("secret", plain.stand->address()));
        QVERIFY(!box.differs());
        QCOMPARE(done.count(), 0);
        QCOMPARE(box.savedAs(), u"\"Build server\""_s);
        QVERIFY(box.isOverPlainHttp());
        QCOMPARE(box.certificate(), QString());
        QVERIFY(showsNothingOf(box, u"secret"_s));
        QVERIFY(here.stand->asked.isEmpty());
    }

    void whatFirstShowsAtTheSignInAfterAYesIsSaidToBeAnotherWhenTheNextSavingShowsIt()
    {
        // Whoever answers at the address chooses the moment: the second certificate to the
        // questions, before the yes and after it, and a third only to the sign-in.
        Held held;
        const MadeUpKvm here(tls("first"));
        auto second = std::make_unique<MadeUpKvm>(tls("second"));
        const quint16 port = second->stand->serverPort();
        const QString there = u"127.0.0.1:%1"_s.arg(port);
        const QString id = held.put(here, u"Build server"_s, "first");
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        const auto save = [&] { box.save(box.name(), there, box.user(), u"secret"_s, box.group()); };
        const auto sayYes = [&] {
            box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        };
        save();
        QTRY_VERIFY_WITH_TIMEOUT(box.differs(), 20000);
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("second")));
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        // Replace the listener before the final discovery answer is sent. The sign-in can
        // only reach the third certificate, regardless of when this outer test resumes.
        const qsizetype asked = second->stand->asked.size();
        std::unique_ptr<MadeUpKvm> third;
        second->stand->beforeAnswer = [&] {
            if (second->stand->asked.size() == asked + 3) {
                second->stand->takeNoMore();
                third = std::make_unique<MadeUpKvm>(tls("comet"), port);
            }
        };
        sayYes();
        QTest::qWait(750);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(third != nullptr);
        QVERIFY(third->stand->isListening());
        QCOMPARE(second->stand->asked.size(), asked + 3);
        // The line the sign-in was to go out on met the third, and nothing went out on it: the
        // box is back at its lines, and says why.
        QCOMPARE(box.problem(), u"The device now presents a different certificate than the one shown"_s);
        QVERIFY(!box.differs());
        QVERIFY(third->stand->asked.isEmpty());
        QCOMPARE(second->signIns, 0);
        QCOMPARE(third->signIns, 0);
        QCOMPARE(held.kvm(id), before);

        // Saved again: what the KVM is known by, and the third. It is not the one that was
        // said yes to for this address: the box says so, and its yes rests for longer.
        second.reset();
        save();
        QTRY_VERIFY_WITH_TIMEOUT(box.differs(), 20000);
        QElapsedTimer since;
        since.start();
        QCOMPARE(box.remembered(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("comet")));
        QVERIFY(box.yesWasToAnother());
        QVERIFY(!box.yesWasOnWindowsWord());
        QVERIFY(!box.mayTrust());
        QTest::qWait(ui::EditKvmView::trustRestsMs + 300);
        QVERIFY(!box.mayTrust());
        sayYes();
        QCOMPARE(third->signIns, 0);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 8000);
        QVERIFY2(since.elapsed() >= ui::EditKvmView::trustRestsAnewMs - 300,
                 qPrintable(QString::number(since.elapsed())));
        // So for as long as no yes was given to it: back at the lines and saved once more.
        box.closeReview();
        save();
        QTRY_VERIFY_WITH_TIMEOUT(box.differs(), 20000);
        QVERIFY(box.yesWasToAnother());
        // With the yes to the third, that is the one that was said yes to there.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 8000);
        sayYes();
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(third->signIns, 1);
        QCOMPARE(held.kvm(id).certificate, sha256Of("comet"));
    }

    void whatTheBoxHoldsAgainstWhatIsShownAtAnAddressIsTheLastYesForThatAddressWhileItIsOpen()
    {
        // (The box by itself: what it is told here is what whoever changes the KVM tells it.)
        ui::EditKvmView box;
        lookAt(box);
        ui::EditKvmView::Shown kvm;
        kvm.id = u"0a1b2c3d"_s;
        kvm.called = u"NAS"_s;
        kvm.https = true;
        const QByteArray known = sha256Of("first");
        const QByteArray one = sha256Of("second");
        const QByteArray another = sha256Of("comet");
        const QString at = u"192.168.1.30"_s;
        const QString elsewhere = u"192.168.1.31"_s;
        const auto shown = [&](const QByteArray& certificate, const QString& where, bool onWindowsWord) {
            box.began(u"saving"_s);
            box.differing(known, certificate, where, onWindowsWord);
        };
        const auto sayYes = [&](const QString& where) {
            return QTest::qWaitFor([&box] { return box.mayTrust(); }, 8000) &&
                   (box.saveKnownByWhatIsShown(u"NAS"_s, where, u"admin"_s, u"secret"_s, {}), true);
        };
        const auto refusedAtTheSignIn = [&] {
            box.began(u"saving"_s);
            box.refused(u"The device now presents a different certificate than the one shown"_s);
        };
        box.show(kvm, false);

        // A yes to Windows' word for what is shown at an address; the sign-in is refused, and
        // the box is back at its lines. What is shown there at the next saving is a
        // certificate that Windows does not trust: another thing than was said yes to, and
        // no certificate stood there that it could be "another than".
        shown(one, at, true);
        QVERIFY(!box.yesWasToAnother());
        QVERIFY(sayYes(at));
        refusedAtTheSignIn();
        QVERIFY(!box.differs());
        shown(another, at, false);
        QVERIFY(box.yesWasToAnother());
        QVERIFY(box.yesWasOnWindowsWord());
        // Another that Windows trusts, too.
        box.closeReview();
        shown(another, at, true);
        QVERIFY(box.yesWasToAnother());
        QVERIFY(box.yesWasOnWindowsWord());
        // What was said yes to is nothing another.
        box.closeReview();
        shown(one, at, true);
        QVERIFY(!box.yesWasToAnother());
        QVERIFY(!box.yesWasOnWindowsWord());
        // At another address nothing was said yes to: what is shown there is new.
        box.closeReview();
        shown(another, elsewhere, false);
        QVERIFY(!box.yesWasToAnother());
        // A yes to a certificate, and Windows' word in its place afterwards.
        QVERIFY(sayYes(elsewhere));
        refusedAtTheSignIn();
        shown(another, elsewhere, true);
        QVERIFY(box.yesWasToAnother());
        QVERIFY(!box.yesWasOnWindowsWord());
        // And the yes for the first address is still held against what is shown there.
        box.closeReview();
        shown(another, at, false);
        QVERIFY(box.yesWasToAnother());
        // Plain HTTP shows nothing that could be another.
        box.closeReview();
        box.began(u"saving"_s);
        box.givingUpHttps(known, at);
        QVERIFY(box.isPlainHttpThere());
        QVERIFY(!box.yesWasToAnother());
        // Closed and opened again, or filled anew, the box holds nothing against anything.
        box.closed();
        box.show(kvm, false);
        shown(another, at, false);
        QVERIFY(!box.yesWasToAnother());
        QVERIFY(sayYes(at));
        box.began(u"saving"_s);
        box.refusedOver(kvm, u"This device was changed meanwhile."_s);
        shown(one, at, false);
        QVERIFY(!box.yesWasToAnother());
    }

    void whatABoxDoesNotShowIsNotOfferedToItAndAYesToItIsNone()
    {
        // The box shows the two of a look at a certificate. A second look is asked while a
        // first is under way: the box is told to wait, and stays on what it showed; what the
        // first then reads is another certificate, which the box never shows.
        Held held;
        auto was = std::make_unique<MadeUpKvm>(tls("first"));
        const QString id = held.put(*was, u"Build server"_s, "first");
        const core::OwnKvm before = held.kvm(id);
        const quint16 port = was->stand->serverPort();
        was.reset();
        auto second = std::make_unique<MadeUpKvm>(tls("second"), port);
        QVERIFY(second->stand->isListening());
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        box.openFor(id, false);
        box.review();
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("second")));
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        second.reset();
        const MadeUpKvm third(tls("comet"), port);
        QVERIFY(third.stand->isListening());
        emit box.reviewAsked(id);
        emit box.reviewAsked(id);
        QTRY_VERIFY_WITH_TIMEOUT(third.stand->asked.size() >= 3, 20000);
        QTest::qWait(400);
        QVERIFY(!box.busy());
        // The box shows the second still, or nothing: never the third.
        QVERIFY(box.presented() != core::kvm::fingerprint(sha256Of("comet")));
        // A yes to the third, sent by hand: it was never on the screen, and is none.
        emit box.trustAsked(id, sha256Of("comet"));
        QTest::qWait(600);
        QVERIFY(!box.busy());
        QCOMPARE(third.stand->asked.size(), 3);
        QCOMPARE(held.kvm(id), before);
        QVERIFY(changed.isEmpty());
        // Nor is one to the second, which the box may show still: that is offered no longer.
        emit box.trustAsked(id, sha256Of("second"));
        QTest::qWait(300);
        QCOMPARE(third.stand->asked.size(), 3);
        QCOMPARE(held.kvm(id), before);
    }

    void aLookAtACertificateAndTheYesToItAreHeldToTheKvmTheBoxWasFilledWith()
    {
        Held held;
        const MadeUpKvm reset(tls("second"));
        const MadeUpKvm otherReset(tls("twin-one"));
        const QString build = held.put(reset, u"Build server"_s, "first");
        const QString database = held.put(otherReset, u"Database"_s, "comet");
        const core::OwnKvm databaseBefore = held.kvm(database);
        ui::EditKvmView box;
        ui::EditKvmView elsewhere;
        held.own.serve(box);
        held.own.serve(elsewhere);
        lookAt(box);
        const QSignalSpy filledAnew(&box, &ui::EditKvmView::filledAnew);
        const QSignalSpy doneElsewhere(&elsewhere, &ui::EditKvmView::done);
        const QString changedMeanwhile = u"This device was changed meanwhile. Look at it again. The fields were "
                                         "filled in again with what is saved now."_s;
        box.openFor(build, false);

        // A look that names another KVM than the box was filled with is nobody's: that KVM is
        // asked nothing, and the box shows nothing of it.
        emit box.reviewAsked(database);
        QTest::qWait(400);
        QVERIFY(!box.busy());
        QVERIFY(!box.isReviewed());
        QVERIFY(otherReset.stand->asked.isEmpty());
        QCOMPARE(box.machine(), build);

        // Another window renames the KVM while this box is open on it as it was: its
        // certificate is not read from this box, which is filled anew with the KVM as it is.
        elsewhere.openFor(build, false);
        saveAs(elsewhere, u"Renamed"_s);
        QTRY_COMPARE(doneElsewhere.count(), 1);
        elsewhere.closed();
        box.review();
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(!box.isReviewed());
        QCOMPARE(box.problem(), changedMeanwhile);
        QCOMPARE(filledAnew.count(), 1);
        QCOMPARE(box.name(), u"Renamed"_s);
        QVERIFY(reset.stand->asked.isEmpty());
        // Over the KVM as it is, it is read.
        box.review();
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QCOMPARE(box.problem(), QString());
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        const qsizetype asked = reset.stand->asked.size();
        // A yes that names another KVM is none, whatever certificate comes with it.
        emit box.trustAsked(database, sha256Of("second"));
        emit box.trustAsked(database, sha256Of("twin-one"));
        QTest::qWait(400);
        QVERIFY(!box.busy());
        QVERIFY(otherReset.stand->asked.isEmpty());
        QCOMPARE(held.kvm(database), databaseBefore);
        // And the box's own yes, after another window changed the KVM once more: said over what
        // the box showed, which the KVM is not any more. Nothing is trusted, and nothing asked.
        elsewhere.openFor(build, false);
        saveAs(elsewhere, u"Renamed again"_s);
        QTRY_COMPARE(doneElsewhere.count(), 2);
        box.trust();
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), changedMeanwhile);
        QVERIFY(!box.isReviewed());
        QCOMPARE(filledAnew.count(), 2);
        QCOMPARE(box.name(), u"Renamed again"_s);
        QCOMPARE(reset.stand->asked.size(), asked);
        QCOMPARE(held.kvm(build).certificate, sha256Of("first"));
        // Looked at again and said yes to over the KVM as it is, it is trusted.
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        box.review();
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.trust();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
        QCOMPARE(held.kvm(build).certificate, sha256Of("second"));
    }

    void whereWindowsVouchesAtANewAddressTheBoxShowsItsWordAndAYesIsToThat()
    {
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        Held held;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm door(tls("signed"));
        const QString id = held.put(here, u"Build server"_s, "first");
        const core::OwnKvm before = held.kvm(id);
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        const QSignalSpy done(&box, &ui::EditKvmView::done);
        const QSignalSpy saveAsked(&box, &ui::EditKvmView::saveAsked);
        const QString there = u"127.0.0.1:%1"_s.arg(door.stand->serverPort());

        // Known by a certificate, and moved with its password typed to where Windows trusts
        // what is shown: the box is told that it is Windows' word a yes would be said to,
        // beside the certificate the KVM is known by; and what answers there was asked what it
        // is, and no more.
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QVERIFY(box.differs());
        QVERIFY(box.isOnWindowsWord());
        QVERIFY(!box.isPlainHttpThere());
        QCOMPARE(box.remembered(), core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("signed")));
        QCOMPARE(box.newAddress(), there);
        QCOMPARE(door.signIns, 0);
        QCOMPARE(held.kvm(id), before);
        // What the box shows is what it has to offer: Windows' word for that certificate.
        const ui::EditKvmView::Offer offered = box.offers().value_or(ui::EditKvmView::Offer{});
        QCOMPARE(offered.machine, id);
        QVERIFY(offered.onWindowsWord);
        QCOMPARE(offered.certificate, sha256Of("signed"));
        // The yes: signed in to once, on Windows' word, and the certificate it was known by is
        // given up. The box stays, and shows that it is known by none now.
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.saveKnownByWhatIsShown(box.name(), there, box.user(), u"secret"_s, box.group());
        QCOMPARE(saveAsked.constLast().at(6).toByteArray(), sha256Of("signed"));
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20000);
        QCOMPARE(box.problem(), QString());
        QCOMPARE(door.signIns, 1);
        QCOMPARE(held.kvm(id).address, door.stand->address());
        QVERIFY(!held.kvm(id).knownByCertificate);
        QVERIFY(held.kvm(id).certificate.isEmpty());
        QCOMPARE(held.secretOf(id), core::testing::savedFor("secret", door.stand->address()));
        QCOMPARE(done.count(), 0);
        QCOMPARE(box.savedAs(), u"\"Build server\""_s);
        QVERIFY(box.isHttps());
        QCOMPARE(box.certificate(), QString());
    }

    void aWishThatNamesAnotherKvmThanItsBoxWasFilledWithIsNobodys()
    {
        Held held;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm otherHere(tls("comet"));
        const MadeUpKvm moved(tls("second"));
        const QString build = held.put(here, u"Build server"_s, "first");
        const QString database = held.put(otherHere, u"Database"_s, "comet");
        const core::OwnKvm buildBefore = held.kvm(build);
        const core::OwnKvm databaseBefore = held.kvm(database);
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        const QSignalSpy changed(&held.own.wall(), &core::LocalWall::changed);
        const QString there = u"127.0.0.1:%1"_s.arg(moved.stand->serverPort());

        // (Named "a yes for one KVM is not taken for another" before: a wish that is sent by
        // hand carries no yes at all, so that was not what it showed. What it shows is this.)
        // The box is open for the one, shows what answers at an address that was typed for
        // it, and was looked at for its moment. A saving for the other, to that address and
        // with that certificate, is no yes for the other: it is typed over a KVM the box was
        // not filled with, and nothing more is asked at that address.
        box.openFor(build, false);
        box.save(box.name(), there, box.user(), u"secret"_s, box.group());
        QTRY_VERIFY_WITH_TIMEOUT(box.differs(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        emit box.saveAsked(database, u"Database"_s, there, u"admin"_s, u"secret"_s, QString(), sha256Of("second"),
                           false);
        QTest::qWait(400);
        QVERIFY(!box.busy());
        // (This had the box told "changed meanwhile" and filled anew: with the other KVM.)
        QCOMPARE(box.machine(), build);
        QVERIFY(box.differs());
        QCOMPARE(moved.stand->asked.size(), 3);
        QCOMPARE(moved.signIns, 0);
        QCOMPARE(held.kvm(database), databaseBefore);
        QCOMPARE(held.kvm(build), buildBefore);
        QVERIFY(changed.isEmpty());
        box.closed();

        // So with the yes to a certificate at the address a KVM has: for the other KVM it is
        // none, though the box shows that certificate for the one.
        const MadeUpKvm reset(tls("second"));
        const MadeUpKvm otherReset(tls("second"));
        const QString nas = held.put(reset, u"NAS"_s, "twin-one");
        const QString storage = held.put(otherReset, u"Storage"_s, "twin-two");
        const core::OwnKvm storageBefore = held.kvm(storage);
        box.openFor(nas, false);
        box.review();
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QCOMPARE(box.presented(), core::kvm::fingerprint(sha256Of("second")));
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        emit box.trustAsked(storage, sha256Of("second"));
        QTest::qWait(400);
        QVERIFY(!box.busy());
        QVERIFY(otherReset.stand->asked.isEmpty());
        QCOMPARE(held.kvm(storage), storageBefore);
        QCOMPARE(changed.count(), 2);
    }

    void whatIsSaidOfSettingsThatCouldNotBeRead()
    {
        const QTemporaryDir folder;
        const QString starts = u"What this program remembered could not be read, so it starts as new."_s;
        const auto damaged = [&folder](const QString& name) {
            const QString file = folder.filePath(name);
            QFile made(file);
            return made.open(QIODevice::WriteOnly) && made.write("{") == 1 ? file : QString();
        };
        // Read, or never there: nothing to say.
        core::Settings none(folder.filePath(u"none.json"_s));
        QCOMPARE(ui::OwnWall::saidOfSettingsThatCouldNotBeRead(none), QString());

        // Put aside, and said where.
        core::Settings unreadable(damaged(u"settings.json"_s));
        QCOMPARE(ui::OwnWall::saidOfSettingsThatCouldNotBeRead(unreadable),
                 starts + u" The file is kept, beside the new one, as settings.unreadable.json."_s);
        QVERIFY(QFile::exists(folder.filePath(u"settings.unreadable.json"_s)));
        // A second one goes beside the first, and that name is the one said.
        core::Settings again(damaged(u"settings.json"_s));
        QCOMPARE(ui::OwnWall::saidOfSettingsThatCouldNotBeRead(again),
                 starts + u" The file is kept, beside the new one, as settings.unreadable-2.json."_s);

        // Held by something else: said as that, and nothing is claimed of where it went.
        const QString held = damaged(u"held.json"_s);
        core::Settings stuck(held);
        {
            QFile holder(held);
            QVERIFY(holder.open(QIODevice::ReadOnly));
            QCOMPARE(ui::OwnWall::saidOfSettingsThatCouldNotBeRead(stuck),
                     starts + u" The file could not be put aside either, so nothing is remembered from here on: "
                              "something else holds it."_s);
        }
        // Taken away by somebody meanwhile: only that the program starts as new.
        const QString gone = damaged(u"gone.json"_s);
        core::Settings vanished(gone);
        QVERIFY(QFile::remove(gone));
        QCOMPARE(ui::OwnWall::saidOfSettingsThatCouldNotBeRead(vanished), starts);
    }

    void whatIsSaidOfAKvmThatWasAdded()
    {
        // (What it is called is handed in as a sentence calls it, and stands there as it is.)
        core::KvmAdding::Added added;
        added.name = u"Build server"_s;
        const QString called = u"\"Build server\""_s;
        QCOMPARE(ui::OwnWall::saidOf(added, called), u"\"Build server\" added"_s);
        added.model = u"RM10"_s;
        QCOMPARE(ui::OwnWall::saidOf(added, called), u"\"Build server\" added (RM10)"_s);
        added.model.clear();
        added.overPlainHttp = true;
        QCOMPARE(ui::OwnWall::saidOf(added, called), u"\"Build server\" added (over plain HTTP)"_s);
    }

    // --- what a KVM is called in a sentence of the screens ---------------------------------------

    void aKvmsNameStandsInASentenceOfTheScreensSetOffAndNeverAsItIs()
    {
        // A name is free text, typed or from a file. In a sentence that the screens finish it
        // stands as the wall's own sentences have it (core::LocalWall::calledInASentence): in
        // quotes that it cannot close, so that it does not read as the program's own words.
        // The three such sentences a reviewer found ("... saved", "... added", "shows the
        // certificate of ..."), and the others of the two boxes: "...: certificate trusted", and
        // the box's own titles.
        const QString poses = u"this KVM. It is safe to trust it"_s;
        const QString setOff = u"\"this KVM. It is safe to trust it\""_s;
        Held held;
        const MadeUpKvm kvm(tls("first"));
        const MadeUpKvm other(tls("first"));
        const QString id = held.put(kvm, u"NAS"_s, "first");
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        const QSignalSpy done(&box, &ui::EditKvmView::done);

        // Saved under that name: what the box is told it is called now.
        box.openFor(id, false);
        QCOMPARE(box.called(), u"NAS"_s);
        QCOMPARE(box.calledInASentence(), u"\"NAS\""_s);
        saveAs(box, poses);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), u"saved"_s);
        QCOMPARE(done.at(0).at(1).toString(), setOff);
        QCOMPARE(held.kvm(id).name, poses);
        box.closed();
        // What its box calls it from then on, in its titles: set off; and in the line its name
        // is typed into, as it is.
        box.openFor(id, false);
        QCOMPARE(box.name(), poses);
        QCOMPARE(box.called(), poses);
        QCOMPARE(box.calledInASentence(), setOff);
        // A name with a quote in it cannot close its own setting-off.
        saveAs(box, u"x\" saved. Trust \"y"_s);
        QTRY_COMPARE(done.count(), 2);
        QCOMPARE(done.at(1).at(1).toString(), u"\"x' saved. Trust 'y\""_s);
        box.closed();
        box.openFor(id, false);
        saveAs(box, poses);
        QTRY_COMPARE(done.count(), 3);
        box.closed();

        // Its certificate trusted: named so in what the window is told.
        const MadeUpKvm reset(tls("second"));
        const QString build = held.put(reset, poses + u" too"_s, "first");
        held.own.openFor(box, build, true);
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(box.mayTrust(), 5000);
        box.trust();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 4, 20000);
        QCOMPARE(done.at(3).at(0).toString(), u"trusted"_s);
        QCOMPARE(done.at(3).at(1).toString(), u"\"this KVM. It is safe to trust it too\""_s);
        box.closed();

        // What shows the certificate of a KVM of the wall: whose it is, in the sentence that
        // says so. (The first KVM is known by "first"; another that shows that one.)
        const QString twin = held.put(other, u"Database"_s, "comet");
        held.own.openFor(box, twin, true);
        QTRY_VERIFY_WITH_TIMEOUT(box.isReviewed(), 20000);
        QCOMPARE(box.presentedIsCalled(), setOff);
        box.closed();

        // And one that was added: named so in what its box says. It is called what it calls
        // itself where nothing was typed, which is the KVM's own word, and anybody's.
        ui::AddKvmView adding;
        held.own.serve(adding);
        MadeUpKvm added;
        added.instead.insert("/api/system/get_hostname"_ba,
                             core::testing::good({{u"hostname"_s, u"Trusted"_s}, {u"success"_s, true}}));
        QCOMPARE(held.own.calledInASentence(id, u"anything"_s), setOff);
        // (One that has no name is called by where it is, as the wall's own sentences call it.)
        core::OwnKvm unnamed;
        unnamed.address = u"https://127.0.0.1:20443"_s;
        const QString unnamedId = held.own.wall().add(unnamed, u"secret"_s).value();
        QCOMPARE(held.own.calledInASentence(unnamedId, u"anything"_s), u"127.0.0.1:20443"_s);
        // (And one that is not on the wall any more: what its card called it, set off the same
        // way; or "this KVM" where that shows nothing.)
        QCOMPARE(held.own.calledInASentence(u"no such one"_s, poses), setOff);
        QCOMPARE(held.own.calledInASentence(u"no such one"_s, QString()), u"this KVM"_s);
        adding.add(added.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!adding.busy(), 20000);
        QCOMPARE(adding.problem(), QString());
        QCOMPARE(held.own.wall().kvms().constLast().name, u"Trusted"_s);
        QVERIFY2(adding.added().startsWith(u"\"Trusted\" added "_s), qPrintable(adding.added()));
    }

    // --- this PC's wall as a file: each window's boxes have a laying-out of their own ------------

    void theBoxesOfEachWindowForTheWallAsAFileHaveALayingOutOfTheirOwn()
    {
        // Two windows, each with its boxes for this PC's wall as a file, served by the one wall:
        // what the one laid out of a file is not the other's to take, and is not forgotten when
        // the other reads a file, takes from one, or goes. (KVMs at this PC's own address, where
        // nobody is: they are laid out and taken, and nothing is asked of anything.)
        Held held;
        // (At a cost a test can pay; the files of this test are plain, and are not sealed at all.)
        held.own.sealFilesWith(
            core::exchange::Sealing{core::scrypt::Cost{4096, 8, 1}, core::exchange::windowsDerive()});
        const auto fileOf = [&](const QString& name, int port) {
            core::exchange::Content content;
            core::exchange::Device device;
            device.address = u"https://127.0.0.1:%1"_s.arg(port);
            device.name = name;
            device.user = u"admin"_s;
            device.kind = u"kvmd"_s;
            device.tls = u"system"_s;
            content.devices = QList<core::exchange::Device>{device};
            const base::Result<core::exchange::Written> written = core::exchange::write(content, QString());
            const QString path = held.folder.filePath(name + u".json"_s);
            QFile file(path);
            if (written && file.open(QIODevice::WriteOnly)) {
                file.write(written.value().file(core::exchange::NotInIt::wasLookedAt));
            }
            return path;
        };
        const QString one = fileOf(u"One"_s, 20001);
        const QString two = fileOf(u"Two"_s, 20002);
        auto first = std::make_unique<ui::OwnFilesView>();
        ui::OwnFilesView second;
        held.own.serve(*first);
        held.own.serve(second);
        first->setDoor({{}, [one] { return base::Result<QString>(one); }});
        second.setDoor({{}, [two] { return base::Result<QString>(two); }});
        first->setLookedAt(true);
        second.setLookedAt(true);

        first->chooseFile();
        first->readFile(QString());
        QTRY_VERIFY(first->isLaidOut());
        second.chooseFile();
        second.readFile(QString());
        QTRY_VERIFY(second.isLaidOut());
        // The second's reading took nothing from the first. (Each holds one device, at this PC's
        // own address: such a one is ticked by whoever wants it, and not to begin with.)
        QVERIFY(first->isLaidOut());
        QCOMPARE(first->tickedCount(), 0);
        // (A tick is taken once each list has stood for its moment.)
        QTRY_VERIFY_WITH_TIMEOUT(first->isSettled() && second.isSettled(), 5000);
        first->setTicked(1, true);
        second.setTicked(1, true);
        QCOMPARE(first->tickedCount(), 1);
        QCOMPARE(second.tickedCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(first->mayTake() && second.mayTake(), 5000);
        const QSignalSpy takenByTheSecond(&second, &ui::OwnFilesView::taken);
        second.take();
        QCOMPARE(takenByTheSecond.count(), 1);
        QCOMPARE(held.own.wall().kvms().size(), 1);
        QCOMPARE(held.own.wall().kvms().constFirst().name, u"Two"_s);
        // Nor did its taking: the first takes its own, and that one alone.
        QVERIFY(first->isLaidOut());
        const QSignalSpy takenByTheFirst(first.get(), &ui::OwnFilesView::taken);
        first->take();
        QCOMPARE(takenByTheFirst.count(), 1);
        QCOMPARE(takenByTheFirst.at(0).at(0).toString(), u"1 KVM was taken onto this wall."_s);
        QCOMPARE(held.own.wall().kvms().size(), 2);
        QCOMPARE(held.own.wall().kvms().constLast().name, u"One"_s);
        // A window that goes takes its own with it, and the other's stays as it is.
        second.chooseFile();
        second.readFile(QString());
        QTRY_VERIFY(second.isLaidOut());
        first.reset();
        QVERIFY(second.isLaidOut());
        // (On the wall already, by now: laid out as that, and not to be ticked, even once its list
        // has stood for its moment.)
        QTRY_VERIFY_WITH_TIMEOUT(second.isSettled(), 5000);
        second.setTicked(1, true);
        QCOMPARE(second.tickedCount(), 0);
    }

    // What the wall makes of a password to keep it (the lines it is saved for, and its bytes behind
    // them) is written over before it is given back, on every road: where it is kept, where the store
    // does not take it, and where the list cannot be written and what the store took is taken back.
    // For a KVM that is added, one whose password is changed, and one taken from a file.
    void whatAPasswordIsKeptAsIsWrittenOverBeforeItIsGivenBackOnEveryRoad()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        // The wall over a store that keeps a copy of its own of what it is handed, as Windows' does:
        // what the wall made is the wall's alone to write over, on every road. (A store that shared
        // it would hide a road where the wall did not: it would hold the bytes until the test ends.)
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        SecretsOfTheirOwn secrets;
        core::LocalWall wall(settings, secrets, quick().driver);
        const auto kvmOf = [&wall](const QString& id) {
            const QList<core::OwnKvm>& all = wall.kvms();
            const auto known = std::ranges::find_if(all, [&id](const core::OwnKvm& one) { return one.id == id; });
            return known == all.end() ? core::OwnKvm{} : *known;
        };
        // At ports of this PC's own address where nothing answers; nothing of them is asked here.
        const auto kvmAt = [](int port) {
            core::OwnKvm kvm;
            kvm.address = u"http://127.0.0.1:%1"_s.arg(port);
            kvm.name = u"Rack %1"_s.arg(port);
            return kvm;
        };
        // Each road with a password of its own, which nothing else has made: its bytes, as the store
        // keeps them, looked for in every block that is given back while the wall keeps it.
        QStringList wrong;
        const auto lookAt = [&wrong](const QString& road, const QString& password, const std::function<void()>& keep) {
            const QByteArray bytes = password.toUtf8();
            int found = 0;
            {
                const WhatIsGivenBack watching(bytes);
                keep();
                found = WhatIsGivenBack::holdingIt();
            }
            if (found > 0) {
                wrong.append(u"%1: %2 blocks held the password as they were given back"_s.arg(road).arg(found));
            }
        };
        const auto notSaved = [](const auto& result) { return !result.ok() && result.error().code == "not_saved"; };

        // Added.
        QString id;
        lookAt(u"added"_s, u"added one 7Qx 4b"_s, [&] {
            const base::Result<QString> added = wall.add(kvmAt(22501), u"added one 7Qx 4b"_s);
            QVERIFY(added.ok());
            id = added.value();
        });
        QVERIFY(!id.isEmpty());
        lookAt(u"added, and the store does not take it"_s, u"added two 8Ry 5c"_s, [&] {
            secrets.refuses = true;
            QVERIFY(notSaved(wall.add(kvmAt(22502), u"added two 8Ry 5c"_s)));
            secrets.refuses = false;
        });
        lookAt(u"added, and the list cannot be written"_s, u"added three 9Sz 6d"_s, [&] {
            // (Something else has the file open: Windows puts no other in its place meanwhile.)
            QFile inTheWay(file);
            QVERIFY(inTheWay.open(QIODevice::ReadOnly));
            QVERIFY(notSaved(wall.add(kvmAt(22503), u"added three 9Sz 6d"_s)));
        });

        // Its password changed.
        lookAt(u"changed"_s, u"changed one 3Kp 7e"_s,
               [&] { QVERIFY(wall.change(id, kvmOf(id), u"changed one 3Kp 7e"_s).ok()); });
        lookAt(u"changed, and the store does not take it"_s, u"changed two 4Lq 8f"_s, [&] {
            secrets.refuses = true;
            QVERIFY(notSaved(wall.change(id, kvmOf(id), u"changed two 4Lq 8f"_s)));
            secrets.refuses = false;
        });
        lookAt(u"changed, and the list cannot be written"_s, u"changed three 5Mr 9g"_s, [&] {
            QFile inTheWay(file);
            QVERIFY(inTheWay.open(QIODevice::ReadOnly));
            QVERIFY(notSaved(wall.change(id, kvmOf(id), u"changed three 5Mr 9g"_s)));
        });

        // Taken from a file.
        const auto fromAFile = [&](int port, const QString& password) {
            core::LocalWall::FromAFile one;
            one.kvm = kvmAt(port);
            one.password = password;
            const QList<base::Result<QString>> came = wall.takeFromAFile({one});
            return came.size() == 1 ? came.constFirst() : base::Result<QString>(base::Error{"none", "none"});
        };
        lookAt(u"taken from a file"_s, u"from a file one 6Nt 2h"_s,
               [&] { QVERIFY(fromAFile(22504, u"from a file one 6Nt 2h"_s).ok()); });
        lookAt(u"taken from a file, and the store does not take it"_s, u"from a file two 7Pu 3j"_s, [&] {
            secrets.refuses = true;
            QVERIFY(notSaved(fromAFile(22505, u"from a file two 7Pu 3j"_s)));
            secrets.refuses = false;
        });
        lookAt(u"taken from a file, and the list cannot be written"_s, u"from a file three 8Qv 4k"_s, [&] {
            QFile inTheWay(file);
            QVERIFY(inTheWay.open(QIODevice::ReadOnly));
            QVERIFY(notSaved(fromAFile(22506, u"from a file three 8Qv 4k"_s)));
        });
        QCOMPARE(wall.kvms().size(), 2);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
#endif
    }

    // A box whose KVM was taken off the wall elsewhere asks nothing more of it, whatever the screens
    // do: no saving, no taking off, no look at its certificate and no yes to one. It says that the
    // KVM went (`gone`), and is busy with nothing.
    void aBoxWhoseKvmWentAsksNothingMore()
    {
        Held held;
        const MadeUpKvm kvm;
        const QString id = held.put(kvm, u"NAS"_s, "first");
        ui::EditKvmView box;
        held.own.serve(box);
        lookAt(box);
        box.openFor(id, false);
        QCOMPARE(box.machine(), id);
        // (Two certificates on show, looked at for their moment: a yes would be taken.)
        box.began(u"looking"_s);
        box.reviewed(sha256Of("first"), sha256Of("second"));
        QTRY_VERIFY(box.mayTrust());
        const QSignalSpy saveAsked(&box, &ui::EditKvmView::saveAsked);
        const QSignalSpy removeAsked(&box, &ui::EditKvmView::removeAsked);
        const QSignalSpy reviewAsked(&box, &ui::EditKvmView::reviewAsked);
        const QSignalSpy trustAsked(&box, &ui::EditKvmView::trustAsked);
        QVERIFY(!box.isGone());
        QVERIFY(held.own.wall().remove(id).ok());
        QVERIFY(box.isGone());
        box.trust();
        saveAs(box, u"Storage"_s, u"another"_s);
        box.remove();
        box.review();
        QTest::qWait(100);
        QCOMPARE(trustAsked.count(), 0);
        QCOMPARE(saveAsked.count(), 0);
        QCOMPARE(removeAsked.count(), 0);
        QCOMPARE(reviewAsked.count(), 0);
        QVERIFY(!box.busy());
        QVERIFY(box.problem().isEmpty());
        QVERIFY(held.own.wall().kvms().isEmpty());
        // What it was asking when its KVM went ends with nothing more said of it than that it went.
        box.began(u"saving"_s);
        box.refused(u"Unknown device"_s);
        QVERIFY(!box.busy());
        QVERIFY(box.isGone());
        QVERIFY(box.problem().isEmpty());
        // Opened anew for another KVM, it asks again.
        const MadeUpKvm other;
        const QString otherId = held.put(other, u"Build server"_s);
        box.closed();
        box.openFor(otherId, false);
        QVERIFY(!box.isGone());
        box.remove();
        QCOMPARE(removeAsked.count(), 1);
    }
};

QTEST_GUILESS_MAIN(TestOwnWall)
#include "test_own_wall.moc"
