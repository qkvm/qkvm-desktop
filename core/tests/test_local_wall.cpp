#include <algorithm>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/local_wall.h>

#include "jetkvm_by_hand.h"
#include "jetkvm_viewer_by_hand.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::LocalWall;
using core::OwnKvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::good;
using core::testing::JetKvmByHand;
using core::testing::JetKvmViewerByHand;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;
using core::testing::refusal;
using core::testing::savedFor;
using core::testing::StandIn;

namespace {

// Where a kvmd is signed in to, and the three questions that are asked before that.
constexpr const char* signInQuestion = "/api/auth/login";
constexpr const char* kvmdsQuestion = "/api/auth/check";
constexpr const char* jetKvmsQuestion = "/device/status";
constexpr const char* redfishsQuestion = "/redfish/v1/";

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

// Whether nothing went to an address, or nothing but the three questions of one asking, each on
// a line of its own: what a KVM hears that the wall has no password for. Nothing at all where it
// is kept that the KVM was asked what it is (one over HTTPS); and the three questions, once,
// where it is asked anew in every run (one over plain HTTP).
bool heardNoMoreThanTheQuestions(const StandIn& stand)
{
    return (stand.asked.isEmpty() && stand.lines == 0) || (heardOnlyTheQuestions(stand) && stand.lines == 3);
}

// Whether all that was asked at an address from the `since`-th asking on is the three questions
// of one asking, and nothing in them says who asks.
bool heardOnlyTheQuestionsSince(const StandIn& stand, qsizetype since)
{
    if (stand.asked.size() != since + 3) {
        return false;
    }
    QSet<QByteArray> questions;
    for (qsizetype index = since; index < stand.asked.size(); ++index) {
        const Asked& one = stand.asked.at(index);
        if (one.method != "GET" || !one.body.isEmpty() || one.headers.contains("cookie")) {
            return false;
        }
        questions.insert(one.path);
    }
    return questions == QSet<QByteArray>{kvmdsQuestion, jetKvmsQuestion, redfishsQuestion};
}

// One clock for whatever a test has to put in an order: begun when it is first asked.
qint64 sinceTheTestsBegan()
{
    static const QElapsedTimer clock = [] {
        QElapsedTimer begun;
        begun.start();
        return begun;
    }();
    return clock.elapsed();
}

// Something that takes every call and never says a word: what is switched off, as far as this
// PC's own address can show that. To whoever asks over HTTPS, nothing is let in until the time
// for that is over; over plain HTTP the question goes out, and nothing comes back. It notes when
// it was first called, by the tests' clock, and how many lines were made to it.
class NeverSpeaks : public QObject {
public:
    NeverSpeaks()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* caller = m_server.nextPendingConnection()) {
                if (firstCalledAt < 0) {
                    firstCalledAt = sinceTheTestsBegan();
                }
                calledAt.append(sinceTheTestsBegan());
                ++lines;
                connect(caller, &QTcpSocket::disconnected, caller, &QObject::deleteLater);
            }
        });
    }

    QString address(bool encrypted) const
    {
        return u"%1://127.0.0.1:%2"_s.arg(encrypted ? u"https"_s : u"http"_s).arg(m_server.serverPort());
    }

    // How often it was asked what it is: the lines of one asking come within a moment of each
    // other, and the next asking of the same address a good while later.
    int askings() const
    {
        int count = 0;
        qint64 before = -1'000'000;
        for (const qint64 at : calledAt) {
            count += at - before > 300 ? 1 : 0;
            before = at;
        }
        return count;
    }

    qint64 firstCalledAt = -1; // never: -1
    QList<qint64> calledAt;
    int lines = 0;

private:
    QTcpServer m_server;
};

// What a tile says of a KVM: its state, and under it the code and the sentence that came with
// it, where one did.
QString tileOf(const core::Status& status)
{
    QString says = status.state;
    if (!status.errorCode.isEmpty() || !status.errorMessage.isEmpty()) {
        says += u" ["_s + status.errorCode + u"] "_s + status.errorMessage;
    }
    return says;
}

// The sentences of a KVM that was asked what it is and is away: nothing answers there; and a
// gateway answers there, and the KVM behind it does not.
const QString nothingAnswersThere =
    u"offline [unreachable] Nothing answers at this address. No password is sent there before a KVM has answered."_s;
const QString onlyAGatewayAnswersThere =
    u"offline [unreachable] Only a gateway answers at this address, and it says that what is behind it does not "
    "answer just now. No password was sent."_s;

} // namespace

// This PC's own wall: KVMs that were added in the program, against made-up ones, with no server.
class TestLocalWall : public QObject {
    Q_OBJECT

private:
    static core::KvmdDevice::Pace quick()
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
        // (A power board's: test_kvmd_board has what they hold. Each wall's of its own.)
        pace.pressHeldMs = 100;
        pace.longPressHeldMs = 300;
        pace.boardAnswerMs = 1500;
        pace.seenWithinMs = 600;
        pace.seenEveryMs = 100;
        pace.boards = std::make_shared<core::BoardHolds>();
        return pace;
    }

    // The same, with room for a TLS handshake: on the loaded slow cores 1.5 s found a KVM over HTTPS "offline" (the
    // fix round's item, and this round's whole runs). For the cases over HTTPS that look at who is asked what, not how
    // soon.
    static core::KvmdDevice::Pace overTls()
    {
        core::KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 10'000;
        return pace;
    }

    // Both families' drivers' times, alike the kvmd's: as a wall made with quick() has them.
    static core::DriverPaces quickDrivers() { return core::DriverPaces::alike(quick()); }

    // Whether a follow onto HTTPS moved the KVM (and was not refused as another's).
    static bool movedBy(const base::Result<bool>& came) { return came.ok() && came.value(); }

    // The code something was refused with; "(ok)" where it was not (and the test fails, not stops).
    template <typename T>
    static std::string codeOf(const base::Result<T>& result)
    {
        return result.ok() ? std::string("(ok)") : result.error().code;
    }

    static OwnKvm at(const MadeUpKvm& kvm, const QString& name = {}, const QString& group = {})
    {
        OwnKvm own;
        own.address = kvm.stand->address();
        own.name = name;
        own.group = group;
        return own;
    }
    // One that is reached over HTTPS, and known by a certificate.
    static OwnKvm knownAt(const QString& address, const QByteArray& certificate)
    {
        OwnKvm own;
        own.address = address;
        own.knownByCertificate = true;
        own.certificate = certificate;
        return own;
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
    static core::Wall wallOf(LocalWall& wall)
    {
        return answerTo<core::Wall>([&](auto done) { wall.askWall(std::move(done)); }).value();
    }
    static QHash<QString, core::Status> statusOf(LocalWall& wall)
    {
        return answerTo<QHash<QString, core::Status>>([&](auto done) { wall.askStatus(std::move(done)); }).value();
    }
    // How a KVM is once it has been looked at: asked until it is no longer "connecting".
    static core::Status settled(LocalWall& wall, const QString& id)
    {
        core::Status status;
        static_cast<void>(QTest::qWaitFor(
            [&] {
                status = statusOf(wall).value(id);
                return status.state != u"connecting"_s;
            },
            20000));
        return status;
    }
    static base::Result<core::Done> doneBy(LocalWall& wall, const QString& id, const core::Action& what)
    {
        return answerTo<core::Done>([&](auto done) { wall.act(id, what, std::move(done)); });
    }

    // The next start of the program: other settings, read from the file, and another wall. The
    // store is the one thing that stays. Where the list says the KVM is then, and how it is once
    // it has been looked at; `read`: the file was read, and has the one KVM.
    struct NextStart {
        bool read = false;
        QString address;
        core::Status status;
    };
    static NextStart atTheNextStart(const QString& file, MemorySecrets& secrets, const QString& id)
    {
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        NextStart next;
        next.read = !settings.wasUnreadable() && wall.kvms().size() == 1 && wall.kvms().constFirst().id == id;
        next.address = wall.kvms().value(0).address;
        next.status = settled(wall, id);
        // (What a driver that had a password would have sent has arrived by now.)
        QTest::qWait(150);
        return next;
    }

private slots:
    void initTestCase()
    {
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void aWallOfNobodysIsEmpty()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        QVERIFY(wallOf(wall).machines.isEmpty());
        QVERIFY(statusOf(wall).isEmpty());
        QVERIFY(wall.kvms().isEmpty());
    }

    void aKvmThatIsAddedIsOnTheWall()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        const QSignalSpy changed(&wall, &LocalWall::changed);
        const auto id = wall.add(at(kvm, u"Build server"_s, u"Rack 2"_s), u"secret"_s);
        QVERIFY(id.ok());
        QCOMPARE(changed.count(), 1);
        // An id of the program's own making: eight of 0-9 and a-f.
        QCOMPARE(id.value().size(), 8);
        QVERIFY(QRegularExpression(u"\\A[0-9a-f]{8}\\z"_s).match(id.value()).hasMatch());

        const core::Wall is = wallOf(wall);
        QCOMPARE(is.machines.size(), 1);
        const core::Machine& machine = is.machines.constFirst();
        QCOMPARE(machine.id, id.value());
        QCOMPARE(machine.name, u"Build server"_s);
        QCOMPARE(machine.group, u"Rack 2"_s);
        QCOMPARE(machine.host, u"127.0.0.1:%1"_s.arg(kvm.stand->serverPort()));
        QCOMPARE(machine.kind, u"kvmd"_s);
        QVERIFY(machine.hasPicture);
        // Its machines can be had live, which a wall says by the form it speaks: this program's.
        QCOMPARE(is.liveForm, core::spokenLiveForm);
        // It is signed in to, and is there: said to the first who asks, by the look that asking
        // began. (This looked for "connecting" here first, as an ask said it before its own look
        // had answered.)
        QCOMPARE(statusOf(wall).value(id.value()).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
    }

    void aKvmSpokenToOverPlainHttpIsSaidToBe()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        QVERIFY(wall.add(at(kvm, u"NAS"_s), u"secret"_s).ok());
        // (Nothing answers at the second: how it is spoken to is known all the same.)
        core::OwnKvm encrypted;
        encrypted.address = u"https://127.0.0.1:9"_s;
        encrypted.name = u"Build server"_s;
        QVERIFY(wall.add(encrypted, u"secret"_s).ok());
        const core::Wall is = wallOf(wall);
        QCOMPARE(is.machines.size(), 2);
        QVERIFY(is.machines.at(0).plain);
        QVERIFY(!is.machines.at(1).plain);
    }

    void everyKvmOfTheWallIsSaidToBeThisPcsOwn()
    {
        // What a machine is by itself, as a server's wall lists one: nobody's to change here.
        QVERIFY(!core::Machine{}.own);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        const QString first = wall.add(at(kvm, u"NAS"_s), u"secret"_s).value();
        // (Over HTTPS and in the clear, there or away: whoever sits here can change each.)
        core::OwnKvm away = knownAt(u"https://127.0.0.1:9"_s, QByteArray(32, 'c'));
        away.name = u"Build server"_s;
        QVERIFY(wall.add(away, u"secret"_s).ok());
        core::Wall is = wallOf(wall);
        QCOMPARE(is.machines.size(), 2);
        QVERIFY(std::ranges::all_of(is.machines, [](const core::Machine& machine) { return machine.own; }));
        // And still once one of them was changed, and after the next start.
        core::OwnKvm renamed = wall.kvms().constFirst();
        renamed.name = u"Storage"_s;
        QVERIFY(wall.change(first, renamed, std::nullopt).ok());
        is = wallOf(wall);
        QCOMPARE(is.machines.constFirst().name, u"Storage"_s);
        QVERIFY(std::ranges::all_of(is.machines, [](const core::Machine& machine) { return machine.own; }));
        LocalWall again(settings, secrets, quick());
        is = wallOf(again);
        QCOMPARE(is.machines.size(), 2);
        QVERIFY(std::ranges::all_of(is.machines, [](const core::Machine& machine) { return machine.own; }));
    }

    void aKvmWithoutANameIsCalledByWhereItIs()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        QVERIFY(wall.add(at(kvm), u"secret"_s).ok());
        QCOMPARE(wallOf(wall).machines.constFirst().name, u"127.0.0.1:%1"_s.arg(kvm.stand->serverPort()));
    }

    void thePasswordIsKeptWithTheSecretsAndNowhereElse()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        const auto id = wall.add(at(kvm, u"Build server"_s), u"a password of its own"_s);
        QVERIFY(id.ok());
        // One secret, with what it was saved for, a line each: the mark of the form, the address
        // as it is written, the certificate the KVM is known by (none here, in a word of its own),
        // and the password.
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id.value())),
                 "QKVM-KVM-2\nhttp://127.0.0.1:" + QByteArray::number(kvm.stand->serverPort()) +
                     "\nnone\na password of its own");
        // And of one that is known by a certificate, that certificate by its SHA-256.
        const core::testing::NobodyThere nobody;
        const QString encrypted = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        const auto known = wall.add(knownAt(encrypted, QByteArray(32, 'c')), u"another one"_s);
        QVERIFY(known.ok());
        QCOMPARE(secrets.all.value(LocalWall::secretOf(known.value())),
                 "QKVM-KVM-2\n" + encrypted.toUtf8() + "\nsha256:" + "63"_ba.repeated(32) + "\nanother one");
        QVERIFY(wall.remove(known.value()).ok());
        QCOMPARE(secrets.all.size(), 1);
        // Under a name that says what it is, beside the program's other secrets.
        QCOMPARE(secrets.all.constBegin().key(), u"kvm:"_s + id.value());
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QByteArray text = written.readAll();
        QVERIFY(text.contains("Build server"));
        QVERIFY(!text.contains("a password of its own"));
        QVERIFY(!text.contains("password"));
    }

    void theWallIsThereAgainAtTheNextStart()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const MadeUpKvm kvm;
        QString id;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quick());
            id = wall.add(at(kvm, u"Build server"_s, u"Rack 2"_s), u"secret"_s).value();
        }
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        const core::Wall is = wallOf(wall);
        QCOMPARE(is.machines.size(), 1);
        QCOMPARE(is.machines.constFirst().id, id);
        QCOMPARE(is.machines.constFirst().name, u"Build server"_s);
        // With its password, from where that was kept.
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
    }

    void aKvmThatIsOnTheWallIsNotAddedTwice()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        QVERIFY(wall.add(at(kvm, u"One"_s), u"secret"_s).ok());
        const auto twice = wall.add(at(kvm, u"The same again"_s), u"secret"_s);
        QVERIFY(!twice.ok());
        QCOMPARE(twice.error().code, std::string("duplicate"));
        QCOMPARE(twice.error().message, std::string("This device is already on the wall"));
        QCOMPARE(wall.kvms().size(), 1);
        QCOMPARE(secrets.all.size(), 1);
        // Nor the other way of speaking to it, at the place it is at.
        OwnKvm otherWay = at(kvm, u"Encrypted"_s);
        otherWay.address = u"https://127.0.0.1:%1"_s.arg(kvm.stand->serverPort());
        QCOMPARE(wall.add(otherWay, u"secret"_s).error().code, std::string("duplicate"));
        // At another port of that host there is another KVM.
        otherWay.address = u"https://127.0.0.1:9"_s;
        QVERIFY(wall.add(otherWay, u"secret"_s).ok());
        QCOMPARE(wall.kvms().size(), 2);
        // And one without a port of its own is at one place both ways.
        OwnKvm noPort;
        noPort.address = u"https://192.168.1.20"_s;
        OwnKvm noPortInTheClear;
        noPortInTheClear.address = u"http://192.168.1.20"_s;
        OwnKvm itsNeighbour;
        itsNeighbour.address = u"https://192.168.1.21"_s;
        QVERIFY(LocalWall::atOnePlace(noPort, noPortInTheClear));
        QVERIFY(!LocalWall::atOnePlace(noPort, itsNeighbour));
        QVERIFY(!LocalWall::atOnePlace(noPort, otherWay));
        // Whatever port is written: the one that is spoken to counts.
        const auto at = [](const QString& address) {
            OwnKvm kvm;
            kvm.address = address;
            return kvm;
        };
        QVERIFY(LocalWall::atOnePlace(at(u"https://kvm.example"_s), at(u"http://kvm.example:443"_s)));
        QVERIFY(LocalWall::atOnePlace(at(u"http://kvm.example"_s), at(u"https://kvm.example:80"_s)));
        QVERIFY(LocalWall::atOnePlace(at(u"https://kvm.example:8443"_s), at(u"http://kvm.example:8443"_s)));
        QVERIFY(!LocalWall::atOnePlace(at(u"https://kvm.example"_s), at(u"http://kvm.example:8080"_s)));
        QVERIFY(!LocalWall::atOnePlace(at(u"https://kvm.example:8443"_s), at(u"https://kvm.example:8444"_s)));
        QVERIFY(!LocalWall::atOnePlace(at(u"http://kvm.example:8080"_s), at(u"https://kvm.example:8443"_s)));
    }

    void aKvmIsTakenOffByTheWallsOwnTextForIt()
    {
        // As whoever writes `remove(kvms().first().id)` names it: by a text that is the wall's own,
        // and gone once the list is another.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm one;
        const MadeUpKvm two;
        QVERIFY(wall.add(at(one, u"One"_s), u"secret"_s).ok());
        const QString second = wall.add(at(two, u"Two"_s), u"secret"_s).value();
        QVERIFY(wall.remove(wall.kvms().constFirst().id).ok());
        QCOMPARE(wall.kvms().size(), 1);
        QCOMPARE(wall.kvms().constFirst().id, second);
        QCOMPARE(secrets.all.keys(), QStringList{LocalWall::secretOf(second)});
    }

    void whatCannotBeWrittenDownIsNotAdded()
    {
        // The settings are to go where no file can be: under a file.
        const QTemporaryDir folder;
        QFile inTheWay(folder.filePath(u"in-the-way"_s));
        QVERIFY(inTheWay.open(QIODevice::WriteOnly));
        inTheWay.close();
        core::Settings settings(folder.filePath(u"in-the-way/settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        const QSignalSpy changed(&wall, &LocalWall::changed);
        const auto refused = wall.add(at(kvm, u"Build server"_s), u"secret"_s);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("not_saved"));
        QVERIFY(QString::fromStdString(refused.error().message).startsWith(u"The new device could not be saved ("_s));
        QVERIFY(QString::fromStdString(refused.error().message).endsWith(u"), so nothing was changed."_s));
        // Nothing of it is anywhere: not on the wall, and no password of a KVM that is not there.
        QVERIFY(wall.kvms().isEmpty());
        QVERIFY(wallOf(wall).machines.isEmpty());
        QVERIFY(secrets.all.isEmpty());
        QVERIFY(changed.isEmpty());

        // Nor where its password cannot be kept.
        core::Settings writable(folder.filePath(u"settings.json"_s));
        MemorySecrets refusing;
        refusing.refuses = true;
        LocalWall other(writable, refusing, quick());
        QCOMPARE(other.add(at(kvm), u"secret"_s).error().code, std::string("not_saved"));
        QVERIFY(other.kvms().isEmpty());
    }

    void aKvmThatIsTakenOffIsGoneWithItsPassword()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm one;
        const MadeUpKvm other;
        const QString first = wall.add(at(one, u"One"_s), u"secret"_s).value();
        const QString second = wall.add(at(other, u"Other"_s), u"secret"_s).value();
        QCOMPARE(settled(wall, first).state, u"online"_s);
        const QSignalSpy changed(&wall, &LocalWall::changed);
        QVERIFY(wall.remove(first).ok());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(wallOf(wall).machines.size(), 1);
        QCOMPARE(wallOf(wall).machines.constFirst().id, second);
        QVERIFY(!statusOf(wall).contains(first));
        QVERIFY(!secrets.all.contains(LocalWall::secretOf(first)));
        QVERIFY(secrets.all.contains(LocalWall::secretOf(second)));
        // And at the next start too.
        core::Settings again(file);
        QCOMPARE(again.remembered().kvms.size(), 1);
        // One that is not there is not taken off.
        QCOMPARE(wall.remove(first).error().code, std::string("not_found"));
        QCOMPARE(wall.remove(u"00000000"_s).error().code, std::string("not_found"));
    }

    void eachKvmFailsByItself()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm there;
        const core::testing::NobodyThere nobody;
        OwnKvm away;
        away.address = nobody.address();
        away.name = u"Away"_s;
        const QString up = wall.add(at(there, u"There"_s), u"secret"_s).value();
        const QString down = wall.add(away, u"secret"_s).value();
        const MadeUpKvm refusing;
        const QString wrong = wall.add(at(refusing, u"Wrong password"_s), u"not it"_s).value();

        QCOMPARE(settled(wall, up).state, u"online"_s);
        const core::Status gone = settled(wall, down);
        QCOMPARE(gone.state, u"offline"_s);
        QCOMPARE(gone.errorMessage, u"Device is unreachable"_s);
        QCOMPARE(settled(wall, wrong).state, u"auth"_s);
        // The wall of the others is as ever: a picture of the one that is there, and none of the rest.
        QVERIFY(answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(up, 400, 1, std::move(done)); }).ok());
        QCOMPARE(
            answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(down, 400, 1, std::move(done)); }).error().code,
            std::string("unreachable"));
        QCOMPARE(answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(u"nobody"_s, 400, 1, std::move(done)); })
                     .error()
                     .code,
                 std::string("not_found"));
    }

    void somethingIsDoneToAKvmOfTheWall()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        kvm.atx = true;
        kvm.toWake = {QJsonObject{{u"mac"_s, u"AA:BB:CC:DD:EE:01"_s}, {u"name"_s, u"nas"_s}}};
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        QVERIFY(doneBy(wall, id, core::Action::plain(u"atx_power"_s)).ok());
        QCOMPARE(kvm.did, QList<QByteArray>{"atx power"_ba});
        const auto targets =
            answerTo<QList<core::WakeTarget>>([&](auto done) { wall.askWakeTargets(id, std::move(done)); });
        QCOMPARE(targets.value().size(), 1);
        // To one that is not on the wall, nothing; and a wall for which no action was saved hands
        // none out, and runs none (test_own_actions.cpp has the ones that are saved).
        QCOMPARE(doneBy(wall, u"nobody"_s, core::Action::plain(u"atx_power"_s)).error().code, std::string("not_found"));
        QVERIFY(answerTo<QList<core::SavedAction>>([&](auto done) { wall.askSavedActions(std::move(done)); })
                    .value()
                    .isEmpty());
        QCOMPARE(doneBy(wall, id, core::Action::run(core::SavedAction{})).error().code, std::string("not_found"));
        QCOMPARE(kvm.did.size(), 1);
        // A KVM of a family this build does not speak to is not added: it would be written and not
        // read back.
        core::OwnKvm ofAnother = at(kvm, u"An Intel AMT machine"_s);
        ofAnother.kind = u"amt"_s;
        QCOMPARE(wall.add(ofAnother, u"secret"_s).error().code, std::string("invalid"));
        // What the KVM's driver says may have been done, the wall says too.
        QVERIFY(wall.mayHaveBeenDone(id, base::Error{"dropped", ""}));
        QVERIFY(wall.mayHaveBeenDone(id, base::Error{"action_unknown", ""}));
        QVERIFY(!wall.mayHaveBeenDone(id, base::Error{"unreachable", ""}));
        // Of a KVM that is not on the wall (taken off meanwhile), the word of every family the program
        // speaks to, yes where any of them says yes: the wall's own refusal ("not_found") was before
        // anything went out.
        QVERIFY(wall.mayHaveBeenDone(u"nobody"_s, base::Error{"dropped", ""}));
        QVERIFY(wall.mayHaveBeenDone(u"nobody"_s, base::Error{"action_unknown", ""}));
        QVERIFY(!wall.mayHaveBeenDone(u"nobody"_s, base::Error{"not_found", ""}));
        QVERIFY(!wall.mayHaveBeenDone(u"nobody"_s, base::Error{"unreachable", ""}));
    }

    void noKvmOfTheWallHandsOutAWayIntoItsConsole()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        // Refused; and later, as everything is answered: never from inside the asking.
        QList<base::Result<QUrl>> ways;
        wall.askConsole(id, [&](const base::Result<QUrl>& way) { ways.append(way); });
        QVERIFY(ways.isEmpty());
        QTRY_COMPARE(ways.size(), 1);
        QVERIFY(!ways.constFirst().ok());
        QCOMPARE(ways.constFirst().error().code, std::string("no_console"));
        QCOMPARE(ways.constFirst().error().message,
                 std::string("This device does not hand out a way into its console"));
        // The KVM was asked nothing for it, so nothing of it is left open.
        QVERIFY(kvm.stand->asked.isEmpty());
        QVERIFY(!wall.mayHaveBeenDone(id, ways.constFirst().error()));
    }

    void oneThingAtATimeGoesToAKvm()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        kvm.atx = true;
        MadeUpKvm other;
        other.atx = true;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        const QString beside = wall.add(at(other, u"Beside"_s), u"secret"_s).value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        Answered late = good();
        late.afterMs = 600;
        kvm.first["/api/atx/click"_ba].append(late);

        QList<base::Result<core::Done>> firstDone;
        wall.act(id, core::Action::plain(u"atx_power_long"_s),
                 [&](const base::Result<core::Done>& result) { firstDone.append(result); });
        // A second while the first is out is refused in the server's words: a power button
        // pressed twice undoes the first press. Another KVM is another matter.
        const auto second = doneBy(wall, id, core::Action::plain(u"atx_power"_s));
        QVERIFY(!second.ok());
        QCOMPARE(second.error().code, std::string("busy"));
        QCOMPARE(
            second.error().message,
            std::string("Something else is being done to \"Build server\" at this moment. Wait for it to finish."));
        QVERIFY(doneBy(wall, beside, core::Action::plain(u"atx_power"_s)).ok());
        // Nor is it taken off the wall under what is being done to it.
        QCOMPARE(wall.remove(id).error().code, std::string("busy"));
        QTRY_COMPARE(firstDone.size(), 1);
        QVERIFY(firstDone.constFirst().ok());
        QCOMPARE(kvm.asked("/api/atx/click"), 1);
        // And after it the next.
        QVERIFY(doneBy(wall, id, core::Action::plain(u"atx_power"_s)).ok());
        QVERIFY(wall.remove(id).ok());
    }

    void theWallIsPutInAnOrder()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm one;
        const MadeUpKvm two;
        const MadeUpKvm three;
        const QString a = wall.add(at(one, u"A"_s), u"secret"_s).value();
        const QString b = wall.add(at(two, u"B"_s), u"secret"_s).value();
        const QString c = wall.add(at(three, u"C"_s), u"secret"_s).value();
        const QSignalSpy changed(&wall, &LocalWall::changed);
        // Those named first, as named and each once; one that is nobody's is passed over; the
        // rest after them as they stood.
        const auto order =
            answerTo<QStringList>([&](auto done) { wall.arrange({c, u"nobody"_s, a, c}, std::move(done)); });
        QVERIFY(order.ok());
        QCOMPARE(order.value(), (QStringList{c, a, b}));
        QCOMPARE(changed.count(), 1);
        const core::Wall is = wallOf(wall);
        QCOMPARE(is.machines.at(0).id, c);
        QCOMPARE(is.machines.at(1).id, a);
        QCOMPARE(is.machines.at(2).id, b);
        core::Settings again(file);
        QCOMPARE(again.remembered().kvms.constFirst().id, c);
        // And the KVMs are spoken to as before.
        QCOMPARE(settled(wall, a).state, u"online"_s);
    }

    void whatAKvmThatIsTakenOffStillAnswersReachesNobody()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        // A picture and the computers it can wake are asked for, and it takes its time over both.
        kvm.pictureAfterMs = 300;
        Answered late = good({{u"devices"_s, QJsonArray{QJsonObject{{u"mac"_s, u"AA:BB:CC:DD:EE:01"_s}}}}});
        late.afterMs = 300;
        kvm.first["/api/wol/list"_ba].append(late);
        QList<base::Result<core::Snapshot>> pictures;
        QList<base::Result<QList<core::WakeTarget>>> targets;
        const auto picture = [&pictures](const base::Result<core::Snapshot>& answer) { pictures.append(answer); };
        wall.askSnapshot(id, 400, 1, picture);
        wall.askWakeTargets(
            id, [&targets](const base::Result<QList<core::WakeTarget>>& answer) { targets.append(answer); });
        QTRY_VERIFY(kvm.asked("/api/streamer/snapshot") == 1 && kvm.asked("/api/wol/list") == 1);
        QVERIFY(wall.remove(id).ok());
        // Whoever asked is told by the wall that the KVM is not there: later, and once.
        QVERIFY(pictures.isEmpty() && targets.isEmpty());
        QTRY_VERIFY(pictures.size() == 1 && targets.size() == 1);
        QCOMPARE(pictures.constFirst().error().code, std::string("not_found"));
        QCOMPARE(pictures.constFirst().error().message, std::string("Unknown device"));
        QCOMPARE(targets.constFirst().error().code, std::string("not_found"));
        // And what the KVM answers after that is nobody's.
        QTest::qWait(700);
        QCOMPARE(pictures.size(), 1);
        QCOMPARE(targets.size(), 1);

        // Nor an answer its driver had ready, to be handed over a turn of the loop later. Of a
        // KVM that is away, whoever asks for a picture is told so at once, by its driver.
        const core::testing::NobodyThere nobody;
        OwnKvm away;
        away.address = nobody.address();
        const QString gone = wall.add(away, u"secret"_s).value();
        QCOMPARE(settled(wall, gone).state, u"offline"_s);
        const auto told = answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(gone, 400, 1, std::move(done)); });
        QCOMPARE(told.error().code, std::string("unreachable"));
        // Asked again, and taken off before the loop has turned: the wall's word, not the driver's.
        wall.askSnapshot(gone, 400, 1, picture);
        QVERIFY(wall.remove(gone).ok());
        QTest::qWait(300);
        QCOMPARE(pictures.size(), 2);
        QCOMPARE(pictures.constLast().error().code, std::string("not_found"));
        QVERIFY(secrets.all.isEmpty());
    }

    void aKvmOfTheWallIsChanged()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm before;
        MadeUpKvm kvm;
        const MadeUpKvm after;
        QVERIFY(wall.add(at(before, u"Before"_s), u"secret"_s).ok());
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        QVERIFY(wall.add(at(after, u"After"_s), u"secret"_s).ok());
        QCOMPARE(settled(wall, id).state, u"online"_s);
        const QSignalSpy changed(&wall, &LocalWall::changed);

        // Another name and group: in its place on the wall, under the wall's own name for it,
        // whatever stands in what was handed over.
        OwnKvm as = at(kvm, u"Database"_s, u"Rack 2"_s);
        as.id = u"whatever"_s;
        QVERIFY(wall.change(id, as, std::nullopt).ok());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(wall.kvms().size(), 3);
        QCOMPARE(wall.kvms().at(1).id, id);
        QCOMPARE(wallOf(wall).machines.at(1).name, u"Database"_s);
        QCOMPARE(wallOf(wall).machines.at(1).group, u"Rack 2"_s);
        QCOMPARE(core::Settings(file).remembered().kvms.at(1).name, u"Database"_s);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", kvm.stand->address()));
        QCOMPARE(secrets.keeps, 3);
        // Its driver is the one it was: at its next look it is signed in as before.
        const qsizetype asked = kvm.stand->asked.size();
        QTest::qWait(quick().freshMs + 100);
        QCOMPARE(statusOf(wall).value(id).state, u"online"_s);
        QTRY_VERIFY(kvm.stand->asked.size() > asked);
        QTest::qWait(150);
        QCOMPARE(kvm.signIns, 1);
        // The same once more changes nothing, and nothing is said.
        QVERIFY(wall.change(id, as, std::nullopt).ok());
        QCOMPARE(changed.count(), 1);

        // Another password: kept in the place of the one before, and a new driver signs in with it.
        kvm.password = u"another one"_s;
        QVERIFY(wall.change(id, as, u"another one"_s).ok());
        QCOMPARE(changed.count(), 2);
        QCOMPARE(secrets.all.size(), 3);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("another one", kvm.stand->address()));
        // (Said to the first who asks, by the look of the new driver: this looked for
        // "connecting" here first, as an ask said it before its own look had answered.)
        QCOMPARE(statusOf(wall).value(id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.last("/api/auth/login")->body, "user=admin&passwd=another%20one"_ba);

        // Another address, and no password with it: the one that is kept does not go there,
        // whoever asks the wall. (This KVM is known by nothing, and so known nowhere else.)
        MadeUpKvm moved;
        moved.password = u"another one"_s;
        const OwnKvm stays = wall.kvms().at(1);
        as.address = moved.stand->address();
        const auto refused = wall.change(id, as, std::nullopt);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("password_needed"));
        QCOMPARE(refused.error().message,
                 std::string("There is nothing to know this device by at another address. Type its password to use it "
                             "there."));
        QCOMPARE(changed.count(), 2);
        QCOMPARE(wall.kvms().at(1), stays);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("another one", kvm.stand->address()));
        // Its driver is the one it was, and nothing was asked where it was to go.
        QTest::qWait(quick().freshMs + 100);
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 2);
        QVERIFY(moved.stand->asked.isEmpty());
        QCOMPARE(moved.stand->lines, 0);

        // With its password given for that address: saved for it, and its driver speaks to it there.
        QVERIFY(wall.change(id, as, u"another one"_s).ok());
        QCOMPARE(changed.count(), 3);
        QCOMPARE(wallOf(wall).machines.at(1).host, u"127.0.0.1:%1"_s.arg(moved.stand->serverPort()));
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("another one", moved.stand->address()));
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QCOMPARE(moved.signIns, 1);
        QCOMPARE(core::Settings(file).remembered().kvms.at(1), wall.kvms().at(1));
    }

    void aKvmIsNotChangedIntoAnotherOfTheWall()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm one;
        const MadeUpKvm other;
        const QString id = wall.add(at(one, u"One"_s), u"secret"_s).value();
        QVERIFY(wall.add(at(other, u"Other"_s), u"secret"_s).ok());
        const QList<OwnKvm> before = wall.kvms();
        const QSignalSpy changed(&wall, &LocalWall::changed);
        // Not to where another one is.
        OwnKvm as = before.constFirst();
        as.address = other.stand->address();
        auto refused = wall.change(id, as, u"another one"_s);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("duplicate"));
        QCOMPARE(refused.error().message, std::string("This device is already on the wall"));
        // And one that is not on the wall is not changed, nor put there by it.
        refused = wall.change(u"00000000"_s, as, std::nullopt);
        QCOMPARE(refused.error().code, std::string("not_found"));
        QCOMPARE(refused.error().message, std::string("Unknown device"));
        QCOMPARE(wall.kvms(), before);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", one.stand->address()));
        QCOMPARE(secrets.all.size(), 2);
        QVERIFY(changed.isEmpty());
    }

    void whatCannotBeWrittenDownIsNotChanged()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"kept/settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        const core::testing::NobodyThere nobody;
        OwnKvm encrypted;
        encrypted.address = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        const QString other = wall.add(encrypted, u"secret"_s).value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        const QList<OwnKvm> before = wall.kvms();
        const QSignalSpy changed(&wall, &LocalWall::changed);
        OwnKvm as = before.constFirst();
        as.name = u"Renamed"_s;
        const auto notSaved = [](const base::Result<void>& refused, const QString& what) {
            const QString sentence = refused.ok() ? QString() : QString::fromStdString(refused.error().message);
            return !refused.ok() && refused.error().code == "not_saved" &&
                   sentence.startsWith(what + u" could not be saved ("_s) &&
                   sentence.endsWith(u"), so nothing was changed."_s);
        };
        // Its password cannot be kept: then its name is not saved either.
        const QByteArray kept = savedFor("secret", kvm.stand->address());
        secrets.refuses = true;
        QVERIFY(notSaved(wall.change(id, as, u"another one"_s), u"The change to this device"_s));
        QCOMPARE(wall.kvms(), before);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), kept);
        secrets.refuses = false;
        // The settings cannot be written (where their folder was, a file is): then the password
        // that was kept is the kept one again.
        QVERIFY(QDir(folder.filePath(u"kept"_s)).removeRecursively());
        QFile inTheWay(folder.filePath(u"kept"_s));
        QVERIFY(inTheWay.open(QIODevice::WriteOnly));
        inTheWay.close();
        QVERIFY(notSaved(wall.change(id, as, u"another one"_s), u"The change to this device"_s));
        QCOMPARE(wall.kvms(), before);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), kept);
        // Nor a certificate, which is said in its own words: and the password is the one that was
        // saved for the KVM as it was.
        QVERIFY(notSaved(wall.knowBy(other, QByteArray(32, 'c')), u"Trusting this certificate"_s));
        QCOMPARE(wall.kvms(), before);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(other)), savedFor("secret", encrypted.address));
        // And a KVM that had no password kept has none again.
        secrets.all.remove(LocalWall::secretOf(id));
        QVERIFY(notSaved(wall.change(id, as, u"another one"_s), u"The change to this device"_s));
        QVERIFY(!secrets.all.contains(LocalWall::secretOf(id)));
        QVERIFY(changed.isEmpty());
        // Its driver is the one it was, signed in as it was.
        QTest::qWait(quick().freshMs + 100);
        QCOMPARE(statusOf(wall).value(id).state, u"online"_s);
        QTest::qWait(300);
        QCOMPARE(kvm.signIns, 1);
    }

    void aKvmsDriverIsNotReplacedUnderWhatIsBeingDoneToIt()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        kvm.atx = true;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        Answered late = good();
        late.afterMs = 600;
        kvm.first["/api/atx/click"_ba].append(late);
        QList<base::Result<core::Done>> done;
        wall.act(id, core::Action::plain(u"atx_power_long"_s),
                 [&](const base::Result<core::Done>& result) { done.append(result); });
        // What its driver is made of is not changed meanwhile: whoever pressed the button would
        // never hear how that ended.
        OwnKvm as = wall.kvms().constFirst();
        auto refused = wall.change(id, as, u"another one"_s);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("busy"));
        QCOMPARE(
            refused.error().message,
            std::string("Something else is being done to \"Build server\" at this moment. Wait for it to finish."));
        as.user = u"root"_s;
        QCOMPARE(wall.change(id, as, std::nullopt).error().code, std::string("busy"));
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", kvm.stand->address()));
        QCOMPARE(wall.kvms().constFirst().user, u"admin"_s);
        // A new name is none of that, and is taken at any time.
        as = wall.kvms().constFirst();
        as.name = u"Renamed"_s;
        QVERIFY(wall.change(id, as, std::nullopt).ok());
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done.constFirst().ok());
        // And after it the rest.
        QVERIFY(wall.change(id, as, u"secret"_s).ok());
        QCOMPARE(settled(wall, id).state, u"online"_s);
    }

    void whoeverWaitsForAPictureIsToldToAskAgainWhenTheDriverIsReplaced()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        kvm.pictureAfterMs = 300;
        QList<base::Result<core::Snapshot>> pictures;
        wall.askSnapshot(id, 400, 1, [&](const base::Result<core::Snapshot>& answer) { pictures.append(answer); });
        QTRY_COMPARE(kvm.asked("/api/streamer/snapshot"), 1);
        // The password is typed anew: another driver. A card that never heard of its picture
        // again would never ask for the next one.
        QVERIFY(wall.change(id, wall.kvms().constFirst(), u"secret"_s).ok());
        QVERIFY(pictures.isEmpty());
        QTRY_COMPARE(pictures.size(), 1);
        QCOMPARE(pictures.constFirst().error().code, std::string("no_stream"));
        QCOMPARE(pictures.constFirst().error().message, std::string("Connecting to the KVM"));
        // What the driver before still brings is nobody's; the next picture is the new one's.
        QTest::qWait(600);
        QCOMPARE(pictures.size(), 1);
        kvm.pictureAfterMs = 0;
        QVERIFY(QTest::qWaitFor(
            [&] {
                return answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(id, 400, 1, std::move(done)); }).ok();
            },
            20000));
        QCOMPARE(kvm.signIns, 2);
    }

    void aCertificateIsRememberedForAKvmThatHasOne()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        // (Nobody is at either address, and neither is ever asked: nothing looks at this wall.)
        const core::testing::NobodyThere nobody;
        const core::testing::NobodyThere nobodyEither;
        OwnKvm encrypted;
        encrypted.address = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        OwnKvm plain;
        plain.address = nobodyEither.address();
        const QString id = wall.add(encrypted, u"secret"_s).value();
        const QString inTheClear = wall.add(plain, u"secret"_s).value();
        const QSignalSpy changed(&wall, &LocalWall::changed);
        const QByteArray sha256(32, 'c');
        QVERIFY(wall.knowBy(id, sha256).ok());
        QCOMPARE(changed.count(), 1);
        QVERIFY(wall.kvms().constFirst().knownByCertificate);
        QCOMPARE(wall.kvms().constFirst().certificate, sha256);
        QCOMPARE(core::Settings(file).remembered().kvms.constFirst(), wall.kvms().constFirst());
        // The same again changes nothing.
        QVERIFY(wall.knowBy(id, sha256).ok());
        QCOMPARE(changed.count(), 1);

        // The plain way shows none, so there is none to know a KVM by.
        auto refused = wall.knowBy(inTheClear, sha256);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("no_certificate"));
        QCOMPARE(refused.error().message,
                 std::string("This device is reached over plain HTTP, where there is no certificate"));
        QVERIFY(!wall.kvms().constLast().knownByCertificate);
        // What is no certificate's SHA-256 is not remembered in the place of one.
        for (const QByteArray& none : {QByteArray(), QByteArray(31, 'c'), QByteArray(33, 'c')}) {
            refused = wall.knowBy(id, none);
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().code, std::string("invalid"));
            QCOMPARE(refused.error().message, std::string("This is not the SHA-256 of a certificate"));
        }
        QCOMPARE(wall.kvms().constFirst().certificate, sha256);
        QCOMPARE(wall.knowBy(u"00000000"_s, sha256).error().code, std::string("not_found"));
        QCOMPARE(changed.count(), 1);
    }

    void theKeptPasswordIsSignedInWithOnlyWhereTheKvmIsKnown()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        const MadeUpKvm elsewhere;
        const core::testing::NobodyThere nobody;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        const auto identified = [](core::OneSignIn& signer) {
            return answerTo<core::OneSignIn::Said>([&](auto done) { signer.identify(std::move(done)); });
        };
        const QString sentence =
            u"There is nothing to know this device by at another address. Type its password to use it there."_s;
        const auto needsThePassword = [&sentence](const base::Result<std::unique_ptr<core::OneSignIn>>& refused) {
            return !refused.ok() && refused.error().code == "password_needed" &&
                   QString::fromStdString(refused.error().message) == sentence;
        };

        // At the address it has, under another user name: a driver of its own, which signs in
        // with the password that is kept.
        auto signer = wall.signerIn(id, QUrl(kvm.stand->address()), u"root"_s, quickDrivers());
        QVERIFY(signer.ok());
        QCOMPARE(identified(*signer.value()).error().code, std::string("auth_failed"));
        QCOMPARE(kvm.last("/api/auth/login")->body, "user=root&passwd=secret"_ba);
        signer = wall.signerIn(id, QUrl(kvm.stand->address()), u"admin"_s, quickDrivers());
        QVERIFY(signer.ok());
        QVERIFY(identified(*signer.value()).ok());

        // A KVM that is known by nothing is known nowhere else: to another address the kept
        // password does not go, the plain way or over HTTPS.
        const QUrl encrypted(u"https://127.0.0.1:%1"_s.arg(nobody.port()));
        QVERIFY(needsThePassword(wall.signerIn(id, QUrl(elsewhere.stand->address()), u"admin"_s, quickDrivers())));
        QVERIFY(needsThePassword(wall.signerIn(id, encrypted, u"admin"_s, quickDrivers())));

        // One that is known by a certificate is known by it over HTTPS, and not the plain way.
        OwnKvm known;
        known.address = u"https://127.0.0.1:%1"_s.arg(elsewhere.stand->serverPort());
        known.knownByCertificate = true;
        known.certificate = QByteArray(32, 'c');
        const QString byCertificate = wall.add(known, u"secret"_s).value();
        QVERIFY(needsThePassword(
            wall.signerIn(byCertificate, QUrl(elsewhere.stand->address()), u"admin"_s, quickDrivers())));
        signer = wall.signerIn(byCertificate, encrypted, u"root"_s, quickDrivers());
        QVERIFY(signer.ok());
        // And there it is the KVM only by that certificate.
        QCOMPARE(signer.value()->where().address, encrypted);
        QCOMPARE(signer.value()->where().user, u"root"_s);
        QVERIFY(signer.value()->where().knownByCertificate);
        QCOMPARE(signer.value()->where().certificate, QByteArray(32, 'c'));
        // One that Windows vouches for at its address is known by nothing of its own.
        OwnKvm vouchedFor;
        vouchedFor.address = u"https://kvm.invalid"_s;
        const QString byWindows = wall.add(vouchedFor, u"secret"_s).value();
        QVERIFY(needsThePassword(wall.signerIn(byWindows, encrypted, u"admin"_s, quickDrivers())));
        // And a KVM that is not on the wall has no password kept.
        QCOMPARE(wall.signerIn(u"00000000"_s, encrypted, u"admin"_s, quickDrivers()).error().code,
                 std::string("not_found"));
        QVERIFY(elsewhere.stand->asked.isEmpty());
        QCOMPARE(elsewhere.stand->lines, 0);
    }

    void withoutItsPasswordAKvmIsChangedOnlyToWhereTheKeptOneMayGo()
    {
        // The rule of signerIn() is the wall's own: whoever changes a KVM and gives no password is
        // refused wherever the kept one may not go. (Nobody is at any of these addresses, and
        // nothing looks at this wall.)
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const core::testing::NobodyThere one;
        const core::testing::NobodyThere two;
        const core::testing::NobodyThere three;
        const auto encryptedAt = [](const core::testing::NobodyThere& nobody) {
            return u"https://127.0.0.1:%1"_s.arg(nobody.port());
        };
        const QByteArray first(32, 'c');
        const QByteArray second(32, 'd');
        const QString id = wall.add(knownAt(encryptedAt(one), first), u"secret"_s).value();
        const OwnKvm before = wall.kvms().constFirst();
        const QByteArray kept = savedFor("secret", encryptedAt(one), first);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), kept);
        const QSignalSpy changed(&wall, &LocalWall::changed);
        const std::string sentence =
            "There is nothing to know this device by at another address. Type its password to use it there.";
        const auto needsThePassword = [&](const QString& which, const OwnKvm& as) {
            const auto refused = wall.change(which, as, std::nullopt);
            return !refused.ok() && refused.error().code == "password_needed" && refused.error().message == sentence;
        };

        // Not the plain way, at another address.
        OwnKvm as = before;
        as.address = two.address();
        as.knownByCertificate = false;
        as.certificate.clear();
        QVERIFY(needsThePassword(id, as));
        // Not over HTTPS at another address as what shows another certificate, or one that
        // Windows vouches for.
        as = before;
        as.address = encryptedAt(two);
        as.certificate = second;
        QVERIFY(needsThePassword(id, as));
        as.knownByCertificate = false;
        as.certificate.clear();
        QVERIFY(needsThePassword(id, as));
        // And not at its own address under another certificate, or under none.
        as = before;
        as.certificate = second;
        QVERIFY(needsThePassword(id, as));
        as.knownByCertificate = false;
        as.certificate.clear();
        QVERIFY(needsThePassword(id, as));
        QCOMPARE(wall.kvms().constFirst(), before);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), kept);
        QVERIFY(changed.isEmpty());

        // Over HTTPS at another address, as what shows the certificate it is known by: there the
        // kept one goes, and is saved anew for that address.
        as = before;
        as.address = encryptedAt(two);
        QVERIFY(wall.change(id, as, std::nullopt).ok());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(wall.kvms().constFirst().address, encryptedAt(two));
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", encryptedAt(two), first));
        // Another user name, where it is, is none of this.
        as.user = u"root"_s;
        QVERIFY(wall.change(id, as, std::nullopt).ok());
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", encryptedAt(two), first));
        // The one exception: a certificate somebody was shown and said yes to.
        QVERIFY(wall.knowBy(id, second).ok());
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", encryptedAt(two), second));

        // One that Windows vouches for is known by nothing of its own, and so known nowhere else.
        OwnKvm vouchedFor;
        vouchedFor.address = encryptedAt(three);
        const QString byWindows = wall.add(vouchedFor, u"secret"_s).value();
        OwnKvm other = wall.kvms().constLast();
        other.address = encryptedAt(one);
        QVERIFY(needsThePassword(byWindows, other));
        QCOMPARE(secrets.all.value(LocalWall::secretOf(byWindows)), savedFor("secret", encryptedAt(three)));
        // With its password given, any of these is taken: whoever gives one has signed in with it there.
        QVERIFY(wall.change(byWindows, other, u"secret"_s).ok());
        QCOMPARE(secrets.all.value(LocalWall::secretOf(byWindows)), savedFor("secret", encryptedAt(one)));
    }

    void aKvmOnTheWallOverHttpsIsNotChangedToThePlainWayAtItsPlace()
    {
        // (Nobody is at these addresses, and nothing looks at this wall.)
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const core::testing::NobodyThere nobody;
        const core::testing::NobodyThere nobodyEither;
        const QString encrypted = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        const QString id = wall.add(knownAt(encrypted, QByteArray(32, 'c')), u"secret"_s).value();
        const OwnKvm before = wall.kvms().constFirst();
        const QSignalSpy changed(&wall, &LocalWall::changed);
        OwnKvm as = before;
        as.address = nobody.address();
        as.knownByCertificate = false;
        as.certificate.clear();
        // With its password or without: its password would go a second way, unencrypted.
        for (const std::optional<QString>& password : {std::optional<QString>(), std::optional<QString>(u"secret"_s)}) {
            const auto refused = wall.change(id, as, password);
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().code, std::string("not_in_the_clear"));
            QCOMPARE(refused.error().message,
                     std::string("This device is on the wall over HTTPS. It is not changed to plain HTTP at the same "
                                 "address, where its password would travel unencrypted. To use it that way, remove "
                                 "it from the wall and add it again."));
        }
        QCOMPARE(wall.kvms().constFirst(), before);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", encrypted, QByteArray(32, 'c')));
        QVERIFY(changed.isEmpty());

        // The other way round is a change like any other, with its password.
        OwnKvm plain;
        plain.address = nobodyEither.address();
        const QString inTheClear = wall.add(plain, u"secret"_s).value();
        as = wall.kvms().constLast();
        as.address = u"https://127.0.0.1:%1"_s.arg(nobodyEither.port());
        as.knownByCertificate = true;
        as.certificate = QByteArray(32, 'd');
        QVERIFY(wall.change(inTheClear, as, u"secret"_s).ok());
        QCOMPARE(wall.kvms().constLast().address, as.address);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(inTheClear)),
                 savedFor("secret", as.address, QByteArray(32, 'd')));
    }

    void everyWritingOfAKvmsSecretIsCounted()
    {
        // What a box that changes a KVM goes by, to see that its password is another than it was
        // when the box read it: the list does not tell. (Nobody is at these addresses, and nothing
        // looks at this wall.)
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const core::testing::NobodyThere one;
        const core::testing::NobodyThere two;
        const core::testing::NobodyThere three;
        const QString id =
            wall.add(knownAt(u"https://127.0.0.1:%1"_s.arg(one.port()), QByteArray(32, 'c')), u"secret"_s).value();
        OwnKvm plain;
        plain.address = two.address();
        const QString other = wall.add(plain, u"secret"_s).value();
        const quint64 before = wall.secretWrites(id);
        const quint64 beside = wall.secretWrites(other);
        // A name, a group, a user name: nothing of its secret is written.
        OwnKvm as = wall.kvms().constFirst();
        as.name = u"Renamed"_s;
        as.group = u"Rack 2"_s;
        as.user = u"root"_s;
        QVERIFY(wall.change(id, as, std::nullopt).ok());
        QCOMPARE(wall.secretWrites(id), before);
        // A password that is given, be it the one that was kept.
        QVERIFY(wall.change(id, as, u"secret"_s).ok());
        QCOMPARE(wall.secretWrites(id), before + 1);
        // The kept one saved anew: for another address, and for another certificate.
        as.address = u"https://127.0.0.1:%1"_s.arg(three.port());
        QVERIFY(wall.change(id, as, std::nullopt).ok());
        QCOMPARE(wall.secretWrites(id), before + 2);
        QVERIFY(wall.knowBy(id, QByteArray(32, 'd')).ok());
        QCOMPARE(wall.secretWrites(id), before + 3);
        // One that could not be kept is no writing: what is kept is what it was. (This had it
        // counted all the same, as "begun to be written": turned round. A box that still held
        // what it was opened with was told "changed meanwhile" for it at its next saving.)
        secrets.refuses = true;
        QCOMPARE(wall.change(id, wall.kvms().constFirst(), u"another one"_s).error().code, std::string("not_saved"));
        QCOMPARE(wall.secretWrites(id), before + 3);
        secrets.refuses = false;
        // What is refused before that counts nothing.
        as = wall.kvms().constFirst();
        as.address = u"https://127.0.0.1:%1"_s.arg(one.port());
        as.certificate = QByteArray(32, 'e');
        QCOMPARE(wall.change(id, as, std::nullopt).error().code, std::string("password_needed"));
        QCOMPARE(wall.secretWrites(id), before + 3);
        // Each KVM by itself; and one that is not on the wall has had none.
        QCOMPARE(wall.secretWrites(other), beside);
        QCOMPARE(wall.secretWrites(u"00000000"_s), quint64{0});
    }

    void aWritingOfAKvmsSecretIsCountedWhenWhatIsKeptIsAnotherAndNotBefore()
    {
        // (Nobody is at this address, and nothing looks at this wall.)
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const core::testing::NobodyThere nobody;
        OwnKvm plain;
        plain.address = nobody.address();
        const QString id = wall.add(plain, u"secret"_s).value();
        const QString name = LocalWall::secretOf(id);
        const OwnKvm known = wall.kvms().constFirst();
        const QByteArray keptBefore = secrets.all.value(name);
        const quint64 before = wall.secretWrites(id);

        // The store does not take the new password: nothing was written.
        secrets.refuses = true;
        QCOMPARE(wall.change(id, known, u"another one"_s).error().code, std::string("not_saved"));
        secrets.refuses = false;
        QCOMPARE(secrets.all.value(name), keptBefore);
        QCOMPARE(wall.secretWrites(id), before);

        // The store takes it, the list cannot be written, and the password before is put back:
        // what is kept is what it was.
        {
            QFile held(file);
            QVERIFY(held.open(QIODevice::ReadOnly));
            QCOMPARE(wall.change(id, known, u"another one"_s).error().code, std::string("not_saved"));
            QCOMPARE(secrets.all.value(name), keptBefore);
            QCOMPARE(wall.secretWrites(id), before);

            // The password before cannot be put back either: what is kept is another now, and
            // whoever read the KVM before that sees it by the count.
            secrets.refusesTheKeep = secrets.keeps + 2;
            QCOMPARE(wall.change(id, known, u"a third one"_s).error().code, std::string("not_saved"));
            QVERIFY(secrets.all.value(name) != keptBefore);
            QCOMPARE(wall.secretWrites(id), before + 1);
        }
        // And one that is saved is counted, once.
        QVERIFY(wall.change(id, known, u"another one"_s).ok());
        QCOMPARE(wall.secretWrites(id), before + 2);
    }

    void whereNoPasswordIsKeptNoneGoesAlongToAnotherAddress()
    {
        // The kept password goes with a KVM to its other address. Where none is kept for the KVM
        // as it is (none at all, or none that Windows could read back; one that was saved for
        // something else; one of no form), there is none to take along, and the KVM is not moved
        // without one: nothing is changed, and what the store holds is left as it is. (Nobody is
        // at these addresses, and nothing looks at this wall.)
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const core::testing::NobodyThere one;
        const core::testing::NobodyThere two;
        const QString here = u"https://127.0.0.1:%1"_s.arg(one.port());
        const QString there = u"https://127.0.0.1:%1"_s.arg(two.port());
        const QByteArray first(32, 'c');
        const QByteArray second(32, 'd');
        const QString id = wall.add(knownAt(here, first), u"secret"_s).value();
        const QString name = LocalWall::secretOf(id);
        const OwnKvm before = wall.kvms().constFirst();
        const QSignalSpy changed(&wall, &LocalWall::changed);
        OwnKvm as = before;
        as.address = there;
        for (const QByteArray& held : {QByteArray(), savedFor("secret", there, first), "secret"_ba}) {
            secrets.all.insert(name, held);
            const int keeps = secrets.keeps;
            const auto refused = wall.change(id, as, std::nullopt);
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().code, std::string("no_password"));
            QCOMPARE(refused.error().message, std::string("No password saved for this device"));
            QCOMPARE(wall.kvms().constFirst(), before);
            QCOMPARE(secrets.all.value(name), held);
            QCOMPARE(secrets.keeps, keeps);
        }
        QVERIFY(changed.isEmpty());
        // Another user name where it is takes nothing along, and is taken.
        as = before;
        as.user = u"root"_s;
        QVERIFY(wall.change(id, as, std::nullopt).ok());
        // And a certificate somebody said yes to is remembered all the same: the KVM has no
        // password under that one either, and none is written for it.
        const int keeps = secrets.keeps;
        QVERIFY(wall.knowBy(id, second).ok());
        QCOMPARE(wall.kvms().constFirst().certificate, second);
        QCOMPARE(secrets.keeps, keeps);
        QCOMPARE(secrets.all.value(name), "secret"_ba);
        // With its password given, it goes where it is told.
        as = wall.kvms().constFirst();
        as.address = there;
        QVERIFY(wall.change(id, as, u"secret"_s).ok());
        QCOMPARE(secrets.all.value(name), savedFor("secret", there, second));
    }

    void aSecretThatWasNotSavedForTheKvmAsItIsIsNotUsed_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<QByteArray>("what");
        QTest::newRow("saved for another address") << false << "another address"_ba;
        QTest::newRow("saved for the same place over HTTPS") << false << "the other way"_ba;
        QTest::newRow("saved for a certificate where the KVM is known by none") << false << "a certificate"_ba;
        QTest::newRow("saved for another certificate") << true << "another certificate"_ba;
        QTest::newRow("saved for no certificate where the KVM is known by one") << true << "no certificate"_ba;
        QTest::newRow("saved for the address in other letters") << false << "other letters"_ba;
        QTest::newRow("the password and nothing else") << false << "bare"_ba;
        QTest::newRow("of a form that is not this one") << false << "another form"_ba;
        // The form before this one said "known by none" and "known by a certificate that is empty"
        // in one and the same line. Nothing of it is read.
        QTest::newRow("of the form before, saved for no certificate") << false << "the form before"_ba;
        QTest::newRow("of the form before, saved for its certificate") << true << "the form before, known"_ba;
        QTest::newRow("saved for no certificate, said as the form before said it") << false << "an empty line"_ba;
        QTest::newRow("saved for a certificate that is empty") << false << "an empty certificate"_ba;
        QTest::newRow("saved for its certificate without the word before it") << true << "bare hex"_ba;
        QTest::newRow("saved for its certificate in capitals") << true << "capitals"_ba;
        QTest::newRow("with its lines ended another way") << false << "other line ends"_ba;
        QTest::newRow("cut off before the password") << false << "cut off"_ba;
        QTest::newRow("with no password in it") << false << "no password"_ba;
        QTest::newRow("empty") << false << "empty"_ba;
        QTest::newRow("none at all") << false << "none"_ba;
    }

    void aSecretThatWasNotSavedForTheKvmAsItIsIsNotUsed()
    {
        QFETCH(bool, encrypted);
        QFETCH(QByteArray, what);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const MadeUpKvm kvm(encrypted ? std::optional(tls("first")) : std::nullopt);
        const QString address = kvm.stand->address();
        const QByteArray certificate = encrypted ? sha256Of("first") : QByteArray();
        QString id;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quick());
            id = wall.add(encrypted ? knownAt(address, certificate) : at(kvm), u"secret"_s).value();
        }
        const QString name = LocalWall::secretOf(id);
        QCOMPARE(secrets.all.value(name), savedFor("secret", address, certificate));

        // What the store holds for it when the program starts the next time.
        const QByteArray written = address.toUtf8();
        const QHash<QByteArray, QByteArray> held{
            {"another address"_ba, savedFor("secret", u"http://127.0.0.1:9"_s)},
            {"the other way"_ba, savedFor("secret", u"https://127.0.0.1:%1"_s.arg(kvm.stand->serverPort()))},
            {"a certificate"_ba, savedFor("secret", address, QByteArray(32, 'c'))},
            {"another certificate"_ba, savedFor("secret", address, sha256Of("second"))},
            {"no certificate"_ba, savedFor("secret", address)},
            {"other letters"_ba, savedFor("secret", address.toUpper())},
            {"bare"_ba, "secret"_ba},
            {"another form"_ba, "QKVM-KVM-3\n" + written + "\nnone\nsecret"},
            {"the form before"_ba, "QKVM-KVM-1\n" + written + "\n\nsecret"},
            {"the form before, known"_ba, "QKVM-KVM-1\n" + written + "\n" + certificate.toHex() + "\nsecret"},
            {"an empty line"_ba, "QKVM-KVM-2\n" + written + "\n\nsecret"},
            {"an empty certificate"_ba, "QKVM-KVM-2\n" + written + "\nsha256:\nsecret"},
            {"bare hex"_ba, "QKVM-KVM-2\n" + written + "\n" + certificate.toHex() + "\nsecret"},
            {"capitals"_ba, "QKVM-KVM-2\n" + written + "\nsha256:" + certificate.toHex().toUpper() + "\nsecret"},
            {"other line ends"_ba, "QKVM-KVM-2\r\n" + written + "\r\nnone\r\nsecret"},
            {"cut off"_ba, "QKVM-KVM-2\n" + written + "\n"},
            {"no password"_ba, savedFor("", address)},
            {"empty"_ba, QByteArray()},
        };
        if (what == "none") {
            secrets.all.remove(name);
        } else {
            QVERIFY(held.contains(what));
            secrets.all.insert(name, held.value(what));
        }
        const QByteArray left = secrets.all.value(name);

        // That KVM has no password: its card says so, and asks for one. Nothing is guessed, and
        // nothing goes out: not by the wall's own driver, and not by one that signs in for a change.
        // (Nothing but what a KVM over plain HTTP is asked at every start before anything else:
        // what it is, with nothing in the question that says who asks.)
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        QCOMPARE(wall.kvms().size(), 1);
        // (The wall's mark says that one was kept: what the store holds was put there behind the
        // wall's back. Whoever lists the KVMs is told what its driver finds, below.)
        QVERIFY(wall.marksAPasswordFor(id));
        const core::Status status = settled(wall, id);
        QCOMPARE(status.errorCode, u"no_password"_s);
        QCOMPARE(status.errorMessage, u"No password saved for this device"_s);
        QCOMPARE(status.state, u"auth"_s);
        QCOMPARE(wall.foundWithoutAPassword(), QSet<QString>{id});
        auto signer = wall.signerIn(id, QUrl(address), u"admin"_s, quickDrivers());
        QVERIFY(signer.ok());
        const auto who =
            answerTo<core::OneSignIn::Said>([&signer](auto done) { signer.value()->identify(std::move(done)); });
        QCOMPARE(who.error().code, std::string("no_password"));
        QTest::qWait(150);
        QVERIFY(heardNoMoreThanTheQuestions(*kvm.stand));
        QCOMPARE(kvm.signIns, 0);
        // What the store holds is left as it is by all of that.
        QCOMPARE(secrets.all.value(name), left);

        // Its password given anew is saved for the KVM as it is, and signed in with.
        QVERIFY(wall.change(id, wall.kvms().constFirst(), u"secret"_s).ok());
        QCOMPARE(secrets.all.value(name), savedFor("secret", address, certificate));
        QVERIFY(wall.marksAPasswordFor(id));
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(wall.foundWithoutAPassword().isEmpty());
    }

    void theWallMarksWithTheListAtEveryKeepingAndForgettingWhetherAPasswordIsKept()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const MadeUpKvm byHand;
        const MadeUpKvm givenEmpty;
        const MadeUpKvm without;
        const MadeUpKvm with;
        // What the file says of a KVM's mark: as the next start reads it.
        const auto markedInTheFile = [&file](const QString& id) {
            return core::Settings(file).remembered().kvmPasswordsKept.contains(id);
        };
        QString added;
        QString addedEmpty;
        QList<base::Result<QString>> fromAFile;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quick());
            const auto kvmOf = [&wall](const QString& id) {
                const auto found = std::ranges::find_if(wall.kvms(), [&id](const OwnKvm& one) { return one.id == id; });
                return found == wall.kvms().end() ? OwnKvm{} : *found;
            };
            // Added with a password: marked, in the writing of the list. Added with one that is
            // empty: that is none, and it is not.
            added = wall.add(at(byHand, u"By hand"_s), u"secret"_s).value();
            QVERIFY(wall.marksAPasswordFor(added));
            QVERIFY(markedInTheFile(added));
            addedEmpty = wall.add(at(givenEmpty, u"Given empty"_s), QString()).value();
            QVERIFY(!wall.marksAPasswordFor(addedEmpty));
            QVERIFY(!markedInTheFile(addedEmpty));
            // Taken from a file, not asked yet what they are: as the file had a password or not.
            LocalWall::FromAFile one;
            one.kvm = at(without);
            LocalWall::FromAFile other;
            other.kvm = at(with);
            other.password = u"from the file"_s;
            fromAFile = wall.takeFromAFile({one, other});
            QCOMPARE(fromAFile.size(), 2);
            QVERIFY(fromAFile.at(0).ok() && fromAFile.at(1).ok());
            QVERIFY(!wall.marksAPasswordFor(fromAFile.at(0).value()));
            QVERIFY(!markedInTheFile(fromAFile.at(0).value()));
            QVERIFY(wall.marksAPasswordFor(fromAFile.at(1).value()));
            QVERIFY(markedInTheFile(fromAFile.at(1).value()));
            // A change that gives one a password marks it; a new name leaves the mark as it is.
            QVERIFY(wall.change(addedEmpty, kvmOf(addedEmpty), u"given"_s).ok());
            QVERIFY(wall.marksAPasswordFor(addedEmpty));
            QVERIFY(markedInTheFile(addedEmpty));
            OwnKvm renamed = kvmOf(added);
            renamed.name = u"By hand, renamed"_s;
            QVERIFY(wall.change(added, renamed, std::nullopt).ok());
            QVERIFY(wall.marksAPasswordFor(added));
            QVERIFY(markedInTheFile(added));
            // A change that gives one that is empty: none is kept for it from then on.
            QVERIFY(wall.change(added, kvmOf(added), QString()).ok());
            QVERIFY(!wall.marksAPasswordFor(added));
            QVERIFY(!markedInTheFile(added));
            QVERIFY(wall.change(added, kvmOf(added), u"secret"_s).ok());
            QVERIFY(markedInTheFile(added));
            // Taken off the wall: its mark goes with it, in the same writing.
            QVERIFY(wall.remove(fromAFile.at(1).value()).ok());
            QVERIFY(!wall.marksAPasswordFor(fromAFile.at(1).value()));
            QVERIFY(!core::Settings(file).remembered().kvmPasswordsKept.contains(fromAFile.at(1).value()));
            QCOMPARE(core::Settings(file).remembered().kvmPasswordsKept, (QSet<QString>{added, addedEmpty}));
        }
        const QHash<QString, QByteArray> heldBefore = secrets.all;

        // The next start: the marks as they were written. Nothing was sent to any KVM for them,
        // nor is anything for the asking of one: the one from the file was not asked what it is.
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        QVERIFY(wall.marksAPasswordFor(added));
        QVERIFY(wall.marksAPasswordFor(addedEmpty));
        QVERIFY(!wall.marksAPasswordFor(fromAFile.at(0).value()));
        // One that is not on the wall has none.
        QVERIFY(!wall.marksAPasswordFor(u"00000000"_s));
        QVERIFY(!wall.wasAskedWhatItIs(fromAFile.at(0).value()));
        QTest::qWait(150);
        QVERIFY(without.stand->asked.isEmpty() && without.stand->lines == 0);
        QCOMPARE(secrets.all, heldBefore);
        // (And what the store holds says nothing of a mark: one that was put there behind the
        // wall's back, or taken out, changes no mark. What the KVM's driver finds is told apart:
        // aSecretThatWasNotSavedForTheKvmAsItIsIsNotUsed.)
        secrets.all.remove(LocalWall::secretOf(added));
        secrets.all.insert(LocalWall::secretOf(fromAFile.at(0).value()), savedFor("secret", without.stand->address()));
        QVERIFY(wall.marksAPasswordFor(added));
        QVERIFY(!wall.marksAPasswordFor(fromAFile.at(0).value()));
    }

    void aYesToAnotherCertificateMarksWhatComesOfThePasswordThatIsKept()
    {
        // KVMs over HTTPS, known by a certificate, at an address where nothing answers: one with
        // a password, one without, and one whose password Windows lost.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const core::testing::NobodyThere first;
        const core::testing::NobodyThere second;
        const core::testing::NobodyThere third;
        const auto encrypted = [](const core::testing::NobodyThere& nobody) {
            return u"https://127.0.0.1:%1"_s.arg(nobody.port());
        };
        const QString with = wall.add(knownAt(encrypted(first), QByteArray(32, 'a')), u"secret"_s).value();
        const QString without = wall.add(knownAt(encrypted(second), QByteArray(32, 'b')), QString()).value();
        const QString lost = wall.add(knownAt(encrypted(third), QByteArray(32, 'c')), u"secret"_s).value();
        secrets.all.remove(LocalWall::secretOf(lost));
        // The kept one goes along, saved anew for the new certificate: marked still.
        QVERIFY(wall.knowBy(with, QByteArray(32, 'd')).ok());
        QCOMPARE(secrets.all.value(LocalWall::secretOf(with)),
                 savedFor("secret", encrypted(first), QByteArray(32, 'd')));
        QVERIFY(wall.marksAPasswordFor(with));
        // None was kept: none is.
        QVERIFY(wall.knowBy(without, QByteArray(32, 'e')).ok());
        QVERIFY(!wall.marksAPasswordFor(without));
        // The mark said one was kept, and the store holds none for the KVM as it was: under the new
        // certificate none goes along, and the mark says so from then on.
        QVERIFY(wall.marksAPasswordFor(lost));
        QVERIFY(wall.knowBy(lost, QByteArray(32, 'f')).ok());
        QVERIFY(!wall.marksAPasswordFor(lost));
        QCOMPARE(settings.remembered().kvmPasswordsKept, QSet<QString>{with});
    }

    void whereTheListCannotBeWrittenTheMarksAndTheStoreAreWhatTheyWere()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const MadeUpKvm with;
        const MadeUpKvm without;
        const MadeUpKvm another;
        const MadeUpKvm fromAFile;
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        const QString kept = wall.add(at(with, u"With"_s), u"secret"_s).value();
        const QString none = wall.add(at(without, u"Without"_s), QString()).value();
        const QSet<QString> marks{kept};
        const QHash<QString, QByteArray> held = secrets.all;
        QCOMPARE(settings.remembered().kvmPasswordsKept, marks);
        const auto asTheyWere = [&] {
            return settings.remembered().kvmPasswordsKept == marks && secrets.all == held &&
                   core::Settings(file).remembered().kvmPasswordsKept == marks;
        };
        {
            // Something else has the file open: Windows puts no other in its place meanwhile.
            QFile inTheWay(file);
            QVERIFY(inTheWay.open(QIODevice::ReadOnly));
            // A KVM added with a password.
            QCOMPARE(wall.add(at(another), u"secret"_s).error().code, std::string("not_saved"));
            QVERIFY(asTheyWere());
            // A password given to the one without, and an empty one to the one with.
            QCOMPARE(wall.change(none, wall.kvms().at(1), u"given"_s).error().code, std::string("not_saved"));
            QVERIFY(asTheyWere());
            QCOMPARE(wall.change(kept, wall.kvms().at(0), QString()).error().code, std::string("not_saved"));
            QVERIFY(asTheyWere());
            // Taken off.
            QCOMPARE(wall.remove(kept).error().code, std::string("not_saved"));
            QVERIFY(asTheyWere());
            // Taken from a file with a password.
            LocalWall::FromAFile one;
            one.kvm = at(fromAFile);
            one.password = u"from the file"_s;
            const QList<base::Result<QString>> came = wall.takeFromAFile({one});
            QCOMPARE(came.size(), 1);
            QCOMPARE(came.constFirst().error().code, std::string("not_saved"));
            QVERIFY(asTheyWere());
        }
        QCOMPARE(wall.kvms().size(), 2);
        // Once it can be written, each is marked as it is.
        QVERIFY(wall.change(none, wall.kvms().at(1), u"given"_s).ok());
        QCOMPARE(core::Settings(file).remembered().kvmPasswordsKept, (QSet<QString>{kept, none}));
    }

    void aPasswordIsReadBackWholeWhateverIsInIt_data()
    {
        QTest::addColumn<QString>("password");
        QTest::newRow("a line's end in it") << u"two\nlines"_s;
        QTest::newRow("a line's end before it") << u"\nsecret"_s;
        QTest::newRow("a line's end after it") << u"secret\n"_s;
        QTest::newRow("a line ended the other way") << u"two\r\nlines"_s;
        QTest::newRow("nothing but ends of lines") << u"\n\n\n"_s;
        QTest::newRow("what a secret begins with") << u"QKVM-KVM-2\nhttp://127.0.0.1:9\nnone\nanother one"_s;
        QTest::newRow("what a secret of the form before begins with")
            << u"QKVM-KVM-1\nhttp://127.0.0.1:9\n\nanother one"_s;
        // The sign that says in which order a text's bytes stand, which draws nothing: where it is
        // the first of a text, Qt's usual reading of bytes takes it for no part of the text and
        // drops it. (Made from its number: no source holds a sign nobody can see.)
        QTest::newRow("a password that begins with U+FEFF") << QString(QChar(0xFEFF)) + u"secret"_s;
        QTest::newRow("U+FEFF and nothing else") << QString(QChar(0xFEFF));
        QTest::newRow("U+FEFF twice, and a line's end between") << QString(QChar(0xFEFF)) + u'\n' + QChar(0xFEFF);
        QTest::newRow("blanks around it") << u"  secret\t "_s;
        QTest::newRow("signs of every kind")
            << u"p+a s&s=w%rd# "_s + core::testing::sign(0xE4) + core::testing::sign(0x1F600);
    }

    void aPasswordIsReadBackWholeWhateverIsInIt()
    {
        // A password may hold anything, a line's end too: nothing in the box that takes one says
        // otherwise. It is the last of the secret, and all that follows the third line's end.
        QFETCH(QString, password);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        MadeUpKvm kvm;
        kvm.password = password;
        QString id;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quick());
            id = wall.add(at(kvm, u"Build server"_s), password).value();
        }
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor(password.toUtf8(), kvm.stand->address()));
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(kvm.last("/api/auth/login")->body, "user=admin&passwd=" + QUrl::toPercentEncoding(password));
    }

    void aKvmIsKeptOnlyUnderAnAddressAsTheProgramWritesOne_data()
    {
        QTest::addColumn<QString>("address");
        QTest::newRow("a line's end in it, and what a secret's lines look like behind")
            << u"http://127.0.0.1:9/\nnone\nnot the password"_s;
        QTest::newRow("a path") << u"http://127.0.0.1:9/kvm"_s;
        QTest::newRow("capitals in the name") << u"https://KVM.example"_s;
        QTest::newRow("the port it has anyway") << u"https://kvm.example:443"_s;
        QTest::newRow("somebody's name before it") << u"https://admin@kvm.example"_s;
        QTest::newRow("no way of speaking") << u"kvm.example"_s;
        QTest::newRow("another way of speaking") << u"ftp://kvm.example"_s;
        QTest::newRow("nothing") << QString();
    }

    void aKvmIsKeptOnlyUnderAnAddressAsTheProgramWritesOne()
    {
        // The settings read back only an address as the program writes one (kvm::addressFrom): a
        // KVM under any other would be on the wall until the program ends, and its password in the
        // store without it after that. Whoever adds or changes a KVM has made its address so; the
        // wall is asked directly here, and holds the rule itself.
        QFETCH(QString, address);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        OwnKvm odd;
        odd.address = address;
        odd.user = u"admin"_s;
        QVERIFY(!wall.add(odd, u"secret"_s).ok());
        QVERIFY(wall.kvms().isEmpty());
        // Nothing was kept for it, not for a moment.
        QCOMPARE(secrets.keeps, 0);

        // Nor is one that is on the wall changed to such an address, with its password or without.
        const MadeUpKvm kvm;
        const auto id = wall.add(at(kvm, u"Build server"_s), u"secret"_s);
        QVERIFY(id.ok());
        const QList<OwnKvm> before = wall.kvms();
        const QHash<QString, QByteArray> keptBefore = secrets.all;
        OwnKvm moved = before.constFirst();
        moved.address = address;
        QVERIFY(!wall.change(id.value(), moved, u"secret"_s).ok());
        QVERIFY(!wall.change(id.value(), moved, std::nullopt).ok());
        QCOMPARE(wall.kvms(), before);
        QCOMPARE(secrets.all, keptBefore);
        QCOMPARE(secrets.keeps, 1);
    }

    void bytesThatAreNoTextAreNoPassword_data()
    {
        QTest::addColumn<QByteArray>("behindTheLines");
        // (Each made from its numbers: none of these is a sign anybody could type.)
        QTest::newRow("a byte that begins no sign") << QByteArray::fromHex("ff");
        QTest::newRow("a password with such a byte in it") << "sec" + QByteArray::fromHex("ff") + "ret";
        QTest::newRow("a sign of two bytes, cut after the first") << "secret" + QByteArray::fromHex("c3");
        QTest::newRow("a sign of three bytes, cut after the second") << QByteArray::fromHex("e282") + "secret";
        QTest::newRow("a byte that only follows, with nothing before it") << QByteArray::fromHex("80") + "secret";
        QTest::newRow("a sign written longer than it is") << QByteArray::fromHex("c0af") + "secret";
        QTest::newRow("half of a pair, which no text holds by itself") << QByteArray::fromHex("eda080") + "secret";
        QTest::newRow("a text of sixteen bits to a sign") << QByteArray::fromHex("fffe730065006300");
    }

    void bytesThatAreNoTextAreNoPassword()
    {
        // What stands behind the three lines of a KVM's secret is a password this program wrote
        // only where it reads as text. Bytes that do not (the store was written by something
        // else, or is damaged) are not made into a password by putting a sign in the place of
        // each: that one would be sent to the KVM, and count against its limit of wrong ones.
        QFETCH(QByteArray, behindTheLines);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const MadeUpKvm kvm;
        const QString address = kvm.stand->address();
        QString id;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quick());
            id = wall.add(at(kvm), u"secret"_s).value();
        }
        const QString name = LocalWall::secretOf(id);
        const QByteArray held = savedFor(behindTheLines, address);
        secrets.all.insert(name, held);

        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        const core::Status status = settled(wall, id);
        QCOMPARE(status.errorCode, u"no_password"_s);
        QCOMPARE(status.errorMessage, u"No password saved for this device"_s);
        QCOMPARE(status.state, u"auth"_s);
        auto signer = wall.signerIn(id, QUrl(address), u"admin"_s, quickDrivers());
        QVERIFY(signer.ok());
        const auto who =
            answerTo<core::OneSignIn::Said>([&signer](auto done) { signer.value()->identify(std::move(done)); });
        QCOMPARE(who.error().code, std::string("no_password"));
        // Nothing went to the KVM but the question what it is, which a KVM over plain HTTP is
        // asked at every start; and what the store holds is left as it is.
        QTest::qWait(150);
        QVERIFY(heardNoMoreThanTheQuestions(*kvm.stand));
        QCOMPARE(kvm.signIns, 0);
        QCOMPARE(secrets.all.value(name), held);
    }

    void whereThePasswordBeforeCannotBePutBackTheOneThatStaysGoesNowhereElse()
    {
        // The list cannot be written, and putting the password before back fails too. The store
        // then holds a password that was given for another address than the list names: at the
        // next start it is not sent to the address the list still has. (This had "the next start"
        // made with the settings that were in memory, whose file was gone by then. Now it is what
        // the program does: other settings, read from the file, and another wall.)
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const MadeUpKvm kvm;
        const MadeUpKvm elsewhere;
        QString id;
        OwnKvm as;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quick());
            id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
            QCOMPARE(settled(wall, id).state, u"online"_s);
            const QList<OwnKvm> before = wall.kvms();
            // Something else has the file open: Windows puts no other in its place meanwhile, and
            // what is in it stays what it was.
            QFile held(file);
            QVERIFY(held.open(QIODevice::ReadOnly));
            as = before.constFirst();
            as.address = elsewhere.stand->address();
            // The new one is kept, and the one before is not taken back.
            const int keeps = secrets.keeps;
            secrets.refusesTheKeep = keeps + 2;
            const auto refused = wall.change(id, as, u"given for elsewhere"_s);
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().code, std::string("not_saved"));
            QCOMPARE(secrets.keeps, keeps + 2);
            QCOMPARE(wall.kvms(), before);
        }
        const QString name = LocalWall::secretOf(id);
        const QByteArray stays = savedFor("given for elsewhere", elsewhere.stand->address());
        QCOMPARE(secrets.all.value(name), stays);
        // What the KVM at the address the list has was asked up to here, and on how many lines.
        // (Counted once the made-up KVM has seen all of it: a line the wall before had made in
        // its last moment is noted there when the loop next turns.)
        QTest::qWait(300);
        const qsizetype asked = kvm.stand->asked.size();
        const int lines = kvm.stand->lines;
        // At the next start, with the list as the file still has it: that KVM has no password, and
        // hears nothing of one: only what a KVM over plain HTTP is asked at every start, what it
        // is, on three lines. Nor does the one the password was given for hear anything.
        NextStart next = atTheNextStart(file, secrets, id);
        QVERIFY(next.read);
        QCOMPARE(next.address, kvm.stand->address());
        QCOMPARE(next.status.errorCode, u"no_password"_s);
        QVERIFY(heardOnlyTheQuestionsSince(*kvm.stand, asked));
        QCOMPARE(kvm.stand->lines, lines + 3);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(elsewhere.stand->asked.isEmpty());
        QCOMPARE(elsewhere.stand->lines, 0);
        QCOMPARE(secrets.all.value(name), stays);

        // The same where the KVM had no password kept, and the new one cannot be taken out of the
        // store again.
        {
            secrets.all.remove(name);
            secrets.forgetsNothing = true;
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quick());
            QFile held(file);
            QVERIFY(held.open(QIODevice::ReadOnly));
            QCOMPARE(wall.change(id, as, u"given for elsewhere"_s).error().code, std::string("not_saved"));
            QCOMPARE(wall.kvms().constFirst().address, kvm.stand->address());
        }
        QCOMPARE(secrets.all.value(name), stays);
        next = atTheNextStart(file, secrets, id);
        QVERIFY(next.read);
        QCOMPARE(next.address, kvm.stand->address());
        QCOMPARE(next.status.errorCode, u"no_password"_s);
        // (Asked what it is once more at this start, and nothing else.)
        QVERIFY(heardOnlyTheQuestionsSince(*kvm.stand, asked + 3));
        QCOMPARE(kvm.stand->lines, lines + 6);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(elsewhere.stand->asked.isEmpty());
        QCOMPARE(elsewhere.stand->lines, 0);
        QCOMPARE(secrets.all.value(name), stays);
    }

    void whereTheProgramEndsBetweenTheKeepingOfAPasswordAndTheWritingOfTheListThePasswordIsLost_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<QByteArray>("what"); // what the password in the store was saved for
        QTest::newRow("on its way to another address") << false << "another address"_ba;
        QTest::newRow("on its way to another address, both known by one certificate") << true << "another address"_ba;
        QTest::newRow("on its way under another certificate") << true << "another certificate"_ba;
    }

    void whereTheProgramEndsBetweenTheKeepingOfAPasswordAndTheWritingOfTheListThePasswordIsLost()
    {
        // A change keeps the password first and writes the list after it. A program that ends
        // between the two (the PC loses its power) leaves the new secret in the store and the old
        // list in the file. At the next start the password is lost: it was given for something
        // else than the list names, and what the list names never hears it. (The store is put
        // into that state by hand: nothing in a wall ends the program half way through.)
        QFETCH(bool, encrypted);
        QFETCH(QByteArray, what);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const std::optional<core::testing::Tls> own = encrypted ? std::optional(tls("first")) : std::nullopt;
        const MadeUpKvm kvm(own);
        const MadeUpKvm elsewhere(own);
        const QString address = kvm.stand->address();
        const QByteArray certificate = encrypted ? sha256Of("first") : QByteArray();
        const core::KvmdDevice::Pace pace = encrypted ? overTls() : quick();
        QString id;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, pace);
            id = wall.add(encrypted ? knownAt(address, certificate) : at(kvm), u"secret"_s).value();
            QCOMPARE(settled(wall, id).state, u"online"_s);
        }
        const QString name = LocalWall::secretOf(id);
        QCOMPARE(secrets.all.value(name), savedFor("secret", address, certificate));
        // What that KVM was asked up to here, and on how many lines. (Counted once the made-up
        // KVM has seen all of it: a line the wall before had made in its last moment is noted
        // there when the loop next turns.)
        QTest::qWait(300);
        const qsizetype asked = kvm.stand->asked.size();
        const int lines = kvm.stand->lines;
        const int signIns = kvm.signIns;
        // The keeping was done, and the writing never came.
        const QByteArray stays = what == "another address"
                                     ? savedFor("given for elsewhere", elsewhere.stand->address(), certificate)
                                     : savedFor("given for elsewhere", address, sha256Of("second"));
        secrets.all.insert(name, stays);

        const NextStart next = atTheNextStart(file, secrets, id);
        QVERIFY(next.read);
        QCOMPARE(next.address, address);
        QCOMPARE(next.status.errorCode, u"no_password"_s);
        QCOMPARE(next.status.state, u"auth"_s);
        // What the KVM at the address the list names heard of it: nothing at all, on no line,
        // where it is kept that it was asked what it is (over HTTPS); and where it is asked that
        // at every start (over plain HTTP), that question and nothing else.
        if (encrypted) {
            QCOMPARE(kvm.stand->asked.size(), asked);
            QCOMPARE(kvm.stand->lines, lines);
        } else {
            QVERIFY(heardOnlyTheQuestionsSince(*kvm.stand, asked));
            QCOMPARE(kvm.stand->lines, lines + 3);
        }
        QCOMPARE(kvm.signIns, signIns);
        QVERIFY(elsewhere.stand->asked.isEmpty());
        QCOMPARE(elsewhere.stand->lines, 0);
        // Nothing is made of what is in the store, and nothing is taken out of it.
        QCOMPARE(secrets.all.value(name), stays);
        // Typed anew for the KVM as the list has it, it is its password.
        core::Settings settings(file);
        LocalWall wall(settings, secrets, pace);
        QVERIFY(wall.change(id, wall.kvms().constFirst(), u"secret"_s).ok());
        QCOMPARE(secrets.all.value(name), savedFor("secret", address, certificate));
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QCOMPARE(kvm.signIns, signIns + 1);
        QVERIFY(elsewhere.stand->asked.isEmpty());
    }

    void aKvmIsNotKnownByTheCertificateAnotherKvmOfTheWallIsKnownBy()
    {
        // Two KVMs of the wall change places: their addresses were handed out anew. The card of
        // the one says that the certificate does not match, and what answers there shows the
        // other's. Said yes to, the one's kept password would go to the other, a card named for
        // the one would press the other's buttons, and two entries would hold one KVM, each with
        // its own "one thing at a time". The wall is the last that is asked, and refuses.
        // (Nobody is at these addresses, and nothing looks at this wall.)
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const core::testing::NobodyThere one;
        const core::testing::NobodyThere two;
        const core::testing::NobodyThere three;
        const auto encryptedAt = [](const core::testing::NobodyThere& nobody) {
            return u"https://127.0.0.1:%1"_s.arg(nobody.port());
        };
        const QByteArray first(32, 'c');
        const QByteArray second(32, 'd');
        OwnKvm build = knownAt(encryptedAt(one), first);
        build.name = u"Build server"_s;
        const QString twin = wall.add(build, u"secret"_s).value();
        const QString id = wall.add(knownAt(encryptedAt(two), second), u"another one"_s).value();
        OwnKvm vouchedFor;
        vouchedFor.address = encryptedAt(three);
        const QString byWindows = wall.add(vouchedFor, u"a third"_s).value();
        const QList<OwnKvm> before = wall.kvms();
        const QHash<QString, QByteArray> kept = secrets.all;
        const int keeps = secrets.keeps;
        const QSignalSpy changed(&wall, &LocalWall::changed);
        for (const QString& which : {id, byWindows}) {
            const auto refused = wall.knowBy(which, first);
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().code, std::string("same_device"));
            QCOMPARE(QString::fromStdString(refused.error().message),
                     u"This is the same device as \"Build server\", which is on the wall already (127.0.0.1:%1)"_s.arg(
                         one.port()));
        }
        QCOMPARE(wall.kvms(), before);
        QCOMPARE(secrets.all, kept);
        QCOMPARE(secrets.keeps, keeps);
        QCOMPARE(wall.secretWrites(id), quint64{0});
        QVERIFY(changed.isEmpty());

        // Its own certificate is nobody else's, and neither is one that no KVM is known by.
        QVERIFY(wall.knowBy(twin, first).ok());
        QVERIFY(changed.isEmpty());
        QVERIFY(wall.knowBy(id, QByteArray(32, 'e')).ok());
        QCOMPARE(changed.count(), 1);
        // And once the other is off the wall, nobody is known by its certificate.
        QVERIFY(wall.remove(twin).ok());
        QVERIFY(wall.knowBy(id, first).ok());
        QCOMPARE(wall.kvms().constFirst().certificate, first);
    }

    void aKvmThatSaysItIsKnownByACertificateThatIsNoneHasNoPassword_data()
    {
        QTest::addColumn<QByteArray>("certificate");
        QTest::newRow("an empty one") << QByteArray();
        QTest::newRow("one that was cut short") << QByteArray(31, 'c');
        QTest::newRow("one that is too long") << QByteArray(33, 'c');
    }

    void aKvmThatSaysItIsKnownByACertificateThatIsNoneHasNoPassword()
    {
        // A list that was written by hand, or damaged in one place, can say of a KVM that it is
        // known by a certificate and name none that is one. No secret is that KVM's password,
        // whatever it says it was saved for: the wall itself gives its driver none, and does not
        // lean on the driver to send nothing. "Known by a certificate that is empty" is not
        // "known by none" either: what was saved for the one is not the other's.
        QFETCH(QByteArray, certificate);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const MadeUpKvm kvm(tls("first"));
        const QString address = kvm.stand->address();
        OwnKvm own = knownAt(address, certificate);
        own.id = u"0123abcd"_s;
        own.name = u"Build server"_s;
        {
            core::Settings settings(file);
            core::Remembered remembered;
            remembered.kvms = {own};
            QVERIFY(settings.remember(remembered).ok());
        }
        const QString name = LocalWall::secretOf(own.id);
        // What the store may hold for it. Saved for what stands in the list, in the form before
        // this one and in this one; and saved for the address and no certificate, in both.
        const QByteArray written = address.toUtf8();
        const QList<QByteArray> held{
            "QKVM-KVM-1\n" + written + "\n" + certificate.toHex() + "\nsecret",
            "QKVM-KVM-1\n" + written + "\n\nsecret",
            "QKVM-KVM-2\n" + written + "\nsha256:" + certificate.toHex() + "\nsecret",
            savedFor("secret", address, certificate),
            savedFor("secret", address),
        };
        for (const QByteArray& one : held) {
            secrets.all.insert(name, one);
            core::Settings settings(file);
            QCOMPARE(settings.remembered().kvms, QList<OwnKvm>{own});
            LocalWall wall(settings, secrets, quick());
            const core::Status status = settled(wall, own.id);
            QCOMPARE(status.errorCode, u"no_password"_s);
            QCOMPARE(status.errorMessage, u"No password saved for this device"_s);
            QCOMPARE(status.state, u"auth"_s);
            auto signer = wall.signerIn(own.id, QUrl(address), u"admin"_s, quickDrivers());
            QVERIFY(signer.ok());
            const auto who =
                answerTo<core::OneSignIn::Said>([&signer](auto done) { signer.value()->identify(std::move(done)); });
            QCOMPARE(who.error().code, std::string("no_password"));
            // A password that is given for it is not kept either: none could ever go out under
            // what is no certificate. Said as its driver says it, with what to do about it.
            const int keeps = secrets.keeps;
            const QSignalSpy changed(&wall, &LocalWall::changed);
            const auto refused = wall.change(own.id, wall.kvms().constFirst(), u"secret"_s);
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().code, std::string("cert_invalid"));
            QCOMPARE(refused.error().message,
                     std::string("The certificate saved for this device cannot be read. Look at its certificate "
                                 "and trust it again."));
            QCOMPARE(secrets.keeps, keeps);
            QCOMPARE(secrets.all.value(name), one);
            QVERIFY(changed.isEmpty());
            // A new name is none of that.
            OwnKvm renamed = wall.kvms().constFirst();
            renamed.name = u"Renamed"_s;
            QVERIFY(wall.change(own.id, renamed, std::nullopt).ok());
            QVERIFY(wall.change(own.id, own, std::nullopt).ok());
            QCOMPARE(secrets.keeps, keeps);
            QTest::qWait(150);
            QVERIFY(kvm.stand->asked.isEmpty());
            QCOMPARE(kvm.stand->lines, 0);
        }
        // Nor is one put on the wall that way.
        {
            core::Settings settings(folder.filePath(u"other.json"_s));
            LocalWall wall(settings, secrets, quick());
            const int keeps = secrets.keeps;
            QCOMPARE(wall.add(knownAt(address, certificate), u"secret"_s).error().code, std::string("cert_invalid"));
            QVERIFY(wall.kvms().isEmpty());
            QCOMPARE(secrets.keeps, keeps);
        }

        // Once somebody has looked at what it shows and said yes, it is known by that: and its
        // password, which was never its own under what was no certificate, is typed anew.
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        QVERIFY(wall.knowBy(own.id, sha256Of("first")).ok());
        QCOMPARE(settled(wall, own.id).errorCode, u"no_password"_s);
        // (The yes was to a certificate. What shows it was asked what it is, under that
        // certificate, before anything else could go to it: and nothing else did.)
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
        QVERIFY(wall.change(own.id, wall.kvms().constFirst(), u"secret"_s).ok());
        QCOMPARE(secrets.all.value(name), savedFor("secret", address, sha256Of("first")));
        QCOMPARE(settled(wall, own.id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
    }

    void whoeverAsksHowTheKvmsAreIsToldWhatTheLookThatBeganFound()
    {
        // The first asking of a wall begins the look at each of its KVMs, and waits a moment for
        // it. Answered at once, it said "connecting" of every one, with no size and no signal, and
        // what the look had found came with the asking after it, seconds later.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        kvm.width = 2560;
        kvm.height = 1440;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        // Later, as everything is answered: never from inside the asking.
        QList<QHash<QString, core::Status>> told;
        const auto note = [&told](const base::Result<QHash<QString, core::Status>>& all) {
            told.append(all.ok() ? all.value() : QHash<QString, core::Status>{});
        };
        wall.askStatus(note);
        QVERIFY(told.isEmpty());
        QTRY_COMPARE(told.size(), 1);
        const core::Status status = told.constFirst().value(id);
        QCOMPARE(status.state, u"online"_s);
        QCOMPARE(status.signal, std::optional<bool>(true));
        QCOMPARE(status.width, 2560);
        QCOMPARE(status.height, 1440);
        QCOMPARE(kvm.asked("/api/streamer"), 1);
        // While that is fresh, an asking begins no look and waits for none: but is answered later
        // all the same.
        wall.askStatus(note);
        QVERIFY2(told.size() == 1, "answered from inside the asking");
        QTRY_COMPARE(told.size(), 2);
        QCOMPARE(told.constLast().value(id).width, 2560);
        QCOMPARE(kvm.asked("/api/streamer"), 1);
        // And a wall of nobody's is answered too.
        core::Settings none(folder.filePath(u"none.json"_s));
        LocalWall empty(none, secrets, quick());
        empty.askStatus(note);
        QTRY_COMPARE(told.size(), 3);
        QVERIFY(told.constLast().isEmpty());
    }

    void aKvmWhoseVideoRestsIsKnownOnItsCardSoonAfterItsPicturesCome()
    {
        // As seen on a real KVM of one maker: it says nothing of its video while that rests, so
        // the first look finds no size, and whether the computer sends a picture is not known.
        // Its pictures came, and its card had no size ten seconds later. Here with the wall's
        // pace and the driver's own times, each a quarter.
        const int wallsPaceMs = 4000 / 4;
        const core::KvmdDevice::Pace usual;
        core::KvmdDevice::Pace pace = quick();
        pace.freshMs = usual.freshMs / 4;
        pace.lookPatienceMs = usual.lookPatienceMs / 4;
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, pace);
        MadeUpKvm kvm;
        kvm.restsUntilWatched = true;
        kvm.saysNothingOfAVideoThatRests = true;
        kvm.width = 2560;
        kvm.height = 1440;
        kvm.picture = core::testing::jpeg(480, 270); // (as small as a card is sent one)
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        const auto pictureOf = [&wall, &id] {
            return answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(id, 400, 1, std::move(done)); });
        };
        // The first look: it is there, and nothing is known of its video. (Waited for: its first look signs in, which
        // took longer than an asking waits on the loaded slow cores; this round's whole runs found it "connecting".)
        core::Status status = settled(wall, id);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.signal.has_value());
        QCOMPARE(status.width, 0);
        // Its card asks for a picture: the video is woken, and the picture comes.
        QCOMPARE(pictureOf().error().code, std::string("no_stream"));
        QTRY_VERIFY(kvm.stand->live.isConnected());
        QTest::qWait(pace.holdMs / 2 + 50);
        QVERIFY(pictureOf().ok());
        // That the computer sends one is known from there on, to whoever asks.
        QCOMPARE(statusOf(wall).value(id).signal, std::optional<bool>(true));
        // And its size by the wall's next asking: the picture was one the KVM made smaller,
        // which says nothing of that, and the KVM's state says it now that its video is awake.
        QTest::qWait(wallsPaceMs);
        status = statusOf(wall).value(id);
        QCOMPARE(status.signal, std::optional<bool>(true));
        QCOMPARE(status.width, 2560);
        QCOMPARE(status.height, 1440);
    }

    void anAskingWaitsOnlyAMomentForAKvmThatIsSlow()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::KvmdDevice::Pace pace = quick();
        pace.lookPatienceMs = 400;
        LocalWall wall(settings, secrets, pace);
        const MadeUpKvm there;
        MadeUpKvm slow;
        // (Slow by far more than the asking waits, on loaded cores too.)
        Answered late = good({{u"token"_s, u"pass9"_s}});
        late.afterMs = 3000;
        slow.passes.insert("pass9"_ba);
        slow.first["/api/auth/login"_ba].append(late);
        const QString up = wall.add(at(there, u"There"_s), u"secret"_s).value();
        // The one that answers, looked at and known first: what is looked at here is the moment the asking waits for
        // the slow one, and a first look at the other one took longer than that moment on the loaded slow cores (the
        // fix round's item). Known, it is told as it is known while it is looked at again.
        QCOMPARE(settled(wall, up).state, u"online"_s);
        const QString behind = wall.add(at(slow, u"Slow"_s), u"secret"_s).value();
        QElapsedTimer watch;
        watch.start();
        const QHash<QString, core::Status> all = statusOf(wall);
        // The one that answered is told as it is, and the slow one as it was last known: nobody
        // waits longer than that for it, and the whole wall's answer does not hang on it.
        QVERIFY2(watch.elapsed() >= pace.lookPatienceMs - 50, qPrintable(QString::number(watch.elapsed())));
        QVERIFY2(watch.elapsed() < late.afterMs - 200, qPrintable(QString::number(watch.elapsed())));
        QCOMPARE(all.value(up).state, u"online"_s);
        QCOMPARE(all.value(behind).state, u"connecting"_s);
        // Its look goes on by itself, and what it finds is told to whoever asks next.
        QCOMPARE(settled(wall, behind).state, u"online"_s);
        QCOMPARE(slow.signIns, 0);
        QCOMPARE(slow.asked("/api/auth/login"), 1);
    }

    void anAskingIsAnsweredAlsoWhereAKvmIsTakenOffWhileItWaits()
    {
        // The driver whose look it waited for is let go: the asking is answered all the same, in
        // the time it would have waited, with the wall as it is by then.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::KvmdDevice::Pace pace = quick();
        pace.lookPatienceMs = 400;
        LocalWall wall(settings, secrets, pace);
        const MadeUpKvm there;
        MadeUpKvm slow;
        Answered late = good({{u"token"_s, u"pass9"_s}});
        late.afterMs = 1200;
        slow.first["/api/auth/login"_ba].append(late);
        const QString up = wall.add(at(there, u"There"_s), u"secret"_s).value();
        // The one that answers, looked at and known first, as in anAskingWaitsOnlyAMomentForAKvmThatIsSlow: what is
        // looked at is the asking whose driver goes, and a first look at this one took longer than the asking's moment
        // on the loaded slow cores (this round's whole runs: "connecting").
        QCOMPARE(settled(wall, up).state, u"online"_s);
        const QString behind = wall.add(at(slow, u"Slow"_s), u"secret"_s).value();
        QList<QHash<QString, core::Status>> told;
        wall.askStatus([&told](const base::Result<QHash<QString, core::Status>>& all) {
            told.append(all.ok() ? all.value() : QHash<QString, core::Status>{});
        });
        QTRY_COMPARE(slow.asked("/api/auth/login"), 1);
        QVERIFY(wall.remove(behind).ok());
        QVERIFY(told.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, pace.lookPatienceMs + 600);
        QCOMPARE(told.constFirst().keys(), QStringList{up});
        QCOMPARE(told.constFirst().value(up).state, u"online"_s);
        QTest::qWait(300);
        QCOMPARE(told.size(), 1);
    }

    void aKvmsHostIsNamedInTheSignsOfItsAddressAndNotInLettersItCouldPassFor()
    {
        // A name in its xn-- form is a name of plain signs. Decoded, as Qt decodes it under the
        // endings it knows, it is letters of another script that read as a well-known name: a
        // tile, and the name a KVM is given where none was typed, show the signs.
        OwnKvm kvm;
        kvm.address = u"https://xn--80ak6aa92e.com:8443"_s;
        QCOMPARE(LocalWall::hostOf(kvm), u"xn--80ak6aa92e.com:8443"_s);
        kvm.address = u"https://192.168.1.20"_s;
        QCOMPARE(LocalWall::hostOf(kvm), u"192.168.1.20"_s);
        kvm.address = u"http://kvm.example.net:8080"_s;
        QCOMPARE(LocalWall::hostOf(kvm), u"kvm.example.net:8080"_s);
    }

    void everyAskingAtTheWallsOwnPaceBeginsALook()
    {
        // A wall asks how its machines are every four seconds (WallFeed, where nobody names
        // another pace). What is known of a KVM has to count as old by then: when it counted as
        // fresh for those same four seconds, every second asking found it fresh and began no
        // look, and a KVM was looked at every eight. The driver's own times, as they are where
        // nobody chose others, and the wall's pace: each a quarter here.
        const int wallsPaceMs = 4000;
        const core::KvmdDevice::Pace usual;
        core::KvmdDevice::Pace pace = quick();
        pace.freshMs = usual.freshMs / 4;
        pace.lookPatienceMs = usual.lookPatienceMs / 4;
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, pace);
        const MadeUpKvm kvm;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        // Known first, and old by the first asking: its first look signs in, which took longer than an asking waits for
        // a look on the loaded slow cores (the fix round's item); what is counted is the looks the askings begin.
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QTest::qWait(pace.freshMs + 50);
        const int before = kvm.asked("/api/streamer");
        QElapsedTimer watch;
        watch.start();
        const int askings = 4;
        for (int asking = 0; asking < askings; ++asking) {
            QTest::qWait(static_cast<int>(std::max<qint64>(0, qint64{asking} * wallsPaceMs / 4 - watch.elapsed())));
            QCOMPARE(statusOf(wall).value(id).state, u"online"_s);
        }
        QTRY_COMPARE(kvm.asked("/api/streamer"), before + askings);
        // (With room for a look that takes as long as an asking waits for one.)
        QVERIFY(usual.freshMs + usual.lookPatienceMs <= wallsPaceMs);
    }

    // --- a KVM of the wall, live (what a live does is tried in test_kvmd_live.cpp) ---

    void aKvmOfTheWallIsHadLive()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        // (The made-up KVM as it is has no stream: its pictures are asked for one after the other.)
        const MadeUpKvm kvm;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        // Nothing is asked of a KVM for being on the wall.
        QTest::qWait(150);
        QVERIFY(kvm.stand->asked.isEmpty());

        core::WallSource& source = wall;
        std::unique_ptr<core::Live> live;
        QList<base::Error> refused;
        source.openLive(id, {1920, 30}, [&](base::Result<std::unique_ptr<core::Live>> made) {
            if (made) {
                live = std::move(made).value();
            } else {
                refused.append(made.error());
            }
        });
        // Handed over a turn later, as everything is answered.
        QVERIFY(live == nullptr);
        QTRY_VERIFY(live != nullptr);
        QVERIFY(refused.isEmpty());
        QVERIFY(live->isOpen());
        QList<QByteArray> pictures;
        QList<QSize> sizes;
        int ended = 0;
        connect(live.get(), &core::Live::pictureCame, live.get(),
                [&](const core::Live::Picture& picture) { pictures.append(picture.data); });
        connect(live.get(), &core::Live::videoChanged, live.get(),
                [&](const core::Live::Video& video) { sizes.append(QSize(video.width, video.height)); });
        connect(live.get(), &core::Live::closed, live.get(), [&] { ++ended; });
        // Its picture, as the KVM gave it, with its size said first; signed in to as the wall's
        // driver signs in.
        QTRY_COMPARE(pictures.size(), 1);
        QCOMPARE(pictures[0], core::testing::jpeg(kvm.width, kvm.height));
        QCOMPARE(sizes, (QList<QSize>{QSize(1920, 1080)}));
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(kvm.asked("/streamer/stream"), 1);

        // A second who asks shares what is asked of the KVM: its stream is not asked for again.
        std::unique_ptr<core::Live> other;
        source.openLive(id, {1280, 30}, [&](base::Result<std::unique_ptr<core::Live>> made) {
            if (made) {
                other = std::move(made).value();
            }
        });
        QTRY_VERIFY(other != nullptr);
        QList<QByteArray> others;
        connect(other.get(), &core::Live::pictureCame, other.get(),
                [&](const core::Live::Picture& picture) { others.append(picture.data); });
        QTRY_COMPARE(others.size(), 1);
        QCOMPARE(kvm.asked("/streamer/stream"), 1);
        QCOMPARE(kvm.signIns, 1);
        // Both let go: the KVM is asked for no more pictures, and nobody is told of an end.
        live->close();
        other.reset();
        QTest::qWait(400);
        const int asked = kvm.asked("/api/streamer/snapshot");
        QTest::qWait(600);
        QCOMPARE(kvm.asked("/api/streamer/snapshot"), asked);
        QCOMPARE(ended, 0);
    }

    void whatIsNotOnTheWallIsNotHadLive()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const MadeUpKvm kvm;
        const QString id = wall.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        QList<base::Error> refused;
        int handedOver = 0;
        const auto noted = [&](const base::Result<std::unique_ptr<core::Live>>& made) {
            if (made) {
                ++handedOver;
            } else {
                refused.append(made.error());
            }
        };
        wall.openLive(u"nobody"_s, {1920, 30}, noted);
        QVERIFY(refused.isEmpty());
        QTRY_COMPARE(refused.size(), 1);
        QCOMPARE(refused[0].code, std::string("not_found"));
        QCOMPARE(refused[0].message, std::string("Unknown device"));
        // Nor a KVM that was taken off between the asking and the answer.
        wall.openLive(id, {1920, 30}, noted);
        QVERIFY(wall.remove(id).ok());
        QTRY_COMPARE(refused.size(), 2);
        QCOMPARE(refused[1].code, std::string("not_found"));
        QCOMPARE(handedOver, 0);
        QTest::qWait(200);
        QVERIFY(kvm.stand->asked.isEmpty());
    }

    // --- what the wall asks before it signs in, and what it records by itself ---------------------

    void atItsFirstStartAWallThatWorkedIsAskedAndSignedInToAlsoWhereAKvmIsSlowToLetALineIn()
    {
        // A wall as an earlier program left it: a settings file that says of no KVM that it was
        // asked what it is, and each KVM's password with Windows. Three KVMs: one that is quick,
        // one that lets a line in late (behind a VPN or a relay, or busy), and one that is
        // switched off. With the times the program has: nobody chose any.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const MadeUpKvm quickOne(tls("first"));
        const MadeUpKvm slow(tls("second"));
        // (Both keep a line for the next question, as a real KVM does. The slow one lets each new
        // line in late.)
        quickOne.stand->keepsLines = true;
        slow.stand->keepsLines = true;
        // (Late by all of the program's time to be let in, 4.5 s, but for the 2.5 s its TLS handshake and questions
        // take on a loaded PC: three seconds late left 1.5 s, and this round's whole runs found it "offline" once.)
        QVERIFY(slow.stand->letInOnlyAfter(core::kvm::letInWithinMs - 2500));
        // What is switched off answers nothing. On this PC's own address that is something that
        // takes the call and never says a word: to whoever asks, nothing until its time is over.
        QTcpServer off;
        QVERIFY(off.listen(QHostAddress::LocalHost, 0));
        const QString offAddress = u"https://127.0.0.1:%1"_s.arg(off.serverPort());
        const QString quickId = u"0a000001"_s;
        const QString slowId = u"0a000002"_s;
        const QString offId = u"0a000003"_s;
        const auto entry = [](const QString& id, const QString& name, const QString& address,
                              const QByteArray& sha256) {
            return QJsonObject{{u"kind"_s, u"kvmd"_s},
                               {u"id"_s, id},
                               {u"name"_s, name},
                               {u"address"_s, address},
                               {u"user"_s, u"admin"_s},
                               {u"group"_s, u""_s},
                               {u"known_by_certificate"_s, true},
                               {u"certificate"_s, QString::fromLatin1(sha256.toHex())}};
        };
        const QByteArray asItWasWritten =
            QJsonDocument(
                QJsonObject{
                    {u"own_wall"_s, true},
                    {u"kvms"_s, QJsonArray{entry(quickId, u"Quick"_s, quickOne.stand->address(), sha256Of("first")),
                                           entry(slowId, u"Slow"_s, slow.stand->address(), sha256Of("second")),
                                           entry(offId, u"Off"_s, offAddress, sha256Of("comet"))}}})
                .toJson();
        QVERIFY(!asItWasWritten.contains("asked"));
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(asItWasWritten);
        written.close();
        MemorySecrets secrets;
        secrets.all.insert(LocalWall::secretOf(quickId),
                           savedFor("secret", quickOne.stand->address(), sha256Of("first")));
        secrets.all.insert(LocalWall::secretOf(slowId), savedFor("secret", slow.stand->address(), sha256Of("second")));
        secrets.all.insert(LocalWall::secretOf(offId), savedFor("secret", offAddress, sha256Of("comet")));

        core::Settings settings(file);
        QCOMPARE(settings.remembered().kvms.size(), 3);
        LocalWall wall(settings, secrets);
        QVERIFY(!wall.wasAskedWhatItIs(quickId) && !wall.wasAskedWhatItIs(slowId) && !wall.wasAskedWhatItIs(offId));

        // The wall is asked how its KVMs are as a window asks it: at once, and every four seconds.
        // What a tile says is what the last answer said of its KVM: its state, and under it the
        // code and the sentence that came with it, where one did.
        const auto tile = [](const core::Status& status) {
            QString says = status.state;
            if (!status.errorCode.isEmpty() || !status.errorMessage.isEmpty()) {
                says += u" ["_s + status.errorCode + u"] "_s + status.errorMessage;
            }
            return says;
        };
        const QString nothingAnswers =
            u"offline [unreachable] Nothing answers at this address. No password is sent there before a KVM has "
            "answered."_s;
        QElapsedTimer watch;
        watch.start();
        // And it is asked once more at each word of the wall's own, that a KVM has come to
        // something, as a window asks at that word: when the word was said, when it was
        // answered, and what the quick one's tile says in that answer.
        struct Word {
            qint64 said = 0;
            qint64 answered = 0;
            QString quick;
        };
        QList<Word> atTheWallsWord;
        connect(&wall, &LocalWall::cameToSomething, this, [&] {
            wall.askStatus([&, said = watch.elapsed()](const base::Result<QHash<QString, core::Status>>& all) {
                atTheWallsWord.append({said, watch.elapsed(), tile(all.value().value(quickId))});
            });
        });
        QStringList slowSaid;
        QStringList offSaid;
        QList<qint64> askedAt;
        for (int asking = 0; asking < 6 && slowSaid.value(slowSaid.size() - 1) != u"online"_s; ++asking) {
            QTest::qWait(static_cast<int>(std::max<qint64>(0, qint64{asking} * 4000 - watch.elapsed())));
            askedAt.append(watch.elapsed());
            const QHash<QString, core::Status> all = statusOf(wall);
            const QString quickSays = tile(all.value(quickId));
            slowSaid.append(tile(all.value(slowId)));
            offSaid.append(tile(all.value(offId)));
            qInfo().noquote() << u"asked at %1 s, answered at %2 s: quick: %3 | slow: %4 | off: %5"_s.arg(
                QString::number(static_cast<double>(askedAt.constLast()) / 1000, 'f', 1),
                QString::number(static_cast<double>(watch.elapsed()) / 1000, 'f', 1), quickSays, slowSaid.constLast(),
                offSaid.constLast());
            // The quick one is there in the first answer, and stays. (Where the PC itself is
            // slow, under a sanitizer, the first answer may come before its first look has
            // ended, or before it was asked through: then it is "connecting" there, and nothing
            // else, as the slow one. Nobody waits for the second asking to see it online for
            // that: the wall says by itself when it is, which is looked at below.)
            QVERIFY2(quickSays == u"online"_s || (asking == 0 && quickSays == u"connecting"_s), qPrintable(quickSays));
            // The slow one is "connecting" until it is there, and nothing else is ever said of
            // it: no code, no sentence, nothing that reads as something being wrong.
            QVERIFY2(slowSaid.constLast() == u"connecting"_s || slowSaid.constLast() == u"online"_s,
                     qPrintable(slowSaid.constLast()));
            // And the one that is off is "connecting" until the asking has given it the time a
            // driver gives a KVM to be let in, and "offline" from then on.
            QVERIFY2(offSaid.constLast() == u"connecting"_s || offSaid.constLast() == nothingAnswers,
                     qPrintable(offSaid.constLast()));
        }
        QCOMPARE(slowSaid.constLast(), u"online"_s);
        QCOMPARE(offSaid.constLast(), nothingAnswers);
        QCOMPARE(offSaid.constFirst(), u"connecting"_s);
        // The wall's first word was that the quick one had come to something, said before the
        // second asking (and so before the one that is off was given up on, at 4.5 s, which
        // is a word of its own): whoever asked at that word was told "online". Its tile turns
        // then, and not four seconds after the start, whatever the first answer said of it.
        // (The answer to the word may wait a moment for the slow one's first look, where its
        // asking had ended by then: no longer than any asking waits for a look,
        // anAskingWaitsOnlyAMomentForAKvmThatIsSlow, and a second for a busy PC. On a quiet PC
        // the word comes about a second after the start, under the address sanitizer two to
        // three: these KVMs speak TLS on this thread, one line after the other.)
        // (Before the second asking as it was made: on the loaded slow cores the word came 4.1 and 4.5 s after the
        // start, and the asking that the window makes four seconds after it came later too.)
        QVERIFY(!atTheWallsWord.isEmpty());
        const Word& first = atTheWallsWord.constFirst();
        QCOMPARE(first.quick, u"online"_s);
        QVERIFY(askedAt.size() >= 2);
        QVERIFY2(first.said < askedAt.at(1),
                 qPrintable(u"%1, the second asking at %2"_s.arg(first.said).arg(askedAt.at(1))));
        QVERIFY2(first.answered - first.said <= core::KvmdDevice::Pace{}.lookPatienceMs + 1000,
                 qPrintable(u"%1 to %2"_s.arg(first.said).arg(first.answered)));
        qInfo().noquote() << u"the wall's first word, at which the quick one was online: said at %1 ms, answered at %2 "
                             "ms"_s.arg(first.said)
                                 .arg(first.answered);

        // Each of the two that are there was asked what it is, with nothing that says who asks,
        // and only then signed in to, once.
        for (const MadeUpKvm* kvm : {&quickOne, &slow}) {
            QCOMPARE(kvm->signIns, 1);
            const QList<Asked> asked = kvm->stand->asked;
            // (The three questions, and the one its driver asks before the sign-in, kvmd's again: a password goes
            // only on a line shown to be up.)
            QVERIFY(asked.size() > 4);
            QSet<QByteArray> questions;
            for (qsizetype index = 0; index < 4; ++index) {
                QCOMPARE(asked.at(index).method, "GET"_ba);
                QVERIFY(asked.at(index).body.isEmpty());
                QVERIFY(!asked.at(index).headers.contains("cookie"));
                questions.insert(asked.at(index).path);
            }
            QCOMPARE(questions, (QSet<QByteArray>{kvmdsQuestion, jetKvmsQuestion, redfishsQuestion}));
            QCOMPARE(asked.at(3).path, QByteArray(kvmdsQuestion));
            QCOMPARE(asked.at(4).path, QByteArray(signInQuestion));
            QCOMPARE(asked.at(4).body, "user=admin&passwd=secret"_ba);
        }
        QVERIFY(wall.wasAskedWhatItIs(quickId));
        QVERIFY(wall.wasAskedWhatItIs(slowId));
        QVERIFY(!wall.wasAskedWhatItIs(offId));
        // That is kept, so that each is asked once: written down a short while later.
        QTRY_VERIFY_WITH_TIMEOUT(!settings.hasUnwritten(), 10000);
        const core::Settings again(file);
        QCOMPARE(again.remembered().kvms.size(), 3);
        QVERIFY(again.remembered().kvms.at(0).askedWhatItIs);
        QVERIFY(again.remembered().kvms.at(1).askedWhatItIs);
        QVERIFY(!again.remembered().kvms.at(2).askedWhatItIs);
    }

    void aYesToAnotherCertificateHasTheKvmAskedAgainBeforeItsPasswordGoes()
    {
        // A KVM of the wall that was added by hand, known by its certificate, and signed in to.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        const quint16 port = kvm->stand->serverPort();
        const QString address = kvm->stand->address();
        const QString id = wall.add(knownAt(address, sha256Of("first")), u"secret"_s).value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QVERIFY(wall.wasAskedWhatItIs(id));

        // Its address comes to be another machine's: one that shows another certificate, and is
        // no KVM at all (a page that asks for a name and a password).
        kvm.reset();
        const StandIn page(
            [](const Asked&) { return Answered{200, "<html><h1>Sign in</h1></html>"_ba, "text/html"_ba}; },
            tls("second"), port);
        QVERIFY(page.isListening());
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).errorCode, u"cert_mismatch"_s, 15000);
        QVERIFY(page.asked.isEmpty());
        // Somebody looks at the certificate it shows now, and says yes to it. The look asked
        // which certificate, and never what kind of thing shows it.
        QVERIFY(wall.knowBy(id, sha256Of("second")).ok());
        // So it is asked that before the kept password goes to it: and it is no KVM.
        const core::Status status = settled(wall, id);
        QCOMPARE(page.count(signInQuestion), 0);
        QVERIFY(heardOnlyTheQuestions(page));
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"not_a_kvm"_s);
        QCOMPARE(status.errorMessage,
                 u"Something answers at this address, but it is not a KVM that this program speaks "
                 "to. No password was sent to it."_s);
        QVERIFY(!wall.wasAskedWhatItIs(id));
        QVERIFY(!wall.kvms().constFirst().askedWhatItIs);
        // Its password is where it was, saved for the certificate that was said yes to.
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", address, sha256Of("second")));
    }

    void aKvmThatShowsANewCertificateOfItsOwnIsAskedUnderItAndSignedInToAfterTheYes()
    {
        // The same, where what shows the other certificate IS the KVM (it was set up anew): the
        // yes costs one asking, and then it is signed in to as before.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        auto before = std::make_unique<MadeUpKvm>(tls("first"));
        const quint16 port = before->stand->serverPort();
        const QString id = wall.add(knownAt(before->stand->address(), sha256Of("first")), u"secret"_s).value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        before.reset();
        const MadeUpKvm anew(tls("second"), port);
        QVERIFY(anew.stand->isListening());
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).errorCode, u"cert_mismatch"_s, 15000);
        QVERIFY(wall.knowBy(id, sha256Of("second")).ok());
        QVERIFY(!wall.wasAskedWhatItIs(id));
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QVERIFY(wall.wasAskedWhatItIs(id));
        QCOMPARE(anew.signIns, 1);
        QCOMPARE(anew.stand->asked.value(4).path, QByteArray(signInQuestion)); // (after the question asked first)
        QCOMPARE(anew.stand->count(jetKvmsQuestion), 1);
        // And a yes to the very certificate it is known by changes nothing: it stays one that
        // was asked.
        QVERIFY(wall.knowBy(id, sha256Of("second")).ok());
        QVERIFY(wall.wasAskedWhatItIs(id));
    }

    void aKvmOverPlainHttpIsAskedOnceInEveryRunAndOneOverHttpsOnceForAll()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const MadeUpKvm plain;
        const MadeUpKvm encrypted(tls("first"));
        QString plainId;
        QString encryptedId;
        // Room for a TLS handshake on the loaded slow cores, where 1.5 s found the one over HTTPS "offline" (the fix
        // round's item): what is looked at here is who is asked what, not how soon.
        core::KvmdDevice::Pace roomy = quick();
        roomy.connectWithinMs = 10'000;
        {
            // Added in the program: each was asked by its box, and the wall asks neither again
            // in this run.
            core::Settings settings(file);
            LocalWall wall(settings, secrets, roomy);
            plainId = wall.add(at(plain, u"Plain"_s), u"secret"_s).value();
            encryptedId = wall.add(knownAt(encrypted.stand->address(), sha256Of("first")), u"secret"_s).value();
            QVERIFY(wall.wasAskedWhatItIs(plainId));
            QVERIFY(wall.wasAskedWhatItIs(encryptedId));
            QCOMPARE(settled(wall, plainId).state, u"online"_s);
            QCOMPARE(settled(wall, encryptedId).state, u"online"_s);
            QCOMPARE(plain.stand->count(jetKvmsQuestion), 0);
            QCOMPARE(encrypted.stand->count(jetKvmsQuestion), 0);
            // What is kept of it for the next run: of the one over HTTPS, that it was asked. Of
            // the one over plain HTTP, nothing.
            QVERIFY(!wall.kvms().at(0).askedWhatItIs);
            QVERIFY(wall.kvms().at(1).askedWhatItIs);
        }
        for (int run = 1; run <= 2; ++run) {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, roomy);
            QVERIFY(!wall.wasAskedWhatItIs(plainId));
            QVERIFY(wall.wasAskedWhatItIs(encryptedId));
            const int signInsBefore = plain.signIns;
            const qsizetype askedBefore = plain.stand->asked.size();
            QCOMPARE(settled(wall, plainId).state, u"online"_s);
            QCOMPARE(settled(wall, encryptedId).state, u"online"_s);
            QVERIFY(wall.wasAskedWhatItIs(plainId));
            // The plain one heard the three questions once more, before its password went again.
            QCOMPARE(plain.stand->count(jetKvmsQuestion), run);
            QCOMPARE(plain.stand->count(redfishsQuestion), run);
            QCOMPARE(plain.signIns, signInsBefore + 1);
            // (After the three, the one its driver asks before the sign-in.)
            QCOMPARE(plain.stand->asked.value(askedBefore + 4).path, QByteArray(signInQuestion));
            for (qsizetype index = askedBefore; index < askedBefore + 4; ++index) {
                QVERIFY(plain.stand->asked.value(index).path != signInQuestion);
            }
            // Once in a run: not again at the looks that follow.
            QTest::qWait(quick().freshMs + 100);
            QCOMPARE(settled(wall, plainId).state, u"online"_s);
            QCOMPARE(plain.stand->count(jetKvmsQuestion), run);
            // And the one over HTTPS never: its certificate says each time who answers.
            QCOMPARE(encrypted.stand->count(jetKvmsQuestion), 0);
            QCOMPARE(encrypted.stand->count(redfishsQuestion), 0);
            // (Still nothing is kept of the plain one's having been asked.)
            QVERIFY(!wall.kvms().at(0).askedWhatItIs);
        }
    }

    void overPlainHttpTheKeptPasswordIsNotPostedInALaterRunToWhatIsNoKvmAtAll()
    {
        // A KVM over plain HTTP that was signed in to in one run. By the next, its address is
        // another machine's, which is no KVM: the password, which would go in the clear, is not
        // posted to it. (Whatever answers the three questions as a KVM does would be handed it,
        // and whoever sits on the line reads it anyway: that is what plain HTTP is.)
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        auto kvm = std::make_unique<MadeUpKvm>();
        const quint16 port = kvm->stand->serverPort();
        QString id;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quick());
            id = wall.add(at(*kvm, u"Plain"_s), u"secret"_s).value();
            QCOMPARE(settled(wall, id).state, u"online"_s);
        }
        kvm.reset();
        const StandIn page(
            [](const Asked&) { return Answered{200, "<html><h1>Printer</h1></html>"_ba, "text/html"_ba}; }, port);
        QVERIFY(page.isListening());
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        const core::Status status = settled(wall, id);
        QCOMPARE(page.count(signInQuestion), 0);
        QVERIFY(heardOnlyTheQuestions(page));
        QCOMPARE(status.errorCode, u"not_a_kvm"_s);
        QVERIFY(!wall.wasAskedWhatItIs(id));
    }

    void aKvmBehindAGatewayThatSaysItIsAwayIsAwayAndNotSomethingElse()
    {
        // A wall as it was left, of two KVMs that are asked in every run. The first is well,
        // and stands behind a reverse proxy: while the KVM restarts, the proxy answers every
        // question in its place, 502 and a page of its own. At the address of the second there
        // is something else now, whose own gateway answers for one place of it only.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const Answered badGateway{502, "<html><body><h1>502 Bad Gateway</h1></body></html>"_ba, "text/html"_ba};
        MadeUpKvm kvm;
        for (const char* question : {kvmdsQuestion, jetKvmsQuestion, redfishsQuestion}) {
            kvm.instead.insert(question, badGateway);
        }
        const StandIn other([&badGateway](const Asked& asked) {
            return asked.path == kvmdsQuestion
                       ? badGateway
                       : Answered{404, "<html><body>Not found</body></html>"_ba, "text/html"_ba};
        });
        const QString id = u"0e100001"_s;
        const QString otherId = u"0e100002"_s;
        MemorySecrets secrets;
        secrets.all.insert(LocalWall::secretOf(id), savedFor("secret", kvm.stand->address()));
        secrets.all.insert(LocalWall::secretOf(otherId), savedFor("secret", other.address()));
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(
            QJsonDocument(
                QJsonObject{
                    {u"kvms"_s,
                     QJsonArray{
                         QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, id}, {u"address"_s, kvm.stand->address()}},
                         QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, otherId}, {u"address"_s, other.address()}}}}})
                .toJson());
        written.close();
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());

        // The KVM is away, and its tile says so: offline, as an address where nothing answers,
        // in a sentence that says who answered and who did not, and that no password was sent.
        // It did say "error [not_a_kvm] Something answers at this address, but it is not a KVM
        // that this program speaks to. No password was sent to it.", which is untrue of a KVM
        // that is away for a moment, and reads as if somebody had added the wrong thing.
        QCOMPARE(tileOf(settled(wall, id)), onlyAGatewayAnswersThere);
        QVERIFY(!wall.wasAskedWhatItIs(id));
        // No password went anywhere, in either case: all that was heard is the three questions.
        QCOMPARE(kvm.signIns, 0);
        QCOMPARE(kvm.stand->count(signInQuestion), 0);
        QVERIFY(kvm.stand->asked.size() >= 3);
        for (const Asked& one : std::as_const(kvm.stand->asked)) {
            QCOMPARE(one.method, "GET"_ba);
            QVERIFY(one.body.isEmpty() && !one.headers.contains("cookie") && !one.headers.contains("authorization"));
        }
        // What is something else stays what it is: one answer of another sort, and it is no
        // gateway's word alone.
        QCOMPARE(tileOf(settled(wall, otherId)),
                 u"error [not_a_kvm] Something answers at this address, but it is not a KVM that this program speaks "
                 "to. No password was sent to it."_s);
        QCOMPARE(other.count(signInQuestion), 0);
        // Nothing is to be had of a KVM that is away so, as of one where nothing answers.
        const base::Result<core::Snapshot> picture =
            answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(id, 480, 1, std::move(done)); });
        QVERIFY(!picture.ok());
        QCOMPARE(picture.error().code, std::string("not_asked_yet"));

        // The KVM is back behind its proxy. It is asked again as an address where nothing
        // answers is, by itself, and then it is the KVM: asked, signed in to once, online.
        kvm.instead.clear();
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(wall).value(id).state, u"online"_s, 20000);
        QVERIFY(wall.wasAskedWhatItIs(id));
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(other.count(signInQuestion), 0);
        QVERIFY(!wall.wasAskedWhatItIs(otherId));
    }

    void whatTheWallRecordsByItselfIsWrittenTogetherAndWhatSomebodyDidAtOnce()
    {
        // This checks settings writes, so TLS uses the application's transport budgets.
        // Keep the fast status polling and every write deadline/assertion unchanged.
        core::KvmdDevice::Pace pace = quick();
        const core::KvmdDevice::Pace production;
        pace.connectWithinMs = production.connectWithinMs;
        pace.quietMs = production.quietMs;
        pace.transferMs = production.transferMs;
        // A wall of three KVMs as an older settings file has them. Each is asked what it is, and
        // each shows its certificate, which the wall keeps: six things the wall records by
        // itself, within moments of each other. They are one writing of the settings.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const MadeUpKvm one(tls("first"));
        const MadeUpKvm two(tls("second"));
        const MadeUpKvm three(tls("comet"));
        const QStringList ids{u"0a000001"_s, u"0a000002"_s, u"0a000003"_s};
        const QList<const MadeUpKvm*> kvms{&one, &two, &three};
        const QList<QByteArray> known{sha256Of("first"), sha256Of("second"), sha256Of("comet")};
        MemorySecrets secrets;
        QJsonArray entries;
        for (qsizetype index = 0; index < 3; ++index) {
            entries.append(QJsonObject{{u"kind"_s, u"kvmd"_s},
                                       {u"id"_s, ids.at(index)},
                                       {u"address"_s, kvms.at(index)->stand->address()},
                                       {u"known_by_certificate"_s, true},
                                       {u"certificate"_s, QString::fromLatin1(known.at(index).toHex())}});
            secrets.all.insert(LocalWall::secretOf(ids.at(index)),
                               savedFor("secret", kvms.at(index)->stand->address(), known.at(index)));
        }
        const auto writeAsItWas = [&file, &entries] {
            QFile written(file);
            return written.open(QIODevice::WriteOnly) &&
                   written.write(QJsonDocument(QJsonObject{{u"kvms"_s, entries}}).toJson()) > 0;
        };
        const auto fileText = [&file] {
            QFile kept(file);
            return kept.open(QIODevice::ReadOnly) ? kept.readAll() : QByteArray();
        };
        const auto allRecorded = [&ids](const LocalWall& wall) {
            return std::ranges::all_of(ids, [&wall](const QString& id) {
                return wall.wasAskedWhatItIs(id) && !wall.certificateOf(id).isEmpty();
            });
        };
        QVERIFY(writeAsItWas());
        {
            core::Settings settings(file);
            LocalWall::Own own;
            // (A while longer than the moments between the six on the loaded slow cores, where 1.5 s was less than one
            // of them, and two writings came: the fix round's item.)
            own.writtenAfterMs = 6000;
            own.writtenAtTheLatestMs = 40'000;
            LocalWall wall(settings, secrets, pace, own);
            for (const QString& id : ids) {
                QCOMPARE(settled(wall, id).state, u"online"_s);
            }
            QTRY_VERIFY(allRecorded(wall));
            // Remembered at once: the wall and its list say so.
            for (const OwnKvm& kvm : wall.kvms()) {
                QVERIFY(kvm.askedWhatItIs);
            }
            // And written a short while after the last of it: once.
            QTRY_VERIFY_WITH_TIMEOUT(!settings.hasUnwritten(), 45'000);
            QCOMPARE(settings.writings(), quint64{1});
            QCOMPARE(fileText().count("\"asked\": true"), 3);
            QCOMPARE(fileText().count("\"certificate_pem\""), 3);
            // What somebody does is put off by nothing: a change is in the file as change() returns.
            OwnKvm renamed = wall.kvms().constFirst();
            renamed.name = u"Renamed"_s;
            QVERIFY(wall.change(ids.constFirst(), renamed, std::nullopt).ok());
            QCOMPARE(settings.writings(), quint64{2});
            QVERIFY(fileText().contains("Renamed"));
        }

        // Where the short while is not over when the wall goes, it is written as the wall goes.
        QVERIFY(writeAsItWas());
        {
            core::Settings settings(file);
            LocalWall::Own own;
            own.writtenAfterMs = 600'000;
            own.writtenAtTheLatestMs = 600'000;
            {
                LocalWall wall(settings, secrets, pace, own);
                for (const QString& id : ids) {
                    QCOMPARE(settled(wall, id).state, u"online"_s);
                }
                QTRY_VERIFY(allRecorded(wall));
                QVERIFY(settings.hasUnwritten());
                QCOMPARE(settings.writings(), quint64{0});
                QVERIFY(!fileText().contains("\"asked\": true"));
            }
            QVERIFY(!settings.hasUnwritten());
            QCOMPARE(settings.writings(), quint64{1});
            QCOMPARE(fileText().count("\"asked\": true"), 3);
        }

        // And no later than a while after the first of it, however much comes after.
        QVERIFY(writeAsItWas());
        {
            core::Settings settings(file);
            LocalWall::Own own;
            own.writtenAfterMs = 600'000;
            own.writtenAtTheLatestMs = 1200;
            LocalWall wall(settings, secrets, pace, own);
            QCOMPARE(settled(wall, ids.constFirst()).state, u"online"_s);
            QTRY_VERIFY_WITH_TIMEOUT(settings.writings() >= 1, 10000);
            QVERIFY(fileText().contains("\"asked\": true"));
        }
    }

    // --- a KVM's own number (the owner's Q2, 2026-10-28) ------------------------------------------

    void whatMakesAKvmTheOneItIsIsItsNumberThenItsCertificate()
    {
        // As the web's core.identity, with one difference: a certificate the two are known by is proven (the
        // device has the key to it), and makes them one whatever numbers they give; the number a device gives
        // for itself is said, and makes two one that are known by no common certificate. Two that give none
        // and are known by none are two.
        const QByteArray first = sha256Of("first");
        const auto kvm = [](const QString& number, const QByteArray& certificate) {
            OwnKvm one;
            one.address = u"https://192.0.2.5"_s;
            one.kind = u"jetkvm"_s;
            one.number = number;
            one.knownByCertificate = !certificate.isEmpty();
            one.certificate = certificate;
            return one;
        };
        const QString n = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        const QString m = u"jetkvm:fedcba9876543210fedcba9876543210"_s;
        QVERIFY(LocalWall::twinOf({kvm(n, {})}, kvm(n, {})).has_value());
        QVERIFY(LocalWall::twinOf({kvm(m, first)}, kvm(n, first)).has_value());
        QVERIFY(!LocalWall::twinOf({kvm(m, {})}, kvm(n, {})).has_value());
        QVERIFY(LocalWall::twinOf({kvm({}, first)}, kvm({}, first)).has_value());
        QVERIFY(!LocalWall::twinOf({kvm({}, {})}, kvm({}, {})).has_value());
        QVERIFY(LocalWall::twinOf({kvm({}, first)}, kvm(n, first)).has_value());
        QVERIFY(LocalWall::twinOf({kvm(n, sha256Of("second"))}, kvm(n, first)).has_value());
        // Said as what it says of itself, or as what it proves.
        OwnKvm there = kvm(n, {});
        there.name = u"Rack JetKVM"_s;
        QCOMPARE(LocalWall::sameDevice(there, true).message,
                 std::string("This says it is the same device as \"Rack JetKVM\", which is on the wall already "
                             "(192.0.2.5)"));
        QCOMPARE(LocalWall::sameDevice(there).message,
                 std::string("This is the same device as \"Rack JetKVM\", which is on the wall already (192.0.2.5)"));
    }

    void aKvmsNumberIsKeptFromOneRunToTheNextWhereItIsOneADriverGives()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QString n = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        {
            core::Settings settings(file);
            MemorySecrets secrets;
            LocalWall wall(settings, secrets, quick());
            OwnKvm jet;
            jet.address = u"http://192.0.2.5"_s;
            jet.kind = u"jetkvm"_s;
            jet.number = n;
            QVERIFY(wall.add(jet, u"secret"_s).ok());
        }
        {
            const core::Settings again(file);
            QCOMPARE(again.remembered().kvms.size(), 1);
            QCOMPARE(again.remembered().kvms.constFirst().number, n);
        }
        // What is no number a driver gives (a mark this program makes, a word) is not read as one.
        QFile kept(file);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        const QByteArray text = kept.readAll();
        kept.close();
        for (const QByteArray& instead : {"own:0123456789abcdef"_ba, "jetkvm:0123"_ba, "JetKVM:0123456789abcdef"_ba}) {
            QFile rewritten(file);
            QVERIFY(rewritten.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QByteArray other = text;
            rewritten.write(other.replace(n.toUtf8(), instead));
            rewritten.close();
            const core::Settings read(file);
            QCOMPARE(read.remembered().kvms.size(), 1);
            QVERIFY2(read.remembered().kvms.constFirst().number.isEmpty(), instead.constData());
        }
    }

    void aKvmFromAFileThatGivesTheNumberOfOneOfTheWallIsThatKvm()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const QString n = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        OwnKvm jet;
        jet.address = u"http://192.0.2.5"_s;
        jet.name = u"Rack JetKVM"_s;
        jet.kind = u"jetkvm"_s;
        jet.number = n;
        QVERIFY(wall.add(jet, u"secret"_s).ok());
        LocalWall::FromAFile same;
        same.kvm.address = u"http://192.0.2.6"_s;
        same.kvm.kind = u"jetkvm"_s;
        same.kvm.number = n;
        LocalWall::FromAFile bare;
        bare.kvm.address = u"http://192.0.2.7"_s;
        bare.kvm.kind = u"jetkvm"_s;
        LocalWall::FromAFile bareToo = bare;
        bareToo.kvm.address = u"http://192.0.2.8"_s;
        const QList<base::Result<QString>> took = wall.takeFromAFile({same, bare, bareToo});
        QCOMPARE(codeOf(took.at(0)), std::string("same_device"));
        QCOMPARE(took.at(0).error().message,
                 std::string("This says it is the same device as \"Rack JetKVM\", which is on the wall already "
                             "(192.0.2.5)"));
        QVERIFY(took.at(1).ok());
        QVERIFY(took.at(2).ok());
        QCOMPARE(wall.kvms().size(), 3);
    }

    // --- a JetKVM followed onto HTTPS by itself (M6 1b-iii) ------------------------------------------

    void aJetKvmAddedOverPlainHttpIsFollowedOntoHttpsOnceItOffersIt()
    {
        // As the web's (test_a_jetkvm_whose_https_mode_is_switched_on_later_is_followed): its owner turns HTTPS
        // on, and at its next due look the wall moves it there by itself: known by what it showed there, its
        // password saved anew for that address and certificate, still asked, and the certificate itself kept
        // after its next line there. Its details say so.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        std::optional<QUrl> offered;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.wayEveryMs = 300;
        paces.jetkvm.otherWay = [&offered, from = plain.address()](const QUrl& address) {
            return address == from ? offered : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        OwnKvm jet;
        jet.address = plain.address().toString();
        jet.name = u"Small one"_s;
        jet.kind = u"jetkvm"_s;
        const QString id = wall.add(jet, u"secret"_s).value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QTest::qWait(400);
        statusOf(wall);
        QCOMPARE(wall.kvms().constFirst().address, plain.address().toString());
        QVERIFY(!wall.followedOntoHttps(id));
        const QSignalSpy changed(&wall, &LocalWall::changed);
        offered = secure.address();
        QVERIFY(QTest::qWaitFor(
            [&] {
                statusOf(wall);
                return wall.kvms().constFirst().address == secure.address().toString();
            },
            15000));
        const OwnKvm now = wall.kvms().constFirst();
        QVERIFY(now.knownByCertificate);
        QCOMPARE(now.certificate, sha256Of("first"));
        QVERIFY(now.askedWhatItIs);
        QVERIFY(wall.wasAskedWhatItIs(id));
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", secure.address().toString(), sha256Of("first")));
        QCOMPARE(changed.count(), 1);
        QVERIFY(wall.followedOntoHttps(id));
        QVERIFY(QTest::qWaitFor(
            [&] {
                statusOf(wall);
                return settings.remembered().kvmCertificates.contains(id);
            },
            15000));
        QCOMPARE(settled(wall, id).state, u"online"_s);
        const core::Machine machine = wallOf(wall).machines.constFirst();
        QVERIFY(!machine.plain);
        QCOMPARE(machine.note,
                 u"Over HTTPS now, known by its certificate %1"_s.arg(core::kvm::fingerprint(sha256Of("first"))));
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(secure.signIns, 0);
    }

    void aJetKvmIsNotFollowedOntoHttpsWhereTheWallsOwnRulesSayNo()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"kept/settings.json"_s));
        MemorySecrets secrets;
        // Its HTTPS is where another KVM of the wall is.
        JetKvmByHand taken;
        JetKvmByHand takenThere(tls("first"));
        // A router's own page at its HTTPS: an answer that says no `isSetup`. (Its certificate no KVM of the wall
        // is known by: what keeps it from there is the answer alone, and not that it would be another's twin.)
        JetKvmByHand atARouter;
        const StandIn router(
            [](const Asked&) { return Answered(200, R"({"model":"router"})"_ba, "application/json"_ba); },
            tls("twin-two"));
        // An address with a port of its own: it has no other way (kvm::otherWayOf).
        JetKvmByHand withItsPort;
        // Its HTTPS offered while the settings cannot be written.
        JetKvmByHand unwritten;
        JetKvmByHand unwrittenThere(tls("twin-one"));
        std::optional<QUrl> unwrittenOffers;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.wayEveryMs = 300;
        paces.jetkvm.otherWay = [&](const QUrl& address) -> std::optional<QUrl> {
            if (address == taken.address()) {
                return takenThere.address();
            }
            if (address == atARouter.address()) {
                return QUrl(router.address());
            }
            if (address == unwritten.address()) {
                return unwrittenOffers;
            }
            return core::kvm::otherWayOf(address);
        };
        LocalWall wall(settings, secrets, paces);
        const auto put = [&](const QUrl& address, const char* certificate = nullptr) {
            OwnKvm jet = certificate == nullptr ? OwnKvm{} : knownAt(address.toString(), sha256Of(certificate));
            jet.address = address.toString();
            jet.kind = u"jetkvm"_s;
            return wall.add(jet, u"secret"_s).value();
        };
        const QString takenId = put(taken.address());
        // (Known by another certificate than the one shown there: what keeps the first from that place is the
        // place alone, and not that it would be this one's twin.)
        const QString thereId = put(takenThere.address(), "second");
        const QString routerId = put(atARouter.address());
        const QString portId = put(withItsPort.address());
        const QString unwrittenId = put(unwritten.address());
        for (const QString& id : {takenId, routerId, portId, unwrittenId}) {
            QCOMPARE(settled(wall, id).state, u"online"_s);
        }
        QCOMPARE(settled(wall, thereId).state, u"error"_s);
        const QList<OwnKvm> before = wall.kvms();
        const QByteArray unwrittenKept = secrets.all.value(LocalWall::secretOf(unwrittenId));
        // The settings cannot be written (where their folder was, a file is), and the last offers its HTTPS.
        QVERIFY(QDir(folder.filePath(u"kept"_s)).removeRecursively());
        QFile inTheWay(folder.filePath(u"kept"_s));
        QVERIFY(inTheWay.open(QIODevice::WriteOnly));
        inTheWay.close();
        unwrittenOffers = unwrittenThere.address();
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < 2000) {
            statusOf(wall);
            QTest::qWait(100);
        }
        QVERIFY(unwrittenThere.asked("/device/status") >= 1);
        QVERIFY(router.asked.size() >= 1);
        QCOMPARE(wall.kvms(), before);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(unwrittenId)), unwrittenKept);
        QCOMPARE(withItsPort.stand->notHttp, 0);
        for (const QString& id : {takenId, routerId, portId, unwrittenId}) {
            QVERIFY(!wall.followedOntoHttps(id));
        }
        // Once they can be written: followed at its next due look.
        QVERIFY(QFile::remove(folder.filePath(u"kept"_s)));
        QVERIFY(QDir(folder.path()).mkdir(u"kept"_s));
        QVERIFY(QTest::qWaitFor(
            [&] {
                statusOf(wall);
                return wall.followedOntoHttps(unwrittenId);
            },
            15000));
        const auto moved = std::ranges::find_if(wall.kvms(), [&](const OwnKvm& kvm) { return kvm.id == unwrittenId; });
        QVERIFY(moved != wall.kvms().end());
        QCOMPARE(moved->address, unwrittenThere.address().toString());
        QCOMPARE(moved->certificate, sha256Of("twin-one"));
        QCOMPARE(wall.kvms().at(0).address, taken.address().toString());
        QCOMPARE(wall.kvms().at(2).address, atARouter.address().toString());
        QCOMPARE(wall.kvms().at(3).address, withItsPort.address().toString());
    }

    void aJetKvmIsNotFollowedOntoHttpsWhereItIsAnotherOfTheWall()
    {
        // From the reading of 1b-iii (MEDIUM): its HTTPS answers with the certificate another JetKVM of the wall
        // is known by (its name now reaches that one: an address handed out anew, a port forward). That is the
        // other one: not followed there, and this one's kept password does not go to it. Nor by change().
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        const core::testing::NobodyThere nobody;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.wayEveryMs = 300;
        paces.jetkvm.otherWay = [from = plain.address(), to = secure.address()](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        OwnKvm other = knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), sha256Of("first"));
        other.kind = u"jetkvm"_s;
        other.name = u"The other"_s;
        const QString otherId = wall.add(other, u"its own"_s).value();
        OwnKvm jet;
        jet.address = plain.address().toString();
        jet.kind = u"jetkvm"_s;
        const QString id = wall.add(jet, u"secret"_s).value();
        // (From the second reading of the twins, L4: nothing asks a JetKVM for a session at a look, so its sign-ins
        // say nothing. Its kept password is not so much as read from its adding on: the driver it was added with
        // has it, and the one made where it is held back has none.)
        int readsOfIt = 0;
        secrets.onRead = [&readsOfIt, secret = LocalWall::secretOf(id)](const QString& name) {
            readsOfIt += name == secret ? 1 : 0;
        };
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < 1500) {
            statusOf(wall);
            QTest::qWait(100);
        }
        QVERIFY(secure.asked("/device/status") >= 1);
        QCOMPARE(wall.kvms().at(1).address, plain.address().toString());
        QVERIFY(!wall.followedOntoHttps(id));
        QCOMPARE(secure.signIns, 0);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", plain.address().toString()));
        // (From the reading of the twins, MEDIUM.) What answers the plain way at its address may be that other one
        // too, whose HTTPS proves it: no password goes there in the clear. Held back, said on its card, and not
        // asked what it is until its HTTPS shows that other one no more. (Where only its port 443 was forwarded to
        // the other one, the real one is held back as well until somebody looks: the safe side.)
        QVERIFY(QTest::qWaitFor([&] { return statusOf(wall).value(id).errorCode == u"same_device"_s; }, 15000));
        QVERIFY(!wall.wasAskedWhatItIs(id));
        watch.restart();
        while (watch.elapsed() < 3000) {
            statusOf(wall);
            QTest::qWait(100);
        }
        secrets.onRead = nullptr;
        QCOMPARE(readsOfIt, 0);
        QCOMPARE(codeOf(wall.signerIn(id, plain.address(), u"admin"_s, quickDrivers())), std::string("not_asked_yet"));
        QCOMPARE(plain.signIns, 0);
        static_cast<void>(otherId);
        // And change() with that exception: refused in the same words.
        core::KvmProbe::Found there;
        there.kind = core::KvmProbe::Kind::JetKvm;
        there.shown = sha256Of("first");
        const std::optional<OwnKvm> onto = LocalWall::ontoHttps(wall.kvms().at(1), there, paces.jetkvm.otherWay);
        if (!onto) {
            QFAIL("Its own HTTPS was not where the seam has it.");
        }
        const base::Result<void> changed = wall.change(id, *onto, std::nullopt, there);
        QCOMPARE(codeOf(changed), std::string("same_device"));
        QCOMPARE(changed.error().message,
                 std::string("This is the same device as \"The other\", which is on the wall already (127.0.0.1:") +
                     std::to_string(nobody.port()) + ")");
    }

    void aJetKvmWhoseHttpsIsAnotherOfTheWallIsHeldBackAtEveryAsking()
    {
        // From the reading of the twins (MEDIUM): its asking finds its HTTPS showing the certificate another KVM of
        // the wall is known by, and its plain way a JetKVM. That asking did not find it: "same_device" on its card,
        // and no password; and while that lasts, every asking asks its HTTPS again, whatever the way's pace.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        const core::testing::NobodyThere nobody;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [from = plain.address(), to = secure.address()](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        OwnKvm other = knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), sha256Of("first"));
        other.kind = u"jetkvm"_s;
        other.name = u"The other"_s;
        QVERIFY(wall.add(other, u"its own"_s).ok());
        LocalWall::FromAFile one;
        one.kvm.address = plain.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = wall.takeFromAFile({one}).constFirst().value();
        QVERIFY(QTest::qWaitFor([&] { return statusOf(wall).value(id).errorCode == u"same_device"_s; }, 15000));
        QVERIFY(!wall.wasAskedWhatItIs(id));
        // Its card says why, and the deliberate acts there are for it, in plain words (from the third reading of the
        // twins: a device that is well may be held so for as long as what answers at its HTTPS shows the other one's
        // certificate, and no act of the program's sends its password in the clear beside that doubt).
        QCOMPARE(statusOf(wall).value(id).errorMessage,
                 u"This may be the same device as \"The other\", which is on the wall already (127.0.0.1:%1): its "
                 "HTTPS showed the certificate that one is known by, and no password is sent here while that lasts. "
                 "If it is that device, one of the two can go: remove this one or that one. If it is another device, "
                 "remove this one and add it again, or turn on HTTPS on the JetKVM and change its address here to "
                 "https://."_s.arg(nobody.port()));
        const int askedOverHttps = secure.asked("/device/status");
        // (And meanwhile its kept password is not so much as read: no asking finds it, and no driver of it is made
        // with one. Its HTTPS asked only at the way's pace, an asking would find its plain way, and its driver read
        // the password, before that driver's own asking of its HTTPS let it go again.)
        int readsOfIt = 0;
        secrets.onRead = [&readsOfIt, secret = LocalWall::secretOf(id)](const QString& name) {
            readsOfIt += name == secret ? 1 : 0;
        };
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < 5000) {
            statusOf(wall);
            QTest::qWait(100);
        }
        secrets.onRead = nullptr;
        QCOMPARE(readsOfIt, 0);
        QVERIFY(secure.asked("/device/status") > askedOverHttps);
    }

    void aHeldBackJetKvmStaysHeldWhereItsHttpsAnswersLateOrUnwell_data()
    {
        // How its HTTPS answers once it is held back: 1 past its time, 2 a 503, 3 not at all, though something
        // takes the line (nothing shown: in doubt, something listens).
        QTest::addColumn<int>("then");
        QTest::newRow("an answer past its time") << 1;
        QTest::newRow("a 503") << 2;
        QTest::newRow("a line taken and nothing said") << 3;
    }

    void aHeldBackJetKvmStaysHeldWhereItsHttpsAnswersLateOrUnwell()
    {
        // From the second reading of the twins (MEDIUM): held back, its HTTPS (the other one of the wall) answers
        // otherwise than a JetKVM just then, a moment slow or unwell, showing the certificate the other one is known
        // by: no sign that the other one is gone. Its card still says so, and its kept password is not so much as
        // read. And so after a start: the hold is kept with it, and which KVM it is held for (from the third reading
        // of the twins, L2), so that it is held from the start also where its HTTPS shows nothing at all now (in
        // doubt). A sign lets go of it then, and of what was kept of it.
        QFETCH(int, then);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        JetKvmByHand plain;
        int mode = 0;
        const StandIn secure(
            [&mode](const Asked&) {
                if (mode == 2) {
                    return Answered(503, R"({"error":"Service Unavailable"})"_ba, "application/json"_ba);
                }
                Answered status(200, R"({"isSetup":true})"_ba, "application/json"_ba);
                status.afterMs = mode == 1 ? 4000 : 0;
                return status;
            },
            tls("first"));
        const NeverSpeaks silent;
        // (A sign: a certificate no other KVM of the wall is known by, on an answer that is no JetKVM's.)
        const StandIn another(
            [](const Asked&) { return Answered(503, R"({"error":"Service Unavailable"})"_ba, "application/json"_ba); },
            tls("second"));
        const core::testing::NobodyThere nobody;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [from = plain.address(), to = QUrl(secure.address()), &mode,
                                 elsewhere = QUrl(silent.address(true)),
                                 aSign = QUrl(another.address())](const QUrl& address) -> std::optional<QUrl> {
            if (address != from) {
                return std::nullopt;
            }
            return mode == 3 ? elsewhere : mode == 4 ? aSign : to;
        };
        QString id;
        QString otherId;
        int readsOfIt = 0;
        const auto counted = [&readsOfIt, &id](const QString& name) {
            readsOfIt += name == LocalWall::secretOf(id) ? 1 : 0;
        };
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, paces);
            OwnKvm other = knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), sha256Of("first"));
            other.kind = u"jetkvm"_s;
            otherId = wall.add(other, u"its own"_s).value();
            LocalWall::FromAFile one;
            one.kvm.address = plain.address().toString();
            one.kvm.kind = u"jetkvm"_s;
            one.password = u"secret"_s;
            id = wall.takeFromAFile({one}).constFirst().value();
            QVERIFY(QTest::qWaitFor([&] { return statusOf(wall).value(id).errorCode == u"same_device"_s; }, 30000));
            const int asked = secure.count("/device/status");
            mode = then;
            secrets.onRead = counted;
            QElapsedTimer watch;
            watch.start();
            while (watch.elapsed() < 8000) {
                QCOMPARE(statusOf(wall).value(id).errorCode, u"same_device"_s);
                QTest::qWait(100);
            }
            secrets.onRead = nullptr;
            QCOMPARE(readsOfIt, 0);
            // (Asked again meanwhile, waited for: on the loaded slow cores 8 s was not always enough for it.)
            QTRY_VERIFY_WITH_TIMEOUT(then == 3 ? silent.lines > 0 : secure.count("/device/status") > asked, 20000);
            QVERIFY(!wall.wasAskedWhatItIs(id));
        }
        // The next start, its HTTPS answering as it does now: held from the start, for that other one.
        QCOMPARE(core::Settings(file).remembered().kvmsHeldForATwin.value(id), otherId);
        core::Settings settings(file);
        secrets.onRead = counted;
        LocalWall wall(settings, secrets, paces);
        QCOMPARE(statusOf(wall).value(id).errorCode, u"same_device"_s);
        const auto askedThere = [&] {
            return then == 3 ? silent.lines : static_cast<int>(secure.count("/device/status"));
        };
        const int askedBefore = askedThere();
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < 3000) {
            QCOMPARE(statusOf(wall).value(id).errorCode, u"same_device"_s);
            QTest::qWait(100);
        }
        secrets.onRead = nullptr;
        QCOMPARE(readsOfIt, 0);
        QCOMPARE(plain.signIns, 0);
        // (Asked meanwhile: what holds it is no asking that was left out.)
        QTRY_VERIFY_WITH_TIMEOUT(askedThere() > askedBefore, 20000);
        // A sign lets go of it: on the wall as the JetKVM its plain way answers, and what was kept of the hold goes.
        mode = 4;
        QVERIFY2(QTest::qWaitFor([&] { return statusOf(wall).value(id).state == u"online"_s; }, 30000),
                 qPrintable(tileOf(statusOf(wall).value(id))));
        QVERIFY(wall.wasAskedWhatItIs(id));
        QTRY_VERIFY_WITH_TIMEOUT(!core::Settings(file).remembered().kvmsHeldForATwin.contains(id), 30000);
    }

    void aHoldGoesWithTheKvmItIsHeldFor()
    {
        // From the third reading of the twins (the coordinator's word on a choice of mine): the KVM a hold is for is
        // taken off the wall. No KVM of the wall is known by that certificate any more, and a look anew would find no
        // twin either: the hold goes with it, from what is remembered at once, and at its next asking the KVM is found
        // by its plain way, whatever its HTTPS still shows.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        const StandIn secure(
            [](const Asked&) { return Answered(404, R"({"error":"Not Found"})"_ba, "application/json"_ba); },
            tls("first"));
        const core::testing::NobodyThere nobody;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [from = plain.address(), to = QUrl(secure.address())](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        OwnKvm other = knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), sha256Of("first"));
        other.kind = u"jetkvm"_s;
        const QString otherId = wall.add(other, u"its own"_s).value();
        LocalWall::FromAFile one;
        one.kvm.address = plain.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = wall.takeFromAFile({one}).constFirst().value();
        QVERIFY(QTest::qWaitFor([&] { return statusOf(wall).value(id).errorCode == u"same_device"_s; }, 15000));
        QCOMPARE(settings.remembered().kvmsHeldForATwin.value(id), otherId);
        QCOMPARE(plain.signIns, 0);
        QVERIFY(wall.remove(otherId).ok());
        QVERIFY(!settings.remembered().kvmsHeldForATwin.contains(id));
        QVERIFY2(QTest::qWaitFor([&] { return statusOf(wall).value(id).state == u"online"_s; }, 15000),
                 qPrintable(tileOf(statusOf(wall).value(id))));
        QVERIFY(wall.wasAskedWhatItIs(id));
    }

    void aKvmTakenOffTheWallHasWhatWasKeptOfItsSignInsCleared()
    {
        // a9: its wrong-password counts and their waits go with it, whoever signed in and with whatever password (in
        // memory, and in the file kept beside the settings at its next writing: test_pacing_file); another's stay.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::DriverPaces paces = quickDrivers();
        auto pacing = std::make_shared<core::SignInPacing>();
        paces.kvmd.pacing = pacing;
        paces.jetkvm.pacing = pacing;
        LocalWall wall(settings, secrets, paces);
        const MadeUpKvm kvm;
        const QString id = wall.add(at(kvm), u"secret"_s).value();
        const QUrl address(wall.kvms().constFirst().address);
        const core::SignInPacing::Mark refused = core::SignInPacing::what(u"login", address, u"admin"_s, u"wrong"_s);
        const core::SignInPacing::Mark older = core::SignInPacing::what(u"login", address, u"root"_s, u"older"_s);
        const core::SignInPacing::Mark elsewhere =
            core::SignInPacing::what(u"login", QUrl(u"https://192.0.2.30"_s), u"admin"_s, u"wrong"_s);
        for (const core::SignInPacing::Mark& mark : {refused, older, elsewhere}) {
            pacing->saidNo(mark, {});
        }
        QVERIFY(wall.remove(id).ok());
        QCOMPARE(pacing->times(refused), 0);
        QCOMPARE(pacing->times(older), 0);
        QCOMPARE(pacing->times(elsewhere), 1);
    }

    void aJetKvmOfTheWallIsKnownByTheFirstNumberItGives()
    {
        // J5: a JetKVM of the wall with no number kept (one from a file without it, one added before it was kept): its
        // driver takes the first one seen, at its first connection (a card's), and the wall writes it to the KVM's
        // record, for its next driver and the next start.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        MemorySecrets secrets;
        JetKvmByHand jet;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [](const QUrl&) { return std::optional<QUrl>(); };
        LocalWall wall(settings, secrets, paces);
        OwnKvm kvm;
        kvm.address = jet.address().toString();
        kvm.kind = u"jetkvm"_s;
        const QString id = wall.add(kvm, u"secret"_s).value();
        const QString its =
            u"jetkvm:"_s +
            QString::fromLatin1(QCryptographicHash::hash(jet.deviceId.toString().toUtf8(), QCryptographicHash::Sha256)
                                    .toHex()
                                    .first(32));
        QVERIFY2(QTest::qWaitFor(
                     [&] {
                         static_cast<void>(answerTo<core::Snapshot>(
                             [&](auto done) { wall.askSnapshot(id, 400, 1, std::move(done)); }));
                         return wall.kvms().constFirst().number == its;
                     },
                     15000),
                 qPrintable(wall.kvms().constFirst().number));
        QTRY_COMPARE_WITH_TIMEOUT(core::Settings(file).remembered().kvms.value(0).number, its, 10000);
    }

    void aJetKvmOfTheWallThatIsAnotherNowIsOfferedNothing()
    {
        // J5: the number it is known by is handed to its driver; another JetKVM at its address is offered nothing, and
        // its card and its look say so. What is kept of it stays.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand jet;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [](const QUrl&) { return std::optional<QUrl>(); };
        LocalWall wall(settings, secrets, paces);
        OwnKvm kvm;
        kvm.address = jet.address().toString();
        kvm.kind = u"jetkvm"_s;
        kvm.number = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        const QString id = wall.add(kvm, u"secret"_s).value();
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto card =
                    answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(id, 400, 1, std::move(done)); });
                return !card.ok() && card.error().code == "another_machine";
            },
            15000));
        QVERIFY2(QTest::qWaitFor([&] { return statusOf(wall).value(id).errorCode == u"another_machine"_s; }, 15000),
                 qPrintable(tileOf(statusOf(wall).value(id))));
        QCOMPARE(jet.lineAskings, 0);
        QCOMPARE(jet.offersHeard.size(), 0);
        QCOMPARE(wall.kvms().constFirst().number, kvm.number);
    }

    void aHoldIsLetGoOfWhereItsHttpsShowsNoOtherOfTheWall_data()
    {
        QTest::addColumn<bool>("listens"); // something answers there, showing another certificate; or nothing listens
        QTest::newRow("another certificate") << true;
        QTest::newRow("nothing listens") << false;
    }

    void aHoldIsLetGoOfWhereItsHttpsShowsNoOtherOfTheWall()
    {
        // From the second reading of the twins: held back, its HTTPS shows a certificate no other KVM of the wall is
        // known by, or nothing listens there: the other one is not there, and the hold is let go of. Its plain way
        // answers as a JetKVM: on the wall as one. And its driver asks its HTTPS once more at its first look, the
        // asking that let go of the hold notwithstanding.
        QFETCH(bool, listens);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        const StandIn unwell(
            [](const Asked&) { return Answered(503, R"({"error":"Service Unavailable"})"_ba, "application/json"_ba); },
            tls("second"));
        const core::testing::NobodyThere nobody;
        bool held = true;
        const QUrl afterwards(listens ? unwell.address() : u"https://127.0.0.1:%1"_s.arg(nobody.port()));
        core::DriverPaces paces = quickDrivers();
        // (Room to be turned away: Windows tries a refused line again for a few seconds before it says so, and an
        // answer's quiet counts from the call. The program's own times give it that room.)
        paces.jetkvm.connectWithinMs = core::kvm::letInWithinMs;
        paces.jetkvm.quietMs = core::kvm::quietMs;
        paces.jetkvm.otherWay = [from = plain.address(), to = secure.address(), &held,
                                 afterwards](const QUrl& address) {
            return address == from ? std::optional<QUrl>(held ? to : afterwards) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        OwnKvm other = knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), sha256Of("first"));
        other.kind = u"jetkvm"_s;
        QVERIFY(wall.add(other, u"its own"_s).ok());
        LocalWall::FromAFile one;
        one.kvm.address = plain.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = wall.takeFromAFile({one}).constFirst().value();
        QVERIFY(QTest::qWaitFor([&] { return statusOf(wall).value(id).errorCode == u"same_device"_s; }, 15000));
        held = false;
        QVERIFY2(QTest::qWaitFor([&] { return statusOf(wall).value(id).state == u"online"_s; }, 15000),
                 qPrintable(tileOf(statusOf(wall).value(id))));
        QVERIFY(wall.wasAskedWhatItIs(id));
        if (listens) {
            // (The asking's question, and the driver's at its first look.)
            QTRY_VERIFY_WITH_TIMEOUT(unwell.count("/device/status") >= 2, 10000);
        }
    }

    void aJetKvmsViewerSignsInAnewOnlyOnceItsHttpsWasLookedAt()
    {
        // From the reading of 2g/2h (M1): the wall's JetKVM driver signs in for its viewer (a card that asks, a
        // window), and again where its session is refused. A plain JetKVM's HTTPS is looked at on every such sign-in
        // before its password goes: where it shows the certificate another KVM of the wall is known by, that one may be
        // what answers here now (an address handed out anew), and nothing is sent; the KVM is held back.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        const core::testing::NobodyThere nobody;
        bool twinThere = false;
        core::DriverPaces paces = quickDrivers();
        // (Its driver's own look at its other way comes at its first look and then not for ten minutes, whether it
        // answers or not: what finds the other one below is the sign-in's look, and nothing else. From the third
        // reading of the twins, L6.)
        paces.jetkvm.wayEveryMs = 600'000;
        paces.jetkvm.wayWhenDownMs = 600'000;
        paces.jetkvm.otherWay = [from = plain.address(), to = secure.address(),
                                 off = QUrl(u"https://127.0.0.1:%1"_s.arg(nobody.port())),
                                 &twinThere](const QUrl& address) {
            return address == from ? std::optional<QUrl>(twinThere ? to : off) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        OwnKvm other = knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), sha256Of("first"));
        other.kind = u"jetkvm"_s;
        QVERIFY(wall.add(other, u"its own"_s).ok());
        OwnKvm jet;
        jet.address = plain.address().toString();
        jet.kind = u"jetkvm"_s;
        const QString id = wall.add(jet, u"secret"_s).value();
        // Its first look over (its other way asked then, where nothing answered), and nothing asked of the other one.
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QCOMPARE(secure.asked("/device/status"), 0);
        // Its address handed to the other one since it was added (its HTTPS shows that one's certificate), and nothing
        // looked there yet: its card asks, its driver is to sign in for its viewer. Its HTTPS is looked at on that very
        // sign-in, and its password goes nowhere. (Every later sign-in of its driver, a session refused among them,
        // goes the same road: test_jetkvm_device, itsHttpsIsLookedAtBeforeEverySignInWhereThatIsAsked.)
        twinThere = true;
        const auto cardAsks = [&wall, &id] {
            wall.askSnapshot(id, 320, 0, [](const base::Result<core::Snapshot>&) {});
        };
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < 3000) {
            cardAsks();
            QTest::qWait(100);
        }
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(secure.signIns, 0);
        // (Held by that look: nothing else asked its HTTPS before the hold, and the card's asking says nothing of it.
        // Once held, the wall asks it again at its own askings.)
        QVERIFY(secure.asked("/device/status") >= 1);
        QVERIFY(!wall.wasAskedWhatItIs(id));
        QVERIFY(QTest::qWaitFor([&] { return statusOf(wall).value(id).errorCode == u"same_device"_s; }, 15000));
        QCOMPARE(plain.signIns, 0);
    }

    void aShowHereIsAnsweredAndTheKvmNotBusyWhereItIsHeldBackMeanwhile()
    {
        // From the reading of 2g/2h (M2): "Show here" waits for its connection (here: a signalling line taken and never
        // answered); a look meanwhile finds its HTTPS showing another KVM of the wall: held back, its driver let go of.
        // The press is answered once, in the wall's words for why (not left to the driver), and the KVM is busy no
        // more: it can be taken off.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        plain.linesSayNothing = true;
        JetKvmByHand secure(tls("first"));
        const core::testing::NobodyThere nobody;
        bool twinThere = false;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.wayEveryMs = 300;
        paces.jetkvm.otherWay = [from = plain.address(), to = secure.address(),
                                 off = QUrl(u"https://127.0.0.1:%1"_s.arg(nobody.port())),
                                 &twinThere](const QUrl& address) {
            return address == from ? std::optional<QUrl>(twinThere ? to : off) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        OwnKvm other = knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), sha256Of("first"));
        other.kind = u"jetkvm"_s;
        QVERIFY(wall.add(other, u"its own"_s).ok());
        OwnKvm jet;
        jet.address = plain.address().toString();
        jet.kind = u"jetkvm"_s;
        const QString id = wall.add(jet, u"secret"_s).value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        QList<base::Result<core::Done>> told;
        wall.act(id, core::Action::plain(u"resume"_s),
                 [&told](base::Result<core::Done> what) { told.append(std::move(what)); });
        QTRY_VERIFY_WITH_TIMEOUT(plain.lineAskings >= 1, 15000);
        QVERIFY(told.isEmpty());
        twinThere = true;
        QVERIFY(QTest::qWaitFor([&] { return statusOf(wall).value(id).errorCode == u"same_device"_s; }, 15000));
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 5000);
        QCOMPARE(codeOf(told.constFirst()), std::string("same_device"));
        QTest::qWait(300);
        QCOMPARE(told.size(), 1);
        QVERIFY(wall.remove(id).ok());
    }

    void aJetKvmsDriverHoldsItBackWhereItsHttpsShowsAnotherOfTheWallAnsweringOtherwise()
    {
        // From the second reading of the twins (MEDIUM, the driver's road): a JetKVM of the wall over plain HTTP, with
        // its password; its driver asks its HTTPS, which answers 503 showing the certificate another KVM of the wall is
        // known by. What answers its plain way may be that other one too: held back, its driver one with no password.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        const StandIn secure(
            [](const Asked&) { return Answered(503, R"({"error":"Service Unavailable"})"_ba, "application/json"_ba); },
            tls("first"));
        const core::testing::NobodyThere nobody;
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.wayEveryMs = 300;
        paces.jetkvm.otherWay = [from = plain.address(), to = QUrl(secure.address())](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        OwnKvm other = knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), sha256Of("first"));
        other.kind = u"jetkvm"_s;
        QVERIFY(wall.add(other, u"its own"_s).ok());
        OwnKvm jet;
        jet.address = plain.address().toString();
        jet.kind = u"jetkvm"_s;
        const QString id = wall.add(jet, u"secret"_s).value();
        int readsOfIt = 0;
        secrets.onRead = [&readsOfIt, secret = LocalWall::secretOf(id)](const QString& name) {
            readsOfIt += name == secret ? 1 : 0;
        };
        QVERIFY(QTest::qWaitFor([&] { return statusOf(wall).value(id).errorCode == u"same_device"_s; }, 15000));
        QVERIFY(!wall.wasAskedWhatItIs(id));
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < 3000) {
            QCOMPARE(statusOf(wall).value(id).errorCode, u"same_device"_s);
            QTest::qWait(100);
        }
        secrets.onRead = nullptr;
        QCOMPARE(readsOfIt, 0);
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(codeOf(wall.signerIn(id, plain.address(), u"admin"_s, quickDrivers())), std::string("not_asked_yet"));
        QCOMPARE(statusOf(wall).value(id).errorCode, u"same_device"_s);
        QVERIFY(!wall.wasAskedWhatItIs(id));
        QCOMPARE(plain.signIns, 0);
    }

    void aKeptPasswordIsNotSignedInWithOntoTheHttpsOfAnotherOfTheWall()
    {
        // From the reading of the twins (L2): signerIn's one exception, onto a JetKVM's own HTTPS, is not made onto
        // what shows the certificate another KVM of the wall is known by. (Nothing is asked of these.)
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        OwnKvm other = knownAt(u"https://192.0.2.9"_s, sha256Of("first"));
        other.kind = u"jetkvm"_s;
        other.name = u"The other"_s;
        QVERIFY(wall.add(other, u"its own"_s).ok());
        OwnKvm jet;
        jet.address = u"http://192.0.2.5"_s;
        jet.kind = u"jetkvm"_s;
        const QString id = wall.add(jet, u"secret"_s).value();
        core::KvmProbe::Found there;
        there.kind = core::KvmProbe::Kind::JetKvm;
        there.shown = sha256Of("first");
        const auto signer = wall.signerIn(id, QUrl(u"https://192.0.2.5"_s), u"admin"_s, quickDrivers(), there);
        QCOMPARE(codeOf(signer), std::string("same_device"));
        there.shown = sha256Of("second");
        QVERIFY(wall.signerIn(id, QUrl(u"https://192.0.2.5"_s), u"admin"_s, quickDrivers(), there).ok());
    }

    void theOneExceptionIsNotMadeOntoAKvmOfTheWallThatGivesItsNumber()
    {
        // From the reading of the twins (L4): the number half of change()'s twin check, with its one exception.
        // Two cards of one JetKVM, as a file of an earlier build could hold them.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const QString n = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        OwnKvm other;
        other.address = u"http://192.0.2.9"_s;
        other.kind = u"jetkvm"_s;
        other.name = u"The other"_s;
        other.number = n;
        QVERIFY(wall.add(other, u"its own"_s).ok());
        OwnKvm jet;
        jet.address = u"http://192.0.2.5"_s;
        jet.kind = u"jetkvm"_s;
        jet.number = n;
        const QString id = wall.add(jet, u"secret"_s).value();
        core::KvmProbe::Found there;
        there.kind = core::KvmProbe::Kind::JetKvm;
        there.shown = sha256Of("second");
        const std::optional<OwnKvm> onto = LocalWall::ontoHttps(wall.kvms().at(1), there, core::kvm::otherWayOf);
        if (!onto) {
            QFAIL("It was not taken onto HTTPS.");
        }
        const base::Result<void> changed = wall.change(id, *onto, std::nullopt, there);
        QCOMPARE(codeOf(changed), std::string("same_device"));
        QVERIFY(LocalWall::saysItIsTheSame(changed.error()));
        QCOMPARE(wall.kvms().at(1).address, u"http://192.0.2.5"_s);
    }

    void ofAFilesKvmsThatGiveOneNumberTheFirstIsTakenAndTheOtherKeptBesideItWhereTheFileSaysSo()
    {
        // From the reading of the twins (L4, L3): between the rows of one file, by the number; and the web's keep_both,
        // which a file carries for one that somebody said is to stay beside its twin.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const QString n = u"jetkvm:0123456789abcdef0123456789abcdef"_s;
        const auto jetAt = [&n](const QString& address, bool keptBeside) {
            LocalWall::FromAFile one;
            one.kvm.address = address;
            one.kvm.kind = u"jetkvm"_s;
            one.kvm.number = n;
            one.kvm.keptBesideItsTwin = keptBeside;
            return one;
        };
        const QList<base::Result<QString>> took =
            wall.takeFromAFile({jetAt(u"http://192.0.2.5"_s, false), jetAt(u"http://192.0.2.6"_s, false),
                                jetAt(u"http://192.0.2.7"_s, true)});
        QVERIFY(took.at(0).ok());
        QCOMPARE(codeOf(took.at(1)), std::string("same_device"));
        QVERIFY(LocalWall::saysItIsTheSame(took.at(1).error()));
        QVERIFY(took.at(2).ok());
        QCOMPARE(wall.kvms().size(), 2);
        QVERIFY(wall.kvms().at(1).keptBesideItsTwin);
        // Kept so from one run to the next.
        QTRY_VERIFY_WITH_TIMEOUT(settings.writings() >= 1, 10000);
        const core::Settings again(folder.filePath(u"settings.json"_s));
        QCOMPARE(again.remembered().kvms.size(), 2);
        QVERIFY(!again.remembered().kvms.at(0).keptBesideItsTwin);
        QVERIFY(again.remembered().kvms.at(1).keptBesideItsTwin);
        // A certificate makes two one whatever is kept beside what.
        OwnKvm one = knownAt(u"https://192.0.2.10"_s, sha256Of("first"));
        one.kind = u"jetkvm"_s;
        OwnKvm keptBeside = knownAt(u"https://192.0.2.11"_s, sha256Of("first"));
        keptBeside.kind = u"jetkvm"_s;
        keptBeside.keptBesideItsTwin = true;
        QVERIFY(LocalWall::twinOf({one}, keptBeside).has_value());
        OwnKvm byNumber;
        byNumber.number = n;
        OwnKvm byNumberKept = byNumber;
        byNumberKept.keptBesideItsTwin = true;
        QVERIFY(LocalWall::twinOf({byNumber}, byNumber).has_value());
        QVERIFY(!LocalWall::twinOf({byNumber}, byNumberKept).has_value());
        QVERIFY(!LocalWall::twinOf({byNumberKept}, byNumber).has_value());
    }

    void theAskingOfAPlainJetKvmAsksItsHttpsAtTheWaysPace()
    {
        // From the reading (L2): a JetKVM from a file that stays off is asked its HTTPS at its first asking,
        // and then no more often than wayWhenDownMs, however often its plain way is asked again.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        const core::testing::NobodyThere nobody;
        const StandIn router(
            [](const Asked&) { return Answered(200, R"({"model":"router"})"_ba, "application/json"_ba); },
            tls("second"));
        const QUrl plain(u"http://127.0.0.1:%1"_s.arg(nobody.port()));
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [plain, to = QUrl(router.address())](const QUrl& address) {
            return address == plain ? std::optional<QUrl>(to) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        LocalWall::FromAFile one;
        one.kvm.address = plain.toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = wall.takeFromAFile({one}).constFirst().value();
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < 6000) {
            statusOf(wall);
            QTest::qWait(100);
        }
        QCOMPARE(router.asked.size(), 1);
        QVERIFY(!wall.wasAskedWhatItIs(id));
    }

    void aDriverMadeAtTheWallsAskingDoesNotAskItsHttpsAgainAtOnce()
    {
        // From the reading (L2): the asking asked its HTTPS a moment ago; the driver it makes for it does not ask
        // again at its first look, but at its own pace.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        const StandIn router(
            [](const Asked&) { return Answered(200, R"({"model":"router"})"_ba, "application/json"_ba); },
            tls("second"));
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [from = plain.address(), to = QUrl(router.address())](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        LocalWall::FromAFile one;
        one.kvm.address = plain.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = wall.takeFromAFile({one}).constFirst().value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        for (int look = 0; look < 4; ++look) {
            QTest::qWait(quick().freshMs + 50);
            statusOf(wall);
        }
        QCOMPARE(router.asked.size(), 1);
    }

    void aJetKvmFromAFileWhoseMoveFailsIsNotAskedForIt()
    {
        // From the reading (L3): its HTTPS answers as a JetKVM, and it is not moved there (another KVM of the wall
        // is at that place); its plain way answers as a kvmd. It is not asked: its kept password goes nowhere.
        // The KVM at that place is known by another certificate than the one its HTTPS shows now, so the place
        // alone refuses the move (not the twin rule, which a test of its own holds).
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        const MadeUpKvm kvmd;
        JetKvmByHand secure(tls("first"));
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [from = QUrl(kvmd.stand->address()), to = secure.address()](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        OwnKvm there = knownAt(secure.address().toString(), sha256Of("second"));
        there.kind = u"jetkvm"_s;
        QVERIFY(wall.add(there, u"its own"_s).ok());
        LocalWall::FromAFile one;
        one.kvm.address = kvmd.stand->address();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = wall.takeFromAFile({one}).constFirst().value();
        const core::Status status = settled(wall, id);
        QCOMPARE(status.errorCode, u"other_family"_s);
        QVERIFY(!wall.wasAskedWhatItIs(id));
        QCOMPARE(codeOf(wall.signerIn(id, QUrl(kvmd.stand->address()), u"admin"_s, quickDrivers())),
                 std::string("not_asked_yet"));
        QCOMPARE(kvmd.signIns, 0);
    }

    void aJetKvmIsFollowedOnlyFromWhereItIsToItsOwnHttpsAsItIsNow()
    {
        // From the reading (L3): an offer waits a turn of the loop; meanwhile the KVM may have been moved. It is
        // followed only where it is still at the address offered from, and onto the HTTPS of where it is now.
        class Following : public LocalWall {
        public:
            using LocalWall::followOntoHttps;
            using LocalWall::LocalWall;
        };
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        Following wall(settings, secrets, quickDrivers());
        OwnKvm jet;
        jet.address = u"http://192.0.2.5"_s;
        jet.kind = u"jetkvm"_s;
        const QString id = wall.add(jet, u"secret"_s).value();
        OwnKvm moved = wall.kvms().constFirst();
        moved.address = u"http://192.0.2.6"_s;
        QVERIFY(wall.change(id, moved, u"secret"_s).ok());
        core::KvmProbe::Found found;
        found.kind = core::KvmProbe::Kind::JetKvm;
        found.shown = sha256Of("first");
        // Offered from where it was, at the HTTPS of where it is now: not followed.
        QVERIFY(!movedBy(wall.followOntoHttps(id, QUrl(u"http://192.0.2.5"_s), QUrl(u"https://192.0.2.6"_s), found)));
        // Offered from where it is, at the HTTPS of where it was: not followed.
        QVERIFY(!movedBy(wall.followOntoHttps(id, QUrl(u"http://192.0.2.6"_s), QUrl(u"https://192.0.2.5"_s), found)));
        QCOMPARE(wall.kvms().constFirst().address, u"http://192.0.2.6"_s);
        QVERIFY(!wall.kvms().constFirst().knownByCertificate);
        // From where it is, onto its own HTTPS: followed.
        QVERIFY(movedBy(wall.followOntoHttps(id, QUrl(u"http://192.0.2.6"_s), QUrl(u"https://192.0.2.6"_s), found)));
        QCOMPARE(wall.kvms().constFirst().address, u"https://192.0.2.6"_s);
        QCOMPARE(wall.kvms().constFirst().certificate, sha256Of("first"));
    }

    void aJetKvmIsNotFollowedOntoAnotherOfTheWallThatWindowsVouchesForThere()
    {
        // From the third reading of the twins (L5): its HTTPS shows the certificate another KVM of the wall is known
        // by, and Windows trusts that certificate there, so that it would be known there by Windows' word and not by
        // that certificate. What shows it is that other one all the same (it has the key to it): not followed there,
        // and the wall hands out no sign-in with its kept password for there either.
        class Following : public LocalWall {
        public:
            using LocalWall::followOntoHttps;
            using LocalWall::LocalWall;
        };
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        const core::testing::NobodyThere plainWay;
        const core::testing::NobodyThere httpsWay;
        const core::testing::NobodyThere otherPlace;
        const QUrl from(u"http://127.0.0.1:%1"_s.arg(plainWay.port()));
        const QUrl to(u"https://127.0.0.1:%1"_s.arg(httpsWay.port()));
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [from, to](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        Following wall(settings, secrets, paces);
        OwnKvm other = knownAt(u"https://127.0.0.1:%1"_s.arg(otherPlace.port()), sha256Of("first"));
        other.kind = u"jetkvm"_s;
        other.name = u"The other"_s;
        QVERIFY(wall.add(other, u"its own"_s).ok());
        OwnKvm jet;
        jet.address = from.toString();
        jet.kind = u"jetkvm"_s;
        const QString id = wall.add(jet, u"secret"_s).value();
        core::KvmProbe::Found found;
        found.kind = core::KvmProbe::Kind::JetKvm;
        found.shown = sha256Of("first");
        found.trustedByWindows = true;
        QCOMPARE(codeOf(wall.followOntoHttps(id, from, to, found)), std::string("same_device"));
        QCOMPARE(wall.kvms().at(1).address, from.toString());
        QVERIFY(!wall.followedOntoHttps(id));
        QCOMPARE(codeOf(wall.signerIn(id, to, u"admin"_s, paces, found)), std::string("same_device"));
        // Nor saved there by change() with that exception: the wall's own look, the last that is asked.
        const std::optional<OwnKvm> onto = LocalWall::ontoHttps(wall.kvms().at(1), found, paces.jetkvm.otherWay);
        if (!onto) {
            QFAIL("Its own HTTPS was not where the seam has it.");
        }
        QVERIFY(!onto->knownByCertificate);
        QCOMPARE(codeOf(wall.change(id, *onto, std::nullopt, found)), std::string("same_device"));
        QCOMPARE(wall.kvms().at(1).address, from.toString());
    }

    void overHttpsNowIsSaidUntilItWasSeenAlsoAfterAStart()
    {
        // From the reading (L5): its details say it was moved onto HTTPS until whoever sits here has seen them,
        // whether or not the program was started anew in between.
        class Following : public LocalWall {
        public:
            using LocalWall::followOntoHttps;
            using LocalWall::LocalWall;
        };
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        QString id;
        const QString said =
            u"Over HTTPS now, known by its certificate %1"_s.arg(core::kvm::fingerprint(sha256Of("first")));
        {
            core::Settings settings(file);
            Following wall(settings, secrets, quickDrivers());
            OwnKvm jet;
            jet.address = u"http://192.0.2.5"_s;
            jet.kind = u"jetkvm"_s;
            id = wall.add(jet, u"secret"_s).value();
            core::KvmProbe::Found found;
            found.kind = core::KvmProbe::Kind::JetKvm;
            found.shown = sha256Of("first");
            QVERIFY(
                movedBy(wall.followOntoHttps(id, QUrl(u"http://192.0.2.5"_s), QUrl(u"https://192.0.2.5"_s), found)));
            QCOMPARE(wallOf(wall).machines.constFirst().note, said);
            QTRY_VERIFY_WITH_TIMEOUT(settings.writings() >= 2, 10000);
        }
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quickDrivers());
        QVERIFY(wall.followedOntoHttps(id));
        QCOMPARE(wallOf(wall).machines.constFirst().note, said);
        const QSignalSpy changed(&wall, &LocalWall::changed);
        wall.sawNote(id);
        QVERIFY(!wall.followedOntoHttps(id));
        QVERIFY(wallOf(wall).machines.constFirst().note.isEmpty());
        QCOMPARE(changed.count(), 1);
        wall.sawNote(id);
        QCOMPARE(changed.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!core::Settings(file).remembered().kvmsFollowedOntoHttps.contains(id), 10000);
    }

    void aJetKvmFromAFileOverPlainHttpThatOffersHttpsIsFollowedAtItsAsking()
    {
        // Its asking asks its HTTPS beside the three questions, and a JetKVM there is followed before any
        // driver has its password: that answer is the asking (design 3.5). Nothing but the questions went the
        // plain way.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        core::DriverPaces paces = quickDrivers();
        paces.jetkvm.otherWay = [from = plain.address(), to = secure.address()](const QUrl& address) {
            return address == from ? std::optional<QUrl>(to) : std::nullopt;
        };
        LocalWall wall(settings, secrets, paces);
        LocalWall::FromAFile one;
        one.kvm.address = plain.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QString id = wall.takeFromAFile({one}).constFirst().value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        const OwnKvm now = wall.kvms().constFirst();
        QCOMPARE(now.address, secure.address().toString());
        QCOMPARE(now.certificate, sha256Of("first"));
        QVERIFY(wall.wasAskedWhatItIs(id));
        QVERIFY(wall.followedOntoHttps(id));
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", secure.address().toString(), sha256Of("first")));
        QVERIFY(heardOnlyTheQuestions(*plain.stand));
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(secure.signIns, 0);
        for (const Asked& asked : std::as_const(secure.stand->asked)) {
            QCOMPARE(asked.path, QByteArray(jetKvmsQuestion));
            QVERIFY(asked.headers.value("cookie").isEmpty());
        }
    }

    // --- a JetKVM, the second family --------------------------------------------------------------

    void aJetKvmOfTheWallIsLookedAtByItsOwnDriverAndIsAskedNothingElseYet()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        JetKvmByHand jet;
        OwnKvm own;
        own.address = jet.address().toString();
        own.name = u"Small one"_s;
        own.kind = u"jetkvm"_s;
        const base::Result<QString> added = wall.add(own, u"secret"_s);
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        const QString& id = added.value();
        QCOMPARE(wallOf(wall).machines.constFirst().kind, u"jetkvm"_s);
        // Looked at as a JetKVM is: its status, which anybody may ask, and nothing else. Never as a
        // kvmd, and with no sign-in for it.
        const core::Status status = settled(wall, id);
        QCOMPARE(status.state, u"online"_s);
        QCOMPARE(status.model, u"JetKVM"_s);
        QVERIFY(!jet.stand->asked.isEmpty());
        for (const Asked& one : std::as_const(jet.stand->asked)) {
            QCOMPARE(one.path, QByteArray(jetKvmsQuestion));
            QVERIFY(one.headers.value("cookie").isEmpty());
        }
        QCOMPARE(jet.signIns, 0);
        // What a JetKVM cannot do is refused there, and nothing goes to it. (What it does goes over its connection,
        // test_jetkvm_actions.cpp; its picture is its one viewer's since M6 2g, which a card that asks connects:
        // aJetKvmThatSteppedAsideStaysAside...)
        const qsizetype heard = jet.stand->asked.size();
        QCOMPARE(doneBy(wall, id, core::Action::plain(u"locate_on"_s)).error().code, std::string("invalid"));
        QCOMPARE(jet.stand->asked.size(), heard);
        QCOMPARE(jet.signIns, 0);
        // What may have been done is a JetKVM's word: what got no answer in time may, what found its line
        // gone was not sent.
        QVERIFY(wall.mayHaveBeenDone(id, base::Error{"timeout", ""}));
        QVERIFY(!wall.mayHaveBeenDone(id, base::Error{"dropped", ""}));
        QVERIFY(!wall.mayHaveBeenDone(id, base::Error{"action_unknown", ""}));
        // Of a machine that is not on the wall: yes where either family says yes.
        QVERIFY(wall.mayHaveBeenDone(u"nobody"_s, base::Error{"timeout", ""}));
        QVERIFY(wall.mayHaveBeenDone(u"nobody"_s, base::Error{"dropped", ""}));
        QVERIFY(wall.mayHaveBeenDone(u"nobody"_s, base::Error{"action_unknown", ""}));
    }

    // M6 2g (design 3.4): a JetKVM's card that asks has its picture over its one viewer. Another viewer takes it,
    // and the card says so; the KVM changed (its password typed again: a driver made anew) stays aside, the new
    // driver handed what the KVM's record keeps: no offer is made, until "Show here".
    void aJetKvmThatSteppedAsideStaysAsideAcrossANewDriver()
    {
        QVERIFY2(core::testing::JetKvmPeerByHand::filmIsThere(), core::testing::JetKvmPeerByHand::noFilm);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        OwnKvm own;
        own.address = jet.address().toString();
        own.name = u"Small one"_s;
        own.kind = u"jetkvm"_s;
        const base::Result<QString> added = wall.add(own, u"secret"_s);
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        const QString& id = added.value();
        QCOMPARE(settled(wall, id).state, u"online"_s);
        const auto card = [&wall, &id] {
            return answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(id, 320, 0, std::move(done)); });
        };
        QTRY_VERIFY_WITH_TIMEOUT(card().ok(), 20000);
        viewer.handOver();
        QTRY_VERIFY_WITH_TIMEOUT(!card().ok() && card().error().code == "in_use", 10000);
        QTest::qWait(1500); // (the old connection is dropped by the JetKVM meanwhile)
        QVERIFY(wall.change(id, wall.kvms().constFirst(), u"secret"_s).ok());
        const qsizetype offers = jet.offersHeard.size();
        QTest::qWait(300);
        const base::Result<core::Snapshot> after = card();
        QVERIFY(!after.ok());
        QCOMPARE(after.error().code, std::string("in_use"));
        QCOMPARE(settled(wall, id).state, u"busy"_s);
        QTest::qWait(600);
        QCOMPARE(jet.offersHeard.size(), offers);
        // "Show here" takes it back.
        QVERIFY(doneBy(wall, id, core::Action::plain(u"resume"_s)).ok());
        QTRY_VERIFY_WITH_TIMEOUT(card().ok(), 20000);
        QCOMPARE(jet.offersHeard.size(), offers + 1);
    }

    void aJetKvmFromAFileHearsTheQuestionsAndNothingElseUntilItWasAsked()
    {
        // Its driver has no password until it was asked, and a JetKVM's driver asks as one that needs
        // none: so that it was not asked yet is all that keeps it from being looked at.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const StandIn page([](const Asked&) {
            return Answered(200, "<!doctype html><html><body><h1>Sign in</h1></body></html>"_ba, "text/html"_ba);
        });
        JetKvmByHand jet;
        LocalWall::FromAFile atAPage;
        atAPage.kvm.address = page.address();
        atAPage.kvm.kind = u"jetkvm"_s;
        atAPage.password = u"secret"_s;
        LocalWall::FromAFile atAJetKvm;
        atAJetKvm.kvm.address = jet.address().toString();
        atAJetKvm.kvm.kind = u"jetkvm"_s;
        atAJetKvm.password = u"secret"_s;
        const QList<base::Result<QString>> took = wall.takeFromAFile({atAPage, atAJetKvm});
        QCOMPARE(took.size(), 2);
        QVERIFY2(took.at(0).ok(), took.at(0).ok() ? "" : took.at(0).error().message.c_str());
        QVERIFY2(took.at(1).ok(), took.at(1).ok() ? "" : took.at(1).error().message.c_str());
        // What answers at the page's address is asked what it is, and is not one: its card says so, and
        // it heard the three questions, of each asking, and nothing more. Not its driver's look.
        const core::Status atPage = settled(wall, took.at(0).value());
        QCOMPARE(atPage.errorCode, u"not_a_kvm"_s);
        for (int asking = 0; asking < 3; ++asking) {
            statusOf(wall);
            QTest::qWait(50);
        }
        const int pageAskings = page.count(kvmdsQuestion);
        QVERIFY(pageAskings >= 1);
        QVERIFY(heardOnlyTheQuestions(page, pageAskings));
        QVERIFY(!wall.wasAskedWhatItIs(took.at(0).value()));
        // The JetKVM was asked, and is one: from then on its driver has its password, and looks at it,
        // which signs in nowhere.
        QCOMPARE(settled(wall, took.at(1).value()).state, u"online"_s);
        QVERIFY(wall.wasAskedWhatItIs(took.at(1).value()));
        QCOMPARE(jet.stand->count(kvmdsQuestion), 1);
        QCOMPARE(jet.stand->count(redfishsQuestion), 1);
        QVERIFY(jet.asked(jetKvmsQuestion) >= 2);
        QCOMPARE(jet.signIns, 0);
        QCOMPARE(jet.lineAskings, 0);
    }

    void aKvmWhereOneOfTheOtherFamilyAnswersIsNotTheOne()
    {
        // A KVM kept as a kvmd where a JetKVM answers, and one kept as a JetKVM where a kvmd answers:
        // each is said to be what it is not, and hears no password.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        JetKvmByHand jet;
        const MadeUpKvm kvmd;
        LocalWall::FromAFile kvmdAtAJetKvm;
        kvmdAtAJetKvm.kvm.address = jet.address().toString();
        kvmdAtAJetKvm.password = u"secret"_s;
        LocalWall::FromAFile jetKvmAtAKvmd;
        jetKvmAtAKvmd.kvm.address = kvmd.stand->address();
        jetKvmAtAKvmd.kvm.kind = u"jetkvm"_s;
        jetKvmAtAKvmd.password = u"secret"_s;
        const QList<base::Result<QString>> took = wall.takeFromAFile({kvmdAtAJetKvm, jetKvmAtAKvmd});
        QCOMPARE(took.size(), 2);
        QVERIFY2(took.at(0).ok(), took.at(0).ok() ? "" : took.at(0).error().message.c_str());
        QVERIFY2(took.at(1).ok(), took.at(1).ok() ? "" : took.at(1).error().message.c_str());
        const QString asKvmd = took.at(0).value();
        const QString asJetKvm = took.at(1).value();
        // (Said on its card, where adding the other is refused while this one stands at its address: so
        // this one is to be taken off first.)
        const QString saidOfKvmd = u"error [other_family] A JetKVM answers at this address, and this KVM is on the "
                                   "wall as a GL.iNet Comet / PiKVM. To have the JetKVM here, remove this KVM from "
                                   "the wall and add the JetKVM as a KVM of its own. No password was sent to it."_s;
        const QString saidOfJetKvm =
            u"error [other_family] A GL.iNet Comet / PiKVM answers at this address, and this KVM is on the wall as a "
            "JetKVM. To have the GL.iNet Comet / PiKVM here, remove this KVM from the wall and add the GL.iNet Comet / "
            "PiKVM as a KVM of its own. No password was sent to it."_s;
        QCOMPARE(tileOf(settled(wall, asKvmd)), saidOfKvmd);
        QCOMPARE(tileOf(settled(wall, asJetKvm)), saidOfJetKvm);
        QVERIFY(!wall.wasAskedWhatItIs(asKvmd));
        QVERIFY(!wall.wasAskedWhatItIs(asJetKvm));
        QVERIFY(heardOnlyTheQuestions(*jet.stand, jet.stand->count(kvmdsQuestion)));
        QVERIFY(heardOnlyTheQuestions(*kvmd.stand, kvmd.stand->count(kvmdsQuestion)));
        // Asked again later, as what is not the KVM is: the questions again, and still no password.
        QTest::qWait(quick().holdMs + 100);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                statusOf(wall);
                return jet.stand->count(kvmdsQuestion) >= 2 && kvmd.stand->count(kvmdsQuestion) >= 2;
            }(),
            10000);
        QCOMPARE(tileOf(statusOf(wall).value(asKvmd)), saidOfKvmd);
        QCOMPARE(tileOf(statusOf(wall).value(asJetKvm)), saidOfJetKvm);
        QVERIFY(heardOnlyTheQuestions(*jet.stand, jet.stand->count(kvmdsQuestion)));
        QVERIFY(heardOnlyTheQuestions(*kvmd.stand, kvmd.stand->count(kvmdsQuestion)));
        QCOMPARE(jet.signIns, 0);
        QCOMPARE(kvmd.signIns, 0);
    }

    void theAskingWaitsAsLongAsTheDriverOfEitherFamilyWould()
    {
        // A wall whose JetKVMs are given longer than its kvmds: the asking of what answers at a KVM's
        // address gives it the longer of the two (askingPace), so that a JetKVM its driver would reach is
        // not "something else" for answering slowly.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::DriverPaces paces = core::DriverPaces::alike(quick());
        paces.kvmd.connectWithinMs = 400;
        paces.kvmd.quietMs = 400;
        paces.kvmd.transferMs = 400;
        paces.jetkvm.quietMs = 4000;
        LocalWall wall(settings, secrets, paces);
        JetKvmByHand jet;
        jet.statusAfterMs = 1500;
        LocalWall::FromAFile one;
        one.kvm.address = jet.address().toString();
        one.kvm.kind = u"jetkvm"_s;
        one.password = u"secret"_s;
        const QList<base::Result<QString>> took = wall.takeFromAFile({one});
        QVERIFY2(took.constFirst().ok(), took.constFirst().ok() ? "" : took.constFirst().error().message.c_str());
        const QString& id = took.constFirst().value();
        QCOMPARE(tileOf(settled(wall, id)), u"online"_s);
        QVERIFY(wall.wasAskedWhatItIs(id));

        // And the other way round: a wall whose kvmds are given longer than its JetKVMs, and a kvmd slow to
        // say what it is.
        const QTemporaryDir otherFolder;
        core::Settings otherSettings(otherFolder.filePath(u"settings.json"_s));
        MemorySecrets otherSecrets;
        core::DriverPaces longer = core::DriverPaces::alike(quick());
        longer.jetkvm.connectWithinMs = 400;
        longer.jetkvm.quietMs = 400;
        longer.jetkvm.transferMs = 400;
        longer.kvmd.quietMs = 4000;
        LocalWall otherWall(otherSettings, otherSecrets, longer);
        MadeUpKvm kvm;
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 1500;
        kvm.first.insert("/api/auth/check"_ba, {late});
        LocalWall::FromAFile two;
        two.kvm.address = kvm.stand->address();
        two.kvm.kind = u"kvmd"_s;
        two.password = kvm.password;
        const QList<base::Result<QString>> tookToo = otherWall.takeFromAFile({two});
        QVERIFY2(tookToo.constFirst().ok(),
                 tookToo.constFirst().ok() ? "" : tookToo.constFirst().error().message.c_str());
        const QString& kvmdId = tookToo.constFirst().value();
        QCOMPARE(tileOf(settled(otherWall, kvmdId)), u"online"_s);
        QVERIFY(otherWall.wasAskedWhatItIs(kvmdId));
    }

    void aJetKvmOverPlainHttpIsTakenOntoHttpsOnlyAsItsOtherWayHasIt_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("address");
        QTest::addColumn<int>("found"); // a Kind
        QTest::addColumn<bool>("refused");
        QTest::addColumn<bool>("trusted");     // Windows trusts what was shown there
        QTest::addColumn<QString>("otherWay"); // empty: kvm::otherWayOf's
        QTest::addColumn<QString>("onto");     // empty: none
        const int jetKvm = static_cast<int>(core::KvmProbe::Kind::JetKvm);
        QTest::newRow("a JetKVM over plain HTTP")
            << u"jetkvm"_s << u"http://192.0.2.5"_s << jetKvm << false << false << QString() << u"https://192.0.2.5"_s;
        QTest::newRow("known by Windows' word there")
            << u"jetkvm"_s << u"http://192.0.2.5"_s << jetKvm << false << true << QString() << u"https://192.0.2.5"_s;
        QTest::newRow("a kvmd") << u"kvmd"_s << u"http://192.0.2.5"_s << jetKvm << false << false << QString()
                                << QString();
        QTest::newRow("over HTTPS already")
            << u"jetkvm"_s << u"https://192.0.2.5"_s << jetKvm << false << false << QString() << QString();
        QTest::newRow("a port of its own")
            << u"jetkvm"_s << u"http://192.0.2.5:8080"_s << jetKvm << false << false << QString() << QString();
        QTest::newRow("an answer without isSetup")
            << u"jetkvm"_s << u"http://192.0.2.5"_s << static_cast<int>(core::KvmProbe::Kind::SomethingElse) << false
            << false << QString() << QString();
        QTest::newRow("a line that was refused")
            << u"jetkvm"_s << u"http://192.0.2.5"_s << jetKvm << true << false << QString() << QString();
        QTest::newRow("another host") << u"jetkvm"_s << u"http://192.0.2.5"_s << jetKvm << false << false
                                      << u"https://192.0.2.6"_s << QString();
        QTest::newRow("the plain way at another port") << u"jetkvm"_s << u"http://192.0.2.5"_s << jetKvm << false
                                                       << false << u"http://192.0.2.5:8080"_s << QString();
    }

    void aJetKvmOverPlainHttpIsTakenOntoHttpsOnlyAsItsOtherWayHasIt()
    {
        QFETCH(QString, kind);
        QFETCH(QString, address);
        QFETCH(int, found);
        QFETCH(bool, refused);
        QFETCH(bool, trusted);
        QFETCH(QString, otherWay);
        QFETCH(QString, onto);
        OwnKvm known;
        known.id = u"0a1b2c3d"_s;
        known.address = address;
        known.kind = kind;
        known.name = u"Small one"_s;
        core::KvmProbe::Found there;
        there.kind = static_cast<core::KvmProbe::Kind>(found);
        there.shown = sha256Of("first");
        there.trustedByWindows = trusted;
        if (refused) {
            there.refused = core::kvm::Objected::Changed;
        }
        std::function<std::optional<QUrl>(const QUrl&)> way = core::kvm::otherWayOf;
        if (!otherWay.isEmpty()) {
            way = [otherWay](const QUrl&) { return std::optional<QUrl>(QUrl(otherWay)); };
        }
        const std::optional<OwnKvm> made = LocalWall::ontoHttps(known, there, way);
        if (onto.isEmpty()) {
            QVERIFY(!made.has_value());
            return;
        }
        if (!made) {
            QFAIL("It was not taken onto HTTPS.");
        }
        const OwnKvm& taken = *made;
        QCOMPARE(taken.address, onto);
        QCOMPARE(taken.id, known.id);
        QCOMPARE(taken.name, known.name);
        QCOMPARE(taken.kind, known.kind);
        QCOMPARE(taken.knownByCertificate, !trusted);
        QCOMPARE(taken.certificate, trusted ? QByteArray() : sha256Of("first"));
    }

    void aJetKvmsKeptPasswordGoesOntoHttpsOnlyAsOntoHttpsHasIt()
    {
        // (Nothing is asked of these: the wall is never asked how its KVMs are.)
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const auto jetAt = [](const QString& address) {
            OwnKvm one;
            one.address = address;
            one.kind = u"jetkvm"_s;
            return one;
        };
        const QString id = wall.add(jetAt(u"http://192.0.2.5"_s), u"secret"_s).value();
        core::KvmProbe::Found there;
        there.kind = core::KvmProbe::Kind::JetKvm;
        there.shown = sha256Of("first");
        const std::optional<OwnKvm> made = LocalWall::ontoHttps(wall.kvms().constFirst(), there, core::kvm::otherWayOf);
        if (!made) {
            QFAIL("It was not taken onto HTTPS.");
        }
        const OwnKvm& onto = *made;
        // Without the word of what answered there, the kept password stays behind.
        QCOMPARE(codeOf(wall.change(id, onto, std::nullopt)), std::string("password_needed"));
        // Nor does it go with that word to anything but exactly that: another certificate, another address.
        OwnKvm another = onto;
        another.certificate = sha256Of("second");
        QCOMPARE(codeOf(wall.change(id, another, std::nullopt, there)), std::string("password_needed"));
        another = onto;
        another.address = u"https://192.0.2.6"_s;
        QCOMPARE(codeOf(wall.change(id, another, std::nullopt, there)), std::string("password_needed"));
        QCOMPARE(wall.kvms().constFirst().address, u"http://192.0.2.5"_s);
        // With it: moved, its kept password saved anew for that address and certificate.
        QVERIFY(wall.change(id, onto, std::nullopt, there).ok());
        QCOMPARE(wall.kvms().constFirst().address, u"https://192.0.2.5"_s);
        QVERIFY(wall.kvms().constFirst().knownByCertificate);
        QCOMPARE(wall.kvms().constFirst().certificate, sha256Of("first"));
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", u"https://192.0.2.5"_s, sha256Of("first")));
        // Where another KVM of the wall is at that place, it is not moved there.
        const QString taken = wall.add(jetAt(u"http://192.0.2.9"_s), u"secret"_s).value();
        QVERIFY(wall.add(jetAt(u"http://192.0.2.9:443"_s), u"secret"_s).ok());
        const std::optional<OwnKvm> ontoTaken = LocalWall::ontoHttps(wall.kvms().at(1), there, core::kvm::otherWayOf);
        if (!ontoTaken) {
            QFAIL("It was not taken onto HTTPS.");
        }
        const OwnKvm& toTaken = *ontoTaken;
        QCOMPARE(codeOf(wall.change(taken, toTaken, std::nullopt, there)), std::string("duplicate"));
        QCOMPARE(wall.kvms().at(1).address, u"http://192.0.2.9"_s);
    }

    void aJetKvmsKeptPasswordIsSignedInWithOntoItsOwnHttpsAndNowhereElse()
    {
        // (Nothing is asked of these: a sign-in that is handed out is not begun here.)
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        OwnKvm jet;
        jet.address = u"http://192.0.2.5"_s;
        jet.kind = u"jetkvm"_s;
        const QString id = wall.add(jet, u"secret"_s).value();
        core::KvmProbe::Found there;
        there.kind = core::KvmProbe::Kind::JetKvm;
        there.shown = sha256Of("first");
        // Its own HTTPS, with the word of what answered there: held to what showed that.
        const auto signer = wall.signerIn(id, QUrl(u"https://192.0.2.5"_s), u"admin"_s, quickDrivers(), there);
        QVERIFY2(signer.ok(), signer.ok() ? "" : signer.error().message.c_str());
        QCOMPARE(signer.value()->where().address, QUrl(u"https://192.0.2.5"_s));
        QVERIFY(signer.value()->where().knownByCertificate);
        QCOMPARE(signer.value()->where().certificate, sha256Of("first"));
        // Without that word, or at another host with it: the kept password stays behind.
        QCOMPARE(codeOf(wall.signerIn(id, QUrl(u"https://192.0.2.5"_s), u"admin"_s, quickDrivers())),
                 std::string("password_needed"));
        QCOMPARE(codeOf(wall.signerIn(id, QUrl(u"https://192.0.2.6"_s), u"admin"_s, quickDrivers(), there)),
                 std::string("password_needed"));
    }

    void aJetKvmWhosePasswordCouldNotBeReadIsNotTakenOntoHttpsWithoutIt()
    {
        // Windows hands nothing back this once: a JetKVM whose password is kept (the wall's mark) is not taken
        // onto HTTPS without it, and its mark stays. Only one that has none kept, out of its box, goes without.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const auto jetAt = [](const QString& address) {
            OwnKvm one;
            one.address = address;
            one.kind = u"jetkvm"_s;
            return one;
        };
        const QString id = wall.add(jetAt(u"http://192.0.2.5"_s), u"secret"_s).value();
        core::KvmProbe::Found there;
        there.kind = core::KvmProbe::Kind::JetKvm;
        there.shown = sha256Of("first");
        const std::optional<OwnKvm> onto = LocalWall::ontoHttps(wall.kvms().constFirst(), there, core::kvm::otherWayOf);
        if (!onto) {
            QFAIL("It was not taken onto HTTPS.");
        }
        secrets.refusesReads = true;
        const base::Result<void> refused = wall.change(id, *onto, std::nullopt, there);
        secrets.refusesReads = false;
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("no_password"));
        QCOMPARE(wall.kvms().constFirst().address, u"http://192.0.2.5"_s);
        QVERIFY(wall.marksAPasswordFor(id));
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)), savedFor("secret", u"http://192.0.2.5"_s));
        // Read back, it goes along.
        QVERIFY(wall.change(id, *onto, std::nullopt, there).ok());
        QCOMPARE(secrets.all.value(LocalWall::secretOf(id)),
                 savedFor("secret", u"https://192.0.2.5"_s, sha256Of("first")));
        // One with none kept is taken onto HTTPS with none. (Another JetKVM, with a certificate of its own: one
        // that showed the first's would be the first, and not taken there.)
        const QString open = wall.add(jetAt(u"http://192.0.2.6"_s), QString()).value();
        QVERIFY(!wall.marksAPasswordFor(open));
        there.shown = sha256Of("second");
        const std::optional<OwnKvm> openOnto =
            LocalWall::ontoHttps(wall.kvms().constLast(), there, core::kvm::otherWayOf);
        if (!openOnto) {
            QFAIL("It was not taken onto HTTPS.");
        }
        QVERIFY(wall.change(open, *openOnto, std::nullopt, there).ok());
        QCOMPARE(wall.kvms().constLast().address, u"https://192.0.2.6"_s);
        QVERIFY(!wall.marksAPasswordFor(open));
    }

    void aJetKvmSaidToBeKnownByWhatIsNoCertificateHearsNothingAtAll()
    {
        // As a list written by hand, or damaged, can have it: a JetKVM known by a certificate that is
        // none, with a password in the store saved for what the list says. Its driver has none, and a
        // JetKVM's driver asks as one that needs none: so nothing at all may go out to it, its look
        // neither, over HTTPS and over plain HTTP.
        JetKvmByHand encrypted(tls("first"));
        JetKvmByHand plain;
        for (JetKvmByHand* jet : {&encrypted, &plain}) {
            const QTemporaryDir folder;
            const QString file = folder.filePath(u"settings.json"_s);
            MemorySecrets secrets;
            const QString address = jet->address().toString();
            OwnKvm own = knownAt(address, QByteArray(31, 'c'));
            own.id = u"0123abcd"_s;
            own.kind = u"jetkvm"_s;
            {
                core::Settings settings(file);
                core::Remembered remembered;
                remembered.kvms = {own};
                QVERIFY(settings.remember(remembered).ok());
            }
            secrets.all.insert(LocalWall::secretOf(own.id), savedFor("secret", address, own.certificate));
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quick());
            QCOMPARE(wall.kvms().size(), 1);
            const core::Status status = settled(wall, own.id);
            QCOMPARE(status.state, u"error"_s);
            QCOMPARE(status.errorCode, u"cert_invalid"_s);
            for (int asking = 0; asking < 3; ++asking) {
                statusOf(wall);
                QTest::qWait(50);
            }
            QVERIFY(jet->stand->asked.isEmpty());
            QCOMPARE(jet->stand->lines, 0);
            QCOMPARE(jet->signIns, 0);
            QCOMPARE(jet->lineAskings, 0);
        }
    }

    void aKvmThisBuildDoesNotReadKeepsItsPlaceItsSecretAndItsId()
    {
        // A wall of one KVM, and beside it in the file one that a later build added (an Intel AMT
        // machine), with its password kept in the store under its id.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const MadeUpKvm kvm;
        MemorySecrets secrets;
        const QString keptId = u"0c000009"_s;
        const QJsonObject kept{{u"id"_s, keptId},
                               {u"kind"_s, u"amt"_s},
                               {u"address"_s, u"https://192.0.2.50"_s},
                               {u"name"_s, u"An Intel AMT machine"_s}};
        const QByteArray keptSecret = savedFor("its own", u"https://192.0.2.50"_s);
        secrets.all.insert(LocalWall::secretOf(keptId), keptSecret);
        {
            QFile written(file);
            QVERIFY(written.open(QIODevice::WriteOnly));
            written.write(
                QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{QJsonObject{{u"kind"_s, u"kvmd"_s},
                                                                             {u"id"_s, u"0c000001"_s},
                                                                             {u"address"_s, kvm.stand->address()}},
                                                                 kept}}})
                    .toJson());
        }
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        // Not on the wall; and nothing of this build is put at its place, by any way in.
        QCOMPARE(wall.kvms().size(), 1);
        QCOMPARE(wall.keptAside().size(), 1);
        QCOMPARE(wall.keptAside().constFirst().id, keptId);
        core::OwnKvm there;
        there.address = u"https://192.0.2.50"_s;
        QCOMPARE(wall.add(there, u"secret"_s).error().code, std::string("duplicate"));
        QVERIFY(
            QString::fromStdString(wall.add(there, u"secret"_s).error().message).startsWith(u"A KVM that another"_s));
        core::OwnKvm alsoThere = there;
        alsoThere.address = u"http://192.0.2.50"_s;
        QCOMPARE(wall.add(alsoThere, u"secret"_s).error().code, std::string("duplicate"));
        QCOMPARE(wall.change(u"0c000001"_s, there, u"secret"_s).error().code, std::string("duplicate"));
        LocalWall::FromAFile fromAFile;
        fromAFile.kvm = there;
        fromAFile.password = u"secret"_s;
        const QList<base::Result<QString>> took = wall.takeFromAFile({fromAFile});
        QCOMPARE(took.size(), 1);
        QCOMPARE(took.constFirst().error().code, std::string("duplicate"));
        // A file's KVM of a family this build does not speak to is not taken.
        LocalWall::FromAFile ofAnother;
        ofAnother.kvm.address = u"https://192.0.2.60"_s;
        ofAnother.kvm.kind = u"amt"_s;
        QCOMPARE(wall.takeFromAFile({ofAnother}).constFirst().error().code, std::string("invalid"));
        // A KVM that is added elsewhere is given another id, and what the wall writes keeps the other
        // as it stood, behind its own; its secret is where it was.
        core::OwnKvm elsewhere;
        elsewhere.address = u"https://192.0.2.70"_s;
        const QString added = wall.add(elsewhere, u"secret"_s).value();
        QVERIFY(added != keptId);
        QVERIFY(QFile::exists(file));
        const QJsonArray written = QJsonDocument::fromJson([&file] {
                                       QFile again(file);
                                       return again.open(QIODevice::ReadOnly) ? again.readAll() : QByteArray();
                                   }())
                                       .object()[u"kvms"_s]
                                       .toArray();
        QCOMPARE(written.size(), 3);
        QCOMPARE(written.last().toObject(), kept);
        QCOMPARE(secrets.all.value(LocalWall::secretOf(keptId)), keptSecret);
        // And a change of a KVM of the wall keeps its family, whatever is handed in.
        core::OwnKvm renamed = wall.kvms().constFirst();
        renamed.name = u"Renamed"_s;
        renamed.kind = u"jetkvm"_s;
        QVERIFY(wall.change(renamed.id, renamed, std::nullopt).ok());
        QCOMPARE(wall.kvms().constFirst().kind, u"kvmd"_s);
    }

    void aKvmAtThePlaceOfAKeptOneIsChangedAsEverWhereItStays()
    {
        // As a file of an earlier build can hold them: a KVM of the wall, and one kept beside it at the
        // same place (an earlier build let it be added there).
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const MadeUpKvm kvm;
        MemorySecrets secrets;
        const QString address = kvm.stand->address();
        {
            QFile written(file);
            QVERIFY(written.open(QIODevice::WriteOnly));
            written.write(
                QJsonDocument(
                    QJsonObject{
                        {u"kvms"_s,
                         QJsonArray{
                             QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, u"0c000001"_s}, {u"address"_s, address}},
                             QJsonObject{{u"id"_s, u"0c000009"_s}, {u"kind"_s, u"amt"_s}, {u"address"_s, address}}}}})
                    .toJson());
        }
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        QCOMPARE(wall.kvms().size(), 1);
        core::OwnKvm renamed = wall.kvms().constFirst();
        renamed.name = u"Renamed"_s;
        renamed.group = u"A group"_s;
        QVERIFY(wall.change(renamed.id, renamed, std::nullopt).ok());
        QCOMPARE(wall.kvms().constFirst().name, u"Renamed"_s);
        QCOMPARE(wall.kvms().constFirst().group, u"A group"_s);
    }

    void aChangeThatComesToAKeptOnesPlaceIsRefusedWhereItWasNotThereBefore()
    {
        // A place with no port written is one with every other at its host that has none, whatever
        // the scheme; one with a port written is one with whatever speaks to that port. So the KVM at
        // http://h "stays" when it is changed to https://h, and comes to a kept one at http://h:443,
        // which it was not at before.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        {
            QFile written(file);
            QVERIFY(written.open(QIODevice::WriteOnly));
            written.write(
                QJsonDocument(
                    QJsonObject{{u"kvms"_s, QJsonArray{QJsonObject{{u"kind"_s, u"kvmd"_s},
                                                                   {u"id"_s, u"0c000001"_s},
                                                                   {u"address"_s, u"http://192.0.2.80"_s}},
                                                       QJsonObject{{u"id"_s, u"0c000009"_s},
                                                                   {u"kind"_s, u"amt"_s},
                                                                   {u"address"_s, u"http://192.0.2.80:443"_s}}}}})
                    .toJson());
        }
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        QCOMPARE(wall.kvms().size(), 1);
        core::OwnKvm moved = wall.kvms().constFirst();
        moved.address = u"https://192.0.2.80"_s;
        const base::Result<void> refused = wall.change(moved.id, moved, u"secret"_s);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("duplicate"));
        QVERIFY(QString::fromStdString(refused.error().message).startsWith(u"A KVM that another"_s));
        QCOMPARE(wall.kvms().constFirst().address, u"http://192.0.2.80"_s);
    }

    void aChangeOfAddressThatStaysBesideAKeptOneIsMadeAsEver()
    {
        // The KVM at http://h shares its place with a kept one at https://h (no port written on either);
        // changed to http://h:443 it is at that place still (port 443 is where https://h is spoken to),
        // and came to no place it was not at: the change is made, as round 6's rule has it, and not
        // refused by an address that is merely another.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        {
            QFile written(file);
            QVERIFY(written.open(QIODevice::WriteOnly));
            written.write(QJsonDocument(QJsonObject{{u"kvms"_s,
                                                     QJsonArray{QJsonObject{{u"kind"_s, u"kvmd"_s},
                                                                            {u"id"_s, u"0c000001"_s},
                                                                            {u"address"_s, u"http://192.0.2.81"_s}},
                                                                QJsonObject{{u"id"_s, u"0c000009"_s},
                                                                            {u"kind"_s, u"amt"_s},
                                                                            {u"address"_s, u"https://192.0.2.81"_s}}}}})
                              .toJson());
        }
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        QCOMPARE(wall.kvms().size(), 1);
        core::OwnKvm moved = wall.kvms().constFirst();
        moved.address = u"http://192.0.2.81:443"_s;
        const base::Result<void> made = wall.change(moved.id, moved, u"secret"_s);
        QVERIFY2(made.ok(), made.ok() ? "" : made.error().message.c_str());
        QCOMPARE(wall.kvms().constFirst().address, u"http://192.0.2.81:443"_s);
    }

    void theKvmsOfAWallAreAskedAFewAtATimeAndNotAllAtOnce()
    {
        // Seven KVMs that were not asked yet, each of which takes a while over the question a
        // kvmd is known by; and a wall that asks three at a time.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        constexpr int answersAfterMs = 800;
        std::vector<std::unique_ptr<MadeUpKvm>> kvms;
        QStringList ids;
        MemorySecrets secrets;
        QJsonArray entries;
        for (int number = 0; number < 7; ++number) {
            auto kvm = std::make_unique<MadeUpKvm>();
            Answered late = core::testing::refusal(401, u"UnauthorizedError"_s);
            late.afterMs = answersAfterMs;
            kvm->first[kvmdsQuestion].append(late);
            const QString id = u"0b00000%1"_s.arg(number);
            ids.append(id);
            entries.append(QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, id}, {u"address"_s, kvm->stand->address()}});
            secrets.all.insert(LocalWall::secretOf(id), savedFor("secret", kvm->stand->address()));
            kvms.push_back(std::move(kvm));
        }
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(QJsonDocument(QJsonObject{{u"kvms"_s, entries}}).toJson());
        written.close();
        core::Settings settings(file);
        LocalWall::Own own;
        own.askedAtOnce = 3;
        // (No asking gives up its place before it has ended here: what one that does is, and
        // what the wall asks meanwhile, is another test's.)
        own.placeHeldMs = 60'000;
        LocalWall wall(settings, secrets, quick(), own);
        const auto heardSomething = [&kvms] {
            QList<int> which;
            for (std::size_t number = 0; number < kvms.size(); ++number) {
                if (!kvms[number]->stand->asked.isEmpty()) {
                    which.append(static_cast<int>(number));
                }
            }
            return which;
        };
        wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
        // While the first three are asked, the other four hear nothing: three at a time, and
        // in the wall's order.
        QTest::qWait(answersAfterMs / 2);
        QCOMPARE(heardSomething(), (QList<int>{0, 1, 2}));
        // As each of those ends, the next is asked: without the wall being asked anything again.
        QTest::qWait(answersAfterMs);
        QCOMPARE(heardSomething(), (QList<int>{0, 1, 2, 3, 4, 5}));
        // And every one of them is asked and signed in to in the end.
        for (const QString& id : std::as_const(ids)) {
            QCOMPARE(settled(wall, id).state, u"online"_s);
        }
        for (const auto& kvm : kvms) {
            QCOMPARE(kvm->signIns, 1);
            QCOMPARE(kvm->stand->count(jetKvmsQuestion), 1);
        }
    }

    void ofThoseThatWaitToBeAskedTheOnesNeverAskedInVainGoFirst()
    {
        // A wall that asks one at a time. In its order: something that is no KVM, a KVM that is
        // slow to answer, and a KVM.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const StandIn page([](const Asked&) { return Answered{200, "<html></html>"_ba, "text/html"_ba}; });
        MadeUpKvm slow;
        Answered late = core::testing::refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 900;
        slow.first[kvmdsQuestion].append(late);
        MadeUpKvm last;
        Answered alsoLate = core::testing::refusal(401, u"UnauthorizedError"_s);
        alsoLate.afterMs = 600;
        last.first[kvmdsQuestion].append(alsoLate);
        const QStringList ids{u"0c000001"_s, u"0c000002"_s, u"0c000003"_s};
        const QStringList addresses{page.address(), slow.stand->address(), last.stand->address()};
        MemorySecrets secrets;
        QJsonArray entries;
        for (qsizetype index = 0; index < 3; ++index) {
            entries.append(
                QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, ids.at(index)}, {u"address"_s, addresses.at(index)}});
            secrets.all.insert(LocalWall::secretOf(ids.at(index)), savedFor("secret", addresses.at(index)));
        }
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(QJsonDocument(QJsonObject{{u"kvms"_s, entries}}).toJson());
        written.close();
        core::Settings settings(file);
        LocalWall::Own own;
        own.askedAtOnce = 1;
        // (And the one place is held until its asking has ended: the order of those that wait
        // is what this is about.)
        own.placeHeldMs = 60'000;
        LocalWall wall(settings, secrets, quick(), own);
        const auto ask = [&wall] { wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {}); };
        // The first is asked, and is no KVM (asked in vain, and left alone for a moment); then
        // the second, which takes its time.
        ask();
        QTRY_COMPARE(slow.stand->count(kvmdsQuestion), 1);
        QCOMPARE(page.count(jetKvmsQuestion), 1);
        QVERIFY(last.stand->asked.isEmpty());
        // By the time the second has answered, the first is due again, and the wall was asked
        // once more how its KVMs are. The place that comes free goes to the third, which was
        // never asked in vain, and not back to the first that stands before it.
        QTest::qWait(quick().holdMs + 150);
        ask();
        QTRY_VERIFY(!last.stand->asked.isEmpty());
        QCOMPARE(page.count(jetKvmsQuestion), 1);
        // And the first has its turn after that.
        QTRY_COMPARE(page.count(jetKvmsQuestion), 2);
        QCOMPARE(page.count(signInQuestion), 0);
    }

    void kvmsThatAreOffAndStandFirstDoNotKeepTheOnesThatAreWellWaiting()
    {
        // A wall as an earlier program left it, which says of no KVM that it was asked: thirty
        // of them. The first ten are off: eight are switched off (each takes the call and never
        // says a word, so nothing is let in until the time for that is over), and at two
        // nothing listens. Behind them stand twenty that are well. With the times and the
        // numbers the program has: nobody chose any.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        std::vector<std::unique_ptr<NeverSpeaks>> switchedOff;
        std::vector<std::unique_ptr<core::testing::NobodyThere>> nothingListens;
        std::vector<std::unique_ptr<MadeUpKvm>> well;
        QStringList offIds;
        QStringList wellIds;
        MemorySecrets secrets;
        QJsonArray entries;
        const auto put = [&entries, &secrets](const QString& id, const QString& address) {
            entries.append(QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, id}, {u"address"_s, address}});
            secrets.all.insert(LocalWall::secretOf(id), savedFor("secret", address));
        };
        for (int number = 0; number < 8; ++number) {
            switchedOff.push_back(std::make_unique<NeverSpeaks>());
            offIds.append(u"0e0000%1"_s.arg(number, 2, 10, QLatin1Char('0')));
            put(offIds.constLast(), switchedOff.back()->address(true));
        }
        for (int number = 8; number < 10; ++number) {
            nothingListens.push_back(std::make_unique<core::testing::NobodyThere>());
            offIds.append(u"0e0000%1"_s.arg(number, 2, 10, QLatin1Char('0')));
            put(offIds.constLast(), nothingListens.back()->address());
        }
        for (int number = 10; number < 30; ++number) {
            well.push_back(std::make_unique<MadeUpKvm>());
            wellIds.append(u"0e0000%1"_s.arg(number, 2, 10, QLatin1Char('0')));
            put(wellIds.constLast(), well.back()->stand->address());
        }
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(QJsonDocument(QJsonObject{{u"kvms"_s, entries}}).toJson());
        written.close();
        core::Settings settings(file);
        QCOMPARE(settings.remembered().kvms.size(), 30);
        LocalWall wall(settings, secrets);
        const LocalWall::Own own;
        const core::KvmdDevice::Pace pace;

        // The wall is asked how its KVMs are, once, as a window asks it when it starts. When
        // the first and when the last of those that are well has heard a question:
        QElapsedTimer watch;
        watch.start();
        wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
        qint64 firstWellAt = -1;
        qint64 lastWellAt = -1;
        const bool allWereAsked = QTest::qWaitFor(
            [&] {
                const auto heard =
                    std::ranges::count_if(well, [](const auto& kvm) { return !kvm->stand->asked.isEmpty(); });
                if (heard > 0 && firstWellAt < 0) {
                    firstWellAt = watch.elapsed();
                }
                if (heard == static_cast<qsizetype>(well.size())) {
                    lastWellAt = watch.elapsed();
                }
                return lastWellAt >= 0;
            },
            30000);
        qInfo().noquote() << u"ten that are off before twenty that are well: the first that is well was asked "
                             "after %1 ms, the last after %2 ms"_s.arg(firstWellAt)
                                 .arg(lastWellAt);
        QVERIFY(allWereAsked);
        // Not while the eight places were held by the first eight: there are eight, and no more
        // are asked at once while each of those may still end within the moment.
        QVERIFY2(firstWellAt >= own.placeHeldMs - 30, qPrintable(QString::number(firstWellAt)));
        // And within a moment of that: long before one of those that are off was given up on,
        // which takes as long as a line may take to be let in. (It was that long: every place
        // was held by one that is off, and the first that is well was asked when the first of
        // those had been given up on.)
        QVERIFY2(lastWellAt < pace.connectWithinMs / 2, qPrintable(QString::number(lastWellAt)));
        for (const auto& off : switchedOff) {
            QVERIFY(off->firstCalledAt >= 0);
        }
        // Each of those that are well is signed in to, once, behind the three questions; and
        // what is off says "connecting" and nothing else for as long as it is being asked.
        for (const QString& id : std::as_const(wellIds)) {
            QCOMPARE(settled(wall, id).state, u"online"_s);
        }
        for (const auto& kvm : well) {
            QCOMPARE(kvm->signIns, 1);
            QCOMPARE(kvm->stand->count(jetKvmsQuestion), 1);
        }
        if (watch.elapsed() < pace.connectWithinMs - 500) {
            const QHash<QString, core::Status> all = statusOf(wall);
            for (qsizetype index = 0; index < 8; ++index) {
                QCOMPARE(tileOf(all.value(offIds.at(index))), u"connecting"_s);
            }
        }
        // An asking that went on without its place ends as any does: each of the ten is
        // "offline" when it was given up on, in the sentence for an address where nothing
        // answers, and nothing else is ever said of it.
        QStringList saidOtherwise;
        const auto allAreOffline = [&wall, &offIds, &saidOtherwise] {
            const QHash<QString, core::Status> all = statusOf(wall);
            bool offline = true;
            for (const QString& id : std::as_const(offIds)) {
                const QString says = tileOf(all.value(id));
                offline = offline && says == nothingAnswersThere;
                if (says != nothingAnswersThere && says != u"connecting"_s) {
                    saidOtherwise.append(says);
                }
            }
            return offline;
        };
        QTRY_VERIFY_WITH_TIMEOUT(allAreOffline(), 20000);
        QVERIFY2(saidOtherwise.isEmpty(), qPrintable(saidOtherwise.join(u" | "_s)));
        for (const QString& id : std::as_const(offIds)) {
            QVERIFY(!wall.wasAskedWhatItIs(id));
        }
    }

    void asManyAreAskedAtOnceAsThereMayBeAndNoMoreWhateverTheWallHolds()
    {
        // A wall of eight that are off: each takes the call, hears the questions and never says
        // a word, so that its asking is over when nothing has come for as long as nothing may
        // (two seconds, here). And a wall with two places, each held for four tenths of a
        // second, and four askings in all.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        std::vector<std::unique_ptr<NeverSpeaks>> off;
        MemorySecrets secrets;
        QJsonArray entries;
        for (int number = 0; number < 8; ++number) {
            off.push_back(std::make_unique<NeverSpeaks>());
            const QString id = u"0f00000%1"_s.arg(number);
            entries.append(
                QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, id}, {u"address"_s, off.back()->address(false)}});
            secrets.all.insert(LocalWall::secretOf(id), savedFor("secret", off.back()->address(false)));
        }
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(QJsonDocument(QJsonObject{{u"kvms"_s, entries}}).toJson());
        written.close();
        core::Settings settings(file);
        LocalWall::Own own;
        own.askedAtOnce = 2;
        own.placeHeldMs = 400;
        own.askedInAll = 4;
        core::KvmdDevice::Pace pace = quick();
        pace.quietMs = 2000;
        LocalWall wall(settings, secrets, pace, own);
        const qint64 began = sinceTheTestsBegan();
        wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
        // When each of them was first called, in the order they were, counted from the asking.
        const auto calledAfter = [&off, began] {
            QList<qint64> after;
            for (const auto& one : off) {
                if (one->firstCalledAt >= 0) {
                    after.append(one->firstCalledAt - began);
                }
            }
            std::ranges::sort(after);
            return after;
        };
        QTRY_COMPARE_WITH_TIMEOUT(calledAfter().size(), 8, 20000);
        const QList<qint64> after = calledAfter();
        const auto inWords = [&after] {
            QStringList words;
            for (const qint64 one : after) {
                words.append(QString::number(one));
            }
            return words.join(u", "_s);
        };
        qInfo().noquote() << u"eight that are off, two places, four askings in all: each was first called after "_s +
                                 inWords() + u" ms"_s;
        // Two at once, in the two places.
        QVERIFY2(after.at(1) < own.placeHeldMs - 30, qPrintable(inWords()));
        // The next two when those have held their places for as long as a place is held, and
        // not when they were given up on: what is off does not keep its place.
        QVERIFY2(after.at(2) >= own.placeHeldMs - 30, qPrintable(inWords()));
        QVERIFY2(after.at(3) < pace.quietMs / 2, qPrintable(inWords()));
        // And then nobody, for as long as four are under way: the fifth when one of the four was
        // given up on, whatever the wall still holds.
        QVERIFY2(after.at(4) >= pace.quietMs - pace.quietMs / 8, qPrintable(inWords()));
        // Each in the wall's order. (Two that are called together, as a place each comes free, may be heard of the
        // other way round, a moment apart: 94 ms on the loaded slow cores, in this round's whole runs. Less than half
        // a place's hold apart, then, which still tells one called before those whose places came free before it.)
        for (std::size_t number = 1; number < off.size(); ++number) {
            QVERIFY2(off[number]->firstCalledAt + own.placeHeldMs / 2 >= off[number - 1]->firstCalledAt,
                     qPrintable(inWords()));
        }
    }

    void whoeverAsksHowTheKvmsAreDoesNotWaitForAnAskingThatHasGoneOnWithoutItsPlace()
    {
        // One KVM that was not asked yet and is switched off, and one that is well. A place is
        // held for six tenths of a second; whoever asks how the KVMs are waits three seconds
        // for a look, at the most; and what is off is given up on after four.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const NeverSpeaks off;
        const MadeUpKvm well;
        const QString offId = u"0f100001"_s;
        const QString wellId = u"0f100002"_s;
        MemorySecrets secrets;
        secrets.all.insert(LocalWall::secretOf(offId), savedFor("secret", off.address(false)));
        secrets.all.insert(LocalWall::secretOf(wellId), savedFor("secret", well.stand->address()));
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(
            QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{QJsonObject{{u"kind"_s, u"kvmd"_s},
                                                                         {u"id"_s, offId},
                                                                         {u"address"_s, off.address(false)}},
                                                             QJsonObject{{u"kind"_s, u"kvmd"_s},
                                                                         {u"id"_s, wellId},
                                                                         {u"address"_s, well.stand->address()}}}}})
                .toJson());
        written.close();
        core::Settings settings(file);
        LocalWall::Own own;
        own.placeHeldMs = 600;
        core::KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 4000;
        pace.quietMs = 4000;
        pace.lookPatienceMs = 3000;
        LocalWall wall(settings, secrets, pace, own);
        const QSignalSpy said(&wall, &LocalWall::cameToSomething);

        // The first answer waits for the two askings while each holds its place, and for the
        // look at the one that is well: it has that one online. It does not wait for the one
        // that is off beyond that. (It did, for as long as an answer waits at all: with one
        // KVM of a wall off, every answer came that late.)
        QElapsedTimer watch;
        watch.start();
        QHash<QString, core::Status> all = statusOf(wall);
        const qint64 first = watch.elapsed();
        QCOMPARE(tileOf(all.value(wellId)), u"online"_s);
        QCOMPARE(tileOf(all.value(offId)), u"connecting"_s);
        QVERIFY2(first >= own.placeHeldMs - 30 && first < pace.lookPatienceMs / 2, qPrintable(QString::number(first)));
        // Nor does one that asks while that asking goes on: it is answered there and then.
        watch.restart();
        all = statusOf(wall);
        const qint64 second = watch.elapsed();
        QCOMPARE(tileOf(all.value(wellId)), u"online"_s);
        QCOMPARE(tileOf(all.value(offId)), u"connecting"_s);
        QVERIFY2(second < own.placeHeldMs, qPrintable(QString::number(second)));
        // What the one that is off comes to, the wall says by itself: nobody has to ask until
        // then to hear of it. (The wall's word for the first look at the one that is well may
        // still be on its way here, and is not the one that is meant: the one that counts is
        // the word at which the tile of the one that is off says so.)
        QStringList offAtEachWord;
        const QObject hears;
        connect(&wall, &LocalWall::cameToSomething, &hears, [&wall, &offAtEachWord, offId] {
            wall.askStatus([&offAtEachWord, offId](const base::Result<QHash<QString, core::Status>>& now) {
                offAtEachWord.append(tileOf(now.value().value(offId)));
            });
        });
        QTRY_VERIFY2_WITH_TIMEOUT(offAtEachWord.contains(nothingAnswersThere), qPrintable(offAtEachWord.join(u" | "_s)),
                                  10000);
        QVERIFY(said.count() > 0);
    }

    void everyKvmThatStaysOffIsAskedAgainInItsTurnAlsoTheLastOfMany()
    {
        // Six that are off: each takes the call and never says a word, and is given up on
        // after six tenths of a second. Room for two askings in all. Each is due again two
        // tenths after it was given up on, which is sooner than those behind it get their turn.
        // In the wall's order the first four would be asked again and again by turns, and the
        // last two never a second time: such a KVM says "nothing answers" for ever, also once
        // it is well again. Asked in the order in which they came to be due, each has its turn.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        std::vector<std::unique_ptr<NeverSpeaks>> off;
        MemorySecrets secrets;
        QJsonArray entries;
        for (int number = 0; number < 6; ++number) {
            off.push_back(std::make_unique<NeverSpeaks>());
            const QString id = u"0f30000%1"_s.arg(number);
            entries.append(
                QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, id}, {u"address"_s, off.back()->address(false)}});
            secrets.all.insert(LocalWall::secretOf(id), savedFor("secret", off.back()->address(false)));
        }
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(QJsonDocument(QJsonObject{{u"kvms"_s, entries}}).toJson());
        written.close();
        core::Settings settings(file);
        LocalWall::Own own;
        own.askedAtOnce = 2;
        own.placeHeldMs = 100;
        own.askedInAll = 2;
        core::KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 600;
        pace.quietMs = 600;
        pace.holdMs = 200;
        pace.longestHoldMs = 200;
        LocalWall wall(settings, secrets, pace, own);
        // Whoever shows the wall asks how its KVMs are, again and again.
        QTimer shows;
        connect(&shows, &QTimer::timeout, &wall,
                [&wall] { wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {}); });
        shows.start(100);
        const auto askings = [&off] {
            QList<int> each;
            for (const auto& one : off) {
                each.append(one->askings());
            }
            return each;
        };
        const auto inWords = [&askings] {
            QStringList words;
            for (const int one : askings()) {
                words.append(QString::number(one));
            }
            return words.join(u", "_s);
        };
        // Three rounds of six, two at a time, are five seconds and a half.
        QTRY_VERIFY2_WITH_TIMEOUT(std::ranges::min(askings()) >= 3, qPrintable(inWords()), 15000);
        shows.stop();
        qInfo().noquote() << u"six that are off, room for two askings: each was asked "_s + inWords() + u" times"_s;
        // By turns: none of them far more often than another by then.
        QVERIFY2(std::ranges::max(askings()) - std::ranges::min(askings()) <= 2, qPrintable(inWords()));
    }

    void theWallSaysByItselfWhatAKvmHasComeToOnceForAllThatCameWithinAMoment()
    {
        // Six KVMs that were not asked yet: four that are well, one where something answers
        // that is no KVM, and one where nothing listens.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        std::vector<std::unique_ptr<MadeUpKvm>> well;
        const StandIn page([](const Asked&) { return Answered{200, "<html></html>"_ba, "text/html"_ba}; });
        const core::testing::NobodyThere nobody;
        QStringList ids;
        QStringList addresses;
        for (int number = 0; number < 4; ++number) {
            well.push_back(std::make_unique<MadeUpKvm>());
            addresses.append(well.back()->stand->address());
        }
        addresses.append(page.address());
        addresses.append(nobody.address());
        MemorySecrets secrets;
        QJsonArray entries;
        for (qsizetype index = 0; index < addresses.size(); ++index) {
            ids.append(u"0f20000%1"_s.arg(index));
            entries.append(
                QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, ids.at(index)}, {u"address"_s, addresses.at(index)}});
            secrets.all.insert(LocalWall::secretOf(ids.at(index)), savedFor("secret", addresses.at(index)));
        }
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(QJsonDocument(QJsonObject{{u"kvms"_s, entries}}).toJson());
        written.close();
        core::Settings settings(file);
        LocalWall::Own own;
        // (Long enough for the four that are well to have been looked at within it, whatever
        // else this PC is doing: on the loaded slow cores 1 s was not, and a first look that
        // ended after it was a second word, of tiles that said the same: this round's whole runs.)
        own.saidAfterMs = 6000;
        LocalWall wall(settings, secrets, quick(), own);
        const QSignalSpy changed(&wall, &LocalWall::changed);
        // What the tiles say at each word of the wall's, asked there and then, as whoever shows
        // the wall asks at that word.
        QList<QStringList> atEachWord;
        connect(&wall, &LocalWall::cameToSomething, this, [&] {
            wall.askStatus([&](const base::Result<QHash<QString, core::Status>>& all) {
                QStringList tiles;
                for (const QString& id : std::as_const(ids)) {
                    tiles.append(all.value().value(id).state);
                }
                atEachWord.append(tiles);
            });
        });
        // Nothing is said before anybody asked how the KVMs are: nothing is asked of them then.
        QTest::qWait(300);
        QVERIFY(atEachWord.isEmpty());
        // Asked once, with the answer thrown away: the rest is the wall's own to say.
        wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(atEachWord.size(), 1, 30000);
        // One word for the four that came online and the one that is no KVM, which all came to
        // that within a moment of each other: and at that word each tile says it.
        const QStringList atTheFirst = atEachWord.constFirst();
        QCOMPARE(atTheFirst.mid(0, 5), (QStringList{u"online"_s, u"online"_s, u"online"_s, u"online"_s, u"error"_s}));
        // The one where nothing listens is given up on later, and is said then (unless it was
        // in time for the first word).
        QTRY_VERIFY_WITH_TIMEOUT(atEachWord.constLast().at(5) == u"offline"_s, 20000);
        QVERIFY(atEachWord.size() <= 2);
        // Nothing more is said of a KVM that stays as it is: one that is asked again and again,
        // and is away each time, has come to nothing new.
        const qsizetype words = atEachWord.size();
        const int askedOfThePage = page.count(jetKvmsQuestion);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
                return page.count(jetKvmsQuestion) >= askedOfThePage + 2;
            }(),
            20000);
        QTest::qWait(own.saidAfterMs + 300);
        QCOMPARE(atEachWord.size(), words);
        // And the wall itself is the one it was throughout: that it has changed is never said.
        QCOMPARE(changed.count(), 0);
    }

    void theWallSaysByItselfOfAKvmThatCannotBeAskedThatItsCardSaysSo()
    {
        // A list that was written by hand: a KVM that was not asked yet, said to be known by a
        // certificate, and what is named is none. There is nothing to hold a line to, so it is
        // asked nothing, and its card says what to do. That its card says so, the wall says by
        // itself as of anything else a KVM comes to: once.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const core::testing::NobodyThere nobody;
        const QString address = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        const QString id = u"0f300001"_s;
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{QJsonObject{{u"kind"_s, u"kvmd"_s},
                                                                                   {u"id"_s, id},
                                                                                   {u"address"_s, address},
                                                                                   {u"known_by_certificate"_s, true},
                                                                                   {u"certificate"_s, u"abcd"_s}}}}})
                          .toJson());
        written.close();
        MemorySecrets secrets;
        core::Settings settings(file);
        QCOMPARE(settings.remembered().kvms.size(), 1);
        LocalWall wall(settings, secrets, quick());
        const QSignalSpy said(&wall, &LocalWall::cameToSomething);
        const core::Status status = statusOf(wall).value(id);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"cert_invalid"_s);
        QTRY_COMPARE_WITH_TIMEOUT(said.count(), 1, 3000);
        // Asked how it is again, it is as it was: nothing more is said.
        QCOMPARE(statusOf(wall).value(id).errorCode, u"cert_invalid"_s);
        QTest::qWait(LocalWall::Own{}.saidAfterMs + 300);
        QCOMPARE(said.count(), 1);
    }

    void whatAnAskingFindsOfAKvmThatWasChangedMeanwhileCountsForNothing()
    {
        // A KVM that was not asked yet, at an address where a KVM answers, slowly. While it is
        // asked there, it is moved to an address where something answers that is no KVM. The
        // asking comes back and says "a KVM": of the address it had. It is not taken for the
        // KVM as it is, and no password goes to what is at the address it has now.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MadeUpKvm kvm;
        Answered late = core::testing::refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 600;
        kvm.first[kvmdsQuestion].append(late);
        const StandIn page([](const Asked&) { return Answered{200, "<html></html>"_ba, "text/html"_ba}; });
        const QString id = u"0d000001"_s;
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(
            QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{QJsonObject{{u"kind"_s, u"kvmd"_s},
                                                                         {u"id"_s, id},
                                                                         {u"address"_s, kvm.stand->address()}}}}})
                .toJson());
        written.close();
        MemorySecrets secrets;
        secrets.all.insert(LocalWall::secretOf(id), savedFor("secret", kvm.stand->address()));
        core::Settings settings(file);
        LocalWall wall(settings, secrets, quick());
        wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
        QTRY_COMPARE(kvm.stand->count(kvmdsQuestion), 1);
        OwnKvm moved = wall.kvms().constFirst();
        moved.address = page.address();
        QVERIFY(wall.change(id, moved, u"secret"_s).ok());
        // The asking of the address it had ends, and nothing comes of it.
        QTest::qWait(late.afterMs + 300);
        QCOMPARE(page.count(signInQuestion), 0);
        QVERIFY(!wall.wasAskedWhatItIs(id));
        // It is asked where it is now: no KVM, and nothing but the questions went there.
        const core::Status status = settled(wall, id);
        QCOMPARE(status.errorCode, u"not_a_kvm"_s);
        QCOMPARE(page.count(signInQuestion), 0);
        QVERIFY(heardOnlyTheQuestions(page));
        QCOMPARE(kvm.signIns, 0);
        QVERIFY(!wall.wasAskedWhatItIs(id));
    }

    void theCertificateOfAKvmAsItWasIsNotKept()
    {
        // A KVM's driver says which certificate it was shown, and the wall keeps it a turn of
        // the loop later: by then the KVM may be known by another. What was shown is kept only
        // where it is the certificate the KVM is known by NOW.
        class AskedByItself : public LocalWall {
        public:
            using LocalWall::keepCertificate;
            using LocalWall::LocalWall;
        };
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        AskedByItself wall(settings, secrets, quick());
        // (Nobody is at these addresses, and nothing looks at this wall.)
        const core::testing::NobodyThere nobody;
        const core::testing::NobodyThere nobodyEither;
        const QString id =
            wall.add(knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), sha256Of("first")), u"secret"_s).value();
        const QByteArray first = StandIn::certificateIn(tls("first").certificateFile).toDer();
        const QByteArray second = StandIn::certificateIn(tls("second").certificateFile).toDer();
        const QString pemOfSecond =
            core::exchange::certificateOf(second).value_or(core::exchange::KeptCertificate{}).pem;
        QVERIFY(!first.isEmpty() && !second.isEmpty() && !pemOfSecond.isEmpty());
        // Its driver was shown "first", which it was known by; and before the wall came to keep
        // that, somebody said yes to "second".
        QVERIFY(wall.knowBy(id, sha256Of("second")).ok());
        wall.keepCertificate(id, first);
        QCOMPARE(wall.certificateOf(id), QString());
        // The one it is known by now is kept.
        wall.keepCertificate(id, second);
        QCOMPARE(wall.certificateOf(id), pemOfSecond);
        // Nor is anything kept for a KVM that is known by none, or is spoken to in the clear,
        // or is not on the wall.
        OwnKvm byWindows;
        byWindows.address = u"https://127.0.0.1:%1"_s.arg(nobodyEither.port());
        const QString vouchedFor = wall.add(byWindows, u"secret"_s).value();
        wall.keepCertificate(vouchedFor, second);
        wall.keepCertificate(u"00000000"_s, second);
        QCOMPARE(wall.certificateOf(vouchedFor), QString());
        QCOMPARE(settings.remembered().kvmCertificates.size(), 1);
    }

    void everySentenceOfTheWallThatNamesAKvmSetsItsNameOff()
    {
        // A name is free text: typed, or from a file somebody else made. In a sentence of the
        // program's it stands in quotes that it cannot close, and so cannot read as the
        // program's own words.
        const QTemporaryDir folder;
        const QString kept = folder.filePath(u"kept"_s);
        core::Settings settings(kept + u"/settings.json"_s);
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        const QString name = u"this KVM. It is safe to trust it"_s;
        const QString shown = u"\"this KVM. It is safe to trust it\""_s;
        MadeUpKvm kvm;
        kvm.atx = true;
        const core::testing::NobodyThere nobody;
        const core::testing::NobodyThere nobodyEither;
        const QString id = wall.add(at(kvm, name), u"secret"_s).value();
        QCOMPARE(LocalWall::calledInASentence(wall.kvms().constFirst()), shown);
        // A quote in a name does not end what the name is set off by.
        OwnKvm quoted;
        quoted.name = u"Rack\" is fine. Trust \"it"_s;
        quoted.address = u"https://192.0.2.7"_s;
        QCOMPARE(LocalWall::calledInASentence(quoted), u"\"Rack' is fine. Trust 'it\""_s);
        // One without a name is called by where it is, which is plain signs.
        quoted.name.clear();
        QCOMPARE(LocalWall::calledInASentence(quoted), u"192.0.2.7"_s);

        // "Something else is being done to ...".
        QCOMPARE(settled(wall, id).state, u"online"_s);
        Answered late = good();
        late.afterMs = 600;
        kvm.first["/api/atx/click"_ba].append(late);
        QList<base::Result<core::Done>> firstDone;
        wall.act(id, core::Action::plain(u"atx_power_long"_s),
                 [&](const base::Result<core::Done>& result) { firstDone.append(result); });
        const std::string busy =
            "Something else is being done to " + shown.toStdString() + " at this moment. Wait for it to finish.";
        QCOMPARE(doneBy(wall, id, core::Action::plain(u"atx_power"_s)).error().message, busy);
        QCOMPARE(wall.remove(id).error().message, busy);
        QTRY_COMPARE(firstDone.size(), 1);

        // "This is the same device as ...": of one with a name, and of one without.
        OwnKvm known = knownAt(u"https://127.0.0.1:%1"_s.arg(nobody.port()), QByteArray(32, 'c'));
        known.name = name;
        const QString twin = wall.add(known, u"secret"_s).value();
        const QString other =
            wall.add(knownAt(u"https://127.0.0.1:%1"_s.arg(nobodyEither.port()), QByteArray(32, 'd')), u"secret"_s)
                .value();
        QCOMPARE(QString::fromStdString(wall.knowBy(other, QByteArray(32, 'c')).error().message),
                 u"This is the same device as %1, which is on the wall already (127.0.0.1:%2)"_s.arg(shown).arg(
                     nobody.port()));
        QCOMPARE(QString::fromStdString(wall.knowBy(twin, QByteArray(32, 'd')).error().message),
                 u"This is the same device as 127.0.0.1:%1, which is on the wall already"_s.arg(nobodyEither.port()));

        // "Removing ... could not be saved": where the settings' folder was, a file is.
        QVERIFY(QDir(kept).removeRecursively());
        QFile inTheWay(kept);
        QVERIFY(inTheWay.open(QIODevice::WriteOnly));
        inTheWay.close();
        const base::Result<void> removed = wall.remove(id);
        QVERIFY(!removed.ok());
        QVERIFY2(QString::fromStdString(removed.error().message)
                     .startsWith(u"Removing %1 could not be saved ("_s.arg(shown)),
                 removed.error().message.c_str());
    }
};

QTEST_GUILESS_MAIN(TestLocalWall)
#include "test_local_wall.moc"
