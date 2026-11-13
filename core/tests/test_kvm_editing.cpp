#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/server_client.h>
#include <qkvm/core/tls.h>

#include "jetkvm_by_hand.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmCertificateReview;
using core::KvmEditing;
using core::LocalWall;
using core::OwnKvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::good;
using core::testing::JetKvmByHand;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;
using core::testing::refusal;
using core::testing::savedFor;
using core::testing::sign;
using core::testing::StandIn;
using core::testing::written;

namespace {

// Where a kvmd is signed in to, and the three questions a probe asks.
constexpr const char* signInQuestion = "/api/auth/login";
constexpr const char* kvmdsQuestion = "/api/auth/check";
constexpr const char* jetKvmsQuestion = "/device/status";
constexpr const char* redfishsQuestion = "/redfish/v1/";
constexpr const char* namesQuestion = "/api/system/get_hostname";

// The server's sentences, as its page shows them.
constexpr const char* notOneLine = "this has to be one line: no line breaks or control characters, and nothing that "
                                   "turns the direction of the writing round";
constexpr const char* tooLongBy = "this may have %1 characters at the most";
constexpr const char* nothingToKnowItBy =
    "There is nothing to know this device by at another address. Type its password to use it there.";
constexpr const char* certificateChanged = "Certificate does not match the one saved for this device";
constexpr const char* noCertificateThere = "This device is reached over plain HTTP, where there is no certificate";
// And the program's own, where the server has none.
constexpr const char* notInTheClear =
    "This device is on the wall over HTTPS. It is not changed to plain HTTP at the same address, where its password "
    "would travel unencrypted. To use it that way, remove it from the wall and add it again.";

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

// The times a real KVM is given, made short: for the driver that signs in, and for the wall's own.
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

core::KvmProbe::Pace quickProbe()
{
    core::KvmProbe::Pace pace;
    // Room to be let in, and the whole asking with it: a TLS handshake under the address sanitizer on a busy PC
    // took longer than the 1.5 s this had (gate 18 of M6: seven certificate tests "unreachable", all of them
    // passing when run again by themselves), as the Add box's tests had found in gate 4 of Checkpoint 2.
    pace.connectWithinMs = 5000;
    pace.quietMs = 1500;
    pace.withinMs = 6500;
    return pace;
}

// This PC's own wall for one test: its settings in a folder that goes with the test, its secrets
// in memory.
struct OwnWall {
    OwnWall() : file(folder.filePath(u"kept/settings.json"_s)), settings(file), wall(settings, secrets, quickDriver())
    {}
    // With the times of its drivers, and where a JetKVM's other way is to it, of a test's choosing.
    explicit OwnWall(const core::DriverPaces& paces)
        : file(folder.filePath(u"kept/settings.json"_s)), settings(file), wall(settings, secrets, paces)
    {}

    // From here on the settings cannot be written: where their folder was, a file is.
    bool settingsNoLongerWritable() const
    {
        QFile inTheWay(folder.filePath(u"kept"_s));
        return QDir(folder.filePath(u"kept"_s)).removeRecursively() && inTheWay.open(QIODevice::WriteOnly);
    }

    QTemporaryDir folder;
    QString file;
    core::Settings settings;
    MemorySecrets secrets;
    LocalWall wall;
};

// What answers at an address and is no kvmd. A page, whatever is asked of it:
Answered asAPage(const Asked&)
{
    return {200, "<!doctype html><html><body><h1>Sign in</h1></body></html>"_ba, "text/html"_ba};
}
// And a JetKVM, which says so itself and answers everything else with its page:
Answered asAJetKvm(const Asked& asked)
{
    if (asked.path == jetKvmsQuestion) {
        return {200, written({{u"isSetup"_s, true}})};
    }
    return asAPage(asked);
}

// What the program says aloud while a test listens: Qt's own messages and the program's.
QStringList* saidAloud = nullptr;
QtMessageHandler handlerBefore = nullptr;
void listening(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (saidAloud != nullptr) {
        saidAloud->append(message);
    }
    if (handlerBefore != nullptr) {
        handlerBefore(type, context, message);
    }
}

// A JetKVM's own number as its driver gives it (JetKvmDevice::Identity): "jetkvm:" and a digest of it.
QString digestOf(const QString& number)
{
    return u"jetkvm:"_s +
           QString::fromLatin1(QCryptographicHash::hash(number.toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// Two addresses that are one JetKVM's two ways (KvmEditing::Pace::otherWay): the stand-ins of a test are
// at two ports of this PC, where a real one's are ports 80 and 443 of one host.
std::function<std::optional<QUrl>(const QUrl&)> twoWays(const QUrl& one, const QUrl& other)
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

} // namespace

// One KVM of this PC's own wall changed, and the certificate it is known by looked at and said yes
// to anew: against made-up KVMs, and against things that answer at an address and are none.
class TestKvmEditing : public QObject {
    Q_OBJECT

private:
    static KvmEditing::Pace quick()
    {
        KvmEditing::Pace pace;
        pace.probe = quickProbe();
        pace.driver = quickDriver();
        return pace;
    }
    // The same, with room for a sign-in over TLS to be let in (5 s), as the probe has: where a JetKVM's two
    // ways are asked, a slow handshake is not to make a test find the HTTPS way gone.
    static KvmEditing::Pace roomForTls()
    {
        KvmEditing::Pace pace = quick();
        pace.driver.connectWithinMs = 5000;
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

    // A KVM as it stands on the wall once it was added: called `name`, and known by the certificate
    // of that name where one is named.
    static OwnKvm at(const QString& address, const QString& name, const char* certificate = nullptr)
    {
        OwnKvm own;
        own.address = address;
        own.name = name;
        own.knownByCertificate = certificate != nullptr;
        own.certificate = certificate != nullptr ? sha256Of(certificate) : QByteArray();
        return own;
    }
    // Puts it there, with the password the made-up KVMs have, and gives the wall's name for it.
    static QString put(OwnWall& own, const OwnKvm& kvm)
    {
        const base::Result<QString> id = own.wall.add(kvm, u"secret"_s);
        return id.ok() ? id.value() : QString();
    }
    static OwnKvm kvmOf(const LocalWall& wall, const QString& id)
    {
        const auto known = std::ranges::find_if(wall.kvms(), [&id](const OwnKvm& one) { return one.id == id; });
        return known == wall.kvms().end() ? OwnKvm{} : *known;
    }
    // What the store is to hold for a KVM of the wall: this password, saved for the address and
    // the certificate the wall has for it now.
    static QByteArray savedAs(const LocalWall& wall, const QString& id, const QByteArray& password)
    {
        const OwnKvm kvm = kvmOf(wall, id);
        return savedFor(password, kvm.address, kvm.knownByCertificate ? kvm.certificate : QByteArray());
    }
    // What stands in the box when it is opened for a KVM: all of it but its password.
    static KvmEditing::Typed asItIs(const LocalWall& wall, const QString& id)
    {
        const OwnKvm kvm = kvmOf(wall, id);
        KvmEditing::Typed typed;
        typed.name = kvm.name;
        typed.address = kvm.address;
        typed.user = kvm.user;
        typed.group = kvm.group;
        return typed;
    }

    // Changes, lets the program's loop run until the result is there, and gives it. A result that
    // was there before change() came back, or never came, has failed the test.
    static base::Result<KvmEditing::Changed> changedBy(KvmEditing& editing, const QString& id,
                                                       const KvmEditing::Typed& typed)
    {
        return cameOf(editing, id, typed).result;
    }
    // The same, with all that whoever asked is handed: the two certificates of a change that was
    // refused over them, too.
    static KvmEditing::Outcome cameOf(KvmEditing& editing, const QString& id, const KvmEditing::Typed& typed)
    {
        QList<KvmEditing::Outcome> told;
        editing.change(id, typed, [&told](const KvmEditing::Outcome& outcome) { told.append(outcome); });
        if (!told.isEmpty()) {
            return {base::Error{"at_once", "The result was handed over before change() had come back."}, {}, {}, {}};
        }
        if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 20000)) {
            return {base::Error{"never", "The change never ended."}, {}, {}, {}};
        }
        return told.constFirst();
    }
    // The code a change was refused with; "saved" where it was not.
    static std::string refusalOf(const KvmEditing::Outcome& outcome)
    {
        return outcome.result.ok() ? std::string("saved") : outcome.result.error().code;
    }
    // The sentence it was refused with; "saved" where it was not.
    static std::string sentenceOf(const KvmEditing::Outcome& outcome)
    {
        return outcome.result.ok() ? std::string("saved") : outcome.result.error().message;
    }
    static base::Result<KvmCertificateReview::Shown> shownBy(KvmCertificateReview& review, const QString& id)
    {
        QList<base::Result<KvmCertificateReview::Shown>> told;
        review.askCertificates(
            id, [&told](const base::Result<KvmCertificateReview::Shown>& result) { told.append(result); });
        if (!told.isEmpty()) {
            return base::Error{"at_once", "The certificates were handed over before the call had come back."};
        }
        if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 20000)) {
            return base::Error{"never", "The certificates never came."};
        }
        return told.constFirst();
    }
    static base::Result<void> trustedBy(KvmCertificateReview& review, const QString& id, const QByteArray& shown)
    {
        QList<base::Result<void>> told;
        review.trust(id, shown, [&told](const KvmCertificateReview::Outcome& yes) { told.append(yes.result); });
        if (!told.isEmpty()) {
            return base::Error{"at_once", "The result was handed over before trust() had come back."};
        }
        if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 20000)) {
            return base::Error{"never", "The trusting never ended."};
        }
        return told.constFirst();
    }

    // Whether nothing that was asked at an address says who asks: no password, no session.
    static bool heardNothingOfWhoAsks(const StandIn& stand)
    {
        return std::ranges::all_of(stand.asked, [](const Asked& one) {
            return one.method == "GET" && one.body.isEmpty() && !one.headers.contains("cookie") &&
                   !one.headers.contains("authorization") && !one.path.contains("secret");
        });
    }
    // And whether all of it is what a probe asks: its three questions, each `times` times.
    static bool heardOnlyTheQuestions(const StandIn& stand, int times = 1)
    {
        return heardNothingOfWhoAsks(stand) && stand.count(kvmdsQuestion) == times &&
               stand.count(jetKvmsQuestion) == times && stand.count(redfishsQuestion) == times &&
               stand.asked.size() == qsizetype{3} * times;
    }

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
    // How a KVM of the wall is, asked until it is `state`; as it was last where it never is.
    static core::Status onceItIs(LocalWall& wall, const QString& id, const QString& state)
    {
        core::Status status;
        static_cast<void>(QTest::qWaitFor(
            [&] {
                const auto all =
                    answerTo<QHash<QString, core::Status>>([&](auto done) { wall.askStatus(std::move(done)); });
                status = all.ok() ? all.value().value(id) : core::Status{};
                return status.state == state;
            },
            20000));
        return status;
    }

    // Two KVMs of a wall that have changed places since they were added, as addresses that are
    // handed out do: "Build server", known by the certificate "first", and "Database", known by
    // "second", whose password is its own. What answers at the second's address now is the first.
    struct ChangedPlaces {
        QString first; // the wall's names for the two
        QString second;
        quint16 firstsPort = 0;
        std::unique_ptr<MadeUpKvm> atTheSeconds;
    };
    static void changePlaces(OwnWall& own, ChangedPlaces& places)
    {
        auto first = std::make_unique<MadeUpKvm>(tls("first"));
        auto second = std::make_unique<MadeUpKvm>(tls("second"));
        places.first = put(own, at(first->stand->address(), u"Build server"_s, "first"));
        const base::Result<QString> id =
            own.wall.add(at(second->stand->address(), u"Database"_s, "second"), u"the second's own"_s);
        places.second = id.ok() ? id.value() : QString();
        places.firstsPort = first->stand->serverPort();
        const quint16 secondsPort = second->stand->serverPort();
        first.reset();
        second.reset();
        places.atTheSeconds = std::make_unique<MadeUpKvm>(tls("first"), secondsPort);
    }

private slots:
    void initTestCase()
    {
        // The probe, the driver and the client of a QKVM server each choose Windows' own TLS. Qt
        // takes that choice until TLS is in use, and answers every one after that with a warning,
        // also where it is the same choice: "Cannot set backend named schannel as active, another
        // backend is already in use", once for each of the three that came late. So the three
        // choose in one place, once (core/tls.h). The first of them is made here before anything
        // else touches TLS (the stand-ins do); then TLS is used, the other two are made, and what
        // Qt says meanwhile is listened to.
        QStringList aloud;
        saidAloud = &aloud;
        handlerBefore = qInstallMessageHandler(listening);
        const auto quiet = qScopeGuard([] {
            qInstallMessageHandler(handlerBefore);
            saidAloud = nullptr;
        });
        const core::KvmProbe firstProbe;
        QVERIFY(QSslSocket::supportsSsl());
        const core::KvmdDevice firstDriver(core::KvmdDevice::Where{}, QString());
        const core::ServerClient firstClient;
        QVERIFY2(aloud.isEmpty(), qPrintable(aloud.join(u" | "_s)));
        QVERIFY(core::useWindowsOwnTls());
        QCOMPARE(QSslSocket::activeBackend(), u"schannel"_s);
    }

    void aNameAndAGroupAreSavedWithNothingAskedOfTheKvm()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        const qsizetype asked = kvm.stand->asked.size();
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        QVERIFY(!editing.isUnderWay());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"  Database\t"_s;
        typed.group = u" Rack \t 2 "_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(!editing.isUnderWay());
        QCOMPARE(saved.value().id, id);
        QCOMPARE(saved.value().name, u"Database"_s);
        QVERIFY(!saved.value().signedIn);
        QVERIFY(saved.value().overPlainHttp);

        // On the wall by the rules of adding, in its place, and nothing else of it another.
        // (It is spoken to over plain HTTP: that it was asked what it is, is not kept of it.)
        QCOMPARE(changed.count(), 1);
        OwnKvm as = at(kvm.stand->address(), u"Database"_s);
        as.id = id;
        as.group = u"Rack 2"_s;
        as.askedWhatItIs = false;
        QCOMPARE(kvmOf(own.wall, id), as);
        QCOMPARE(own.secrets.all.size(), 1);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        // And at the next start.
        const core::Settings again(own.file);
        QCOMPARE(again.remembered().kvms.constFirst(), as);

        // The KVM was asked nothing for it. And the wall's own driver is the one it was: at its
        // next look it is signed in as before, and not anew.
        QTest::qWait(quickDriver().freshMs + 100);
        QCOMPARE(kvm.stand->asked.size(), asked);
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QTRY_VERIFY(kvm.stand->asked.size() > asked);
        QTest::qWait(150);
        QCOMPARE(kvm.signIns, 1);
    }

    void aKvmThatIsAwayCanStillBeRenamed()
    {
        // Switched off, or on a network this PC is not on today.
        OwnWall own;
        const core::testing::NobodyThere nobody;
        const QString id = put(own, at(nobody.address(), u"Away"_s));
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"Spare"_s;
        typed.group = u"Shelf"_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).name, u"Spare"_s);
        QCOMPARE(kvmOf(own.wall, id).group, u"Shelf"_s);

        // So can one that shows another certificate than it is known by: its card is this PC's own.
        const MadeUpKvm other(tls("second"));
        const QString known = put(own, at(other.stand->address(), u"Reset"_s, "first"));
        typed = asItIs(own.wall, known);
        typed.name = u"Reset last week"_s;
        QVERIFY(changedBy(editing, known, typed).ok());
        QCOMPARE(kvmOf(own.wall, known).name, u"Reset last week"_s);
        // It keeps what it is known by, and hears nothing.
        QCOMPARE(kvmOf(own.wall, known).certificate, sha256Of("first"));
        QTest::qWait(150);
        QVERIFY(other.stand->asked.isEmpty());
    }

    void noNameKeepsTheNameAndNoGroupClearsTheGroup()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        OwnKvm one = at(kvm.stand->address(), u"Build server"_s);
        one.group = u"Rack 2"_s;
        const QString id = put(own, one);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name.clear();
        typed.group.clear();
        auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(saved.value().name, u"Build server"_s);
        QCOMPARE(kvmOf(own.wall, id).name, u"Build server"_s);
        QVERIFY(kvmOf(own.wall, id).group.isEmpty());
        // Blanks are no name, and a blank of no width is nothing.
        typed.name = u"  \t"_s + sign(0x200B);
        typed.group = u"Rack 3"_s;
        QVERIFY(changedBy(editing, id, typed).ok());
        QCOMPARE(kvmOf(own.wall, id).name, u"Build server"_s);
        QCOMPARE(kvmOf(own.wall, id).group, u"Rack 3"_s);

        // A KVM that has no name is called by where it is, and stays so.
        const MadeUpKvm unnamed;
        const QString nameless = put(own, at(unnamed.stand->address(), QString()));
        typed = asItIs(own.wall, nameless);
        typed.group = u"Rack 3"_s;
        saved = changedBy(editing, nameless, typed);
        QVERIFY(saved.ok());
        QCOMPARE(saved.value().name, u"127.0.0.1:%1"_s.arg(unnamed.stand->serverPort()));
        QVERIFY(kvmOf(own.wall, nameless).name.isEmpty());

        // And what was saved as it stood in the box changes nothing, and is no failure.
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        QVERIFY(changedBy(editing, id, asItIs(own.wall, id)).ok());
        QVERIFY(changed.isEmpty());
        QTest::qWait(150);
        QVERIFY(kvm.stand->asked.isEmpty());
    }

    void noUserNameIsTheOneEveryKvmComesWith()
    {
        // On the wall as "root", whom the made-up KVM does not know.
        OwnWall own;
        const MadeUpKvm kvm;
        OwnKvm one = at(kvm.stand->address(), u"Build server"_s);
        one.user = u"root"_s;
        const QString id = put(own, one);
        QCOMPARE(onceItIs(own.wall, id, u"auth"_s).state, u"auth"_s);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.user = u"  "_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QCOMPARE(kvmOf(own.wall, id).user, u"admin"_s);
        // Signed in to as that one first, with the password that is kept.
        QCOMPARE(kvm.last(signInQuestion)->body, "user=admin&passwd=secret"_ba);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        // And the wall's own driver is a new one, which signs in as that one.
        const int before = kvm.signIns;
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QCOMPARE(kvm.signIns, before + 1);
    }

    void anotherUserNameIsSignedInAsBeforeItIsSaved()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.user = u" root "_s;
        typed.name = u"Another name"_s;
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("auth_failed"));
        QCOMPARE(refused.error().message, std::string("Login rejected - check the username and password"));
        QCOMPARE(kvm.last(signInQuestion)->body, "user=root&passwd=secret"_ba);
        // Nothing is changed, the name no more than the user: and the old sign-in stays.
        QCOMPARE(kvmOf(own.wall, id), before);
        QVERIFY(changed.isEmpty());
        // Tried once, and not again: a KVM locks an account out.
        QTest::qWait(300);
        QCOMPARE(kvm.signIns, 2);
        const qsizetype asked = kvm.stand->asked.size();
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QTRY_VERIFY(kvm.stand->asked.size() > asked);
        QTest::qWait(150);
        QCOMPARE(kvm.signIns, 2);
    }

    void aNewPasswordIsSignedInWithBeforeItIsSaved()
    {
        OwnWall own;
        MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        // Its password is changed on the KVM itself, and then here. (A password keeps the blanks
        // it was typed with: they may be part of it.)
        kvm.password = u" another one "_s;
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = kvm.password;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QCOMPARE(kvm.last(signInQuestion)->body, "user=admin&passwd=%20another%20one%20"_ba);
        QCOMPARE(changed.count(), 1);
        // Kept in the place of the one before, and nowhere else.
        QCOMPARE(own.secrets.all.size(), 1);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedFor(" another one ", kvm.stand->address()));
        // The wall's own driver is a new one, and signs in with it.
        const int before = kvm.signIns;
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QCOMPARE(kvm.signIns, before + 1);
        QCOMPARE(kvm.last(signInQuestion)->body, "user=admin&passwd=%20another%20one%20"_ba);
    }

    void aPasswordThatIsRefusedChangesNothing()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        OwnKvm one = at(kvm.stand->address(), u"Build server"_s);
        one.group = u"Rack 2"_s;
        const QString id = put(own, one);
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"not it"_s;
        typed.name = u"Another name"_s;
        typed.group = u"Rack 9"_s;
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("auth_failed"));
        QCOMPARE(refused.error().message, std::string("Login rejected - check the username and password"));
        // Nothing of it: not on the wall, not in the file, not with the secrets.
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(core::Settings(own.file).remembered().kvms.constFirst(), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QVERIFY(changed.isEmpty());
        // Tried once. And the old sign-in stays: the wall's driver is the one it was, and at its
        // next look it is signed in as before.
        QTest::qWait(300);
        QCOMPARE(kvm.signIns, 2);
        const qsizetype asked = kvm.stand->asked.size();
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QTRY_VERIFY(kvm.stand->asked.size() > asked);
        QTest::qWait(150);
        QCOMPARE(kvm.signIns, 2);
        // With the right one, all of it is saved after all.
        typed.password = u"secret"_s;
        QVERIFY(changedBy(editing, id, typed).ok());
        QCOMPARE(kvmOf(own.wall, id).name, u"Another name"_s);
        QCOMPARE(kvmOf(own.wall, id).group, u"Rack 9"_s);
    }

    void whatIsOfAnotherFamilyAtThePlaceItIsAtIsSaidAsOnItsCard()
    {
        // From http to https of one host and port: the same place (LocalWall::atOnePlace). What answers there is
        // of the other family, and can only be added once this KVM is gone from that place: said so, as on its
        // card, and it hears no password.
        OwnWall own;
        JetKvmByHand jet(tls("first"));
        const QString there = u"127.0.0.1:%1"_s.arg(jet.stand->serverPort());
        const QString id = put(own, at(u"http://"_s + there, u"Build server"_s));
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = u"https://"_s + there;
        typed.password = u"secret"_s;
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("other_family"));
        QVERIFY2(QString::fromStdString(refused.error().message).contains(u"remove this KVM from the wall"_s),
                 refused.error().message.c_str());
        QVERIFY(jet.signInBodies.isEmpty());
    }

    void aPasswordThatWasRefusedAtANewAddressIsSentWhenItIsTypedAgain()
    {
        // As at the address it has (aPasswordThatWasRefusedIsSentWhenItIsTypedAgain): at another one too.
        OwnWall own;
        const MadeUpKvm old;
        MadeUpKvm moved;
        const QString id = put(own, at(old.stand->address(), u"Build server"_s));
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = moved.stand->address();
        typed.password = u"the new one"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("auth_failed"));
        const int before = moved.signIns;
        QVERIFY(before > 0);
        moved.password = typed.password;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(moved.signIns > before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "the new one"));
    }

    void aPasswordThatWasRefusedIsSentWhenItIsTypedAgain()
    {
        // A password a KVM refused waits a while before it is offered to it again (SignInPacing). One
        // that a person types is no such offer: it was made the KVM's own on the KVM itself meanwhile,
        // and typed again here it is sent at once.
        OwnWall own;
        MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"the new one"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("auth_failed"));
        const int before = kvm.signIns;
        kvm.password = typed.password;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QVERIFY(kvm.signIns > before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "the new one"));
    }

    void aKvmThatIsAwayIsNotGivenAnotherSignIn()
    {
        OwnWall own;
        const core::testing::NobodyThere nobody;
        const QString id = put(own, at(nobody.address(), u"Away"_s));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        // A password, and a name with it: all of it or nothing.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"another one"_s;
        typed.name = u"Spare"_s;
        auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("unreachable"));
        QCOMPARE(refused.error().message, std::string("Device is unreachable"));
        // Or another user name.
        typed.password.clear();
        typed.user = u"root"_s;
        refused = changedBy(editing, id, typed);
        QCOMPARE(refused.error().code, std::string("unreachable"));
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QVERIFY(changed.isEmpty());
    }

    void whatShowsAnotherCertificateAtItsAddressHearsNoPasswordThatIsTyped()
    {
        // The KVM is known by a certificate, and something else stands at its address now, or
        // the KVM after a reset: a password typed into its box is not sent there either. The
        // certificate is looked at first, by whoever sits there.
        OwnWall own;
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        const QString id = put(own, at(kvm->stand->address(), u"Build server"_s, "first"));
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        const MadeUpKvm other(tls("second"), port);
        QVERIFY(other.stand->isListening());
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"typed for the other"_s;
        auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("cert_mismatch"));
        QCOMPARE(refused.error().message, std::string(certificateChanged));
        // Nor the one that is kept, under another user name.
        typed.password.clear();
        typed.user = u"root"_s;
        refused = changedBy(editing, id, typed);
        QCOMPARE(refused.error().code, std::string("cert_mismatch"));
        QCOMPARE(refused.error().message, std::string(certificateChanged));
        QTest::qWait(150);
        QVERIFY(other.stand->asked.isEmpty());
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
    }

    void atANewAddressWithItsPasswordTypedItIsAskedAndSignedInToAsWhenAdded()
    {
        // From the plain way to HTTPS at another address.
        OwnWall own;
        const MadeUpKvm old;
        const MadeUpKvm moved(tls("first"));
        OwnKvm one = at(old.stand->address(), u"Build server"_s);
        one.group = u"Rack 2"_s;
        const QString id = put(own, one);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = u" HTTPS://127.0.0.1:%1/kvm/#/ "_s.arg(moved.stand->serverPort());
        typed.password = u"secret"_s;
        // (It is known by none, and a certificate is shown there: somebody has seen that one
        // and said yes. This had it signed in to at once, and known by whatever answered:
        // turned round, and what goes there without the yes is in
        // whatReachesANewAddressBeforeAYesAndAfterItByHowTheKvmIsKnownAndWhatIsShownThere.)
        typed.saidYesTo = sha256Of("first");
        auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QVERIFY(!saved.value().overPlainHttp);
        QCOMPARE(saved.value().name, u"Build server"_s);
        QCOMPARE(changed.count(), 1);
        // Where it is now, and known by the certificate it showed there. Its name, its group and
        // its place on the wall are what they were.
        OwnKvm kept = kvmOf(own.wall, id);
        QCOMPARE(kept.address, moved.stand->address());
        QVERIFY(kept.knownByCertificate);
        QCOMPARE(kept.certificate, sha256Of("first"));
        QCOMPARE(kept.name, u"Build server"_s);
        QCOMPARE(kept.group, u"Rack 2"_s);
        QCOMPARE(own.wall.kvms().size(), 1);
        // First asked what it is, with nothing that says who asks (and whether it signs this program in, with nothing
        // to sign in with: a password goes only on a line shown to be up); then signed in to, once.
        QVERIFY(moved.stand->asked.size() > 4);
        for (qsizetype index = 0; index < 4; ++index) {
            const Asked& question = moved.stand->asked.at(index);
            QCOMPARE(question.method, "GET"_ba);
            QVERIFY(!question.headers.contains("cookie"));
            QVERIFY(question.body.isEmpty());
        }
        QCOMPARE(moved.stand->asked.at(4).path, QByteArray(signInQuestion));
        QCOMPARE(moved.stand->asked.at(4).body, "user=admin&passwd=secret"_ba);
        QCOMPARE(moved.signIns, 1);
        // Where it was, nothing was asked for it.
        QVERIFY(old.stand->asked.isEmpty());
        // And the wall's own driver speaks to it there, by that certificate.
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QCOMPARE(moved.signIns, 2);

        // At yet another address, where another certificate is shown, it is not signed in to
        // with that alone: the certificate it is known by is not forgotten with the address.
        const MadeUpKvm another(tls("second"));
        typed = asItIs(own.wall, id);
        typed.address = another.stand->address();
        typed.password = u"secret"_s;
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("certificate_differs"));
        QCOMPARE(another.signIns, 0);
        kept = kvmOf(own.wall, id);
        QCOMPARE(kept.address, moved.stand->address());
        QCOMPARE(kept.certificate, sha256Of("first"));

        // And back to the plain way: in the clear, which is said, and known by nothing. (With
        // the yes to that: this had the password sent there with nobody asked. Turned round;
        // what goes there without the yes is in
        // whereTheNewAddressIsPlainHttpAPasswordThatIsTypedGoesThereOnlyOnAYesToThat.)
        const MadeUpKvm plain;
        typed.address = plain.stand->address();
        typed.saidYesToPlainHttpAt = u"127.0.0.1:%1"_s.arg(plain.stand->serverPort());
        saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().overPlainHttp);
        kept = kvmOf(own.wall, id);
        QCOMPARE(kept.address, plain.stand->address());
        QVERIFY(!kept.knownByCertificate);
        QVERIFY(kept.certificate.isEmpty());
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
    }

    void aKvmThatWindowsVouchesForAtItsNewAddressIsKnownByNoCertificate()
    {
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        const MadeUpKvm old;
        const MadeUpKvm moved(tls("signed"));
        const QString id = put(own, at(old.stand->address(), u"Build server"_s));
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = moved.stand->address();
        typed.password = u"secret"_s;
        // (From the plain way: somebody has seen that Windows vouches there, and said yes to
        // that. This had it signed in to at once: turned round.)
        typed.saidYesTo = sha256Of("signed");
        typed.saidYesOnWindowsWord = true;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().certificate.isEmpty());
        QVERIFY(!kvmOf(own.wall, id).knownByCertificate);
        QVERIFY(kvmOf(own.wall, id).certificate.isEmpty());
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
    }

    void atANewAddressTheKeptPasswordGoesToWhatShowsTheCertificateItIsKnownBy()
    {
        // One KVM reached two ways, on the network it stands in and over a VPN: it shows the one
        // certificate it made for itself at both.
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm there(tls("first"));
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = there.stand->address();
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QCOMPARE(changed.count(), 1);
        // There now, and known by the certificate it was known by.
        const OwnKvm kept = kvmOf(own.wall, id);
        QCOMPARE(kept.address, there.stand->address());
        QVERIFY(kept.knownByCertificate);
        QCOMPARE(kept.certificate, sha256Of("first"));
        // Signed in to with the password that is kept, which is saved for that address from now on (and whether it
        // signs this program in, with nothing to sign in with: a password goes only on a line shown to be up).
        QCOMPARE(there.stand->asked.constFirst().path, QByteArray(kvmdsQuestion));
        QVERIFY(!there.stand->asked.constFirst().headers.contains("cookie"));
        QCOMPARE(there.stand->asked.at(1).path, QByteArray(signInQuestion));
        QCOMPARE(there.stand->asked.at(1).body, "user=admin&passwd=secret"_ba);
        QCOMPARE(there.signIns, 1);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", there.stand->address(), sha256Of("first")));
        QVERIFY(here.stand->asked.isEmpty());
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QCOMPARE(there.signIns, 2);
    }

    void whatShowsAnotherCertificateAtANewAddressHearsNothingOfTheKeptPassword()
    {
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm other(tls("second"));
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = other.stand->address();
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        // The server's sentence, which says what does it: its password, typed for that address.
        QCOMPARE(refused.error().code, std::string("password_needed"));
        QCOMPARE(refused.error().message,
                 std::string("This device could not be checked at its new address (Certificate does not match the "
                             "one saved for this device). Type its password to save it there all the same."));
        QTest::qWait(150);
        QVERIFY(other.stand->asked.isEmpty());
        QCOMPARE(other.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QVERIFY(changed.isEmpty());
        // Nor once somebody has said yes to that certificate: where the kept password goes is
        // the wall's rule alone, and it goes to no other certificate at another address.
        typed.saidYesTo = sha256Of("second");
        const auto still = changedBy(editing, id, typed);
        QVERIFY(!still.ok());
        QCOMPARE(still.error().code, std::string("password_needed"));
        QCOMPARE(still.error().message, refused.error().message);
        QTest::qWait(150);
        QVERIFY(other.stand->asked.isEmpty());
        QCOMPARE(kvmOf(own.wall, id), before);
        // Typed, it does not go there either before the two were looked at: what answers there
        // is asked what it is, and no more.
        typed.saidYesTo.clear();
        typed.password = u"secret"_s;
        const auto looked = changedBy(editing, id, typed);
        QVERIFY(!looked.ok());
        QCOMPARE(looked.error().code, std::string("certificate_differs"));
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*other.stand));
        QCOMPARE(other.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QVERIFY(changed.isEmpty());
    }

    void aKeptPasswordThatIsRefusedAtANewAddressIsToBeTyped()
    {
        // The same KVM at another address, by its certificate; and its password is another one
        // there than the one that is kept.
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        MadeUpKvm there(tls("first"));
        there.password = u"another one"_s;
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = there.stand->address();
        const auto refused = changedBy(editing, id, typed);
        QCOMPARE(refused.error().code, std::string("password_needed"));
        QCOMPARE(refused.error().message,
                 std::string("This device could not be checked at its new address (Login rejected - check the "
                             "username and password). Type its password to save it there all the same."));
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(there.signIns, 1);
    }

    void whereNothingAnswersAtANewAddressTheDriversOwnWordIsGiven()
    {
        // There is no saving without checking here: so nobody is told to type a password "to
        // save it there all the same", which typing would not do.
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        const core::testing::NobodyThere nobody;
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("unreachable"));
        QCOMPARE(refused.error().message, std::string("Device is unreachable"));
        // Nor with its password typed.
        typed.password = u"secret"_s;
        refused = changedBy(editing, id, typed);
        QCOMPARE(refused.error().code, std::string("unreachable"));
        QCOMPARE(refused.error().message, std::string("Device is unreachable"));
        QCOMPARE(kvmOf(own.wall, id), before);
    }

    void whereAKvmIsKnownByNothingTheKeptPasswordGoesToNoOtherAddress_data()
    {
        QTest::addColumn<QByteArray>("from");
        QTest::addColumn<QByteArray>("to");
        // Over plain HTTP there is nothing to know it by, at either end.
        QTest::newRow("from the plain way to the plain way") << "plain"_ba << "plain"_ba;
        QTest::newRow("from the plain way to HTTPS") << "plain"_ba << "first"_ba;
        QTest::newRow("from HTTPS to the plain way") << "first"_ba << "plain"_ba;
        // Nor is a KVM that Windows vouches for known by anything of its own: behind somebody's
        // front door the certificate is the door's.
        QTest::newRow("from one Windows vouches for") << "signed"_ba << "signed"_ba;
    }

    void whereAKvmIsKnownByNothingTheKeptPasswordGoesToNoOtherAddress()
    {
        QFETCH(QByteArray, from);
        QFETCH(QByteArray, to);
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        const auto made = [](const QByteArray& how) {
            return how == "plain" ? std::make_unique<MadeUpKvm>() : std::make_unique<MadeUpKvm>(tls(how.constData()));
        };
        OwnWall own;
        const auto here = made(from);
        const auto there = made(to);
        const QString id = put(own, at(here->stand->address(), u"Build server"_s, from == "first" ? "first" : nullptr));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = there->stand->address();
        typed.name = u"Another name"_s;
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("password_needed"));
        QCOMPARE(refused.error().message, std::string(nothingToKnowItBy));
        // Not one line was made to that address, and nothing is changed.
        QTest::qWait(150);
        QVERIFY(there->stand->asked.isEmpty());
        QCOMPARE(there->stand->lines, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QVERIFY(changed.isEmpty());
    }

    void theAddressOfAnotherKvmOfTheWallIsNotTaken()
    {
        OwnWall own;
        const MadeUpKvm one;
        const MadeUpKvm other;
        const QString id = put(own, at(one.stand->address(), u"One"_s));
        QVERIFY(!put(own, at(other.stand->address(), u"Other"_s)).isEmpty());
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        // However it is typed, and with a password or without.
        // (The last: the other way of speaking to it, which is no other place.)
        for (const QString& address :
             {other.stand->address(), u"HTTP://127.0.0.1.:%1/#/"_s.arg(other.stand->serverPort()),
              u"https://127.0.0.1:%1"_s.arg(other.stand->serverPort())}) {
            for (const QString& password : {QString(), u"secret"_s}) {
                typed.address = address;
                typed.password = password;
                const auto refused = changedBy(editing, id, typed);
                QVERIFY(!refused.ok());
                QCOMPARE(refused.error().code, std::string("duplicate"));
                QCOMPARE(refused.error().message, std::string("This device is already on the wall"));
            }
        }
        // The other was asked nothing for that.
        QTest::qWait(150);
        QVERIFY(other.stand->asked.isEmpty());
        QCOMPARE(kvmOf(own.wall, id), before);

        // Its own address, typed another way, is its own: nothing has moved.
        typed.address = u"HTTP://127.0.0.1:%1/kvm/#/"_s.arg(one.stand->serverPort());
        typed.password.clear();
        typed.name = u"Renamed"_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(!saved.value().signedIn);
        QCOMPARE(kvmOf(own.wall, id).address, one.stand->address());
        QCOMPARE(kvmOf(own.wall, id).name, u"Renamed"_s);
        QVERIFY(one.stand->asked.isEmpty());
    }

    void aKvmOnTheWallOverHttpsIsNotChangedToPlainHttpAtItsPlace()
    {
        // Its card says that the certificate does not match: somebody sits on the line, or stands
        // where the KVM was, and answers the plain way too. Whoever types "http://" on that day
        // would hand that one the password in the clear, and the certificate would be forgotten.
        OwnWall own;
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        const QString id = put(own, at(kvm->stand->address(), u"Build server"_s, "first"));
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        const MadeUpKvm inTheClear({}, port);
        QVERIFY(inTheClear.stand->isListening());
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = inTheClear.stand->address();
        // With its password typed, and without.
        for (const QString& password : {u"secret"_s, QString()}) {
            typed.password = password;
            const auto refused = changedBy(editing, id, typed);
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().code, std::string("not_in_the_clear"));
            QCOMPARE(refused.error().message, std::string(notInTheClear));
        }
        // Before anything was asked there: not one line was made to it.
        QTest::qWait(150);
        QVERIFY(inTheClear.stand->asked.isEmpty());
        QCOMPARE(inTheClear.stand->lines, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QVERIFY(changed.isEmpty());
    }

    void aKvmOnTheWallOverPlainHttpIsChangedToHttpsAtItsPlaceAsToANewAddress()
    {
        // Its owner has turned HTTPS on: the other direction, in which nothing is given away.
        OwnWall own;
        auto plain = std::make_unique<MadeUpKvm>();
        const QString id = put(own, at(plain->stand->address(), u"Build server"_s));
        const quint16 port = plain->stand->serverPort();
        plain.reset();
        const MadeUpKvm encrypted(tls("first"), port);
        QVERIFY(encrypted.stand->isListening());
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = encrypted.stand->address();
        // The kept password goes to no address it was not saved for, as ever.
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("password_needed"));
        QCOMPARE(refused.error().message, std::string(nothingToKnowItBy));
        QCOMPARE(kvmOf(own.wall, id), before);
        // Typed, it is asked what it is, and no more: it was known by none, and what it shows
        // is put before somebody first. (This had it signed in to at once, and known by
        // whatever answered there: turned round.)
        typed.password = u"secret"_s;
        const KvmEditing::Outcome shown = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(shown), std::string("certificate_differs"));
        QVERIFY(shown.remembered.isEmpty());
        QCOMPARE(shown.presented, sha256Of("first"));
        QVERIFY(!shown.onWindowsWord);
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*encrypted.stand));
        QCOMPARE(kvmOf(own.wall, id), before);
        // With the yes to that one it is signed in to, and known by it.
        typed.saidYesTo = shown.presented;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QVERIFY(!saved.value().overPlainHttp);
        QCOMPARE(kvmOf(own.wall, id).address, encrypted.stand->address());
        QVERIFY(kvmOf(own.wall, id).knownByCertificate);
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("first"));
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", encrypted.stand->address(), sha256Of("first")));
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
    }

    void aKvmIsNotMovedOntoAnotherOneOfTheWall()
    {
        // One KVM reached two ways shows the one certificate it made for itself at both. Another
        // KVM of the wall moved to its second address would be that KVM a second time: two
        // drivers for one KVM, each keeping "one thing at a time" by itself.
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm there(tls("first"));
        const MadeUpKvm other(tls("second"));
        const QString twin = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const QString id = put(own, at(other.stand->address(), u"Other"_s, "second"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = there.stand->address();
        typed.password = u"secret"_s;
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("same_device"));
        QCOMPARE(QString::fromStdString(refused.error().message),
                 u"This is the same device as \"Build server\", which is on the wall already (127.0.0.1:%1)"_s.arg(
                     here.stand->serverPort()));
        // Said before its password went out there: asked what it is, and nothing after that.
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*there.stand));
        QCOMPARE(there.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QVERIFY(changed.isEmpty());

        // To its own second address the KVM itself is moved: it is in nobody's way but its own.
        typed = asItIs(own.wall, twin);
        typed.address = there.stand->address();
        typed.password = u"secret"_s;
        const auto saved = changedBy(editing, twin, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, twin).address, there.stand->address());
        QCOMPARE(kvmOf(own.wall, twin).certificate, sha256Of("first"));
        QCOMPARE(own.wall.kvms().size(), 2);
    }

    void aKvmIsNotMovedOntoOneThatCameOntoTheWallMeanwhile()
    {
        // While it is signed in to at its new address, the same KVM is put on the wall under its
        // other one: what was looked at before the KVM was spoken to is looked at once more.
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        MadeUpKvm there(tls("first"));
        Answered late = good({{u"hostname"_s, u"glkvm"_s}});
        late.afterMs = 600;
        there.first[namesQuestion].append(late);
        const MadeUpKvm other(tls("second"));
        const QString id = put(own, at(other.stand->address(), u"Other"_s, "second"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        QList<base::Result<KvmEditing::Changed>> told;
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = there.stand->address();
        typed.password = u"secret"_s;
        // (It shows another certificate there than this KVM is known by: somebody has looked
        // at the two, and said yes.)
        typed.saidYesTo = sha256Of("first");
        editing.change(id, typed, [&told](const KvmEditing::Outcome& outcome) { told.append(outcome.result); });
        QTRY_COMPARE(there.asked(signInQuestion), 1);
        QVERIFY(!put(own, at(here.stand->address(), u"Build server"_s, "first")).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QVERIFY(!told.constFirst().ok());
        QCOMPARE(told.constFirst().error().code, std::string("same_device"));
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
    }

    void aKvmIsNotMovedOntoAnotherOfTheWallThatWindowsVouchesForThere()
    {
        // From the third reading of the twins: what answers at the new address, or at a plain JetKVM's own HTTPS, shows
        // the certificate another KVM of the wall is known by, and Windows trusts that certificate there (it would be
        // known there by Windows' word). That is the other one all the same: "same_device", with the password typed and
        // with a yes to Windows' word, and nothing goes to it.
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        const core::testing::NobodyThere nobody;
        OwnWall own;
        QVERIFY(!put(own, at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"The other"_s, "signed")).isEmpty());
        // Moved to another HTTPS address.
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm door(tls("signed"));
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        JetKvmByHand plain;
        JetKvmByHand secure(tls("signed"));
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = twoWays(plain.address(), secure.address());
        KvmEditing editing(own.wall, pace);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = door.stand->address();
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        typed.saidYesTo = sha256Of("signed");
        typed.saidYesOnWindowsWord = true;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        QCOMPARE(door.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        // A plain JetKVM signed in to anew, at its own HTTPS.
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString jetId = put(own, jet);
        typed = asItIs(own.wall, jetId);
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, jetId, typed)), std::string("same_device"));
        QCOMPARE(secure.signIns, 0);
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(kvmOf(own.wall, jetId).address, plain.address().toString());
    }

    void aKvmIsNotMovedOntoOneThatCameOntoTheWallMeanwhileThatWindowsVouchesFor()
    {
        // From the third reading of the twins: moved, on a yes to Windows' word, to where Windows trusts what is shown;
        // while it is signed in to there, a KVM known by that very certificate comes onto the wall. Looked at once more
        // at its save, by what was shown: that one, and nothing is saved.
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        const core::testing::NobodyThere nobody;
        OwnWall own;
        MadeUpKvm door(tls("signed"));
        Answered late = good({{u"hostname"_s, u"glkvm"_s}});
        late.afterMs = 600;
        door.first[namesQuestion].append(late);
        const MadeUpKvm other(tls("second"));
        const QString id = put(own, at(other.stand->address(), u"Other"_s, "second"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        QList<base::Result<KvmEditing::Changed>> told;
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = door.stand->address();
        typed.password = u"secret"_s;
        typed.saidYesTo = sha256Of("signed");
        typed.saidYesOnWindowsWord = true;
        editing.change(id, typed, [&told](const KvmEditing::Outcome& outcome) { told.append(outcome.result); });
        QTRY_COMPARE(door.asked(signInQuestion), 1);
        QVERIFY(!put(own, at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"Build server"_s, "signed")).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QVERIFY(!told.constFirst().ok());
        QCOMPARE(told.constFirst().error().code, std::string("same_device"));
        QCOMPARE(kvmOf(own.wall, id), before);
    }

    void aKvmHeldBackForItsTwinIsLetGoOfByTheActsItsCardNames()
    {
        // From the third reading of the twins: plain JetKVMs held back for another KVM of the wall (their HTTPS,
        // answering as no JetKVM does, showed that one's certificate). Their card names two acts for a device that is
        // another one. Taken off and added again: the hold goes with it, and the adding asks anew, its sign-in looking
        // at the HTTPS as the wall's driver would; nothing goes while that shows the other one. And moved onto its own
        // HTTPS, with its password and a yes to what that shows: the hold goes too, and it is on the wall there.
        const auto notFound = [](const Asked&) {
            return Answered(404, R"({"error":"Not Found"})"_ba, "application/json"_ba);
        };
        const StandIn twinsHttps(notFound, tls("first"));
        const core::testing::NobodyThere nobody;
        const core::testing::NobodyThere nothingThere;
        JetKvmByHand moved;
        JetKvmByHand movedsOwn(tls("second"));
        JetKvmByHand again;
        bool itsOwnHttps = false;
        const auto seam = [&](const QUrl& address) -> std::optional<QUrl> {
            if (address == moved.address()) {
                return QUrl(twinsHttps.address());
            }
            if (address == again.address()) {
                return itsOwnHttps ? QUrl(u"https://127.0.0.1:%1"_s.arg(nothingThere.port()))
                                   : QUrl(twinsHttps.address());
            }
            if (address == movedsOwn.address()) {
                return moved.address();
            }
            return std::nullopt;
        };
        core::DriverPaces paces = core::DriverPaces::alike(quickDriver());
        paces.jetkvm.otherWay = seam;
        paces.jetkvm.connectWithinMs = 5000;
        OwnWall own(paces);
        OwnKvm other = at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"The other"_s, "first");
        other.kind = u"jetkvm"_s;
        const QString otherId = put(own, other);
        LocalWall::FromAFile one;
        one.kvm.address = moved.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        LocalWall::FromAFile two = one;
        two.kvm.address = again.address().toString();
        const QList<base::Result<QString>> took = own.wall.takeFromAFile({one, two});
        const QString movedId = took.at(0).value();
        const QString againId = took.at(1).value();
        QCOMPARE(onceItIs(own.wall, movedId, u"error"_s).errorCode, u"same_device"_s);
        QCOMPARE(onceItIs(own.wall, againId, u"error"_s).errorCode, u"same_device"_s);
        QTRY_COMPARE_WITH_TIMEOUT(core::Settings(own.file).remembered().kvmsHeldForATwin.value(againId), otherId,
                                  10000);
        QCOMPARE(core::Settings(own.file).remembered().kvmsHeldForATwin.value(movedId), otherId);
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;

        // Taken off: the hold goes with it, from what is remembered and from the file.
        QVERIFY(own.wall.remove(againId).ok());
        QVERIFY(!own.settings.remembered().kvmsHeldForATwin.contains(againId));
        QVERIFY(!core::Settings(own.file).remembered().kvmsHeldForATwin.contains(againId));
        // Added again, while its HTTPS shows the other one still: asked anew, and its sign-in sends nothing.
        core::KvmAdding adding(own.wall, pace);
        core::KvmAdding::Typed typing;
        typing.address = again.address().toString();
        typing.password = u"secret"_s;
        const auto added = [&adding, &typing] {
            QList<base::Result<core::KvmAdding::Added>> told;
            adding.add(typing, [&told](const base::Result<core::KvmAdding::Added>& result) { told.append(result); });
            return QTest::qWaitFor([&told] { return !told.isEmpty(); }, 20000)
                       ? told.constFirst()
                       : base::Result<core::KvmAdding::Added>(base::Error{"never", "The adding never ended."});
        };
        const base::Result<core::KvmAdding::Added> refused = added();
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("same_device"));
        QCOMPARE(again.signIns, 0);
        // Once its HTTPS is its own (here: nothing listens there), it is added, and held for nothing.
        itsOwnHttps = true;
        const base::Result<core::KvmAdding::Added> back = added();
        QVERIFY2(back.ok(), back.ok() ? "" : back.error().message.c_str());
        QCOMPARE(again.signIns, 1);
        QVERIFY(!own.settings.remembered().kvmsHeldForATwin.contains(back.value().id));
        QCOMPARE(onceItIs(own.wall, back.value().id, u"online"_s).state, u"online"_s);

        // Moved onto its own HTTPS: what that shows is put before somebody first, and nothing goes anywhere.
        KvmEditing editing(own.wall, pace);
        KvmEditing::Typed moving = asItIs(own.wall, movedId);
        moving.address = movedsOwn.address().toString();
        moving.password = u"secret"_s;
        const KvmEditing::Outcome shown = cameOf(editing, movedId, moving);
        QVERIFY(!shown.result.ok());
        QCOMPARE(shown.presented, sha256Of("second"));
        QCOMPARE(movedsOwn.signIns, 0);
        // With the yes to it: moved there, the hold gone, and on the wall there.
        moving.saidYesTo = shown.presented;
        moving.saidYesOnWindowsWord = shown.onWindowsWord;
        const KvmEditing::Outcome saved = cameOf(editing, movedId, moving);
        QVERIFY2(saved.result.ok(), saved.result.ok() ? "" : saved.result.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, movedId).address, movedsOwn.address().toString());
        QVERIFY(!own.settings.remembered().kvmsHeldForATwin.contains(movedId));
        QVERIFY(!core::Settings(own.file).remembered().kvmsHeldForATwin.contains(movedId));
        QCOMPARE(onceItIs(own.wall, movedId, u"online"_s).state, u"online"_s);
        QCOMPARE(moved.signIns, 0);
    }

    void aChangeIsNotSavedOverAPasswordThatWasChangedMeanwhile()
    {
        // Two boxes open on one KVM, in two windows. The first changes the user name, and signs
        // in for that with the password that is kept; the second saves another password
        // meanwhile. The list says nothing of that: the KVM stands in it as it stood. But what
        // the first box signed in with is not what is kept now, and the two were never tried
        // together.
        OwnWall own;
        MadeUpKvm kvm;
        Answered late = good({{u"hostname"_s, u"glkvm"_s}});
        late.afterMs = 600;
        kvm.first[namesQuestion].append(late);
        OwnKvm one = at(kvm.stand->address(), u"Build server"_s);
        one.user = u"root"_s;
        const QString id = put(own, one);
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing first(own.wall, quick());
        QList<base::Result<KvmEditing::Changed>> told;
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.user = u"admin"_s;
        first.change(id, typed, [&told](const KvmEditing::Outcome& outcome) { told.append(outcome.result); });
        QTRY_COMPARE(kvm.asked(signInQuestion), 1);
        QCOMPARE(kvm.last(signInQuestion)->body, "user=admin&passwd=secret"_ba);
        // (The second box's saving, as the wall hears of it: the KVM as it stands there, and
        // another password. The made-up KVM knows one user, and would not let that box sign in.)
        QVERIFY(own.wall.change(id, before, u"another one"_s).ok());
        QCOMPARE(kvmOf(own.wall, id), before);
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QVERIFY(!told.constFirst().ok());
        QCOMPARE(told.constFirst().error().code, std::string("conflict"));
        QCOMPARE(told.constFirst().error().message,
                 std::string("This device was changed meanwhile. Look at it again."));
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "another one"));

        // Looked at again, it is signed in to with the two together, and saved.
        kvm.password = u"another one"_s;
        const auto saved = changedBy(first, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(kvm.last(signInQuestion)->body, "user=admin&passwd=another%20one"_ba);
        QCOMPARE(kvmOf(own.wall, id).user, u"admin"_s);
    }

    void whatAChangeRemembersIsTold()
    {
        // The certificate the KVM is known by once it is changed, for the box to show beside
        // what the KVM's own page shows: as an adding tells it.
        OwnWall own;
        const MadeUpKvm plain;
        const MadeUpKvm encrypted(tls("first"));
        const MadeUpKvm sameAgain(tls("first"));
        const QString id = put(own, at(plain.stand->address(), u"Build server"_s));
        KvmEditing editing(own.wall, quick());
        // In the clear it is known by none.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"Renamed"_s;
        auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().certificate.isEmpty());
        // At a new address, with its password typed: by the one it showed there, which somebody
        // said yes to.
        typed.address = encrypted.stand->address();
        typed.password = u"secret"_s;
        typed.saidYesTo = sha256Of("first");
        saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(saved.value().certificate, sha256Of("first"));
        QCOMPARE(saved.value().certificate, kvmOf(own.wall, id).certificate);
        // A new group alone, and then the kept password gone with it to its other address: by
        // the one it keeps.
        typed = asItIs(own.wall, id);
        typed.group = u"Rack 2"_s;
        saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(saved.value().certificate, sha256Of("first"));
        typed.address = sameAgain.stand->address();
        saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(saved.value().certificate, sha256Of("first"));
        // And back in the clear, somewhere else: by none again. (With the yes to that: this had
        // the password sent there with nobody asked. Turned round.)
        typed.address = plain.stand->address();
        typed.password = u"secret"_s;
        typed.saidYesToPlainHttpAt = u"127.0.0.1:%1"_s.arg(plain.stand->serverPort());
        saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().overPlainHttp);
        QVERIFY(saved.value().certificate.isEmpty());
    }

    void whatIsNoKvmAtANewAddressIsSaidAndHearsNoPassword_data()
    {
        QTest::addColumn<QByteArray>("what");
        QTest::addColumn<QByteArray>("code");
        QTest::addColumn<QString>("sentence");
        QTest::newRow("a page") << "page"_ba << "not_a_kvm"_ba
                                << u"Something answers at this address, but it is not a KVM that this program "
                                   "speaks to"_s;
        QTest::newRow("a JetKVM") << "jetkvm"_ba << "other_family"_ba
                                  << u"A JetKVM answers at this address, and this KVM is on the wall as a GL.iNet "
                                     "Comet / PiKVM. To have the JetKVM here, add it as a KVM of its own."_s;
    }

    void whatIsNoKvmAtANewAddressIsSaidAndHearsNoPassword()
    {
        QFETCH(QByteArray, what);
        QFETCH(QByteArray, code);
        QFETCH(QString, sentence);
        OwnWall own;
        const MadeUpKvm kvm;
        const StandIn stand(what == "page" ? asAPage : asAJetKvm, tls("first"));
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = stand.address();
        typed.password = u"secret"_s;
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(QByteArray::fromStdString(refused.error().code), code);
        QCOMPARE(QString::fromStdString(refused.error().message), sentence);
        // Asked what it is, and nothing after that.
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(stand));
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
    }

    void aJetKvmIsSignedInToByItsOwnDriverWhenItIsChanged()
    {
        OwnWall own;
        // A password typed anew, at the address it has: tried by a JetKVM's driver, as a JetKVM is
        // signed in to, and saved.
        JetKvmByHand plain;
        plain.password = u"newer"_s;
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        QVERIFY(!id.isEmpty());
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"newer"_s;
        auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QCOMPARE(plain.signInBodies, QList<QByteArray>{R"({"password":"newer"})"_ba});
        QCOMPARE(plain.stand->count(signInQuestion), 0);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "newer"));
        QCOMPARE(kvmOf(own.wall, id).kind, u"jetkvm"_s);
        // Moved, with a password, to where a kvmd answers: of another family, and it hears nothing.
        const MadeUpKvm kvmd;
        typed.address = kvmd.stand->address();
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("other_family"));
        QCOMPARE(refused.error().message,
                 std::string("A GL.iNet Comet / PiKVM answers at this address, and this KVM is on the wall as a "
                             "JetKVM. To have the GL.iNet Comet / PiKVM here, add it as a KVM of its own."));
        QVERIFY(heardOnlyTheQuestions(*kvmd.stand));
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());

        // Its kept password goes with it to another HTTPS address only where what answers there shows
        // the certificate it is known by, and is then tried by a JetKVM's driver.
        JetKvmByHand first(tls("first"));
        const QString encrypted = put(own, [&] {
            OwnKvm one = at(first.address().toString(), u"Encrypted"_s, "first");
            one.kind = u"jetkvm"_s;
            return one;
        }());
        QVERIFY(!encrypted.isEmpty());
        JetKvmByHand second(tls("second"));
        KvmEditing::Typed moving = asItIs(own.wall, encrypted);
        moving.address = second.address().toString();
        const auto notThere = changedBy(editing, encrypted, moving);
        QVERIFY(!notThere.ok());
        QCOMPARE(notThere.error().code, std::string("password_needed"));
        QCOMPARE(second.signIns, 0);
        QVERIFY(second.stand->asked.isEmpty());
        JetKvmByHand again(tls("first"));
        moving.address = again.address().toString();
        saved = changedBy(editing, encrypted, moving);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(again.signInBodies, QList<QByteArray>{R"({"password":"secret"})"_ba});
        QCOMPARE(again.stand->count(signInQuestion), 0);
        QCOMPARE(kvmOf(own.wall, encrypted).address, again.address().toString());
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(encrypted)), savedAs(own.wall, encrypted, "secret"));
        // And not to a plain address, where nothing could show it.
        moving.address = u"http://127.0.0.1:%1"_s.arg(first.stand->serverPort());
        QCOMPARE(changedBy(editing, encrypted, moving).error().code, std::string("password_needed"));
    }

    void aJetKvmMovedWithATypedPasswordIsSignedInToAsAJetKvmThere()
    {
        OwnWall own;
        JetKvmByHand before;
        JetKvmByHand after;
        after.password = u"newer"_s;
        OwnKvm jet = at(before.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        QVERIFY(!id.isEmpty());
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = after.address().toString();
        // Signed in to by a JetKVM's driver: and a password it refused a moment ago is tried at once when it
        // is typed again, as a JetKVM's driver counts it.
        typed.password = u"wrong"_s;
        QCOMPARE(changedBy(editing, id, typed).error().code, std::string("auth_failed"));
        QCOMPARE(after.signIns, 1);
        QCOMPARE(changedBy(editing, id, typed).error().code, std::string("auth_failed"));
        QCOMPARE(after.signIns, 2);
        typed.password = u"newer"_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QCOMPARE(after.signInBodies.last(), R"({"password":"newer"})"_ba);
        QCOMPARE(after.stand->count(signInQuestion), 0);
        QCOMPARE(kvmOf(own.wall, id).address, after.address().toString());
        QCOMPARE(kvmOf(own.wall, id).kind, u"jetkvm"_s);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "newer"));
        QCOMPARE(before.signIns, 0);
    }

    void aKvmMovedWithItsKeptPasswordWhereOneOfTheOtherFamilyAnswersIsNotMoved()
    {
        // A kvmd from a file, not asked yet, moved with the password that is kept to where a JetKVM shows the
        // certificate it is known by: of another family, said as at a new address (it is added as a KVM of its
        // own), and nothing but the three questions went there.
        OwnWall own;
        const core::testing::NobodyThere nobody;
        JetKvmByHand jet(tls("first"));
        LocalWall::FromAFile one;
        one.kvm = at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"Build server"_s, "first");
        one.password = u"secret"_s;
        const QList<base::Result<QString>> took = own.wall.takeFromAFile({one});
        QVERIFY(took.constFirst().ok());
        const QString& id = took.constFirst().value();
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = jet.address().toString();
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("other_family"));
        QCOMPARE(refused.error().message,
                 std::string("A JetKVM answers at this address, and this KVM is on the wall as a GL.iNet Comet / "
                             "PiKVM. To have the JetKVM here, add it as a KVM of its own. No password was sent to "
                             "it."));
        QVERIFY(heardOnlyTheQuestions(*jet.stand));
        QCOMPARE(jet.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id).address, one.kvm.address);
    }

    // --- a JetKVM's two ways (M6 1b-ii) --------------------------------------------------------------

    void aPlainJetKvmSignedInToAnewWithItsKeptPasswordIsAskedOverHttpsFirst()
    {
        // As the web's: whenever it is signed in to again, with the password typed or the one kept, a JetKVM
        // over plain HTTP is asked over HTTPS first. Where a JetKVM answers there, its kept password goes
        // there, onto HTTPS at its own host (the one way it goes to another address without being typed), and
        // it is known by what it showed there.
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        const auto seam = twoWays(plain.address(), secure.address());
        core::DriverPaces paces = core::DriverPaces::alike(quickDriver());
        paces.jetkvm.otherWay = seam;
        OwnWall own(paces);
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        KvmEditing editing(own.wall, pace);
        // Another user name, at the address it has: signed in to anew with the kept password.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.user = u"root"_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QVERIFY(saved.value().ontoHttps);
        QCOMPARE(saved.value().certificate, sha256Of("first"));
        QCOMPARE(kvmOf(own.wall, id).address, secure.address().toString());
        QCOMPARE(kvmOf(own.wall, id).user, u"root"_s);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", secure.address().toString(), sha256Of("first")));
        QVERIFY(plain.stand->asked.isEmpty());
        QCOMPARE(secure.signInBodies, QList<QByteArray>{R"({"password":"secret"})"_ba});
    }

    void aPlainJetKvmWhoseHttpsShowsAnotherOfTheWallHearsNoPasswordFromTheBox()
    {
        // From the third reading of the twins (MEDIUM 1): a plain JetKVM of the wall whose own HTTPS shows the
        // certificate another KVM of the wall is known by, answering as no JetKVM does (a 404 here): that other one may
        // be what answers its plain way too (an address handed out anew). No sign-in of the box sends a password there:
        // not one typed at the address it has, not the kept one under another user name, and not one typed for another
        // plain address whose HTTPS shows that other one. Each looks at the HTTPS first, as the wall's own driver does
        // (JetKvmDevice::lookAtItsHttpsBeforeEverySignIn), and is refused in the wall's words.
        const auto notFound = [](const Asked&) {
            return Answered(404, R"({"error":"Not Found"})"_ba, "application/json"_ba);
        };
        JetKvmByHand plain;
        const StandIn itsHttps(notFound, tls("first"));
        JetKvmByHand elsewhere;
        const StandIn elsewhereHttps(notFound, tls("first"));
        const core::testing::NobodyThere nobody;
        const auto seam =
            [ways = std::array{twoWays(plain.address(), QUrl(itsHttps.address())),
                               twoWays(elsewhere.address(), QUrl(elsewhereHttps.address()))}](const QUrl& address) {
                const std::optional<QUrl> one = ways[0](address);
                return one ? one : ways[1](address);
            };
        OwnWall own;
        OwnKvm other = at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"The other"_s, "first");
        other.kind = u"jetkvm"_s;
        QVERIFY(!put(own, other).isEmpty());
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        QVERIFY(own.wall.wasAskedWhatItIs(id));
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        KvmEditing editing(own.wall, pace);
        // A password typed at the address it has.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        // The kept one, under another user name.
        typed = asItIs(own.wall, id);
        typed.user = u"root"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        // One typed for another plain address, whose HTTPS shows that other one too.
        typed = asItIs(own.wall, id);
        typed.address = elsewhere.address().toString();
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(elsewhere.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
    }

    void aTwinTheBoxSawIsNotForgottenWhereThatHttpsFallsSilentAtTheSamePort()
    {
        // From the outside review (F1, HIGH): a plain JetKVM of the wall is changed. Its own HTTPS shows the
        // certificate another KVM of the wall is known by, answering as no JetKVM does (a 404), and then, at that
        // very port, takes the line and says nothing. What the first look saw is not forgotten by what the next one
        // does not see: judged there and then, it holds the KVM back for that other one, on the wall and for the
        // next start; and a look in doubt keeps the hold. No password goes in the clear, at that change or the next.
        JetKvmByHand plain;
        core::testing::FallsSilent itsHttps(tls("first"));
        JetKvmByHand elsewhere;
        core::testing::FallsSilent elsewhereHttps(tls("second"));
        const auto seam = [ways = std::array{twoWays(plain.address(), QUrl(itsHttps.address())),
                                             twoWays(elsewhere.address(), QUrl(elsewhereHttps.address()))}](
                              const QUrl& address) -> std::optional<QUrl> {
            const std::optional<QUrl> one = ways[0](address);
            return one ? one : ways[1](address);
        };
        // (The wall's own drivers do not look at that HTTPS here: what holds it back is the box's look alone.)
        OwnWall own;
        const core::testing::NobodyThere nobody;
        OwnKvm other = at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"The other"_s, "first");
        other.kind = u"jetkvm"_s;
        const QString otherId = put(own, other);
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        QVERIFY(own.wall.wasAskedWhatItIs(id));
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        KvmEditing editing(own.wall, pace);
        // A password typed at the address it has: its HTTPS shows the other one, and falls silent after that.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        QCOMPARE(itsHttps.answered(), qsizetype{1});
        QVERIFY(itsHttps.fellSilent());
        QCOMPARE(plain.signIns, 0);
        // Held back for that one from now on: its driver has no password, and the hold is kept for the next start.
        QCOMPARE(own.settings.remembered().kvmsHeldForATwin.value(id), otherId);
        QTRY_VERIFY(!own.wall.wasAskedWhatItIs(id));
        // The next change looks at that HTTPS again, which says nothing now: in doubt, it stays held.
        const KvmEditing::Outcome again = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(again), std::string("same_device"));
        QVERIFY(itsHttps.calledSilently());
        QCOMPARE(QString::fromStdString(sentenceOf(again)),
                 u"This may be the same device as \"The other\", which is on the wall already (127.0.0.1:%1): its "
                 "HTTPS showed the certificate that one is known by, and no password is sent here while that lasts. "
                 "If it is that device, one of the two can go: remove this one or that one. If it is another device, "
                 "remove this one and add it again, or turn on HTTPS on the JetKVM and change its address here to "
                 "https://."_s.arg(nobody.port()));
        // And so with the kept one, under another user name.
        typed = asItIs(own.wall, id);
        typed.user = u"root"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        // Nor is it moved in the clear to another plain address where a look in doubt is the last there is: the HTTPS
        // there showed a certificate no KVM of the wall is known by, and then nothing on the look of the sign-in
        // itself, which is held to the hold as the box is.
        typed = asItIs(own.wall, id);
        typed.address = elsewhere.address().toString();
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        QCOMPARE(elsewhereHttps.answered(), qsizetype{1});
        QVERIFY(elsewhereHttps.calledSilently());
        QCOMPARE(elsewhere.signIns, 0);
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
        QCOMPARE(own.settings.remembered().kvmsHeldForATwin.value(id), otherId);
    }

    void aTwinTheBoxSawWhereBothWaysAreAskedIsNotForgottenEither()
    {
        // From the outside review (F1): where the change box asks both ways of an address, what each HTTPS showed is
        // judged, whatever it answered, before a way is chosen. Moved to another plain address whose HTTPS shows the
        // certificate another KVM of the wall is known by, and then falls silent: nothing goes there, and this KVM is
        // not held for it (it is not its own HTTPS). Typed at its own HTTPS, which shows that one and answers as no
        // JetKVM does: held back for it, as where the wall finds it so.
        JetKvmByHand plain;
        core::testing::FallsSilent itsHttps(tls("first"));
        JetKvmByHand elsewhere;
        core::testing::FallsSilent elsewhereHttps(tls("first"));
        const auto seam = [ways = std::array{twoWays(plain.address(), QUrl(itsHttps.address())),
                                             twoWays(elsewhere.address(), QUrl(elsewhereHttps.address()))}](
                              const QUrl& address) -> std::optional<QUrl> {
            const std::optional<QUrl> one = ways[0](address);
            return one ? one : ways[1](address);
        };
        OwnWall own;
        const core::testing::NobodyThere nobody;
        OwnKvm other = at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"The other"_s, "first");
        other.kind = u"jetkvm"_s;
        const QString otherId = put(own, other);
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        KvmEditing editing(own.wall, pace);
        // Moved, with a password typed for it, to another plain address.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = elsewhere.address().toString();
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        QCOMPARE(elsewhereHttps.answered(), qsizetype{1});
        QVERIFY(elsewhereHttps.fellSilent());
        QCOMPARE(elsewhere.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
        QVERIFY(!own.settings.remembered().kvmsHeldForATwin.contains(id));
        QVERIFY(own.wall.wasAskedWhatItIs(id));
        // Typed at its own HTTPS.
        typed = asItIs(own.wall, id);
        typed.address = itsHttps.address();
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        QVERIFY(itsHttps.answered() > 0);
        QCOMPARE(own.settings.remembered().kvmsHeldForATwin.value(id), otherId);
        QTRY_VERIFY(!own.wall.wasAskedWhatItIs(id));
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
    }

    void aTwinTheBoxSawHoldsTheKvmBackFromTheNextStart()
    {
        // From the outside review (F1): the hold a change box noted is kept with the KVM, as the wall's own is. After a
        // start it is held from the first moment, also where its HTTPS says nothing at all by now (in doubt), and
        // neither the wall nor a box sends its password in the clear. A sign lets go of it: a certificate no KVM of
        // the wall is known by, on an answer that is no JetKVM's.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        JetKvmByHand plain;
        core::testing::FallsSilent itsHttps(tls("first"));
        const StandIn another(
            [](const Asked&) { return Answered(503, R"({"error":"Service Unavailable"})"_ba, "application/json"_ba); },
            tls("second"));
        bool aSign = false;
        const auto seam = [from = plain.address(), to = QUrl(itsHttps.address()), elsewhere = QUrl(another.address()),
                           &aSign](const QUrl& address) {
            return address == from ? std::optional<QUrl>(aSign ? elsewhere : to) : std::nullopt;
        };
        const core::testing::NobodyThere nobody;
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        QString id;
        QString otherId;
        {
            // (Its drivers do not look at that HTTPS in this run: the box's look is what holds it back.)
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quickDriver());
            OwnKvm other = at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"The other"_s, "first");
            other.kind = u"jetkvm"_s;
            otherId = wall.add(other, u"its own"_s).value();
            OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
            jet.kind = u"jetkvm"_s;
            id = wall.add(jet, u"secret"_s).value();
            KvmEditing editing(wall, pace);
            KvmEditing::Typed typed = asItIs(wall, id);
            typed.password = u"secret"_s;
            QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
            QVERIFY(itsHttps.fellSilent());
        }
        QCOMPARE(core::Settings(file).remembered().kvmsHeldForATwin.value(id), otherId);
        // The next start, its drivers looking at that HTTPS as the program's do.
        core::DriverPaces paces = core::DriverPaces::alike(quickDriver());
        paces.jetkvm.otherWay = seam;
        paces.jetkvm.connectWithinMs = 5000;
        core::Settings settings(file);
        LocalWall wall(settings, secrets, paces);
        QCOMPARE(onceItIs(wall, id, u"error"_s).errorCode, u"same_device"_s);
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < 3000) {
            QCOMPARE(onceItIs(wall, id, u"error"_s).errorCode, u"same_device"_s);
            QTest::qWait(100);
        }
        QVERIFY(itsHttps.calledSilently());
        QCOMPARE(plain.signIns, 0);
        KvmEditing editing(wall, pace);
        KvmEditing::Typed typed = asItIs(wall, id);
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("same_device"));
        QCOMPARE(plain.signIns, 0);
        // A sign lets go of it: on the wall as the JetKVM its plain way answers, and what was kept of the hold goes.
        aSign = true;
        const core::Status well = onceItIs(wall, id, u"online"_s);
        QVERIFY2(well.state == u"online"_s, qPrintable(well.state + u' ' + well.errorCode + u' ' + well.errorMessage));
        QVERIFY(wall.wasAskedWhatItIs(id));
        QTRY_VERIFY_WITH_TIMEOUT(!core::Settings(file).remembered().kvmsHeldForATwin.contains(id), 10000);
    }

    void aPlainJetKvmMovedToItsOwnHttpsTakesItsKeptPasswordAlongAndToNoOtherAddress()
    {
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        JetKvmByHand plainToo;
        JetKvmByHand elsewhere(tls("first"));
        const auto seam = [ways = std::array{twoWays(plain.address(), secure.address()),
                                             twoWays(plainToo.address(), elsewhere.address())}](const QUrl& address) {
            const std::optional<QUrl> one = ways[0](address);
            return one ? one : ways[1](address);
        };
        core::DriverPaces paces = core::DriverPaces::alike(quickDriver());
        paces.jetkvm.otherWay = seam;
        OwnWall own(paces);
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        KvmEditing editing(own.wall, pace);
        // Moved to its own HTTPS, with nothing typed: what answers there is asked first, and its kept
        // password goes along to what showed that.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = secure.address().toString();
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().ontoHttps);
        QCOMPARE(kvmOf(own.wall, id).address, secure.address().toString());
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("first"));
        QCOMPARE(secure.signIns, 1);
        QVERIFY(plain.stand->asked.isEmpty());
        // To an HTTPS address that is not its own other way, the kept password stays behind, as ever.
        OwnKvm other = at(plainToo.address().toString(), u"Another"_s);
        other.kind = u"jetkvm"_s;
        const QString otherId = put(own, other);
        KvmEditing::Typed moving = asItIs(own.wall, otherId);
        // (The other KVM's HTTPS, where that one is now, is taken.)
        moving.address = secure.address().toString();
        QCOMPARE(refusalOf(cameOf(editing, otherId, moving)), std::string("duplicate"));
        JetKvmByHand stranger(tls("first"));
        moving.address = stranger.address().toString();
        QCOMPARE(refusalOf(cameOf(editing, otherId, moving)), std::string("password_needed"));
        QVERIFY(stranger.stand->asked.isEmpty());
        QCOMPARE(plainToo.signIns, 0);
        // And where the wall does not have that HTTPS as its other way (its own rule, whatever the box was
        // given), the kept password stays behind there too.
        OwnWall strict;
        const QString strictId = put(strict, other);
        KvmEditing strictEditing(strict.wall, pace);
        KvmEditing::Typed toItsHttps = asItIs(strict.wall, strictId);
        toItsHttps.address = elsewhere.address().toString();
        QCOMPARE(refusalOf(cameOf(strictEditing, strictId, toItsHttps)), std::string("password_needed"));
        QCOMPARE(elsewhere.signIns, 0);
    }

    void aJetKvmIsMovedOrNotByTheAddressChosenAndNotByHowItWasTyped()
    {
        // Not asked yet (from a file), at http://h: its password typed with "http://h", or with "h" where only
        // the plain way answers, comes to the same: the address chosen is the one it has, and nothing is signed
        // in to before the wall has asked.
        OwnWall own;
        JetKvmByHand plain;
        LocalWall::FromAFile one;
        one.kvm.address = plain.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = own.wall.takeFromAFile({one}).constFirst().value();
        KvmEditing editing(own.wall, roomForTls());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("not_asked_yet"));
        typed.address = u"127.0.0.1:%1"_s.arg(plain.stand->serverPort());
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("not_asked_yet"));
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(plain.lineAskings, 0);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
    }

    void theAddressChosenIsHeldToWhatATypedOneIsHeldTo()
    {
        // (Of addresses that reach nothing: nothing is asked of them.)
        OwnWall own;
        OwnKvm encrypted = at(u"https://192.0.2.5"_s, u"Small one"_s, "first");
        encrypted.kind = u"jetkvm"_s;
        const QString id = put(own, encrypted);
        OwnKvm there = at(u"http://192.0.2.9"_s, u"There"_s);
        there.kind = u"jetkvm"_s;
        QVERIFY(!put(own, there).isEmpty());
        const OwnKvm before = kvmOf(own.wall, id);
        const auto at = [&before](const QString& address) {
            OwnKvm after = before;
            after.address = address;
            after.knownByCertificate = false;
            after.certificate.clear();
            return after;
        };
        const auto refusal = [&](const QString& address, const QString& yes = {}) {
            const std::optional<KvmEditing::Outcome> said =
                KvmEditing::refusalAtTheWayChosen(own.wall, before, at(address), yes);
            return said ? said->result.error().code : std::string("none");
        };
        QCOMPARE(refusal(u"http://192.0.2.5"_s), std::string("not_in_the_clear"));
        QCOMPARE(refusal(u"http://192.0.2.5"_s, u"192.0.2.5"_s), std::string("not_in_the_clear"));
        QCOMPARE(refusal(u"http://192.0.2.9"_s), std::string("duplicate"));
        QCOMPARE(refusal(u"https://192.0.2.9"_s), std::string("duplicate"));
        QCOMPARE(refusal(u"http://192.0.2.6"_s), std::string("plain_http_there"));
        const std::optional<KvmEditing::Outcome> givingUp =
            KvmEditing::refusalAtTheWayChosen(own.wall, before, at(u"http://192.0.2.6"_s), {});
        if (!givingUp) {
            QFAIL("A move to plain HTTP at another place was not put before anybody.");
        }
        const KvmEditing::Outcome& gaveUp = *givingUp;
        QCOMPARE(gaveUp.remembered, sha256Of("first"));
        QCOMPARE(gaveUp.presentedAt, u"192.0.2.6"_s);
        QVERIFY(gaveUp.presented.isEmpty());
        QCOMPARE(refusal(u"http://192.0.2.6"_s, u"192.0.2.6"_s), std::string("none"));
        QCOMPARE(refusal(u"http://192.0.2.6"_s, u"192.0.2.7"_s), std::string("plain_http_there"));
        QCOMPARE(refusal(u"https://192.0.2.7"_s), std::string("none"));
        // One spoken to the plain way already gives nothing up at another plain address.
        const OwnKvm plainBefore = kvmOf(own.wall, own.wall.kvms().at(1).id);
        OwnKvm plainAfter = plainBefore;
        plainAfter.address = u"http://192.0.2.8"_s;
        QVERIFY(!KvmEditing::refusalAtTheWayChosen(own.wall, plainBefore, plainAfter, {}).has_value());
    }

    void aJetKvmNotAskedYetDoesNotTakeItsKeptPasswordOntoItsOwnHttps()
    {
        // Taken from a file at http://h with a password, and not asked yet; a JetKVM answers at https://h. Moved
        // there in the box with nothing typed: what answers there is asked, and the kept password stays behind
        // all the same. (The wall asks whether it was asked before it makes its one exception.)
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        const auto seam = twoWays(plain.address(), secure.address());
        core::DriverPaces paces = core::DriverPaces::alike(quickDriver());
        paces.jetkvm.otherWay = seam;
        OwnWall own(paces);
        LocalWall::FromAFile one;
        one.kvm.address = plain.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = own.wall.takeFromAFile({one}).constFirst().value();
        const QHash<QString, QByteArray> storeBefore = own.secrets.all;
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        KvmEditing editing(own.wall, pace);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = secure.address().toString();
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("not_asked_yet"));
        QCOMPARE(secure.signIns, 0);
        QVERIFY(secure.signInBodies.isEmpty());
        QVERIFY(plain.stand->asked.isEmpty());
        QCOMPARE(own.secrets.all, storeBefore);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
    }

    void aJetKvmWithNoOtherWayGivenIsSignedInToAsTyped()
    {
        // (A Pace whose otherWay is none: nothing is asked beside the address, and the change goes on.)
        OwnWall own;
        JetKvmByHand plain;
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        KvmEditing::Pace pace = quick();
        pace.otherWay = {};
        KvmEditing editing(own.wall, pace);
        // Its password typed at the address it has: signed in to there.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(!saved.value().ontoHttps);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
        QCOMPARE(plain.signIns, 1);
        // Moved with its password typed "http://": asked that way alone, and moved there.
        JetKvmByHand elsewhere;
        typed = asItIs(own.wall, id);
        typed.address = elsewhere.address().toString();
        typed.password = u"secret"_s;
        const auto moved = changedBy(editing, id, typed);
        QVERIFY2(moved.ok(), moved.ok() ? "" : moved.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).address, elsewhere.address().toString());
        QCOMPARE(elsewhere.signIns, 1);
    }

    void aPlainJetKvmWhoseWayChosenIsItsOwnHttpsIsKnownThereByWhatItShowedFirst()
    {
        // At http://h, a JetKVM answering at https://h too. Its password typed with "h", where the way chosen is
        // its own HTTPS (its other way, as the wall's exception has it), comes to what the kept password or one
        // typed with "http://h" comes to: signed in to there, known by what it showed on that first contact.
        // Any other HTTPS chosen is a new address: its certificate is put before somebody first.
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        const auto seam = twoWays(plain.address(), secure.address());
        core::DriverPaces paces = core::DriverPaces::alike(quickDriver());
        paces.jetkvm.otherWay = seam;
        OwnWall own(paces);
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        KvmEditing editing(own.wall, pace);
        // Another JetKVM over HTTPS, with nothing before its address: another address, and so the yes first.
        JetKvmByHand stranger(tls("second"));
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = u"127.0.0.1:%1"_s.arg(stranger.stand->serverPort());
        typed.password = u"secret"_s;
        const KvmEditing::Outcome differs = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(differs), std::string("certificate_differs"));
        QCOMPARE(differs.presented, sha256Of("second"));
        QCOMPARE(stranger.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
        // Its own HTTPS, with nothing before the address (as "h" is typed; here with the port it has).
        typed.address = u"127.0.0.1:%1"_s.arg(secure.stand->serverPort());
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QVERIFY(saved.value().ontoHttps);
        QCOMPARE(saved.value().certificate, sha256Of("first"));
        const OwnKvm now = kvmOf(own.wall, id);
        QCOMPARE(now.address, secure.address().toString());
        QVERIFY(now.knownByCertificate);
        QCOMPARE(now.certificate, sha256Of("first"));
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", secure.address().toString(), sha256Of("first")));
        QCOMPARE(secure.signInBodies, QList<QByteArray>{R"({"password":"secret"})"_ba});
        QCOMPARE(plain.signIns, 0);
    }

    void aJetKvmWithNoneKeptChosenAtItsOwnHttpsIsSignedInToThereAsOneThatNeedsNone()
    {
        // Out of its box, from a file and not asked yet: moved with nothing typed to its own HTTPS, both ways
        // are asked first, and it is signed in to there as one that needs none, known by what it showed on that
        // first contact, as at any address it is moved to without one. (No password goes anywhere.)
        JetKvmByHand plain;
        plain.password.clear();
        JetKvmByHand secure(tls("first"));
        secure.password.clear();
        const auto seam = twoWays(plain.address(), secure.address());
        core::DriverPaces paces = core::DriverPaces::alike(quickDriver());
        paces.jetkvm.otherWay = seam;
        OwnWall own(paces);
        LocalWall::FromAFile one;
        one.kvm.address = plain.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        const QString id = own.wall.takeFromAFile({one}).constFirst().value();
        QVERIFY(!own.wall.marksAPasswordFor(id));
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        KvmEditing editing(own.wall, pace);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = secure.address().toString();
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QVERIFY(saved.value().ontoHttps);
        const OwnKvm now = kvmOf(own.wall, id);
        QCOMPARE(now.address, secure.address().toString());
        QCOMPARE(now.certificate, sha256Of("first"));
        QCOMPARE(secure.signIns, 0);
        QCOMPARE(secure.lineCookies, QList<QByteArray>{"(none)"_ba});
        QVERIFY(!own.wall.marksAPasswordFor(id));
    }

    void aJetKvmsNumberIsKeptWhereItCouldNotBeReadThisOnce()
    {
        // From the reading of the twins (L1): its own number not handed out this once (GET /device answered 500) is
        // no number of none: the one that is kept stays, as the web keeps its ident. And as the web's edit with the
        // password typed (`take`, J5): which JetKVM answers could not be told, and nothing is saved.
        OwnWall own;
        JetKvmByHand jet;
        jet.deviceFails = true;
        OwnKvm one = at(jet.address().toString(), u"Rack JetKVM"_s);
        one.kind = u"jetkvm"_s;
        one.number = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        const QString id = put(own, one);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        typed.name = u"Renamed"_s;
        const KvmEditing::Outcome refused = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(refused), std::string("bad_response"));
        QVERIFY(sentenceOf(refused).starts_with("Which JetKVM answers at this address could not be told: "));
        QCOMPARE(kvmOf(own.wall, id).number, u"jetkvm:0123456789abcdef0123456789abcdef"_s);
        QCOMPARE(kvmOf(own.wall, id).name, u"Rack JetKVM"_s);
        QCOMPARE(jet.lineAskings, 0);
    }

    void anEditWithThePasswordTypedTakesTheJetKvmThatAnswersNow()
    {
        // J5, as the web's edit (`take`): whoever types the password says "this is it". The JetKVM that answers at the
        // card's address now gives another number than the one the card is known by: it is taken, and saved.
        OwnWall own;
        JetKvmByHand jet;
        OwnKvm one = at(jet.address().toString(), u"Rack JetKVM"_s);
        one.kind = u"jetkvm"_s;
        one.number = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        const QString id = put(own, one);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).number, digestOf(jet.deviceId.toString()));
    }

    void anEditSignedInWithTheKeptPasswordIsHeldToTheJetKvmItIsKnownBy()
    {
        // J5: signed in to with the password that is kept (the wall's signerIn: here for another user name), what
        // answers is held to the number the card is known by. Another JetKVM at its address is refused, and nothing is
        // saved.
        OwnWall own;
        JetKvmByHand jet;
        OwnKvm one = at(jet.address().toString(), u"Rack JetKVM"_s);
        one.kind = u"jetkvm"_s;
        one.number = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        const QString id = put(own, one);
        QVERIFY(QTest::qWaitFor([&] { return own.wall.wasAskedWhatItIs(id); }, 15000));
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.user = u"someone"_s;
        const KvmEditing::Outcome refused = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(refused), std::string("another_machine"));
        QCOMPARE(kvmOf(own.wall, id).user, one.user);
        QCOMPARE(kvmOf(own.wall, id).number, one.number);
        QCOMPARE(jet.lineAskings, 0);
    }

    void anEditWithNoPasswordTypedIsHeldToTheJetKvmItIsKnownBy()
    {
        // J5: a JetKVM that runs without a password, moved with none typed, is not "this is it": what answers at the
        // new address is held to the number the card is known by (the web's connect without `take`). Another JetKVM
        // there is refused "another_machine", sent nothing more, and nothing is saved.
        OwnWall own;
        JetKvmByHand was;
        was.password.clear();
        OwnKvm one = at(was.address().toString(), u"Small one"_s);
        one.kind = u"jetkvm"_s;
        one.number = digestOf(was.deviceId.toString());
        const QString id = own.wall.add(one, QString()).value();
        JetKvmByHand other;
        other.password.clear();
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = other.address().toString();
        const KvmEditing::Outcome refused = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(refused), std::string("another_machine"));
        QCOMPARE(sentenceOf(refused), core::JetKvmDevice::anotherJetKvm().message);
        QCOMPARE(other.lineAskings, 0);
        QCOMPARE(kvmOf(own.wall, id).address, was.address().toString());
        QCOMPARE(kvmOf(own.wall, id).number, digestOf(was.deviceId.toString()));
    }

    void aJetKvmThatSaysItIsAnotherOfTheWallIsKeptBesideItWhereThatIsSaid()
    {
        // From the reading of the twins (L3): as when it is added, the person may keep both.
        OwnWall own;
        JetKvmByHand first;
        JetKvmByHand second;
        second.deviceId = first.deviceId;
        OwnKvm one = at(first.address().toString(), u"Rack JetKVM"_s);
        one.kind = u"jetkvm"_s;
        one.number = digestOf(first.deviceId.toString());
        QVERIFY(!put(own, one).isEmpty());
        OwnKvm two = at(second.address().toString(), u"Another"_s);
        two.kind = u"jetkvm"_s;
        const QString twoId = put(own, two);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, twoId);
        typed.password = u"secret"_s;
        const KvmEditing::Outcome refused = cameOf(editing, twoId, typed);
        QCOMPARE(refusalOf(refused), std::string("same_device"));
        QVERIFY(LocalWall::saysItIsTheSame(refused.result.error()));
        typed.keepBoth = true;
        const auto kept = changedBy(editing, twoId, typed);
        QVERIFY2(kept.ok(), kept.ok() ? "" : kept.error().message.c_str());
        QVERIFY(kvmOf(own.wall, twoId).keptBesideItsTwin);
        QCOMPARE(kvmOf(own.wall, twoId).number, digestOf(first.deviceId.toString()));
    }

    void aJetKvmThatSaysItIsAnotherOfTheWallWhenSignedInToAnewIsNotSaved()
    {
        // A JetKVM's number is what it says it is: signed in to anew, one that says the number another KVM of the
        // wall gave is that KVM, and is not saved beside it ("same_device", as when it is added). Its own number,
        // where it gives one, is kept with it at its save.
        OwnWall own;
        JetKvmByHand first;
        JetKvmByHand second;
        second.deviceId = first.deviceId;
        OwnKvm one = at(first.address().toString(), u"Rack JetKVM"_s);
        one.kind = u"jetkvm"_s;
        one.number = digestOf(first.deviceId.toString());
        QVERIFY(!put(own, one).isEmpty());
        OwnKvm two = at(second.address().toString(), u"Another"_s);
        two.kind = u"jetkvm"_s;
        const QString twoId = put(own, two);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, twoId);
        typed.password = u"secret"_s;
        const KvmEditing::Outcome refused = cameOf(editing, twoId, typed);
        QCOMPARE(refusalOf(refused), std::string("same_device"));
        QCOMPARE(sentenceOf(refused),
                 u"This says it is the same device as \"Rack JetKVM\", which is on the wall already (127.0.0.1:%1)"_s
                     .arg(first.stand->serverPort())
                     .toStdString());
        QVERIFY(kvmOf(own.wall, twoId).number.isEmpty());
        JetKvmByHand third;
        OwnKvm three = at(third.address().toString(), u"Third"_s);
        three.kind = u"jetkvm"_s;
        const QString threeId = put(own, three);
        typed = asItIs(own.wall, threeId);
        typed.password = u"secret"_s;
        const auto saved = changedBy(editing, threeId, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, threeId).number, digestOf(third.deviceId.toString()));
    }

    void aChangeBoxOpenOverAJetKvmFollowedOntoHttpsMeanwhileIsToldItChanged()
    {
        // As the web's new record in the old one's place: a box opened before the wall moved it says so at
        // its save ("conflict"), and puts back nothing.
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        std::optional<QUrl> offered;
        core::DriverPaces paces = core::DriverPaces::alike(quickDriver());
        paces.jetkvm.wayEveryMs = 300;
        paces.jetkvm.otherWay = [&offered, from = plain.address()](const QUrl& address) {
            return address == from ? offered : std::nullopt;
        };
        OwnWall own(paces);
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        KvmEditing editing(own.wall, roomForTls());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.seen = KvmEditing::Seen{kvmOf(own.wall, id), own.wall.secretWrites(id)};
        typed.name = u"Renamed"_s;
        offered = secure.address();
        QVERIFY(QTest::qWaitFor(
            [&] {
                own.wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
                QTest::qWait(100);
                return kvmOf(own.wall, id).address == secure.address().toString();
            },
            15000));
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("conflict"));
        QCOMPARE(kvmOf(own.wall, id).address, secure.address().toString());
        QCOMPARE(kvmOf(own.wall, id).name, u"Small one"_s);
    }

    void aKvmdAtAJetKvmsOwnHttpsChosenThereIsOfAnotherFamilyAndHearsNoPassword()
    {
        // A JetKVM at http://h, and a kvmd over TLS where its own HTTPS is (the seam): its password typed with
        // "https://" there. Both ways are asked, and the way chosen is that HTTPS, where a kvmd answered: that
        // is another KVM ("other_family", as at any new address), and hears no password.
        JetKvmByHand plain;
        const MadeUpKvm kvmd(tls("first"));
        const auto seam = twoWays(plain.address(), QUrl(kvmd.stand->address()));
        core::DriverPaces paces = core::DriverPaces::alike(quickDriver());
        paces.jetkvm.otherWay = seam;
        OwnWall own(paces);
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = seam;
        KvmEditing editing(own.wall, pace);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = kvmd.stand->address();
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("other_family"));
        QCOMPARE(kvmd.signIns, 0);
        for (const Asked& one : std::as_const(kvmd.stand->asked)) {
            QVERIFY2(!one.body.contains("secret"), one.path.constData());
        }
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
    }

    void aPlainJetKvmIsNotAskedOverHttpsAtAnotherHost()
    {
        // Its other way, where that is HTTPS at another host than its own (here "localhost" beside 127.0.0.1, one
        // machine under two names): not its own HTTPS, asked nothing; the password typed goes where it is.
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        const QUrl atAnotherHost(u"https://localhost:%1"_s.arg(secure.stand->serverPort()));
        OwnWall own;
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = twoWays(plain.address(), atAnotherHost);
        KvmEditing editing(own.wall, pace);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(!saved.value().ontoHttps);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
        QCOMPARE(plain.signIns, 1);
        QVERIFY(secure.stand->asked.isEmpty());
        QCOMPARE(secure.signIns, 0);
    }

    void theChangeBoxSaysItsOwnWordsWhereAJetKvmAnswersOnlyThePlainWay()
    {
        // Typed "https://" where only the plain way answers: what to type is said as the change box has it, to
        // keep the KVM over plain HTTP or to move it there so; and nothing is signed in to.
        const core::testing::NobodyThere nobody;
        JetKvmByHand onlyPlain;
        const QString overHttps = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        KvmEditing::Pace pace = quick();
        pace.otherWay = twoWays(QUrl(overHttps), onlyPlain.address());
        OwnWall own;
        OwnKvm jet = at(onlyPlain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        KvmEditing editing(own.wall, pace);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = overHttps;
        typed.password = u"secret"_s;
        const KvmEditing::Outcome kept = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(kept), std::string("plain_only"));
        QCOMPARE(sentenceOf(kept), "Nothing answers over HTTPS at this address, but a JetKVM answers over plain HTTP. "
                                   "To keep it over plain HTTP, type " +
                                       onlyPlain.address().toString().toStdString());
        JetKvmByHand before;
        OwnKvm other = at(before.address().toString(), u"Another"_s);
        other.kind = u"jetkvm"_s;
        const QString otherId = put(own, other);
        typed = asItIs(own.wall, otherId);
        typed.address = overHttps;
        typed.password = u"secret"_s;
        const KvmEditing::Outcome moving = cameOf(editing, otherId, typed);
        QCOMPARE(refusalOf(moving), std::string("plain_only"));
        QCOMPARE(sentenceOf(moving), "Nothing answers over HTTPS at this address, but a JetKVM answers over plain "
                                     "HTTP. To move it there over plain HTTP, type " +
                                         onlyPlain.address().toString().toStdString());
        QCOMPARE(onlyPlain.signIns, 0);
        QCOMPARE(before.signIns, 0);
    }

    void aPasswordTypedForAPlainJetKvmGoesOverHttpsWhereThatIsOfferedByNow()
    {
        // As the web's: a JetKVM added the plain way, whose owner has turned HTTPS on since. What is typed
        // is about to be sent: where to is looked at again first, and it goes over HTTPS, known by the
        // certificate shown there on this first contact.
        OwnWall own;
        JetKvmByHand plain;
        OwnKvm jet = at(plain.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        QVERIFY(!id.isEmpty());
        JetKvmByHand secure(tls("first"));
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = twoWays(plain.address(), secure.address());
        KvmEditing editing(own.wall, pace);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QVERIFY(saved.value().ontoHttps);
        QVERIFY(!saved.value().overPlainHttp);
        QCOMPARE(saved.value().certificate, sha256Of("first"));
        const OwnKvm now = kvmOf(own.wall, id);
        QCOMPARE(now.address, secure.address().toString());
        QVERIFY(now.knownByCertificate);
        QCOMPARE(now.certificate, sha256Of("first"));
        QCOMPARE(now.kind, u"jetkvm"_s);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", secure.address().toString(), sha256Of("first")));
        // Its question and its password went over HTTPS; the plain way heard nothing.
        QVERIFY(plain.stand->asked.isEmpty());
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(secure.signIns, 1);
        QCOMPARE(secure.stand->count(kvmdsQuestion), 0);
        QVERIFY(secure.asked(jetKvmsQuestion) >= 1);
        // Where nothing offers HTTPS beside it, it is signed in to where it is, as ever.
        JetKvmByHand alone;
        OwnKvm other = at(alone.address().toString(), u"Another"_s);
        other.kind = u"jetkvm"_s;
        const QString otherId = put(own, other);
        KvmEditing::Typed again = asItIs(own.wall, otherId);
        again.password = u"secret"_s;
        const auto there = changedBy(editing, otherId, again);
        QVERIFY2(there.ok(), there.ok() ? "" : there.error().message.c_str());
        QVERIFY(!there.value().ontoHttps);
        QCOMPARE(kvmOf(own.wall, otherId).address, alone.address().toString());
        QCOMPARE(alone.signIns, 1);
    }

    void aJetKvmNotAskedYetIsAskedOverHttpsFirstAndThatIsItsAsking()
    {
        // Taken from a file, its password typed at the address it has: the JetKVM's question alone goes
        // over HTTPS first, and a JetKVM there is what answers at its address, asked. Where none answers
        // there, nothing is signed in to, as ever ("not_asked_yet").
        OwnWall own;
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        LocalWall::FromAFile one;
        one.kvm.address = plain.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = own.wall.takeFromAFile({one}).constFirst().value();
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = twoWays(plain.address(), secure.address());
        KvmEditing editing(own.wall, pace);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().ontoHttps);
        QCOMPARE(kvmOf(own.wall, id).address, secure.address().toString());
        QVERIFY(plain.stand->asked.isEmpty());
        QCOMPARE(secure.signIns, 1);
        // With no other way to it: refused, and nothing went to it at all.
        JetKvmByHand alone;
        one.kvm.address = alone.address().toString();
        const QString aloneId = own.wall.takeFromAFile({one}).constFirst().value();
        KvmEditing::Typed again = asItIs(own.wall, aloneId);
        again.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(editing, aloneId, again)), std::string("not_asked_yet"));
        QVERIFY(alone.stand->asked.isEmpty());
    }

    void aJetKvmMovedWithATypedPasswordIsAskedBothWays()
    {
        OwnWall own;
        JetKvmByHand before;
        OwnKvm jet = at(before.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = put(own, jet);
        // Typed with a port and nothing before it, where a JetKVM answers the plain way: that way.
        JetKvmByHand plain;
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = u"127.0.0.1:%1"_s.arg(plain.stand->serverPort());
        typed.password = u"secret"_s;
        auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().overPlainHttp);
        QCOMPARE(kvmOf(own.wall, id).address, plain.address().toString());
        QCOMPARE(plain.signIns, 1);
        // Typed "http://" where a JetKVM answers over HTTPS beside it: there, and only once the certificate
        // shown there was said yes to, as at any new address over HTTPS (step 4).
        JetKvmByHand plainToo;
        JetKvmByHand secure(tls("first"));
        KvmEditing::Pace pace = roomForTls();
        pace.otherWay = twoWays(plainToo.address(), secure.address());
        KvmEditing seamed(own.wall, pace);
        typed = asItIs(own.wall, id);
        typed.address = plainToo.address().toString();
        typed.password = u"secret"_s;
        const KvmEditing::Outcome differs = cameOf(seamed, id, typed);
        QCOMPARE(refusalOf(differs), std::string("certificate_differs"));
        QCOMPARE(differs.presented, sha256Of("first"));
        QCOMPARE(differs.presentedAt, u"127.0.0.1:%1"_s.arg(secure.stand->serverPort()));
        QCOMPARE(secure.signIns, 0);
        QCOMPARE(plainToo.signIns, 0);
        typed.saidYesTo = sha256Of("first");
        saved = changedBy(seamed, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).address, secure.address().toString());
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("first"));
        QCOMPARE(secure.signIns, 1);
        QCOMPARE(plainToo.signIns, 0);
        // Typed "https://" where only the plain way answers: said so, and nothing is signed in to.
        const core::testing::NobodyThere nobody;
        JetKvmByHand onlyPlain;
        const QString overHttps = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        KvmEditing::Pace plainly = quick();
        plainly.otherWay = twoWays(QUrl(overHttps), onlyPlain.address());
        KvmEditing third(own.wall, plainly);
        typed = asItIs(own.wall, id);
        typed.address = overHttps;
        typed.password = u"secret"_s;
        QCOMPARE(refusalOf(cameOf(third, id, typed)), std::string("plain_only"));
        QCOMPARE(onlyPlain.signIns, 0);
        QCOMPARE(onlyPlain.stand->asked.size(), 1);
        QCOMPARE(kvmOf(own.wall, id).address, secure.address().toString());
        // Typed with a port and nothing before it where only the plain way answers: that way is chosen, and,
        // spoken to over HTTPS where it is, it is held to what is given up there as a typed plain address
        // is: nothing is signed in to before the yes to that address.
        JetKvmByHand plainAgain;
        typed = asItIs(own.wall, id);
        typed.address = u"127.0.0.1:%1"_s.arg(plainAgain.stand->serverPort());
        typed.password = u"secret"_s;
        const KvmEditing::Outcome givingUp = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(givingUp), std::string("plain_http_there"));
        QCOMPARE(givingUp.presentedAt, u"127.0.0.1:%1"_s.arg(plainAgain.stand->serverPort()));
        QCOMPARE(plainAgain.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id).address, secure.address().toString());
    }

    void aJetKvmWithNoPasswordIsMovedWithoutOne()
    {
        // Out of its box a JetKVM runs without one: moved to a new address, what answers there is asked
        // what it is first, and signed in to as one that needs none, with nothing to type.
        OwnWall own;
        JetKvmByHand before;
        before.password.clear();
        OwnKvm jet = at(before.address().toString(), u"Small one"_s);
        jet.kind = u"jetkvm"_s;
        const QString id = own.wall.add(jet, QString()).value();
        KvmEditing editing(own.wall, quick());
        JetKvmByHand after;
        after.password.clear();
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = after.address().toString();
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QVERIFY(saved.value().signedIn);
        QCOMPARE(kvmOf(own.wall, id).address, after.address().toString());
        QCOMPARE(after.signIns, 0);
        QCOMPARE(after.lineCookies, QList<QByteArray>{"(none)"_ba});
        QVERIFY(!own.wall.marksAPasswordFor(id));
        // Where the JetKVM there asks for one: said so, and it stays where it is.
        JetKvmByHand guarded;
        typed.address = guarded.address().toString();
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("no_password"));
        // (Known by the number the one at its address gave, which let anybody in: the web's words for one that asks for
        // a password now, which may be another, J5.)
        QVERIFY(refused.error().message.starts_with("The JetKVM at this address asks for a password now, and this card "
                                                    "has none saved: it may be another JetKVM"));
        QCOMPARE(guarded.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id).address, after.address().toString());
        // (A JetKVM with a password kept, and a kvmd with none, are not moved without one typed: as ever.)
        JetKvmByHand guardedBefore;
        OwnKvm withOne = at(guardedBefore.address().toString(), u"Guarded"_s);
        withOne.kind = u"jetkvm"_s;
        const QString withOneId = put(own, withOne);
        JetKvmByHand openElsewhere;
        openElsewhere.password.clear();
        KvmEditing::Typed movingIt = asItIs(own.wall, withOneId);
        movingIt.address = openElsewhere.address().toString();
        QCOMPARE(refusalOf(cameOf(editing, withOneId, movingIt)), std::string("password_needed"));
        QVERIFY(openElsewhere.stand->asked.isEmpty());
        const MadeUpKvm kvmd;
        const QString kvmdId = put(own, at(kvmd.stand->address(), u"Build server"_s));
        const MadeUpKvm elsewhere;
        KvmEditing::Typed moving = asItIs(own.wall, kvmdId);
        moving.address = elsewhere.stand->address();
        QCOMPARE(refusalOf(cameOf(editing, kvmdId, moving)), std::string("password_needed"));
        QVERIFY(elsewhere.stand->asked.isEmpty());
    }

    void aFieldThatIsRefusedEndsItBeforeAnythingIsAsked_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("sentence");
        const QString noLine = QLatin1StringView(notOneLine);
        const auto tooLong = [](qsizetype longest) { return QString::fromLatin1(tooLongBy).arg(longest); };
        QTest::newRow("a name too long") << u"name"_s << QString(core::kvm::longestName + 1, u'n')
                                         << u"name: "_s + tooLong(core::kvm::longestName);
        QTest::newRow("a name of two lines") << u"name"_s << u"Build\nserver"_s << u"name: "_s + noLine;
        QTest::newRow("a group too long") << u"group"_s << QString(core::kvm::longestGroup + 1, u'g')
                                          << u"group: "_s + tooLong(core::kvm::longestGroup);
        QTest::newRow("a group turned round")
            << u"group"_s << u"Rack"_s + sign(0x202E) + u"2"_s << u"group: "_s + noLine;
        QTest::newRow("a user too long") << u"user"_s << QString(core::kvm::longestUser + 1, u'u')
                                         << u"username: "_s + tooLong(core::kvm::longestUser);
        QTest::newRow("a user of two lines") << u"user"_s << u"ad\rmin"_s << u"username: "_s + noLine;
        QTest::newRow("a password too long") << u"password"_s << QString(core::kvm::longestPassword + 1, u'p')
                                             << u"password: "_s + tooLong(core::kvm::longestPassword);
        // An address is not kept where none is typed: there is none to keep it for.
        QTest::newRow("no address") << u"address"_s << u""_s << u"Not a usable device address: ''"_s;
        QTest::newRow("what is no address")
            << u"address"_s << u"kvm lan"_s << u"Not a usable device address: 'kvm lan'"_s;
    }

    void aFieldThatIsRefusedEndsItBeforeAnythingIsAsked()
    {
        QFETCH(QString, field);
        QFETCH(QString, typed);
        QFETCH(QString, sentence);
        OwnWall own;
        const MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed all = asItIs(own.wall, id);
        if (field == u"name"_s) {
            all.name = typed;
        } else if (field == u"group"_s) {
            all.group = typed;
        } else if (field == u"user"_s) {
            all.user = typed;
        } else if (field == u"password"_s) {
            all.password = typed;
        } else {
            all.address = typed;
        }
        const auto refused = changedBy(editing, id, all);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("invalid"));
        QCOMPARE(QString::fromStdString(refused.error().message), sentence);
        // The rule is said, and never what was typed beside the address.
        QVERIFY(field == u"address"_s || !QString::fromStdString(refused.error().message).contains(typed));
        QTest::qWait(150);
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvm.stand->lines, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
    }

    void aKvmThatIsNotOnTheWallIsNotFound()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        KvmEditing editing(own.wall, quick());
        const KvmEditing::Typed typed = asItIs(own.wall, id);
        auto refused = changedBy(editing, u"00000000"_s, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("not_found"));
        QCOMPARE(refused.error().message, std::string("Unknown device"));
        // Nor one that was taken off.
        QVERIFY(own.wall.remove(id).ok());
        refused = changedBy(editing, id, typed);
        QCOMPARE(refused.error().code, std::string("not_found"));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(kvm.stand->asked.isEmpty());
    }

    void aSecondChangeWhileOneIsUnderWayIsRefused()
    {
        OwnWall own;
        MadeUpKvm kvm;
        Answered late = good({{u"hostname"_s, u"glkvm"_s}});
        late.afterMs = 600;
        kvm.first[namesQuestion].append(late);
        const MadeUpKvm beside;
        const QString id = put(own, at(kvm.stand->address(), u"One"_s));
        const QString other = put(own, at(beside.stand->address(), u"Beside"_s));
        KvmEditing editing(own.wall, quick());
        QList<base::Result<KvmEditing::Changed>> toldTheFirst;
        QList<base::Result<KvmEditing::Changed>> toldTheSecond;
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        typed.name = u"One, signed in to anew"_s;
        editing.change(id, typed,
                       [&toldTheFirst](const KvmEditing::Outcome& outcome) { toldTheFirst.append(outcome.result); });
        QVERIFY(editing.isUnderWay());
        KvmEditing::Typed renamed = asItIs(own.wall, other);
        renamed.name = u"Renamed"_s;
        editing.change(other, renamed,
                       [&toldTheSecond](const KvmEditing::Outcome& outcome) { toldTheSecond.append(outcome.result); });
        // Not there and then, either.
        QVERIFY(toldTheSecond.isEmpty());
        QTRY_COMPARE(toldTheSecond.size(), 1);
        QVERIFY(!toldTheSecond.constFirst().ok());
        QCOMPARE(toldTheSecond.constFirst().error().code, std::string("busy"));
        QCOMPARE(toldTheSecond.constFirst().error().message,
                 std::string("Another KVM is being changed at this moment. Wait for it to finish."));
        // The first goes on as if nothing had been asked.
        QVERIFY(toldTheFirst.isEmpty());
        QVERIFY(editing.isUnderWay());
        QTRY_COMPARE_WITH_TIMEOUT(toldTheFirst.size(), 1, 20000);
        QVERIFY(toldTheFirst.constFirst().ok());
        QCOMPARE(kvmOf(own.wall, id).name, u"One, signed in to anew"_s);
        QVERIFY(!editing.isUnderWay());
        // Of the second nothing is anywhere.
        QCOMPARE(kvmOf(own.wall, other).name, u"Beside"_s);
        QCOMPARE(toldTheSecond.size(), 1);
        // And after it the next.
        QVERIFY(changedBy(editing, other, renamed).ok());
        QCOMPARE(kvmOf(own.wall, other).name, u"Renamed"_s);
    }

    void theResultComesLaterAndOnce()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        KvmEditing editing(own.wall, quick());
        QList<base::Result<KvmEditing::Changed>> told;
        const auto note = [&told](const KvmEditing::Outcome& outcome) { told.append(outcome.result); };
        // Also where there is nothing to wait for: a KVM that is not there.
        editing.change(u"00000000"_s, asItIs(own.wall, id), note);
        QVERIFY(told.isEmpty());
        QVERIFY(editing.isUnderWay());
        QTRY_COMPARE(told.size(), 1);
        QVERIFY(!editing.isUnderWay());
        // A name alone is on the wall when change() comes back, and its result comes later.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"Renamed"_s;
        editing.change(id, typed, note);
        QCOMPARE(kvmOf(own.wall, id).name, u"Renamed"_s);
        QCOMPARE(told.size(), 1);
        QVERIFY(editing.isUnderWay());
        QTRY_COMPARE(told.size(), 2);
        QVERIFY(told.at(1).ok());
        // And one that is signed in to first.
        typed.password = u"secret"_s;
        editing.change(id, typed, note);
        QCOMPARE(told.size(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 3, 20000);
        QVERIFY(told.at(2).ok());
        QTest::qWait(400);
        QCOMPARE(told.size(), 3);
    }

    void whoeverIsToldMayBeginTheNextThereAndThen()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        KvmEditing editing(own.wall, quick());
        QList<base::Result<KvmEditing::Changed>> told;
        KvmEditing::Typed first = asItIs(own.wall, id);
        first.password = u"secret"_s;
        first.name = u"One"_s;
        KvmEditing::Typed next = first;
        next.name = u"Next"_s;
        editing.change(id, first, [&](const KvmEditing::Outcome& outcome) {
            told.append(outcome.result);
            editing.change(id, next, [&told](const KvmEditing::Outcome& after) { told.append(after.result); });
        });
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 2, 20000);
        QVERIFY(told.at(0).ok());
        QVERIFY2(told.at(1).ok(), told.at(1).ok() ? "" : told.at(1).error().message.c_str());
        QCOMPARE(told.at(1).value().name, u"Next"_s);
        QCOMPARE(kvmOf(own.wall, id).name, u"Next"_s);
    }

    void aChangeThatIsGoneChangesNothingAndTellsNobody()
    {
        OwnWall own;
        int told = 0;
        const auto count = [&told](const KvmEditing::Outcome&) { ++told; };
        // Gone while the KVM is being signed in to.
        MadeUpKvm signingIn;
        Answered lateIn = good({{u"token"_s, u"pass1"_s}});
        lateIn.afterMs = 800;
        signingIn.first[signInQuestion].append(lateIn);
        const QString id = put(own, at(signingIn.stand->address(), u"Build server"_s));
        const OwnKvm before = kvmOf(own.wall, id);
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"Another name"_s;
        typed.password = u"another one"_s;
        {
            KvmEditing editing(own.wall, quick());
            editing.change(id, typed, count);
            QTRY_COMPARE(signingIn.asked(signInQuestion), 1);
        }
        const qsizetype askedOfIt = signingIn.stand->asked.size();
        // Gone while a new address is being asked what it is.
        MadeUpKvm probing;
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 800;
        probing.instead.insert(kvmdsQuestion, late);
        typed.address = probing.stand->address();
        {
            KvmEditing editing(own.wall, quick());
            editing.change(id, typed, count);
            QTRY_COMPARE(probing.stand->asked.size(), 3);
        }
        // And gone with a refusal on its way.
        {
            KvmEditing editing(own.wall, quick());
            editing.change(u"00000000"_s, typed, count);
            editing.change(id, typed, count);
        }
        // (Longer than either of the two was to take over its answer.)
        QTest::qWait(1300);
        QCOMPARE(told, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QVERIFY(changed.isEmpty());
        // And none of the KVMs heard anything after that.
        QCOMPARE(probing.asked(signInQuestion), 0);
        QCOMPARE(signingIn.stand->asked.size(), askedOfIt);
    }

    void whoeverHearsTheWallSayThatItChangedMayEndTheChange()
    {
        // The wall says so while it saves, in the middle of the change's last step: a box that
        // closes on that word takes the change with it.
        OwnWall own;
        const MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        auto editing = std::make_unique<KvmEditing>(own.wall, quick());
        int told = 0;
        connect(&own.wall, &LocalWall::changed, this, [&editing] { editing.reset(); });
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        typed.name = u"Signed in to anew"_s;
        editing->change(id, typed, [&told](const KvmEditing::Outcome&) { ++told; });
        QTRY_VERIFY_WITH_TIMEOUT(editing == nullptr, 20000);
        QTest::qWait(300);
        // Nobody is told by what is gone. The change is on the wall, as the wall itself said.
        QCOMPARE(told, 0);
        QCOMPARE(kvmOf(own.wall, id).name, u"Signed in to anew"_s);

        // Also where nothing is asked of the KVM, and the wall says so before change() is back.
        editing = std::make_unique<KvmEditing>(own.wall, quick());
        typed.password.clear();
        typed.name = u"Renamed"_s;
        editing->change(id, typed, [&told](const KvmEditing::Outcome&) { ++told; });
        QVERIFY(editing == nullptr);
        QTest::qWait(300);
        QCOMPARE(told, 0);
        QCOMPARE(kvmOf(own.wall, id).name, u"Renamed"_s);
    }

    void aKvmThatWasChangedMeanwhileIsNotSavedOver()
    {
        // Two boxes open on one KVM, in two windows. The first signs in, which takes a while; the
        // second saves meanwhile. What the first read before that is not what is there now.
        OwnWall own;
        MadeUpKvm kvm;
        Answered late = good({{u"hostname"_s, u"glkvm"_s}});
        late.afterMs = 600;
        kvm.first[namesQuestion].append(late);
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        KvmEditing first(own.wall, quick());
        KvmEditing second(own.wall, quick());
        QList<base::Result<KvmEditing::Changed>> told;
        const auto note = [&told](const KvmEditing::Outcome& outcome) { told.append(outcome.result); };
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.password = u"secret"_s;
        typed.name = u"From the first box"_s;
        first.change(id, typed, note);
        QTRY_COMPARE(kvm.asked(signInQuestion), 1);
        KvmEditing::Typed other = asItIs(own.wall, id);
        other.group = u"From the second box"_s;
        QVERIFY(changedBy(second, id, other).ok());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QVERIFY(!told.constFirst().ok());
        QCOMPARE(told.constFirst().error().code, std::string("conflict"));
        QCOMPARE(told.constFirst().error().message,
                 std::string("This device was changed meanwhile. Look at it again."));
        QCOMPARE(kvmOf(own.wall, id).name, u"Build server"_s);
        QCOMPARE(kvmOf(own.wall, id).group, u"From the second box"_s);

        // And one that was taken off the wall meanwhile is not put back.
        kvm.first[namesQuestion].append(late);
        first.change(id, typed, note);
        QTRY_COMPARE(kvm.asked(signInQuestion), 2);
        QVERIFY(own.wall.remove(id).ok());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 2, 20000);
        QCOMPARE(told.constLast().error().code, std::string("not_found"));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
    }

    void whatCannotBeWrittenDownChangesNothing()
    {
        OwnWall own;
        MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        const OwnKvm before = kvmOf(own.wall, id);
        kvm.password = u"another one"_s;
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"Renamed"_s;
        typed.password = u"another one"_s;
        const auto notSaved = [](const base::Result<KvmEditing::Changed>& refused) {
            const QString sentence = refused.ok() ? QString() : QString::fromStdString(refused.error().message);
            return !refused.ok() && refused.error().code == "not_saved" &&
                   sentence.startsWith(u"The change to this device could not be saved ("_s) &&
                   sentence.endsWith(u"), so nothing was changed."_s);
        };
        // Its password cannot be kept: then its name is not saved either.
        own.secrets.refuses = true;
        QVERIFY(notSaved(changedBy(editing, id, typed)));
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        own.secrets.refuses = false;
        // The settings cannot be written: then the password that was kept is the kept one again.
        QVERIFY(own.settingsNoLongerWritable());
        QVERIFY(notSaved(changedBy(editing, id, typed)));
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.size(), 1);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        // Nor a name alone.
        typed.password.clear();
        QVERIFY(notSaved(changedBy(editing, id, typed)));
        QCOMPARE(kvmOf(own.wall, id), before);
        QVERIFY(changed.isEmpty());
    }

    void whatCouldNotBeWrittenDownIsSavedAtTheNextTryFromTheSameBox()
    {
        // A box holds the KVM as it was when the box was opened. Its saving is signed in for,
        // and then the store does not take the password: nothing was changed, and nobody else
        // changed anything either. The box saves again what it still holds.
        OwnWall own;
        MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        const OwnKvm before = kvmOf(own.wall, id);
        kvm.password = u"another one"_s;
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.seen = KvmEditing::Seen{before, own.wall.secretWrites(id)};
        typed.name = u"Renamed"_s;
        typed.password = u"another one"_s;
        own.secrets.refuses = true;
        const auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("not_saved"));
        QCOMPARE(kvmOf(own.wall, id), before);
        own.secrets.refuses = false;
        const auto saved = changedBy(editing, id, typed);
        QVERIFY2(saved.ok(), saved.ok() ? "" : saved.error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).name, u"Renamed"_s);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "another one"));
    }

    void nothingOfAPasswordIsInAFileASentenceOrALog()
    {
        QStringList aloud;
        saidAloud = &aloud;
        handlerBefore = qInstallMessageHandler(listening);
        const auto quiet = qScopeGuard([] {
            qInstallMessageHandler(handlerBefore);
            saidAloud = nullptr;
        });

        OwnWall own;
        MadeUpKvm kvm;
        kvm.password = u"Kept-Zq7-one"_s;
        const MadeUpKvm beside;
        const MadeUpKvm elsewhere(tls("first"));
        const StandIn page(asAPage);
        const core::testing::NobodyThere nobody;
        const QString id = own.wall.add(at(kvm.stand->address(), u"Build server"_s), kvm.password).value();
        QVERIFY(!put(own, at(beside.stand->address(), u"Beside"_s)).isEmpty());
        KvmEditing editing(own.wall, quick());
        QStringList sentences;
        const auto tried = [&](const QString& address, const QString& password) {
            KvmEditing::Typed typed = asItIs(own.wall, id);
            typed.address = address;
            typed.password = password;
            const auto result = changedBy(editing, id, typed);
            sentences.append(result.ok() ? result.value().name : QString::fromStdString(result.error().message));
            return result.ok() ? std::string("saved") : result.error().code;
        };
        // A password that is refused and one that is taken; and one of each kind of thing that
        // is never signed in to.
        const QString here = kvm.stand->address();
        QCOMPARE(tried(here, u"Wrong-Zq7-two"_s), std::string("auth_failed"));
        kvm.password = u"Kept-Zq7-three"_s;
        QCOMPARE(tried(here, kvm.password), std::string("saved"));
        QCOMPARE(tried(page.address(), u"Never-Zq7-four"_s), std::string("not_a_kvm"));
        QCOMPARE(tried(nobody.address(), u"Never-Zq7-five"_s), std::string("unreachable"));
        QCOMPARE(tried(beside.stand->address(), u"Never-Zq7-six"_s), std::string("duplicate"));
        QCOMPARE(tried(u"kvm lan"_s, u"Never-Zq7-seven"_s), std::string("invalid"));
        QCOMPARE(tried(here, QString(core::kvm::longestPassword, u'p') + u"-Zq7-"_s), std::string("invalid"));
        // And the kept one, which goes to no address it was not saved for.
        QCOMPARE(tried(elsewhere.stand->address(), QString()), std::string("password_needed"));
        QTest::qWait(200);

        // With the secrets under the KVM's id, the one that was taken, and nowhere else.
        QCOMPARE(own.secrets.all.size(), 2);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedFor("Kept-Zq7-three", here));
        QFile settings(own.file);
        QVERIFY(settings.open(QIODevice::ReadOnly));
        const QByteArray text = settings.readAll();
        QVERIFY(text.contains("Build server"));
        QVERIFY(!text.contains("Zq7"));
        QVERIFY(!text.contains("password"));
        QCOMPARE(sentences.size(), 8);
        for (const QString& one : std::as_const(sentences)) {
            QVERIFY2(!one.contains(u"Zq7"_s), qPrintable(one));
        }
        for (const QString& one : std::as_const(aloud)) {
            QVERIFY2(!one.contains(u"Zq7"_s), qPrintable(one));
        }
        // And to what was never signed in to, nothing of it went.
        for (const StandIn* stand :
             std::initializer_list<const StandIn*>{&page, elsewhere.stand.get(), beside.stand.get()}) {
            for (const Asked& one : std::as_const(stand->asked)) {
                QVERIFY(!one.body.contains("Zq7") && !one.path.contains("Zq7"));
                QVERIFY(one.headers.value("cookie").isEmpty() && one.headers.value("authorization").isEmpty());
            }
        }
    }

    void whatShowsAnotherCertificateAtANewAddressIsNotSignedInToBeforeSomebodySaidYes()
    {
        // The KVM is known by a certificate, and what answers at the address that is typed for it
        // shows another one. Its password, typed with that address, does not go there: nobody
        // has seen the two.
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm other(tls("second"));
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = other.stand->address();
        typed.password = u"secret"_s;
        const KvmEditing::Outcome refused = cameOf(editing, id, typed);
        QVERIFY(!refused.result.ok());
        QCOMPARE(refused.result.error().code, std::string("certificate_differs"));
        // Asked what it is, with nothing that says who asks, and nothing after that.
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*other.stand));
        QCOMPARE(other.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QVERIFY(changed.isEmpty());
        // Whoever asked is handed the two, and where the second was shown: to be put side by
        // side. Neither is in the sentence.
        QCOMPARE(refused.remembered, sha256Of("first"));
        QCOMPARE(refused.presented, sha256Of("second"));
        QCOMPARE(refused.presentedAt, u"127.0.0.1:%1"_s.arg(other.stand->serverPort()));
        const QString sentence = QString::fromStdString(refused.result.error().message);
        QCOMPARE(sentence,
                 u"What answers at the new address shows another certificate than the one saved for this device"_s);
        for (const QByteArray& one : {refused.remembered, refused.presented}) {
            QVERIFY(!sentence.contains(QString::fromLatin1(one.toHex()), Qt::CaseInsensitive));
            QVERIFY(!sentence.contains(core::kvm::fingerprint(one)));
        }
        // Nor does a yes to some other certificate take the password there: not to the one it
        // is known by (which is not shown there), not to what is no certificate at all.
        for (const QByteArray& notThatOne :
             {sha256Of("first"), sha256Of("comet"), QByteArray(32, 'x'), QByteArray(31, 'x')}) {
            typed.saidYesTo = notThatOne;
            const KvmEditing::Outcome again = cameOf(editing, id, typed);
            QCOMPARE(again.result.error().code, std::string("certificate_differs"));
            QCOMPARE(again.presented, sha256Of("second"));
        }
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*other.stand, 5));
        QCOMPARE(other.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        // Other changes that are refused hand over no certificates.
        typed.address = u"not an address at all"_s;
        const KvmEditing::Outcome invalid = cameOf(editing, id, typed);
        QCOMPARE(invalid.result.error().code, std::string("invalid"));
        QVERIFY(invalid.remembered.isEmpty() && invalid.presented.isEmpty() && invalid.presentedAt.isEmpty());
    }

    void withTheYesToItACertificateShownAtANewAddressIsWhatTheKvmIsSignedInToAndKnownBy()
    {
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm other(tls("second"));
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = other.stand->address();
        typed.password = u"secret"_s;
        // Shown the two, as the change hands them over, somebody says yes to the second.
        const KvmEditing::Outcome shown = cameOf(editing, id, typed);
        QCOMPARE(shown.result.error().code, std::string("certificate_differs"));
        typed.saidYesTo = shown.presented;
        const KvmEditing::Outcome saved = cameOf(editing, id, typed);
        QVERIFY2(saved.result.ok(), saved.result.ok() ? "" : saved.result.error().message.c_str());
        QVERIFY(saved.result.value().signedIn);
        QVERIFY(!saved.result.value().overPlainHttp);
        QCOMPARE(saved.result.value().certificate, sha256Of("second"));
        QVERIFY(saved.remembered.isEmpty() && saved.presented.isEmpty());
        QCOMPARE(changed.count(), 1);
        // There now, known by the one that was said yes to; and its password is saved for that.
        const OwnKvm kept = kvmOf(own.wall, id);
        QCOMPARE(kept.address, other.stand->address());
        QVERIFY(kept.knownByCertificate);
        QCOMPARE(kept.certificate, sha256Of("second"));
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", other.stand->address(), sha256Of("second")));
        // It was read once more for the yes, again with nothing that says who asks (and whether it signs this program
        // in, with nothing to sign in with: a password goes only on a line shown to be up); and only then signed in to,
        // once.
        QVERIFY(other.stand->asked.size() > 7);
        for (qsizetype index = 0; index < 7; ++index) {
            const Asked& question = other.stand->asked.at(index);
            QCOMPARE(question.method, "GET"_ba);
            QVERIFY(!question.headers.contains("cookie"));
            QVERIFY(question.body.isEmpty());
        }
        QCOMPARE(other.stand->asked.at(7).path, QByteArray(signInQuestion));
        QCOMPARE(other.stand->asked.at(7).body, "user=admin&passwd=secret"_ba);
        QCOMPARE(other.signIns, 1);
        QVERIFY(here.stand->asked.isEmpty());
        // And the wall's own driver speaks to it there, by that certificate.
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QCOMPARE(other.signIns, 2);
    }

    void aYesToOneCertificateIsNoYesToWhatIsShownInItsPlaceByThen()
    {
        // Somebody was shown the second certificate and said yes to it. By the time the yes
        // arrives, something else again answers at that address.
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        auto second = std::make_unique<MadeUpKvm>(tls("second"));
        const quint16 port = second->stand->serverPort();
        const QString there = second->stand->address();
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = there;
        typed.password = u"secret"_s;
        const KvmEditing::Outcome shown = cameOf(editing, id, typed);
        QCOMPARE(shown.presented, sha256Of("second"));
        second.reset();
        const MadeUpKvm third(tls("comet"), port);
        QVERIFY(third.stand->isListening());
        typed.saidYesTo = shown.presented;
        const KvmEditing::Outcome refused = cameOf(editing, id, typed);
        // Refused again, with what it shows now: nobody has seen that one.
        QVERIFY(!refused.result.ok());
        QCOMPARE(refused.result.error().code, std::string("certificate_differs"));
        QCOMPARE(refused.remembered, sha256Of("first"));
        QCOMPARE(refused.presented, sha256Of("comet"));
        // And nothing but the three questions went to it.
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*third.stand));
        QCOMPARE(third.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QVERIFY(changed.isEmpty());
    }

    void whatTakesThePlaceOfWhatWasSaidYesToBeforeTheSignInGoesOutHearsNothing()
    {
        // The certificate that was said yes to is read once more, and is the one. Then, before
        // the sign-in goes out on a line of its own, something else stands at that address: the
        // line is known by the one that was said yes to, and by no other.
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        auto second = std::make_unique<MadeUpKvm>(tls("second"));
        const quint16 port = second->stand->serverPort();
        const QString there = second->stand->address();
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        // (It says what it is only after a while, on the lines it was asked on: by then it
        // takes no new ones, and what comes instead does. A while in which also a slow PC has
        // seen the three questions and changed what stands there; and a probe that waits for
        // it. With the short times of the other tests the probe gave up first where the
        // program is slow, as under the address sanitizer, and said "not_a_kvm".)
        KvmEditing::Pace patient = quick();
        patient.probe.connectWithinMs = 5000;
        patient.probe.quietMs = 8000;
        patient.probe.withinMs = 12000;
        KvmEditing editing(own.wall, patient);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = there;
        typed.password = u"secret"_s;
        typed.saidYesTo = sha256Of("second");
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 2500;
        second->instead.insert(kvmdsQuestion, late);
        QList<KvmEditing::Outcome> told;
        editing.change(id, typed, [&told](const KvmEditing::Outcome& outcome) { told.append(outcome); });
        QTRY_COMPARE(second->stand->asked.size(), 3);
        second->stand->takeNoMore();
        const MadeUpKvm third(tls("comet"), port);
        QVERIFY(third.stand->isListening());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QVERIFY(!told.constFirst().result.ok());
        QCOMPARE(told.constFirst().result.error().code, std::string("cert_mismatch"));
        // (That the third hears nothing is a rule that was there before a yes was asked for at
        // all: the driver's lines were held to what the probe had seen, and this passed without
        // the yes. What it is told as is of the yes: as a yes to a certificate says it, and
        // not as of a certificate "saved for this device", which that line was not held to.)
        QCOMPARE(told.constFirst().result.error().message,
                 std::string("The device now presents a different certificate than the one shown"));
        QTest::qWait(150);
        QVERIFY(third.stand->asked.isEmpty());
        QCOMPARE(second->signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
    }

    void whereWindowsVouchesAtTheNewAddressTheCertificateAKvmIsKnownByIsGivenUpOnlyOnAYes()
    {
        // Known by a certificate, and moved with its password typed to where Windows trusts
        // what is shown: somebody's front door, or a proxy that looks into TLS and has its root
        // in Windows' store. (This was the first half of
        // whereThereIsNoSecondCertificateToLookAtAPasswordThatIsTypedGoesToTheNewAddressAsBefore,
        // and had it signed in to at once, and known by none from then on: turned round.)
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm door(tls("signed"));
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = door.stand->address();
        typed.password = u"secret"_s;
        const KvmEditing::Outcome shown = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(shown), std::string("certificate_differs"));
        // Whoever asked is handed the certificate it is known by, and that Windows vouches for
        // what is shown there: neither is in the sentence.
        QCOMPARE(shown.remembered, sha256Of("first"));
        QCOMPARE(shown.presented, sha256Of("signed"));
        QVERIFY(shown.onWindowsWord);
        QCOMPARE(QString::fromStdString(shown.result.error().message),
                 u"At the new address this device would be known otherwise than it is known now"_s);
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*door.stand));
        QCOMPARE(door.signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        // With the yes to that: known by none from then on, and by Windows' word.
        typed.saidYesTo = shown.presented;
        typed.saidYesOnWindowsWord = true;
        const KvmEditing::Outcome saved = cameOf(editing, id, typed);
        QVERIFY2(saved.result.ok(), saved.result.ok() ? "" : saved.result.error().message.c_str());
        QVERIFY(saved.result.value().certificate.isEmpty());
        QVERIFY(!saved.result.value().overPlainHttp);
        QVERIFY(!kvmOf(own.wall, id).knownByCertificate);
        QCOMPARE(door.signIns, 1);
    }

    void whereTheNewAddressIsPlainHttpAPasswordThatIsTypedGoesThereOnlyOnAYesToThat()
    {
        // One that is known by a certificate, moved to another place that is spoken to in the
        // clear: there is none to show, so what is given up is put before somebody first.
        // (This was whereTheNewAddressIsPlainHttpAPasswordThatIsTypedGoesThereAsBefore, and
        // had the password sent there at once, unencrypted, with nobody asked: turned round.)
        OwnWall own;
        const MadeUpKvm encrypted(tls("second"));
        const MadeUpKvm plain;
        const QString other = put(own, at(encrypted.stand->address(), u"NAS"_s, "second"));
        const OwnKvm before = kvmOf(own.wall, other);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, other);
        typed.address = plain.stand->address();
        typed.password = u"secret"_s;
        const KvmEditing::Outcome refused = cameOf(editing, other, typed);
        QCOMPARE(refusalOf(refused), std::string("plain_http_there"));
        QCOMPARE(QString::fromStdString(refused.result.error().message),
                 u"At the new address this device would be spoken to over plain HTTP, where nothing shows who "
                 "answers and its password travels unencrypted"_s);
        QCOMPARE(refused.remembered, sha256Of("second"));
        QVERIFY(refused.presented.isEmpty());
        QCOMPARE(refused.presentedAt, u"127.0.0.1:%1"_s.arg(plain.stand->serverPort()));
        // Nothing at all has gone there, and nothing is changed.
        QTest::qWait(150);
        QVERIFY(plain.stand->asked.isEmpty());
        QCOMPARE(plain.stand->lines, 0);
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(kvmOf(own.wall, other), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(other)), savedAs(own.wall, other, "secret"));
        QVERIFY(changed.isEmpty());
        // With the yes to that address: asked what it is, signed in to once, and whoever shows
        // the save is told that it is spoken to in the clear.
        typed.saidYesToPlainHttpAt = refused.presentedAt;
        const KvmEditing::Outcome saved = cameOf(editing, other, typed);
        QVERIFY2(saved.result.ok(), saved.result.ok() ? "" : saved.result.error().message.c_str());
        QVERIFY(saved.result.value().overPlainHttp);
        QVERIFY(saved.result.value().certificate.isEmpty());
        QCOMPARE(kvmOf(own.wall, other).address, plain.stand->address());
        QVERIFY(!kvmOf(own.wall, other).knownByCertificate);
        QCOMPARE(plain.signIns, 1);
        QCOMPARE(plain.stand->asked.at(4).path, QByteArray(signInQuestion)); // (after the question asked first)
    }

    void aYesToPlainHttpIsOneForThatAddressAloneAndForAPasswordThatWasTyped()
    {
        OwnWall own;
        const MadeUpKvm encrypted(tls("first"));
        const MadeUpKvm plain;
        const MadeUpKvm elsewhere;
        const QString id = put(own, at(encrypted.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        const QString there = u"127.0.0.1:%1"_s.arg(plain.stand->serverPort());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = plain.stand->address();
        typed.password = u"secret"_s;
        // A yes that was said for another address, or for this one written another way, is none
        // for this one.
        for (const QString& notThere :
             {u"127.0.0.1:%1"_s.arg(elsewhere.stand->serverPort()), u"127.0.0.1"_s, plain.stand->address(),
              u"localhost:%1"_s.arg(plain.stand->serverPort()), there + u' '}) {
            typed.saidYesToPlainHttpAt = notThere;
            const KvmEditing::Outcome refused = cameOf(editing, id, typed);
            QCOMPARE(refusalOf(refused), std::string("plain_http_there"));
            QCOMPARE(refused.presentedAt, there);
        }
        // Another host is another place as another port is: the same there. (Nothing listens
        // for it; nothing is asked of it.)
        typed.address = u"http://localhost:%1"_s.arg(encrypted.stand->serverPort());
        typed.saidYesToPlainHttpAt = there;
        const KvmEditing::Outcome otherHost = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(otherHost), std::string("plain_http_there"));
        QCOMPARE(otherHost.presentedAt, u"localhost:%1"_s.arg(encrypted.stand->serverPort()));
        // The kept password does not go there on that yes: it is for one that was typed.
        typed.address = plain.stand->address();
        typed.password.clear();
        const KvmEditing::Outcome kept = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(kept), std::string("password_needed"));
        QCOMPARE(kept.result.error().message, std::string(nothingToKnowItBy));
        QVERIFY(kept.presentedAt.isEmpty());
        // And at the place the KVM is at there is no yes to say: refused outright, as ever.
        typed.address = u"http://127.0.0.1:%1"_s.arg(encrypted.stand->serverPort());
        typed.password = u"secret"_s;
        typed.saidYesToPlainHttpAt = u"127.0.0.1:%1"_s.arg(encrypted.stand->serverPort());
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("not_in_the_clear"));
        // Through all of it not one line was made to any of them, and nothing is changed.
        QTest::qWait(150);
        QCOMPARE(plain.stand->lines, 0);
        QCOMPARE(elsewhere.stand->lines, 0);
        QCOMPARE(encrypted.stand->lines, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QVERIFY(changed.isEmpty());
        // The yes for the address itself is taken, once: what answers there is no KVM, so the
        // password stays here; and the next saving is refused as the first was.
        const StandIn page(asAPage);
        typed.address = page.address();
        typed.saidYesToPlainHttpAt = u"127.0.0.1:%1"_s.arg(page.serverPort());
        const KvmEditing::Outcome noKvm = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(noKvm), std::string("not_a_kvm"));
        QVERIFY(heardOnlyTheQuestions(page));
        typed.saidYesToPlainHttpAt.clear();
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("plain_http_there"));
        QVERIFY(heardOnlyTheQuestions(page));
        QCOMPARE(kvmOf(own.wall, id), before);
    }

    void whatReachesANewAddressBeforeAYesAndAfterItByHowTheKvmIsKnownAndWhatIsShownThere_data()
    {
        // How the KVM is known where it is, and the certificate it shows there (none: it is
        // spoken to in the clear); what is shown at the address that is typed for it (none:
        // plain HTTP); whether a yes is asked for first, and whether that is a yes to
        // Windows' word; and the certificate it is known by once it is saved there (none: by
        // none). "signed" and "twin-one" are trusted here as Windows trusts a maker's.
        QTest::addColumn<QByteArray>("knownBy");
        QTest::addColumn<QByteArray>("shows");
        QTest::addColumn<QByteArray>("shownThere");
        QTest::addColumn<bool>("yesFirst");
        QTest::addColumn<bool>("onWindowsWord");
        QTest::addColumn<QByteArray>("knownByThere");
        const QByteArray none;
        QTest::newRow("by a certificate, to where it shows the same one")
            << "certificate"_ba << "first"_ba << "first"_ba << false << false << "first"_ba;
        QTest::newRow("by a certificate, to where another is shown")
            << "certificate"_ba << "first"_ba << "second"_ba << true << false << "second"_ba;
        QTest::newRow("by a certificate, to where Windows trusts what is shown")
            << "certificate"_ba << "first"_ba << "signed"_ba << true << true << none;
        // (To plain HTTP from HTTPS: a yes of its own first, to what is given up there. These
        // two rows had the password sent there at once: turned round.)
        QTest::newRow("by a certificate, to plain HTTP")
            << "certificate"_ba << "first"_ba << none << true << false << none;
        // (Somebody said yes to it once, in a look at its certificate: it stays known by it.)
        QTest::newRow("by a certificate that Windows trusts too, to where it shows the same one")
            << "certificate"_ba << "signed"_ba << "signed"_ba << false << false << "signed"_ba;
        QTest::newRow("on Windows' word, to where it shows the same certificate")
            << "windows"_ba << "signed"_ba << "signed"_ba << false << false << none;
        QTest::newRow("on Windows' word, to where Windows trusts another")
            << "windows"_ba << "signed"_ba << "twin-one"_ba << false << false << none;
        QTest::newRow("on Windows' word, to where a certificate is shown that Windows does not trust")
            << "windows"_ba << "signed"_ba << "second"_ba << true << false << "second"_ba;
        QTest::newRow("on Windows' word, to plain HTTP")
            << "windows"_ba << "signed"_ba << none << true << false << none;
        // (In the clear it shows no certificate: there is no "the same one" for it.)
        QTest::newRow("in the clear, to where a certificate is shown")
            << "none"_ba << none << "first"_ba << true << false << "first"_ba;
        QTest::newRow("in the clear, to where Windows trusts what is shown")
            << "none"_ba << none << "signed"_ba << true << true << none;
        QTest::newRow("in the clear, to plain HTTP") << "none"_ba << none << none << false << false << none;
    }

    void whatReachesANewAddressBeforeAYesAndAfterItByHowTheKvmIsKnownAndWhatIsShownThere()
    {
        QFETCH(QByteArray, knownBy);
        QFETCH(QByteArray, shows);
        QFETCH(QByteArray, shownThere);
        QFETCH(bool, yesFirst);
        QFETCH(bool, onWindowsWord);
        QFETCH(QByteArray, knownByThere);
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        const VouchedFor alsoVouched(QStringLiteral(QKVM_TESTDATA "/tls/twin-one.crt"));
        const auto made = [](const QByteArray& showing) {
            return showing.isEmpty() ? std::make_unique<MadeUpKvm>()
                                     : std::make_unique<MadeUpKvm>(tls(showing.constData()));
        };
        const auto sha256 = [](const QByteArray& name) {
            return name.isEmpty() ? QByteArray() : sha256Of(name.constData());
        };
        OwnWall own;
        const auto here = made(shows);
        const auto there = made(shownThere);
        const QString id = put(
            own, at(here->stand->address(), u"Build server"_s, knownBy == "certificate" ? shows.constData() : nullptr));
        const OwnKvm before = kvmOf(own.wall, id);
        // Certificate consent is tested here; use the real probe budget for TLS setup.
        // Driver timing and every trust/credential assertion stay unchanged.
        KvmEditing::Pace pace = quick();
        pace.probe = core::KvmProbe::Pace{};
        KvmEditing editing(own.wall, pace);
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = there->stand->address();
        typed.password = u"secret"_s;
        int looks = 1;
        if (yesFirst && shownThere.isEmpty()) {
            // Into the clear, before anybody said yes: refused with a word of its own, and
            // not one line was made to that address. Nothing is changed.
            const KvmEditing::Outcome asked = cameOf(editing, id, typed);
            QCOMPARE(refusalOf(asked), std::string("plain_http_there"));
            QTest::qWait(150);
            QVERIFY(there->stand->asked.isEmpty());
            QCOMPARE(there->stand->lines, 0);
            QCOMPARE(kvmOf(own.wall, id), before);
            QVERIFY(changed.isEmpty());
            // Whoever asked is handed what it is known by now, and that address: nothing was
            // presented there, and Windows vouches for nothing.
            QCOMPARE(asked.remembered, before.knownByCertificate ? before.certificate : QByteArray());
            QVERIFY(asked.presented.isEmpty());
            QVERIFY(!asked.onWindowsWord);
            QCOMPARE(asked.presentedAt, u"127.0.0.1:%1"_s.arg(there->stand->serverPort()));
            // A yes of the other kind is no yes to this: to a certificate, or to Windows' word.
            typed.saidYesTo = sha256(shows);
            typed.saidYesOnWindowsWord = knownBy == "windows";
            QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("plain_http_there"));
            QTest::qWait(150);
            QCOMPARE(there->stand->lines, 0);
            typed.saidYesTo.clear();
            typed.saidYesOnWindowsWord = false;
            typed.saidYesToPlainHttpAt = asked.presentedAt;
        } else if (yesFirst) {
            // Before anybody said yes: what answers there is asked what it is, with nothing that
            // says who asks, and nothing else. It is not signed in to, and nothing is changed.
            const KvmEditing::Outcome asked = cameOf(editing, id, typed);
            QCOMPARE(refusalOf(asked), std::string("certificate_differs"));
            QTest::qWait(150);
            QVERIFY(heardOnlyTheQuestions(*there->stand));
            QCOMPARE(there->signIns, 0);
            QCOMPARE(kvmOf(own.wall, id), before);
            QVERIFY(changed.isEmpty());
            // Whoever asked is handed what it is known by now, and what it would be known by
            // there: to be put side by side.
            QCOMPARE(asked.remembered, before.knownByCertificate ? before.certificate : QByteArray());
            QCOMPARE(asked.presented, sha256(shownThere));
            QCOMPARE(asked.onWindowsWord, onWindowsWord);
            QCOMPARE(asked.presentedAt, u"127.0.0.1:%1"_s.arg(there->stand->serverPort()));
            // A yes to the same certificate, known the other way, is no yes to this.
            typed.saidYesTo = asked.presented;
            typed.saidYesOnWindowsWord = !asked.onWindowsWord;
            QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("certificate_differs"));
            QTest::qWait(150);
            QVERIFY(heardOnlyTheQuestions(*there->stand, 2));
            QCOMPARE(there->signIns, 0);
            typed.saidYesOnWindowsWord = asked.onWindowsWord;
            looks = 3;
        }
        // As it is known now, or with the yes: asked what it is (once more for a yes), and only
        // then signed in to, once.
        const KvmEditing::Outcome saved = cameOf(editing, id, typed);
        QVERIFY2(saved.result.ok(), saved.result.ok() ? "" : saved.result.error().message.c_str());
        QVERIFY(saved.result.value().signedIn);
        QCOMPARE(saved.result.value().overPlainHttp, shownThere.isEmpty());
        QCOMPARE(changed.count(), 1);
        // (And, the last, the one its driver asks before the sign-in: a password goes only on a line shown to be up.)
        const qsizetype questions = qsizetype{3} * looks + 1;
        QVERIFY(there->stand->asked.size() > questions);
        for (qsizetype index = 0; index < questions; ++index) {
            const Asked& question = there->stand->asked.at(index);
            QCOMPARE(question.method, "GET"_ba);
            QVERIFY(!question.headers.contains("cookie"));
            QVERIFY(question.body.isEmpty());
        }
        QCOMPARE(there->stand->asked.at(questions).path, QByteArray(signInQuestion));
        QCOMPARE(there->stand->asked.at(questions).body, "user=admin&passwd=secret"_ba);
        QCOMPARE(there->signIns, 1);
        QVERIFY(here->stand->asked.isEmpty());
        // There now, and known as the row says: by the certificate it was known by, by the one
        // that was said yes to, or by none.
        const OwnKvm kept = kvmOf(own.wall, id);
        QCOMPARE(kept.address, there->stand->address());
        QCOMPARE(kept.knownByCertificate, !knownByThere.isEmpty());
        QCOMPARE(kept.certificate, sha256(knownByThere));
        QCOMPARE(saved.result.value().certificate, kept.certificate);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", there->stand->address(), kept.certificate));
        // And the wall's own driver speaks to it there, known so.
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QCOMPARE(there->signIns, 2);
    }

    void whatWasTypedOverAKvmThatIsAnotherByNowIsNotSaved()
    {
        // A box was opened and left open; another window changed the KVM since. What stands in
        // the first box's lines is the KVM as it was.
        OwnWall own;
        const MadeUpKvm kvm;
        OwnKvm one = at(kvm.stand->address(), u"Build server"_s);
        one.group = u"Rack 2"_s;
        const QString id = put(own, one);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.seen = KvmEditing::Seen{kvmOf(own.wall, id), own.wall.secretWrites(id)};
        typed.group = u"Rack 9"_s;
        // The other window renames it.
        KvmEditing::Typed renamed = asItIs(own.wall, id);
        renamed.name = u"Database"_s;
        QVERIFY(changedBy(editing, id, renamed).ok());
        const OwnKvm after = kvmOf(own.wall, id);
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        auto refused = changedBy(editing, id, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("conflict"));
        QCOMPARE(refused.error().message, std::string("This device was changed meanwhile. Look at it again."));
        // The old name is not back, and the group is not changed either.
        QCOMPARE(kvmOf(own.wall, id), after);
        QVERIFY(changed.isEmpty());
        QVERIFY(kvm.stand->asked.isEmpty());

        // Typed over the KVM as it is now, it is saved.
        typed = asItIs(own.wall, id);
        typed.seen = KvmEditing::Seen{kvmOf(own.wall, id), own.wall.secretWrites(id)};
        typed.group = u"Rack 9"_s;
        QVERIFY(changedBy(editing, id, typed).ok());
        QCOMPARE(kvmOf(own.wall, id).name, u"Database"_s);
        QCOMPARE(kvmOf(own.wall, id).group, u"Rack 9"_s);

        // So with a password that another window saved meanwhile, of which the list says
        // nothing: the KVM stands in it as it stood.
        typed = asItIs(own.wall, id);
        typed.seen = KvmEditing::Seen{kvmOf(own.wall, id), own.wall.secretWrites(id)};
        typed.name = u"Renamed"_s;
        QVERIFY(own.wall.change(id, kvmOf(own.wall, id), u"another one"_s).ok());
        QCOMPARE(kvmOf(own.wall, id), typed.seen->kvm);
        refused = changedBy(editing, id, typed);
        QCOMPARE(refused.error().code, std::string("conflict"));
        QCOMPARE(kvmOf(own.wall, id).name, u"Database"_s);
        // And what was seen of another KVM is not this one.
        const MadeUpKvm beside;
        const QString other = put(own, at(beside.stand->address(), u"Beside"_s));
        typed = asItIs(own.wall, id);
        typed.seen = KvmEditing::Seen{kvmOf(own.wall, other), own.wall.secretWrites(other)};
        QCOMPARE(changedBy(editing, id, typed).error().code, std::string("conflict"));
        typed.seen = KvmEditing::Seen{};
        QCOMPARE(changedBy(editing, id, typed).error().code, std::string("conflict"));
    }

    void aChangeThatIsGivenUpChangesNothingAndWhoeverAskedIsToldSoOnce()
    {
        OwnWall own;
        MadeUpKvm kvm;
        Answered lateIn = good({{u"token"_s, u"pass1"_s}});
        lateIn.afterMs = 700;
        kvm.first[signInQuestion].append(lateIn);
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        QList<base::Result<KvmEditing::Changed>> told;
        const auto note = [&told](const KvmEditing::Outcome& outcome) { told.append(outcome.result); };
        // Nothing is under way: nothing is given up, and nobody is told anything.
        editing.giveUp();
        QTest::qWait(50);
        QVERIFY(told.isEmpty());

        // Given up while the KVM is being signed in to.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"Another name"_s;
        typed.password = u"another one"_s;
        editing.change(id, typed, note);
        QTRY_COMPARE(kvm.asked(signInQuestion), 1);
        const qsizetype askedOfIt = kvm.stand->asked.size();
        editing.giveUp();
        // Told later, as of any other end, and once: also where it is given up a second time.
        QVERIFY(told.isEmpty());
        QVERIFY(editing.isUnderWay());
        editing.giveUp();
        QTRY_COMPARE(told.size(), 1);
        QVERIFY(!editing.isUnderWay());
        QVERIFY(!told.constFirst().ok());
        QCOMPARE(told.constFirst().error().code, std::string("given_up"));
        QCOMPARE(told.constFirst().error().message, std::string("The change was given up. Nothing was changed."));
        // The next change begins at once, and is not held up by the one before.
        KvmEditing::Typed renamed = asItIs(own.wall, id);
        renamed.group = u"Rack 2"_s;
        QVERIFY(changedBy(editing, id, renamed).ok());
        QCOMPARE(changed.count(), 1);
        // And when the KVM answers at last, nothing of what was given up is saved, and nothing
        // more is asked of it.
        QTest::qWait(1000);
        QCOMPARE(told.size(), 1);
        QCOMPARE(changed.count(), 1);
        OwnKvm now = before;
        now.group = u"Rack 2"_s;
        QCOMPARE(kvmOf(own.wall, id), now);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        QCOMPARE(kvm.stand->asked.size(), askedOfIt);

        // Given up while a new address is being asked what it is: its password goes nowhere.
        MadeUpKvm elsewhere;
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 600;
        elsewhere.instead.insert(kvmdsQuestion, late);
        typed = asItIs(own.wall, id);
        typed.address = elsewhere.stand->address();
        typed.password = u"secret"_s;
        editing.change(id, typed, note);
        QTRY_COMPARE(elsewhere.stand->asked.size(), 3);
        editing.giveUp();
        QTRY_COMPARE(told.size(), 2);
        QCOMPARE(told.constLast().error().code, std::string("given_up"));
        QTest::qWait(900);
        QCOMPARE(elsewhere.asked(signInQuestion), 0);
        QCOMPARE(kvmOf(own.wall, id), now);
        QCOMPARE(told.size(), 2);
    }

    void aChangeThatIsGivenUpInTheTurnInWhichItWasSavedIsToldAsSaved()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        KvmEditing editing(own.wall, quick());
        QList<base::Result<KvmEditing::Changed>> told;
        const auto note = [&told](const KvmEditing::Outcome& outcome) { told.append(outcome.result); };
        // A name alone is on the wall when change() comes back, and its result comes later:
        // given up in between, whoever asked is told that it was saved, which it was.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"Renamed"_s;
        editing.change(id, typed, note);
        QCOMPARE(kvmOf(own.wall, id).name, u"Renamed"_s);
        editing.giveUp();
        QTRY_COMPARE(told.size(), 1);
        QVERIFY2(told.constFirst().ok(), told.constFirst().ok() ? "" : told.constFirst().error().message.c_str());
        QCOMPARE(told.constFirst().value().name, u"Renamed"_s);

        // So with one that is signed in to first, given up at the moment the wall says that it
        // has changed; and with one whose refusal is on its way.
        const auto atThatWord = connect(&own.wall, &LocalWall::changed, &editing, [&editing] { editing.giveUp(); });
        typed.name = u"Signed in to anew"_s;
        typed.password = u"secret"_s;
        editing.change(id, typed, note);
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 2, 20000);
        QVERIFY2(told.constLast().ok(), told.constLast().ok() ? "" : told.constLast().error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).name, u"Signed in to anew"_s);
        disconnect(atThatWord);
        typed.address = u"not an address at all"_s;
        editing.change(id, typed, note);
        editing.giveUp();
        QTRY_COMPARE(told.size(), 3);
        QCOMPARE(told.constLast().error().code, std::string("invalid"));
        QTest::qWait(200);
        QCOMPARE(told.size(), 3);
    }

    void aFingerprintIsWrittenAsThePageShowsIt()
    {
        // Capitals, and a colon between every two.
        QCOMPARE(core::kvm::fingerprint(QByteArray::fromHex("00ab7f10")), u"00:AB:7F:10"_s);
        const QByteArray sha256 = sha256Of("first");
        QString shownAs = core::kvm::fingerprint(sha256);
        QVERIFY(QRegularExpression(u"\\A([0-9A-F]{2}:){31}[0-9A-F]{2}\\z"_s).match(shownAs).hasMatch());
        QCOMPARE(QByteArray::fromHex(shownAs.remove(u':').toLatin1()), sha256);
        // And nothing where there is none.
        QVERIFY(core::kvm::fingerprint({}).isEmpty());
    }

    void whatAKvmIsKnownByAndWhatItShowsAreReadWithNothingThatSaysWhoAsks()
    {
        OwnWall own;
        const MadeUpKvm kvm(tls("first"));
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmCertificateReview review(own.wall, quickProbe());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        QVERIFY(!review.isUnderWay());
        const auto shown = shownBy(review, id);
        QVERIFY2(shown.ok(), shown.ok() ? "" : shown.error().message.c_str());
        QVERIFY(!review.isUnderWay());
        // The same KVM as before: they match. And its own certificate is no other KVM's.
        QCOMPARE(shown.value().remembered, sha256Of("first"));
        QCOMPARE(shown.value().presented, sha256Of("first"));
        QVERIFY(shown.value().presentedIsOf.isEmpty());
        QVERIFY(shown.value().presentedIsCalled.isEmpty());
        // It was asked what it is, and nothing else: no password, no session, and no sign-in.
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
        QCOMPARE(kvm.signIns, 0);
        // And looking changes nothing.
        QCOMPARE(kvmOf(own.wall, id), before);
        QVERIFY(changed.isEmpty());
    }

    void aKvmThatShowsAnotherCertificateIsLetInAgainOnceSomebodySaidYesToIt()
    {
        OwnWall own;
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        const QString id = put(own, at(kvm->stand->address(), u"Build server"_s, "first"));
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        // After a factory reset it has another certificate, at the address it had.
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        const MadeUpKvm reset(tls("second"), port);
        QVERIFY(reset.stand->isListening());
        const core::Status refused = onceItIs(own.wall, id, u"error"_s);
        QCOMPARE(refused.errorCode, u"cert_mismatch"_s);
        QCOMPARE(refused.errorMessage, QLatin1StringView(certificateChanged));
        QVERIFY(reset.stand->asked.isEmpty());

        // Both are shown: the one it is known by, and the one it shows right now.
        KvmCertificateReview review(own.wall, quickProbe());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        const auto shown = shownBy(review, id);
        QVERIFY2(shown.ok(), shown.ok() ? "" : shown.error().message.c_str());
        QCOMPARE(shown.value().remembered, sha256Of("first"));
        QCOMPARE(shown.value().presented, sha256Of("second"));
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*reset.stand));
        // Nothing is trusted by looking.
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("first"));
        QVERIFY(changed.isEmpty());

        // Somebody says yes to the one that was shown.
        const auto trusted = trustedBy(review, id, shown.value().presented);
        QVERIFY2(trusted.ok(), trusted.ok() ? "" : trusted.error().message.c_str());
        QCOMPARE(changed.count(), 1);
        OwnKvm kept = kvmOf(own.wall, id);
        QVERIFY(kept.knownByCertificate);
        QCOMPARE(kept.certificate, sha256Of("second"));
        QCOMPARE(kept.name, u"Build server"_s);
        QCOMPARE(core::Settings(own.file).remembered().kvms.constFirst(), kept);
        // It was read once more for that, again with nothing that says who asks; and until then
        // nothing of a password went anywhere.
        QVERIFY(heardOnlyTheQuestions(*reset.stand, 2));
        QCOMPARE(reset.signIns, 0);
        // The wall's own driver is a new one, and the next question goes out under the new
        // certificate: it is signed in to, with the password that was kept all along, which is
        // saved for that certificate from now on.
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QCOMPARE(reset.signIns, 1);
        QCOMPARE(reset.last(signInQuestion)->body, "user=admin&passwd=secret"_ba);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", reset.stand->address(), sha256Of("second")));

        // And where both are the same, saying yes changes nothing.
        QVERIFY(trustedBy(review, id, sha256Of("second")).ok());
        QCOMPARE(changed.count(), 1);
    }

    void whatShowsTheCertificateOfAnotherKvmOfTheWallIsSaidToBeThatKvm()
    {
        // Two KVMs of the wall have changed places: their addresses were handed out anew. The
        // card of the second says that its certificate does not match. What answers at its
        // address shows the certificate the first is known by; and whoever compares that with
        // what the KVM's own page shows finds the two the same, since it is that KVM. So it is
        // said with the two certificates, before anybody compares, whose the one is.
        OwnWall own;
        ChangedPlaces places;
        changePlaces(own, places);
        QVERIFY(places.atTheSeconds != nullptr && places.atTheSeconds->stand->isListening());
        const OwnKvm before = kvmOf(own.wall, places.second);
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmCertificateReview review(own.wall, quickProbe());
        const auto shown = shownBy(review, places.second);
        QVERIFY2(shown.ok(), shown.ok() ? "" : shown.error().message.c_str());
        QCOMPARE(shown.value().remembered, sha256Of("second"));
        QCOMPARE(shown.value().presented, sha256Of("first"));
        QCOMPARE(shown.value().presentedIsOf, places.first);
        QCOMPARE(shown.value().presentedIsCalled, u"Build server"_s);
        // Looking changes nothing, and says nothing of who asks.
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*places.atTheSeconds->stand));
        QCOMPARE(kvmOf(own.wall, places.second), before);
        QVERIFY(changed.isEmpty());

        // A KVM without a name is called by where it is, as its card calls it.
        OwnKvm unnamed = kvmOf(own.wall, places.first);
        unnamed.name.clear();
        QVERIFY(own.wall.change(places.first, unnamed, std::nullopt).ok());
        const auto again = shownBy(review, places.second);
        QVERIFY(again.ok());
        QCOMPARE(again.value().presentedIsOf, places.first);
        QCOMPARE(again.value().presentedIsCalled, u"127.0.0.1:%1"_s.arg(places.firstsPort));
    }

    void sayingYesToTheCertificateAnotherKvmOfTheWallIsKnownByIsRefused()
    {
        // Said yes to, the second's kept password would go to the first KVM, the card named for
        // the second would press the first's buttons, and two entries would hold one KVM, each
        // with its own "one thing at a time". Refused in the sentence that adding has for it,
        // which names the KVM it is.
        OwnWall own;
        ChangedPlaces places;
        changePlaces(own, places);
        QVERIFY(places.atTheSeconds != nullptr && places.atTheSeconds->stand->isListening());
        const OwnKvm before = kvmOf(own.wall, places.second);
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmCertificateReview review(own.wall, quickProbe());
        const auto refused = trustedBy(review, places.second, sha256Of("first"));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("same_device"));
        QCOMPARE(QString::fromStdString(refused.error().message),
                 u"This is the same device as \"Build server\", which is on the wall already (127.0.0.1:%1)"_s.arg(
                     places.firstsPort));
        QVERIFY(!review.isUnderWay());
        QCOMPARE(kvmOf(own.wall, places.second), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(places.second)),
                 savedAs(own.wall, places.second, "the second's own"));
        QCOMPARE(own.wall.secretWrites(places.second), quint64{0});
        QVERIFY(changed.isEmpty());
        // The wall's own driver for the second knows it by the certificate it had, as before:
        // what answers at its address is not that, and hears nothing. All that the first KVM was
        // asked there is what it shows, once: no sign-in, and nothing of the second's password.
        const core::Status status = onceItIs(own.wall, places.second, u"error"_s);
        QCOMPARE(status.errorCode, u"cert_mismatch"_s);
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*places.atTheSeconds->stand));
        QCOMPARE(places.atTheSeconds->signIns, 0);
        for (const Asked& one : std::as_const(places.atTheSeconds->stand->asked)) {
            QVERIFY(!one.body.contains("own") && !one.path.contains("own"));
        }
    }

    void whatIsNotTheCertificateThatWasShownIsNotTrusted()
    {
        OwnWall own;
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        const QString id = put(own, at(kvm->stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        auto second = std::make_unique<MadeUpKvm>(tls("second"), port);
        QVERIFY(second->stand->isListening());
        KvmCertificateReview review(own.wall, quickProbe());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        const auto shown = shownBy(review, id);
        QVERIFY(shown.ok());
        QCOMPARE(shown.value().presented, sha256Of("second"));
        // While somebody reads the two, something else again takes its place.
        second.reset();
        const MadeUpKvm third(tls("comet"), port);
        QVERIFY(third.stand->isListening());
        const auto refused = trustedBy(review, id, shown.value().presented);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("cert_mismatch"));
        QCOMPARE(refused.error().message,
                 std::string("The device now presents a different certificate than the one shown"));
        // Nor is anything trusted that nobody was shown: nothing at all, something that is no
        // certificate's SHA-256, and the one it is known by, which it does not show.
        for (const QByteArray& unseen : {QByteArray(), QByteArray(32, 'x'), QByteArray(31, 'x'), sha256Of("first")}) {
            QCOMPARE(trustedBy(review, id, unseen).error().code, std::string("cert_mismatch"));
        }
        QCOMPARE(kvmOf(own.wall, id), before);
        QVERIFY(changed.isEmpty());
        QTest::qWait(150);
        QVERIFY(heardNothingOfWhoAsks(*third.stand));
        QCOMPARE(third.signIns, 0);
    }

    void whatAKvmShowsInThePlaceOfWhatWasSaidYesToIsHandedOverToBeShown()
    {
        // The KVM is known by "first" and shows "second" by now; another KVM of the wall is
        // known by "comet".
        OwnWall own;
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        const QString id = put(own, at(kvm->stand->address(), u"Build server"_s, "first"));
        const MadeUpKvm beside(tls("comet"));
        const QString other = put(own, at(beside.stand->address(), u"Database"_s, "comet"));
        const OwnKvm before = kvmOf(own.wall, id);
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        auto second = std::make_unique<MadeUpKvm>(tls("second"), port);
        QVERIFY(second->stand->isListening());
        KvmCertificateReview review(own.wall, quickProbe());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        const auto yesTo = [&](const QString& which, const QByteArray& shown) {
            QList<KvmCertificateReview::Outcome> told;
            review.trust(which, shown, [&told](const KvmCertificateReview::Outcome& yes) { told.append(yes); });
            static_cast<void>(QTest::qWaitFor([&told] { return !told.isEmpty(); }, 20000));
            return told.isEmpty() ? KvmCertificateReview::Outcome{base::Error{"never", "The yes never ended."}, {}}
                                  : told.constFirst();
        };
        const auto shown = shownBy(review, id);
        QVERIFY(shown.ok());
        QCOMPARE(shown.value().presented, sha256Of("second"));

        // While somebody reads the two, something else again takes its place. The yes is
        // refused; and whoever asked is handed what it shows now, beside what the KVM is known
        // by, to be shown in the place of the two that were said yes to.
        second.reset();
        auto third = std::make_unique<MadeUpKvm>(tls("twin-one"), port);
        QVERIFY(third->stand->isListening());
        KvmCertificateReview::Outcome refused = yesTo(id, shown.value().presented);
        QVERIFY(!refused.result.ok());
        QCOMPARE(refused.result.error().code, std::string("cert_mismatch"));
        QCOMPARE(refused.shownNow.remembered, sha256Of("first"));
        QCOMPARE(refused.shownNow.presented, sha256Of("twin-one"));
        QVERIFY(refused.shownNow.presentedIsOf.isEmpty());
        QVERIFY(refused.shownNow.presentedIsCalled.isEmpty());
        // And whose it is, where it is another KVM of the wall's.
        third.reset();
        const MadeUpKvm fourth(tls("comet"), port);
        QVERIFY(fourth.stand->isListening());
        refused = yesTo(id, sha256Of("twin-one"));
        QCOMPARE(refused.result.error().code, std::string("cert_mismatch"));
        QCOMPARE(refused.shownNow.remembered, sha256Of("first"));
        QCOMPARE(refused.shownNow.presented, sha256Of("comet"));
        QCOMPARE(refused.shownNow.presentedIsOf, other);
        QCOMPARE(refused.shownNow.presentedIsCalled, u"Database"_s);
        // Nothing was trusted by any of it.
        QCOMPARE(kvmOf(own.wall, id), before);
        QVERIFY(changed.isEmpty());

        // Any other end of a yes hands over no certificates: one that is refused for another
        // reason, and one that is taken.
        refused = yesTo(u"00000000"_s, sha256Of("comet"));
        QCOMPARE(refused.result.error().code, std::string("not_found"));
        QVERIFY(refused.shownNow.remembered.isEmpty() && refused.shownNow.presented.isEmpty());
        const KvmCertificateReview::Outcome taken = yesTo(other, sha256Of("comet"));
        QVERIFY2(taken.result.ok(), taken.result.ok() ? "" : taken.result.error().message.c_str());
        QVERIFY(taken.shownNow.remembered.isEmpty() && taken.shownNow.presented.isEmpty());
    }

    void whatAChangeCameToIsKnownFromWhenItIsSettledUntilWhoeverAskedIsTold()
    {
        // For whoever ends a change in the turn in which it was saved: it would tell nobody.
        OwnWall own;
        MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        KvmEditing editing(own.wall, quick());
        QList<KvmEditing::Outcome> told;
        const auto note = [&told](const KvmEditing::Outcome& outcome) { told.append(outcome); };
        // What it says it came to: "open" while nothing is settled, or nobody is to be told.
        const auto cameTo = [&editing] {
            const std::optional<KvmEditing::Outcome>& settled = editing.cameTo();
            return settled.has_value() ? QString::fromStdString(refusalOf(*settled)) : u"open"_s;
        };
        QCOMPARE(cameTo(), u"open"_s);
        // A name alone is on the wall when change() comes back: settled, and nobody told yet.
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"Renamed"_s;
        editing.change(id, typed, note);
        QVERIFY(told.isEmpty());
        QCOMPARE(cameTo(), u"saved"_s);
        QTRY_COMPARE(told.size(), 1);
        QVERIFY(told.constFirst().result.ok());
        QCOMPARE(told.constFirst().result.value().name, u"Renamed"_s);
        QCOMPARE(cameTo(), u"open"_s);

        // One that is signed in to first: open while the KVM is asked, and settled from the
        // moment the wall says aloud that it has changed.
        Answered lateIn = good({{u"token"_s, u"pass1"_s}});
        // (Late enough for the box to be looked at while it waits, also on a busy PC under the
        // address sanitizer, where the lines are let in one after the other on this thread.)
        lateIn.afterMs = 1000;
        kvm.first[signInQuestion].append(lateIn);
        QStringList atThatWord;
        const auto listening = connect(&own.wall, &LocalWall::changed, &editing, [&] { atThatWord.append(cameTo()); });
        typed.name = u"Signed in to anew"_s;
        typed.password = u"secret"_s;
        editing.change(id, typed, note);
        QTRY_COMPARE(kvm.asked(signInQuestion), 1);
        QCOMPARE(cameTo(), u"open"_s);
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 2, 20000);
        QCOMPARE(atThatWord, QStringList{u"saved"_s});
        QVERIFY(told.constLast().result.ok());
        QCOMPARE(cameTo(), u"open"_s);
        disconnect(listening);

        // And a refusal that is on its way.
        typed.address = u"not an address at all"_s;
        editing.change(id, typed, note);
        QCOMPARE(cameTo(), u"invalid"_s);
        QTRY_COMPARE(told.size(), 3);
        QCOMPARE(cameTo(), u"open"_s);
    }

    void whatAYesCameToIsKnownFromWhenItIsSettledUntilWhoeverAskedIsTold()
    {
        OwnWall own;
        MadeUpKvm kvm(tls("second"));
        Answered late = refusal(401, u"UnauthorizedError"_s);
        // (Late enough for the review to be looked at while it waits for it: the three lines of a
        // reading are let in one after the other on this thread, and on a busy PC under the
        // address sanitizer the last came after an answer given 400 ms.)
        late.afterMs = 1000;
        kvm.instead.insert(kvmdsQuestion, late);
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s, "first"));
        KvmCertificateReview review(own.wall, quickProbe());
        QList<KvmCertificateReview::Outcome> told;
        const auto note = [&told](const KvmCertificateReview::Outcome& yes) { told.append(yes); };
        // What it says it came to: "open" while nothing is settled, or nobody is to be told.
        const auto cameTo = [&review] {
            const std::optional<KvmCertificateReview::Outcome>& settled = review.cameTo();
            return !settled.has_value() ? u"open"_s : settled->result.ok() ? u"trusted"_s : u"refused"_s;
        };
        QCOMPARE(cameTo(), u"open"_s);
        // Open while the KVM is read once more, and settled from the moment the wall says aloud
        // that it has changed.
        QStringList atThatWord;
        connect(&own.wall, &LocalWall::changed, &review, [&] { atThatWord.append(cameTo()); });
        review.trust(id, sha256Of("second"), note);
        QTRY_COMPARE(kvm.stand->asked.size(), 3);
        QCOMPARE(cameTo(), u"open"_s);
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QVERIFY(told.constFirst().result.ok());
        QCOMPARE(atThatWord, QStringList{u"trusted"_s});
        QCOMPARE(cameTo(), u"open"_s);
        // A refusal that is on its way.
        review.trust(u"00000000"_s, sha256Of("second"), note);
        QCOMPARE(cameTo(), u"refused"_s);
        QTRY_COMPARE(told.size(), 2);
        QCOMPARE(cameTo(), u"open"_s);
        // And nothing of a reading, which trusts nothing.
        QList<base::Result<KvmCertificateReview::Shown>> shown;
        review.askCertificates(u"00000000"_s, [&shown](const auto& result) { shown.append(result); });
        QCOMPARE(cameTo(), u"open"_s);
        QTRY_COMPARE(shown.size(), 1);
    }

    void aKvmThatWindowsVouchesForIsKnownByItsCertificateOnceThatIsTrusted()
    {
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        const MadeUpKvm kvm(tls("signed"));
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        KvmCertificateReview review(own.wall, quickProbe());
        // Known by none yet, and by Windows' word.
        const auto shown = shownBy(review, id);
        QVERIFY2(shown.ok(), shown.ok() ? "" : shown.error().message.c_str());
        QVERIFY(shown.value().remembered.isEmpty());
        QCOMPARE(shown.value().presented, sha256Of("signed"));
        QVERIFY(trustedBy(review, id, shown.value().presented).ok());
        QVERIFY(kvmOf(own.wall, id).knownByCertificate);
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("signed"));
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
    }

    void aKvmOverPlainHttpHasNoCertificateToLookAtOrToTrust()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmCertificateReview review(own.wall, quickProbe());
        const auto shown = shownBy(review, id);
        QVERIFY(!shown.ok());
        QCOMPARE(shown.error().code, std::string("no_certificate"));
        QCOMPARE(shown.error().message, std::string(noCertificateThere));
        const auto trusted = trustedBy(review, id, QByteArray(32, 'x'));
        QVERIFY(!trusted.ok());
        QCOMPARE(trusted.error().code, std::string("no_certificate"));
        QCOMPARE(trusted.error().message, std::string(noCertificateThere));
        // It was asked nothing for that, and is known by nothing as before.
        QTest::qWait(150);
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvmOf(own.wall, id), before);
    }

    void aKvmThatDoesNotAnswerShowsNoCertificate()
    {
        OwnWall own;
        const core::testing::NobodyThere nobody;
        const QString id = put(own, at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"Away"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmCertificateReview review(own.wall, quickProbe());
        const auto shown = shownBy(review, id);
        QVERIFY(!shown.ok());
        QCOMPARE(shown.error().code, std::string("unreachable"));
        QCOMPARE(shown.error().message, std::string("Device is unreachable"));
        QCOMPARE(trustedBy(review, id, sha256Of("first")).error().code, std::string("unreachable"));
        QCOMPARE(kvmOf(own.wall, id), before);
        // And one that is not on the wall has none to show.
        QCOMPARE(shownBy(review, u"00000000"_s).error().code, std::string("not_found"));
        QCOMPARE(trustedBy(review, u"00000000"_s, sha256Of("first")).error().message, std::string("Unknown device"));
    }

    void oneCertificateIsLookedAtATime()
    {
        OwnWall own;
        MadeUpKvm kvm(tls("first"));
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 500;
        kvm.instead.insert(kvmdsQuestion, late);
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s, "first"));
        KvmCertificateReview review(own.wall, quickProbe());
        QList<base::Result<KvmCertificateReview::Shown>> shown;
        QList<base::Result<KvmCertificateReview::Shown>> second;
        QList<base::Result<void>> trusted;
        review.askCertificates(
            id, [&shown](const base::Result<KvmCertificateReview::Shown>& result) { shown.append(result); });
        QVERIFY(review.isUnderWay());
        review.askCertificates(
            id, [&second](const base::Result<KvmCertificateReview::Shown>& result) { second.append(result); });
        review.trust(id, sha256Of("first"),
                     [&trusted](const KvmCertificateReview::Outcome& yes) { trusted.append(yes.result); });
        // Not there and then, either.
        QVERIFY(second.isEmpty() && trusted.isEmpty());
        QTRY_VERIFY(second.size() == 1 && trusted.size() == 1);
        QCOMPARE(second.constFirst().error().code, std::string("busy"));
        QCOMPARE(second.constFirst().error().message,
                 std::string("A certificate is being looked at at this moment. Wait for it to finish."));
        QCOMPARE(trusted.constFirst().error().code, std::string("busy"));
        // The first goes on as if nothing had been asked, and comes once.
        QVERIFY(shown.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(shown.size(), 1, 20000);
        QVERIFY(shown.constFirst().ok());
        QVERIFY(!review.isUnderWay());
        QTest::qWait(300);
        QCOMPARE(shown.size(), 1);
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
        // Whoever is told may ask the next there and then.
        review.askCertificates(id, [&](const base::Result<KvmCertificateReview::Shown>& result) {
            shown.append(result);
            review.trust(id, result.ok() ? result.value().presented : QByteArray(),
                         [&trusted](const KvmCertificateReview::Outcome& after) { trusted.append(after.result); });
        });
        QTRY_COMPARE_WITH_TIMEOUT(trusted.size(), 2, 20000);
        QVERIFY2(trusted.constLast().ok(), trusted.constLast().ok() ? "" : trusted.constLast().error().message.c_str());
    }

    void aKvmThatWasChangedWhileItsCertificateWasReadIsLookedAtAgain_data()
    {
        QTest::addColumn<int>("delayedTls");
        QTest::newRow("immediate TLS") << 0;
        QTest::newRow("delayed TLS") << 1800;
    }

    void aKvmThatWasChangedWhileItsCertificateWasReadIsLookedAtAgain()
    {
        QFETCH(int, delayedTls);
        OwnWall own;
        MadeUpKvm kvm(tls("first"));
        QVERIFY(kvm.stand->letInOnlyAfter(delayedTls));
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 500;
        kvm.instead.insert(kvmdsQuestion, late);
        const MadeUpKvm elsewhere(tls("second"));
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s, "first"));
        // Review/cancel ordering is tested after TLS, not against a shortened
        // connection deadline. The delayed row exercises that distinction.
        KvmCertificateReview review(own.wall);
        QList<base::Result<void>> told;
        const auto note = [&told](const KvmCertificateReview::Outcome& yes) { told.append(yes.result); };
        // It is moved to another address while what it shows at the old one is on its way: that
        // is not the certificate of what is at the new one.
        review.trust(id, sha256Of("first"), note);
        QTRY_COMPARE_WITH_TIMEOUT(kvm.stand->asked.size(), 3, 20000);
        OwnKvm moved = kvmOf(own.wall, id);
        moved.address = elsewhere.stand->address();
        QVERIFY(own.wall.change(id, moved, std::nullopt).ok());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QCOMPARE(told.constFirst().error().code, std::string("conflict"));
        QCOMPARE(told.constFirst().error().message,
                 std::string("This device was changed meanwhile. Look at it again."));
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("first"));
        // And one that is taken off the wall meanwhile is not there.
        moved.address = kvm.stand->address();
        QVERIFY(own.wall.change(id, moved, std::nullopt).ok());
        review.trust(id, sha256Of("first"), note);
        QTRY_COMPARE_WITH_TIMEOUT(kvm.stand->asked.size(), 6, 20000);
        QVERIFY(own.wall.remove(id).ok());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 2, 20000);
        QCOMPARE(told.constLast().error().code, std::string("not_found"));
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void aReviewThatIsGoneTrustsNothingAndTellsNobody()
    {
        OwnWall own;
        MadeUpKvm kvm(tls("second"));
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 600;
        kvm.instead.insert(kvmdsQuestion, late);
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        int told = 0;
        {
            KvmCertificateReview review(own.wall, quickProbe());
            review.trust(id, sha256Of("second"), [&told](const KvmCertificateReview::Outcome&) { ++told; });
            QTRY_COMPARE(kvm.stand->asked.size(), 3);
        }
        {
            // (And gone with a refusal on its way.)
            KvmCertificateReview review(own.wall, quickProbe());
            review.trust(u"00000000"_s, sha256Of("second"), [&told](const KvmCertificateReview::Outcome&) { ++told; });
        }
        QTest::qWait(1000);
        QCOMPARE(told, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QVERIFY(changed.isEmpty());
    }

    void whoeverHearsTheWallSayThatItChangedMayEndTheReview()
    {
        // The wall says so while the certificate is remembered: a box that closes on that word
        // takes the review with it.
        OwnWall own;
        const MadeUpKvm kvm(tls("second"));
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s, "first"));
        auto review = std::make_unique<KvmCertificateReview>(own.wall, quickProbe());
        int told = 0;
        connect(&own.wall, &LocalWall::changed, this, [&review] { review.reset(); });
        review->trust(id, sha256Of("second"), [&told](const KvmCertificateReview::Outcome&) { ++told; });
        QTRY_VERIFY_WITH_TIMEOUT(review == nullptr, 20000);
        QTest::qWait(300);
        // Nobody is told by what is gone. The certificate is the KVM's, as the wall itself said.
        QCOMPARE(told, 0);
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("second"));
    }

    void aLookAtACertificateThatIsGivenUpTrustsNothingAndWhoeverAskedIsToldSoOnce_data()
    {
        QTest::addColumn<int>("delayedTls");
        QTest::newRow("immediate TLS") << 0;
        QTest::newRow("delayed TLS") << 1800;
    }

    void aLookAtACertificateThatIsGivenUpTrustsNothingAndWhoeverAskedIsToldSoOnce()
    {
        QFETCH(int, delayedTls);
        OwnWall own;
        MadeUpKvm kvm(tls("second"));
        QVERIFY(kvm.stand->letInOnlyAfter(delayedTls));
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 600;
        kvm.instead.insert(kvmdsQuestion, late);
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        // Review/cancel ordering is tested after TLS, not against a shortened
        // connection deadline. The delayed row exercises that distinction.
        KvmCertificateReview review(own.wall);
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        QList<base::Result<KvmCertificateReview::Shown>> shown;
        QList<base::Result<void>> trusted;
        // Nothing is under way: nothing is given up, and nobody is told anything.
        review.giveUp();

        // A reading, given up while the KVM is slow to say what it is.
        review.askCertificates(
            id, [&shown](const base::Result<KvmCertificateReview::Shown>& result) { shown.append(result); });
        QTRY_COMPARE_WITH_TIMEOUT(kvm.stand->asked.size(), 3, 20000);
        review.giveUp();
        // Told later, as of any other end, and once: also where it is given up a second time.
        QVERIFY(shown.isEmpty());
        QVERIFY(review.isUnderWay());
        review.giveUp();
        QTRY_COMPARE(shown.size(), 1);
        QVERIFY(!review.isUnderWay());
        QVERIFY(!shown.constFirst().ok());
        QCOMPARE(shown.constFirst().error().code, std::string("given_up"));
        QCOMPARE(shown.constFirst().error().message,
                 std::string("The look at the certificate was given up. Nothing was trusted."));

        // And a yes, given up while the KVM is read once more for it.
        review.trust(id, sha256Of("second"),
                     [&trusted](const KvmCertificateReview::Outcome& yes) { trusted.append(yes.result); });
        QTRY_COMPARE_WITH_TIMEOUT(kvm.stand->asked.size(), 6, 20000);
        review.giveUp();
        QTRY_COMPARE(trusted.size(), 1);
        QVERIFY(!trusted.constFirst().ok());
        QCOMPARE(trusted.constFirst().error().code, std::string("given_up"));
        // The next look begins at once, and is not held up by the ones before.
        kvm.instead.remove(kvmdsQuestion);
        const auto next = shownBy(review, id);
        QVERIFY2(next.ok(), next.ok() ? "" : next.error().message.c_str());
        QCOMPARE(next.value().presented, sha256Of("second"));
        // And when the KVM answers at last, nothing is trusted, and nobody is told a second time.
        QTest::qWait(900);
        QCOMPARE(shown.size(), 1);
        QCOMPARE(trusted.size(), 1);
        QCOMPARE(kvmOf(own.wall, id), before);
        QVERIFY(changed.isEmpty());
    }

    void aYesThatIsGivenUpAtTheMomentItIsRememberedIsToldAsTrusted()
    {
        OwnWall own;
        const MadeUpKvm kvm(tls("second"));
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s, "first"));
        KvmCertificateReview review(own.wall, quickProbe());
        QList<base::Result<void>> told;
        const auto note = [&told](const KvmCertificateReview::Outcome& yes) { told.append(yes.result); };
        // The wall says that it has changed while the certificate is remembered: given up at
        // that word, whoever asked is told that it is trusted, which it is.
        const auto atThatWord = connect(&own.wall, &LocalWall::changed, &review, [&review] { review.giveUp(); });
        review.trust(id, sha256Of("second"), note);
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QVERIFY2(told.constFirst().ok(), told.constFirst().ok() ? "" : told.constFirst().error().message.c_str());
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("second"));
        disconnect(atThatWord);
        // And with a refusal on its way, that refusal.
        review.trust(u"00000000"_s, sha256Of("second"), note);
        review.giveUp();
        QTRY_COMPARE(told.size(), 2);
        QCOMPARE(told.constLast().error().code, std::string("not_found"));
        QTest::qWait(200);
        QCOMPARE(told.size(), 2);
    }

    void aYesOnWindowsWordThatMeetsWhatWindowsDoesNotTrustAtTheSignInSendsNoPassword()
    {
        // Windows vouches for what is shown at the new address, and somebody said yes to its
        // word. It is read once more, and Windows still vouches. Then, before the sign-in goes
        // out on a line of its own, something stands there that Windows does not trust: that
        // line is held to Windows' word, and to nothing else.
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        auto door = std::make_unique<MadeUpKvm>(tls("signed"));
        const quint16 port = door->stand->serverPort();
        const QString id = put(own, at(here.stand->address(), u"Build server"_s, "first"));
        const OwnKvm before = kvmOf(own.wall, id);
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = door->stand->address();
        typed.password = u"secret"_s;
        typed.saidYesTo = sha256Of("signed");
        typed.saidYesOnWindowsWord = true;
        // Switch before the final probe answer is sent, while all three old TLS lines exist.
        // No timed delay consumes the probe deadline or lets sign-in race the test runner.
        std::unique_ptr<MadeUpKvm> other;
        door->stand->beforeAnswer = [&] {
            if (door->stand->asked.size() == 3) {
                door->stand->takeNoMore();
                other = std::make_unique<MadeUpKvm>(tls("comet"), port);
            }
        };
        QList<KvmEditing::Outcome> told;
        editing.change(id, typed, [&told](const KvmEditing::Outcome& outcome) { told.append(outcome); });
        // Even if the outer test resumes late, the request barrier has already switched it.
        QTest::qWait(750);
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QVERIFY(other != nullptr);
        QVERIFY(other->stand->isListening());
        QCOMPARE(door->stand->asked.size(), 3);
        // The driver's own word, and nothing of two certificates: none was said yes to.
        QCOMPARE(refusalOf(told.constFirst()), std::string("cert_invalid"));
        QVERIFY(told.constFirst().presented.isEmpty() && told.constFirst().remembered.isEmpty());
        QTest::qWait(150);
        QVERIFY(other->stand->asked.isEmpty());
        QCOMPARE(other->signIns, 0);
        QCOMPARE(door->signIns, 0);
        QCOMPARE(kvmOf(own.wall, id), before);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
    }

    void aKvmThatIsNotWhatWhoeverAsksHadBeforeThemIsNeitherReadNorTrusted()
    {
        // A box was filled with the KVM and left open; another window changed the KVM since.
        // The two certificates would stand in that box beside the KVM as it was.
        OwnWall own;
        MadeUpKvm kvm(tls("second"));
        const MadeUpKvm otherKvm(tls("comet"));
        const QString id = put(own, at(kvm.stand->address(), u"Build server"_s, "first"));
        const QString other = put(own, at(otherKvm.stand->address(), u"Database"_s, "comet"));
        KvmCertificateReview review(own.wall, quickProbe());
        const auto shownWith = [&](const QString& which, const KvmEditing::Seen& seen) {
            QList<base::Result<KvmCertificateReview::Shown>> told;
            review.askCertificates(
                which, [&told](const base::Result<KvmCertificateReview::Shown>& result) { told.append(result); }, seen);
            static_cast<void>(QTest::qWaitFor([&told] { return !told.isEmpty(); }, 20000));
            return told.isEmpty()           ? std::string("never")
                   : told.constFirst().ok() ? "shown"
                                            : told.constFirst().error().code;
        };
        const auto trustedWith = [&](const QString& which, const KvmEditing::Seen& seen) {
            QList<KvmCertificateReview::Outcome> told;
            review.trust(
                which, sha256Of("second"), [&told](const KvmCertificateReview::Outcome& yes) { told.append(yes); },
                seen);
            static_cast<void>(QTest::qWaitFor([&told] { return !told.isEmpty(); }, 20000));
            return told.isEmpty()                  ? std::string("never")
                   : told.constFirst().result.ok() ? "trusted"
                                                   : told.constFirst().result.error().code;
        };
        const KvmEditing::Seen asFilled{kvmOf(own.wall, id), own.wall.secretWrites(id)};

        // As it is what the box was filled with, it is read.
        QCOMPARE(shownWith(id, asFilled), std::string("shown"));
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
        // Another window renames it: neither read nor trusted from that box, in the words a
        // change has for it, and nothing is asked of the KVM.
        OwnKvm renamed = kvmOf(own.wall, id);
        renamed.name = u"Renamed"_s;
        QVERIFY(own.wall.change(id, renamed, std::nullopt).ok());
        QCOMPARE(shownWith(id, asFilled), std::string("conflict"));
        QCOMPARE(trustedWith(id, asFilled), std::string("conflict"));
        // Nor is what was seen of one KVM a yes for another.
        QCOMPARE(shownWith(other, asFilled), std::string("conflict"));
        QCOMPARE(trustedWith(other, asFilled), std::string("conflict"));
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
        QVERIFY(otherKvm.stand->asked.isEmpty());
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("first"));
        // So where only its password was saved anew meanwhile, of which the list shows nothing.
        const KvmEditing::Seen asRenamed{kvmOf(own.wall, id), own.wall.secretWrites(id)};
        QVERIFY(own.wall.change(id, kvmOf(own.wall, id), u"another one"_s).ok());
        QCOMPARE(trustedWith(id, asRenamed), std::string("conflict"));
        // And one that is changed while it is read for the yes is not trusted either.
        const KvmEditing::Seen asItIsNow{kvmOf(own.wall, id), own.wall.secretWrites(id)};
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 500;
        kvm.instead.insert(kvmdsQuestion, late);
        QList<KvmCertificateReview::Outcome> told;
        review.trust(
            id, sha256Of("second"), [&told](const KvmCertificateReview::Outcome& yes) { told.append(yes); }, asItIsNow);
        QTRY_VERIFY(kvm.stand->asked.size() > 3);
        OwnKvm regrouped = kvmOf(own.wall, id);
        regrouped.group = u"Rack 9"_s;
        QVERIFY(own.wall.change(id, regrouped, std::nullopt).ok());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QCOMPARE(told.constFirst().result.error().code, std::string("conflict"));
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("first"));
        // With the KVM as it is before whoever asks, the yes is taken.
        kvm.instead.remove(kvmdsQuestion);
        const KvmEditing::Seen last{kvmOf(own.wall, id), own.wall.secretWrites(id)};
        QCOMPARE(trustedWith(id, last), std::string("trusted"));
        QCOMPARE(kvmOf(own.wall, id).certificate, sha256Of("second"));
    }

    // --- a KVM that was not asked yet, and what the wall records by itself ------------------------

    void aKvmThatWasNotAskedYetIsMovedWithItsKeptPasswordAsTheWallMovesIt()
    {
        // A KVM as a file brought it: pinned over HTTPS, its password kept, never asked. The
        // address it came with is stale: nobody is there, and the asking there never succeeds.
        // In the box: the address it has now, and its password left as it is kept (whoever
        // took it from a file may never have seen that).
        OwnWall own;
        const core::testing::NobodyThere stale;
        const MadeUpKvm kvm(tls("first"));
        LocalWall::FromAFile brought;
        brought.kvm = at(u"https://127.0.0.1:%1"_s.arg(stale.port()), u"From a file"_s, "first");
        brought.password = u"secret"_s;
        const QString id = own.wall.takeFromAFile({brought}).constFirst().value();
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.address = kvm.stand->address();
        const KvmEditing::Outcome outcome = cameOf(editing, id, typed);
        QCOMPARE(refusalOf(outcome), std::string("saved"));
        // Moved, with nothing signed in to: what went to the new address is the three
        // questions, on lines that were held to the certificate it is known by.
        QVERIFY(!outcome.result.value().signedIn);
        QCOMPARE(outcome.result.value().certificate, sha256Of("first"));
        QCOMPARE(kvmOf(own.wall, id).address, kvm.stand->address());
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
        QCOMPARE(kvm.signIns, 0);
        // Its password went with it in the store, saved for the address it has now.
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), savedAs(own.wall, id, "secret"));
        // It is still one that was not asked: the wall asks it there itself, and signs in then.
        QVERIFY(!own.wall.wasAskedWhatItIs(id));
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QVERIFY(own.wall.wasAskedWhatItIs(id));
        QCOMPARE(kvm.signIns, 1);
        // (The box's three questions, the wall's three, the one asked before the sign-in, and then the sign-in.)
        QCOMPARE(kvm.stand->asked.value(7).path, QByteArray(signInQuestion));
        QCOMPARE(kvm.stand->count(jetKvmsQuestion), 2);
    }

    void aKvmThatWasNotAskedYetIsNotMovedToWhatIsNotItNorWhereItsKeptPasswordMayNotGo()
    {
        OwnWall own;
        const core::testing::NobodyThere stale;
        const core::testing::NobodyThere nobody;
        LocalWall::FromAFile brought;
        brought.kvm = at(u"https://127.0.0.1:%1"_s.arg(stale.port()), u"From a file"_s, "first");
        brought.password = u"secret"_s;
        const QString id = own.wall.takeFromAFile({brought}).constFirst().value();
        const OwnKvm asItWas = kvmOf(own.wall, id);
        const QByteArray keptAsItWas = own.secrets.all.value(LocalWall::secretOf(id));
        KvmEditing editing(own.wall, quick());
        const auto movedTo = [&](const QString& address) {
            KvmEditing::Typed typed = asItIs(own.wall, id);
            typed.address = address;
            return cameOf(editing, id, typed);
        };
        const auto sentenceOf = [](const KvmEditing::Outcome& outcome) {
            return outcome.result.ok() ? QString() : QString::fromStdString(outcome.result.error().message);
        };

        // What answers at the new address shows another certificate than the KVM is known by:
        // it hears nothing, not the three questions either. Typing the password gets over that
        // (it goes there once somebody has compared the two), and the sentence says so.
        const MadeUpKvm another(tls("second"));
        KvmEditing::Outcome outcome = movedTo(another.stand->address());
        QCOMPARE(refusalOf(outcome), std::string("password_needed"));
        QCOMPARE(sentenceOf(outcome),
                 u"This device could not be checked at its new address (Certificate does not match the one saved for "
                 "this device). Type its password to save it there all the same."_s);
        QVERIFY(another.stand->asked.isEmpty());

        // What shows the right certificate and is no KVM hears the three questions and nothing
        // else, and the KVM is not moved there.
        const StandIn page(asAPage, tls("first"));
        outcome = movedTo(page.address());
        QCOMPARE(refusalOf(outcome), std::string("not_a_kvm"));
        QCOMPARE(sentenceOf(outcome),
                 u"Something answers at this address, but it is not a KVM that this program speaks "
                 "to. No password was sent to it."_s);
        QVERIFY(heardOnlyTheQuestions(page));

        // Where nothing answers, that is said.
        outcome = movedTo(u"https://127.0.0.1:%1"_s.arg(nobody.port()));
        QCOMPARE(refusalOf(outcome), std::string("unreachable"));
        QCOMPARE(sentenceOf(outcome),
                 u"Nothing answers at this address. No password is sent there before a KVM has answered."_s);

        // To plain HTTP the kept password does not go, whatever is there: nothing is asked.
        const MadeUpKvm plain;
        outcome = movedTo(plain.stand->address());
        QCOMPARE(refusalOf(outcome), std::string("password_needed"));
        QCOMPARE(sentenceOf(outcome), QString::fromLatin1(nothingToKnowItBy));
        QVERIFY(plain.stand->asked.isEmpty());

        // Through all of that it is the KVM it was, where it was, with what was kept for it.
        QCOMPARE(kvmOf(own.wall, id), asItWas);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(id)), keptAsItWas);
        QVERIFY(!own.wall.wasAskedWhatItIs(id));

        // Nor does the kept password of one that is known by Windows' word go anywhere else.
        LocalWall::FromAFile byWindows;
        byWindows.kvm = at(u"https://127.0.0.1:%1"_s.arg(nobody.port()), u"By Windows"_s);
        byWindows.password = u"secret"_s;
        const QString vouchedFor = own.wall.takeFromAFile({byWindows}).constFirst().value();
        const MadeUpKvm elsewhere(tls("first"));
        KvmEditing::Typed typed = asItIs(own.wall, vouchedFor);
        typed.address = elsewhere.stand->address();
        QCOMPARE(refusalOf(cameOf(editing, vouchedFor, typed)), std::string("password_needed"));
        QVERIFY(elsewhere.stand->asked.isEmpty());

        // And one that was taken without a password is not moved without one: there is none to
        // take along. (It was asked there, with nothing that says who asks.)
        const core::testing::NobodyThere staleToo;
        LocalWall::FromAFile withoutOne;
        withoutOne.kvm = at(u"https://127.0.0.1:%1"_s.arg(staleToo.port()), u"Without a password"_s, "comet");
        const QString bare = own.wall.takeFromAFile({withoutOne}).constFirst().value();
        const MadeUpKvm there(tls("comet"));
        typed = asItIs(own.wall, bare);
        typed.address = there.stand->address();
        outcome = cameOf(editing, bare, typed);
        QCOMPARE(refusalOf(outcome), std::string("no_password"));
        QVERIFY(heardOnlyTheQuestions(*there.stand));
        QVERIFY(kvmOf(own.wall, bare).address != there.stand->address());
    }

    void whatTheWallRecordedByItselfMeanwhileIsNoChangeThatSomebodyMade()
    {
        // A box is opened over a KVM that was not asked yet (the first start of a wall; a KVM
        // that came from a file), and is filled with the KVM as it is. While the box is open,
        // the wall asks what the KVM is, and records that it has.
        OwnWall own;
        const MadeUpKvm kvm(tls("first"));
        LocalWall::FromAFile brought;
        brought.kvm = at(kvm.stand->address(), u"From a file"_s, "first");
        brought.password = u"secret"_s;
        const QString id = own.wall.takeFromAFile({brought}).constFirst().value();
        const KvmEditing::Seen seen{kvmOf(own.wall, id), own.wall.secretWrites(id)};
        QVERIFY(!seen.kvm.askedWhatItIs);
        QCOMPARE(onceItIs(own.wall, id, u"online"_s).state, u"online"_s);
        QVERIFY(kvmOf(own.wall, id).askedWhatItIs);
        // Not the same to the letter as the one the box was filled with.
        QVERIFY(kvmOf(own.wall, id) != seen.kvm);

        // The look at its certificate from that box is no "changed meanwhile".
        KvmCertificateReview review(own.wall, quickProbe());
        QList<base::Result<KvmCertificateReview::Shown>> shown;
        review.askCertificates(
            id, [&shown](const base::Result<KvmCertificateReview::Shown>& result) { shown.append(result); }, seen);
        QTRY_COMPARE_WITH_TIMEOUT(shown.size(), 1, 20000);
        QVERIFY2(shown.constFirst().ok(), shown.constFirst().ok() ? "" : shown.constFirst().error().message.c_str());
        // Nor is the saving from it.
        KvmEditing editing(own.wall, quick());
        KvmEditing::Typed typed = asItIs(own.wall, id);
        typed.name = u"Renamed"_s;
        typed.seen = seen;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("saved"));
        QCOMPARE(kvmOf(own.wall, id).name, u"Renamed"_s);
        // (The one question both ask: with what the wall records by itself left out, and with
        // nothing else left out.)
        OwnKvm askedSince = seen.kvm;
        askedSince.askedWhatItIs = !seen.kvm.askedWhatItIs;
        QVERIFY(LocalWall::isStillTheOne(askedSince, seen.kvm));
        QVERIFY(!LocalWall::isStillTheOne(kvmOf(own.wall, id), seen.kvm));
        // What somebody did is one still: the box was filled before the renaming.
        typed.name = u"Renamed again"_s;
        QCOMPARE(refusalOf(cameOf(editing, id, typed)), std::string("conflict"));

        // And where the wall records it during the box's own asking and sign-in: a KVM that was
        // not asked yet is moved, with its password typed, while the new address holds its
        // discovery answer open. The wall must finish discovery before that answer is released.
        const MadeUpKvm where(tls("second"));
        MadeUpKvm slowThere(tls("second"));
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.streams = true;
        slowThere.first[kvmdsQuestion].append(late);
        // Keep the existing deadline while the test explicitly controls the answer's end.
        KvmEditing::Pace patient = quick();
        patient.probe.quietMs = 6000;
        patient.probe.withinMs = 9000;
        KvmEditing patientBox(own.wall, patient);
        LocalWall::FromAFile second;
        second.kvm = at(where.stand->address(), u"Moving"_s, "second");
        second.password = u"secret"_s;
        const QString moving = own.wall.takeFromAFile({second}).constFirst().value();
        typed = asItIs(own.wall, moving);
        typed.address = slowThere.stand->address();
        typed.password = u"secret"_s;
        QList<KvmEditing::Outcome> told;
        patientBox.change(moving, typed, [&told](const KvmEditing::Outcome& outcome) { told.append(outcome); });
        QTRY_VERIFY_WITH_TIMEOUT(slowThere.stand->isStreaming(), 20000);
        QCOMPARE(onceItIs(own.wall, moving, u"online"_s).state, u"online"_s);
        QVERIFY(kvmOf(own.wall, moving).askedWhatItIs);
        QVERIFY2(told.isEmpty(), "the box had ended before the wall had asked: nothing was tried");
        slowThere.stand->endStream();
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QCOMPARE(refusalOf(told.constFirst()), std::string("saved"));
        QCOMPARE(kvmOf(own.wall, moving).address, slowThere.stand->address());
    }
};

QTEST_GUILESS_MAIN(TestKvmEditing)
#include "test_kvm_editing.moc"
