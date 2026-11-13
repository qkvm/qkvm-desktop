#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/wall_exchange.h>

#include "exchange_by_hand.h"
#include "jetkvm_by_hand.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"
#include "wall_of_a_test.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::LocalWall;
using core::OwnKvm;
using core::WallExchange;
using core::exchange::Content;
using core::exchange::Device;
using core::exchange::Written;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::exampleFile;
using core::testing::isOfTheServersExampleFiles;
using core::testing::isThisPcsOwn;
using core::testing::JetKvmByHand;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;
using core::testing::plainFile;
using core::testing::quickCost;
using core::testing::reachesNothingByTheBooks;
using core::testing::saidOf;
using core::testing::savedFor;
using core::testing::sign;
using core::testing::StandIn;
using core::testing::textOf;
using core::testing::thePassphrase;
using core::testing::WallOfATest;

namespace {

// Where a kvmd is signed in to, and the three questions that are asked before that.
constexpr const char* signInQuestion = "/api/auth/login";
constexpr const char* kvmdsQuestion = "/api/auth/check";
constexpr const char* jetKvmsQuestion = "/device/status";
constexpr const char* redfishsQuestion = "/redfish/v1/";

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

// The times a real KVM is given, made short.
core::KvmdDevice::Pace quickDriver()
{
    core::KvmdDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.freshMs = 200;
    pace.firstContactMs = 400;
    pace.patienceMs = 400;
    return pace;
}

core::KvmAdding::Pace quickBox()
{
    core::KvmAdding::Pace pace;
    pace.probe.connectWithinMs = 1500;
    pace.probe.quietMs = 1500;
    pace.probe.withinMs = 3000;
    pace.driver = quickDriver();
    return pace;
}

// Sealing at a cost that a test can pay a hundred times.
core::exchange::Sealing quickly()
{
    return core::exchange::Sealing{quickCost, core::exchange::windowsDerive()};
}

// Secrets kept in memory, and how often the store was asked for one: what is read from Windows
// is part of what these tests are about.
class WatchedSecrets final : public core::SecretStore {
public:
    base::Result<void> keep(const QString& name, const QByteArray& secret) override { return inner.keep(name, secret); }
    QByteArray kept(const QString& name) override
    {
        ++reads;
        return handsNothingOut ? QByteArray() : inner.kept(name);
    }
    void forget(const QString& name) override { inner.forget(name); }

    MemorySecrets inner;
    int reads = 0;
    // What is kept cannot be read back: Windows hands nothing out.
    bool handsNothingOut = false;
};

// (What holds these tests to this PC's own address is the wall each of them has: WallOfATest,
// in wall_of_a_test.h, which the tests of the boxes on the screen have too.)

// This PC's own wall for one test: its settings in a folder that goes with the test, its secrets
// in memory. It may hold what the server's own example files hold, which some of these tests take:
// such a wall is asked nothing, as any that holds an address that is not this PC's own.
struct OwnWall {
    explicit OwnWall(core::KvmdDevice::Pace pace = quickDriver())
        : file(folder.filePath(u"settings.json"_s)), settings(file),
          wall(settings, secrets, std::move(pace), WallOfATest::AlsoHolds::TheServersExampleFiles)
    {}
    // Over a settings' file that holds this before the wall reads it.
    explicit OwnWall(const QByteArray& written, core::KvmdDevice::Pace pace = quickDriver())
        : file(writtenIn(folder, written)), settings(file),
          wall(settings, secrets, std::move(pace), WallOfATest::AlsoHolds::TheServersExampleFiles)
    {}
    static QString writtenIn(const QTemporaryDir& folder, const QByteArray& written)
    {
        const QString file = folder.filePath(u"settings.json"_s);
        QFile out(file);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(written);
        }
        return file;
    }

    QTemporaryDir folder;
    QString file;
    core::Settings settings;
    WatchedSecrets secrets;
    WallOfATest wall;
};

// The certificate of the server's example files of a wall, as its bytes: one that Windows reads
// as a certificate, and nobody's.
QByteArray exampleCertificate()
{
    const QJsonObject file = QJsonDocument::fromJson(exampleFile("plain.json")).object();
    const QString pem = file[u"devices"_s].toArray().first().toObject()[u"cert_pem"_s].toString();
    return QByteArray::fromBase64(
        QString(pem).remove(u"-----BEGIN CERTIFICATE-----"_s).remove(u"-----END CERTIFICATE-----"_s).toLatin1());
}

// What answers at an address and is no kvmd. A page, whatever is asked of it:
Answered asAPage(const Asked& /*asked*/)
{
    return {200, "<!doctype html><html><body><h1>Sign in</h1></body></html>"_ba, "text/html"_ba};
}
// A JetKVM, which says so itself and answers everything else with its page:
Answered asAJetKvm(const Asked& asked)
{
    if (asked.path == jetKvmsQuestion) {
        return {200, core::testing::written({{u"isSetup"_s, true}})};
    }
    return asAPage(asked);
}
// And a server's management controller, which shows the root of its Redfish service to anyone.
Answered asAController(const Asked& asked)
{
    if (asked.path == redfishsQuestion) {
        return {200, core::testing::written({{u"@odata.id"_s, u"/redfish/v1/"_s}, {u"RedfishVersion"_s, u"1.8.0"_s}})};
    }
    return {401, core::testing::written({{u"error"_s, u"unauthorized"_s}})};
}

core::testing::Tls tls(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

QByteArray sha256Of(const char* name)
{
    return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
}

// A test certificate's text, as its file has it.
QString pemOf(const char* name)
{
    QFile file(tls(name).certificateFile);
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.readAll()) : QString();
}

// The same as a file of the wall holds it, and as this PC's wall keeps it.
QString keptPemOf(const char* name)
{
    const std::optional<core::exchange::KeptCertificate> kept = core::exchange::certificateFrom(pemOf(name));
    return kept ? kept->pem : QString();
}

// A KVM as a file names one: of the family this program speaks to, with the password the
// made-up KVMs have.
Device kvmAt(const QString& address, const QString& name = {}, const QString& password = u"secret"_s)
{
    Device device;
    device.address = address;
    device.name = name.isEmpty() ? address.mid(address.indexOf(u"://"_s) + 3) : name;
    device.user = u"admin"_s;
    device.password = password;
    device.kind = u"kvmd"_s;
    device.tls = u"pinned"_s;
    return device;
}

// One that cannot be taken onto this PC's wall for what the file has for it: a user name of forty signs from
// far up, which a file holds and which takes eighty places in a text here, more than this wall keeps.
Device notKeptHere(Device device)
{
    device.user = core::testing::wideSigns(40);
    return device;
}

// The same, with its certificate named in the file.
Device knownBy(Device device, const char* certificate)
{
    device.certificatePem = keptPemOf(certificate);
    device.certificate = sha256Of(certificate);
    return device;
}

Device saying(Device device, const QString& tlsWord)
{
    device.tls = tlsWord;
    return device;
}

// A file that holds these devices: sealed with the tests' passphrase, with their passwords; or
// plain, and then without.
QByteArray fileOf(const QList<Device>& devices, bool sealed = true)
{
    Content content;
    content.devices = devices;
    const base::Result<Written> written =
        core::exchange::writeWith(content, core::SecretBytes::utf8Of(sealed ? thePassphrase() : QString()), quickly());
    return written ? textOf(written.value()) : QByteArray();
}

// Whether all that was asked at an address is what is asked before a password goes: the three
// questions, each as often as the others, with nothing in them that says who asks.
bool heardOnlyTheQuestions(const StandIn& stand, int times = 1)
{
    const bool nothingSaid = std::ranges::all_of(stand.asked, [](const Asked& one) {
        return one.method == "GET" && one.body.isEmpty() && !one.headers.contains("cookie") &&
               !one.headers.contains("authorization") && !one.path.contains("secret");
    });
    return nothingSaid && stand.asked.size() == qsizetype{3} * times && stand.count(kvmdsQuestion) == times &&
           stand.count(jetKvmsQuestion) == times && stand.count(redfishsQuestion) == times;
}

// Whether a type hands out its list of devices to whoever asks for it and says nothing.
template <typename Laid>
concept HandsOutItsListUnasked = requires(const Laid& laid) { laid.devices(); };
template <typename Laid>
concept HasItsListAsAMember = requires { requires std::is_member_object_pointer_v<decltype(&Laid::devices)>; };
template <typename Laid>
concept HandsOutItsListForEmptyBraces = requires(const Laid& laid) { laid.devices({}); };
template <typename Laid>
concept HandsOutItsListForTheWord =
    requires(const Laid& laid) { laid.devices(WallExchange::BesideTheList::wasLookedAt); };
template <typename Word>
concept IsSaidByEmptyBraces = requires { Word{}; };

} // namespace

// This PC's wall and the wall as a file: what a file holds is laid out and taken, a KVM from a
// file is asked what it is before its password goes anywhere, and the wall is written to a file.
// Against made-up KVMs and against things that answer at an address and are none, all of them
// at this PC's own address. (The devices of the server's example files are at addresses that are
// none of this PC's: they are laid out and taken, and no wall that holds one is ever asked how
// its KVMs are, which is what would have the wall ask at an address. That is held by the wall
// these tests have, WallOfATest above, and not by their care.)
class TestWallExchange : public QObject {
    Q_OBJECT

private:
    template <typename Answer>
    static base::Result<Answer> answerTo(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
    {
        QList<base::Result<Answer>> answers;
        ask([&](base::Result<Answer> answer) { answers.append(std::move(answer)); });
        if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 30000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return answers.takeFirst();
    }
    // The same of what is no Result: what was handed over; or, where nothing ever was, what stands
    // for that (`never`), which says so and fails whatever looks at it.
    template <typename Outcome>
    static Outcome outcomeOf(const std::function<void(std::function<void(Outcome)>)>& ask, const Outcome& never)
    {
        QList<Outcome> outcomes;
        ask([&](Outcome outcome) { outcomes.append(std::move(outcome)); });
        static_cast<void>(QTest::qWaitFor([&] { return !outcomes.isEmpty(); }, 30000));
        return outcomes.isEmpty() ? never : outcomes.constFirst();
    }
    // What stands for a change, and for a yes to a certificate, that never came to anything.
    static core::KvmEditing::Outcome neverChanged()
    {
        return {base::Error{"never", "The change never ended."}, {}, {}, {}, false};
    }
    static core::KvmCertificateReview::Outcome neverTrusted()
    {
        return {base::Error{"never", "The yes never came to anything."}, {}};
    }
    static QHash<QString, core::Status> statusOf(LocalWall& wall)
    {
        return answerTo<QHash<QString, core::Status>>([&](auto done) { wall.askStatus(std::move(done)); }).value();
    }
    // How a KVM is once the wall has made up its mind: asked until it is no longer "connecting".
    static core::Status settled(LocalWall& wall, const QString& id)
    {
        core::Status status;
        static_cast<void>(QTest::qWaitFor(
            [&] {
                status = statusOf(wall).value(id);
                return status.state != u"connecting"_s;
            },
            30000));
        return status;
    }

    static base::Result<WallExchange::LaidOut> laidOutBy(WallExchange& exchange, const QByteArray& file,
                                                         const QString& passphrase = thePassphrase())
    {
        QObject asker;
        QList<base::Result<WallExchange::LaidOut>> told;
        exchange.layOut(file, passphrase, &asker,
                        [&told](const base::Result<WallExchange::LaidOut>& result) { told.append(result); });
        if (!told.isEmpty()) {
            return base::Error{"at_once", "The result was handed over before layOut() had come back."};
        }
        if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 30000)) {
            return base::Error{"never", "The laying out never ended."};
        }
        return told.constFirst();
    }
    static QList<WallExchange::Offered> rowsOf(const base::Result<WallExchange::LaidOut>& laidOut)
    {
        return laidOut ? laidOut.value().devices(WallExchange::BesideTheList::wasLookedAt)
                       : QList<WallExchange::Offered>();
    }
    static QList<qsizetype> placesOf(const QList<WallExchange::Offered>& rows)
    {
        QList<qsizetype> places;
        for (const WallExchange::Offered& row : rows) {
            places.append(row.place);
        }
        return places;
    }
    // Lays a file of these devices out and takes all of it: what came of each device.
    static QList<WallExchange::Took> takenFrom(WallExchange& exchange, const QList<Device>& devices, bool sealed = true)
    {
        const base::Result<WallExchange::LaidOut> laidOut =
            laidOutBy(exchange, fileOf(devices, sealed), sealed ? thePassphrase() : QString());
        if (!laidOut) {
            return {};
        }
        return exchange.take(laidOut.value().number(), placesOf(rowsOf(laidOut)));
    }
    // The wall's name for the one KVM that was taken; empty where it was not.
    static QString theOne(const QList<WallExchange::Took>& took)
    {
        return took.size() == 1 && took.constFirst().came.ok() ? took.constFirst().came.value() : QString();
    }
    static QString codeOf(const WallExchange::Took& took)
    {
        return took.came.ok() ? u"taken"_s : QString::fromStdString(took.came.error().code);
    }
    static QString sentenceOf(const WallExchange::Took& took)
    {
        return took.came.ok() ? QString() : QString::fromStdString(took.came.error().message);
    }

    static base::Result<Written> writtenBy(WallExchange& exchange, const QString& passphrase)
    {
        QObject asker;
        QList<base::Result<Written>> told;
        exchange.write(passphrase, &asker, [&told](const base::Result<Written>& result) { told.append(result); });
        if (!told.isEmpty()) {
            return base::Error{"at_once", "The result was handed over before write() had come back."};
        }
        if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 30000)) {
            return base::Error{"never", "The writing never ended."};
        }
        return told.constFirst();
    }

    static OwnKvm kvmOf(const LocalWall& wall, const QString& id)
    {
        const auto known = std::ranges::find_if(wall.kvms(), [&id](const OwnKvm& one) { return one.id == id; });
        return known == wall.kvms().end() ? OwnKvm{} : *known;
    }
    // One that is reached over HTTPS, and known by a certificate: as somebody added it by hand.
    static OwnKvm knownAt(const QString& address, const QByteArray& certificate, const QString& name = {})
    {
        OwnKvm own;
        own.address = address;
        own.name = name;
        own.knownByCertificate = true;
        own.certificate = certificate;
        return own;
    }
    static QByteArray settingsText(const OwnWall& own)
    {
        QFile file(own.file);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    // Every text of what was laid out, in one: for a test that looks for what must not be there.
    static QString everyWordOf(const WallExchange::LaidOut& laidOut)
    {
        QStringList all = laidOut.besideTheList();
        for (const WallExchange::Offered& row : laidOut.devices(WallExchange::BesideTheList::wasLookedAt)) {
            all << row.nameAndAddress << row.address << row.fileSaysOfTls << row.fingerprint << row.whyNot
                << row.verdict << row.says;
        }
        return all.join(u'\n');
    }

private slots:
    void savedActionsAndSharedSettingsArePortable()
    {
        OwnWall own;
        QVERIFY(own.wall.actions().add({u"Example text"_s, true, {}, u"private test text"_s, true, true}));
        QVERIFY(own.wall.actions().add({u"Shortcut"_s, false, u"F5"_s, {}, false, false}));
        WallExchange exchange(own.wall, quickly());
        const auto plain = writtenBy(exchange, {});
        QVERIFY(plain);
        const auto content = core::exchange::read(textOf(plain.value()), {});
        QVERIFY(content);
        QVERIFY(content.value().actions.has_value());
        const auto plainActions = content.value().actions.value_or(QList<core::exchange::Action>());
        QCOMPARE(plainActions.size(), 1);
        QVERIFY(!plainActions.first().types);
        QCOMPARE(plainActions.first().keys, u"F5"_s);
        QVERIFY(content.value().wall.has_value());
        QVERIFY(!textOf(plain.value()).contains("private test text"));
        const auto sealed = writtenBy(exchange, thePassphrase());
        QVERIFY(sealed);
        const auto opened = core::exchange::read(textOf(sealed.value()), thePassphrase());
        QVERIFY(opened);
        const auto sealedActions = opened.value().actions.value_or(QList<core::exchange::Action>());
        QCOMPARE(sealedActions.size(), 2);
        QCOMPARE(sealedActions.first().text, u"private test text"_s);
        QVERIFY(!textOf(sealed.value()).contains("private test text"));
        QCOMPARE(sealedActions.first().enter, true);
        QCOMPARE(sealedActions.first().danger, true);
    }

    void actionImportPreviewsMergesAndKeepsSettingsOptIn()
    {
        OwnWall own;
        QVERIFY(own.wall.actions().add({u"Existing"_s, false, u"F5"_s, {}, false, false}));
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = plainFile(
            {{u"actions"_s,
              QJsonArray{QJsonObject{{u"name"_s, u" existing "_s}, {u"kind"_s, u"keys"_s}, {u"keys"_s, u"F6"_s}},
                         QJsonObject{{u"name"_s, u"Text action"_s},
                                     {u"kind"_s, u"text"_s},
                                     {u"text"_s, u"fixture text"_s},
                                     {u"enter"_s, true}},
                         QJsonObject{{u"name"_s, u"Shortcut"_s}, {u"kind"_s, u"keys"_s}, {u"keys"_s, u"F7"_s}}}},
             {u"wall"_s, QJsonObject{{u"interval"_s, 2.75}, {u"update_check"_s, false}}}});
        const auto laid = laidOutBy(exchange, file, {});
        QVERIFY(laid);
        QCOMPARE(own.wall.actions().all().size(), 1);
        QCOMPARE(own.settings.remembered().wallPictureEveryMs, 0);
        const auto rows = laid.value().otherParts();
        QCOMPARE(rows.size(), 4);
        QVERIFY(!rows[0].canBeTaken);
        QVERIFY(rows[1].canBeTaken);
        QVERIFY(!rows[3].initiallyTicked);
        const auto took = exchange.take(laid.value().number(), {rows[1].place, rows[2].place});
        QCOMPARE(took.size(), 2);
        QVERIFY(took[0].came && took[1].came);
        QCOMPARE(own.wall.actions().all().size(), 3);
        QVERIFY(!own.wall.actions().all()[1].writing.isEmpty());
        QCOMPARE(own.settings.remembered().wallPictureEveryMs, 0);
        QVERIFY(own.settings.remembered().checkUpdates);
        const auto again = laidOutBy(exchange, file, {});
        QVERIFY(again);
        for (int i = 0; i < 3; ++i) {
            QVERIFY(!again.value().otherParts()[i].canBeTaken);
        }
        const auto settings = exchange.take(again.value().number(), {WallExchange::settingsPlace});
        QVERIFY(settings.first().came);
        QCOMPARE(own.settings.remembered().wallPictureEveryMs, 2750);
        QVERIFY(!own.settings.remembered().checkUpdates);
        QCOMPARE(core::Settings(own.file).remembered().wallPictureEveryMs, 2750);
    }

    void importedSecretsAreHiddenUntilAskedAndForgottenWithTheirPreview()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QByteArray file =
            plainFile({{u"actions"_s, QJsonArray{QJsonObject{{u"name"_s, u"Example"_s},
                                                             {u"kind"_s, u"text"_s},
                                                             {u"text"_s, u"private fixture text"_s}}}}});
        const auto laid = laidOutBy(exchange, file, {});
        QVERIFY(laid);
        const auto row = laid.value().otherParts().first();
        QVERIFY(!row.verdict.contains(u"private fixture text"_s));
        QVERIFY(!row.says.join(u' ').contains(u"private fixture text"_s));
        QCOMPARE(exchange.actionText(laid.value().number(), row.place), u"private fixture text"_s);
        QVERIFY(exchange.actionText(laid.value().number() + 1, row.place).isEmpty());
        exchange.forget();
        QVERIFY(exchange.actionText(laid.value().number(), row.place).isEmpty());
        QVERIFY(!exchange.take(laid.value().number(), {row.place}).first().came);
        QVERIFY(own.wall.actions().all().isEmpty());
    }

    void importingRechecksNamesAndRollsBackARefusedSettingWrite()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = plainFile(
            {{u"actions"_s,
              QJsonArray{QJsonObject{{u"name"_s, u"Shortcut"_s}, {u"kind"_s, u"keys"_s}, {u"keys"_s, u"F7"_s}}}}});
        const auto laid = laidOutBy(exchange, file, {});
        QVERIFY(laid);
        QVERIFY(own.wall.actions().add({u"SHORTCUT"_s, false, u"F5"_s, {}, false, false}));
        const auto took = exchange.take(laid.value().number(), {WallExchange::firstActionPlace});
        QVERIFY(!took.first().came);
        QCOMPARE(own.wall.actions().all().size(), 1);
        QCOMPARE(own.wall.actions().all().first().keys, u"F5"_s);
        QVERIFY(QFile::remove(own.file));
        QVERIFY(QDir().mkdir(own.file));
        const auto before = own.settings.remembered();
        QVERIFY(!own.wall.takeSettings({2.75, false}));
        QCOMPARE(own.settings.remembered(), before);
    }

    void importedActionNamesUseTheWebComparisonAndSecretsFailWithoutAnEntry()
    {
        QCOMPARE(core::OwnActions::nameForImport(u"  Shortcut  "_s), u"shortcut"_s);
        QCOMPARE(core::OwnActions::nameForImport(u"Stra"_s + QChar(0x00DF) + u"e"_s), u"strasse"_s);
        QCOMPARE(core::OwnActions::nameForImport(QString(QChar(0xFF21)) + QChar(0x200B)), u"a"_s);
        OwnWall own;
        own.secrets.inner.refuses = true;
        const core::exchange::Action text{u"Example"_s, true, {}, u"fixture secret"_s, false, true};
        QVERIFY(!own.wall.actions().takeFromAFile(text));
        QVERIFY(own.wall.actions().all().isEmpty());
        QVERIFY(own.secrets.inner.all.isEmpty());
        own.secrets.inner.refuses = false;
        QVERIFY(own.wall.actions().takeFromAFile(text));
        QVERIFY(!settingsText(own).contains("fixture secret"));
        QCOMPARE(own.wall.actions().textsHandedOutForASealedFile().actions.first().text, text.text);
    }

    void fullNameFoldingMatchesPythonFixtures()
    {
        QFile file(QStringLiteral(QKVM_EXPORT_EXAMPLES "/../import-name-folds.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray cases = QJsonDocument::fromJson(file.readAll()).object()[u"cases"_s].toArray();
        QVERIFY(cases.size() >= 100);
        for (const auto& item : cases) {
            const QJsonArray pair = item.toArray();
            const char32_t sign = static_cast<char32_t>(pair[0].toInt());
            const QString expected = pair[1].toString();
            const QString actual = core::OwnActions::nameForImport(QString::fromUcs4(&sign, 1));
            QVERIFY2(actual == expected,
                     qPrintable(u"Full folding differs for U+%1"_s.arg(static_cast<uint>(sign), 0, 16)));
        }
    }

    void initTestCase()
    {
        // The probe and the driver choose Windows' own TLS, which is refused once TLS is in use: so
        // one of each is made before anything else touches TLS (the stand-ins do).
        const core::KvmProbe firstProbe;
        const core::KvmdDevice firstDriver(core::KvmdDevice::Where{}, QString());
    }

    // --- two steps, and the first changes nothing ---------------------------------------------------

    void layingAFileOutChangesNothingAndNeitherStepContactsAnything()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        WallExchange exchange(own.wall, quickly());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        const base::Result<WallExchange::LaidOut> laidOut =
            laidOutBy(exchange, fileOf({kvmAt(kvm.stand->address(), u"Build server"_s)}));
        QCOMPARE(saidOf(laidOut), u"read"_s);
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 1);
        QVERIFY(rows.constFirst().canBeTaken);
        QCOMPARE(rows.constFirst().verdict, u"It can be taken, as a KVM that runs kvmd (PiKVM, GL.iNet Comet)."_s);
        // Laid out, and nothing is anywhere: not on the wall, not in the store, not in a file.
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.inner.all.isEmpty());
        QCOMPARE(own.secrets.inner.keeps, 0);
        QVERIFY(!QFile::exists(own.file));
        QCOMPARE(changed.count(), 0);
        QTest::qWait(200);
        QCOMPARE(kvm.stand->lines, 0);

        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), placesOf(rows));
        QCOMPARE(took.size(), 1);
        QVERIFY2(took.constFirst().came.ok(), qPrintable(sentenceOf(took.constFirst())));
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.wall.kvms().constFirst().name, u"Build server"_s);
        QCOMPARE(changed.count(), 1);
        // Taken, and still nothing was said to anything: nobody has made a line to it.
        QTest::qWait(400);
        QCOMPARE(kvm.stand->lines, 0);
        QVERIFY(kvm.stand->asked.isEmpty());
    }

    void onlyWhatWasLaidOutAndChosenIsTaken()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QByteArray file =
            fileOf({kvmAt(u"http://127.0.0.1:20001"_s, u"One"_s), kvmAt(u"http://127.0.0.1:20002"_s, u"Two"_s),
                    kvmAt(u"http://127.0.0.1:20003"_s, u"Three"_s)});
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        QCOMPARE(placesOf(rowsOf(laidOut)), (QList<qsizetype>{1, 2, 3}));
        // The second, named twice: once is what it is taken.
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), {2, 2});
        QCOMPARE(took.size(), 1);
        QCOMPARE(took.constFirst().place, qsizetype{2});
        QVERIFY(took.constFirst().came.ok());
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.wall.kvms().constFirst().name, u"Two"_s);
        QCOMPARE(own.wall.kvms().constFirst().id, took.constFirst().came.value());
        QCOMPARE(own.secrets.inner.all.size(), 1);
    }

    void aChoosingThatNamesWhatWasNotLaidOutTakesNothingOfIt()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        // The first entry of the file is none the reader takes: the two that are laid out stand
        // at the second and the third place, and nothing was laid out at the first.
        const QJsonObject broken{{u"name"_s, u"No address"_s}, {u"kind"_s, u"kvmd"_s}};
        const auto entry = [](const QString& address) {
            return QJsonObject{{u"url"_s, address}, {u"kind"_s, u"kvmd"_s}, {u"password"_s, u"secret"_s}};
        };
        const QByteArray file = plainFile({{u"devices"_s, QJsonArray{broken, entry(u"http://127.0.0.1:20001"_s),
                                                                     entry(u"http://127.0.0.1:20002"_s)}}});
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file, QString());
        QCOMPARE(saidOf(laidOut), u"read"_s);
        QCOMPARE(placesOf(rowsOf(laidOut)), (QList<qsizetype>{2, 3}));
        const QString notLaidOut = u"This was not laid out, or what was laid out is not there any more. Nothing of it "
                                   "was taken: read the file again."_s;
        // Places that are nobody's: the one the reader left out, none, one before the first,
        // one behind the last.
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), {1, 0, -1, 4, 2000});
        QCOMPARE(took.size(), 5);
        for (const WallExchange::Took& one : took) {
            QCOMPARE(codeOf(one), u"not_laid_out"_s);
            QCOMPARE(sentenceOf(one), notLaidOut);
        }
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.inner.all.isEmpty());
    }

    void whatIsNotTheLayingOutThatWasMadeLastTakesNothing()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = fileOf({kvmAt(u"http://127.0.0.1:20001"_s), kvmAt(u"http://127.0.0.1:20002"_s)});
        // Nothing was laid out at all.
        QCOMPARE(codeOf(exchange.take(0, {1}).constFirst()), u"not_laid_out"_s);
        QCOMPARE(codeOf(exchange.take(1, {1}).constFirst()), u"not_laid_out"_s);
        const base::Result<WallExchange::LaidOut> first = laidOutBy(exchange, file);
        QCOMPARE(saidOf(first), u"read"_s);
        // A number that is not this one's: one before, one behind, and another's laying-out.
        WallExchange another(own.wall, quickly());
        const base::Result<WallExchange::LaidOut> anothers = laidOutBy(another, file);
        QCOMPARE(saidOf(anothers), u"read"_s);
        QVERIFY(anothers.value().number() != first.value().number());
        for (const quint64 number :
             {quint64{0}, first.value().number() - 1, first.value().number() + 1, anothers.value().number()}) {
            for (const WallExchange::Took& one : exchange.take(number, {1, 2})) {
                QCOMPARE(codeOf(one), u"not_laid_out"_s);
            }
        }
        QVERIFY(own.wall.kvms().isEmpty());
        // What was laid out before another file was asked for is taken from no more, whatever
        // came of that file: here a file that is none.
        const base::Result<WallExchange::LaidOut> none = laidOutBy(exchange, "not a file"_ba);
        QCOMPARE(saidOf(none), u"not_a_file: This is not a file that qKVM made"_s);
        QCOMPARE(codeOf(exchange.take(first.value().number(), {1}).constFirst()), u"not_laid_out"_s);
        // Nor from one that was forgotten.
        const base::Result<WallExchange::LaidOut> second = laidOutBy(exchange, file);
        exchange.forget();
        QCOMPARE(codeOf(exchange.take(second.value().number(), {1}).constFirst()), u"not_laid_out"_s);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.inner.all.isEmpty());
        // And the one that was made last is taken from.
        const base::Result<WallExchange::LaidOut> third = laidOutBy(exchange, file);
        QCOMPARE(codeOf(exchange.take(third.value().number(), {1}).constFirst()), u"taken"_s);
        QCOMPARE(own.wall.kvms().size(), 1);
    }

    // --- only what this program speaks to ------------------------------------------------------------

    void everyDeviceOfTheServersOwnFileIsTakenAsWhatItIs()
    {
        // The server's own file: three KVMs that run kvmd, a JetKVM and a server's controller, each a family this
        // program speaks to by itself (the controller from M7 1b-ii on).
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, exampleFile("sealed-quick.qkvm"));
        QCOMPARE(saidOf(laidOut), u"read"_s);
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 5);
        QVERIFY(rows.at(0).canBeTaken && rows.at(1).canBeTaken && rows.at(2).canBeTaken);
        QCOMPARE(rows.at(0).verdict, u"It can be taken, as a KVM that runs kvmd (PiKVM, GL.iNet Comet)."_s);
        // The JetKVM, as what it is.
        QVERIFY(rows.at(3).canBeTaken);
        QCOMPARE(rows.at(3).verdict, u"It can be taken, as a JetKVM."_s);
        // And the server's controller, as what it is.
        QVERIFY(rows.at(4).canBeTaken);
        QCOMPARE(rows.at(4).verdict, u"It can be taken, as a server's management controller (BMC)."_s);
        // All five chosen: all five are taken.
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), placesOf(rows));
        QCOMPARE(took.size(), 5);
        for (const WallExchange::Took& one : took) {
            QCOMPARE(codeOf(one), u"taken"_s);
        }
        QCOMPARE(own.wall.kvms().size(), 5);
        QStringList addresses;
        QStringList kinds;
        for (const OwnKvm& kvm : own.wall.kvms()) {
            addresses.append(kvm.address);
            kinds.append(kvm.kind);
        }
        QCOMPARE(addresses.first(4), (QStringList{u"https://192.168.1.21"_s, u"http://192.168.1.22:8080"_s,
                                                  u"https://kvm.example.net"_s, u"http://192.168.1.30"_s}));
        QCOMPARE(kinds, (QStringList{u"kvmd"_s, u"kvmd"_s, u"kvmd"_s, u"jetkvm"_s, u"redfish"_s}));
        // What is in the store is the passwords of the five, of each that came with one.
        const qsizetype withPassword =
            std::ranges::count_if(rows, [](const WallExchange::Offered& row) { return row.hasPassword; });
        QVERIFY(withPassword >= 1);
        QCOMPARE(own.secrets.inner.all.size(), withPassword);
    }

    // --- never twice -----------------------------------------------------------------------------------

    void anOpenJetKvmOfAFileWithoutAPasswordIsSaidToNeedNone()
    {
        // A JetKVM out of its box runs without a password: the file's having none is said of it calmly.
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        Device jet = kvmAt(u"http://127.0.0.1:20007"_s, u"Desk"_s, QString());
        jet.kind = u"jetkvm"_s;
        const QList<WallExchange::Offered> rows =
            rowsOf(laidOutBy(exchange, fileOf({jet, kvmAt(u"http://127.0.0.1:20008"_s, u"Rack"_s, QString())})));
        QCOMPARE(rows.size(), 2);
        QVERIFY(rows.at(0).canBeTaken && rows.at(1).canBeTaken);
        QCOMPARE(rows.at(0).says.constLast(),
                 u"The file has no password for it. It is taken without one: fine for a JetKVM that runs without "
                 "one, and where it asks for one, it is typed on this PC."_s);
        QCOMPARE(rows.at(1).says.constLast(), u"The file has no password for it. It is taken without one, and its "
                                              "password is to be typed on this PC."_s);
    }

    void aJetKvmOfAFileIsLaidOutAsOneTakenAskedAndLookedAt()
    {
        OwnWall own;
        JetKvmByHand jet;
        WallExchange exchange(own.wall, quickly());
        Device device = kvmAt(jet.address().toString(), u"A small one"_s);
        device.kind = u"jetkvm"_s;
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, fileOf({device}));
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 1);
        QVERIFY(rows.constFirst().canBeTaken);
        QCOMPARE(rows.constFirst().verdict, u"It can be taken, as a JetKVM."_s);
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), {1});
        QCOMPARE(codeOf(took.constFirst()), u"taken"_s);
        const QString id = theOne(took);
        QCOMPARE(kvmOf(own.wall, id).kind, u"jetkvm"_s);
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
        // Asked what it is, before anything else; a JetKVM answers, and its driver, which has its
        // password from then on, looks at it: online. Nothing of that signs in.
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        QVERIFY(own.wall.wasAskedWhatItIs(id));
        QCOMPARE(jet.stand->count(kvmdsQuestion), 1);
        QCOMPARE(jet.stand->count(redfishsQuestion), 1);
        QCOMPARE(jet.stand->asked.value(3).path, QByteArray(jetKvmsQuestion));
        QCOMPARE(jet.signIns, 0);
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(id)), savedFor("secret", jet.address().toString()));
    }

    void aDeviceAtAnAddressThatIsOnTheWallIsLaidOutAsOnTheWallAlreadyAndIsNotTaken()
    {
        OwnWall own;
        OwnKvm byHand;
        byHand.address = u"https://127.0.0.1:20443"_s;
        byHand.name = u"Rack \"one\""_s;
        QVERIFY(own.wall.add(byHand, u"secret"_s).ok());
        OwnKvm noPort;
        noPort.address = u"https://kvm.invalid"_s;
        QVERIFY(own.wall.add(noPort, u"secret"_s).ok());
        WallExchange exchange(own.wall, quickly());
        // At its address; at its place the other way of speaking to it; one without a port of its
        // own both ways; and one that is somewhere else.
        const QByteArray file = fileOf(
            {kvmAt(u"https://127.0.0.1:20443"_s, u"The same"_s), kvmAt(u"http://127.0.0.1:20443"_s, u"In clear"_s),
             kvmAt(u"http://kvm.invalid"_s, u"Its name"_s), kvmAt(u"https://127.0.0.1:20444"_s, u"Another"_s)});
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 4);
        for (qsizetype index = 0; index < 3; ++index) {
            QVERIFY(!rows.at(index).canBeTaken);
            QCOMPARE(rows.at(index).whyNot, u"duplicate"_s);
        }
        // Which one, by its name set off and its address: a quote in its name does not end that.
        QCOMPARE(rows.at(0).verdict,
                 u"It cannot be taken: it is on the wall already (\"Rack 'one'\" at https://127.0.0.1:20443)."_s);
        QCOMPARE(rows.at(1).verdict, rows.at(0).verdict);
        QCOMPARE(rows.at(2).verdict, u"It cannot be taken: it is on the wall already (https://kvm.invalid)."_s);
        QVERIFY(rows.at(3).canBeTaken);
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), placesOf(rows));
        QCOMPARE(codeOf(took.at(0)), u"duplicate"_s);
        QCOMPARE(codeOf(took.at(1)), u"duplicate"_s);
        QCOMPARE(codeOf(took.at(2)), u"duplicate"_s);
        QCOMPARE(codeOf(took.at(3)), u"taken"_s);
        QCOMPARE(own.wall.kvms().size(), 3);
        // What is kept for the one that was there is what it was: nothing of the file's.
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(own.wall.kvms().constFirst().id)),
                 savedFor("secret", u"https://127.0.0.1:20443"_s));
    }

    void aDeviceWhoseCertificateIsThatOfAKvmOfTheWallIsThatKvmAndIsNotTaken()
    {
        OwnWall own;
        QVERIFY(
            own.wall.add(knownAt(u"https://127.0.0.1:20443"_s, sha256Of("first"), u"Rack one"_s), u"secret"_s).ok());
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = fileOf({knownBy(kvmAt(u"https://127.0.0.1:20999"_s, u"Elsewhere"_s), "first"),
                                        knownBy(kvmAt(u"https://127.0.0.1:20998"_s, u"Another"_s), "second")});
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 2);
        QVERIFY(!rows.at(0).canBeTaken);
        QCOMPARE(rows.at(0).whyNot, u"same_device"_s);
        QVERIFY(!rows.at(0).saysItIsTheSame);
        QCOMPARE(rows.at(0).verdict,
                 u"It cannot be taken: it is the same device as one that is on the wall already at another address "
                 "(\"Rack one\" at https://127.0.0.1:20443): the file names the certificate that one is known by."_s);
        QVERIFY(rows.at(1).canBeTaken);
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), placesOf(rows));
        QCOMPARE(codeOf(took.at(0)), u"same_device"_s);
        QCOMPARE(codeOf(took.at(1)), u"taken"_s);
        QCOMPARE(own.wall.kvms().size(), 2);
    }

    void aJetKvmThatGivesTheNumberOfOneOfTheWallIsThatKvmAndIsNotTaken()
    {
        // As the web's (the second is the first again, and is left out and said): the number a JetKVM gives for
        // itself, which the file carries ("ident"), says which device it is, whatever its address. Of two in a
        // file the first is the one; two that give none are two. And it goes into a file the wall writes.
        OwnWall own;
        const QString n = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        const QString m = u"jetkvm:fedcba9876543210fedcba9876543210"_s;
        OwnKvm there;
        there.address = u"http://127.0.0.1:20443"_s;
        there.name = u"Rack one"_s;
        there.kind = u"jetkvm"_s;
        there.number = n;
        QVERIFY(own.wall.add(there, u"secret"_s).ok());
        WallExchange exchange(own.wall, quickly());
        const auto jetAt = [](const QString& address, const QString& name, const QString& number) {
            Device device = kvmAt(address, name);
            device.kind = u"jetkvm"_s;
            device.ident = number;
            return device;
        };
        const QByteArray file = fileOf(
            {jetAt(u"http://127.0.0.1:20999"_s, u"Elsewhere"_s, n), jetAt(u"http://127.0.0.1:20500"_s, u"Known"_s, m),
             jetAt(u"http://127.0.0.1:20501"_s, u"Its twin"_s, m), jetAt(u"http://127.0.0.1:20600"_s, u"Bare"_s, {}),
             jetAt(u"http://127.0.0.1:20601"_s, u"Bare too"_s, {})});
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 5);
        QVERIFY(!rows.at(0).canBeTaken);
        QCOMPARE(rows.at(0).whyNot, u"same_device"_s);
        // (Said by its number, and so said in its row's few words: OwnFilesView.)
        QVERIFY(rows.at(0).saysItIsTheSame);
        QCOMPARE(rows.at(0).verdict,
                 u"It cannot be taken: it says it is the same device as one that is on the wall already at another "
                 "address (\"Rack one\" at http://127.0.0.1:20443): the file names the number that one gives for "
                 "itself."_s);
        QVERIFY(rows.at(1).canBeTaken);
        QVERIFY(!rows.at(2).canBeTaken);
        QCOMPARE(rows.at(2).whyNot, u"same_device"_s);
        QVERIFY(rows.at(2).saysItIsTheSame);
        QCOMPARE(rows.at(2).verdict,
                 u"It cannot be taken: it says it is the same device as device 2 of this file, which stands before "
                 "it: the file names one number for both."_s);
        QVERIFY(rows.at(3).canBeTaken);
        QVERIFY(rows.at(4).canBeTaken);
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), placesOf(rows));
        QCOMPARE(codeOf(took.at(0)), u"same_device"_s);
        QCOMPARE(codeOf(took.at(1)), u"taken"_s);
        QCOMPARE(codeOf(took.at(2)), u"same_device"_s);
        QCOMPARE(codeOf(took.at(3)), u"taken"_s);
        QCOMPARE(codeOf(took.at(4)), u"taken"_s);
        QCOMPARE(own.wall.kvms().size(), 4);
        QCOMPARE(own.wall.kvms().at(1).number, m);
        QVERIFY(own.wall.kvms().at(2).number.isEmpty());
        const LocalWall::AsAFile written = own.wall.asAFile(false);
        if (!written.content.devices) {
            QFAIL("The wall's file has no devices.");
        }
        const QList<Device>& devices = *written.content.devices;
        QCOMPARE(devices.at(0).ident, n);
        QCOMPARE(devices.at(1).ident, m);
        QVERIFY(devices.at(2).ident.isEmpty());
    }

    void aJetKvmTheFileKeepsBesideItsTwinIsTakenAndWrittenSo()
    {
        // From the reading of the twins (L3): the web's keep_both, which a file carries for a device somebody said is
        // to stay beside its twin, is taken so and written so again.
        OwnWall own;
        const QString n = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        OwnKvm there;
        there.address = u"http://127.0.0.1:20443"_s;
        there.name = u"Rack one"_s;
        there.kind = u"jetkvm"_s;
        there.number = n;
        QVERIFY(own.wall.add(there, u"secret"_s).ok());
        WallExchange exchange(own.wall, quickly());
        Device beside = kvmAt(u"http://127.0.0.1:20999"_s, u"Over the VPN"_s);
        beside.kind = u"jetkvm"_s;
        beside.ident = n;
        beside.keepBoth = true;
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, fileOf({beside}));
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 1);
        QVERIFY2(rows.at(0).canBeTaken, qPrintable(rows.at(0).verdict));
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), placesOf(rows));
        QCOMPARE(codeOf(took.at(0)), u"taken"_s);
        QVERIFY(own.wall.kvms().at(1).keptBesideItsTwin);
        const LocalWall::AsAFile written = own.wall.asAFile(false);
        if (!written.content.devices) {
            QFAIL("The wall's file has no devices.");
        }
        QVERIFY(!written.content.devices->at(0).keepBoth);
        QVERIFY(written.content.devices->at(1).keepBoth);
    }

    void ofTwoDevicesOfOneFileThatAreTheSameTheFirstIsTheOne()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        // By the place they are at (the reader takes both: their addresses are two texts); and
        // by the certificate the file names for them.
        const QByteArray file = fileOf({kvmAt(u"https://127.0.0.1:20443"_s, u"First"_s),
                                        kvmAt(u"http://127.0.0.1:20443"_s, u"The same in the clear"_s),
                                        knownBy(kvmAt(u"https://127.0.0.1:20500"_s, u"Known"_s), "first"),
                                        knownBy(kvmAt(u"https://127.0.0.1:20501"_s, u"Its twin"_s), "first")});
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 4);
        QVERIFY(rows.at(0).canBeTaken);
        QVERIFY(!rows.at(1).canBeTaken);
        QCOMPARE(rows.at(1).whyNot, u"duplicate"_s);
        QCOMPARE(rows.at(1).verdict,
                 u"It cannot be taken: device 1 of this file is at the same address and stands before it."_s);
        QVERIFY(rows.at(2).canBeTaken);
        QVERIFY(!rows.at(3).canBeTaken);
        QCOMPARE(rows.at(3).whyNot, u"same_device"_s);
        QVERIFY(!rows.at(3).saysItIsTheSame);
        QCOMPARE(rows.at(3).verdict,
                 u"It cannot be taken: it is the same device as device 3 of this file, which stands before it: the "
                 "file names one certificate for both."_s);
        // Chosen all the same, and chosen alone: the second of two is not taken.
        QCOMPARE(codeOf(exchange.take(laidOut.value().number(), {2, 4}).constFirst()), u"duplicate"_s);
        QVERIFY(own.wall.kvms().isEmpty());
        // (A taking ends the laying-out it takes from: laid out once more for the next.)
        const base::Result<WallExchange::LaidOut> again = laidOutBy(exchange, file);
        const QList<WallExchange::Took> took = exchange.take(again.value().number(), placesOf(rows));
        QCOMPARE(codeOf(took.at(0)), u"taken"_s);
        QCOMPARE(codeOf(took.at(1)), u"duplicate"_s);
        QCOMPARE(codeOf(took.at(2)), u"taken"_s);
        QCOMPARE(codeOf(took.at(3)), u"same_device"_s);
        QCOMPARE(own.wall.kvms().size(), 2);
    }

    void aSecondTakingOfTheSameFileAddsNothing()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = fileOf({kvmAt(u"http://127.0.0.1:20001"_s, u"One"_s),
                                        knownBy(kvmAt(u"https://127.0.0.1:20002"_s, u"Two"_s), "first"),
                                        saying(kvmAt(u"https://127.0.0.1:20003"_s, u"Three"_s), u"system"_s)});
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        const QList<qsizetype> places = placesOf(rowsOf(laidOut));
        for (const WallExchange::Took& one : exchange.take(laidOut.value().number(), places)) {
            QCOMPARE(codeOf(one), u"taken"_s);
        }
        QCOMPARE(own.wall.kvms().size(), 3);
        const QList<OwnKvm> taken = own.wall.kvms();
        const QHash<QString, QByteArray> kept = own.secrets.inner.all;
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        // Taken once more from what was laid out: that laying-out ended with its taking, and
        // nothing of it is there.
        for (const WallExchange::Took& one : exchange.take(laidOut.value().number(), places)) {
            QCOMPARE(codeOf(one), u"not_laid_out"_s);
        }
        // And laid out anew, each is laid out as that, and none is taken.
        const base::Result<WallExchange::LaidOut> again = laidOutBy(exchange, file);
        for (const WallExchange::Offered& row : rowsOf(again)) {
            QVERIFY(!row.canBeTaken);
            QCOMPARE(row.whyNot, u"duplicate"_s);
        }
        for (const WallExchange::Took& one : exchange.take(again.value().number(), places)) {
            QCOMPARE(codeOf(one), u"duplicate"_s);
        }
        QCOMPARE(own.wall.kvms(), taken);
        QCOMPARE(own.secrets.inner.all, kept);
        QCOMPARE(changed.count(), 0);
    }

    void theWallItselfTakesNoKvmTwiceWhoeverAsks()
    {
        // What a laying-out refuses, the wall refuses once more by itself: whoever hands it KVMs
        // from a file, by whatever road.
        OwnWall own;
        QVERIFY(
            own.wall.add(knownAt(u"https://127.0.0.1:20443"_s, sha256Of("first"), u"Rack one"_s), u"secret"_s).ok());
        const auto from = [](const QString& address, const QByteArray& certificate = {}) {
            LocalWall::FromAFile one;
            one.kvm.address = address;
            one.kvm.knownByCertificate = !certificate.isEmpty();
            one.kvm.certificate = certificate;
            // (Said to have been asked: that is not this one's to say.)
            one.kvm.askedWhatItIs = true;
            one.password = u"from the file"_s;
            return one;
        };
        const QList<base::Result<QString>> came = own.wall.takeFromAFile(
            {from(u"http://127.0.0.1:20443"_s), from(u"https://127.0.0.1:20999"_s, sha256Of("first")),
             from(u"https://127.0.0.1:20500"_s, sha256Of("second")), from(u"http://127.0.0.1:20500"_s),
             from(u"https://127.0.0.1:20501"_s, sha256Of("second")), from(u"HTTPS://127.0.0.1:20502"_s),
             from(u"https://127.0.0.1:20503"_s, QByteArray(5, 'c')), from(u"http://127.0.0.1:20504"_s)});
        QCOMPARE(came.size(), 8);
        QCOMPARE(saidOf(came.at(0)), u"duplicate: This device is already on the wall"_s);
        QCOMPARE(
            saidOf(came.at(1)),
            u"same_device: This is the same device as \"Rack one\", which is on the wall already (127.0.0.1:20443)"_s);
        QVERIFY(came.at(2).ok());
        // The same as one that was taken a moment ago, from this very list.
        QCOMPARE(saidOf(came.at(3)), u"duplicate: This device is already on the wall"_s);
        QVERIFY(saidOf(came.at(4)).startsWith(u"same_device: "_s));
        // An address that is not written as the program writes one, and a certificate that is none.
        QCOMPARE(saidOf(came.at(5)), u"invalid: This address is not written the way the wall keeps one"_s);
        QVERIFY(saidOf(came.at(6)).startsWith(u"cert_invalid: "_s));
        QVERIFY(came.at(7).ok());
        QCOMPARE(own.wall.kvms().size(), 3);
        // Only what was taken has anything in the store, and none of them was asked what it is.
        QCOMPARE(own.secrets.inner.all.size(), 3);
        QVERIFY(!own.wall.wasAskedWhatItIs(came.at(2).value()));
        QVERIFY(!kvmOf(own.wall, came.at(2).value()).askedWhatItIs);
        QVERIFY(!own.wall.wasAskedWhatItIs(came.at(7).value()));
    }

    // --- what a KVM from a file is known by ---------------------------------------------------------

    void whatAKvmFromAFileWillBeKnownByIsLaidOutAndIsWhatItIsTakenAs()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = fileOf({knownBy(kvmAt(u"https://127.0.0.1:20001"_s, u"Pinned"_s), "first"),
                                        saying(kvmAt(u"https://127.0.0.1:20002"_s, u"By Windows"_s), u"system"_s),
                                        kvmAt(u"https://127.0.0.1:20003"_s, u"No certificate"_s),
                                        saying(kvmAt(u"https://127.0.0.1:20004"_s, u"Not checked"_s), u"insecure"_s),
                                        kvmAt(u"http://127.0.0.1:20005"_s, u"Plain"_s)});
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 5);
        using KnownBy = WallExchange::KnownBy;

        // The file names its certificate: by that one, with its fingerprint to compare.
        QCOMPARE(rows.at(0).knownBy, KnownBy::ItsCertificate);
        QCOMPARE(rows.at(0).fileSaysOfTls, u"pinned"_s);
        QVERIFY(rows.at(0).fileNamesACertificate);
        QVERIFY(!rows.at(0).notByWhatItShowsFirst);
        QCOMPARE(rows.at(0).fingerprint, core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(rows.at(0).fingerprint.size(), 95);
        QVERIFY(rows.at(0).says.contains(u"The file names its certificate. It will be known by that certificate and "
                                         "by no other."_s));
        // Windows' word.
        QCOMPARE(rows.at(1).knownBy, KnownBy::WindowsWord);
        QCOMPARE(rows.at(1).fileSaysOfTls, u"system"_s);
        QVERIFY(!rows.at(1).fileNamesACertificate);
        QVERIFY(!rows.at(1).notByWhatItShowsFirst);
        QVERIFY(rows.at(1).fingerprint.isEmpty());
        // "Pinned" and no certificate, and "not checked": not by what it shows first.
        const QString notAtFirstSight =
            u" This program does not take the first certificate a device from a file shows. It will be known by "
            "Windows' word. A KVM's own certificate does not have that: then its tile says that its certificate is "
            "not trusted, and nothing is sent to it before somebody has looked at that certificate and trusted it. "
            "Where Windows does trust the certificate it shows, it is asked what it is and signed in to on Windows' "
            "word, with nobody having looked at that certificate."_s;
        QCOMPARE(rows.at(2).knownBy, KnownBy::WindowsWord);
        QCOMPARE(rows.at(2).fileSaysOfTls, u"pinned"_s);
        QVERIFY(!rows.at(2).fileNamesACertificate);
        QVERIFY(rows.at(2).notByWhatItShowsFirst);
        QVERIFY(rows.at(2).says.contains(u"The file names no certificate for it: a qKVM server would know it by the "
                                         "first certificate it shows."_s +
                                         notAtFirstSight));
        QCOMPARE(rows.at(3).knownBy, KnownBy::WindowsWord);
        QCOMPARE(rows.at(3).fileSaysOfTls, u"insecure"_s);
        QVERIFY(rows.at(3).notByWhatItShowsFirst);
        QVERIFY(rows.at(3).says.contains(u"The file says that its certificate is not checked."_s + notAtFirstSight));
        // Plain HTTP: by nothing, and its password goes in the clear.
        QCOMPARE(rows.at(4).knownBy, KnownBy::Nothing);
        QVERIFY(rows.at(4).passwordInTheClear);
        QVERIFY(rows.at(4).fileSaysOfTls.isEmpty());
        QVERIFY(rows.at(4).says.contains(u"It is spoken to over plain HTTP: its password goes in the clear, and so "
                                         "does everything that is seen and typed there. Nothing shows who answers at "
                                         "that address."_s));
        for (qsizetype index = 0; index < 4; ++index) {
            QVERIFY(!rows.at(index).passwordInTheClear);
        }

        for (const WallExchange::Took& one : exchange.take(laidOut.value().number(), placesOf(rows))) {
            QCOMPARE(codeOf(one), u"taken"_s);
        }
        const QList<OwnKvm> kvms = own.wall.kvms();
        QCOMPARE(kvms.size(), 5);
        QVERIFY(kvms.at(0).knownByCertificate);
        QCOMPARE(kvms.at(0).certificate, sha256Of("first"));
        // No pin is made of anything for the others: there is none to make one of.
        for (qsizetype index = 1; index < 5; ++index) {
            QVERIFY(!kvms.at(index).knownByCertificate);
            QVERIFY(kvms.at(index).certificate.isEmpty());
        }
        for (const OwnKvm& kvm : kvms) {
            QVERIFY(!kvm.askedWhatItIs);
        }
        // The certificate itself is the wall's from the file, at once; and only that one's.
        QCOMPARE(own.wall.certificateOf(kvms.at(0).id), keptPemOf("first"));
        QVERIFY(!keptPemOf("first").isEmpty());
        QCOMPARE(own.settings.remembered().kvmCertificates.size(), 1);
    }

    void aKvmWhoseFileNamesItsCertificateIsAskedAndSignedInToUnderThatCertificate()
    {
        OwnWall own;
        const MadeUpKvm kvm(tls("first"));
        WallExchange exchange(own.wall, quickly());
        const QString id = theOne(takenFrom(exchange, {knownBy(kvmAt(kvm.stand->address()), "first")}));
        QVERIFY(!id.isEmpty());
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", kvm.stand->address(), sha256Of("first")));
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(own.wall.wasAskedWhatItIs(id));
        // Asked first, and signed in to only then.
        QVERIFY(kvm.stand->asked.size() > 3);
        for (qsizetype index = 0; index < 3; ++index) {
            QCOMPARE(kvm.stand->asked.at(index).method, "GET"_ba);
            QVERIFY(kvm.stand->asked.at(index).path != signInQuestion);
        }
    }

    void whatShowsAnotherCertificateThanTheOneTheFileNamesHearsNothingAtAll()
    {
        OwnWall own;
        // At the address the file names, something that shows another certificate than the file's.
        const MadeUpKvm kvm(tls("second"));
        WallExchange exchange(own.wall, quickly());
        const QString id = theOne(takenFrom(exchange, {knownBy(kvmAt(kvm.stand->address()), "first")}));
        QVERIFY(!id.isEmpty());
        const core::Status status = settled(own.wall, id);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"cert_mismatch"_s);
        QCOMPARE(status.errorMessage, u"Certificate does not match the one saved for this device"_s);
        // Not the password, and not the three questions either: no question at all.
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvm.stand->notHttp, 0);
        QCOMPARE(kvm.signIns, 0);
        // Nor at the askings after that; and it stays what it was: known by the file's
        // certificate, not asked, its password where it was.
        QTest::qWait(quickDriver().holdMs + 100);
        QCOMPARE(settled(own.wall, id).errorCode, u"cert_mismatch"_s);
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("first"));
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", kvm.stand->address(), sha256Of("first")));
    }

    void aKvmWhoseFileNamesNoCertificateIsNotKnownByWhatItShowsFirst_data()
    {
        QTest::addColumn<QString>("fileSays");
        QTest::newRow("pinned, and no certificate named") << u"pinned"_s;
        QTest::newRow("its certificate is not checked") << u"insecure"_s;
    }

    void aKvmWhoseFileNamesNoCertificateIsNotKnownByWhatItShowsFirst()
    {
        QFETCH(QString, fileSays);
        OwnWall own;
        // A KVM as they come: with a certificate it made itself, which Windows does not trust.
        const MadeUpKvm kvm(tls("first"));
        WallExchange exchange(own.wall, quickly());
        const QString id = theOne(takenFrom(exchange, {saying(kvmAt(kvm.stand->address()), fileSays)}));
        QVERIFY(!id.isEmpty());
        QVERIFY(!kvmOf(own.wall, id).knownByCertificate);

        // The wall is asked how its KVMs are. No certificate is remembered for what it shows
        // first, and nothing was said to it: not the three questions either.
        core::Status status = settled(own.wall, id);
        QVERIFY(!kvmOf(own.wall, id).knownByCertificate);
        QVERIFY(kvmOf(own.wall, id).certificate.isEmpty());
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvm.signIns, 0);
        // Its tile: something to look at, in the driver's code for a certificate that is not
        // trusted.
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"cert_invalid"_s);
        QCOMPARE(status.errorMessage,
                 u"Windows does not trust the certificate this device shows, and no certificate is saved for it. "
                 "Nothing was sent to it. Review its certificate and trust it; no password goes to it before "
                 "that."_s);
        // It is asked again later, and nothing comes of that by itself: no certificate is
        // remembered that nobody said yes to, and nothing is said to what shows it.
        QTest::qWait(quickDriver().holdMs + 100);
        status = settled(own.wall, id);
        QCOMPARE(status.errorCode, u"cert_invalid"_s);
        QVERIFY(kvm.stand->asked.isEmpty());
        QVERIFY(!kvmOf(own.wall, id).knownByCertificate);
        QVERIFY(kvmOf(own.wall, id).certificate.isEmpty());
        QVERIFY(!own.wall.wasAskedWhatItIs(id));

        // The way on is the one there is for a certificate nobody has said yes to: somebody looks
        // at what it shows, beside what it is known by (Windows' word, and so nothing).
        LocalWall* const forTheBox = own.wall.forABox();
        QVERIFY(forTheBox != nullptr);
        core::KvmCertificateReview review(*forTheBox, quickBox().probe);
        const base::Result<core::KvmCertificateReview::Shown> shown = answerTo<core::KvmCertificateReview::Shown>(
            [&](auto done) { review.askCertificates(id, std::move(done)); });
        QVERIFY2(shown.ok(), shown.ok() ? "" : shown.error().message.c_str());
        QVERIFY(shown.value().remembered.isEmpty());
        QCOMPARE(shown.value().presented, sha256Of("first"));
        // (That look asks the three questions, with nothing in them that says who asks.)
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
        QCOMPARE(kvm.signIns, 0);
        // And says yes to that very certificate.
        const core::KvmCertificateReview::Outcome yes = outcomeOf<core::KvmCertificateReview::Outcome>(
            [&](auto done) { review.trust(id, shown.value().presented, std::move(done)); }, neverTrusted());
        QVERIFY2(yes.result.ok(), yes.result.ok() ? "" : yes.result.error().message.c_str());
        QVERIFY(kvmOf(own.wall, id).knownByCertificate);
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("first"));
        // The yes was to the certificate. What it is, is still to be asked: nothing was signed
        // in to, and its password went with it, saved for that certificate.
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
        QCOMPARE(kvm.signIns, 0);
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", kvm.stand->address(), sha256Of("first")));
        // Now the wall asks, under that certificate, and signs in.
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(own.wall.wasAskedWhatItIs(id));
    }

    void aKvmThatWindowsVouchesForIsAskedAndSignedInToOnWindowsWordAndOnNoOther()
    {
        const MadeUpKvm kvm(tls("signed"));
        const Device device = saying(kvmAt(kvm.stand->address()), u"system"_s);
        {
            // Nobody vouches for what it shows: it hears nothing.
            OwnWall own;
            WallExchange exchange(own.wall, quickly());
            const QString id = theOne(takenFrom(exchange, {device}));
            QVERIFY(!id.isEmpty());
            const core::Status status = settled(own.wall, id);
            QCOMPARE(status.state, u"error"_s);
            QCOMPARE(status.errorCode, u"cert_invalid"_s);
            QVERIFY(kvm.stand->asked.isEmpty());
            QVERIFY(!own.wall.wasAskedWhatItIs(id));
        }
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QString id = theOne(takenFrom(exchange, {device}));
        QVERIFY(!id.isEmpty());
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(own.wall.wasAskedWhatItIs(id));
        QVERIFY(!kvmOf(own.wall, id).knownByCertificate);
    }

    // --- no password before what answers was asked what it is ----------------------------------------

    void noPasswordGoesToAKvmFromAFileBeforeItWasAskedWhatItIs()
    {
        OwnWall own;
        // (Over HTTPS, known by its certificate: of such a KVM it is kept that it was asked.)
        const MadeUpKvm kvm(tls("first"));
        WallExchange exchange(own.wall, quickly());
        const QString id =
            theOne(takenFrom(exchange, {knownBy(kvmAt(kvm.stand->address(), u"Build server"_s), "first")}));
        QVERIFY(!id.isEmpty());
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
        QVERIFY(!kvmOf(own.wall, id).askedWhatItIs);
        QVERIFY(settingsText(own).contains("\"asked\": false"));
        QVERIFY(kvm.stand->asked.isEmpty());

        // The wall is asked how its KVMs are: it asks what answers there, and signs in then.
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        const QList<Asked> asked = kvm.stand->asked;
        qsizetype signIn = -1;
        for (qsizetype index = 0; index < asked.size() && signIn < 0; ++index) {
            signIn = asked.at(index).path == signInQuestion ? index : -1;
        }
        // Before the sign-in: the three questions, each once, and nothing in them that says who
        // asks; and kvmd's again, by its driver: a password goes only on a line shown to be up.
        // And nothing else.
        QCOMPARE(signIn, qsizetype{4});
        QCOMPARE(asked.at(3).path, QByteArray(kvmdsQuestion));
        QSet<QByteArray> questions;
        for (qsizetype index = 0; index < signIn; ++index) {
            const Asked& one = asked.at(index);
            QCOMPARE(one.method, "GET"_ba);
            QVERIFY(one.body.isEmpty());
            QVERIFY(!one.headers.contains("cookie"));
            QVERIFY(!one.headers.contains("authorization"));
            QVERIFY(!one.path.contains("secret"));
            questions.insert(one.path);
        }
        QCOMPARE(questions, (QSet<QByteArray>{kvmdsQuestion, jetKvmsQuestion, redfishsQuestion}));
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(asked.at(signIn).body, "user=admin&passwd=secret"_ba);

        // That it was asked is kept, and it is asked once: not again at the next look, and not
        // at the next start.
        QVERIFY(own.wall.wasAskedWhatItIs(id));
        QVERIFY(kvmOf(own.wall, id).askedWhatItIs);
        // (Written down a short while later, with what else the wall records by itself.)
        QTRY_VERIFY(settingsText(own).contains("\"asked\": true"));
        QTest::qWait(quickDriver().freshMs + 100);
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        core::Settings settings(own.file);
        QVERIFY(settings.remembered().kvms.constFirst().askedWhatItIs);
        WallOfATest again(settings, own.secrets, quickDriver());
        QCOMPARE(settled(again, id).state, u"online"_s);
        QTest::qWait(150);
        QCOMPARE(kvm.stand->count(jetKvmsQuestion), 1);
        QCOMPARE(kvm.stand->count(redfishsQuestion), 1);
    }

    void aKvmFromAFileIsAskedAgainAtTheNextStartForAsLongAsItWasNot()
    {
        // Taken, and the program ended before the wall was ever asked how its KVMs are: at the
        // next start it is one that was not asked, and its driver is given no password.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        WatchedSecrets secrets;
        const MadeUpKvm kvm;
        QString id;
        {
            core::Settings settings(file);
            WallOfATest wall(settings, secrets, quickDriver());
            WallExchange exchange(wall, quickly());
            id = theOne(takenFrom(exchange, {kvmAt(kvm.stand->address())}));
            QVERIFY(!id.isEmpty());
        }
        core::Settings settings(file);
        const int readBefore = secrets.reads;
        WallOfATest wall(settings, secrets, quickDriver());
        QVERIFY(!wall.wasAskedWhatItIs(id));
        // (Nothing is read from the store for a KVM that was not asked.)
        QCOMPARE(secrets.reads, readBefore);
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QCOMPARE(kvm.stand->asked.value(4).path, QByteArray(signInQuestion)); // (after the question asked first)
        QCOMPARE(kvm.stand->count(jetKvmsQuestion), 1);
    }

    void whatAnswersThereAndIsNoKvmIsSaidAndThePasswordStaysWhereItIs_data()
    {
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("sentence");
        QTest::newRow("a page") << u"page"_s << u"not_a_kvm"_s
                                << u"Something answers at this address, but it is not a KVM that this program "
                                   "speaks to. No password was sent to it."_s;
        // (On its card: the JetKVM is not added where this KVM stands, so this one goes first.)
        QTest::newRow("a JetKVM") << u"jetkvm"_s << u"other_family"_s
                                  << u"A JetKVM answers at this address, and this KVM is on the wall as a GL.iNet "
                                     "Comet / PiKVM. To have the JetKVM here, remove this KVM from the wall and add "
                                     "the JetKVM as a KVM of its own. No password was sent to it."_s;
        // (A server's controller is a family this program speaks to from M7 1b-i on: another device, as a JetKVM is.)
        QTest::newRow("a server's controller")
            << u"controller"_s << u"other_family"_s
            << u"A server's management controller (BMC) answers at this address, and this KVM is on the wall as a "
               "GL.iNet Comet / PiKVM. To have the server's management controller (BMC) here, remove this KVM from "
               "the wall and add the server's management controller (BMC) as a KVM of its own. No password was sent "
               "to it."_s;
    }

    void whatAnswersThereAndIsNoKvmIsSaidAndThePasswordStaysWhereItIs()
    {
        QFETCH(QString, what);
        QFETCH(QString, code);
        QFETCH(QString, sentence);
        OwnWall own;
        StandIn stand(what == u"page"_s ? asAPage : what == u"jetkvm"_s ? asAJetKvm : asAController);
        WallExchange exchange(own.wall, quickly());
        const QString id = theOne(takenFrom(exchange, {kvmAt(stand.address(), u"Looks like a KVM"_s)}));
        QVERIFY(!id.isEmpty());
        const core::Status status = settled(own.wall, id);
        // It heard the three questions, and nothing of a password.
        QCOMPARE(stand.count(signInQuestion), 0);
        QVERIFY(heardOnlyTheQuestions(stand));
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, code);
        QCOMPARE(status.errorMessage, sentence);
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
        QVERIFY(!kvmOf(own.wall, id).askedWhatItIs);
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(id)), savedFor("secret", stand.address()));
        // Asked again later, it hears the three questions again, and still nothing else.
        QTest::qWait(quickDriver().holdMs + 100);
        QCOMPARE(settled(own.wall, id).errorCode, code);
        QTRY_VERIFY(heardOnlyTheQuestions(stand, 2));
        QCOMPARE(stand.count(signInQuestion), 0);
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
    }

    void whereNothingAnswersThatIsSaidAndTheKvmIsAskedWhenItIsThere()
    {
        OwnWall own;
        quint16 port = 0;
        QString address;
        QString id;
        WallExchange exchange(own.wall, quickly());
        {
            const core::testing::NobodyThere nobody;
            port = nobody.port();
            address = nobody.address();
            id = theOne(takenFrom(exchange, {kvmAt(address)}));
            QVERIFY(!id.isEmpty());
            const core::Status status = settled(own.wall, id);
            QCOMPARE(status.state, u"offline"_s);
            QCOMPARE(status.errorCode, u"unreachable"_s);
            QCOMPARE(status.errorMessage,
                     u"Nothing answers at this address. No password is sent there before a KVM has answered."_s);
            QVERIFY(!own.wall.wasAskedWhatItIs(id));
        }
        // The KVM comes up at that address: asked at the wall's next asking that is due, and
        // signed in to.
        const MadeUpKvm kvm({}, port);
        QVERIFY(kvm.stand->isListening());
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(own.wall).value(id).state, u"online"_s, 15000);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(own.wall.wasAskedWhatItIs(id));
    }

    void whatIsNoKvmIsAskedAgainLaterAndLaterEachTime()
    {
        OwnWall own;
        StandIn stand(asAPage);
        WallExchange exchange(own.wall, quickly());
        const QString id = theOne(takenFrom(exchange, {kvmAt(stand.address())}));
        QVERIFY(!id.isEmpty());
        QCOMPARE(settled(own.wall, id).errorCode, u"not_a_kvm"_s);
        QVERIFY(heardOnlyTheQuestions(stand));
        // However often the wall is asked how its KVMs are: it is left alone for a moment.
        for (int times = 0; times < 5; ++times) {
            QCOMPARE(statusOf(own.wall).value(id).errorCode, u"not_a_kvm"_s);
        }
        QTest::qWait(100);
        QVERIFY(heardOnlyTheQuestions(stand));
        // After that moment (holdMs) it is asked again; and then left alone for twice as long.
        QTest::qWait(quickDriver().holdMs);
        QCOMPARE(statusOf(own.wall).value(id).errorCode, u"not_a_kvm"_s);
        QTRY_VERIFY(heardOnlyTheQuestions(stand, 2));
        QTest::qWait(quickDriver().holdMs + 50);
        static_cast<void>(statusOf(own.wall));
        QTest::qWait(100);
        QVERIFY(heardOnlyTheQuestions(stand, 2));
        QTest::qWait(quickDriver().holdMs);
        static_cast<void>(statusOf(own.wall));
        QTRY_VERIFY(heardOnlyTheQuestions(stand, 3));
    }

    void whileAKvmIsAskedForTheFirstTimeItsTileSaysConnectingAndNothingElse()
    {
        // Something that takes its time over every answer, and a wall that waits for nobody.
        core::KvmdDevice::Pace pace = quickDriver();
        pace.lookPatienceMs = 100;
        OwnWall own(pace);
        StandIn stand([](const Asked& asked) {
            Answered answer = asAPage(asked);
            answer.afterMs = 500;
            return answer;
        });
        WallExchange exchange(own.wall, quickly());
        const QString id = theOne(takenFrom(exchange, {kvmAt(stand.address())}));
        QVERIFY(!id.isEmpty());
        // As the tile of any KVM that has not answered yet: "connecting", with no code and no
        // sentence under it. Nothing is wrong with a KVM that is being asked what it is, and
        // every KVM of a wall that is well stands here for a moment at its first start. (The
        // sentence for a KVM that was not asked yet stood under it once.)
        const core::Status meanwhile = statusOf(own.wall).value(id);
        QCOMPARE(meanwhile.state, u"connecting"_s);
        QCOMPARE(meanwhile.errorCode, QString());
        QCOMPARE(meanwhile.errorMessage, QString());
        // That sentence is for whoever asks something of such a KVM that cannot be done yet.
        QCOMPARE(QString::fromStdString(LocalWall::notAskedYet().message),
                 u"This device has not been asked yet what it is. Nothing is sent to it, and no password goes "
                 "anywhere, before what answers at its address has said that it is a KVM."_s);
        // Asked a second time while the first asking is under way, it is not asked twice.
        // And its picture, which the wall asks for by itself, is to be asked for again soon.
        const auto meanwhilePicture =
            answerTo<core::Snapshot>([&](auto told) { own.wall.askSnapshot(id, 480, 0, std::move(told)); });
        QCOMPARE(meanwhilePicture.error().code, std::string("no_stream"));
        static_cast<void>(statusOf(own.wall));
        QCOMPARE(settled(own.wall, id).errorCode, u"not_a_kvm"_s);
        QVERIFY(heardOnlyTheQuestions(stand));
        // Once an asking did not find the KVM, that it was not asked yet is what is said.
        const auto picture =
            answerTo<core::Snapshot>([&](auto told) { own.wall.askSnapshot(id, 480, 0, std::move(told)); });
        QCOMPARE(picture.error().code, std::string("not_asked_yet"));
        QCOMPARE(picture.error().message, LocalWall::notAskedYet().message);
        QVERIFY(heardOnlyTheQuestions(stand));
    }

    void aKvmThatWasNotAskedYetIsAskedNothingAndNothingIsDoneToIt()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        WallExchange exchange(own.wall, quickly());
        const QString id = theOne(takenFrom(exchange, {kvmAt(kvm.stand->address())}));
        QVERIFY(!id.isEmpty());
        const std::string notAsked = "not_asked_yet";
        // Nothing is done to it, no picture is asked of it, it is not had live, and the kept
        // password is handed to no driver: each refused, and no line is made.
        const auto done = answerTo<core::Done>([&](auto told) {
            own.wall.act(id, core::Action::press(u"ControlLeft,AltLeft,Delete"_s), std::move(told));
        });
        QVERIFY(!done.ok());
        QCOMPARE(done.error().code, notAsked);
        QVERIFY(!own.wall.mayHaveBeenDone(id, done.error()));
        // (A picture and a live view are what a wall asks for again by itself: while no asking
        // has found anything amiss they are refused as of a driver that was only just made,
        // "ask again soon", and nothing is asked of the KVM for them either.)
        const std::string askAgainSoon = "no_stream";
        const auto picture =
            answerTo<core::Snapshot>([&](auto told) { own.wall.askSnapshot(id, 480, 0, std::move(told)); });
        QVERIFY(!picture.ok());
        QCOMPARE(picture.error().code, askAgainSoon);
        QCOMPARE(picture.error().message, std::string("Connecting to the KVM"));
        const auto toWake =
            answerTo<QList<core::WakeTarget>>([&](auto told) { own.wall.askWakeTargets(id, std::move(told)); });
        QVERIFY(!toWake.ok());
        QCOMPARE(toWake.error().code, notAsked);
        QList<std::string> liveSaid;
        own.wall.openLive(id, {1920, 30}, [&](base::Result<std::unique_ptr<core::Live>> made) {
            liveSaid.append(made ? std::string("live") : made.error().code);
        });
        QTRY_COMPARE(liveSaid.size(), 1);
        QCOMPARE(liveSaid.constFirst(), askAgainSoon);
        const auto signer =
            own.wall.signerIn(id, QUrl(kvm.stand->address()), u"operator"_s, core::DriverPaces::alike(quickDriver()));
        QVERIFY(!signer.ok());
        QCOMPARE(signer.error().code, notAsked);
        QTest::qWait(300);
        QCOMPARE(kvm.stand->lines, 0);
        QVERIFY(kvm.stand->asked.isEmpty());
    }

    void aPasswordTypedForAKvmThatWasNotAskedYetGoesNowhere()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        WallExchange exchange(own.wall, quickly());
        const QString id = theOne(takenFrom(exchange, {kvmAt(kvm.stand->address(), u"From a file"_s)}));
        QVERIFY(!id.isEmpty());
        LocalWall* const forTheBox = own.wall.forABox();
        QVERIFY(forTheBox != nullptr);
        core::KvmEditing editing(*forTheBox, quickBox());
        const auto changedBy = [&](const core::KvmEditing::Typed& typed) {
            return outcomeOf<core::KvmEditing::Outcome>([&](auto done) { editing.change(id, typed, std::move(done)); },
                                                        neverChanged());
        };
        // A password typed in the box that changes a KVM, for the address it has; and another
        // user name, with the kept one.
        core::KvmEditing::Typed typed;
        typed.address = kvm.stand->address();
        typed.password = u"typed in the box"_s;
        core::KvmEditing::Outcome outcome = changedBy(typed);
        QVERIFY(!outcome.result.ok());
        QCOMPARE(outcome.result.error().code, std::string("not_asked_yet"));
        QCOMPARE(outcome.result.error().message, LocalWall::notAskedYet().message);
        typed.password.clear();
        typed.user = u"operator"_s;
        outcome = changedBy(typed);
        QVERIFY(!outcome.result.ok());
        QCOMPARE(outcome.result.error().code, std::string("not_asked_yet"));
        QTest::qWait(300);
        QCOMPARE(kvm.stand->lines, 0);
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(id)), savedFor("secret", kvm.stand->address()));
        // Its name and its group are this PC's own, and are changed with nothing asked of it;
        // it stays one that was not asked.
        typed.user.clear();
        typed.name = u"Renamed"_s;
        typed.group = u"Rack 2"_s;
        outcome = changedBy(typed);
        QVERIFY2(outcome.result.ok(), outcome.result.ok() ? "" : outcome.result.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).name, u"Renamed"_s);
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
        QVERIFY(!kvmOf(own.wall, id).askedWhatItIs);
        QCOMPARE(kvm.stand->lines, 0);
        // Whatever the wall is handed as the KVM, it is not made one that was asked by that.
        OwnKvm claimed = kvmOf(own.wall, id);
        claimed.askedWhatItIs = true;
        claimed.group = u"Rack 3"_s;
        QVERIFY(own.wall.change(id, claimed, std::nullopt).ok());
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
        QVERIFY(!kvmOf(own.wall, id).askedWhatItIs);
        // And with a password handed to the wall for it, the wall's own driver still has none:
        // it is kept, and goes nowhere before the KVM was asked.
        QVERIFY(own.wall.change(id, kvmOf(own.wall, id), u"secret"_s).ok());
        QTest::qWait(300);
        QCOMPARE(kvm.stand->lines, 0);
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        QCOMPARE(kvm.stand->asked.value(4).path, QByteArray(signInQuestion)); // (after the question asked first)
    }

    void aKvmThatWasNotAskedAndIsKnownByWhatIsNoCertificateHearsNothing()
    {
        // A list that was written by hand, or damaged in one place: a KVM that is said to be known
        // by a certificate, names none that is one, and was never asked what it is. There is
        // nothing to hold a line to: nothing is asked of it, and its card says what to do.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        WatchedSecrets secrets;
        const MadeUpKvm kvm(tls("first"));
        OwnKvm own = knownAt(kvm.stand->address(), QByteArray(31, 'c'));
        own.id = u"0123abcd"_s;
        own.askedWhatItIs = false;
        core::Remembered remembered;
        remembered.kvms = {own};
        QVERIFY(settings.remember(remembered).ok());
        secrets.inner.all.insert(LocalWall::secretOf(own.id), savedFor("secret", kvm.stand->address()));
        WallOfATest wall(settings, secrets, quickDriver());
        const core::Status status = settled(wall, own.id);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"cert_invalid"_s);
        QCOMPARE(
            status.errorMessage,
            u"The certificate saved for this device cannot be read. Look at its certificate and trust it again."_s);
        QTest::qWait(200);
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvm.stand->lines, 0);
        QVERIFY(!wall.wasAskedWhatItIs(own.id));
        // Once somebody has looked at what it shows and said yes to that, it is asked under that
        // certificate. (Its password was never its own under what was no certificate.)
        QVERIFY(wall.knowBy(own.id, sha256Of("first")).ok());
        const core::Status known = settled(wall, own.id);
        QCOMPARE(known.state, u"auth"_s);
        QCOMPARE(known.errorCode, u"no_password"_s);
        QVERIFY(wall.wasAskedWhatItIs(own.id));
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
    }

    void aKvmThatSomebodyAddedByHandIsNotAskedByTheWall()
    {
        // Whoever adds a KVM in the program has asked what answers at its address: the wall signs
        // in to it as ever, and asks none of the questions that are for a KVM from a file.
        OwnWall own;
        const MadeUpKvm kvm;
        OwnKvm byHand;
        byHand.address = kvm.stand->address();
        const base::Result<QString> id = own.wall.add(byHand, u"secret"_s);
        QVERIFY(id.ok());
        // (In this run. What is kept of that for the next is another matter: of a KVM over
        // plain HTTP, as this one, nothing. The wall's own tests have that.)
        QVERIFY(own.wall.wasAskedWhatItIs(id.value()));
        QCOMPARE(settled(own.wall, id.value()).state, u"online"_s);
        // (Its driver asks first whether it signs this program in, with nothing to sign in with.)
        QCOMPARE(kvm.stand->asked.constFirst().path, QByteArray(kvmdsQuestion));
        QCOMPARE(kvm.stand->asked.value(1).path, QByteArray(signInQuestion));
        QCOMPARE(kvm.stand->count(jetKvmsQuestion), 0);
        QCOMPARE(kvm.stand->count(redfishsQuestion), 0);
    }

    // --- passwords -----------------------------------------------------------------------------------

    void aPasswordFromAFileIsKeptAsThatOfAKvmThatWasAddedByHand()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QString password = u" a \"password\"\nof two lines "_s + sign(0xE4) + sign(0x1F600);
        const QList<WallExchange::Took> took =
            takenFrom(exchange, {kvmAt(u"http://127.0.0.1:20001"_s, u"Plain"_s, password),
                                 knownBy(kvmAt(u"https://127.0.0.1:20002"_s, u"Pinned"_s, password), "first"),
                                 saying(kvmAt(u"https://127.0.0.1:20003"_s, u"By Windows"_s, password), u"system"_s)});
        QCOMPARE(took.size(), 3);
        // One secret each, under the wall's name for the KVM, saved for its address and for what
        // it is known by: as the wall keeps the password of a KVM that was added by hand.
        QCOMPARE(own.secrets.inner.all.size(), 3);
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(took.at(0).came.value())),
                 savedFor(password.toUtf8(), u"http://127.0.0.1:20001"_s));
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(took.at(1).came.value())),
                 savedFor(password.toUtf8(), u"https://127.0.0.1:20002"_s, sha256Of("first")));
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(took.at(2).came.value())),
                 savedFor(password.toUtf8(), u"https://127.0.0.1:20003"_s));
        // And nowhere else: not in the settings.
        QVERIFY(!settingsText(own).contains("password"));
        QVERIFY(!settingsText(own).contains("of two lines"));
    }

    void aDeviceThatComesWithoutAPasswordIsTakenWithoutAndItsPasswordIsToBeTyped()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        WallExchange exchange(own.wall, quickly());
        // A plain file: it holds no password.
        const base::Result<WallExchange::LaidOut> laidOut =
            laidOutBy(exchange, fileOf({kvmAt(kvm.stand->address(), u"From a plain file"_s)}, false), QString());
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 1);
        QVERIFY(!rows.constFirst().hasPassword);
        QVERIFY(rows.constFirst().canBeTaken);
        QVERIFY(rows.constFirst().says.contains(u"The file has no password for it. It is taken without one, and its "
                                                "password is to be typed on this PC."_s));
        const QString id = theOne(exchange.take(laidOut.value().number(), {1}));
        QVERIFY(!id.isEmpty());
        QVERIFY(own.secrets.inner.all.isEmpty());
        QCOMPARE(own.secrets.inner.keeps, 0);
        // Asked what it is all the same; and then its card asks for its password, as that of a
        // KVM that has none.
        const core::Status status = settled(own.wall, id);
        QCOMPARE(status.state, u"auth"_s);
        QCOMPARE(status.errorCode, u"no_password"_s);
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
        QVERIFY(own.wall.wasAskedWhatItIs(id));
        // Typed in the box that changes a KVM, it is signed in with and kept.
        LocalWall* const forTheBox = own.wall.forABox();
        QVERIFY(forTheBox != nullptr);
        core::KvmEditing editing(*forTheBox, quickBox());
        core::KvmEditing::Typed typed;
        typed.address = kvm.stand->address();
        typed.password = u"secret"_s;
        const core::KvmEditing::Outcome outcome = outcomeOf<core::KvmEditing::Outcome>(
            [&](auto done) { editing.change(id, typed, std::move(done)); }, neverChanged());
        QVERIFY2(outcome.result.ok(), outcome.result.ok() ? "" : outcome.result.error().message.c_str());
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        QCOMPARE(own.secrets.inner.all.value(LocalWall::secretOf(id)), savedFor("secret", kvm.stand->address()));
    }

    void whereWindowsWillNotKeepAPasswordThatDeviceIsNotTakenAndTheOthersAre()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        // The second of the three: Windows does not keep it.
        own.secrets.inner.refusesTheKeep = 2;
        const QList<WallExchange::Took> took = takenFrom(exchange, {kvmAt(u"http://127.0.0.1:20001"_s, u"One"_s),
                                                                    kvmAt(u"http://127.0.0.1:20002"_s, u"Two"_s),
                                                                    kvmAt(u"http://127.0.0.1:20003"_s, u"Three"_s)});
        QCOMPARE(took.size(), 3);
        QCOMPARE(codeOf(took.at(0)), u"taken"_s);
        QCOMPARE(codeOf(took.at(1)), u"not_saved"_s);
        QCOMPARE(sentenceOf(took.at(1)),
                 u"This device could not be saved (Windows did not keep it.), so nothing was changed."_s);
        QCOMPARE(codeOf(took.at(2)), u"taken"_s);
        QCOMPARE(own.wall.kvms().size(), 2);
        QCOMPARE(own.wall.kvms().at(0).name, u"One"_s);
        QCOMPARE(own.wall.kvms().at(1).name, u"Three"_s);
        QCOMPARE(own.secrets.inner.all.size(), 2);
        QVERIFY(!settingsText(own).contains("20002"));
        // It can be taken once Windows keeps it: laid out anew, it is the one that is not there.
        const QList<WallExchange::Took> again = takenFrom(exchange, {kvmAt(u"http://127.0.0.1:20001"_s, u"One"_s),
                                                                     kvmAt(u"http://127.0.0.1:20002"_s, u"Two"_s),
                                                                     kvmAt(u"http://127.0.0.1:20003"_s, u"Three"_s)});
        QCOMPARE(codeOf(again.at(0)), u"duplicate"_s);
        QCOMPARE(codeOf(again.at(1)), u"taken"_s);
        QCOMPARE(codeOf(again.at(2)), u"duplicate"_s);
        QCOMPARE(own.wall.kvms().size(), 3);
    }

    // --- what is laid out ----------------------------------------------------------------------------

    void everythingAPersonHasToBeShownIsLaidOutForEachDevice()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QString password = u"PASSWORD-nobody-is-shown"_s;
        const auto entry = [&password](const QString& address, const QString& name = {}, const QString& tlsWord = {}) {
            QJsonObject device{{u"url"_s, address},
                               {u"kind"_s, u"kvmd"_s},
                               {u"username"_s, u"USER-nobody-is-shown"_s},
                               {u"password"_s, password}};
            if (!name.isEmpty()) {
                device.insert(u"name"_s, name);
            }
            if (!tlsWord.isEmpty()) {
                device.insert(u"tls"_s, tlsWord);
            }
            return device;
        };
        QJsonObject pinned = entry(u"HTTPS://Rack1.Invalid:443/kvm/"_s, u"Rack 1 \"top\""_s, u"pinned"_s);
        pinned.insert(u"cert_pem"_s, pemOf("first"));
        QJsonObject withoutPassword = entry(u"https://192.0.2.50"_s, u"No password"_s, u"system"_s);
        withoutPassword.remove(u"password"_s);
        const QJsonArray devices{pinned,
                                 entry(u"https://xn--mnchen-3ya.invalid"_s, u"M"_s + sign(0xFC) + u"nchen"_s),
                                 entry(u"http://127.0.0.1:8080"_s),
                                 entry(u"https://localhost"_s, u"This PC"_s, u"system"_s),
                                 entry(u"http://169.254.169.254"_s, u"Link-local"_s),
                                 entry(u"https://[fe80::1]:8443"_s, u"Link-local, new kind"_s, u"insecure"_s),
                                 entry(u"ftp://192.0.2.60"_s, u"Not an address"_s),
                                 withoutPassword};
        const QJsonArray actions{QJsonObject{
            {u"name"_s, u"Ctrl+Alt+Del"_s}, {u"kind"_s, u"keys"_s}, {u"keys"_s, u"ControlLeft,AltLeft,Delete"_s}}};
        const base::Result<WallExchange::LaidOut> laidOut =
            laidOutBy(exchange, plainFile({{u"devices"_s, devices}, {u"actions"_s, actions}}), QString());
        QCOMPARE(saidOf(laidOut), u"read"_s);
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 7);
        using Where = WallExchange::Where;
        const QString hasPassword =
            u"The file has its password. It is kept with Windows, for this address and for what the KVM is known by."_s;
        const QString hasNoPassword =
            u"The file has no password for it. It is taken without one, and its password is to be typed on this PC."_s;
        for (const WallExchange::Offered& row : rows) {
            // Every sign of an address is plain, and nothing was asked at any of them: which is
            // a plain value of each, for whoever shows the list to say once.
            QVERIFY(std::ranges::all_of(row.address, [](QChar one) { return one > u' ' && one < QChar(0x7F); }));
            QVERIFY(row.nothingAskedYet);
            QVERIFY(row.canBeTaken);
            // And the last thing that is said of each is whether the file has its password.
            QCOMPARE(row.says.constLast(), row.hasPassword ? hasPassword : hasNoPassword);
        }

        // Its place; its name set off, so that a quote in it does not end that, and its address
        // as it will be kept, in one line.
        QCOMPARE(rows.at(0).place, qsizetype{1});
        QCOMPARE(rows.at(0).address, u"https://rack1.invalid"_s);
        QCOMPARE(rows.at(0).nameAndAddress, u"\"Rack 1 'top'\" at https://rack1.invalid"_s);
        QCOMPARE(rows.at(0).where, Where::Elsewhere);
        QVERIFY(!rows.at(0).nameInXnForm);
        QVERIFY(rows.at(0).hasPassword);
        QCOMPARE(rows.at(0).fingerprint, core::kvm::fingerprint(sha256Of("first")));
        QCOMPARE(rows.at(0).says,
                 (QStringList{u"The file names its certificate. It will be known by that certificate and by no "
                              "other."_s,
                              hasPassword}));
        // A name in its xn-- form stays in that form, and is said to be one.
        QCOMPARE(rows.at(1).address, u"https://xn--mnchen-3ya.invalid"_s);
        QCOMPARE(rows.at(1).nameAndAddress, u"\"M"_s + sign(0xFC) + u"nchen\" at https://xn--mnchen-3ya.invalid"_s);
        QVERIFY(rows.at(1).nameInXnForm);
        QVERIFY(rows.at(1).says.contains(u"Its address is a name in its xn-- form, which stands for letters beyond the "
                                         "twenty-six. Such a name can read as another name does: compare the address "
                                         "sign for sign."_s));
        // One without a name has none, and is laid out by its address alone; this PC's own
        // address is said, and that its password goes in the clear.
        QCOMPARE(rows.at(2).nameAndAddress, u"http://127.0.0.1:8080"_s);
        QCOMPARE(rows.at(2).where, Where::ThisPc);
        QVERIFY(rows.at(2).passwordInTheClear);
        QCOMPARE(rows.at(2).says.constFirst(),
                 u"Its address is this PC itself: whatever listens there on this PC would be taken for this KVM."_s);
        QCOMPARE(rows.at(3).where, Where::ThisPc);
        QCOMPARE(rows.at(3).knownBy, WallExchange::KnownBy::WindowsWord);
        // And one of the kind that is not routed, written either way.
        const QString notRouted =
            u"Its address is of the kind that no router passes on (169.254.x.x, fe80::): it means whatever has that "
            "number on the network this PC is plugged into."_s;
        QCOMPARE(rows.at(4).where, Where::NotRouted);
        QCOMPARE(rows.at(4).says.constFirst(), notRouted);
        QCOMPARE(rows.at(5).where, Where::NotRouted);
        QCOMPARE(rows.at(5).address, u"https://[fe80::1]:8443"_s);
        QCOMPARE(rows.at(5).says.constFirst(), notRouted);
        QVERIFY(rows.at(5).notByWhatItShowsFirst);
        // What the reader left out has its place still: the one behind it is the eighth.
        QCOMPARE(rows.at(6).place, qsizetype{8});
        QVERIFY(!rows.at(6).hasPassword);
        QCOMPARE(rows.at(6).where, Where::Elsewhere);

        // Beside the list: every sentence of what the reader left out, and what else the file
        // holds that is not taken.
        QCOMPARE(laidOut.value().besideTheList(),
                 (QStringList{u"Device 7 (\"Not an address\"): its address does not begin with http:// or https://"_s,
                              u"This file contains secrets without a passphrase. Keep it somewhere private."_s}));
        // No password is in any of it, and nobody's user name.
        const QString everyWord = everyWordOf(laidOut.value());
        QVERIFY(!everyWord.contains(u"PASSWORD"_s));
        QVERIFY(!everyWord.contains(u"nobody-is-shown"_s));
        QVERIFY(!everyWord.contains(u"USER"_s));
    }

    void whatIsSoOfEveryDeviceIsInNoSentenceOfOneAndACertificatesSha256IsInNone()
    {
        // A list of many devices is read by a person: what is said of each is what is so of
        // that one. That nothing was asked at its address yet is so of every device of every
        // file: a plain value of each, in no sentence, for whoever shows the list to say once.
        // And the SHA-256 of a certificate stands where it is compared (`fingerprint`), whole,
        // and not once more inside a sentence.
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        // (At addresses that reach nothing by the books: they are laid out, and never taken.)
        const QByteArray file = fileOf({knownBy(kvmAt(u"https://192.0.2.1"_s, u"Pinned"_s), "first"),
                                        saying(kvmAt(u"https://192.0.2.2"_s, u"By Windows"_s), u"system"_s),
                                        kvmAt(u"http://192.0.2.3"_s, u"Plain"_s)});
        const QList<WallExchange::Offered> rows = rowsOf(laidOutBy(exchange, file));
        QCOMPARE(rows.size(), 3);
        const QString fingerprint = core::kvm::fingerprint(sha256Of("first"));
        QCOMPARE(rows.at(0).fingerprint, fingerprint);
        for (const WallExchange::Offered& row : rows) {
            QVERIFY(row.canBeTaken);
            QVERIFY(row.nothingAskedYet);
            // How it will be known, and whether the file has its password: two sentences, and
            // no third.
            QCOMPARE(row.says.size(), 2);
            const QString said = row.verdict + u' ' + row.says.join(u' ');
            QVERIFY2(!said.contains(u"Nothing was asked"_s), qPrintable(said));
            QVERIFY2(!said.contains(u"asks what answers"_s), qPrintable(said));
            QVERIFY2(!said.contains(u"SHA-256"_s), qPrintable(said));
            QVERIFY2(!said.contains(fingerprint.left(11)), qPrintable(said));
        }
    }

    void aDeviceAtTheAddressOfAKeptKvmCannotBeTakenAndSaysWhy()
    {
        // The settings keep a KVM that another build added, which this one does not read, at the
        // address a device of the file is at: that device cannot be taken, and says so in its own
        // words, not as one that is on the wall.
        const QJsonObject kept{
            {u"id"_s, u"0c000009"_s}, {u"kind"_s, u"amt"_s}, {u"address"_s, u"https://127.0.0.1:20777"_s}};
        OwnWall own(QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{kept}}}).toJson());
        QVERIFY(own.wall.kvms().isEmpty());
        QCOMPARE(own.wall.keptAside().size(), 1);
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = fileOf({kvmAt(u"https://127.0.0.1:20777"_s, u"At the kept one's place"_s),
                                        kvmAt(u"http://127.0.0.1:20778"_s, u"Elsewhere"_s)});
        const QList<WallExchange::Offered> rows = rowsOf(laidOutBy(exchange, file));
        QCOMPARE(rows.size(), 2);
        QVERIFY(!rows.at(0).canBeTaken);
        QCOMPARE(rows.at(0).whyNot, u"duplicate"_s);
        QVERIFY2(rows.at(0).verdict.contains(u"another version of qKVM"_s), qPrintable(rows.at(0).verdict));
        QVERIFY2(!rows.at(0).verdict.contains(u"on the wall already"_s), qPrintable(rows.at(0).verdict));
        QVERIFY(rows.at(1).canBeTaken);
    }

    void aDeviceThatCannotBeTakenSaysWhyAndNothingOfHowItWouldBeKnownOrOfItsPassword()
    {
        OwnWall own;
        OwnKvm byHand;
        byHand.address = u"https://127.0.0.1:20443"_s;
        byHand.name = u"There already"_s;
        QVERIFY(own.wall.add(byHand, u"secret"_s).ok());
        WallExchange exchange(own.wall, quickly());
        // Three that cannot be taken, each for another reason: one over plain HTTP with what this wall
        // does not keep; one that is on the wall already, with its certificate named; and one at a name in
        // its xn-- form, with what this wall does not keep. And one that can, over plain HTTP at this PC.
        const Device plainNotKept = notKeptHere(kvmAt(u"http://127.0.0.1:20002"_s, u"A small one"_s));
        const Device notKept =
            notKeptHere(saying(kvmAt(u"https://xn--mnchen-3ya.invalid"_s, u"A server"_s), u"system"_s));
        const QByteArray file = fileOf({plainNotKept, knownBy(kvmAt(u"https://127.0.0.1:20443"_s, u"Twice"_s), "first"),
                                        notKept, kvmAt(u"http://127.0.0.1:20005"_s, u"The one"_s)});
        const QList<WallExchange::Offered> rows = rowsOf(laidOutBy(exchange, file));
        QCOMPARE(rows.size(), 4);
        // (The one that can be taken: where its address is, how it is spoken to, its password.)
        QVERIFY(rows.at(3).canBeTaken);
        QCOMPARE(rows.at(3).says.size(), 3);
        QVERIFY(rows.at(3).says.at(1).startsWith(u"It is spoken to over plain HTTP"_s));
        QVERIFY(rows.at(3).says.at(2).startsWith(u"The file has its password."_s));
        for (const qsizetype at : {0, 1, 2}) {
            const WallExchange::Offered& row = rows.at(at);
            QVERIFY(!row.canBeTaken);
            // Why, in its verdict; and its plain values are what they are.
            QVERIFY2(row.verdict.startsWith(u"It cannot be taken: "_s), qPrintable(row.verdict));
            QVERIFY(row.hasPassword);
            QVERIFY(row.nothingAskedYet);
            // Nothing of how it would be known, nor of its password: neither comes to pass.
            const QString said = row.says.join(u' ');
            for (const QString& word :
                 {u"password"_s, u"known by"_s, u"certificate"_s, u"plain HTTP"_s, u"Windows"_s}) {
                QVERIFY2(!said.contains(word, Qt::CaseInsensitive), qPrintable(said));
            }
        }
        QCOMPARE(rows.at(0).whyNot, u"invalid"_s);
        QCOMPARE(rows.at(0).says.size(), 1);
        QVERIFY(rows.at(0).says.constFirst().startsWith(u"Its address is this PC itself"_s));
        QVERIFY(rows.at(0).passwordInTheClear);
        QCOMPARE(rows.at(1).whyNot, u"duplicate"_s);
        QCOMPARE(rows.at(1).says.size(), 1);
        QCOMPARE(rows.at(1).fingerprint, core::kvm::fingerprint(sha256Of("first")));
        // Where its address is, is said of it all the same: that is of the address a person
        // reads beside it.
        QCOMPARE(rows.at(2).whyNot, u"invalid"_s);
        QVERIFY(rows.at(2).nameInXnForm);
        QCOMPARE(rows.at(2).says.size(), 1);
        QVERIFY(rows.at(2).says.constFirst().startsWith(u"Its address is a name in its xn-- form"_s));
    }

    void theListOfDevicesIsHandedOutOnlyForTheWordThatWhatStandsBesideItWasLookedAt()
    {
        static_assert(HandsOutItsListForTheWord<WallExchange::LaidOut>);
        static_assert(!HandsOutItsListUnasked<WallExchange::LaidOut>);
        static_assert(!HasItsListAsAMember<WallExchange::LaidOut>);
        static_assert(!HandsOutItsListForEmptyBraces<WallExchange::LaidOut>);
        static_assert(!IsSaidByEmptyBraces<WallExchange::BesideTheList>);
        // And a file that was written comes in the writer's own shape, which hands out its text
        // only for the word that the lines beside it were looked at (core/tests of exchange).
        static_assert(std::is_invocable_v<decltype(&WallExchange::write), WallExchange&, const QString&, QObject*,
                                          std::function<void(base::Result<Written>)>>);
        // A laying-out that was made of nothing has nothing in it, and names none that was made.
        const WallExchange::LaidOut none;
        QCOMPARE(none.number(), quint64{0});
        QVERIFY(none.devices(WallExchange::BesideTheList::wasLookedAt).isEmpty());
    }

    void whatAFileOfNoDevicesHoldsIsSaidBesideTheList()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, exampleFile("sealed-alerts-ntfy.qkvm"));
        QCOMPARE(saidOf(laidOut), u"read"_s);
        QVERIFY(rowsOf(laidOut).isEmpty());
        QCOMPARE(
            laidOut.value().besideTheList(),
            (QStringList{
                u"This file holds no devices."_s,
                u"Server alert settings are not supported on this PC. They stay in the file; Windows notifications are unchanged."_s}));
    }

    void aFileThatIsRefusedIsRefusedInTheReadersWordsAndNothingIsLaidOut()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        QCOMPARE(saidOf(laidOutBy(exchange, exampleFile("sealed-quick.qkvm"), u"another passphrase"_s)),
                 u"wrong_passphrase: That is not the passphrase this file was made with"_s);
        QCOMPARE(saidOf(laidOutBy(exchange, exampleFile("sealed-quick.qkvm"), QString())),
                 u"passphrase_needed: This file is encrypted. Enter the passphrase it was made with."_s);
        QCOMPARE(saidOf(laidOutBy(exchange, "{}"_ba)), u"not_a_file: This is not a file that qKVM made"_s);
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void whatThisPcsWallDoesNotKeepIsNotTaken()
    {
        // What a file holds is counted in signs, and what this PC's wall keeps in the places a
        // text takes here: a name of eighty signs from far up is one a file holds and this wall
        // would cut in two at the next start. It is not taken, and that is said.
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const Device longName = kvmAt(u"http://127.0.0.1:20001"_s, core::testing::wideSigns(60));
        const base::Result<WallExchange::LaidOut> laidOut =
            laidOutBy(exchange, fileOf({longName, kvmAt(u"http://127.0.0.1:20002"_s, u"Fits"_s)}));
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 2);
        QVERIFY(!rows.at(0).canBeTaken);
        QCOMPARE(rows.at(0).whyNot, u"invalid"_s);
        QCOMPARE(rows.at(0).verdict,
                 u"It cannot be taken: this PC's wall does not keep what the file has for it (name: this may have 80 "
                 "characters at the most)."_s);
        QVERIFY(rows.at(1).canBeTaken);
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), {1, 2});
        QCOMPARE(codeOf(took.at(0)), u"invalid"_s);
        QCOMPARE(codeOf(took.at(1)), u"taken"_s);
        QCOMPARE(own.wall.kvms().size(), 1);
    }

    // --- all or each ---------------------------------------------------------------------------------

    void eachDeviceIsTakenOrNotAndWhatCameOfEachIsHandedBack()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const Device notKept = notKeptHere(kvmAt(u"http://127.0.0.1:20002"_s, u"A server"_s));
        const QByteArray file = fileOf(
            {kvmAt(u"http://127.0.0.1:20001"_s, u"One"_s), notKept, kvmAt(u"https://127.0.0.1:20003"_s, u"Three"_s),
             kvmAt(u"http://127.0.0.1:20003"_s, u"Three again"_s), kvmAt(u"http://127.0.0.1:20005"_s, u"Five"_s)});
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        // Named in another order than the file's, and one that is nobody's among them.
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), {5, 9, 4, 3, 2, 1});
        QStringList came;
        for (const WallExchange::Took& one : took) {
            came.append(u"%1 %2"_s.arg(one.place).arg(codeOf(one)));
        }
        QCOMPARE(came, (QStringList{u"1 taken"_s, u"2 invalid"_s, u"3 taken"_s, u"4 duplicate"_s, u"5 taken"_s,
                                    u"9 not_laid_out"_s}));
        // On the wall in the file's order, behind what was there; and the wall has said once
        // that it changed.
        QStringList names;
        for (const OwnKvm& kvm : own.wall.kvms()) {
            names.append(kvm.name);
        }
        QCOMPARE(names, (QStringList{u"One"_s, u"Three"_s, u"Five"_s}));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(core::Settings(own.file).remembered().kvms, own.wall.kvms());
    }

    void whereTheListCannotBeWrittenDownNoneIsTakenAndNothingIsKept()
    {
        // Where the settings should be, there is a folder: nothing can be written there.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QVERIFY(QDir().mkpath(file));
        core::Settings settings(file);
        WatchedSecrets secrets;
        WallOfATest wall(settings, secrets, quickDriver());
        WallExchange exchange(wall, quickly());
        const QSignalSpy changed(&wall, &LocalWall::changed);
        const QList<WallExchange::Took> took = takenFrom(
            exchange, {kvmAt(u"http://127.0.0.1:20001"_s), knownBy(kvmAt(u"https://127.0.0.1:20002"_s), "first"),
                       kvmAt(u"http://127.0.0.1:20003"_s)});
        QCOMPARE(took.size(), 3);
        for (const WallExchange::Took& one : took) {
            QCOMPARE(codeOf(one), u"not_saved"_s);
            QVERIFY(sentenceOf(one).startsWith(u"The devices from the file could not be saved ("_s));
        }
        QVERIFY(wall.kvms().isEmpty());
        QVERIFY(settings.remembered().kvmCertificates.isEmpty());
        // Their passwords were kept first, and were taken out of the store again.
        QCOMPARE(secrets.inner.keeps, 3);
        QVERIFY(secrets.inner.all.isEmpty());
        QCOMPARE(changed.count(), 0);
    }

    void aFileOfFiftyDevicesIsTakenAtOnceAndASecondTimeNot()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        QList<Device> devices;
        for (int number = 0; number < 50; ++number) {
            devices.append(kvmAt(u"http://127.0.0.1:%1"_s.arg(21000 + number), u"KVM %1"_s.arg(number + 1)));
        }
        const QByteArray file = fileOf(devices);
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        const QList<qsizetype> places = placesOf(rowsOf(laidOut));
        QCOMPARE(places.size(), 50);
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        for (const WallExchange::Took& one : exchange.take(laidOut.value().number(), places)) {
            QCOMPARE(codeOf(one), u"taken"_s);
        }
        QCOMPARE(own.wall.kvms().size(), 50);
        QCOMPARE(own.secrets.inner.all.size(), 50);
        QCOMPARE(changed.count(), 1);
        for (const WallExchange::Took& one : takenFrom(exchange, devices)) {
            QCOMPARE(codeOf(one), u"duplicate"_s);
        }
        QCOMPARE(own.wall.kvms().size(), 50);
        QCOMPARE(changed.count(), 1);
    }

    void aWallThatIsFullTakesNoMore()
    {
        OwnWall own;
        // Two short of full, by hand.
        core::Remembered full;
        for (int number = 0; number < core::Remembered::mostKvms - 2; ++number) {
            OwnKvm kvm;
            kvm.id = u"%1"_s.arg(number, 8, 16, QLatin1Char('0'));
            kvm.address = u"https://192.0.2.1:%1"_s.arg(1024 + number);
            kvm.askedWhatItIs = true;
            full.kvms.append(kvm);
        }
        QVERIFY(own.settings.remember(full).ok());
        WallOfATest wall(own.settings, own.secrets, quickDriver());
        WallExchange exchange(wall, quickly());
        const base::Result<WallExchange::LaidOut> laidOut =
            laidOutBy(exchange, fileOf({kvmAt(u"http://127.0.0.1:20001"_s), kvmAt(u"http://127.0.0.1:20002"_s),
                                        kvmAt(u"http://127.0.0.1:20003"_s)}));
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QVERIFY(rows.at(0).canBeTaken && rows.at(1).canBeTaken);
        QVERIFY(!rows.at(2).canBeTaken);
        QCOMPARE(rows.at(2).whyNot, u"too_many"_s);
        QCOMPARE(rows.at(2).verdict, u"It cannot be taken: this PC's wall is full."_s);
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), {1, 2, 3});
        QCOMPARE(codeOf(took.at(2)), u"too_many"_s);
        QCOMPARE(wall.kvms().size(), core::Remembered::mostKvms);
    }

    // --- the certificate itself ----------------------------------------------------------------------

    void theWallKeepsACertificateItselfFromWhenTheKvmNextShowsTheOneItIsKnownBy()
    {
        OwnWall own;
        const MadeUpKvm kvm(tls("first"));
        // Added by hand: known by the fingerprint of what it showed, and by no more than that.
        const base::Result<QString> id = own.wall.add(knownAt(kvm.stand->address(), sha256Of("first")), u"secret"_s);
        QVERIFY(id.ok());
        QVERIFY(own.wall.certificateOf(id.value()).isEmpty());
        QVERIFY(!settingsText(own).contains("certificate_pem"));
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        // It is looked at, and shows that certificate: from then on the wall has it.
        QCOMPARE(settled(own.wall, id.value()).state, u"online"_s);
        QTRY_COMPARE(own.wall.certificateOf(id.value()), keptPemOf("first"));
        // (Written down a short while later, with what else the wall records by itself.)
        QTRY_VERIFY(settingsText(own).contains("certificate_pem"));
        QVERIFY(settingsText(own).contains("-----BEGIN CERTIFICATE-----"));
        // (The wall is the one it was: nothing is said of that.)
        QCOMPARE(changed.count(), 0);
        // It is there at the next start, and what the KVM is known by is what it was.
        const core::Settings settings(own.file);
        QCOMPARE(settings.remembered().kvmCertificates.value(id.value()), keptPemOf("first"));
        QCOMPARE(settings.remembered().kvms.constFirst().certificate, sha256Of("first"));
        QCOMPARE(settings.remembered().kvms.constFirst(), kvmOf(own.wall, id.value()));
    }

    void theCertificateItselfIsNeverAnotherThanTheOneTheKvmIsKnownBy()
    {
        OwnWall own;
        // Known by one certificate, and what answers shows another: nothing of that is kept.
        const MadeUpKvm other(tls("second"));
        const base::Result<QString> away =
            own.wall.add(knownAt(other.stand->address(), sha256Of("first")), u"secret"_s);
        QVERIFY(away.ok());
        QCOMPARE(settled(own.wall, away.value()).errorCode, u"cert_mismatch"_s);
        QTest::qWait(200);
        QVERIFY(own.wall.certificateOf(away.value()).isEmpty());
        QVERIFY(own.settings.remembered().kvmCertificates.isEmpty());

        // One that is kept goes when the KVM comes to be known by another certificate: what is
        // kept beside a fingerprint is the certificate that has it, or nothing.
        QVERIFY(own.wall.remove(away.value()).ok());
        const MadeUpKvm kvm(tls("first"));
        const base::Result<QString> known = own.wall.add(knownAt(kvm.stand->address(), sha256Of("first")), u"secret"_s);
        QVERIFY(known.ok());
        QCOMPARE(settled(own.wall, known.value()).state, u"online"_s);
        QTRY_COMPARE(own.wall.certificateOf(known.value()), keptPemOf("first"));
        QVERIFY(own.wall.knowBy(known.value(), sha256Of("second")).ok());
        QVERIFY(own.wall.certificateOf(known.value()).isEmpty());
        QVERIFY(!settingsText(own).contains("certificate_pem"));
        // (It shows the first still, which is not what it is known by now: none is kept.)
        QCOMPARE(settled(own.wall, known.value()).errorCode, u"cert_mismatch"_s);
        QTest::qWait(200);
        QVERIFY(own.wall.certificateOf(known.value()).isEmpty());
        // Known by the first again, by somebody's yes, it gains it again when it shows it.
        QVERIFY(own.wall.knowBy(known.value(), sha256Of("first")).ok());
        QCOMPARE(settled(own.wall, known.value()).state, u"online"_s);
        QTRY_COMPARE(own.wall.certificateOf(known.value()), keptPemOf("first"));
        // And it goes with the KVM when that is taken off the wall.
        QVERIFY(own.wall.remove(known.value()).ok());
        QVERIFY(own.settings.remembered().kvmCertificates.isEmpty());
        QVERIFY(!settingsText(own).contains("certificate_pem"));
    }

    void aCertificateOfAFileThatIsNotTheOneTheKvmIsToBeKnownByIsNotKept()
    {
        // Whoever hands the wall a KVM from a file with a certificate that does not have the
        // fingerprint it is to be known by: the KVM is taken, by the fingerprint, and the
        // certificate is dropped.
        OwnWall own;
        LocalWall::FromAFile one;
        one.kvm.address = u"https://127.0.0.1:20001"_s;
        one.kvm.knownByCertificate = true;
        one.kvm.certificate = sha256Of("first");
        one.certificatePem = keptPemOf("second");
        LocalWall::FromAFile damaged = one;
        damaged.kvm.address = u"https://127.0.0.1:20002"_s;
        damaged.kvm.certificate = sha256Of("second");
        damaged.certificatePem = keptPemOf("second").replace(40, 6, u"!!!!!!"_s);
        LocalWall::FromAFile fits = one;
        fits.kvm.address = u"https://127.0.0.1:20003"_s;
        fits.kvm.certificate = sha256Of("comet");
        fits.certificatePem = pemOf("comet");
        const QList<base::Result<QString>> came = own.wall.takeFromAFile({one, damaged, fits});
        QVERIFY(came.at(0).ok() && came.at(1).ok() && came.at(2).ok());
        QVERIFY(own.wall.certificateOf(came.at(0).value()).isEmpty());
        QVERIFY(own.wall.certificateOf(came.at(1).value()).isEmpty());
        QCOMPARE(own.wall.certificateOf(came.at(2).value()), keptPemOf("comet"));
        QCOMPARE(kvmOf(own.wall, came.at(0).value()).certificate, sha256Of("first"));
        QCOMPARE(own.settings.remembered().kvmCertificates.size(), 1);
    }

    // --- writing the wall to a file ----------------------------------------------------------------

    void theWallIsWrittenAsAFileHoldsItAndReadBackAsItIs()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        // A wall of every kind of KVM: by hand over plain HTTP, with a user name and a group;
        // from a file with its certificate; by Windows' word; and one that came with no password.
        OwnKvm plain;
        plain.address = u"http://127.0.0.1:20001"_s;
        plain.name = u"Plain \"one\""_s;
        plain.user = u"operator"_s;
        plain.group = u"Rack 2"_s;
        QVERIFY(own.wall.add(plain, u"a password of its own"_s).ok());
        const QList<WallExchange::Took> took = takenFrom(
            exchange, {knownBy(kvmAt(u"https://127.0.0.1:20002"_s, u"Pinned"_s, u"second password"_s), "first"),
                       saying(kvmAt(u"https://kvm.invalid"_s, u"By Windows"_s, u"third password"_s), u"system"_s),
                       kvmAt(u"http://127.0.0.1:20004"_s, u"No password"_s, QString())});
        QCOMPARE(took.size(), 3);
        QCOMPARE(own.wall.kvms().size(), 4);

        const base::Result<Written> written = writtenBy(exchange, thePassphrase());
        QCOMPARE(saidOf(written), u"read"_s);
        // Beside the file: what is not this PC's yet, in one line; and the KVM that has no
        // password, by its place on the wall and its name.
        QCOMPARE(
            written.value().notInIt(),
            (QStringList{
                u"Server alert settings and this PC's accounts, app password, window layout and preferences are not in this file."_s,
                u"Device 4 of this wall (\"No password\"): its password is not in this file. Windows "
                "did not hand it out, or none is kept for this device as it is."_s}));
        const QByteArray file = written.value().file(core::exchange::NotInIt::wasLookedAt);
        // Sealed: nothing of the wall can be read off it.
        for (const char* word : {"127.0.0.1", "Pinned", "password", "CERTIFICATE", "kvm.invalid"}) {
            QVERIFY2(!file.contains(word), word);
        }
        const base::Result<Content> read = core::exchange::read(file, thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        QCOMPARE(read.value().leftOut, QStringList());
        QVERIFY(read.value().sealed);
        // No actions on this wall; shared settings are portable, server alerts are not.
        QVERIFY(read.value().actions.has_value());
        QVERIFY(read.value().actions.value_or(QList<core::exchange::Action>()).isEmpty());
        QVERIFY(!read.value().alerts.has_value());
        QCOMPARE(read.value().wall, std::optional<core::exchange::WallSettings>(own.wall.portableSettings()));
        const QList<Device> devices = core::testing::devicesIn(read.value());
        QCOMPARE(devices.size(), 4);
        for (const Device& device : devices) {
            QCOMPARE(device.kind, u"kvmd"_s);
        }
        QCOMPARE(devices.at(0).name, u"Plain \"one\""_s);
        QCOMPARE(devices.at(0).address, u"http://127.0.0.1:20001"_s);
        QCOMPARE(devices.at(0).user, u"operator"_s);
        QCOMPARE(devices.at(0).group, u"Rack 2"_s);
        QCOMPARE(devices.at(0).password, u"a password of its own"_s);
        QCOMPARE(devices.at(0).tls, u"pinned"_s);
        QVERIFY(devices.at(0).certificatePem.isEmpty());
        // Pinned, with the certificate itself.
        QCOMPARE(devices.at(1).address, u"https://127.0.0.1:20002"_s);
        QCOMPARE(devices.at(1).tls, u"pinned"_s);
        QCOMPARE(devices.at(1).certificatePem, keptPemOf("first"));
        QCOMPARE(devices.at(1).certificate, sha256Of("first"));
        QCOMPARE(devices.at(1).password, u"second password"_s);
        // Windows' word.
        QCOMPARE(devices.at(2).address, u"https://kvm.invalid"_s);
        QCOMPARE(devices.at(2).tls, u"system"_s);
        QVERIFY(devices.at(2).certificatePem.isEmpty());
        QCOMPARE(devices.at(2).password, u"third password"_s);
        QVERIFY(devices.at(3).password.isEmpty());

        // And what was written is taken by another wall as it was: the same KVMs, known by the
        // same, with the same passwords kept.
        OwnWall other;
        WallExchange othersExchange(other.wall, quickly());
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(othersExchange, file);
        QCOMPARE(saidOf(laidOut), u"read"_s);
        QCOMPARE(laidOut.value().besideTheList(), QStringList());
        for (const WallExchange::Took& one : othersExchange.take(laidOut.value().number(), placesOf(rowsOf(laidOut)))) {
            QCOMPARE(codeOf(one), u"taken"_s);
        }
        QCOMPARE(other.wall.kvms().size(), 4);
        for (qsizetype index = 0; index < 4; ++index) {
            const OwnKvm& was = own.wall.kvms().at(index);
            const OwnKvm& is = other.wall.kvms().at(index);
            QCOMPARE(is.name, was.name);
            QCOMPARE(is.address, was.address);
            QCOMPARE(is.user, was.user);
            QCOMPARE(is.group, was.group);
            QCOMPARE(is.knownByCertificate, was.knownByCertificate);
            QCOMPARE(is.certificate, was.certificate);
            QCOMPARE(other.wall.certificateOf(is.id), own.wall.certificateOf(was.id));
            const QByteArray kept = own.secrets.inner.all.value(LocalWall::secretOf(was.id));
            QCOMPARE(other.secrets.inner.all.value(LocalWall::secretOf(is.id)), kept);
        }
    }

    void aPlainFileHoldsNoPasswordAndNoneIsReadFromTheStoreForIt()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        OwnKvm plain;
        plain.address = u"http://127.0.0.1:20001"_s;
        plain.name = u"Plain"_s;
        QVERIFY(own.wall.add(plain, u"made-up-password"_s).ok());
        QVERIFY(
            own.wall.add(knownAt(u"https://127.0.0.1:20002"_s, sha256Of("first"), u"Pinned"_s), u"made-up-two"_s).ok());
        const int readBefore = own.secrets.reads;
        const base::Result<Written> written = writtenBy(exchange, QString());
        QCOMPARE(saidOf(written), u"read"_s);
        // No password was asked of the store for it.
        QCOMPARE(own.secrets.reads, readBefore);
        const QByteArray file = written.value().file(core::exchange::NotInIt::wasLookedAt);
        QVERIFY(!file.contains("made-up"));
        QVERIFY(!file.contains("password"));
        QVERIFY(file.contains("\"http://127.0.0.1:20001\""));
        const base::Result<Content> read = core::exchange::read(file, QString());
        QCOMPARE(saidOf(read), u"read"_s);
        QVERIFY(!read.value().sealed);
        QCOMPARE(core::testing::devicesIn(read.value()).size(), 2);
        // Beside it: what is not this PC's yet; and the KVM that has not shown its certificate
        // since it was known by it, of which only the fingerprint is known (the writer's line).
        QCOMPARE(
            written.value().notInIt(),
            (QStringList{
                u"Server alert settings and this PC's accounts, app password, window layout and preferences are not in this file."_s,
                u"Device 2 of this wall (\"Pinned\"): only the fingerprint of its certificate is known "
                "here, and a file holds a certificate itself or none. A qKVM server that reads this "
                "file will take the first certificate this device shows."_s}));
        // A sealed one reads the store once for each KVM.
        const base::Result<Written> sealed = writtenBy(exchange, thePassphrase());
        QCOMPARE(saidOf(sealed), u"read"_s);
        QCOMPARE(own.secrets.reads, readBefore + 2);
    }

    void aPasswordThatWindowsWillNotHandOutIsLeftOutAndThatIsSaidBesideTheFile()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        OwnKvm one;
        one.address = u"http://127.0.0.1:20001"_s;
        one.name = u"One"_s;
        const QString first = own.wall.add(one, u"first password"_s).value();
        OwnKvm two;
        two.address = u"http://127.0.0.1:20002"_s;
        const QString second = own.wall.add(two, u"second password"_s).value();
        OwnKvm three;
        three.address = u"http://127.0.0.1:20003"_s;
        three.name = u"Three"_s;
        QVERIFY(own.wall.add(three, u"third password"_s).ok());
        // Of the first, what the store holds was saved for another address; of the second,
        // nothing is there.
        own.secrets.inner.all.insert(LocalWall::secretOf(first), savedFor("first password", u"http://127.0.0.1:9"_s));
        own.secrets.inner.all.remove(LocalWall::secretOf(second));
        const base::Result<Written> written = writtenBy(exchange, thePassphrase());
        QCOMPARE(saidOf(written), u"read"_s);
        const QString without = u": its password is not in this file. Windows did not hand it out, or none is kept "
                                "for this device as it is."_s;
        QCOMPARE(
            written.value().notInIt(),
            (QStringList{
                u"Server alert settings and this PC's accounts, app password, window layout and preferences are not in this file."_s,
                u"Device 1 of this wall (\"One\")"_s + without, u"Device 2 of this wall"_s + without}));
        const base::Result<Content> read =
            core::exchange::read(written.value().file(core::exchange::NotInIt::wasLookedAt), thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        const QList<Device> devices = core::testing::devicesIn(read.value());
        QCOMPARE(devices.size(), 3);
        QVERIFY(devices.at(0).password.isEmpty());
        QVERIFY(devices.at(1).password.isEmpty());
        QCOMPARE(devices.at(2).password, u"third password"_s);

        // And where Windows hands out nothing at all, every KVM is written without, and said.
        own.secrets.handsNothingOut = true;
        const base::Result<Written> none = writtenBy(exchange, thePassphrase());
        QCOMPARE(saidOf(none), u"read"_s);
        QCOMPARE(none.value().notInIt().size(), 4);
    }

    void untilAKvmHasShownItsCertificateAgainWritingSaysSoBesideTheFile()
    {
        OwnWall own;
        const MadeUpKvm kvm(tls("first"));
        WallExchange exchange(own.wall, quickly());
        OwnKvm known = knownAt(kvm.stand->address(), sha256Of("first"), u"Known by its fingerprint"_s);
        const QString id = own.wall.add(known, u"secret"_s).value();
        const QString fingerprintAlone =
            u"Device 1 of this wall (\"Known by its fingerprint\"): only the fingerprint of its certificate is known "
            "here, and a file holds a certificate itself or none. A qKVM server that reads this file will take the "
            "first certificate this device shows."_s;
        base::Result<Written> written = writtenBy(exchange, thePassphrase());
        QCOMPARE(saidOf(written), u"read"_s);
        QVERIFY(written.value().notInIt().contains(fingerprintAlone));
        base::Result<Content> read =
            core::exchange::read(written.value().file(core::exchange::NotInIt::wasLookedAt), thePassphrase());
        QVERIFY(core::testing::devicesIn(read.value()).constFirst().certificatePem.isEmpty());
        // It shows its certificate again: from then on the file holds it, and nothing is said.
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        QTRY_VERIFY(!own.wall.certificateOf(id).isEmpty());
        written = writtenBy(exchange, thePassphrase());
        QCOMPARE(saidOf(written), u"read"_s);
        QCOMPARE(written.value().notInIt().size(), 1);
        read = core::exchange::read(written.value().file(core::exchange::NotInIt::wasLookedAt), thePassphrase());
        QCOMPARE(core::testing::devicesIn(read.value()).constFirst().certificatePem, keptPemOf("first"));
        QCOMPARE(core::testing::devicesIn(read.value()).constFirst().certificate, sha256Of("first"));
    }

    void anEmptyWallIsWrittenAsAFileOfNoDevices()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const base::Result<Written> written = writtenBy(exchange, QString());
        QCOMPARE(saidOf(written), u"read"_s);
        // The one line is beside every file, this one too.
        QCOMPARE(written.value().notInIt().size(), 1);
        const base::Result<Content> read =
            core::exchange::read(written.value().file(core::exchange::NotInIt::wasLookedAt), QString());
        QCOMPARE(saidOf(read), u"read"_s);
        QVERIFY(read.value().devices.has_value());
        QVERIFY(core::testing::devicesIn(read.value()).isEmpty());
    }

    void aPassphraseThatIsNoneSealsNothing()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        OwnKvm one;
        one.address = u"http://127.0.0.1:20001"_s;
        QVERIFY(own.wall.add(one, u"secret"_s).ok());
        const int readBefore = own.secrets.reads;
        const base::Result<Written> written = writtenBy(exchange, u"too short"_s);
        QCOMPARE(saidOf(written), u"invalid: A passphrase has at least 10 characters."_s);
        // Refused before anything was read from the store for it.
        QCOMPARE(own.secrets.reads, readBefore);
        QVERIFY(!exchange.isBusy());
    }

    // --- off the interface thread ------------------------------------------------------------------

    void aFileIsWorkedOnOffTheInterfaceThreadAndWhoeverAskedMayBeGone()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = fileOf({kvmAt(u"http://127.0.0.1:20001"_s)});
        // Told on the interface thread, and later than the call.
        QObject asker;
        QThread* toldOn = nullptr;
        int told = 0;
        exchange.layOut(file, thePassphrase(), &asker, [&](const base::Result<WallExchange::LaidOut>& laidOut) {
            QVERIFY(laidOut.ok());
            toldOn = QThread::currentThread();
            ++told;
        });
        QVERIFY(exchange.isBusy());
        QCOMPARE(told, 0);
        // One file at a time: a second is refused, and told so later too.
        int refused = 0;
        exchange.layOut(file, thePassphrase(), &asker, [&](const base::Result<WallExchange::LaidOut>& laidOut) {
            QCOMPARE(saidOf(laidOut), u"busy: Another file is being worked on. Try again in a moment."_s);
            ++refused;
        });
        QCOMPARE(refused, 0);
        QTRY_COMPARE(told, 1);
        QTRY_COMPARE(refused, 1);
        QCOMPARE(toldOn, QThread::currentThread());
        QVERIFY(!exchange.isBusy());

        // Whoever asked is gone before the work has ended: nobody is told, nothing is laid out to
        // anybody, and the next file is worked on as ever.
        int toldToNobody = 0;
        {
            const auto gone = std::make_unique<QObject>();
            exchange.layOut(file, thePassphrase(), gone.get(),
                            [&](const base::Result<WallExchange::LaidOut>&) { ++toldToNobody; });
            exchange.write(thePassphrase(), gone.get(), [&](const base::Result<Written>&) { ++toldToNobody; });
        }
        QTRY_VERIFY(!exchange.isBusy());
        QTest::qWait(100);
        QCOMPARE(toldToNobody, 0);
        QCOMPARE(saidOf(laidOutBy(exchange, file)), u"read"_s);
        // Nobody to tell: nothing is worked on.
        exchange.layOut(file, thePassphrase(), nullptr, [&](const base::Result<WallExchange::LaidOut>&) { ++told; });
        exchange.write(thePassphrase(), nullptr, [&](const base::Result<Written>&) { ++told; });
        QVERIFY(!exchange.isBusy());
        QTest::qWait(100);
        QCOMPARE(told, 1);
        // And one that goes while it works takes its work with it.
        {
            WallExchange shortLived(own.wall, quickly());
            shortLived.layOut(file, thePassphrase(), &asker,
                              [&](const base::Result<WallExchange::LaidOut>&) { ++toldToNobody; });
        }
        QTest::qWait(100);
        QCOMPARE(toldToNobody, 0);
        QVERIFY(!exchange.isBusy());
    }

    // --- what holds these tests to this PC's own address ------------------------------------------

    void aWallOfTheseTestsIsAskedNothingUnlessEveryKvmOfItIsAtThisPcsOwnAddress()
    {
        // This PC's own: where the made-up KVMs are, and where nobody is.
        for (const QString& address : {u"http://127.0.0.1:20001"_s, u"https://127.0.0.1"_s, u"https://localhost:8443"_s,
                                       u"https://[::1]:8443"_s}) {
            QVERIFY2(isThisPcsOwn(address), qPrintable(address));
        }
        // What reaches nothing, by the books: the three ranges that are for examples, and a
        // name under ".invalid". A wall of a test may hold these, and is asked nothing then.
        for (const QString& address : {u"https://192.0.2.7"_s, u"https://198.51.100.20:8443"_s, u"http://203.0.113.9"_s,
                                       u"https://kvm.invalid"_s, u"https://rack1.kvm.invalid:8443"_s}) {
            QVERIFY2(!isThisPcsOwn(address), qPrintable(address));
            QVERIFY2(reachesNothingByTheBooks(address), qPrintable(address));
        }
        // And what could be somebody's: the ranges a home and an office have, a name that could
        // exist, numbers no router passes on. No test of this file writes one onto a wall.
        for (const QString& address :
             {u"https://192.168.1.21"_s, u"http://10.0.0.1"_s, u"https://172.16.0.9"_s, u"https://kvm.example.net"_s,
              u"https://192.0.3.7"_s, u"https://invalid.example"_s, u"http://169.254.169.254"_s,
              u"https://[fe80::1]:8443"_s}) {
            QVERIFY2(!isThisPcsOwn(address), qPrintable(address));
            QVERIFY2(!reachesNothingByTheBooks(address), qPrintable(address));
        }
        // (Those of the server's own example files are what that file holds, and are named.)
        QVERIFY(isOfTheServersExampleFiles(u"https://192.168.1.21"_s));
        QVERIFY(!isOfTheServersExampleFiles(u"https://192.168.1.23"_s));

        // A wall that holds one KVM that is not at this PC's own address is one that these
        // tests ask nothing: whoever asks it all the same has failed, and nothing was contacted
        // (WallOfATest, above, is what every wall of this file is).
        OwnWall own;
        OwnKvm here;
        here.address = u"http://127.0.0.1:20001"_s;
        QVERIFY(own.wall.add(here, u"secret"_s).ok());
        QVERIFY(own.wall.holdsOnlyThisPcsOwn());
        QVERIFY(own.wall.forABox() == &own.wall);
        OwnKvm away;
        away.address = u"https://192.0.2.7"_s;
        QVERIFY(own.wall.add(away, u"secret"_s).ok());
        QVERIFY(!own.wall.holdsOnlyThisPcsOwn());

        // What such a wall does where it is asked all the same, looked at here by having it note
        // what it would fail its test with (no other test hands that in). It is not handed to a
        // box: whoever asks for it gets none, and is told that it was not handed over.
        QStringList failedWith;
        own.wall.insteadOfFailing([&failedWith](const QString& why) { failedWith.append(why); });
        QVERIFY(own.wall.forABox() == nullptr);
        QCOMPARE(failedWith, QStringList{u"A wall that holds a KVM at another address than this PC's own was to have "
                                         "a box opened on it. It was not handed over."_s});
        // And each of the five calls that would have a driver speak to an address is refused,
        // with what was refused said.
        failedWith.clear();
        const base::Result<QHash<QString, core::Status>> status =
            answerTo<QHash<QString, core::Status>>([&](auto done) { own.wall.askStatus(std::move(done)); });
        QCOMPARE(saidOf(status), u"test: A wall of these tests is asked nothing while it holds another address."_s);
        QCOMPARE(failedWith, QStringList{u"A wall that holds a KVM at another address than this PC's own was asked "
                                         "how its KVMs are. The call was refused, and nothing was asked at any "
                                         "address."_s});
        // A wall of the strict kind, which every wall is that does not say otherwise, holds
        // neither what could be somebody's nor what the server's example files hold.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        WatchedSecrets secrets;
        WallOfATest strict(settings, secrets, quickDriver());
        QStringList strictFailedWith;
        strict.insteadOfFailing([&strictFailedWith](const QString& why) { strictFailedWith.append(why); });
        OwnKvm ofTheExamples;
        ofTheExamples.address = u"https://192.168.1.21"_s;
        QVERIFY(strict.add(ofTheExamples, u"secret"_s).ok());
        QCOMPARE(strictFailedWith, QStringList{u"A wall of these tests holds https://192.168.1.21: neither this PC's "
                                               "own address, nor one that reaches nothing by the books."_s});
        QVERIFY(strict.forABox() == nullptr);
    }

    // --- what is said, and what a KVM is called -----------------------------------------------------

    void whereWindowsTrustsWhatAKvmWithNoCertificateInItsFileShowsItIsSignedInToOnThatWord_data()
    {
        QTest::addColumn<QString>("fileSays");
        QTest::newRow("pinned, and no certificate named") << u"pinned"_s;
        QTest::newRow("its certificate is not checked") << u"insecure"_s;
    }

    void whereWindowsTrustsWhatAKvmWithNoCertificateInItsFileShowsItIsSignedInToOnThatWord()
    {
        // Such a KVM is taken as known by Windows' word. A KVM's own certificate does not have
        // that, and is put before a person (above). One that Windows trusts for its name has
        // it: the file's password goes to it with nobody having looked at any certificate. That
        // is the rule, and what is laid out for it says so. (It said "nothing is sent to it
        // before somebody has looked at that certificate" of both once.)
        QFETCH(QString, fileSays);
        const MadeUpKvm kvm(tls("signed"));
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const base::Result<WallExchange::LaidOut> laidOut =
            laidOutBy(exchange, fileOf({saying(kvmAt(kvm.stand->address(), u"Signed by a maker"_s), fileSays)}));
        const QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 1);
        QVERIFY(rows.constFirst().notByWhatItShowsFirst);
        QCOMPARE(rows.constFirst().knownBy, WallExchange::KnownBy::WindowsWord);
        const QString said = rows.constFirst().says.join(u' ');
        QVERIFY2(said.contains(u"Where Windows does trust the certificate it shows, it is asked what it is and signed "
                               "in to on Windows' word, with nobody having looked at that certificate."_s),
                 qPrintable(said));
        QVERIFY2(
            said.contains(u"A KVM's own certificate does not have that: then its tile says that its certificate is "
                          "not trusted, and nothing is sent to it before somebody has looked at that certificate "
                          "and trusted it."_s),
            qPrintable(said));
        const QString id = theOne(exchange.take(laidOut.value().number(), {1}));
        QVERIFY(!id.isEmpty());
        // And so it is: asked what it is, and signed in to, with nobody having looked.
        QCOMPARE(settled(own.wall, id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(!kvmOf(own.wall, id).knownByCertificate);
        QCOMPARE(kvm.stand->asked.value(4).path, QByteArray(signInQuestion)); // (after the question asked first)
    }

    void aDeviceWithoutANameHasNoneAndIsCalledByWhereItIsAlsoAfterItWasMoved()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const MadeUpKvm kvm;
        const MadeUpKvm elsewhere;
        const QString host = u"127.0.0.1:%1"_s.arg(kvm.stand->serverPort());
        const QString hostElsewhere = u"127.0.0.1:%1"_s.arg(elsewhere.stand->serverPort());
        // A device with no name in its file. (The reader names it by its address, as a server
        // does: kept as its name, that would stay the address it had.)
        const QJsonObject nameless{{u"url"_s, kvm.stand->address()}, {u"kind"_s, u"kvmd"_s}};
        const base::Result<WallExchange::LaidOut> laidOut =
            laidOutBy(exchange, plainFile({{u"devices"_s, QJsonArray{nameless}}}), QString());
        QCOMPARE(saidOf(laidOut), u"read"_s);
        // Laid out by its address alone.
        QCOMPARE(rowsOf(laidOut).constFirst().nameAndAddress, kvm.stand->address());
        const QString id = theOne(exchange.take(laidOut.value().number(), {1}));
        QVERIFY(!id.isEmpty());
        QCOMPARE(kvmOf(own.wall, id).name, QString());
        const auto calledOnTheWall = [&own] {
            const base::Result<core::Wall> wall =
                answerTo<core::Wall>([&own](auto done) { own.wall.askWall(std::move(done)); });
            return wall.ok() ? wall.value().machines.value(0).name : QString();
        };
        QCOMPARE(calledOnTheWall(), host);
        // Moved, it is called by where it is now.
        OwnKvm moved = kvmOf(own.wall, id);
        moved.address = elsewhere.stand->address();
        QVERIFY(own.wall.change(id, moved, u"secret"_s).ok());
        QCOMPARE(kvmOf(own.wall, id).name, QString());
        QCOMPARE(calledOnTheWall(), hostElsewhere);
        // And written to a file again it has no name there either: whoever reads that file
        // names it by the address it has in it.
        const base::Result<Written> written = writtenBy(exchange, QString());
        QCOMPARE(saidOf(written), u"read"_s);
        const base::Result<Content> read = core::exchange::read(textOf(written.value()), QString());
        QCOMPARE(core::testing::devicesIn(read.value()).constFirst().name, hostElsewhere);
        // A name that is one is kept as it is.
        const QString named = theOne(takenFrom(exchange, {kvmAt(u"http://127.0.0.1:20001"_s, u"Rack 3"_s)}));
        QCOMPARE(kvmOf(own.wall, named).name, u"Rack 3"_s);
    }

    // --- what is kept of a file that was laid out -------------------------------------------------

    void ofAFileThatWasLaidOutOnlyWhatCanBeTakenIsKeptAndThatIsForgotten()
    {
        OwnWall own;
        OwnKvm byHand;
        byHand.address = u"https://127.0.0.1:20443"_s;
        QVERIFY(own.wall.add(byHand, u"secret"_s).ok());
        WallExchange exchange(own.wall, quickly());
        const Device notKept =
            notKeptHere(kvmAt(u"http://127.0.0.1:20002"_s, u"A server"_s, u"a password it would have"_s));
        Device jetKvm = kvmAt(u"http://127.0.0.1:20006"_s, u"A small one"_s, u"a JetKVM's password"_s);
        jetKvm.kind = u"jetkvm"_s;
        const QByteArray file = fileOf({kvmAt(u"http://127.0.0.1:20001"_s, u"One"_s), notKept,
                                        kvmAt(u"https://127.0.0.1:20443"_s, u"There already"_s),
                                        kvmAt(u"http://127.0.0.1:20004"_s, u"No password"_s, QString()),
                                        kvmAt(u"http://127.0.0.1:20005"_s, u"Five"_s), jetKvm});
        QCOMPARE(exchange.passwordsHeld(), 0);
        base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        QList<bool> canBeTaken;
        for (const WallExchange::Offered& row : rowsOf(laidOut)) {
            canBeTaken.append(row.canBeTaken);
        }
        QCOMPARE(canBeTaken, (QList<bool>{true, false, false, true, true, true}));
        // Kept: the passwords of the three that can be taken and came with one, the JetKVM's among them. Not
        // that of the one this wall does not keep as the file has it, nor of the one that is on the wall
        // already: nothing here could ever need them.
        QCOMPARE(exchange.passwordsHeld(), 3);

        // Forgotten with a taking: one is taken, and the laying-out is over.
        QCOMPARE(codeOf(exchange.take(laidOut.value().number(), {1}).constFirst()), u"taken"_s);
        QCOMPARE(exchange.passwordsHeld(), 0);
        QCOMPARE(codeOf(exchange.take(laidOut.value().number(), {5}).constFirst()), u"not_laid_out"_s);
        QCOMPARE(own.wall.kvms().size(), 2);
        // (A taking that names no place is none, and ends nothing.)
        laidOut = laidOutBy(exchange, file);
        QCOMPARE(exchange.passwordsHeld(), 2);
        QVERIFY(exchange.take(laidOut.value().number(), {}).isEmpty());
        QCOMPARE(exchange.passwordsHeld(), 2);
        // With another laying out, whatever comes of that one.
        QCOMPARE(saidOf(laidOutBy(exchange, "not a file"_ba)), u"not_a_file: This is not a file that qKVM made"_s);
        QCOMPARE(exchange.passwordsHeld(), 0);
        // When that is said.
        laidOut = laidOutBy(exchange, file);
        QCOMPARE(exchange.passwordsHeld(), 2);
        exchange.forget();
        QCOMPARE(exchange.passwordsHeld(), 0);

        // And after a time that is said, where nothing ended it sooner: ten minutes as the
        // program has it, and a moment here.
        QCOMPARE(WallExchange::keptForMsAsItIs, 600'000);
        // (Long enough that "still kept" is looked at with seconds to spare on a PC that is
        // busy with other work, and no longer.)
        WallExchange keepsForAMoment(own.wall, quickly(), 2000);
        laidOut = laidOutBy(keepsForAMoment, file);
        QCOMPARE(keepsForAMoment.passwordsHeld(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(keepsForAMoment.passwordsHeld(), 0, 20000);
        QCOMPARE(codeOf(keepsForAMoment.take(laidOut.value().number(), {5}).constFirst()), u"not_laid_out"_s);
        QCOMPARE(own.wall.kvms().size(), 2);
    }

    void thatWhatWasLaidOutIsKeptNoLongerIsSaidToWhoeverShowsIt()
    {
        // A box that shows what was laid out is told when the time is over for which that is
        // kept: once, when it is forgotten already, and of that forgetting alone.
        OwnWall own;
        const QByteArray file = fileOf({kvmAt(u"http://127.0.0.1:20001"_s, u"One"_s)});
        QCOMPARE(WallExchange(own.wall, quickly()).keptForMs(), WallExchange::keptForMsAsItIs);
        // (Kept for two seconds here: "not said yet" is looked at with seconds to spare, on a
        // PC that is busy with other work too; and each wait for what is to have come is long.)
        const int keptFor = 2000;
        WallExchange exchange(own.wall, quickly(), keptFor);
        QCOMPARE(exchange.keptForMs(), keptFor);
        qsizetype heldWhenSaid = -1;
        int said = 0;
        connect(&exchange, &WallExchange::keptNoLonger, this, [&] {
            ++said;
            heldWhenSaid = exchange.passwordsHeld();
        });
        const base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        QCOMPARE(exchange.passwordsHeld(), 1);
        QCOMPARE(said, 0);
        QTRY_COMPARE_WITH_TIMEOUT(said, 1, 20000);
        QCOMPARE(heldWhenSaid, 0);
        QCOMPARE(codeOf(exchange.take(laidOut.value().number(), {1}).constFirst()), u"not_laid_out"_s);
        // (Once, however long one waits: the time ran once.)
        QTest::qWait(500);
        QCOMPARE(said, 1);

        // Not said of what somebody did: a taking, another laying-out, forget(). Each is looked
        // at for longer than a file is kept: what was not stopped would have been said by then.
        said = 0;
        base::Result<WallExchange::LaidOut> again = laidOutBy(exchange, file);
        exchange.forget();
        QTest::qWait(keptFor + 1000);
        QCOMPARE(said, 0);
        again = laidOutBy(exchange, file);
        again = laidOutBy(exchange, file);
        QCOMPARE(said, 0);
        QCOMPARE(codeOf(exchange.take(again.value().number(), {1}).constFirst()), u"taken"_s);
        QTest::qWait(keptFor + 1000);
        QCOMPARE(said, 0);
    }

    void aFileThatIsGivenUpWhileItIsStillReadIsNotLaidOut()
    {
        OwnWall own;
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = fileOf({kvmAt(u"http://127.0.0.1:20001"_s, u"One"_s)});
        // The box that asked is closed while the file is still read, and says what a box that
        // goes says: forget(). What the reading comes to afterwards is nobody's: nothing of the
        // file is kept from then on, for ten minutes and under a number that could be taken from,
        // and whoever asked is told once that it was given up.
        QObject asker;
        QList<base::Result<WallExchange::LaidOut>> told;
        exchange.layOut(file, thePassphrase(), &asker,
                        [&told](const base::Result<WallExchange::LaidOut>& result) { told.append(result); });
        QVERIFY(told.isEmpty());
        exchange.forget();
        QVERIFY(QTest::qWaitFor([&told] { return !told.isEmpty(); }, 30000));
        QCOMPARE(saidOf(told.constFirst()),
                 u"given_up: The file was given up before it was read. Nothing of it is kept."_s);
        QCOMPARE(exchange.passwordsHeld(), 0);
        QTest::qWait(50);
        QCOMPARE(told.size(), 1);
        // The next file is laid out as any.
        const base::Result<WallExchange::LaidOut> next = laidOutBy(exchange, file);
        QVERIFY(next.ok());
        QCOMPARE(exchange.passwordsHeld(), 1);
    }

    void whatWasLaidOutAsNotToBeTakenIsNotTakenWhenTheWallHasChangedSince()
    {
        OwnWall own;
        const QString inTheirWay =
            own.wall.add(knownAt(u"https://127.0.0.1:20443"_s, sha256Of("first"), u"Rack one"_s), u"secret"_s).value();
        WallExchange exchange(own.wall, quickly());
        const QByteArray file = fileOf({kvmAt(u"http://127.0.0.1:20443"_s, u"At its place"_s),
                                        knownBy(kvmAt(u"https://127.0.0.1:20999"_s, u"Its twin"_s), "first"),
                                        kvmAt(u"http://127.0.0.1:20001"_s, u"New"_s)});
        base::Result<WallExchange::LaidOut> laidOut = laidOutBy(exchange, file);
        QList<WallExchange::Offered> rows = rowsOf(laidOut);
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(0).whyNot, u"duplicate"_s);
        QCOMPARE(rows.at(1).whyNot, u"same_device"_s);
        QVERIFY(rows.at(2).canBeTaken);
        // Between the laying out and the taking, the KVM that was in their way is taken off the
        // wall. Nobody was shown the two as devices that would be taken: they are not, for the
        // reason they were laid out with.
        QVERIFY(own.wall.remove(inTheirWay).ok());
        const QList<WallExchange::Took> took = exchange.take(laidOut.value().number(), {1, 2, 3});
        QCOMPARE(codeOf(took.at(0)), u"duplicate"_s);
        QCOMPARE(sentenceOf(took.at(0)), rows.at(0).verdict);
        QCOMPARE(codeOf(took.at(1)), u"same_device"_s);
        QCOMPARE(sentenceOf(took.at(1)), rows.at(1).verdict);
        QCOMPARE(codeOf(took.at(2)), u"taken"_s);
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.secrets.inner.all.size(), 1);
        // (Laid out anew they are shown as devices that can be taken, and then they are.)
        laidOut = laidOutBy(exchange, file);
        rows = rowsOf(laidOut);
        QVERIFY(rows.at(0).canBeTaken && rows.at(1).canBeTaken);
        QCOMPARE(rows.at(2).whyNot, u"duplicate"_s);

        // The other way round: what was laid out as to be taken, and came onto the wall since,
        // is not taken a second time.
        OwnKvm meanwhile;
        meanwhile.address = u"https://127.0.0.1:20443"_s;
        meanwhile.name = u"Added meanwhile"_s;
        QVERIFY(own.wall.add(meanwhile, u"secret"_s).ok());
        const QList<WallExchange::Took> second = exchange.take(laidOut.value().number(), {1, 2});
        QCOMPARE(codeOf(second.at(0)), u"duplicate"_s);
        QCOMPARE(sentenceOf(second.at(0)), u"It cannot be taken: it is on the wall already (\"Added meanwhile\" at "
                                           "https://127.0.0.1:20443)."_s);
        QCOMPARE(codeOf(second.at(1)), u"taken"_s);
        QCOMPARE(own.wall.kvms().size(), 3);
    }

    // --- a wall that is larger than a file may be --------------------------------------------------

    void aWallThatIsTooLargeForAFileWithItsCertificatesIsWrittenWithoutThem()
    {
        // A file of QKVM's holds four million signs, and a certificate may be twenty thousand:
        // a wall of some hundreds of KVMs with long certificates does not fit WITH them. (And
        // always fits without.) The certificates here are the one of the server's example
        // files, made long: certificates to Windows, and nobody's.
        const auto wallOf = [](OwnWall& own, int count) {
            const QByteArray example = exampleCertificate();
            QList<LocalWall::FromAFile> those;
            for (int number = 0; number < count; ++number) {
                const QByteArray der = core::testing::certificateOfSoManyBytes(example, 14'000 - number);
                LocalWall::FromAFile one;
                one.kvm.address = u"https://127.0.0.1:%1"_s.arg(21000 + number);
                one.kvm.name = u"KVM %1"_s.arg(number + 1);
                one.kvm.knownByCertificate = true;
                one.kvm.certificate = QCryptographicHash::hash(der, QCryptographicHash::Sha256);
                one.certificatePem = core::testing::inOneLine(der);
                those.append(one);
            }
            const QList<base::Result<QString>> came = own.wall.takeFromAFile(those);
            return std::ranges::all_of(came, [&own](const base::Result<QString>& one) {
                return one.ok() && own.wall.certificateOf(one.value()).size() > 18'000;
            });
        };
        const QString withoutThem =
            u"With the certificates of its KVMs in it, this wall is larger than a file of qKVM's may be. The file was "
            "written without them: every KVM that is known by a certificate is in it as pinned, with no certificate "
            "named. Whoever reads the file has those certificates to look at anew, and a qKVM server takes the first "
            "one each shows."_s;
        const QString notThisPcsYet =
            u"Server alert settings and this PC's accounts, app password, window layout and preferences are not in this file."_s;
        {
            // One that fits is written with its certificates, as ever.
            OwnWall own;
            QVERIFY(wallOf(own, 100));
            WallExchange exchange(own.wall, quickly());
            const base::Result<Written> written = writtenBy(exchange, QString());
            QCOMPARE(saidOf(written), u"read"_s);
            QCOMPARE(written.value().notInIt(), QStringList{notThisPcsYet});
            const base::Result<Content> read = core::exchange::read(textOf(written.value()), QString());
            const QList<Device> devices = core::testing::devicesIn(read.value());
            QCOMPARE(devices.size(), 100);
            QCOMPARE(devices.constFirst().certificatePem, own.wall.certificateOf(own.wall.kvms().constFirst().id));
        }
        OwnWall own;
        QVERIFY(wallOf(own, 230));
        WallExchange exchange(own.wall, quickly());
        const base::Result<Written> written = writtenBy(exchange, QString());
        // Written all the same: every KVM is in the file.
        QCOMPARE(saidOf(written), u"read"_s);
        QVERIFY(textOf(written.value()).size() < core::exchange::largestPlain);
        const base::Result<Content> read = core::exchange::read(textOf(written.value()), QString());
        QCOMPARE(saidOf(read), u"read"_s);
        const QList<Device> devices = core::testing::devicesIn(read.value());
        QCOMPARE(devices.size(), 230);
        for (const Device& device : devices) {
            QCOMPARE(device.tls, u"pinned"_s);
            QVERIFY(device.certificatePem.isEmpty());
        }
        // And the first line beside the file says what was done and why; then the line of this
        // piece's own that is beside every file; then the writer's line for each of the KVMs.
        const QStringList beside = written.value().notInIt();
        QCOMPARE(beside.size(), 232);
        QCOMPARE(beside.at(0), withoutThem);
        QCOMPARE(beside.at(1), notThisPcsYet);
        QCOMPARE(
            beside.at(2),
            u"Device 1 of this wall (\"KVM 1\"): only the fingerprint of its certificate is known here, and a file "
            "holds a certificate itself or none. A qKVM server that reads this file will take the first "
            "certificate this device shows."_s);
        // The wall itself keeps what it kept.
        QCOMPARE(own.settings.remembered().kvmCertificates.size(), 230);
    }
};

QTEST_GUILESS_MAIN(TestWallExchange)
#include "test_wall_exchange.moc"
