#include <algorithm>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/kvm_adding.h>

#include "jetkvm_by_hand.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmAdding;
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

// Where a kvmd is signed in to, and the three questions a probe asks before that.
constexpr const char* signInQuestion = "/api/auth/login";
constexpr const char* kvmdsQuestion = "/api/auth/check";
constexpr const char* jetKvmsQuestion = "/device/status";
constexpr const char* redfishsQuestion = "/redfish/v1/";

// The server's two rules for a line that is typed, as its page shows them behind the field's name.
constexpr const char* notOneLine = "this has to be one line: no line breaks or control characters, and nothing that "
                                   "turns the direction of the writing round";
constexpr const char* tooLongBy = "this may have %1 characters at the most";

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

// This PC's own wall for one test: its settings in a folder that goes with the test, its secrets
// in memory.
struct OwnWall {
    OwnWall() : settings(folder.filePath(u"settings.json"_s)), wall(settings, secrets, quickDriver()) {}

    QTemporaryDir folder;
    core::Settings settings;
    MemorySecrets secrets;
    LocalWall wall;
};

// What answers at an address and is no kvmd. A page, whatever is asked of it:
Answered asAPage(const Asked&)
{
    return {200, "<!doctype html><html><body><h1>Sign in</h1></body></html>"_ba, "text/html"_ba};
}
// And a server's management controller, which shows the root of its Redfish service to anyone and
// nothing else to somebody who is not signed in.
Answered asAController(const Asked& asked)
{
    if (asked.path == redfishsQuestion) {
        return {200, written({{u"@odata.id"_s, u"/redfish/v1/"_s}, {u"RedfishVersion"_s, u"1.8.0"_s}})};
    }
    return {401, written({{u"error"_s, u"unauthorized"_s}})};
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

// What a probe found, made up: for the decision of the way a KVM is added, which asks nothing.
core::KvmProbe::Found aFound(core::KvmProbe::Kind kind, const QByteArray& shown = {}, bool nothingListens = false)
{
    core::KvmProbe::Found found;
    found.kind = kind;
    found.shown = shown;
    found.nothingListens = nothingListens;
    return found;
}

// Two addresses that are one JetKVM's two ways (KvmAdding::Pace::otherWay): the stand-ins of a test are
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

// One KVM put on this PC's own wall, from what was typed into the box to its card: against made-up
// KVMs, and against things that answer at an address and are none.
class TestKvmAdding : public QObject {
    Q_OBJECT

private:
    // The same, with room to be let in for a sign-in over TLS (5 s), as the probe has: a handshake under the
    // address sanitizer on a busy PC takes longer than 1.5 s, and a test of where a password goes is not to
    // find the HTTPS way gone for that.
    static KvmAdding::Pace roomForTls()
    {
        KvmAdding::Pace pace = quick();
        pace.driver.connectWithinMs = 5000;
        return pace;
    }

    static KvmAdding::Pace quick()
    {
        KvmAdding::Pace pace;
        // The probe has the program's own limits. A TLS handshake under the address sanitizer on a busy PC
        // took longer than the 1.5 s quiet and connect limits this had (gate 4 of Checkpoint 2: four
        // HTTPS rows "unreachable"), as the naming row had found before.
        pace.probe = {};
        pace.driver = quickDriver();
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

    // What somebody types to add what is at an address, with the password the made-up KVMs have.
    static KvmAdding::Typed typedAt(const QString& address, const QString& name = {}, const QString& group = {})
    {
        KvmAdding::Typed typed;
        typed.address = address;
        typed.password = u"secret"_s;
        typed.name = name;
        typed.group = group;
        return typed;
    }

    // Adds, lets the program's loop run until the result is there, and gives it. A result that
    // was there before add() came back, or never came, has failed the test.
    // The code an adding was refused with; "(ok)" where it was not (and the test fails, not stops).
    static std::string codeOf(const base::Result<KvmAdding::Added>& result)
    {
        return result.ok() ? std::string("(ok)") : result.error().code;
    }
    static base::Result<KvmAdding::Added> addedBy(KvmAdding& adding, const KvmAdding::Typed& typed)
    {
        QList<base::Result<KvmAdding::Added>> told;
        adding.add(typed, [&told](const base::Result<KvmAdding::Added>& result) { told.append(result); });
        if (!told.isEmpty()) {
            return base::Error{"at_once", "The result was handed over before add() had come back."};
        }
        if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 20000)) {
            return base::Error{"never", "The adding never ended."};
        }
        return told.constFirst();
    }

    // Whether all that was asked at an address is what a probe asks: its three questions, each
    // once, with nothing in them that says who asks, and nothing of a password.
    static bool heardOnlyTheQuestions(const StandIn& stand)
    {
        const bool nothingSaid = std::ranges::all_of(stand.asked, [](const Asked& one) {
            return one.method == "GET" && one.body.isEmpty() && !one.headers.contains("cookie") &&
                   !one.headers.contains("authorization") && !one.path.contains("secret");
        });
        return nothingSaid && stand.asked.size() == 3 && stand.count(kvmdsQuestion) == 1 &&
               stand.count(jetKvmsQuestion) == 1 && stand.count(redfishsQuestion) == 1;
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
    // How a KVM of the wall is once it has been looked at: asked until it is no longer "connecting".
    static core::Status settled(LocalWall& wall, const QString& id)
    {
        core::Status status;
        static_cast<void>(QTest::qWaitFor(
            [&] {
                const auto all =
                    answerTo<QHash<QString, core::Status>>([&](auto done) { wall.askStatus(std::move(done)); });
                status = all.ok() ? all.value().value(id) : core::Status{};
                return status.state != u"connecting"_s;
            },
            20000));
        return status;
    }

private slots:
    void everyFamilyASavedKvmMayBeOfIsOneTheAskingFinds()
    {
        // The two lists go together: a family a saved KVM may be of is one the asking can find it to
        // be, and a word that is no such family is none it finds.
        for (const QString& family : {u"kvmd"_s, u"jetkvm"_s, u"redfish"_s, u"amt"_s, u"KVMD"_s, u""_s, u"vnc"_s}) {
            QCOMPARE(core::kvm::foundAs(family).has_value(), core::OwnKvm::namesAFamily(family));
        }
        QCOMPARE(core::kvm::foundAs(u"kvmd"_s), std::optional<core::KvmProbe::Kind>(core::KvmProbe::Kind::Kvmd));
        QCOMPARE(core::kvm::foundAs(u"jetkvm"_s), std::optional<core::KvmProbe::Kind>(core::KvmProbe::Kind::JetKvm));
        QCOMPARE(core::kvm::foundAs(u"redfish"_s), std::optional<core::KvmProbe::Kind>(core::KvmProbe::Kind::Redfish));
        // And back: what is found of a family is kept as that family, and what is of none is kept as
        // nothing (it is not added).
        for (const QString& family : {u"kvmd"_s, u"jetkvm"_s, u"redfish"_s}) {
            QCOMPARE(core::kvm::familyOf(*core::kvm::foundAs(family)), family);
        }
        for (const core::KvmProbe::Kind kind :
             {core::KvmProbe::Kind::SomethingElse, core::KvmProbe::Kind::Nothing, core::KvmProbe::Kind::NotAsked}) {
            QVERIFY(core::kvm::familyOf(kind).isEmpty());
        }
    }

    void initTestCase()
    {
        // The probe and the driver choose Windows' own TLS, which is refused once TLS is in use: so
        // one of each is made before anything else touches TLS (the stand-ins do).
        const core::KvmProbe firstProbe;
        const core::KvmdDevice firstDriver(core::KvmdDevice::Where{}, QString());
    }

    void whatIsKeptOfWhatWasTyped_data()
    {
        // Each of these beside what the server keeps of the same text (_a_line, tried with the
        // server's own lines of code).
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("kept");
        const QString name = u"name"_s;
        const QString group = u"group"_s;
        const QString user = u"user"_s;
        QTest::newRow("a name") << name << u"Build server"_s << u"Build server"_s;
        QTest::newRow("no name") << name << u""_s << u""_s;
        QTest::newRow("blanks for a name") << name << u"  \t "_s << u""_s;
        QTest::newRow("blanks around a name") << name << u"  Build server \t\r\n"_s << u"Build server"_s;
        // A name keeps the blanks it was typed with, a no-break one and a wide one among them.
        QTest::newRow("blanks inside a name") << name << u"Build   server"_s << u"Build   server"_s;
        QTest::newRow("a tab inside a name") << name << u"Build\tserver"_s << u"Build server"_s;
        QTest::newRow("a no-break blank inside")
            << name << u"Build"_s + sign(0xA0) + u"server"_s << u"Build"_s + sign(0xA0) + u"server"_s;
        QTest::newRow("a wide blank inside")
            << name << u"Build"_s + sign(0x3000) + u"server"_s << u"Build"_s + sign(0x3000) + u"server"_s;
        // And loses them at its ends, whatever kind they are: also what ends a line there, and the
        // four signs that part the records of a file, which are blanks to the server and not to Qt.
        QTest::newRow("blanks of other kinds around")
            << name << sign(0xA0) + sign(0x3000) + sign(0x2003) + u"Build"_s + sign(0x202F) + sign(0x205F)
            << u"Build"_s;
        QTest::newRow("what ends a line, at the ends")
            << name << sign(0x2028) + sign(0x85) + u"Build"_s + sign(0x2029) + sign(0x0B) << u"Build"_s;
        QTest::newRow("what parts records, at the ends")
            << name << sign(0x1C) + sign(0x1D) + u"Build"_s + sign(0x1E) + sign(0x1F) << u"Build"_s;
        // A blank of no width is nothing, wherever it stands; and the blanks that stood between
        // it and an end are still blanks around the name.
        QTest::newRow("blanks of no width")
            << name << sign(0x200B) + u" Build"_s + sign(0xFEFF) + u"server "_s + sign(0x2060) << u"Buildserver"_s;
        QTest::newRow("nothing but a blank of no width") << name << sign(0x200B) << u""_s;
        // What only stands next to what is refused is kept as it is.
        QTest::newRow("a narrow no-break blank inside")
            << name << u"a"_s + sign(0x202F) + u"b"_s << u"a"_s + sign(0x202F) + u"b"_s;
        QTest::newRow("the sign before those that end a line")
            << name << u"a"_s + sign(0x2027) + u"b"_s << u"a"_s + sign(0x2027) + u"b"_s;
        QTest::newRow("the sign after those that turn the writing")
            << name << u"a"_s + sign(0x206A) + u"b"_s << u"a"_s + sign(0x206A) + u"b"_s;
        QTest::newRow("the neighbours of what moves the cursor")
            << name << u"a"_s + sign(0x7E) + sign(0xA1) + u"b"_s << u"a"_s + sign(0x7E) + sign(0xA1) + u"b"_s;
        // Letters of any alphabet, and signs from far up, the joiner inside many an emoji with them.
        QTest::newRow("another alphabet") << name << u"B"_s + sign(0xFC) + u"ro"_s << u"B"_s + sign(0xFC) + u"ro"_s;
        const QString family = sign(0x1F468) + sign(0x200D) + sign(0x1F469);
        QTest::newRow("signs from far up") << name << u" "_s + family + u" "_s << family;
        // As long as a name may be, and with as much around it as a box takes.
        const QString longest(core::kvm::longestName, u'n');
        QTest::newRow("the longest name") << name << longest << longest;
        QTest::newRow("the longest name with much around it")
            << name << QString(120, u' ') + longest + QString(120, u' ') << longest;
        QTest::newRow("the longest name of signs from far up")
            << name << sign(0x1F600).repeated(core::kvm::longestName / 2)
            << sign(0x1F600).repeated(core::kvm::longestName / 2);

        // In a group a run of blanks is one blank, of whatever kind they were.
        QTest::newRow("a group") << group << u"Rack 2"_s << u"Rack 2"_s;
        QTest::newRow("no group") << group << u""_s << u""_s;
        QTest::newRow("blanks in a group") << group << u"  Rack \t  2  "_s << u"Rack 2"_s;
        QTest::newRow("blanks of other kinds in a group")
            << group << u"Rack"_s + sign(0xA0) + sign(0x3000) + u"2"_s << u"Rack 2"_s;
        QTest::newRow("a blank of no width between blanks")
            << group << u"Rack "_s + sign(0x200B) + u" 2"_s << u"Rack 2"_s;
        QTest::newRow("the longest group")
            << group << QString(core::kvm::longestGroup, u'g') << QString(core::kvm::longestGroup, u'g');
        // (Counted once its blanks are one.)
        const QString half(core::kvm::longestGroup / 2, u'g');
        QTest::newRow("a group that is short enough once its blanks are one")
            << group << half + u"    "_s + half.chopped(1) << half + u' ' + half.chopped(1);

        // No user name is the one every such KVM comes with.
        QTest::newRow("a user") << user << u"root"_s << u"root"_s;
        QTest::newRow("no user") << user << u""_s << u"admin"_s;
        QTest::newRow("blanks for a user") << user << u"   "_s << u"admin"_s;
        QTest::newRow("a blank of no width for a user") << user << sign(0xFEFF) << u"admin"_s;
        QTest::newRow("blanks around a user") << user << u" root\t"_s << u"root"_s;
        QTest::newRow("the longest user")
            << user << QString(core::kvm::longestUser, u'u') << QString(core::kvm::longestUser, u'u');
    }

    void whatIsKeptOfWhatWasTyped()
    {
        QFETCH(QString, field);
        QFETCH(QString, typed);
        QFETCH(QString, kept);
        const base::Result<QString> is = field == u"name"_s    ? core::kvm::nameFrom(typed)
                                         : field == u"group"_s ? core::kvm::groupFrom(typed)
                                                               : core::kvm::userFrom(typed);
        QVERIFY2(is.ok(), is.ok() ? "" : is.error().message.c_str());
        QCOMPARE(is.value(), kept);
        // And typed as it is kept, it is kept as it is.
        const base::Result<QString> again = field == u"name"_s    ? core::kvm::nameFrom(kept)
                                            : field == u"group"_s ? core::kvm::groupFrom(kept)
                                                                  : core::kvm::userFrom(kept);
        QCOMPARE(again.value(), kept);
    }

    void whatIsNoLineOrTooLongIsRefusedInTheServersWords_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("sentence");
        const QString name = u"name"_s;
        const QString group = u"group"_s;
        const QString user = u"user"_s;
        const QString nameNoLine = u"name: "_s + QLatin1StringView(notOneLine);
        const QString nameTooLong = u"name: "_s + QString::fromLatin1(tooLongBy).arg(core::kvm::longestName);
        // What ends a line inside it, and what moves the cursor.
        const auto inside = [](char32_t number) { return u"Build"_s + sign(number) + u"server"_s; };
        QTest::newRow("two lines") << name << inside(0x0A) << nameNoLine;
        QTest::newRow("a line that goes back to its start") << name << inside(0x0D) << nameNoLine;
        QTest::newRow("the sign of no sign") << name << inside(0x00) << nameNoLine;
        QTest::newRow("a bell") << name << inside(0x07) << nameNoLine;
        QTest::newRow("an escape") << name << inside(0x1B) << nameNoLine;
        QTest::newRow("what parts records, inside") << name << inside(0x1C) << nameNoLine;
        QTest::newRow("the last of the first lot") << name << inside(0x1F) << nameNoLine;
        QTest::newRow("a rubbing out") << name << inside(0x7F) << nameNoLine;
        QTest::newRow("the next line of another machine") << name << inside(0x85) << nameNoLine;
        QTest::newRow("the last of the second lot") << name << inside(0x9F) << nameNoLine;
        QTest::newRow("a line's end of its own") << name << inside(0x2028) << nameNoLine;
        QTest::newRow("a paragraph's end") << name << inside(0x2029) << nameNoLine;
        // What turns the direction of the writing round: a name could be made to read as another.
        for (const char32_t number :
             std::initializer_list<char32_t>{0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069}) {
            const QByteArray row = "turned round by " + QByteArray::number(static_cast<uint>(number), 16);
            QTest::newRow(row.constData()) << name << inside(number) << nameNoLine;
        }
        // Such a sign is no blank: at an end it is refused as anywhere.
        QTest::newRow("turned round from the start") << name << sign(0x202E) + u"Build"_s << nameNoLine;
        // Half of a sign that takes two places.
        QTest::newRow("the first half alone") << name << u"Build"_s + QString(QChar(char16_t{0xD83D})) << nameNoLine;
        QTest::newRow("the second half alone") << name << QString(QChar(char16_t{0xDE00})) + u"Build"_s << nameNoLine;
        QTest::newRow("the halves the wrong way round")
            << name << QString(QChar(char16_t{0xDE00})) + QString(QChar(char16_t{0xD83D})) << nameNoLine;
        // Too long, counted as it is kept.
        QTest::newRow("a letter too many") << name << QString(core::kvm::longestName + 1, u'n') << nameTooLong;
        QTest::newRow("a letter too many, with blanks around")
            << name << u"  "_s + QString(core::kvm::longestName + 1, u'n') + u"  "_s << nameTooLong;
        QTest::newRow("blanks inside count")
            << name << u"n"_s + QString(core::kvm::longestName - 1, u' ') + u"n"_s << nameTooLong;
        // (Taken by the server, which counts a sign from far up as one: here it takes two places,
        // as it does where the settings are read back.)
        QTest::newRow("a sign from far up too many")
            << name << sign(0x1F600).repeated(core::kvm::longestName / 2 + 1) << nameTooLong;
        // And what was typed is not gone through beyond four times that, however little of it
        // would be kept. (The server says this one in the words of the library that reads its forms.)
        QTest::newRow("more around it than a box takes")
            << name << QString(core::kvm::longestName, u'n') + QString(3 * core::kvm::longestName + 1, u' ')
            << nameTooLong;

        const QString groupTooLong = u"group: "_s + QString::fromLatin1(tooLongBy).arg(core::kvm::longestGroup);
        QTest::newRow("a group of two lines") << group << inside(0x0A) << u"group: "_s + QLatin1StringView(notOneLine);
        QTest::newRow("a group turned round")
            << group << inside(0x202E) << u"group: "_s + QLatin1StringView(notOneLine);
        QTest::newRow("a group too long") << group << QString(core::kvm::longestGroup + 1, u'g') << groupTooLong;
        QTest::newRow("a group too long by the one blank that is left")
            << group
            << QString(core::kvm::longestGroup / 2, u'g') + u"   "_s + QString(core::kvm::longestGroup / 2, u'g')
            << groupTooLong;

        const QString userTooLong = u"username: "_s + QString::fromLatin1(tooLongBy).arg(core::kvm::longestUser);
        QTest::newRow("a user of two lines") << user << u"ro\not"_s << u"username: "_s + QLatin1StringView(notOneLine);
        QTest::newRow("a user too long") << user << QString(core::kvm::longestUser + 1, u'u') << userTooLong;
    }

    void whatIsNoLineOrTooLongIsRefusedInTheServersWords()
    {
        QFETCH(QString, field);
        QFETCH(QString, typed);
        QFETCH(QString, sentence);
        const base::Result<QString> is = field == u"name"_s    ? core::kvm::nameFrom(typed)
                                         : field == u"group"_s ? core::kvm::groupFrom(typed)
                                                               : core::kvm::userFrom(typed);
        QVERIFY2(!is.ok(), qPrintable(is.ok() ? is.value() : QString()));
        QCOMPARE(is.error().code, std::string("invalid"));
        QCOMPARE(QString::fromStdString(is.error().message), sentence);
    }

    void theNameWhereNoneWasTyped_data()
    {
        QTest::addColumn<QString>("hostname");
        QTest::addColumn<QString>("address");
        QTest::addColumn<QString>("name");
        const QString where = u"https://192.168.1.20"_s;
        QTest::newRow("what the KVM calls itself") << u"build-kvm"_s << where << u"build-kvm"_s;
        QTest::newRow("in its own letters") << u"Build-KVM"_s << where << u"Build-KVM"_s;
        // Nothing, and what every KVM of its make is called, is no name: then it is where it is.
        QTest::newRow("it says nothing") << u""_s << where << u"192.168.1.20"_s;
        for (const QString& every : {u"glkvm"_s, u"pikvm"_s, u"localhost"_s, u"localhost.localdomain"_s, u"GLKVM"_s,
                                     u"PiKVM"_s, u"LocalHost.LocalDomain"_s}) {
            QTest::newRow(qPrintable(every)) << every << where << u"192.168.1.20"_s;
        }
        // Compared as the server compares them: the sign for Kelvin is a "k" in small letters.
        QTest::newRow("a letter that only looks like one")
            << u"GL"_s + sign(0x212A) + u"VM"_s << where << u"192.168.1.20"_s;
        // Only the whole of it.
        QTest::newRow("a name that begins like those") << u"glkvm-2"_s << where << u"glkvm-2"_s;
        QTest::newRow("a name that ends like those") << u"my.localhost"_s << where << u"my.localhost"_s;
        // Where it is: its host, and its port where it has one of its own.
        QTest::newRow("a name and a port") << u""_s << u"http://kvm.lan:8080"_s << u"kvm.lan:8080"_s;
        QTest::newRow("the new kind of numbers") << u"glkvm"_s << u"https://[::1]:8443"_s << u"[::1]:8443"_s;
        QTest::newRow("a name written for another alphabet")
            << u""_s << u"https://xn--mnchen-3ya.de"_s << u"xn--mnchen-3ya.de"_s;
        // No longer than a name may be, and not cut through the middle of a sign.
        const QString far = QString(60, u'a') + u'.' + QString(60, u'b') + u".lan"_s;
        QTest::newRow("an address longer than a name")
            << u""_s << u"https://"_s + far << far.left(core::kvm::longestName);
        QTest::newRow("a name of its own longer than a name")
            << QString(core::kvm::longestName + 5, u'h') << where << QString(core::kvm::longestName, u'h');
        QTest::newRow("a sign of two places at the cut")
            << QString(core::kvm::longestName - 1, u'h') + sign(0x1F600) + u"h"_s << where
            << QString(core::kvm::longestName - 1, u'h');
    }

    void theNameWhereNoneWasTyped()
    {
        QFETCH(QString, hostname);
        QFETCH(QString, address);
        QFETCH(QString, name);
        const base::Result<QUrl> where = core::kvm::addressFrom(address);
        QVERIFY(where.ok());
        QCOMPARE(core::kvm::nameWhereNoneWasTyped(hostname, where.value()), name);
        // What it gets is a name the box would take as it is.
        QCOMPARE(core::kvm::nameFrom(name).value(), name);
    }

    void aKvmIsAddedOverPlainHttp()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        KvmAdding adding(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        QVERIFY(!adding.isUnderWay());
        const auto added = addedBy(adding, typedAt(kvm.stand->address(), u"Build server"_s, u"Rack 2"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QVERIFY(!adding.isUnderWay());
        // An id of the wall's own making, and what the KVM says it is.
        QVERIFY(QRegularExpression(u"\\A[0-9a-f]{8}\\z"_s).match(added.value().id).hasMatch());
        QCOMPARE(added.value().name, u"Build server"_s);
        QCOMPARE(added.value().model, u"RM10"_s);
        QCOMPARE(added.value().version, u"V1.10.1 release2"_s);
        // In the clear, and said so; and known by no certificate, since it shows none.
        QVERIFY(added.value().overPlainHttp);
        QVERIFY(added.value().certificate.isEmpty());

        // On the wall as it was typed, with nothing to know it by: the plain way shows no certificate.
        QCOMPARE(changed.count(), 1);
        QCOMPARE(own.wall.kvms().size(), 1);
        const OwnKvm kept = own.wall.kvms().constFirst();
        QCOMPARE(kept.id, added.value().id);
        QCOMPARE(kept.name, u"Build server"_s);
        QCOMPARE(kept.address, kvm.stand->address());
        QCOMPARE(kept.user, u"admin"_s);
        QCOMPARE(kept.group, u"Rack 2"_s);
        QVERIFY(!kept.knownByCertificate);
        QVERIFY(kept.certificate.isEmpty());
        // Its password with the secrets, under its id, and nowhere else.
        QCOMPARE(own.secrets.all.size(), 1);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(kept.id)), savedFor("secret", kvm.stand->address()));

        // First asked what it is, with nothing that says who asks (and whether it signs this program in, with nothing
        // to sign in with: a password goes only on a line shown to be up); then signed in to, once; then asked who it
        // is.
        QVERIFY(kvm.stand->asked.size() > 4);
        for (qsizetype index = 0; index < 4; ++index) {
            const Asked& question = kvm.stand->asked.at(index);
            QCOMPARE(question.method, "GET"_ba);
            QVERIFY(!question.headers.contains("cookie"));
            QVERIFY(question.body.isEmpty());
        }
        QCOMPARE(kvm.stand->asked.at(4).path, QByteArray(signInQuestion));
        QCOMPARE(kvm.stand->asked.at(4).body, "user=admin&passwd=secret"_ba);
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(kvm.asked(signInQuestion), 1);
        QCOMPARE(kvm.asked("/api/system/get_hostname"), 1);

        // And it is there on the wall, signed in to once more by the wall's own driver.
        QCOMPARE(settled(own.wall, kept.id).state, u"online"_s);
        QCOMPARE(kvm.signIns, 2);
    }

    void aKvmAddedOverHttpsIsKnownByTheCertificateItShowed_data()
    {
        QTest::addColumn<QByteArray>("certificate");
        QTest::newRow("one of its own making") << "first"_ba;
        // As these KVMs make them: named "localhost", and good from 1970 to 1979.
        QTest::newRow("wrongly dated and named") << "comet"_ba;
    }

    void aKvmAddedOverHttpsIsKnownByTheCertificateItShowed()
    {
        QFETCH(QByteArray, certificate);
        OwnWall own;
        const MadeUpKvm kvm(tls(certificate.constData()));
        KvmAdding adding(own.wall, quick());
        const auto added = addedBy(adding, typedAt(kvm.stand->address(), u"Build server"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QVERIFY(!added.value().overPlainHttp);
        const OwnKvm kept = own.wall.kvms().constFirst();
        QCOMPARE(kept.address, kvm.stand->address());
        QVERIFY(kept.knownByCertificate);
        QCOMPARE(kept.certificate, sha256Of(certificate.constData()));
        // Which one it is, is told to whoever added it: to be compared with what the KVM says.
        QCOMPARE(added.value().certificate, sha256Of(certificate.constData()));
        QCOMPARE(kvm.signIns, 1);
        // By that certificate the wall's own driver knows it again.
        QCOMPARE(settled(own.wall, kept.id).state, u"online"_s);
    }

    void aKvmThatWindowsVouchesForIsKnownByNoCertificate()
    {
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        const MadeUpKvm kvm(tls("signed"));
        KvmAdding adding(own.wall, quick());
        const auto added = addedBy(adding, typedAt(kvm.stand->address()));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QVERIFY(!added.value().overPlainHttp);
        // Somebody's front door, whose certificate changes every few weeks: not one to remember.
        const OwnKvm kept = own.wall.kvms().constFirst();
        QVERIFY(!kept.knownByCertificate);
        QVERIFY(kept.certificate.isEmpty());
        QVERIFY(added.value().certificate.isEmpty());
        QCOMPARE(settled(own.wall, kept.id).state, u"online"_s);
    }

    void whatShowsTheCertificateOfAKvmOfTheWallIsThatOneAlsoWhereWindowsVouchesForIt()
    {
        // From the third reading of the twins: what answers over HTTPS shows the certificate a KVM of the wall is known
        // by, and Windows trusts that certificate there, so that it would be known by Windows' word and not by that
        // certificate. It is that KVM all the same (it has the key to it): "same_device", and no password goes to it.
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        const core::testing::NobodyThere nobody;
        OwnKvm other;
        other.address = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        other.name = u"The other"_s;
        other.knownByCertificate = true;
        other.certificate = sha256Of("signed");
        QVERIFY(own.wall.add(other, u"its own"_s).ok());
        const MadeUpKvm kvm(tls("signed"));
        KvmAdding adding(own.wall, quick());
        const auto refused = addedBy(adding, typedAt(kvm.stand->address()));
        QCOMPARE(codeOf(refused), std::string("same_device"));
        QCOMPARE(QString::fromStdString(refused.error().message),
                 u"This is the same device as \"The other\", which is on the wall already (127.0.0.1:%1)"_s.arg(
                     nobody.port()));
        QCOMPARE(kvm.signIns, 0);
        QCOMPARE(own.wall.kvms().size(), 1);
    }

    void whatCameOntoTheWallWhileItWasSignedInToIsLookedForByWhatItShowed()
    {
        // From the third reading of the twins: known by Windows' word where it is added, and while it is signed in to,
        // a KVM known by the certificate it showed comes onto the wall. Looked at once more before it is added, by what
        // it showed: that one, "same_device", and it is not added.
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        const core::testing::NobodyThere nobody;
        MadeUpKvm kvm(tls("signed"));
        Answered late = good({{u"hostname"_s, u"glkvm"_s}});
        late.afterMs = 600;
        kvm.first["/api/system/get_hostname"_ba].append(late);
        KvmAdding adding(own.wall, quick());
        QList<base::Result<KvmAdding::Added>> told;
        adding.add(typedAt(kvm.stand->address()),
                   [&told](const base::Result<KvmAdding::Added>& result) { told.append(result); });
        QTRY_COMPARE(kvm.asked(signInQuestion), 1);
        OwnKvm other;
        other.address = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        other.knownByCertificate = true;
        other.certificate = sha256Of("signed");
        QVERIFY(own.wall.add(other, u"its own"_s).ok());
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QCOMPARE(codeOf(told.constFirst()), std::string("same_device"));
        QCOMPARE(own.wall.kvms().size(), 1);
    }

    void theSameCertificateWithNobodyToVouchForItIsWhatTheKvmIsKnownBy()
    {
        // Windows objects to it now, as to one a KVM made for itself: so it is the one certificate
        // this KVM is known by, whoever signed it.
        OwnWall own;
        const MadeUpKvm kvm(tls("signed"));
        KvmAdding adding(own.wall, quick());
        const auto added = addedBy(adding, typedAt(kvm.stand->address()));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        const OwnKvm kept = own.wall.kvms().constFirst();
        QVERIFY(kept.knownByCertificate);
        QCOMPARE(kept.certificate, sha256Of("signed"));
        QCOMPARE(settled(own.wall, kept.id).state, u"online"_s);
    }

    void aKvmIsNamedAsTypedOrAsItCallsItselfOrByWhereItIs_data()
    {
        QTest::addColumn<int>("delayedTls");
        QTest::newRow("immediate TLS") << 0;
        QTest::newRow("delayed TLS") << 1800;
    }

    void aKvmIsNamedAsTypedOrAsItCallsItselfOrByWhereItIs()
    {
        QFETCH(int, delayedTls);
        OwnWall own;
        auto pace = quick();
        // Naming is independent of TLS speed. Keep the production probe budget;
        // the delayed row crosses the former shortened 1500 ms fixture deadline.
        pace.probe = {};
        KvmAdding adding(own.wall, pace);
        // What every KVM of its make is called is no name: where it is.
        const MadeUpKvm unnamed;
        auto added = addedBy(adding, typedAt(unnamed.stand->address()));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(added.value().name, u"127.0.0.1:%1"_s.arg(unnamed.stand->serverPort()));
        QCOMPARE(own.wall.kvms().constLast().name, added.value().name);
        // A name of its own is its name.
        MadeUpKvm named;
        named.instead.insert("/api/system/get_hostname"_ba, good({{u"hostname"_s, u"build-kvm"_s}}));
        added = addedBy(adding, typedAt(named.stand->address()));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(added.value().name, u"build-kvm"_s);
        QCOMPARE(own.wall.kvms().constLast().name, u"build-kvm"_s);
        // The other maker's says nothing of the kind, and nothing of what it is.
        MadeUpKvm silent(tls("first"));
        QVERIFY(silent.stand->letInOnlyAfter(delayedTls));
        silent.stand->beforeAnswer = [&] {
            if (silent.stand->asked.size() == 3) {
                QVERIFY(silent.stand->letInOnlyAfter(0));
            }
        };
        silent.instead.insert("/api/system/get_hostname"_ba, refusal(404, u"NotFoundError"_s));
        silent.instead.insert("/api/upgrade/version"_ba, refusal(404, u"NotFoundError"_s));
        added = addedBy(adding, typedAt(silent.stand->address()));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(added.value().name, u"127.0.0.1:%1"_s.arg(silent.stand->serverPort()));
        QVERIFY(added.value().model.isEmpty());
        QVERIFY(added.value().version.isEmpty());
        // And what was typed is its name, whatever it calls itself.
        MadeUpKvm typedOver;
        typedOver.instead.insert("/api/system/get_hostname"_ba, good({{u"hostname"_s, u"build-kvm"_s}}));
        added = addedBy(adding, typedAt(typedOver.stand->address(), u"The one by the door"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(added.value().name, u"The one by the door"_s);
        QCOMPARE(own.wall.kvms().size(), 4);
    }

    void whatWasTypedIsKeptByTheRules()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        KvmAdding adding(own.wall, quick());
        KvmAdding::Typed typed = typedAt(u"  HTTP://127.0.0.1:%1/kvm/#/  "_s.arg(kvm.stand->serverPort()));
        typed.name = u"  Build   server\t"_s;
        typed.group = u" Rack \t 2 "_s;
        typed.user = u"  admin "_s + sign(0x200B);
        const auto added = addedBy(adding, typed);
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        const OwnKvm kept = own.wall.kvms().constFirst();
        QCOMPARE(kept.name, u"Build   server"_s);
        QCOMPARE(added.value().name, u"Build   server"_s);
        QCOMPARE(kept.group, u"Rack 2"_s);
        QCOMPARE(kept.user, u"admin"_s);
        QCOMPARE(kept.address, kvm.stand->address());
        QCOMPARE(kvm.last(signInQuestion)->body, "user=admin&passwd=secret"_ba);
    }

    void aUserNameAndAPasswordGoOutAsTheyWereTyped()
    {
        OwnWall own;
        MadeUpKvm kvm;
        kvm.password = u" p+a s&s=w%rd# "_s + sign(0xE4);
        KvmAdding adding(own.wall, quick());
        KvmAdding::Typed typed = typedAt(kvm.stand->address());
        // (A password keeps the blanks around it: they may be part of it.)
        typed.password = kvm.password;
        const auto added = addedBy(adding, typed);
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(added.value().id)),
                 savedFor(kvm.password.toUtf8(), kvm.stand->address()));

        // Another user than the usual one is the one that is signed in as. (The made-up KVM
        // knows "admin" alone.)
        const MadeUpKvm other;
        typed = typedAt(other.stand->address());
        typed.user = u" root "_s;
        const auto refused = addedBy(adding, typed);
        QCOMPARE(refused.error().code, std::string("auth_failed"));
        QCOMPARE(other.last(signInQuestion)->body, "user=root&passwd=secret"_ba);
    }

    void aPasswordThatWasRefusedIsSentWhenItIsTypedAgain()
    {
        // A password a KVM refused waits a while before it is offered to it again (SignInPacing). One
        // that a person types is no such offer: it was made the KVM's own on the KVM itself meanwhile,
        // and typed again here it is sent at once.
        OwnWall own;
        MadeUpKvm kvm;
        KvmAdding adding(own.wall, quick());
        KvmAdding::Typed typed = typedAt(kvm.stand->address());
        typed.password = u"the new one"_s;
        const auto refused = addedBy(adding, typed);
        QCOMPARE(refused.error().code, std::string("auth_failed"));
        const int before = kvm.signIns;
        QVERIFY(before > 0);
        kvm.password = typed.password;
        const auto added = addedBy(adding, typed);
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(kvm.signIns, before + 1);
        QCOMPARE(kvm.last(signInQuestion)->body, "user=admin&passwd=the%20new%20one"_ba);
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
    }

    void aFieldThatIsRefusedEndsItBeforeAnythingIsAsked()
    {
        QFETCH(QString, field);
        QFETCH(QString, typed);
        QFETCH(QString, sentence);
        OwnWall own;
        const MadeUpKvm kvm;
        KvmAdding adding(own.wall, quick());
        KvmAdding::Typed all = typedAt(kvm.stand->address());
        if (field == u"name"_s) {
            all.name = typed;
        } else if (field == u"group"_s) {
            all.group = typed;
        } else if (field == u"user"_s) {
            all.user = typed;
        } else {
            all.password = typed;
        }
        const auto refused = addedBy(adding, all);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("invalid"));
        QCOMPARE(QString::fromStdString(refused.error().message), sentence);
        // The rule is said, and never what was typed.
        QVERIFY(!QString::fromStdString(refused.error().message).contains(typed));
        QTest::qWait(150);
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvm.stand->lines, 0);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
    }

    void aPasswordAsLongAsOneMayBeIsTaken()
    {
        OwnWall own;
        MadeUpKvm kvm;
        kvm.password = QString(core::kvm::longestPassword, u'p');
        KvmAdding adding(own.wall, quick());
        KvmAdding::Typed typed = typedAt(kvm.stand->address());
        typed.password = kvm.password;
        QVERIFY(addedBy(adding, typed).ok());
    }

    void theFieldsAreLookedAtInTheServersOrderAndBeforeTheAddress()
    {
        OwnWall own;
        KvmAdding adding(own.wall, quick());
        KvmAdding::Typed typed = typedAt(u"kvm lan"_s);
        typed.name = u"Build\nserver"_s;
        typed.user = QString(core::kvm::longestUser + 1, u'u');
        typed.password = QString(core::kvm::longestPassword + 1, u'p');
        typed.group = QString(core::kvm::longestGroup + 1, u'g');
        const auto said = [&adding, &typed] { return QString::fromStdString(addedBy(adding, typed).error().message); };
        QVERIFY(said().startsWith(u"name: "_s));
        typed.name.clear();
        QVERIFY(said().startsWith(u"username: "_s));
        typed.user.clear();
        QVERIFY(said().startsWith(u"password: "_s));
        typed.password = u"secret"_s;
        QVERIFY(said().startsWith(u"group: "_s));
        typed.group.clear();
        // And then what is no address, in the server's sentence for it.
        const auto refused = addedBy(adding, typed);
        QCOMPARE(refused.error().code, std::string("invalid"));
        QCOMPARE(refused.error().message, std::string("Not a usable device address: 'kvm lan'"));
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void nothingAtTheAddressIsUnreachable()
    {
        OwnWall own;
        const core::testing::NobodyThere nobody;
        KvmAdding adding(own.wall, quick());
        auto refused = addedBy(adding, typedAt(nobody.address()));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("unreachable"));
        QCOMPARE(refused.error().message, std::string("Device is unreachable"));
        // Nor where a KVM answers the plain way and HTTPS was typed, or nothing: it is not gone
        // to in the clear by itself, and hears no password. Typed with nothing before it, the plain way
        // beside it is asked whether a JetKVM is there (M6, as the web asks), and that alone: one
        // question, with nothing in it that says who asks.
        const MadeUpKvm plain;
        refused = addedBy(adding, typedAt(u"127.0.0.1:%1"_s.arg(plain.stand->serverPort())));
        QCOMPARE(refused.error().code, std::string("unreachable"));
        QCOMPARE(refused.error().message, std::string("Device is unreachable"));
        QCOMPARE(plain.stand->asked.size(), 1);
        QCOMPARE(plain.stand->asked.constFirst().path, QByteArray(jetKvmsQuestion));
        QCOMPARE(plain.stand->asked.constFirst().method, QByteArray("GET"));
        QVERIFY(plain.stand->asked.constFirst().body.isEmpty());
        QVERIFY(plain.stand->asked.constFirst().headers.value("cookie").isEmpty());
        QCOMPARE(plain.signIns, 0);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
    }

    void whatIsNoKvmOfThisProgramsIsSaidAndHearsNoPassword_data()
    {
        QTest::addColumn<QByteArray>("what");
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<QByteArray>("code");
        QTest::addColumn<QString>("sentence");
        QTest::addColumn<int>("slowTls"); // ms before the stand-in lets the TLS handshake begin
        const QString page = u"Something answers at this address, but it is not a KVM that this program speaks to"_s;
        QTest::newRow("a page") << "page"_ba << false << "not_a_kvm"_ba << page << 0;
        QTest::newRow("a page over HTTPS") << "page"_ba << true << "not_a_kvm"_ba << page << 0;
        // (A TLS handshake slower than the fixture's former 1.5 s connect limit, as one under the address
        // sanitizer on a busy PC was seen to be, in gate 4 of Checkpoint 2: within the program's own limit.)
        // A server's controller is signed in to from M7 1b-i on (test_wall_servers).
        QTest::newRow("a page over slow HTTPS") << "page"_ba << true << "not_a_kvm"_ba << page << 1800;
    }

    void whatIsNoKvmOfThisProgramsIsSaidAndHearsNoPassword()
    {
        QFETCH(QByteArray, what);
        QFETCH(bool, encrypted);
        QFETCH(QByteArray, code);
        QFETCH(QString, sentence);
        QFETCH(int, slowTls);
        const std::function<Answered(const Asked&)> answer = what == "page" ? asAPage : asAController;
        const auto stand =
            encrypted ? std::make_unique<StandIn>(answer, tls("first")) : std::make_unique<StandIn>(answer);
        if (slowTls > 0) {
            QVERIFY(stand->letInOnlyAfter(slowTls));
        }
        OwnWall own;
        KvmAdding adding(own.wall, quick());
        const auto refused = addedBy(adding, typedAt(stand->address(), u"Build server"_s));
        QVERIFY(!refused.ok());
        QCOMPARE(QByteArray::fromStdString(refused.error().code), code);
        QCOMPARE(QString::fromStdString(refused.error().message), sentence);
        // Asked what it is, and nothing after that.
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*stand));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
    }

    void aKvmThatIsOnTheWallIsNotAddedTwice()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        KvmAdding adding(own.wall, quick());
        QVERIFY(addedBy(adding, typedAt(kvm.stand->address(), u"One"_s)).ok());
        const qsizetype asked = kvm.stand->asked.size();
        // However its address is typed the second time: it is written the one way.
        for (const QString& again : {kvm.stand->address(), u"HTTP://127.0.0.1:%1/#/"_s.arg(kvm.stand->serverPort()),
                                     u" http://127.0.0.1.:%1 "_s.arg(kvm.stand->serverPort())}) {
            const auto twice = addedBy(adding, typedAt(again, u"The same again"_s));
            QVERIFY(!twice.ok());
            QCOMPARE(twice.error().code, std::string("duplicate"));
            QCOMPARE(twice.error().message, std::string("This device is already on the wall"));
        }
        // It was not asked anything for that, and not signed in to.
        QTest::qWait(150);
        QCOMPARE(kvm.stand->asked.size(), asked);
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.secrets.all.size(), 1);
    }

    void aKvmThatIsOnTheWallIsNotAddedASecondTimeTheOtherWay()
    {
        // Over HTTPS on the wall, and typed with "http://": its password is not sent a second
        // way, in the clear. Whatever answers there in the clear is not even asked what it is.
        OwnWall own;
        const MadeUpKvm kvm(tls("first"));
        KvmAdding adding(own.wall, quick());
        QVERIFY(addedBy(adding, typedAt(kvm.stand->address(), u"One"_s)).ok());
        const qsizetype asked = kvm.stand->asked.size();
        const auto inTheClear =
            addedBy(adding, typedAt(u"http://127.0.0.1:%1"_s.arg(kvm.stand->serverPort()), u"The same again"_s));
        QVERIFY(!inTheClear.ok());
        QCOMPARE(inTheClear.error().code, std::string("duplicate"));
        QCOMPARE(inTheClear.error().message, std::string("This device is already on the wall"));
        // And the other way round: in the clear on the wall, and typed with nothing before it,
        // which is HTTPS.
        const MadeUpKvm plain;
        QVERIFY(addedBy(adding, typedAt(plain.stand->address(), u"Two"_s)).ok());
        const qsizetype askedOfIt = plain.stand->asked.size();
        const auto encrypted = addedBy(adding, typedAt(u"127.0.0.1:%1"_s.arg(plain.stand->serverPort()), u"Again"_s));
        QVERIFY(!encrypted.ok());
        QCOMPARE(encrypted.error().code, std::string("duplicate"));
        QTest::qWait(150);
        QCOMPARE(kvm.stand->asked.size(), asked);
        QCOMPARE(plain.stand->asked.size(), askedOfIt);
        QCOMPARE(own.wall.kvms().size(), 2);
        QCOMPARE(own.secrets.all.size(), 2);
    }

    void theSameKvmAtAnotherAddressIsSaidAndNotAddedTwice()
    {
        // One KVM reached two ways, on the network it stands in and over a VPN: it shows the one
        // certificate it made for itself at both.
        OwnWall own;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm there(tls("first"));
        KvmAdding adding(own.wall, quick());
        QVERIFY(addedBy(adding, typedAt(here.stand->address(), u"Build server"_s)).ok());
        auto refused = addedBy(adding, typedAt(there.stand->address(), u"Over the VPN"_s));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("same_device"));
        QCOMPARE(QString::fromStdString(refused.error().message),
                 u"This is the same device as \"Build server\", which is on the wall already (127.0.0.1:%1)"_s.arg(
                     here.stand->serverPort()));
        // Said before its password went out a second way.
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*there.stand));
        QCOMPARE(there.signIns, 0);
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.secrets.all.size(), 1);

        // One that is called by where it is, is named by that once.
        OwnWall other;
        KvmAdding again(other.wall, quick());
        QVERIFY(addedBy(again, typedAt(here.stand->address())).ok());
        refused = addedBy(again, typedAt(there.stand->address()));
        QCOMPARE(refused.error().code, std::string("same_device"));
        QCOMPARE(
            QString::fromStdString(refused.error().message),
            u"This is the same device as 127.0.0.1:%1, which is on the wall already"_s.arg(here.stand->serverPort()));

        // And a KVM that shows another certificate is another KVM.
        const MadeUpKvm another(tls("second"));
        QVERIFY(addedBy(adding, typedAt(another.stand->address())).ok());
        QCOMPARE(own.wall.kvms().size(), 2);
    }

    void kvmsThatAreKnownByNoCertificateAreNotTakenForOneAnother()
    {
        // Behind somebody's front door the certificate is the door's, and many a KVM may stand
        // behind one door: by that nothing is known again.
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        OwnWall own;
        const MadeUpKvm one(tls("signed"));
        const MadeUpKvm other(tls("signed"));
        KvmAdding adding(own.wall, quick());
        QVERIFY(addedBy(adding, typedAt(one.stand->address())).ok());
        QVERIFY(addedBy(adding, typedAt(other.stand->address())).ok());
        QCOMPARE(own.wall.kvms().size(), 2);
    }

    void aWrongPasswordAddsNothingAndKeepsNothing()
    {
        OwnWall own;
        const MadeUpKvm kvm(tls("first"));
        KvmAdding adding(own.wall, quick());
        const QSignalSpy changed(&own.wall, &LocalWall::changed);
        KvmAdding::Typed typed = typedAt(kvm.stand->address(), u"Build server"_s);
        typed.password = u"not it"_s;
        const auto refused = addedBy(adding, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("auth_failed"));
        QCOMPARE(refused.error().message, std::string("Login rejected - check the username and password"));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
        QVERIFY(changed.isEmpty());
        QVERIFY(own.settings.remembered().kvms.isEmpty());
        // Tried once, and not again: a KVM locks an account out.
        QTest::qWait(300);
        QCOMPARE(kvm.signIns, 1);
        // With the right one it is added after all.
        QVERIFY(addedBy(adding, typedAt(kvm.stand->address(), u"Build server"_s)).ok());
        QCOMPARE(own.secrets.all.size(), 1);
    }

    void withoutAPasswordNothingGoesOutButTheQuestions()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        KvmAdding adding(own.wall, quick());
        KvmAdding::Typed typed = typedAt(kvm.stand->address());
        typed.password.clear();
        const auto refused = addedBy(adding, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("no_password"));
        QCOMPARE(refused.error().message, std::string("No password saved for this device"));
        QTest::qWait(150);
        QVERIFY(heardOnlyTheQuestions(*kvm.stand));
        QCOMPARE(kvm.stand->lines, 3);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
    }

    void whatGoesWrongAtTheSignInIsSaidInItsSentence_data()
    {
        QTest::addColumn<QByteArray>("how");
        QTest::addColumn<QByteArray>("code");
        QTest::addColumn<QByteArray>("sentence");
        // (What went wrong is said inside the sentence of a sign-in that let the box nowhere, as the web
        // says it: under a code that is no word that something may have been done. And not that it is asked
        // again shortly, as a card says it: a box asks once.)
        const auto nowhere = [](const char* why) {
            return "The device does not sign qKVM in (" + QByteArray(why) + ").";
        };
        QTest::newRow("an error of its own") << "error"_ba << "no_sign_in"_ba << nowhere("Login failed (HTTP 500)");
        QTest::newRow("no pass") << "no pass"_ba << "no_sign_in"_ba
                                 << nowhere("Login succeeded but the device returned no session token");
        QTest::newRow("it hangs up") << "hangs up"_ba << "no_sign_in"_ba << nowhere("The device closed the connection");
        QTest::newRow("it never answers")
            << "silent"_ba << "no_sign_in"_ba << nowhere("Device did not respond in time");
        QTest::newRow("it refuses the session it gave")
            << "refuses"_ba << "auth_failed"_ba << "Device keeps rejecting the session"_ba;
    }

    void whatGoesWrongAtTheSignInIsSaidInItsSentence()
    {
        QFETCH(QByteArray, how);
        QFETCH(QByteArray, code);
        QFETCH(QByteArray, sentence);
        OwnWall own;
        MadeUpKvm kvm;
        Answered amiss;
        if (how == "error") {
            kvm.signInStatus = 500;
        } else if (how == "no pass") {
            kvm.instead.insert(signInQuestion, good());
        } else if (how == "hangs up") {
            amiss.hangUp = true;
            kvm.instead.insert(signInQuestion, amiss);
        } else if (how == "silent") {
            amiss.never = true;
            kvm.instead.insert(signInQuestion, amiss);
        } else {
            // (Signed in, refused, signed in once more, refused again: and that is all.)
            kvm.instead.insert("/api/upgrade/version"_ba, refusal(401, u"UnauthorizedError"_s));
        }
        KvmAdding adding(own.wall, quick());
        const auto refused = addedBy(adding, typedAt(kvm.stand->address()));
        QVERIFY(!refused.ok());
        QCOMPARE(QByteArray::fromStdString(refused.error().code), code);
        QCOMPARE(QByteArray::fromStdString(refused.error().message), sentence);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
        QVERIFY(!adding.isUnderWay());
    }

    void whatAnswersInItsPlaceAtTheSignInHearsNoPassword()
    {
        // The KVM answers the probe's question and shows its certificate. Before it is signed in
        // to, something else stands at its address, with another certificate: that is not the
        // KVM the password was typed for.
        OwnWall own;
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        // (It leaves the two questions that are not its own open, so that the probe is still
        // waiting when the other takes its place.)
        Answered silent;
        silent.never = true;
        kvm->instead.insert(jetKvmsQuestion, silent);
        kvm->instead.insert(redfishsQuestion, silent);
        // (And the probe has patience with them, however busy this PC is: the test ends them.)
        KvmAdding::Pace pace = quick();
        pace.probe.connectWithinMs = 6000;
        pace.probe.quietMs = 8000;
        pace.probe.withinMs = 12'000;
        KvmAdding adding(own.wall, pace);
        QList<base::Result<KvmAdding::Added>> told;
        adding.add(typedAt(kvm->stand->address()),
                   [&told](const base::Result<KvmAdding::Added>& result) { told.append(result); });
        QTRY_COMPARE(kvm->stand->asked.size(), 3);
        QTest::qWait(300); // (its answer to the one question has arrived by now)
        QVERIFY(told.isEmpty());
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        StandIn other([](const Asked&) { return Answered{200, "{}"_ba}; }, tls("second"), port);
        QVERIFY(other.isListening());

        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QVERIFY(!told.constFirst().ok());
        QCOMPARE(told.constFirst().error().code, std::string("cert_mismatch"));
        QCOMPARE(told.constFirst().error().message,
                 std::string("Certificate does not match the one saved for this device"));
        // Whatever of the probe's questions it was asked again: no sign-in, and nothing that
        // carries anything.
        QTest::qWait(300);
        QCOMPARE(other.count(signInQuestion), 0);
        for (const Asked& one : std::as_const(other.asked)) {
            QCOMPARE(one.method, "GET"_ba);
            QVERIFY(one.body.isEmpty());
            QVERIFY(!one.headers.contains("cookie"));
        }
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
    }

    void whatCannotBeWrittenDownIsNotAdded()
    {
        // The settings are to go where no file can be: under a file.
        const QTemporaryDir folder;
        QFile blocking(folder.filePath(u"in-the-way"_s));
        QVERIFY(blocking.open(QIODevice::WriteOnly));
        blocking.close();
        core::Settings settings(folder.filePath(u"in-the-way/settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickDriver());
        const MadeUpKvm kvm;
        KvmAdding adding(wall, quick());
        auto refused = addedBy(adding, typedAt(kvm.stand->address(), u"Build server"_s));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("not_saved"));
        QVERIFY(QString::fromStdString(refused.error().message).startsWith(u"The new device could not be saved ("_s));
        QVERIFY(QString::fromStdString(refused.error().message).endsWith(u"), so nothing was changed."_s));
        QVERIFY(wall.kvms().isEmpty());
        QVERIFY(secrets.all.isEmpty());

        // Nor where its password cannot be kept.
        core::Settings writable(folder.filePath(u"settings.json"_s));
        MemorySecrets refusing;
        refusing.refuses = true;
        LocalWall other(writable, refusing, quickDriver());
        KvmAdding again(other, quick());
        refused = addedBy(again, typedAt(kvm.stand->address(), u"Build server"_s));
        QCOMPARE(refused.error().code, std::string("not_saved"));
        QVERIFY(other.kvms().isEmpty());
    }

    void aWallThatIsFullTakesNoMore()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::Remembered full = settings.remembered();
        for (qsizetype index = 0; index < core::Remembered::mostKvms; ++index) {
            OwnKvm one;
            one.id = u"%1"_s.arg(index, 8, 16, QLatin1Char('0'));
            // (Nobody is at any of these, and none of them is ever asked: nothing looks at this wall.)
            one.address = u"http://kvm-%1.invalid"_s.arg(index);
            full.kvms.append(one);
        }
        QVERIFY(settings.remember(full).ok());
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickDriver());
        QCOMPARE(wall.kvms().size(), core::Remembered::mostKvms);
        const MadeUpKvm kvm;
        KvmAdding adding(wall, quick());
        const auto refused = addedBy(adding, typedAt(kvm.stand->address()));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("too_many"));
        QCOMPARE(refused.error().message, std::string("This wall is full."));
        QCOMPARE(wall.kvms().size(), core::Remembered::mostKvms);
        QVERIFY(secrets.all.isEmpty());
    }

    void aSecondAddingWhileOneIsUnderWayIsRefused()
    {
        OwnWall own;
        MadeUpKvm kvm;
        Answered late = good({{u"hostname"_s, u"glkvm"_s}});
        late.afterMs = 600;
        kvm.first["/api/system/get_hostname"_ba].append(late);
        const MadeUpKvm beside;
        KvmAdding adding(own.wall, quick());
        QList<base::Result<KvmAdding::Added>> toldTheFirst;
        QList<base::Result<KvmAdding::Added>> toldTheSecond;
        adding.add(typedAt(kvm.stand->address(), u"One"_s),
                   [&toldTheFirst](const base::Result<KvmAdding::Added>& result) { toldTheFirst.append(result); });
        QVERIFY(adding.isUnderWay());
        adding.add(typedAt(beside.stand->address(), u"Beside"_s),
                   [&toldTheSecond](const base::Result<KvmAdding::Added>& result) { toldTheSecond.append(result); });
        // Not there and then, either.
        QVERIFY(toldTheSecond.isEmpty());
        QTRY_COMPARE(toldTheSecond.size(), 1);
        QVERIFY(!toldTheSecond.constFirst().ok());
        QCOMPARE(toldTheSecond.constFirst().error().code, std::string("busy"));
        QCOMPARE(toldTheSecond.constFirst().error().message,
                 std::string("Another KVM is being added at this moment. Wait for it to finish."));
        // The first goes on as if nothing had been asked.
        QVERIFY(toldTheFirst.isEmpty());
        QVERIFY(adding.isUnderWay());
        QTRY_COMPARE_WITH_TIMEOUT(toldTheFirst.size(), 1, 20000);
        QVERIFY(toldTheFirst.constFirst().ok());
        QCOMPARE(toldTheFirst.constFirst().value().name, u"One"_s);
        QVERIFY(!adding.isUnderWay());
        // Of the second's address nothing was asked, and nothing of it is anywhere.
        QTest::qWait(150);
        QVERIFY(beside.stand->asked.isEmpty());
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.secrets.all.size(), 1);
        QCOMPARE(toldTheSecond.size(), 1);
        // And after it the next.
        QVERIFY(addedBy(adding, typedAt(beside.stand->address(), u"Beside"_s)).ok());
        QCOMPARE(own.wall.kvms().size(), 2);
    }

    void whoeverIsToldMayBeginTheNextThereAndThen()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        const MadeUpKvm next;
        KvmAdding adding(own.wall, quick());
        QList<base::Result<KvmAdding::Added>> told;
        adding.add(typedAt(kvm.stand->address(), u"One"_s), [&](const base::Result<KvmAdding::Added>& result) {
            told.append(result);
            adding.add(typedAt(next.stand->address(), u"Next"_s),
                       [&told](const base::Result<KvmAdding::Added>& after) { told.append(after); });
        });
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 2, 20000);
        QVERIFY(told.at(0).ok());
        QVERIFY2(told.at(1).ok(), told.at(1).ok() ? "" : told.at(1).error().message.c_str());
        QCOMPARE(told.at(1).value().name, u"Next"_s);
        QCOMPARE(own.wall.kvms().size(), 2);
    }

    void twoAddingsOfOneKvmAtOnceAddItOnce()
    {
        // Two boxes open on one wall. Both look at the wall before the KVM is spoken to, and
        // both find it free: so each looks again before it adds.
        const auto addedAndRefused = [](LocalWall& wall, const QString& one, const QString& other) {
            // This checks concurrent identity/deduplication, not deadlines. Six concurrent TLS
            // probe handshakes can exhaust quick() under parallel ASAN load before sign-in.
            // Exercise the real admission budget; timeout behavior has separate tests.
            KvmAdding first(wall);
            KvmAdding second(wall);
            QList<base::Result<KvmAdding::Added>> told;
            const auto note = [&told](const base::Result<KvmAdding::Added>& result) { told.append(result); };
            first.add(typedAt(one, u"Build server"_s), note);
            second.add(typedAt(other, u"Build server"_s), note);
            static_cast<void>(QTest::qWaitFor([&told] { return told.size() == 2; }, 20000));
            QStringList codes;
            for (const auto& result : std::as_const(told)) {
                codes.append(result.ok() ? u"added"_s : QString::fromStdString(result.error().code));
            }
            std::ranges::sort(codes);
            return codes;
        };
        // At one address:
        OwnWall own;
        const MadeUpKvm kvm;
        // (One password at a time to one KVM: the other waits for the first's sign-in while that is out, as the web's
        // second asker waits for the one sign-in, then signs in itself, and finds it added.)
        QCOMPARE(addedAndRefused(own.wall, kvm.stand->address(), kvm.stand->address()),
                 (QStringList{u"added"_s, u"duplicate"_s}));
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.secrets.all.size(), 1);
        // And at two addresses of the one KVM:
        OwnWall another;
        const MadeUpKvm here(tls("first"));
        const MadeUpKvm there(tls("first"));
        QCOMPARE(addedAndRefused(another.wall, here.stand->address(), there.stand->address()),
                 (QStringList{u"added"_s, u"same_device"_s}));
        QCOMPARE(another.wall.kvms().size(), 1);
        QCOMPARE(another.secrets.all.size(), 1);
        // (The pacing's third reading.) And with a password the KVM refuses: the second waits for the first's no, which
        // is its no too. One strike, not two (sent beside it, the two above would have added it all the same).
        OwnWall third;
        MadeUpKvm refusing;
        refusing.password = u"changed on the KVM"_s;
        refusing.signInAfterMs = 500;
        QCOMPARE(addedAndRefused(third.wall, refusing.stand->address(), refusing.stand->address()),
                 (QStringList{u"auth_failed"_s, u"auth_failed"_s}));
        QCOMPARE(refusing.signIns, 1);
        QVERIFY(third.wall.kvms().isEmpty());
    }

    void theResultComesLaterAndOnce()
    {
        OwnWall own;
        const MadeUpKvm kvm;
        KvmAdding adding(own.wall, quick());
        QList<base::Result<KvmAdding::Added>> told;
        const auto note = [&told](const base::Result<KvmAdding::Added>& result) { told.append(result); };
        // Also where there is nothing to wait for: what was typed is no address.
        adding.add(typedAt(u"kvm lan"_s), note);
        QVERIFY(told.isEmpty());
        QVERIFY(adding.isUnderWay());
        QTRY_COMPARE(told.size(), 1);
        QVERIFY(!adding.isUnderWay());
        // A KVM that is added, and the same again, which is on the wall already.
        adding.add(typedAt(kvm.stand->address()), note);
        QCOMPARE(told.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 2, 20000);
        QVERIFY(told.at(1).ok());
        adding.add(typedAt(kvm.stand->address()), note);
        QCOMPARE(told.size(), 2);
        QTRY_COMPARE(told.size(), 3);
        QCOMPARE(told.at(2).error().code, std::string("duplicate"));
        QTest::qWait(400);
        QCOMPARE(told.size(), 3);
    }

    void anAddingThatIsGivenUpAddsNothingAndSaysSoOnce()
    {
        OwnWall own;
        QList<std::string> told;
        const auto note = [&told](const base::Result<KvmAdding::Added>& added) {
            told.append(added.ok() ? std::string("added") : added.error().code);
        };
        KvmAdding adding(own.wall, quick());
        // Nothing is under way: nothing is given up, and nobody is told anything.
        adding.giveUp();
        QTest::qWait(50);
        QVERIFY(told.isEmpty());

        // Given up while its questions are out: the answer that comes later reaches nothing.
        MadeUpKvm probing;
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 500;
        probing.instead.insert(kvmdsQuestion, late);
        adding.add(typedAt(probing.stand->address()), note);
        QTRY_COMPARE(probing.stand->asked.size(), 3);
        adding.giveUp();
        // Told later, and as what it is; given up twice is given up once.
        QVERIFY(told.isEmpty());
        QVERIFY(adding.isUnderWay());
        adding.giveUp();
        QTRY_COMPARE(told, QList<std::string>{"given_up"});
        QVERIFY(!adding.isUnderWay());
        // The KVM's answer comes, with nothing under way: it is nobody's, and nobody is told again.
        QTest::qWait(800);
        QCOMPARE(told, QList<std::string>{"given_up"});
        QCOMPARE(probing.asked(signInQuestion), 0);
        QVERIFY(own.wall.kvms().isEmpty());

        // Given up while the KVM is being signed in to, and the next one begun at once: the first
        // one's sign-in, which the KVM answers later, is nothing to the second.
        MadeUpKvm signingIn;
        Answered lateIn = good({{u"token"_s, u"pass1"_s}});
        lateIn.afterMs = 500;
        signingIn.first[signInQuestion].append(lateIn);
        adding.add(typedAt(signingIn.stand->address()), note);
        QTRY_COMPARE(signingIn.asked(signInQuestion), 1);
        adding.giveUp();
        QTRY_COMPARE(told.size(), 2);
        QCOMPARE(told.constLast(), std::string("given_up"));
        const MadeUpKvm next;
        adding.add(typedAt(next.stand->address(), u"The next one"_s), note);
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 3, 20000);
        QCOMPARE(told.constLast(), std::string("added"));
        // (Longer than either of the first two was to take over its answer.)
        QTest::qWait(900);
        QCOMPARE(told.size(), 3);
        // Only the last is on the wall, with its password and no other.
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.wall.kvms().constFirst().name, u"The next one"_s);
        QCOMPARE(own.secrets.all.size(), 1);
        QCOMPARE(probing.asked(signInQuestion), 0);
        QCOMPARE(signingIn.asked(signInQuestion), 1);
        QCOMPARE(signingIn.asked("/api/system/get_hostname"), 0);
    }

    void whoeverGivesUpWhenTheWallSaysItHasChangedIsToldThatTheKvmWasAdded()
    {
        // The wall says aloud that it has changed while the KVM is put on it. Giving up then is
        // too late: it is on the wall, and that is what is told, once.
        OwnWall own;
        const MadeUpKvm kvm;
        KvmAdding adding(own.wall, quick());
        QList<std::string> told;
        connect(&own.wall, &LocalWall::changed, &adding, [&adding] { adding.giveUp(); });
        adding.add(typedAt(kvm.stand->address(), u"Build server"_s),
                   [&told](const base::Result<KvmAdding::Added>& added) {
                       told.append(added.ok() ? std::string("added") : added.error().code);
                   });
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 20000);
        QTest::qWait(200);
        QCOMPARE(told, QList<std::string>{"added"});
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.secrets.all.size(), 1);
        QVERIFY(!adding.isUnderWay());
    }

    void anAddingThatIsGoneAddsNothingAndTellsNobody()
    {
        OwnWall own;
        int told = 0;
        const auto count = [&told](const base::Result<KvmAdding::Added>&) { ++told; };
        // Gone while its questions are out.
        MadeUpKvm probing;
        Answered late = refusal(401, u"UnauthorizedError"_s);
        late.afterMs = 800;
        probing.instead.insert(kvmdsQuestion, late);
        {
            KvmAdding adding(own.wall, quick());
            adding.add(typedAt(probing.stand->address()), count);
            QTRY_COMPARE(probing.stand->asked.size(), 3);
        }
        // Gone while the KVM is being signed in to.
        MadeUpKvm signingIn;
        Answered lateIn = good({{u"token"_s, u"pass1"_s}});
        lateIn.afterMs = 800;
        signingIn.first[signInQuestion].append(lateIn);
        {
            KvmAdding adding(own.wall, quick());
            adding.add(typedAt(signingIn.stand->address()), count);
            QTRY_COMPARE(signingIn.asked(signInQuestion), 1);
        }
        const qsizetype askedOfIt = signingIn.stand->asked.size();
        // Gone before a question was out at all, and gone with a refusal on its way.
        const MadeUpKvm untouched;
        {
            KvmAdding adding(own.wall, quick());
            adding.add(typedAt(untouched.stand->address()), count);
        }
        {
            KvmAdding adding(own.wall, quick());
            adding.add(typedAt(u"kvm lan"_s), count);
            adding.add(typedAt(untouched.stand->address()), count);
        }
        // (Longer than either of the two was to take over its answer.)
        QTest::qWait(1300);
        QCOMPARE(told, 0);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.isEmpty());
        // And none of the KVMs heard anything after that: no sign-in, and nothing with a pass.
        QCOMPARE(probing.asked(signInQuestion), 0);
        QCOMPARE(signingIn.stand->asked.size(), askedOfIt);
        QCOMPARE(untouched.asked(signInQuestion), 0);
    }

    void whoeverHearsTheWallSayThatItChangedMayEndTheAdding()
    {
        // The wall says so while it adds, in the middle of the adding's last step: a box that
        // closes on that word takes the adding with it.
        OwnWall own;
        const MadeUpKvm kvm;
        auto adding = std::make_unique<KvmAdding>(own.wall, quick());
        int told = 0;
        connect(&own.wall, &LocalWall::changed, this, [&adding] { adding.reset(); });
        adding->add(typedAt(kvm.stand->address(), u"Build server"_s),
                    [&told](const base::Result<KvmAdding::Added>&) { ++told; });
        QTRY_VERIFY_WITH_TIMEOUT(adding == nullptr, 20000);
        QTest::qWait(300);
        // Nobody is told by what is gone. The KVM is on the wall, as the wall itself said.
        QCOMPARE(told, 0);
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.wall.kvms().constFirst().name, u"Build server"_s);
        QCOMPARE(own.secrets.all.size(), 1);
    }

    // --- a JetKVM, the second family this program speaks to by itself -------------------------------

    void aJetKvmThatSaysItIsOneOfTheWallIsKeptBesideItWhereThatIsSaid()
    {
        // From the reading of the twins (L3), as the web asks it (keep_both): a twin by the number a JetKVM gives is
        // a question to the person, who may keep both; by a certificate, it stays a refusal.
        OwnWall own;
        JetKvmByHand here;
        JetKvmByHand there;
        there.deviceId = here.deviceId;
        JetKvmByHand third;
        third.deviceId = here.deviceId;
        KvmAdding adding(own.wall, quick());
        QVERIFY(addedBy(adding, typedAt(here.address().toString(), u"Rack JetKVM"_s)).ok());
        const auto refused = addedBy(adding, typedAt(there.address().toString()));
        QVERIFY(!refused.ok());
        QVERIFY(core::LocalWall::saysItIsTheSame(refused.error()));
        KvmAdding::Typed both = typedAt(there.address().toString(), u"Over the VPN"_s);
        both.keepBoth = true;
        const auto kept = addedBy(adding, both);
        QVERIFY2(kept.ok(), kept.ok() ? "" : kept.error().message.c_str());
        QCOMPARE(own.wall.kvms().size(), 2);
        QVERIFY(!own.wall.kvms().at(0).keptBesideItsTwin);
        QVERIFY(own.wall.kvms().at(1).keptBesideItsTwin);
        // A third, not said to be kept: the first's twin again.
        QCOMPARE(codeOf(addedBy(adding, typedAt(third.address().toString()))), std::string("same_device"));
        // By a certificate: refused, also where both were to be kept.
        JetKvmByHand secure(tls("first"));
        JetKvmByHand secureToo(tls("first"));
        QVERIFY(addedBy(adding, typedAt(secure.address().toString())).ok());
        KvmAdding::Typed proven = typedAt(secureToo.address().toString());
        proven.keepBoth = true;
        const auto byCertificate = addedBy(adding, proven);
        QCOMPARE(codeOf(byCertificate), std::string("same_device"));
        QVERIFY(!core::LocalWall::saysItIsTheSame(byCertificate.error()));
        QCOMPARE(own.wall.kvms().size(), 3);
    }

    void oneJetKvmUnderTwoAddressesIsOneDeviceByTheNumberItGives()
    {
        // As the web's (test_one_jetkvm_under_two_addresses_is_one_device): over plain HTTP a JetKVM shows no
        // certificate, and one found on the network and again over a VPN was on the wall twice. It gives a
        // number for itself: a second card of it is refused, in the web's words for what is said and not
        // proven. Another JetKVM is another device; two that give no number fit to be one are two.
        OwnWall own;
        JetKvmByHand here;
        JetKvmByHand there;
        there.deviceId = here.deviceId;
        JetKvmByHand other;
        KvmAdding adding(own.wall, quick());
        const auto added = addedBy(adding, typedAt(here.address().toString(), u"Rack JetKVM"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        const auto again = addedBy(adding, typedAt(there.address().toString()));
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, std::string("same_device"));
        QCOMPARE(
            QString::fromStdString(again.error().message),
            u"This says it is the same device as \"Rack JetKVM\", which is on the wall already (127.0.0.1:%1)"_s.arg(
                here.stand->serverPort()));
        QCOMPARE(own.wall.kvms().size(), 1);
        const auto another = addedBy(adding, typedAt(other.address().toString()));
        QVERIFY2(another.ok(), another.ok() ? "" : another.error().message.c_str());
        QCOMPARE(own.wall.kvms().size(), 2);
        // Kept as a digest: its number itself is nobody's business.
        const QString kept = own.wall.kvms().constFirst().number;
        QVERIFY(kept.startsWith(u"jetkvm:"_s));
        QFile written(own.folder.filePath(u"settings.json"_s));
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QByteArray text = written.readAll();
        written.close(); // (held open, the next writing of the settings is refused on Windows)
        QVERIFY(text.contains(kept.toUtf8()));
        QVERIFY(!text.contains(here.deviceId.toString().toUtf8()));
        // Two that give no number fit to be one are two.
        JetKvmByHand bare;
        bare.deviceId = u"short"_s;
        JetKvmByHand bareToo;
        bareToo.deviceId = u"short"_s;
        const auto bareAdded = addedBy(adding, typedAt(bare.address().toString()));
        QVERIFY2(bareAdded.ok(), bareAdded.ok() ? "" : bareAdded.error().message.c_str());
        const auto bareTooAdded = addedBy(adding, typedAt(bareToo.address().toString()));
        QVERIFY2(bareTooAdded.ok(), bareTooAdded.ok() ? "" : bareTooAdded.error().message.c_str());
        QCOMPARE(own.wall.kvms().size(), 4);
        QVERIFY(own.wall.kvms().at(2).number.isEmpty());
        QVERIFY(own.wall.kvms().at(3).number.isEmpty());
    }

    void aJetKvmWhoseNumberCouldNotBeReadIsNotAdded()
    {
        // J5, as the web's identify (`taking`): an addition never takes a number it could not read (GET /device
        // answered 500 this once). Which JetKVM answers could not be told: nothing is added, and no line was asked for.
        OwnWall own;
        JetKvmByHand jet;
        jet.deviceFails = true;
        KvmAdding adding(own.wall, quick());
        const auto refused = addedBy(adding, typedAt(jet.address().toString(), u"Rack JetKVM"_s));
        QCOMPARE(codeOf(refused), std::string("bad_response"));
        QVERIFY(refused.error().message.starts_with("Which JetKVM answers at this address could not be told: "));
        QVERIFY(own.wall.kvms().isEmpty());
        QCOMPARE(jet.lineAskings, 0);
    }

    void aJetKvmIsAddedBySigningInToItAsAJetKvmIsSignedInTo()
    {
        OwnWall own;
        JetKvmByHand jet;
        KvmAdding adding(own.wall, quick());
        const QString address = jet.address().toString();
        const auto added = addedBy(adding, typedAt(address));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        // What it says it is: a JetKVM of its version. It calls itself nothing, so it is called by
        // where it is; spoken to in the clear, and known by no certificate.
        QCOMPARE(added.value().model, u"JetKVM"_s);
        QCOMPARE(added.value().version, u"0.4.8"_s);
        QCOMPARE(added.value().name, u"127.0.0.1:%1"_s.arg(jet.stand->serverPort()));
        QVERIFY(added.value().overPlainHttp);
        QVERIFY(added.value().certificate.isEmpty());
        QCOMPARE(own.wall.kvms().size(), 1);
        const OwnKvm kvm = own.wall.kvms().constFirst();
        QCOMPARE(kvm.kind, u"jetkvm"_s);
        QCOMPARE(kvm.address, address);
        QVERIFY(!kvm.knownByCertificate);
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(kvm.id)), savedFor("secret", address));
        // Asked what it is first; then signed in to once, as a JetKVM is (its password as JSON), and
        // let in on its signalling line with the session it gave. Never as a kvmd is signed in to. (Whether it
        // was set up is asked by the box; its driver asks which it is before the sign-in over plain HTTP, with no
        // session, since the merged review's 4.)
        QCOMPARE(jet.asked(jetKvmsQuestion), 1);
        QCOMPARE(jet.asked("/device"), 2);
        QCOMPARE(jet.stand->count(kvmdsQuestion), 1);
        QCOMPARE(jet.stand->count(signInQuestion), 0);
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(jet.signInBodies, QList<QByteArray>{R"({"password":"secret"})"_ba});
        QCOMPARE(jet.lineCookies, QList<QByteArray>{"authToken=jet.1"_ba});
        QCOMPARE(jet.linesIn, 1);
        // On the wall, its own driver looks at it: online, and signed in to by nobody for that.
        QCOMPARE(settled(own.wall, kvm.id).state, u"online"_s);
        QCOMPARE(jet.asked(jetKvmsQuestion), 2);
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(jet.stand->count(signInQuestion), 0);
    }

    void aJetKvmOverHttpsIsKnownByTheCertificateItShowed_data()
    {
        QTest::addColumn<int>("slowTls"); // ms before the stand-in lets the TLS handshake begin
        QTest::newRow("over HTTPS") << 0;
        // (A TLS handshake slower than the fixture's former 1.5 s connect limit, as one under the address
        // sanitizer on a busy PC was seen to be: within the program's own limit.)
        QTest::newRow("over slow HTTPS") << 1800;
    }

    void aJetKvmOverHttpsIsKnownByTheCertificateItShowed()
    {
        QFETCH(int, slowTls);
        OwnWall own;
        JetKvmByHand jet(tls("first"));
        KvmAdding::Pace pace = quick();
        if (slowTls > 0) {
            QVERIFY(jet.stand->letInOnlyAfter(slowTls));
            // (And the JetKVM's driver with the program's own times, beside a kvmd's that are short:
            // each of its lines is let in that late.)
            pace.jetkvm = core::JetKvmDevice::Pace{};
        }
        KvmAdding adding(own.wall, pace);
        const QString address = jet.address().toString();
        const auto added = addedBy(adding, typedAt(address, u"Small one"_s));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QVERIFY(!added.value().overPlainHttp);
        QCOMPARE(added.value().certificate, sha256Of("first"));
        const OwnKvm kvm = own.wall.kvms().constFirst();
        QCOMPARE(kvm.kind, u"jetkvm"_s);
        QCOMPARE(kvm.name, u"Small one"_s);
        QVERIFY(kvm.knownByCertificate);
        QCOMPARE(kvm.certificate, sha256Of("first"));
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(kvm.id)), savedFor("secret", address, sha256Of("first")));
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(jet.linesIn, 1);
    }

    void aJetKvmWithoutAPasswordIsAddedOnlyWhereItRunsWithoutOne()
    {
        // One out of its box runs without a password: it is asked as one that needs none, and hears no
        // sign-in, and its signalling line no cookie.
        OwnWall own;
        JetKvmByHand open;
        open.password.clear();
        KvmAdding adding(own.wall, quick());
        KvmAdding::Typed typed = typedAt(open.address().toString());
        typed.password.clear();
        const auto added = addedBy(adding, typed);
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(own.wall.kvms().constFirst().kind, u"jetkvm"_s);
        QCOMPARE(open.signIns, 0);
        QCOMPARE(open.lineCookies, QList<QByteArray>{"(none)"_ba});
        QVERIFY(!own.wall.marksAPasswordFor(own.wall.kvms().constFirst().id));
        // One that asks for one, and is given none: said so, and nothing is kept.
        JetKvmByHand guarded;
        typed.address = guarded.address().toString();
        const auto refused = addedBy(adding, typed);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("no_password"));
        QCOMPARE(refused.error().message, std::string("This JetKVM asks for a password and none is saved"));
        QCOMPARE(guarded.signIns, 0);
        QCOMPARE(own.wall.kvms().size(), 1);
        QCOMPARE(own.secrets.all.keys().filter(u"kvm:"_s).size(), 1);
    }

    void whatAJetKvmSaysToASignInIsSaidAndNothingIsAdded_data()
    {
        QTest::addColumn<QString>("how");
        QTest::addColumn<QByteArray>("code");
        QTest::addColumn<QString>("sentence");
        QTest::newRow("a wrong password") << u"wrong"_s << "auth_failed"_ba << u"Login rejected - check the password"_s;
        QTest::newRow("never set up") << u"fresh"_s << "not_set_up"_ba
                                      << u"This JetKVM has not been set up yet. Open its own page once and choose how "
                                         "it is signed in to; then it can be added here."_s;
        QTest::newRow("refusing for a while")
            << u"refusing"_s << "auth_failed"_ba
            << u"The JetKVM is refusing logins after too many wrong passwords - try again later"_s;
    }

    void whatAJetKvmSaysToASignInIsSaidAndNothingIsAdded()
    {
        QFETCH(QString, how);
        QFETCH(QByteArray, code);
        QFETCH(QString, sentence);
        OwnWall own;
        JetKvmByHand jet;
        jet.password = u"another"_s;
        if (how == u"fresh"_s) {
            jet.isSetup = false;
        } else if (how == u"refusing"_s) {
            jet.signInInstead =
                Answered(429, R"({"error":"Too many attempts"})"_ba, "application/json"_ba, {"Retry-After: 120"_ba});
        }
        KvmAdding adding(own.wall, quick());
        const auto refused = addedBy(adding, typedAt(jet.address().toString()));
        QVERIFY(!refused.ok());
        QCOMPARE(QByteArray::fromStdString(refused.error().code), code);
        QCOMPARE(QString::fromStdString(refused.error().message), sentence);
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(jet.linesIn, 0);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.keys().filter(u"kvm:"_s).isEmpty());
    }

    void aJetKvmsPasswordThatWasRefusedIsTriedAtOnceWhenItIsTypedAgain()
    {
        // Typed just now: tried as for the first time, whatever the JetKVM refused a moment ago (its
        // driver would otherwise hold the same password back for a while).
        OwnWall own;
        JetKvmByHand jet;
        KvmAdding adding(own.wall, quick());
        KvmAdding::Typed typed = typedAt(jet.address().toString());
        typed.password = u"wrong"_s;
        QCOMPARE(addedBy(adding, typed).error().code, std::string("auth_failed"));
        QCOMPARE(jet.signIns, 1);
        QCOMPARE(addedBy(adding, typed).error().code, std::string("auth_failed"));
        QCOMPARE(jet.signIns, 2);
        typed.password = u"secret"_s;
        QVERIFY(addedBy(adding, typed).ok());
        QCOMPARE(jet.signIns, 3);
    }

    void aJetKvmsSignInsAreCountedWhereAKvmdsAre()
    {
        // Given no times of its own, a JetKVM's driver has a kvmd's (DriverPaces::alike): and its sign-ins
        // are counted where a kvmd's are, the test's own pacing here, and not in the program's.
        OwnWall own;
        JetKvmByHand jet;
        KvmAdding::Pace pace = quick();
        pace.driver.pacing = std::make_shared<core::SignInPacing>();
        KvmAdding adding(own.wall, pace);
        KvmAdding::Typed typed = typedAt(jet.address().toString());
        typed.password = u"wrong"_s;
        QCOMPARE(addedBy(adding, typed).error().code, std::string("auth_failed"));
        const core::SignInPacing::Mark mark = core::SignInPacing::what(u"login", jet.address(), QString(), u"wrong"_s);
        QCOMPARE(pace.driver.pacing->times(mark), 1);
        QCOMPARE(core::SignInPacing::program().times(mark), 0);
    }

    // --- which way a KVM is added (M6 1b-ii: a JetKVM's two ways, as the web decides) ------------------

    void whatIsAskedBesideAnAddressIsWorkedOutFromWhatWasTyped_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("address");
        QTest::addColumn<QString>("beside"); // empty: none
        QTest::addColumn<bool>("schemeTyped");
        QTest::addColumn<bool>("portTyped");
        QTest::newRow("a name") << u"jet.lan"_s << u"https://jet.lan"_s << u"http://jet.lan"_s << false << false;
        QTest::newRow("a name and https's port")
            << u"jet.lan:443"_s << u"https://jet.lan"_s << u"http://jet.lan:443"_s << false << true;
        QTest::newRow("a name and plain's port")
            << u"jet.lan:80"_s << u"https://jet.lan:80"_s << u"http://jet.lan"_s << false << true;
        QTest::newRow("a name and a port of its own")
            << u"jet.lan:8443"_s << u"https://jet.lan:8443"_s << u"http://jet.lan:8443"_s << false << true;
        QTest::newRow("https typed") << u"https://jet.lan"_s << u"https://jet.lan"_s << u"http://jet.lan"_s << true
                                     << false;
        QTest::newRow("plain typed") << u"http://jet.lan"_s << u"http://jet.lan"_s << u"https://jet.lan"_s << true
                                     << false;
        QTest::newRow("https typed with its port")
            << u"https://jet.lan:443"_s << u"https://jet.lan"_s << u"http://jet.lan"_s << true << false;
        QTest::newRow("https typed with a port of its own")
            << u"https://jet.lan:8443"_s << u"https://jet.lan:8443"_s << QString() << true << false;
        QTest::newRow("numbers") << u"192.168.1.40"_s << u"https://192.168.1.40"_s << u"http://192.168.1.40"_s << false
                                 << false;
        QTest::newRow("numbers and https's port")
            << u"192.168.1.40:443"_s << u"https://192.168.1.40"_s << u"http://192.168.1.40:443"_s << false << true;
        QTest::newRow("numbers, plain typed")
            << u"http://192.168.1.40"_s << u"http://192.168.1.40"_s << u"https://192.168.1.40"_s << true << false;
        QTest::newRow("IPv6") << u"[fd00::7]"_s << u"https://[fd00::7]"_s << u"http://[fd00::7]"_s << false << false;
        QTest::newRow("IPv6 and plain's port")
            << u"[fd00::7]:80"_s << u"https://[fd00::7]:80"_s << u"http://[fd00::7]"_s << false << true;
        QTest::newRow("IPv6 and https's port")
            << u"[fd00::7]:443"_s << u"https://[fd00::7]"_s << u"http://[fd00::7]:443"_s << false << true;
        QTest::newRow("IPv6, plain typed")
            << u"http://[fd00::7]"_s << u"http://[fd00::7]"_s << u"https://[fd00::7]"_s << true << false;
    }

    void whatIsAskedBesideAnAddressIsWorkedOutFromWhatWasTyped()
    {
        QFETCH(QString, typed);
        QFETCH(QString, address);
        QFETCH(QString, beside);
        QFETCH(bool, schemeTyped);
        QFETCH(bool, portTyped);
        const base::Result<QUrl> made = core::kvm::addressFrom(typed);
        QVERIFY(made.ok());
        QCOMPARE(made.value().toString(QUrl::FullyEncoded), address);
        const core::kvm::Ways ways = core::kvm::waysFor(typed, made.value(), core::kvm::otherWayOf);
        QCOMPARE(ways.address, made.value());
        QCOMPARE(ways.twin ? ways.twin->toString(QUrl::FullyEncoded) : QString(), beside);
        QCOMPARE(ways.schemeTyped, schemeTyped);
        QCOMPARE(ways.portTyped, portTyped);
        // Nothing was asked yet.
        QCOMPARE(ways.atAddress.kind, core::KvmProbe::Kind::Nothing);
        QCOMPARE(ways.atTwin.kind, core::KvmProbe::Kind::Nothing);
    }

    void theWallIsGivenWhereAJetKvmsOtherWayIsAsTheBoxesAre()
    {
        // (The wall takes a JetKVM onto HTTPS at its own host only at that other way: LocalWall::ontoHttps.)
        KvmAdding::Pace pace = quick();
        pace.otherWay = [](const QUrl&) { return std::optional<QUrl>(QUrl(u"https://192.0.2.99"_s)); };
        const core::DriverPaces paces = pace.drivers();
        QVERIFY(paces.jetkvm.otherWay);
        QCOMPARE(paces.jetkvm.otherWay(QUrl(u"http://jet.lan"_s)), std::optional<QUrl>(QUrl(u"https://192.0.2.99"_s)));
        QCOMPARE(KvmAdding::Pace{}.drivers().jetkvm.otherWay(QUrl(u"http://jet.lan"_s)),
                 std::optional<QUrl>(QUrl(u"https://jet.lan"_s)));
    }

    void theOtherWayGivenIsAskedOnlyWhereAWayOfSpeakingWasTyped()
    {
        // (The seam a test has two stand-ins be one JetKVM's two ways by: KvmAdding::Pace::otherWay.)
        const auto elsewhere = [](const QUrl&) { return std::optional<QUrl>(QUrl(u"https://192.0.2.99"_s)); };
        const QUrl plain(u"http://jet.lan"_s);
        QCOMPARE(core::kvm::waysFor(u"http://jet.lan"_s, plain, elsewhere).twin,
                 std::optional<QUrl>(QUrl(u"https://192.0.2.99"_s)));
        const QUrl encrypted(u"https://jet.lan"_s);
        QCOMPARE(core::kvm::waysFor(u"jet.lan"_s, encrypted, elsewhere).twin,
                 std::optional<QUrl>(QUrl(u"http://jet.lan"_s)));
        // One that gives the address itself, or nothing, is no way beside it.
        const auto itself = [](const QUrl& address) { return std::optional<QUrl>(address); };
        QVERIFY(!core::kvm::waysFor(u"https://jet.lan"_s, encrypted, itself).twin.has_value());
        QVERIFY(!core::kvm::waysFor(u"https://jet.lan"_s, encrypted, {}).twin.has_value());
    }

    void theChangeBoxSaysWhatToTypeToKeepAJetKvmOrMoveIt_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<bool>("nothingListens");
        QTest::addColumn<QString>("itsAddress"); // the address of the KVM that is changed
        QTest::addColumn<QString>("sentence");
        const QString plainly = u"Nothing answers over HTTPS at this address, but a JetKVM answers over plain HTTP. "_s;
        const QString listens = u"A JetKVM answers over plain HTTP at this address, and something on its HTTPS port "
                                "did not answer in time. "_s;
        const QString onHttps = u" For the device on HTTPS, type https://192.0.2.7 and try again."_s;
        QTest::newRow("https typed, kept as it is") << u"https://192.0.2.7"_s << true << u"http://192.0.2.7"_s
                                                    << plainly + u"To keep it over plain HTTP, type http://192.0.2.7"_s;
        QTest::newRow("https typed, moved") << u"https://192.0.2.7"_s << true << u"http://192.0.2.8"_s
                                            << plainly + u"To move it there over plain HTTP, type http://192.0.2.7"_s;
        QTest::newRow("nothing typed, something listens, kept as it is")
            << u"192.0.2.7"_s << false << u"http://192.0.2.7"_s
            << listens + u"To keep the JetKVM over plain HTTP, type http://192.0.2.7."_s + onHttps;
        QTest::newRow("nothing typed, something listens, moved")
            << u"192.0.2.7"_s << false << u"https://192.0.2.8"_s
            << listens + u"To move the JetKVM there over plain HTTP, type http://192.0.2.7."_s + onHttps;
    }

    void theChangeBoxSaysWhatToTypeToKeepAJetKvmOrMoveIt()
    {
        // The web's sentences say "add" in both of its boxes; the change box says what it does instead. The
        // adding box's are the web's (theWayAKvmIsAddedIsDecidedHttpsFirst).
        QFETCH(QString, typed);
        QFETCH(bool, nothingListens);
        QFETCH(QString, itsAddress);
        QFETCH(QString, sentence);
        using Kind = core::KvmProbe::Kind;
        core::kvm::Ways ways = core::kvm::waysFor(typed, core::kvm::addressFrom(typed).value(), core::kvm::otherWayOf);
        QVERIFY(ways.twin.has_value());
        ways.atAddress = aFound(Kind::Nothing, {}, nothingListens);
        ways.atTwin = aFound(Kind::JetKvm);
        const base::Result<core::kvm::Way> changing = core::kvm::wayOf(ways, QUrl(itsAddress));
        QVERIFY(!changing.ok());
        QCOMPARE(changing.error().code, std::string("plain_only"));
        QCOMPARE(QString::fromStdString(changing.error().message), sentence);
        const base::Result<core::kvm::Way> adding = core::kvm::wayOf(ways);
        QVERIFY(!adding.ok());
        QCOMPARE(adding.error().code, std::string("plain_only"));
        QVERIFY(QString::fromStdString(adding.error().message).contains(u" To add "_s));
    }

    void theWayAKvmIsAddedIsDecidedHttpsFirst_data()
    {
        using Kind = core::KvmProbe::Kind;
        QTest::addColumn<QString>("typed");
        QTest::addColumn<int>("atAddress"); // a Kind
        QTest::addColumn<bool>("nothingListens");
        QTest::addColumn<int>("atTwin");     // a Kind; -1: no twin
        QTest::addColumn<QString>("code");   // "ok", or what it is refused with
        QTest::addColumn<QString>("chosen"); // the address, or the refusal's sentence
        const int none = -1;
        const auto kind = [](Kind one) { return static_cast<int>(one); };
        const QString askedPlainly = u"Nothing answers over HTTPS at this address, but a JetKVM answers over plain "
                                     "HTTP. To add it that way, type http://192.0.2.7"_s;
        const QString somethingListens =
            u"A JetKVM answers over plain HTTP at this address, and something on its HTTPS port did not answer in "
            "time. To add the JetKVM, type http://192.0.2.7. For the device on HTTPS, type https://192.0.2.7 and try "
            "again."_s;
        // 1. A JetKVM over HTTPS: there, whatever was typed.
        QTest::newRow("a JetKVM over https, nothing typed")
            << u"192.0.2.7"_s << kind(Kind::JetKvm) << false << kind(Kind::JetKvm) << u"ok"_s << u"https://192.0.2.7"_s;
        QTest::newRow("http typed, a JetKVM over https beside it")
            << u"http://192.0.2.7"_s << kind(Kind::JetKvm) << false << kind(Kind::JetKvm) << u"ok"_s
            << u"https://192.0.2.7"_s;
        QTest::newRow("http typed, something else over https beside it")
            << u"http://192.0.2.7"_s << kind(Kind::JetKvm) << false << kind(Kind::SomethingElse) << u"ok"_s
            << u"http://192.0.2.7"_s;
        QTest::newRow("http typed, https not asked beside it")
            << u"http://192.0.2.7"_s << kind(Kind::JetKvm) << false << kind(Kind::NotAsked) << u"ok"_s
            << u"http://192.0.2.7"_s;
        // 2. A JetKVM that answers only the plain way, beside an HTTPS address.
        QTest::newRow("https typed, a kvmd there") << u"https://192.0.2.7"_s << kind(Kind::Kvmd) << false
                                                   << kind(Kind::JetKvm) << u"ok"_s << u"https://192.0.2.7"_s;
        QTest::newRow("nothing typed, a kvmd there")
            << u"192.0.2.7"_s << kind(Kind::Kvmd) << false << kind(Kind::JetKvm) << u"ok"_s << u"https://192.0.2.7"_s;
        QTest::newRow("nothing typed, something else there") << u"192.0.2.7"_s << kind(Kind::SomethingElse) << true
                                                             << kind(Kind::JetKvm) << u"ok"_s << u"https://192.0.2.7"_s;
        QTest::newRow("nothing typed, a controller there")
            << u"192.0.2.7"_s << kind(Kind::Redfish) << true << kind(Kind::JetKvm) << u"ok"_s << u"https://192.0.2.7"_s;
        QTest::newRow("https typed, nothing there") << u"https://192.0.2.7"_s << kind(Kind::Nothing) << true
                                                    << kind(Kind::JetKvm) << u"plain_only"_s << askedPlainly;
        QTest::newRow("https typed with its port, nothing there")
            << u"https://192.0.2.7:443"_s << kind(Kind::Nothing) << false << kind(Kind::JetKvm) << u"plain_only"_s
            << askedPlainly;
        QTest::newRow("nothing typed, something listens there")
            << u"192.0.2.7"_s << kind(Kind::Nothing) << false << kind(Kind::JetKvm) << u"plain_only"_s
            << somethingListens;
        QTest::newRow("nothing typed, nothing listens there")
            << u"192.0.2.7"_s << kind(Kind::Nothing) << true << kind(Kind::JetKvm) << u"ok"_s << u"http://192.0.2.7"_s;
        QTest::newRow("nothing typed, a port typed") << u"192.0.2.7:8443"_s << kind(Kind::Nothing) << false
                                                     << kind(Kind::JetKvm) << u"ok"_s << u"http://192.0.2.7:8443"_s;
        QTest::newRow("nothing typed, https's port typed")
            << u"192.0.2.7:443"_s << kind(Kind::Nothing) << false << kind(Kind::JetKvm) << u"ok"_s
            << u"http://192.0.2.7:443"_s;
        QTest::newRow("http typed, a JetKVM there alone") << u"http://192.0.2.7"_s << kind(Kind::JetKvm) << false
                                                          << kind(Kind::Nothing) << u"ok"_s << u"http://192.0.2.7"_s;
        // 3. No JetKVM: what answers at the address, as typed.
        QTest::newRow("nothing anywhere") << u"192.0.2.7"_s << kind(Kind::Nothing) << true << kind(Kind::Nothing)
                                          << u"ok"_s << u"https://192.0.2.7"_s;
        QTest::newRow("a kvmd, beside it something else")
            << u"192.0.2.7"_s << kind(Kind::Kvmd) << false << kind(Kind::SomethingElse) << u"ok"_s
            << u"https://192.0.2.7"_s;
        QTest::newRow("a scheme and a port: no twin") << u"https://192.0.2.7:8443"_s << kind(Kind::Kvmd) << false
                                                      << none << u"ok"_s << u"https://192.0.2.7:8443"_s;
        // 4. An HTTPS address that was not asked is that, whatever answers beside it.
        QTest::newRow("https not asked, a JetKVM beside it") << u"192.0.2.7"_s << kind(Kind::NotAsked) << true
                                                             << kind(Kind::JetKvm) << u"ok"_s << u"https://192.0.2.7"_s;
    }

    void theWayAKvmIsAddedIsDecidedHttpsFirst()
    {
        QFETCH(QString, typed);
        QFETCH(int, atAddress);
        QFETCH(bool, nothingListens);
        QFETCH(int, atTwin);
        QFETCH(QString, code);
        QFETCH(QString, chosen);
        using Kind = core::KvmProbe::Kind;
        core::kvm::Ways ways = core::kvm::waysFor(typed, core::kvm::addressFrom(typed).value(), core::kvm::otherWayOf);
        const std::optional<QUrl> twin = ways.twin;
        // (The certificate each line showed, to tell which was taken: one over HTTPS shows one.)
        const auto shownAt = [](const QUrl& at) {
            return at.scheme() == u"https"_s ? at.toString().toUtf8() : QByteArray();
        };
        ways.atAddress = aFound(static_cast<Kind>(atAddress), shownAt(ways.address), nothingListens);
        if (atTwin >= 0) {
            QVERIFY(twin.has_value());
            ways.twin = twin;
            ways.atTwin = aFound(static_cast<Kind>(atTwin), shownAt(*twin));
        }
        const base::Result<core::kvm::Way> way = core::kvm::wayOf(ways);
        if (code == u"ok"_s) {
            QVERIFY2(way.ok(), way.ok() ? "" : way.error().message.c_str());
            QCOMPARE(way.value().address.toString(QUrl::FullyEncoded), chosen);
            // What was found on the line of the address that is taken: its certificate and its answer.
            const bool atTheTwin = twin && way.value().address == *twin && *twin != ways.address;
            QCOMPARE(way.value().found.shown, shownAt(way.value().address));
            QCOMPARE(static_cast<int>(way.value().found.kind), atTheTwin ? atTwin : atAddress);
        } else {
            QVERIFY(!way.ok());
            QCOMPARE(QString::fromStdString(way.error().code), code);
            QCOMPARE(QString::fromStdString(way.error().message), chosen);
        }
    }

    void aJetKvmTypedPlainWhereItOffersHttpsIsAddedOverHttps()
    {
        OwnWall own;
        JetKvmByHand plain;
        JetKvmByHand secure(tls("first"));
        KvmAdding::Pace pace = roomForTls();
        pace.otherWay = twoWays(plain.address(), secure.address());
        KvmAdding adding(own.wall, pace);
        const auto added = addedBy(adding, typedAt(plain.address().toString()));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QVERIFY(!added.value().overPlainHttp);
        QCOMPARE(added.value().certificate, sha256Of("first"));
        const OwnKvm kvm = own.wall.kvms().constFirst();
        QCOMPARE(kvm.kind, u"jetkvm"_s);
        QCOMPARE(kvm.address, secure.address().toString());
        QVERIFY(kvm.knownByCertificate);
        QCOMPARE(kvm.certificate, sha256Of("first"));
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(kvm.id)),
                 savedFor("secret", secure.address().toString(), sha256Of("first")));
        // The plain way heard what is asked of the address, and nothing of a password; the HTTPS way its
        // JetKVM's question alone (by the box, and again by its driver before the sign-in), and then the sign-in.
        QVERIFY(heardOnlyTheQuestions(*plain.stand));
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(plain.lineAskings, 0);
        QCOMPARE(secure.stand->count(kvmdsQuestion), 0);
        QCOMPARE(secure.stand->count(redfishsQuestion), 0);
        QCOMPARE(secure.asked(jetKvmsQuestion), 2);
        QCOMPARE(secure.signIns, 1);
        QCOMPARE(secure.linesIn, 1);
    }

    void aJetKvmTypedPlainWhoseHttpsShowsAnotherOfTheWallHearsNoPassword()
    {
        // From the third reading of the twins (L4): typed the plain way, its HTTPS answering as no JetKVM does (a 404
        // here) and showing the certificate another KVM of the wall is known by: that other one may be what answers
        // the plain way too. The box's sign-in looks at the HTTPS first, as the wall's own driver does, and sends
        // nothing: "same_device", in the wall's words.
        OwnWall own;
        const core::testing::NobodyThere nobody;
        OwnKvm other;
        other.address = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        other.kind = u"jetkvm"_s;
        other.name = u"The other"_s;
        other.knownByCertificate = true;
        other.certificate = sha256Of("first");
        QVERIFY(own.wall.add(other, u"its own"_s).ok());
        JetKvmByHand plain;
        const StandIn itsHttps(
            [](const Asked&) { return Answered(404, R"({"error":"Not Found"})"_ba, "application/json"_ba); },
            tls("first"));
        KvmAdding::Pace pace = roomForTls();
        pace.otherWay = twoWays(plain.address(), QUrl(itsHttps.address()));
        KvmAdding adding(own.wall, pace);
        const auto refused = addedBy(adding, typedAt(plain.address().toString()));
        QCOMPARE(codeOf(refused), std::string("same_device"));
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(own.wall.kvms().size(), 1);
    }

    void aTwinTheBoxSawIsNotForgottenWhereThatHttpsFallsSilentAtTheSamePort()
    {
        // From the outside review (F1): as above, and once its HTTPS has answered that first look, it takes every line
        // at that very port and says nothing. What the first look saw is judged there and then, whatever it answered:
        // nothing goes, whatever a later look does not see.
        OwnWall own;
        const core::testing::NobodyThere nobody;
        OwnKvm other;
        other.address = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        other.kind = u"jetkvm"_s;
        other.name = u"The other"_s;
        other.knownByCertificate = true;
        other.certificate = sha256Of("first");
        QVERIFY(own.wall.add(other, u"its own"_s).ok());
        JetKvmByHand plain;
        core::testing::FallsSilent itsHttps(tls("first"));
        KvmAdding::Pace pace = roomForTls();
        pace.otherWay = twoWays(plain.address(), QUrl(itsHttps.address()));
        KvmAdding adding(own.wall, pace);
        const auto refused = addedBy(adding, typedAt(plain.address().toString()));
        QCOMPARE(codeOf(refused), std::string("same_device"));
        QCOMPARE(itsHttps.answered(), qsizetype{1});
        QVERIFY(itsHttps.fellSilent());
        QCOMPARE(plain.signIns, 0);
        QCOMPARE(own.wall.kvms().size(), 1);
    }

    void anAddressTypedWithHttpsIsNeverAnsweredThePlainWay()
    {
        OwnWall own;
        JetKvmByHand jet;
        const core::testing::NobodyThere nobody;
        const QString typed = u"https://127.0.0.1:%1"_s.arg(nobody.port());
        KvmAdding::Pace pace = quick();
        pace.otherWay = twoWays(QUrl(typed), jet.address());
        KvmAdding adding(own.wall, pace);
        const auto refused = addedBy(adding, typedAt(typed));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("plain_only"));
        QCOMPARE(QString::fromStdString(refused.error().message),
                 u"Nothing answers over HTTPS at this address, but a JetKVM answers over plain HTTP. To add it that "
                 "way, type "_s +
                     jet.address().toString());
        // The plain way heard its question alone, and no password went anywhere.
        QCOMPARE(jet.stand->asked.size(), 1);
        QCOMPARE(jet.asked(jetKvmsQuestion), 1);
        QCOMPARE(jet.signIns, 0);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(own.secrets.all.keys().filter(u"kvm:"_s).isEmpty());
        // Typed the plain way, it is added that way.
        const auto added = addedBy(adding, typedAt(jet.address().toString()));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QVERIFY(added.value().overPlainHttp);
        QCOMPARE(jet.signIns, 1);
    }

    void aJetKvmTypedWithAPortAndNoSchemeIsAddedTheWayItAnswers()
    {
        // Typed with a port and no way of speaking, the two ways are one port: what answers the plain way
        // there is all that listens on it.
        OwnWall own;
        KvmAdding adding(own.wall, roomForTls());
        JetKvmByHand plain;
        auto added = addedBy(adding, typedAt(u"127.0.0.1:%1"_s.arg(plain.stand->serverPort())));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QVERIFY(added.value().overPlainHttp);
        QCOMPARE(own.wall.kvms().constFirst().address, plain.address().toString());
        QCOMPARE(plain.signIns, 1);
        // And one over HTTPS is added over HTTPS.
        JetKvmByHand secure(tls("first"));
        added = addedBy(adding, typedAt(u"127.0.0.1:%1"_s.arg(secure.stand->serverPort())));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QVERIFY(!added.value().overPlainHttp);
        QCOMPARE(added.value().certificate, sha256Of("first"));
        QCOMPARE(own.wall.kvms().constLast().address, secure.address().toString());
        QCOMPARE(secure.signIns, 1);
    }

    void aKvmdTypedWithHttpsIsAddedThoughAJetKvmAnswersBesideIt()
    {
        // What answers over HTTPS at the address is what was meant: what says "JetKVM" beside it the plain
        // way may be somebody else, and hears no password.
        OwnWall own;
        const MadeUpKvm kvmd(tls("first"));
        JetKvmByHand jet;
        KvmAdding::Pace pace = roomForTls();
        pace.otherWay = twoWays(QUrl(kvmd.stand->address()), jet.address());
        KvmAdding adding(own.wall, pace);
        const auto added = addedBy(adding, typedAt(kvmd.stand->address()));
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        QCOMPARE(own.wall.kvms().constFirst().kind, u"kvmd"_s);
        QCOMPARE(own.wall.kvms().constFirst().address, kvmd.stand->address());
        QCOMPARE(kvmd.signIns, 1);
        QCOMPARE(jet.stand->asked.size(), 1);
        QCOMPARE(jet.asked(jetKvmsQuestion), 1);
        QCOMPARE(jet.signIns, 0);
    }

    void anAddressTypedWithASchemeAndAPortIsAskedOnlyThatWay()
    {
        // A port of its own is somebody's own arrangement: the other way is not asked at all.
        OwnWall own;
        KvmAdding adding(own.wall, quick());
        JetKvmByHand plain;
        QVERIFY(addedBy(adding, typedAt(plain.address().toString())).ok());
        QCOMPARE(plain.stand->notHttp, 0);
        // (By the box, and its number by its driver before the sign-in: never the other way.)
        QCOMPARE(plain.asked(jetKvmsQuestion), 1);
        QCOMPARE(plain.asked("/device"), 2);
        const MadeUpKvm kvmd;
        QVERIFY(addedBy(adding, typedAt(kvmd.stand->address())).ok());
        QCOMPARE(kvmd.stand->notHttp, 0);
        QCOMPARE(kvmd.stand->count(jetKvmsQuestion), 1);
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
        KvmAdding adding(own.wall, quick());
        QStringList sentences;
        const auto tried = [&adding, &sentences](const QString& address, const QString& password) {
            KvmAdding::Typed typed = typedAt(address, u"Build server"_s);
            typed.password = password;
            const auto result = addedBy(adding, typed);
            sentences.append(result.ok() ? result.value().name : QString::fromStdString(result.error().message));
            return result.ok() ? std::string("added") : result.error().code;
        };
        // One that is added, over each way; one whose password is refused; a JetKVM that is added, and one
        // that refuses its password; and one of each kind of thing that is never signed in to.
        MadeUpKvm kept;
        kept.password = u"Kept-Zq7-one"_s;
        MadeUpKvm keptEncrypted(tls("first"));
        keptEncrypted.password = u"Kept-Zq7-two"_s;
        const MadeUpKvm refusing(tls("second"));
        const StandIn page(asAPage);
        const StandIn controller(asAController);
        const core::testing::NobodyThere nobody;
        JetKvmByHand jetKept;
        jetKept.password = u"Kept-Zq7-jet"_s;
        const JetKvmByHand jetRefusing;
        QCOMPARE(tried(kept.stand->address(), kept.password), std::string("added"));
        QCOMPARE(tried(keptEncrypted.stand->address(), keptEncrypted.password), std::string("added"));
        QCOMPARE(tried(refusing.stand->address(), u"Wrong-Zq7-three"_s), std::string("auth_failed"));
        QCOMPARE(tried(jetKept.address().toString(), jetKept.password), std::string("added"));
        QCOMPARE(tried(jetRefusing.address().toString(), u"Wrong-Zq7-jet"_s), std::string("auth_failed"));
        QCOMPARE(tried(page.address(), u"Never-Zq7-four"_s), std::string("not_a_kvm"));
        // (A server's controller is signed in to from M7 1b-i on, and refuses this one's.)
        QCOMPARE(tried(controller.address(), u"Never-Zq7-five"_s), std::string("auth_failed"));
        QCOMPARE(tried(nobody.address(), u"Never-Zq7-six"_s), std::string("unreachable"));
        QCOMPARE(tried(kept.stand->address(), u"Never-Zq7-seven"_s), std::string("duplicate"));
        QCOMPARE(tried(u"kvm lan"_s, u"Never-Zq7-eight"_s), std::string("invalid"));
        QCOMPARE(tried(page.address(), QString(core::kvm::longestPassword, u'p') + u"-Zq7-"_s), std::string("invalid"));
        QTest::qWait(200);

        // The three that are on the wall, each with the secrets under its id, and no other.
        QCOMPARE(own.secrets.all.size(), 3);
        // (Each with what it was saved for: its address, and the certificate it is known by.)
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(own.wall.kvms().at(0).id)),
                 savedFor("Kept-Zq7-one", kept.stand->address()));
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(own.wall.kvms().at(1).id)),
                 savedFor("Kept-Zq7-two", keptEncrypted.stand->address(), sha256Of("first")));
        QCOMPARE(own.secrets.all.value(LocalWall::secretOf(own.wall.kvms().at(2).id)),
                 savedFor("Kept-Zq7-jet", jetKept.address().toString()));
        // Not in the settings, not in a sentence, and not in anything that was said aloud.
        QFile settings(own.folder.filePath(u"settings.json"_s));
        QVERIFY(settings.open(QIODevice::ReadOnly));
        const QByteArray text = settings.readAll();
        QVERIFY(text.contains("Build server"));
        QVERIFY(!text.contains("Zq7"));
        QVERIFY(!text.contains("password"));
        QCOMPARE(sentences.size(), 11);
        for (const QString& one : std::as_const(sentences)) {
            QVERIFY2(!one.contains(u"Zq7"_s), qPrintable(one));
        }
        for (const QString& one : std::as_const(aloud)) {
            QVERIFY2(!one.contains(u"Zq7"_s), qPrintable(one));
        }
        // And to what was never signed in to, nothing of it went. (The controller was signed in to once, with the
        // password typed for it, in its sign-in's body alone: never in a path, a cookie or an Authorization.)
        for (const StandIn* stand : {&page, &controller}) {
            for (const Asked& one : std::as_const(stand->asked)) {
                const bool itsSignIn = stand == &controller && one.method == "POST" &&
                                       one.path == "/redfish/v1/SessionService/Sessions" &&
                                       one.body.contains("Never-Zq7-five");
                QVERIFY(itsSignIn || !one.body.contains("Zq7"));
                QVERIFY(!one.path.contains("Zq7"));
                QVERIFY(one.headers.value("cookie").isEmpty() && one.headers.value("authorization").isEmpty());
            }
        }
        QCOMPARE(std::ranges::count_if(controller.asked, [](const Asked& one) { return one.body.contains("Zq7"); }), 1);
    }
};

QTEST_GUILESS_MAIN(TestKvmAdding)
#include "test_kvm_adding.moc"
