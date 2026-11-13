#include <array>
#include <memory>
#include <optional>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <qkvm/core/awake_line.h>
#include <qkvm/core/device_line.h>
#include <qkvm/core/kvm_certificate.h>
#include <qkvm/core/kvmd_console_line.h>
#include <qkvm/core/kvmd_input.h>
#include <qkvm/core/tls.h>

#include "kvm_by_hand.h"
#include "stand_in.h"

#include <winsock2.h>

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::AwakeLine;
using core::DeviceLine;
using core::KvmdConsoleLine;
using core::testing::KvmByHand;
using core::testing::NobodyThere;
using core::testing::StandIn;
using core::testing::Tls;

namespace {

// WebSocket's numbers for the kinds of message.
constexpr int text = 1;
constexpr int goodbye = 8;
constexpr int areYouThere = 9;
constexpr int iAm = 10;

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

Tls tlsOf(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

QByteArray sha256Of(const char* name)
{
    return StandIn::certificateIn(tlsOf(name).certificateFile).digest(QCryptographicHash::Sha256);
}

// A made-up KVM in the clear, or over TLS with the certificate the tests' KVMs are known by.
std::unique_ptr<KvmByHand> kvmOver(bool encrypted)
{
    return std::make_unique<KvmByHand>(encrypted ? std::optional<Tls>(tlsOf("first")) : std::nullopt);
}

// The times a line keeps, made short. The KVM is not asked whether it is there, unless a test is
// about that: what it is sent is then what the test had said, and nothing else. Nor is it watched
// for silence then: a made-up KVM says nothing by itself.
KvmdConsoleLine::Pace quick()
{
    KvmdConsoleLine::Pace pace;
    pace.letInWithinMs = 2000;
    pace.askEveryMs = 600'000;
    pace.silentAfterMs = 600'000;
    pace.goodbyeWithinMs = 500;
    return pace;
}

KvmdConsoleLine::Where whereOf(const KvmByHand& kvm)
{
    KvmdConsoleLine::Where where;
    where.address = kvm.address();
    where.query = "stream=0";
    where.cookie = "auth_token=pass1";
    // (In texts, which the tests below read; what is told in bytes has tests of its own.)
    where.inTexts = true;
    if (where.address.scheme() == "https"_L1) {
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
    }
    return where;
}

DeviceLine::Where lineTo(const KvmByHand& kvm)
{
    DeviceLine::Where where;
    where.address = kvm.address();
    where.asking = "/api/ws?stream=0";
    where.cookie = "auth_token=pass1";
    if (where.address.scheme() == "https"_L1) {
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
    }
    return where;
}

DeviceLine::Limits roomy()
{
    DeviceLine::Limits limits;
    limits.letInWithinMs = 2000;
    limits.mostUnsentBytes = qint64{1024} * 1024;
    limits.goodbyeWithinMs = 500;
    return limits;
}

// Keyboard and mouse to a made-up KVM, and what the line said of itself.
struct Console {
    explicit Console(const KvmdConsoleLine::Where& where, KvmdConsoleLine::Pace pace = quick())
        : line(where, pace), opened(&line, &KvmdConsoleLine::opened), closed(&line, &KvmdConsoleLine::closed)
    {}

    QString code() const { return closed.value(0).value(0).toString(); }
    QString sentence() const { return closed.value(0).value(1).toString(); }
    // Whether it said that the machine may still hold what was down.
    bool leftDown() const { return closed.value(0).value(2).toBool(); }

    KvmdConsoleLine line;
    QSignalSpy opened;
    QSignalSpy closed;
};

// kvmd's words, as the made-up KVM is to read them.
QJsonObject keyOf(const QString& name, bool down)
{
    return {{u"event_type"_s, u"key"_s}, {u"event"_s, QJsonObject{{u"key"_s, name}, {u"state"_s, down}}}};
}

QJsonObject buttonOf(const QString& name, bool down)
{
    return {{u"event_type"_s, u"mouse_button"_s}, {u"event"_s, QJsonObject{{u"button"_s, name}, {u"state"_s, down}}}};
}

QJsonObject deltaOf(const QString& kind, int x, int y)
{
    return {{u"event_type"_s, kind}, {u"event"_s, QJsonObject{{u"delta"_s, QJsonObject{{u"x"_s, x}, {u"y"_s, y}}}}}};
}

QJsonObject areYouThereOf()
{
    return {{u"event_type"_s, u"ping"_s}, {u"event"_s, QJsonObject{}}};
}

// What a KVM answers when it is asked whether it is there.
QByteArray iAmHere()
{
    return R"({"event_type":"pong","event":{}})";
}

// USB's numbers for the keys the tests press.
constexpr int keyA = 0x04;
constexpr int keyB = 0x05;
constexpr int keyC = 0x06;
constexpr int enter = 0x28;
constexpr int keyF5 = 0x3E;
constexpr int del = 0x4C;
constexpr int leftControl = 0xE0;
constexpr int leftShift = 0xE1;
constexpr int leftAlt = 0xE2;

// How many bytes a short message is on the line beyond what it says: how it begins, and the four
// bytes it is hidden with.
constexpr qint64 aroundAShortMessage = 6;

// The keys a made-up KVM was told of, in either wording and in the order they came: each by its
// name, with "+" before it where it went down and "-" where it went up.
QStringList keysToldTo(const KvmByHand& kvm)
{
    QStringList keys;
    const QList<KvmByHand::Message> all = kvm.messages();
    for (const KvmByHand::Message& one : all) {
        if (one.kind == text) {
            const QJsonObject word = QJsonDocument::fromJson(one.what).object();
            const QJsonObject event = word["event"_L1].toObject();
            if (word["event_type"_L1].toString() == "key"_L1) {
                keys.append((event["state"_L1].toBool() ? u"+"_s : u"-"_s) + event["key"_L1].toString());
            }
        } else if (one.kind == 2 && one.what.size() > 2 && one.what.at(0) == 1) {
            keys.append((one.what.at(1) != 0 ? u"+"_s : u"-"_s) + QString::fromLatin1(one.what.mid(2)));
        }
    }
    return keys;
}

// The same of the mouse: each button by its name, with "+" or "-"; "at" where the pointer's place
// was said, and "wheel" for a turn of the wheel.
QStringList mouseToldTo(const KvmByHand& kvm)
{
    QStringList told;
    const QList<KvmByHand::Message> all = kvm.messages();
    for (const KvmByHand::Message& one : all) {
        if (one.kind == text) {
            const QJsonObject word = QJsonDocument::fromJson(one.what).object();
            const QJsonObject event = word["event"_L1].toObject();
            const QString what = word["event_type"_L1].toString();
            if (what == "mouse_button"_L1) {
                told.append((event["state"_L1].toBool() ? u"+"_s : u"-"_s) + event["button"_L1].toString());
            } else if (what == "mouse_move"_L1) {
                told.append(u"at"_s);
            } else if (what == "mouse_wheel"_L1) {
                told.append(u"wheel"_s);
            }
        } else if (one.kind == 2 && one.what.size() > 2 && one.what.at(0) == 2) {
            told.append((one.what.at(1) != 0 ? u"+"_s : u"-"_s) + QString::fromLatin1(one.what.mid(2)));
        } else if (one.kind == 2 && !one.what.isEmpty() && one.what.at(0) == 3) {
            told.append(u"at"_s);
        } else if (one.kind == 2 && !one.what.isEmpty() && one.what.at(0) == 5) {
            told.append(u"wheel"_s);
        }
    }
    return told;
}

// What Windows itself has for a line, and how the line stands with it: asked by Windows' own call
// and not through Qt, which says of a line that was reset what it says of one that was closed.
struct Bare {
    qint64 bytes = 0;
    QByteArray end; // none yet: the line is there; "closed": ended the ordinary way; "reset"; or Windows' number
};

// Takes all that has come on the line off it, without waiting for more.
void takeBare(qintptr line, Bare& into)
{
    std::array<char, 16384> some{};
    while (into.end.isEmpty()) {
        const int came = ::recv(static_cast<SOCKET>(line), some.data(), static_cast<int>(some.size()), 0);
        if (came > 0) {
            into.bytes += came;
        } else if (came == 0) {
            into.end = "closed";
        } else if (const int why = ::WSAGetLastError(); why == WSAEWOULDBLOCK) {
            return;
        } else {
            into.end = why == WSAECONNRESET ? "reset"_ba : QByteArray::number(why);
        }
    }
}

} // namespace

// The line to a KVM that the program speaks itself (DeviceLine), the two that are made of it: the
// one that only keeps a KVM awake (AwakeLine), and keyboard and mouse to a KVM (KvmdConsoleLine).
// Against a KVM whose end of the line is played by hand, so that what went over the line is seen
// as it went.
class TestKvmdConsoleLine : public QObject {
    Q_OBJECT

private slots:
    // Windows' own TLS, as in the program.
    void initTestCase() { QVERIFY(core::useWindowsOwnTls()); }

    // The line itself.

    void aTextIsSaidAsOneHiddenMessageInTheFormItsLengthAsksFor_data()
    {
        QTest::addColumn<int>("length");
        QTest::addColumn<int>("lengthBytes");
        QTest::newRow("nothing") << 0 << 1;
        QTest::newRow("one byte") << 1 << 1;
        QTest::newRow("the longest of the short form") << 125 << 1;
        QTest::newRow("the shortest of the middle form") << 126 << 3;
        QTest::newRow("the longest of the middle form") << 65535 << 3;
        QTest::newRow("the shortest of the long form") << 65536 << 9;
        QTest::newRow("longer than that") << 70000 << 9;
    }

    void aTextIsSaidAsOneHiddenMessageInTheFormItsLengthAsksFor()
    {
        QFETCH(int, length);
        QFETCH(int, lengthBytes);
        KvmByHand kvm;
        DeviceLine line(lineTo(kvm), roomy());
        line.open();
        QTRY_VERIFY(line.isIn());
        QByteArray said(length, '\0');
        for (int index = 0; index < length; ++index) {
            said[index] = static_cast<char>('a' + index % 23);
        }
        QVERIFY(line.say(said));
        QTRY_COMPARE(kvm.messages().size(), 1);
        const KvmByHand::Message came = kvm.messages().at(0);
        QCOMPARE(came.kind, text);
        QVERIFY(came.whole);
        QVERIFY(!came.spare);
        QVERIFY(came.hidden);
        QCOMPARE(came.hiddenBy.size(), 4);
        QCOMPARE(came.lengthBytes, lengthBytes);
        QCOMPARE(came.what, said);
        // Nothing came after it: the message is as long as it says, to the byte.
        QCOMPARE(kvm.bytesAfterTheAsking(), length + lengthBytes + 5);
    }

    void everyMessageIsHiddenWithBytesOfItsOwn()
    {
        KvmByHand kvm;
        DeviceLine line(lineTo(kvm), roomy());
        line.open();
        QTRY_VERIFY(line.isIn());
        for (int one = 0; one < 16; ++one) {
            QVERIFY(line.say("the same words every time"_ba));
        }
        QTRY_COMPARE(kvm.messages().size(), 16);
        QSet<QByteArray> hiddenBy;
        QSet<QByteArray> asTheyCame;
        const QList<KvmByHand::Message> all = kvm.messages();
        for (const KvmByHand::Message& one : all) {
            QVERIFY(one.hidden);
            QCOMPARE(one.what, "the same words every time"_ba);
            QVERIFY(one.asItCame != one.what);
            hiddenBy.insert(one.hiddenBy);
            asTheyCame.insert(one.asItCame);
        }
        // (Sixteen times the same four bytes by chance is not to be waited for.)
        QVERIFY(hiddenBy.size() >= 15);
        QVERIFY(asTheyCame.size() >= 15);
    }

    void nothingIsSaidOnALineTheKvmHasNotLetInOrThatIsOver()
    {
        KvmByHand kvm;
        kvm.answers = false;
        DeviceLine line(lineTo(kvm), roomy());
        const QSignalSpy letIn(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        QVERIFY(!line.say("before it was opened"_ba));
        line.open();
        QVERIFY(!line.say("before a line was there"_ba));
        QTRY_VERIFY(kvm.wasAsked());
        QVERIFY(!line.say("before it was let in"_ba));
        QTest::qWait(100);
        QCOMPARE(kvm.bytesAfterTheAsking(), 0);
        kvm.letIn();
        QTRY_COMPARE(letIn.count(), 1);
        QVERIFY(line.isIn());
        QVERIFY(line.say("in"_ba));
        line.close();
        QVERIFY(!line.isIn());
        QVERIFY(!line.say("after it was closed"_ba));
        QTRY_VERIFY(!kvm.isOn());
        // The one text, and the goodbye after it.
        const QList<KvmByHand::Message> all = kvm.messages();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all.at(0).kind, text);
        QCOMPARE(all.at(0).what, "in"_ba);
        QCOMPARE(all.at(1).kind, goodbye);
        QVERIFY(all.at(1).whole && all.at(1).hidden);
        QCOMPARE(all.at(1).what, QByteArray("\x03\xE8", 2)); // 1000: nothing is wrong
        // An end from here is not said as an end.
        QTest::qWait(roomy().goodbyeWithinMs + 200);
        QCOMPARE(ended.count(), 0);
    }

    void nothingIsSaidOnALineThatFellEither()
    {
        KvmByHand kvm;
        DeviceLine line(lineTo(kvm), roomy());
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        kvm.fallOver();
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(0).value<DeviceLine::End>(), DeviceLine::End::Fell);
        QVERIFY(!line.isIn());
        QVERIFY(!line.say("to nobody"_ba));
        line.close(); // (and there is nothing left to close)
        QTest::qWait(100);
        QCOMPARE(ended.count(), 1);
    }

    void aLineThatWasNeverLetInIsClosedWithoutAWord()
    {
        KvmByHand kvm;
        kvm.answers = false;
        DeviceLine line(lineTo(kvm), roomy());
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(kvm.wasAsked());
        line.close();
        QTRY_VERIFY(!kvm.isOn());
        QCOMPARE(kvm.bytesAfterTheAsking(), 0);
        QCOMPARE(ended.count(), 0);
    }

    void aTextThatWouldBeMoreThanMayWaitEndsTheLineAndIsNotSent()
    {
        KvmByHand kvm;
        DeviceLine::Limits limits = roomy();
        limits.mostUnsentBytes = 4096;
        DeviceLine line(lineTo(kvm), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        QVERIFY(line.say(QByteArray(1000, 'a')));
        QTRY_COMPARE(kvm.messages().size(), 1);
        QVERIFY(!line.say(QByteArray(5000, 'b')));
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(0).value<DeviceLine::End>(), DeviceLine::End::TakesNothing);
        QVERIFY(!line.isIn());
        QTRY_VERIFY(!kvm.isOn());
        QVERIFY(!kvm.afterTheAsking().contains("bbbb"));
        QCOMPARE(kvm.messages().size(), 1);
    }

    void whatAKvmHasNoReasonToSendEndsTheLine_data()
    {
        QTest::addColumn<QByteArray>("sent");
        QTest::addColumn<DeviceLine::End>("why");
        QTest::newRow("a goodbye") << QByteArray("\x88\x00", 2) << DeviceLine::End::Fell;
        QTest::newRow("a goodbye with its reason") << QByteArray("\x88\x02\x03\xE9", 4) << DeviceLine::End::Fell;
        QTest::newRow("a message hidden as a caller hides it")
            << QByteArray("\x81\x80\x01\x02\x03\x04", 6) << DeviceLine::End::Misbehaved;
        QTest::newRow("a mark nobody agreed on") << QByteArray("\xC1\x00", 2) << DeviceLine::End::Misbehaved;
        QTest::newRow("a kind there is none of") << QByteArray("\x83\x00", 2) << DeviceLine::End::Misbehaved;
        QTest::newRow("another kind there is none of") << QByteArray("\x8B\x00", 2) << DeviceLine::End::Misbehaved;
        QTest::newRow("an are-you-there longer than one may be")
            << QByteArray("\x89\x7E\x00\x7E", 4) + QByteArray(126, 'x') << DeviceLine::End::Misbehaved;
        QTest::newRow("a length that is none")
            << QByteArray("\x82\x7F\x80\x00\x00\x00\x00\x00\x00\x00", 10) << DeviceLine::End::Misbehaved;
        // (65 537 bytes, of which none needs to come: it is refused by what it says of itself.)
        QTest::newRow("a message a byte longer than a KVM has reason to send")
            << QByteArray("\x82\x7F\x00\x00\x00\x00\x00\x01\x00\x01", 10) << DeviceLine::End::Misbehaved;
        QTest::newRow("a part of a message that is as long")
            << QByteArray("\x01\x7F\x00\x00\x00\x00\x00\x01\x00\x01", 10) << DeviceLine::End::Misbehaved;
        QTest::newRow("a message as long as a length can say")
            << QByteArray("\x81\x7F\x7F\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 10) << DeviceLine::End::Misbehaved;
        // (More than twice the most there may be in a second: whichever second they fall into.)
        QTest::newRow("more messages than a KVM has reason to send")
            << QByteArray("\x81\x00", 2).repeated(4100) << DeviceLine::End::Misbehaved;
    }

    void whatAKvmHasNoReasonToSendEndsTheLine()
    {
        QFETCH(QByteArray, sent);
        QFETCH(DeviceLine::End, why);
        KvmByHand kvm;
        DeviceLine line(lineTo(kvm), roomy());
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        kvm.sendRaw(sent);
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(0).value<DeviceLine::End>(), why);
        QVERIFY(!line.isIn());
        QTRY_VERIFY(!kvm.isOn());
        // Nothing went back for it, not even a goodbye.
        QCOMPARE(kvm.bytesAfterTheAsking(), 0);
    }

    void whatAKvmSaysIsThrownAwayAndTheLineStaysUp()
    {
        KvmByHand kvm;
        DeviceLine line(lineTo(kvm), roomy());
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        // A word, a long word, bytes in the long form and as many as one message may be, the
        // answer to an "are you there" that nobody asked, and a message in two parts.
        kvm.send(text, R"({"event_type":"pong","event":{}})");
        kvm.send(text, QByteArray(300, 'x'));
        kvm.send(2, QByteArray(65'536, 'y'));
        kvm.send(iAm, "late");
        kvm.sendRaw(QByteArray("\x01\x03", 2) + "abc" + QByteArray("\x80\x03", 2) + "def");
        // After all of it, an "are you there": its answer says that the rest was got through.
        kvm.send(areYouThere, "still");
        QTRY_COMPARE(kvm.messages().size(), 1);
        QCOMPARE(kvm.messages().at(0).kind, iAm);
        QCOMPARE(kvm.messages().at(0).what, "still"_ba);
        QVERIFY(line.isIn());
        QCOMPARE(ended.count(), 0);
        QVERIFY(line.say("and it can still be said to"_ba));
        QTRY_COMPARE(kvm.messages().size(), 2);
    }

    void aLineThatHearsShortTextsHandsThemOutAndThrowsAwayTheRest()
    {
        // The line that keeps a KVM's video awake hears its board there (kvmd's /api/ws: an `atx` event of some ninety
        // bytes on a real Comet): short texts in one part are handed out whole; anything else is thrown away unread,
        // as before, and the line stays up.
        KvmByHand kvm;
        DeviceLine line(lineTo(kvm), roomy());
        line.hearShortTexts(64);
        QList<QByteArray> heard;
        connect(&line, &DeviceLine::textCame, &line, [&heard](const QByteArray& said) { heard.append(said); });
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        kvm.send(text, R"({"event_type":"atx","event":{}})");
        kvm.send(text, QByteArray(65, 'x'));
        kvm.send(2, "bytes");
        kvm.sendRaw(QByteArray("\x01\x03", 2) + "abc" + QByteArray("\x80\x03", 2) + "def");
        kvm.send(text, QByteArray(64, 'y'));
        kvm.send(areYouThere, "still");
        QTRY_COMPARE(kvm.messages().size(), 1);
        QCOMPARE(kvm.messages().at(0).kind, iAm);
        QCOMPARE(heard, (QList<QByteArray>{R"({"event_type":"atx","event":{}})"_ba, QByteArray(64, 'y')}));
        QVERIFY(line.isIn());
        QCOMPARE(ended.count(), 0);
    }

    void aKvmThatTakesNoAnswersIsNotAnsweredWithoutEnd_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<bool>("onlyHeld");
        QTest::newRow("in the clear") << false << false;
        QTest::newRow("over TLS") << true << false;
        QTest::newRow("the line that only keeps a KVM awake, over TLS") << true << true;
    }

    void aKvmThatTakesNoAnswersIsNotAnsweredWithoutEnd()
    {
        // It asks "are you there" and never takes an answer off the line: the answers pile up on
        // this PC. Not without end. (Fewer a second than end the line by their number.)
        QFETCH(bool, encrypted);
        QFETCH(bool, onlyHeld);
        const std::unique_ptr<KvmByHand> kvm = kvmOver(encrypted);
        DeviceLine::Limits limits = roomy();
        limits.mostUnsentBytes = 65'536;
        DeviceLine line(lineTo(*kvm), limits);
        AwakeLine held(lineTo(*kvm), 2000);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        const QSignalSpy heldEnded(&held, &AwakeLine::ended);
        if (onlyHeld) {
            held.open();
        } else {
            line.open();
        }
        QTRY_VERIFY(onlyHeld ? held.isIn() : line.isIn());
        kvm->stopReading();
        QTimer asking;
        connect(&asking, &QTimer::timeout, kvm.get(), [&kvm] {
            for (int one = 0; one < 25; ++one) {
                kvm->send(areYouThere, QByteArray(125, 'x'));
            }
        });
        asking.start(20);
        QTRY_COMPARE_WITH_TIMEOUT(ended.count() + heldEnded.count(), 1, 20'000);
        if (!onlyHeld) {
            QCOMPARE(ended.at(0).at(0).value<DeviceLine::End>(), DeviceLine::End::TakesNothing);
        }
    }

    void moreBytesInASecondThanAKvmHasReasonToSendEndTheLine()
    {
        KvmByHand kvm;
        DeviceLine::Limits limits = roomy();
        limits.mostBytesASecond = 16'384;
        DeviceLine line(lineTo(kvm), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        // Less than that is thrown away, and the line stays.
        kvm.send(2, QByteArray(4000, 'x'));
        kvm.send(areYouThere, "still");
        QTRY_COMPARE(kvm.messages().size(), 1);
        QCOMPARE(ended.count(), 0);
        // More than twice what there may be in a second, whichever second it falls into: in
        // messages none of which is longer than one may be, and far fewer than end a line by
        // their number.
        for (int one = 0; one < 40; ++one) {
            kvm.send(2, QByteArray(1000, 'y'));
        }
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(0).value<DeviceLine::End>(), DeviceLine::End::Misbehaved);
        QVERIFY(!line.isIn());
        QTRY_VERIFY(!kvm.isOn());
        // Nothing went back for it: the one answer from before, and no goodbye.
        QCOMPARE(kvm.messages().size(), 1);
    }

    void aLineIsOpenedOnceAndNotASecondTime_data()
    {
        QTest::addColumn<bool>("onlyHeld");
        QTest::newRow("the line itself") << false;
        QTest::newRow("the line that only keeps a KVM awake") << true;
    }

    void aLineIsOpenedOnceAndNotASecondTime()
    {
        QFETCH(bool, onlyHeld);
        KvmByHand kvm;
        DeviceLine line(lineTo(kvm), roomy());
        AwakeLine held(lineTo(kvm), 2000);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        const QSignalSpy heldEnded(&held, &AwakeLine::ended);
        const auto open = [&] {
            if (onlyHeld) {
                held.open();
            } else {
                line.open();
            }
        };
        open();
        open();
        QTRY_VERIFY_WITH_TIMEOUT(onlyHeld ? held.isIn() : line.isIn(), 2000);
        // And once more when it is in.
        open();
        QTest::qWait(200);
        QVERIFY(onlyHeld ? held.isIn() : line.isIn());
        QCOMPARE(ended.count() + heldEnded.count(), 0);
        // One line, asked for once: nothing came after the one asking.
        QCOMPARE(kvm.lines, 1);
        QCOMPARE(kvm.bytesAfterTheAsking(), 0);
    }

    void aLineThatWasClosedIsNotOpened()
    {
        KvmByHand kvm;
        {
            // Closed before it was ever opened: nobody holds it, and the pass goes nowhere.
            DeviceLine line(lineTo(kvm), roomy());
            const QSignalSpy ended(&line, &DeviceLine::ended);
            line.close();
            line.open();
            QTest::qWait(300);
            QCOMPARE(kvm.lines, 0);
            QCOMPARE(kvm.bytesHeard, 0);
            QVERIFY(!line.isIn());
            QCOMPARE(ended.count(), 0);
        }
        // Opened, let in, closed: not opened again.
        DeviceLine line(lineTo(kvm), roomy());
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        line.close();
        QTRY_VERIFY(!kvm.isOn());
        const qint64 heard = kvm.bytesHeard;
        line.open();
        QTest::qWait(300);
        QCOMPARE(kvm.lines, 1);
        QCOMPARE(kvm.bytesHeard, heard);
        QVERIFY(!line.isIn());
        QCOMPARE(ended.count(), 0);
    }

    void whatWouldSplitTheAskingIsNotSent_data()
    {
        QTest::addColumn<QByteArray>("asking");
        QTest::addColumn<QByteArray>("cookie");
        const QByteArray asking = "/api/ws?stream=0";
        const QByteArray cookie = "auth_token=pass1";
        QTest::newRow("a line's end in the asking") << asking + "\r\nX-Slipped-In: 1" << cookie;
        QTest::newRow("a bare line feed in the asking") << asking + "\nX-Slipped-In: 1" << cookie;
        QTest::newRow("a bare carriage return in the asking") << asking + "\rX-Slipped-In: 1" << cookie;
        QTest::newRow("a blank in the asking") << asking + " HTTP/1.1" << cookie;
        QTest::newRow("a tab in the asking") << asking + "\tx" << cookie;
        QTest::newRow("a byte of nothing in the asking") << asking + QByteArray(1, '\0') << cookie;
        QTest::newRow("the sign for rubbing out in the asking") << asking + QByteArray(1, '\x7F') << cookie;
        QTest::newRow("a letter beyond ASCII in the asking") << asking + QByteArray("\xC3\xA4", 2) << cookie;
        QTest::newRow("no asking at all") << QByteArray() << cookie;
        QTest::newRow("an asking that is no path") << "api/ws"_ba << cookie;
        QTest::newRow("a line's end in the cookie") << asking << cookie + "\r\nX-Slipped-In: 1";
        QTest::newRow("a bare line feed in the cookie") << asking << cookie + "\nX-Slipped-In: 1";
        QTest::newRow("a second cookie behind the first") << asking << cookie + "; other=1";
        QTest::newRow("a blank in the cookie") << asking << "auth_token=pass 1"_ba;
        QTest::newRow("a cookie that is only a name") << asking << "auth_token="_ba;
        QTest::newRow("a cookie without a name") << asking << "=pass1"_ba;
        QTest::newRow("a cookie that is no pair") << asking << "pass1"_ba;
        QTest::newRow("a name that holds what parts two cookies") << asking << "auth;token=pass1"_ba;
        QTest::newRow("a pass longer than a pass is") << asking << "auth_token=" + QByteArray(513, 'a');
        QTest::newRow("a letter beyond ASCII in the pass") << asking << "auth_token=" + QByteArray("\xC3\xA4", 2);
        QTest::newRow("no cookie at all") << asking << QByteArray();
    }

    void whatWouldSplitTheAskingIsNotSent()
    {
        QFETCH(QByteArray, asking);
        QFETCH(QByteArray, cookie);
        KvmByHand kvm;
        DeviceLine::Where where = lineTo(kvm);
        where.asking = asking;
        where.cookie = cookie;
        DeviceLine line(where, roomy());
        const QSignalSpy letIn(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        // Said a turn of the loop later, as every end is, and not from inside open().
        QCOMPARE(ended.count(), 0);
        QTest::qWait(150);
        QVERIFY2(!kvm.headers.contains("x-slipped-in"), "a header of somebody else's making went out with the asking");
        // No line was made at all: nothing of the asking, and so not the pass, went anywhere.
        QCOMPARE(kvm.lines, 0);
        QCOMPARE(kvm.bytesHeard, 0);
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(0).value<DeviceLine::End>(), DeviceLine::End::NotAsked);
        QCOMPARE(letIn.count(), 0);
        QVERIFY(!line.isIn());
        QVERIFY(!line.say("to nobody"_ba));
    }

    void theLineSaysThatSomethingCameAndNothingOfWhat()
    {
        KvmByHand kvm;
        DeviceLine line(lineTo(kvm), roomy());
        const QSignalSpy heard(&line, &DeviceLine::heard);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        // That it was let in is not something that came on the line.
        QTest::qWait(100);
        QCOMPARE(heard.count(), 0);
        // A word, an "are you there", an answer nobody asked for: each is something.
        kvm.send(text, iAmHere());
        QTRY_COMPARE(heard.count(), 1);
        QCOMPARE(heard.at(0).size(), 0); // that something came, and nothing of what
        kvm.send(areYouThere, "abc");
        QTRY_COMPARE(heard.count(), 2);
        kvm.send(iAm);
        QTRY_COMPARE(heard.count(), 3);
        // So is the first half of a message, and its other half.
        kvm.sendRaw(QByteArray("\x81\x05", 2) + "he");
        QTRY_COMPARE(heard.count(), 4);
        kvm.sendRaw("llo");
        QTRY_COMPARE(heard.count(), 5);
        // What ends the line is not said as something that came.
        kvm.sendRaw(QByteArray("\x83\x00", 2));
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(heard.count(), 5);
    }

    void aLineThatIsCutSaysNoGoodbyeAndNothingAfterIt()
    {
        // (Named for more than it shows before: nothing waits here, and no reset is seen. Both
        // are in the test after this one.)
        KvmByHand kvm;
        DeviceLine line(lineTo(kvm), roomy());
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        QVERIFY(line.say("in"_ba));
        QTRY_COMPARE(kvm.messages().size(), 1);
        line.cut();
        QVERIFY(!line.isIn());
        QVERIFY(!line.say("after it was cut"_ba));
        QTRY_VERIFY(!kvm.isOn());
        // The one text from before, and nothing behind it.
        QCOMPARE(kvm.messages().size(), 1);
        QCOMPARE(kvm.messagesOf(goodbye).size(), 0);
        // Not said as an end, and neither closed nor opened after it.
        line.close();
        line.open();
        QTest::qWait(200);
        QCOMPARE(ended.count(), 0);
        QCOMPARE(kvm.lines, 1);
    }

    void aLineThatThisProgramEndsIsResetAndWhatWaitedHereIsNotSent_data()
    {
        QTest::addColumn<QString>("how");
        // (Of these, only the line that was cut was reset before.)
        QTest::newRow("it is cut") << u"cut"_s;
        QTest::newRow("what was said has waited too long") << u"late"_s;
        QTest::newRow("more waits to go than may") << u"takes nothing"_s;
    }

    void aLineThatThisProgramEndsIsResetAndWhatWaitedHereIsNotSent()
    {
        // The KVM keeps the line and reads nothing. What is said piles up: with Windows, and
        // then in this program. However this program then ends the line, it resets it: closed the
        // ordinary way, Windows would go on delivering what it had taken, and the keys that
        // waited too long would be played when the way came back. Seen here: the reset itself, as
        // the far end's Windows tells it, and that what waited in this program was not sent.
        // (A way that itself stalls no test on one PC can make: there the far end hears of no
        // reset, and it is this end's Windows alone that throws away what it held.)
        QFETCH(QString, how);
        KvmByHand kvm;
        DeviceLine::Limits limits = roomy();
        limits.mostUnsentBytes = how == u"takes nothing"_s ? qint64{65'536} : qint64{64} * 1024 * 1024;
        limits.lateAfterMs = how == u"late"_s ? 100 : 600'000;
        DeviceLine line(lineTo(kvm), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        kvm.stopReading();
        const qintptr farEnd = kvm.descriptor();
        const QByteArray words(1000, 'k');
        const qint64 onTheLine = words.size() + 8;
        qint64 said = 0;
        if (how == u"cut"_s) {
            // Two megabytes: far more than Windows keeps for a line that nobody reads.
            for (int one = 0; one < 2000; ++one) {
                QVERIFY(line.say(words));
                ++said;
            }
            // (The made-up KVM takes the little it keeps, and listens no more.)
            QTest::qWait(100);
            line.cut();
            QCOMPARE(ended.count(), 0);
        } else {
            for (int one = 0; one < 400'000 && ended.isEmpty(); ++one) {
                said += line.say(words) ? 1 : 0;
                if (one % 20 == 19) {
                    QCoreApplication::processEvents();
                }
            }
            QCOMPARE(ended.count(), 1);
            QCOMPARE(ended.at(0).at(0).value<DeviceLine::End>(),
                     how == u"late"_s ? DeviceLine::End::Late : DeviceLine::End::TakesNothing);
        }
        QVERIFY(!line.isIn());
        // What Windows has for the far end of the line, and what it says became of the line.
        Bare came;
        QTRY_VERIFY_WITH_TIMEOUT((takeBare(farEnd, came), !came.end.isEmpty()), 5000);
        QCOMPARE(came.end, "reset"_ba);
        // Less came than was said, also counting all that the made-up KVM keeps of a line it
        // does not read (a kilobyte): what waited in this program went nowhere.
        QVERIFY2(came.bytes + 1024 < said * onTheLine, QByteArray::number(came.bytes).constData());
    }

    // A line that is closed from here, and what becomes of it after the goodbye.

    void aLineThatWasClosedSaysOnceThatItHasGoneWhenTheKvmHasEndedIt()
    {
        KvmByHand kvm;
        DeviceLine::Limits limits = roomy();
        limits.goodbyeWithinMs = 1500;
        DeviceLine line(lineTo(kvm), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        const QSignalSpy gone(&line, &DeviceLine::gone);
        line.open();
        QTRY_VERIFY(line.isIn());
        QVERIFY(line.say("in"_ba));
        line.close();
        // Never from inside the closing, and not while the KVM has not ended the line.
        QCOMPARE(gone.count(), 0);
        QTRY_COMPARE(kvm.messagesOf(goodbye).size(), 1);
        QTest::qWait(200);
        QCOMPARE(gone.count(), 0);
        QVERIFY(kvm.isOn());
        // The KVM ends the line, as one that was said goodbye to does: said then, and not only
        // when the line's time is up.
        QElapsedTimer since;
        since.start();
        kvm.send(goodbye, QByteArray("\x03\xE8", 2));
        kvm.endLine();
        QTRY_COMPARE(gone.count(), 1);
        QVERIFY2(since.elapsed() < 1000, QByteArray::number(since.elapsed()).constData());
        // Once: not a second time when its time is up. And it is no end of the line by itself.
        QTest::qWait(limits.goodbyeWithinMs + 300);
        QCOMPARE(gone.count(), 1);
        QCOMPARE(ended.count(), 0);
    }

    void aLineThatHasGoneSaysWhetherTheKvmAnsweredItsGoodbye_data()
    {
        QTest::addColumn<QString>("how");
        QTest::addColumn<bool>("arrived");
        QTest::newRow("the KVM says its goodbye and ends the line") << u"answered"_s << true;
        QTest::newRow("the KVM says its goodbye, and does not end the line in its time") << u"answered, kept"_s << true;
        QTest::newRow("the KVM ends the line without a word") << u"ended"_s << false;
        QTest::newRow("the line falls") << u"fell"_s << false;
        QTest::newRow("the KVM does not end the line, and its time is up") << u"time"_s << false;
    }

    void aLineThatHasGoneSaysWhetherTheKvmAnsweredItsGoodbye()
    {
        // The KVM's own goodbye, said after the one from here, is the one sign that it got what
        // was said: a line that goes without it may have lost the last of that on the way.
        QFETCH(const QString, how);
        QFETCH(const bool, arrived);
        KvmByHand kvm;
        DeviceLine::Limits limits = roomy();
        limits.goodbyeWithinMs = 700;
        DeviceLine line(lineTo(kvm), limits);
        const QSignalSpy gone(&line, &DeviceLine::gone);
        line.open();
        QTRY_VERIFY(line.isIn());
        QVERIFY(line.say("in"_ba));
        QVERIFY(line.close());
        QTRY_COMPARE(kvm.messagesOf(goodbye).size(), 1);
        if (how.startsWith(u"answered"_s)) {
            kvm.send(goodbye, QByteArray("\x03\xE8", 2));
        }
        if (how == u"answered"_s || how == u"ended"_s) {
            kvm.endLine();
        } else if (how == u"fell"_s) {
            kvm.fallOver();
        }
        QTRY_COMPARE_WITH_TIMEOUT(gone.count(), 1, 3000);
        QCOMPARE(gone.at(0).at(0).toBool(), arrived);
    }

    void aLineThatWasClosedAndThatTheKvmDoesNotEndIsResetWhenItsTimeIsUp()
    {
        // The KVM keeps the line and reads nothing, and more was said than Windows holds for such
        // a line: so the goodbye waits here behind it, and the KVM never ends the line. When the
        // line's time is up it is reset, as the far end's Windows tells it, and only then is it
        // said to have gone.
        KvmByHand kvm;
        DeviceLine::Limits limits = roomy();
        limits.goodbyeWithinMs = 900;
        limits.lateAfterMs = 600'000;
        limits.mostUnsentBytes = qint64{64} * 1024 * 1024;
        DeviceLine line(lineTo(kvm), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        const QSignalSpy gone(&line, &DeviceLine::gone);
        line.open();
        QTRY_VERIFY(line.isIn());
        kvm.stopReading();
        const qintptr farEnd = kvm.descriptor();
        const QByteArray words(1000, 'k');
        const int said = 300;
        for (int one = 0; one < said; ++one) {
            QVERIFY(line.say(words));
        }
        // (The made-up KVM takes the little it keeps, and listens no more.)
        QTest::qWait(100);
        QElapsedTimer since;
        since.start();
        line.close();
        QCOMPARE(gone.count(), 0);
        // Not before its time is up: the line stands, and nothing is said of it.
        QTest::qWait(limits.goodbyeWithinMs - 400);
        QCOMPARE(gone.count(), 0);
        Bare came;
        takeBare(farEnd, came);
        QCOMPARE(came.end, QByteArray());
        QTRY_COMPARE_WITH_TIMEOUT(gone.count(), 1, 4000);
        QVERIFY2(since.elapsed() >= limits.goodbyeWithinMs - 50, QByteArray::number(since.elapsed()).constData());
        QTRY_VERIFY_WITH_TIMEOUT((takeBare(farEnd, came), !came.end.isEmpty()), 5000);
        QCOMPARE(came.end, "reset"_ba);
        // Less came than was said: what waited in this program, the goodbye behind it, went nowhere.
        QVERIFY2(came.bytes + 1024 < said * (words.size() + 8), QByteArray::number(came.bytes).constData());
        QTest::qWait(300);
        QCOMPARE(gone.count(), 1);
        QCOMPARE(ended.count(), 0);
    }

    void aLineThatHadEndedBeforeItWasClosedSaysThatItHasGoneATurnLater_data()
    {
        QTest::addColumn<bool>("cut");
        QTest::newRow("it ended by itself") << false;
        QTest::newRow("it was cut") << true;
    }

    void aLineThatHadEndedBeforeItWasClosedSaysThatItHasGoneATurnLater()
    {
        QFETCH(bool, cut);
        KvmByHand kvm;
        DeviceLine::Limits limits = roomy();
        limits.goodbyeWithinMs = 3000;
        DeviceLine line(lineTo(kvm), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        const QSignalSpy gone(&line, &DeviceLine::gone);
        line.open();
        QTRY_VERIFY(line.isIn());
        if (cut) {
            line.cut();
        } else {
            kvm.fallOver();
            QTRY_COMPARE(ended.count(), 1);
        }
        QTest::qWait(100);
        QCOMPARE(gone.count(), 0);
        // There is nothing to wait for: said a turn later, never from inside, and far sooner than
        // a line that is waited for.
        QElapsedTimer since;
        since.start();
        line.close();
        QCOMPARE(gone.count(), 0);
        QTRY_COMPARE(gone.count(), 1);
        QVERIFY2(since.elapsed() < 1000, QByteArray::number(since.elapsed()).constData());
        line.close();
        QTest::qWait(200);
        QCOMPARE(gone.count(), 1);
        QCOMPARE(ended.count(), cut ? 0 : 1);
    }

    void aLineThatIsResetAtOnceWhereItIsClosedSaysThatItHasGoneATurnLater_data()
    {
        QTest::addColumn<bool>("letIn");
        QTest::newRow("the KVM had not let it in") << false;
        QTest::newRow("what was said has waited too long") << true;
    }

    void aLineThatIsResetAtOnceWhereItIsClosedSaysThatItHasGoneATurnLater()
    {
        // No goodbye is said on such a line, and the KVM is not waited for: the line is reset in
        // the closing, and said to have gone a turn of the loop later.
        QFETCH(bool, letIn);
        KvmByHand kvm;
        kvm.answers = letIn;
        DeviceLine::Limits limits = roomy();
        limits.goodbyeWithinMs = 4000;
        limits.lateAfterMs = 800;
        limits.mostUnsentBytes = qint64{64} * 1024 * 1024;
        DeviceLine line(lineTo(kvm), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        const QSignalSpy gone(&line, &DeviceLine::gone);
        line.open();
        qintptr farEnd = -1;
        if (letIn) {
            QTRY_VERIFY(line.isIn());
            // The KVM keeps the line and reads nothing; half a megabyte is said in one go, far
            // more than Windows holds for such a line, so some of it waits here. Nothing is said
            // after it, so the line does not end by itself over what waits.
            kvm.stopReading();
            farEnd = kvm.descriptor();
            for (int one = 0; one < 5; ++one) {
                QVERIFY(line.say(QByteArray(100'000, 'k')));
            }
            QTest::qWait(limits.lateAfterMs + 300);
        } else {
            QTRY_VERIFY(kvm.wasAsked());
        }
        QCOMPARE(ended.count(), 0);
        QElapsedTimer since;
        since.start();
        line.close();
        QCOMPARE(gone.count(), 0);
        QTRY_COMPARE(gone.count(), 1);
        QVERIFY2(since.elapsed() < 1500, QByteArray::number(since.elapsed()).constData());
        if (letIn) {
            Bare came;
            QTRY_VERIFY_WITH_TIMEOUT((takeBare(farEnd, came), !came.end.isEmpty()), 2000);
            QCOMPARE(came.end, "reset"_ba);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(!kvm.isOn(), 2000);
            QCOMPARE(kvm.bytesAfterTheAsking(), 0);
        }
        QVERIFY2(since.elapsed() < limits.goodbyeWithinMs - 500, QByteArray::number(since.elapsed()).constData());
        QTest::qWait(200);
        QCOMPARE(gone.count(), 1);
        QCOMPARE(ended.count(), 0);
    }

    // The rule for a certificate that Windows objects to, which the line has from the driver.

    void whatWindowsObjectsToIsPassedOverOnlyForTheVeryCertificateAKvmIsKnownBy()
    {
        using core::kvm::Objected;
        using core::kvm::objectedTo;
        const QList<QSslCertificate> chain = QSslCertificate::fromPath(tlsOf("signed").certificateFile, QSsl::Pem);
        QCOMPARE(chain.size(), 2);
        const QSslCertificate& shown = chain.at(0);
        const QSslCertificate& signer = chain.at(1);
        const QSslCertificate other = StandIn::certificateIn(tlsOf("second").certificateFile);
        const QByteArray knownBy = sha256Of("signed");
        const QSslError itsSigner(QSslError::UnableToGetLocalIssuerCertificate, shown);
        const QSslError itsName(QSslError::HostNameMismatch, shown);
        const QSslError theSigners(QSslError::UnableToGetIssuerCertificate, signer);
        // The one it is known by: passed over where every objection names that certificate itself,
        // and where there is none.
        QVERIFY(objectedTo(shown, {itsSigner, itsName}, true, knownBy) == Objected::PassedOver);
        QVERIFY(objectedTo(shown, {}, true, knownBy) == Objected::PassedOver);
        // Not where one of them names another certificate (its signer's, sent along), or none.
        QVERIFY(objectedTo(shown, {itsSigner, theSigners}, true, knownBy) == Objected::BySigner);
        QVERIFY(objectedTo(shown, {theSigners}, true, knownBy) == Objected::BySigner);
        QVERIFY(objectedTo(shown, {itsName, QSslError(QSslError::UnspecifiedError)}, true, knownBy) ==
                Objected::BySigner);
        // Another than it is known by, whatever is objected to in it; and none at all.
        QVERIFY(objectedTo(other, {QSslError(QSslError::SelfSignedCertificate, other)}, true, knownBy) ==
                Objected::Changed);
        QVERIFY(objectedTo(other, {}, true, knownBy) == Objected::Changed);
        QVERIFY(objectedTo(signer, {theSigners}, true, knownBy) == Objected::Changed);
        QVERIFY(objectedTo(QSslCertificate(), {}, true, knownBy) == Objected::Changed);
        QVERIFY(objectedTo(QSslCertificate(), {}, true, QByteArray()) == Objected::Changed);
        QVERIFY(objectedTo(shown, {itsName}, true, knownBy.left(20)) == Objected::Changed);
        // Known by none: Windows has to trust it, and has just said that it does not.
        QVERIFY(objectedTo(shown, {itsName}, false, QByteArray()) == Objected::Untrusted);
        QVERIFY(objectedTo(shown, {itsName}, false, knownBy) == Objected::Untrusted);
    }

    // The line that only keeps a KVM awake.

    void theLineThatKeepsAKvmAwakeAnswersAnAreYouThereHiddenAndSaysNothingElse()
    {
        KvmByHand kvm;
        AwakeLine line(lineTo(kvm), 2000);
        const QSignalSpy ended(&line, &AwakeLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        QCOMPARE(kvm.path, "/api/ws?stream=0"_ba);
        QCOMPARE(kvm.headers.value("cookie"), "auth_token=pass1"_ba);
        kvm.send(areYouThere, "abc");
        kvm.send(areYouThere);
        kvm.send(areYouThere, QByteArray(125, 'x'));
        QTRY_COMPARE(kvm.messages().size(), 3);
        const QList<KvmByHand::Message> answers = kvm.messages();
        for (const KvmByHand::Message& one : answers) {
            QCOMPARE(one.kind, iAm);
            QVERIFY(one.whole);
            QVERIFY(!one.spare);
            QVERIFY(one.hidden);
            QCOMPARE(one.lengthBytes, 1);
        }
        QCOMPARE(answers.at(0).what, "abc"_ba);
        QCOMPARE(answers.at(1).what, QByteArray());
        QCOMPARE(answers.at(2).what, QByteArray(125, 'x'));
        QVERIFY(answers.at(2).asItCame != answers.at(2).what);
        QVERIFY(answers.at(0).hiddenBy != answers.at(2).hiddenBy);
        // What a KVM says by itself is answered with nothing.
        kvm.send(text, R"({"event_type":"loop","event":{}})");
        QTest::qWait(200);
        QCOMPARE(kvm.messages().size(), 3);
        QCOMPARE(ended.count(), 0);
        // And its goodbye ends the line, once.
        kvm.send(goodbye);
        QTRY_COMPARE(ended.count(), 1);
        QVERIFY(!line.isIn());
    }

    // Keyboard and mouse.

    void keysArriveAsKvmdsWordsOnceTheKvmHasLetTheLineIn_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("in the clear") << false;
        QTest::newRow("over TLS, to the certificate the KVM is known by") << true;
    }

    void keysArriveAsKvmdsWordsOnceTheKvmHasLetTheLineIn()
    {
        QFETCH(bool, encrypted);
        const std::unique_ptr<KvmByHand> kvm = kvmOver(encrypted);
        Console console(whereOf(*kvm));
        QVERIFY(!console.line.isOpen());
        QTRY_COMPARE(console.opened.count(), 1);
        QVERIFY(console.line.isOpen());
        // Asked for as a browser asks for it, at the line's own place, with the pass.
        QCOMPARE(kvm->lines, 1);
        QCOMPARE(kvm->path, "/api/ws?stream=0"_ba);
        QCOMPARE(kvm->headers.value("cookie"), "auth_token=pass1"_ba);
        QCOMPARE(kvm->headers.value("upgrade").toLower(), "websocket"_ba);
        QCOMPARE(kvm->headers.value("sec-websocket-version"), "13"_ba);
        QCOMPARE(kvm->headers.value("host"), "127.0.0.1:" + QByteArray::number(kvm->address().port()));
        QVERIFY(!kvm->headers.contains("authorization"));

        console.line.key(keyA, true);
        console.line.key(keyA, false);
        QTRY_COMPARE(kvm->messages().size(), 2);
        const QList<KvmByHand::Message> all = kvm->messages();
        for (const KvmByHand::Message& one : all) {
            QCOMPARE(one.kind, text);
            QVERIFY(one.whole);
            QVERIFY(!one.spare);
            QVERIFY(one.hidden);
        }
        QCOMPARE(kvm->said().at(0), keyOf(u"KeyA"_s, true));
        QCOMPARE(kvm->said().at(1), keyOf(u"KeyA"_s, false));
        QCOMPARE(console.closed.count(), 0);
    }

    void aKvmThatWindowsTrustsForItsNameIsSpokenToAsWell()
    {
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        KvmByHand kvm(tlsOf("signed"));
        KvmdConsoleLine::Where where = whereOf(kvm);
        where.knownByCertificate = false;
        where.certificate.clear();
        Console console(where);
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(keyB, true);
        QTRY_COMPARE(kvm.said().size(), 1);
        QCOMPARE(kvm.said().at(0), keyOf(u"KeyB"_s, true));
    }

    void whatStandsBehindTheQuestionMarkIsTheCallersToChoose()
    {
        KvmByHand kvm;
        KvmdConsoleLine::Where where = whereOf(kvm);
        where.query = "stream=1&auth_token=pass1";
        const Console watched(where);
        QTRY_COMPARE(watched.opened.count(), 1);
        QCOMPARE(kvm.path, "/api/ws?stream=1&auth_token=pass1"_ba);
        where.query.clear();
        const Console plain(where);
        QTRY_COMPARE(plain.opened.count(), 1);
        QCOMPARE(kvm.lines, 2);
        QCOMPARE(kvm.path, QByteArray(KvmdConsoleLine::path));
        QCOMPARE(kvm.path, "/api/ws"_ba);
    }

    void nothingIsSaidBeforeTheKvmHasLetTheLineInAndNothingOfItLater()
    {
        KvmByHand kvm;
        kvm.answers = false;
        Console console(whereOf(kvm));
        // Before a line is there at all, and while the KVM has not answered the asking for it.
        console.line.key(keyA, true);
        QTRY_VERIFY(kvm.wasAsked());
        console.line.key(keyB, true);
        console.line.button(1, true);
        console.line.pointerAt(100, 100);
        console.line.pointerBy(5, 5);
        console.line.wheel(0, 1);
        console.line.releaseAll();
        QTest::qWait(150);
        QVERIFY(!console.line.isOpen());
        QCOMPARE(kvm.bytesAfterTheAsking(), 0);
        // Let in: none of it arrives now. It was dropped, not kept.
        kvm.letIn();
        QTRY_COMPARE(console.opened.count(), 1);
        QTest::qWait(150);
        QCOMPARE(kvm.bytesAfterTheAsking(), 0);
        // Nor are those keys down for this line: letting go of them says nothing.
        console.line.key(keyA, false);
        console.line.key(keyB, false);
        console.line.button(1, false);
        console.line.releaseAll();
        // What is pressed now is said now.
        console.line.key(keyB, true);
        QTRY_COMPARE(kvm.messages().size(), 1);
        QTest::qWait(100);
        QCOMPARE(kvm.said(), QList<QJsonObject>{keyOf(u"KeyB"_s, true)});
    }

    void aKeyHeldAcrossTheOpeningIsNotPressedByItsRepeat()
    {
        // Somebody presses Enter to take a machine and holds it a moment. Windows repeats a held
        // key, and the repeat that comes once the line stands is no press: Enter was pressed
        // here, for this PC, before the machine could be told anything.
        KvmByHand kvm;
        kvm.answers = false;
        Console console(whereOf(kvm));
        console.line.key(enter, true); // before a line is there at all
        console.line.button(1, true);
        QTRY_VERIFY(kvm.wasAsked());
        console.line.key(enter, true); // and while the KVM has not answered
        console.line.key(leftShift, true);
        kvm.letIn();
        QTRY_COMPARE(console.opened.count(), 1);
        for (int repeat = 0; repeat < 3; ++repeat) {
            console.line.key(enter, true);
            console.line.key(leftShift, true);
            console.line.button(1, true);
        }
        // (This test had the next press said: A, with Shift held here and up on the machine,
        // which was the defect: a small "a" where a capital was typed. Under a modifier that is
        // held here unsaid no key's press is said, and that key is held too.)
        console.line.key(keyA, true);
        QTest::qWait(150);
        QCOMPARE(kvm.messages().size(), 0);
        console.line.key(keyA, false);
        // (And it had a button that is pressed now said, "the first thing the machine hears":
        // a click where Shift and a click was pressed, the same defect. Nor is a button's press
        // said under a modifier that is held here unsaid, and that button is held too.)
        console.line.button(2, true);
        QTest::qWait(150);
        QCOMPARE(kvm.messages().size(), 0);
        // Letting go of everything says nothing for what never went down there, and does not
        // make the held ones new: they are still held here.
        console.line.releaseAll();
        console.line.key(enter, true);
        console.line.button(1, true);
        console.line.button(2, true);
        // Told to be up, they are that, without a word. Their next press is none yet: Shift is
        // still held here, and still unsaid.
        console.line.key(enter, false);
        console.line.button(1, false);
        console.line.button(2, false);
        console.line.key(enter, true);
        console.line.button(1, true);
        console.line.key(leftShift, true);
        QTest::qWait(150);
        QCOMPARE(kvm.messages().size(), 0);
        // Shift, Enter and the button are let go here, without a word, and the next press of
        // each is a press.
        console.line.key(leftShift, false);
        console.line.key(enter, false);
        console.line.button(1, false);
        QTest::qWait(100);
        QCOMPARE(kvm.messages().size(), 0);
        console.line.key(leftShift, true);
        console.line.key(enter, true);
        console.line.button(1, true);
        const QList<QJsonObject> expected{keyOf(u"ShiftLeft"_s, true), keyOf(u"Enter"_s, true),
                                          buttonOf(u"left"_s, true)};
        QTRY_COMPARE(kvm.messages().size(), expected.size());
        QCOMPARE(kvm.said(), expected);
    }

    void whatIsStillHeldWhenEverythingWasLetGoIsNotPressedAgainByItsRepeat()
    {
        KvmByHand kvm;
        Console console(whereOf(kvm));
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(keyA, true);
        console.line.button(1, true);
        QTRY_COMPARE(kvm.messages().size(), 2);
        // The window went to the back, say, with a finger still on the key and on the button.
        console.line.releaseAll();
        QTRY_COMPARE(kvm.messages().size(), 4);
        // Windows goes on repeating the key: that is no press.
        for (int repeat = 0; repeat < 3; ++repeat) {
            console.line.key(keyA, true);
            console.line.button(1, true);
        }
        console.line.releaseAll();
        QTest::qWait(150);
        QCOMPARE(kvm.messages().size(), 4);
        // The finger comes off: nothing to say, the machine has it up already. Then it is a key
        // like any other again.
        console.line.key(keyA, false);
        console.line.button(1, false);
        QTest::qWait(100);
        QCOMPARE(kvm.messages().size(), 4);
        console.line.key(keyA, true);
        console.line.button(1, true);
        console.line.key(keyA, false);
        const QList<QJsonObject> expected{
            keyOf(u"KeyA"_s, true), buttonOf(u"left"_s, true), buttonOf(u"left"_s, false), keyOf(u"KeyA"_s, false),
            keyOf(u"KeyA"_s, true), buttonOf(u"left"_s, true), keyOf(u"KeyA"_s, false)};
        QTRY_COMPARE(kvm.messages().size(), expected.size());
        QCOMPARE(kvm.said(), expected);
    }

    void aKeyPressedUnderAModifierThatIsHeldHereAndUpOnTheMachineIsNotSaid_data()
    {
        QTest::addColumn<bool>("inTexts");
        QTest::newRow("told in bytes") << false;
        QTest::newRow("told in texts") << true;
    }

    void aKeyPressedUnderAModifierThatIsHeldHereAndUpOnTheMachineIsNotSaid()
    {
        QFETCH(bool, inTexts);
        KvmByHand kvm;
        KvmdConsoleLine::Where where = whereOf(kvm);
        where.inTexts = inTexts;
        Console console(where);
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(leftControl, true);
        console.line.key(keyC, true);
        // The window goes to the back with both fingers down: everything is let go on the machine.
        console.line.releaseAll();
        const QStringList letGo{u"+ControlLeft"_s, u"+KeyC"_s, u"-KeyC"_s, u"-ControlLeft"_s};
        QTRY_COMPARE(keysToldTo(kvm), letGo);
        // The finger comes off C and goes down on it again, Control still held here. On the
        // machine Control is up: said now, C would arrive bare, a "c" typed into a shell where
        // Ctrl+C was pressed. It is not said.
        console.line.key(keyC, false);
        console.line.key(keyC, true);
        // Nor is Alt, pressed under the Control that is held unsaid; nor Delete under the two,
        // which would arrive as a bare Delete.
        console.line.key(leftAlt, true);
        console.line.key(del, true);
        // Nor what Windows repeats of them.
        console.line.key(keyC, true);
        console.line.key(del, true);
        QTest::qWait(150);
        QCOMPARE(keysToldTo(kvm), letGo);
        // Control is let go here, and Alt is still held. C and Delete went down unsaid, and stay
        // so until they were let go here: no repeat of theirs is a press. And a key that goes
        // down now, under Alt, is not said either.
        console.line.key(leftControl, false);
        console.line.key(keyC, true);
        console.line.key(del, true);
        console.line.key(keyA, true);
        QTest::qWait(150);
        QCOMPARE(keysToldTo(kvm), letGo);
        // Every finger comes off: nothing is said for what never went down on the machine.
        console.line.key(leftAlt, false);
        console.line.key(keyC, false);
        console.line.key(del, false);
        console.line.key(keyA, false);
        console.line.releaseAll();
        QTest::qWait(150);
        QCOMPARE(keysToldTo(kvm), letGo);
        // Pressed anew, they are keys like any other: Control goes down on the machine, and C
        // under it.
        console.line.key(leftControl, true);
        console.line.key(keyC, true);
        const QStringList andAnew = letGo + QStringList{u"+ControlLeft"_s, u"+KeyC"_s};
        QTRY_COMPARE(keysToldTo(kvm), andAnew);
        QCOMPARE(console.closed.count(), 0);
    }

    void aButtonPressedUnderAModifierThatIsHeldHereAndUpOnTheMachineIsNotSaid_data()
    {
        QTest::addColumn<bool>("inTexts");
        QTest::newRow("told in bytes") << false;
        QTest::newRow("told in texts") << true;
    }

    void aButtonPressedUnderAModifierThatIsHeldHereAndUpOnTheMachineIsNotSaid()
    {
        QFETCH(bool, inTexts);
        KvmByHand kvm;
        KvmdConsoleLine::Where where = whereOf(kvm);
        where.inTexts = inTexts;
        Console console(where);
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(leftControl, true);
        // The window goes to the back with Control down: it is let go on the machine, and still
        // held here.
        console.line.releaseAll();
        const QStringList letGo{u"+ControlLeft"_s, u"-ControlLeft"_s};
        QTRY_COMPARE(keysToldTo(kvm), letGo);
        // A click now would arrive bare, where Control and a click was pressed: one thing
        // picked, where it was to be added to those that are. Neither its press nor its release
        // is said.
        console.line.button(1, true);
        console.line.button(1, false);
        // Nor the press of another, however often that is told while it stays down.
        console.line.button(2, true);
        console.line.button(2, true);
        // Where the pointer is, and the wheel, are said as ever.
        console.line.pointerAt(100, 200);
        console.line.wheel(0, 1);
        const QStringList thePointerAlone{u"at"_s, u"wheel"_s};
        QTRY_COMPARE(mouseToldTo(kvm), thePointerAlone);
        QTest::qWait(150);
        QCOMPARE(mouseToldTo(kvm), thePointerAlone);
        // Control is let go here. The button that went down under it stays unsaid until it was
        // let go here: nothing for it, and nothing for its release.
        console.line.key(leftControl, false);
        console.line.button(2, true);
        console.line.button(2, false);
        QTest::qWait(150);
        QCOMPARE(mouseToldTo(kvm), thePointerAlone);
        // From here a click is a click.
        console.line.button(2, true);
        console.line.button(2, false);
        console.line.button(1, true);
        const QStringList andClicks = thePointerAlone + QStringList{u"+right"_s, u"-right"_s, u"+left"_s};
        QTRY_COMPARE(mouseToldTo(kvm), andClicks);
        QCOMPARE(keysToldTo(kvm), letGo);
        QCOMPARE(console.closed.count(), 0);
    }

    void aHeldKeyIsSaidOnceAndAKeyThatNeverWentDownIsNotLetGo()
    {
        KvmByHand kvm;
        Console console(whereOf(kvm));
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(keyA, true);
        console.line.key(keyA, true); // Windows repeats a held key
        console.line.key(keyA, true);
        console.line.key(keyB, false); // never went down here
        console.line.key(keyA, false);
        console.line.key(keyA, false);
        // A number that is no key, and one kvmd has no name for: neither said, nor kept as down.
        console.line.key(0, true);
        console.line.key(0xA5, true);
        console.line.key(0xA5, false);
        console.line.key(-1, true);
        console.line.button(1, true);
        console.line.button(1, true);
        console.line.button(2, false);
        console.line.button(1, false);
        console.line.button(1, false);
        console.line.button(0, true);
        console.line.button(6, true);
        console.line.key(keyA, true); // and down again after it went up is said again
        const QList<QJsonObject> expected{keyOf(u"KeyA"_s, true), keyOf(u"KeyA"_s, false), buttonOf(u"left"_s, true),
                                          buttonOf(u"left"_s, false), keyOf(u"KeyA"_s, true)};
        QTRY_COMPARE(kvm.messages().size(), expected.size());
        QTest::qWait(100);
        QCOMPARE(kvm.said(), expected);
    }

    void thePointerItsButtonsAndTheWheelAreSaidInTheOrderTheyCame()
    {
        KvmByHand kvm;
        Console console(whereOf(kvm));
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.pointerAt(0, 32767);
        console.line.button(2, true);
        console.line.pointerBy(300, -10);
        console.line.pointerBy(0, 0);
        console.line.wheel(0, -5);
        console.line.wheel(0, 0);
        console.line.button(2, false);
        console.line.button(4, true);
        console.line.button(5, true);
        const QJsonObject to{{u"x"_s, -32768}, {u"y"_s, 32767}};
        const QList<QJsonObject> expected{
            QJsonObject{{u"event_type"_s, u"mouse_move"_s}, {u"event"_s, QJsonObject{{u"to"_s, to}}}},
            buttonOf(u"right"_s, true),
            deltaOf(u"mouse_relative"_s, 100, -3),
            deltaOf(u"mouse_relative"_s, 100, -3),
            deltaOf(u"mouse_relative"_s, 100, -4),
            deltaOf(u"mouse_wheel"_s, 0, -5),
            buttonOf(u"right"_s, false),
            buttonOf(u"up"_s, true),
            buttonOf(u"down"_s, true),
        };
        QTRY_COMPARE(kvm.messages().size(), expected.size());
        QTest::qWait(100);
        QCOMPARE(kvm.said(), expected);
    }

    void lettingGoOfEverythingSaysEveryButtonUpAndThenTheKeysTheLastDownFirst()
    {
        // (This test had it the other way round, the keys first and in whichever order, which
        // was the defect: Shift up before the key it was held with leaves that key bare, and a
        // key up before the button changes what is being dragged.)
        KvmByHand kvm;
        Console console(whereOf(kvm));
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.button(3, true);
        console.line.key(leftShift, true);
        console.line.button(1, true);
        console.line.key(keyA, true);
        console.line.key(keyF5, true);
        console.line.key(keyB, true);
        console.line.key(keyB, false); // up again already: not let go a second time
        QTRY_COMPARE(kvm.messages().size(), 7);
        console.line.releaseAll();
        QTRY_COMPARE(kvm.messages().size(), 12);
        // The buttons first, then the keys; of each the last that went down first, so that
        // Shift, which went down before the keys it was held with, goes up after them.
        const QList<QJsonObject> lettingGo{buttonOf(u"left"_s, false), buttonOf(u"middle"_s, false),
                                           keyOf(u"F5"_s, false), keyOf(u"KeyA"_s, false),
                                           keyOf(u"ShiftLeft"_s, false)};
        QCOMPARE(kvm.said().mid(7), lettingGo);
        // They are forgotten: nothing is let go a second time, by name or all together.
        console.line.releaseAll();
        console.line.key(keyA, false);
        console.line.button(1, false);
        QTest::qWait(150);
        QCOMPARE(kvm.messages().size(), 12);
        // (This test had A said again here, with Shift, which nobody has said is up, still held
        // here and let go on the machine: a small "a" where a capital was typed, the defect.)
        console.line.key(keyA, true);
        QTest::qWait(150);
        QCOMPARE(kvm.messages().size(), 12);
        console.line.key(keyA, false);
        // Shift is let go here as well: a key that goes down again is said again.
        console.line.key(leftShift, false);
        console.line.key(keyA, true);
        QTRY_COMPARE(kvm.messages().size(), 13);
        QCOMPARE(kvm.said().at(12), keyOf(u"KeyA"_s, true));
        QVERIFY(console.line.isOpen());
    }

    void closingLetsGoOfWhatIsDownAndThenSaysGoodbye_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("in the clear") << false;
        QTest::newRow("over TLS") << true;
    }

    void closingLetsGoOfWhatIsDownAndThenSaysGoodbye()
    {
        QFETCH(bool, encrypted);
        const std::unique_ptr<KvmByHand> kvm = kvmOver(encrypted);
        Console console(whereOf(*kvm));
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(keyA, true);
        console.line.button(1, true);
        console.line.close();
        QVERIFY(!console.line.isOpen());
        // The line is let go, and all of it had arrived by then.
        QTRY_VERIFY(!kvm->isOn());
        const QList<KvmByHand::Message> all = kvm->messages();
        QCOMPARE(all.size(), 5);
        // (The button before the key: this had them the other way round, as the line did.)
        const QList<QJsonObject> expected{keyOf(u"KeyA"_s, true), buttonOf(u"left"_s, true), buttonOf(u"left"_s, false),
                                          keyOf(u"KeyA"_s, false)};
        QCOMPARE(kvm->said(), expected);
        QCOMPARE(all.at(4).kind, goodbye);
        QVERIFY(all.at(4).whole);
        QVERIFY(all.at(4).hidden);
        QCOMPARE(all.at(4).what, QByteArray("\x03\xE8", 2));
        // An end from here is not said as an end, and nothing is said on the line after it.
        console.line.key(keyB, true);
        console.line.releaseAll();
        console.line.close();
        QTest::qWait(quick().goodbyeWithinMs + 200);
        QCOMPARE(console.closed.count(), 0);
        QCOMPARE(kvm->messages().size(), 5);
        QCOMPARE(kvm->lines, 1);
    }

    void aLineClosedBeforeItWasBegunAsksForNothing()
    {
        KvmByHand kvm;
        Console console(whereOf(kvm));
        console.line.close();
        QTest::qWait(300);
        QCOMPARE(kvm.lines, 0);
        QCOMPARE(kvm.bytesHeard, 0);
        QCOMPARE(console.opened.count(), 0);
        QCOMPARE(console.closed.count(), 0);
    }

    void aLineThrownAwayWithKeysDownLetsGoOfThemFirst()
    {
        KvmByHand kvm;
        auto console = std::make_unique<Console>(whereOf(kvm));
        QTRY_COMPARE(console->opened.count(), 1);
        console->line.key(leftShift, true);
        console->line.button(2, true);
        QTRY_COMPARE(kvm.messages().size(), 2);
        console.reset();
        QTRY_VERIFY(!kvm.isOn());
        // (The button before the key: this had them the other way round, as the line did.)
        const QList<QJsonObject> expected{keyOf(u"ShiftLeft"_s, true), buttonOf(u"right"_s, true),
                                          buttonOf(u"right"_s, false), keyOf(u"ShiftLeft"_s, false)};
        QCOMPARE(kvm.said(), expected);
        QCOMPARE(kvm.messagesOf(goodbye).size(), 1);
        QCOMPARE(kvm.messages().size(), 5);
    }

    void closingSaysNothingOfAnEndAlsoWhereTheLineTakesNoMoreMeanwhile()
    {
        // Room for a key going down, and none for the same key going up (a byte longer): what
        // the line takes no more of, and ends over, is the letting go on closing. Whoever closes
        // a line has ended it, and is not told from inside that call that it ended by itself.
        KvmByHand kvm;
        KvmdConsoleLine::Pace pace = quick();
        pace.mostUnsentBytes = core::kvmd::keyWord(keyA, true).value_or(QByteArray()).size() + aroundAShortMessage;
        Console console(whereOf(kvm), pace);
        QTRY_COMPARE(console.opened.count(), 1);
        int told = 0;
        connect(&console.line, &KvmdConsoleLine::closed, this, [&told] { ++told; });
        console.line.key(keyA, true);
        QTRY_COMPARE(kvm.messages().size(), 1);
        console.line.close();
        QCOMPARE(told, 0);
        QVERIFY(!console.line.isOpen());
        // Nor later. The line is gone all the same.
        QTRY_VERIFY(!kvm.isOn());
        QTest::qWait(quick().goodbyeWithinMs + 200);
        QCOMPARE(told, 0);
        QCOMPARE(console.closed.count(), 0);
        // What it took no more of did not arrive, and no goodbye behind it: the key is still
        // down on the machine, which is the KVM's to mend.
        QCOMPARE(kvm.said(), QList<QJsonObject>{keyOf(u"KeyA"_s, true)});
        QCOMPARE(kvm.messages().size(), 1);
        // And the line can be used as one that was closed: nothing comes of anything.
        console.line.key(keyB, true);
        console.line.releaseAll();
        console.line.close();
        QTest::qWait(100);
        QCOMPARE(kvm.lines, 1);
        QCOMPARE(told, 0);
    }

    void closingSaysWhetherEverythingThatWasDownCouldBeLetGo_data()
    {
        QTest::addColumn<QString>("how");
        QTest::addColumn<bool>("allLetGo");
        QTest::newRow("a key and a button were down, and were said up") << u"said"_s << true;
        QTest::newRow("nothing was down") << u"nothing"_s << true;
        QTest::newRow("the KVM had not let the line in") << u"not in"_s << true;
        QTest::newRow("the line takes no more while it is let go of") << u"no more"_s << false;
        QTest::newRow("what was said up went with a line that is reset in the closing") << u"reset"_s << false;
        // (What that one left down it said when it ended.)
        QTest::newRow("the line had ended by itself") << u"ended"_s << true;
    }

    void closingSaysWhetherEverythingThatWasDownCouldBeLetGo()
    {
        QFETCH(QString, how);
        QFETCH(bool, allLetGo);
        KvmByHand kvm;
        kvm.answers = how != u"not in"_s;
        KvmdConsoleLine::Pace pace = quick();
        if (how == u"no more"_s) {
            // Room for a key going down, and none for the same key going up (a byte longer).
            pace.mostUnsentBytes = core::kvmd::keyWord(keyA, true).value_or(QByteArray()).size() + aroundAShortMessage;
        } else if (how == u"reset"_s) {
            pace.lateAfterMs = 1500;
            pace.mostUnsentBytes = qint64{64} * 1024 * 1024;
        }
        Console console(whereOf(kvm), pace);
        if (how == u"not in"_s) {
            QTRY_VERIFY(kvm.wasAsked());
            console.line.key(keyA, true);
            QCOMPARE(console.line.close(), allLetGo);
            return;
        }
        QTRY_COMPARE(console.opened.count(), 1);
        if (how != u"nothing"_s) {
            console.line.key(keyA, true);
            QTRY_COMPARE(kvm.messages().size(), 1);
        }
        if (how == u"said"_s) {
            console.line.button(1, true);
        } else if (how == u"reset"_s) {
            // The KVM is there, and says so; then it keeps the line and reads nothing. The key
            // goes up, which Windows takes, and much is said behind it in one go, far more than
            // Windows holds for such a line: that waits here, and for too long by the time the
            // line is closed. So no goodbye is said and nothing pushed out: the line is reset,
            // and the "up" goes with it, which the KVM was not heard from after.
            kvm.send(text, iAmHere());
            QTest::qWait(150);
            kvm.stopReading();
            console.line.key(keyA, false);
            for (int one = 0; one < 6000; ++one) {
                console.line.pointerAt(one, 7);
            }
            QCOMPARE(console.closed.count(), 0);
            QTest::qWait(pace.lateAfterMs + 300);
            QCOMPARE(console.closed.count(), 0);
        } else if (how == u"ended"_s) {
            kvm.fallOver();
            QTRY_COMPARE(console.closed.count(), 1);
            QVERIFY(console.leftDown());
        }
        QCOMPARE(console.line.close(), allLetGo);
        // Said once: closed, it has nothing left to say.
        QVERIFY(console.line.close());
        QCOMPARE(console.closed.count(), how == u"ended"_s ? 1 : 0);
    }

    void aLineThatIsLetGoIsKeptUntilItHasGoneAndSaysNothingToWhoeverHeldIt_data()
    {
        QTest::addColumn<QString>("how");
        QTest::newRow("the KVM ends it after the goodbye") << u"ended"_s;
        QTest::newRow("the KVM does not end it, and its time is up") << u"time"_s;
        QTest::newRow("it had ended by itself before") << u"before"_s;
        QTest::newRow("the letting go itself ended it") << u"letting go"_s;
    }

    void aLineThatIsLetGoIsKeptUntilItHasGoneAndSaysNothingToWhoeverHeldIt()
    {
        QFETCH(QString, how);
        KvmByHand kvm;
        KvmdConsoleLine::Pace pace = quick();
        pace.goodbyeWithinMs = 1200;
        if (how == u"letting go"_s) {
            // Room for a key going down, and none for the same key going up (a byte longer): the
            // line takes no more while it is let go of, and ends over that.
            pace.mostUnsentBytes = core::kvmd::keyWord(keyA, true).value_or(QByteArray()).size() + aroundAShortMessage;
        }
        auto line = std::make_unique<KvmdConsoleLine>(whereOf(kvm), pace);
        const QPointer<KvmdConsoleLine> there(line.get());
        const QSignalSpy opened(line.get(), &KvmdConsoleLine::opened);
        int told = 0;
        connect(line.get(), &KvmdConsoleLine::closed, this, [&told] { ++told; });
        QTRY_COMPARE(opened.count(), 1);
        line->key(keyA, true);
        QTRY_COMPARE(kvm.messages().size(), 1);
        if (how == u"before"_s) {
            kvm.fallOver();
            QTRY_COMPARE(told, 1);
        }
        const int toldBefore = told;
        QElapsedTimer since;
        since.start();
        // What it gives back is what the closing says: whether all that was down could be let go.
        QCOMPARE(KvmdConsoleLine::letGo(std::move(line)), how != u"letting go"_s);
        // Kept, and not thrown away in the same breath.
        QVERIFY(!there.isNull());
        if (how == u"ended"_s) {
            // What was down was let go, and goodbye said. While the KVM has not ended the line it
            // is kept; when it has, it is thrown away, long before its time is up.
            QTRY_COMPARE(kvm.messagesOf(goodbye).size(), 1);
            QCOMPARE(kvm.said(), (QList<QJsonObject>{keyOf(u"KeyA"_s, true), keyOf(u"KeyA"_s, false)}));
            QTest::qWait(150);
            QVERIFY(!there.isNull());
            kvm.endLine();
            QTRY_VERIFY(there.isNull());
            QVERIFY2(since.elapsed() < pace.goodbyeWithinMs - 300, QByteArray::number(since.elapsed()).constData());
        } else if (how == u"time"_s) {
            // The KVM keeps the line: so does the program, for the line's time and no longer.
            QTest::qWait(pace.goodbyeWithinMs - 400);
            QVERIFY(!there.isNull());
            QVERIFY(kvm.isOn());
            QTRY_VERIFY_WITH_TIMEOUT(there.isNull(), 4000);
            QVERIFY2(since.elapsed() >= pace.goodbyeWithinMs - 50, QByteArray::number(since.elapsed()).constData());
            QTRY_VERIFY(!kvm.isOn());
        } else {
            // There is nothing to wait for: thrown away a turn or two later.
            QTRY_VERIFY(there.isNull());
            QVERIFY2(since.elapsed() < pace.goodbyeWithinMs - 400, QByteArray::number(since.elapsed()).constData());
            QTRY_VERIFY(!kvm.isOn());
        }
        // Whoever held it was told nothing of what became of it.
        QTest::qWait(100);
        QCOMPARE(told, toldBefore);
    }

    void whoeverHeldALineThatWasLetGoIsToldLateWhereWhatWasSaidUpMayNotHaveArrived_data()
    {
        QTest::addColumn<QString>("how");
        QTest::addColumn<bool>("keyDown");
        QTest::addColumn<int>("times");
        QTest::newRow("the KVM answers the goodbye and ends the line") << u"answered"_s << true << 0;
        QTest::newRow("the KVM ends the line without its goodbye") << u"ended"_s << true << 1;
        QTest::newRow("the line falls when the goodbye has been said") << u"fell"_s << true << 1;
        QTest::newRow("the way has died, and the line is reset when its time is up") << u"time"_s << true << 1;
        QTest::newRow("the same, and nothing was down") << u"time"_s << false << 0;
    }

    void whoeverHeldALineThatWasLetGoIsToldLateWhereWhatWasSaidUpMayNotHaveArrived()
    {
        // A key is down, and the line is let go: the "up" is said and offered to Windows with
        // the goodbye, and the closing says that all was let go. Whether the KVM got it, only
        // its own goodbye says. A line that goes without that is one whose last words may have
        // gone with it: whoever held it is told then, late as that is.
        QFETCH(const QString, how);
        QFETCH(const bool, keyDown);
        QFETCH(const int, times);
        KvmByHand kvm;
        KvmdConsoleLine::Pace pace = quick();
        pace.goodbyeWithinMs = 700;
        auto line = std::make_unique<KvmdConsoleLine>(whereOf(kvm), pace);
        const QPointer<KvmdConsoleLine> there(line.get());
        const QSignalSpy opened(line.get(), &KvmdConsoleLine::opened);
        QTRY_COMPARE(opened.count(), 1);
        if (keyDown) {
            line->key(keyA, true);
            QTRY_COMPARE(kvm.messages().size(), 1);
        }
        if (how == u"time"_s) {
            // The way to the KVM dies: nothing is taken off the line there any more, and the
            // line is never ended from there. What is said now, Windows takes all the same.
            kvm.stopReading();
        }
        int told = 0;
        QElapsedTimer since;
        since.start();
        QVERIFY(KvmdConsoleLine::letGo(std::move(line), [&told] { ++told; }));
        // Never from inside the letting go.
        QCOMPARE(told, 0);
        if (how != u"time"_s) {
            QTRY_COMPARE(kvm.messagesOf(goodbye).size(), 1);
            QCOMPARE(told, 0);
        }
        if (how == u"answered"_s) {
            kvm.send(goodbye, QByteArray("\x03\xE8", 2));
            kvm.endLine();
        } else if (how == u"ended"_s) {
            kvm.endLine();
        } else if (how == u"fell"_s) {
            kvm.fallOver();
        }
        QTRY_VERIFY_WITH_TIMEOUT(there.isNull(), 4000);
        if (how == u"time"_s) {
            QVERIFY2(since.elapsed() >= pace.goodbyeWithinMs - 50, QByteArray::number(since.elapsed()).constData());
        }
        QTest::qWait(100);
        QCOMPARE(told, times);
    }

    void whatALineSaidLastArrivesAlsoWhereItIsThrownAwayOverSomethingOfTheKvmsThatWasNotRead_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("in the clear") << false;
        QTest::newRow("over TLS") << true;
    }

    void whatALineSaidLastArrivesAlsoWhereItIsThrownAwayOverSomethingOfTheKvmsThatWasNotRead()
    {
        // The KVM says something of its own accord, as kvmd does all the while, and the line is
        // thrown away before the program has got round to reading it. Windows resets a line that
        // is closed over what was not read from it, and the far end may then lose what it had not
        // taken yet: here, where the far end is Windows too, both "up" and the goodbye, every
        // time. (Not Qt's doing: this made-up KVM reads its line by hand.) So what had come is
        // taken off the line before it goes: in the clear, and over TLS, where what had come
        // lies on the line hidden and is taken off it as that.
        QFETCH(const bool, encrypted);
        const std::unique_ptr<KvmByHand> kvm = kvmOver(encrypted);
        auto console = std::make_unique<Console>(whereOf(*kvm));
        QTRY_COMPARE(console->opened.count(), 1);
        console->line.key(leftShift, true);
        console->line.button(2, true);
        QTRY_COMPARE(kvm->messages().size(), 2);
        kvm->send(text, iAmHere());
        kvm->pushOut();
        QThread::msleep(50);
        console.reset();
        QTRY_VERIFY(!kvm->isOn());
        const QList<QJsonObject> expected{keyOf(u"ShiftLeft"_s, true), buttonOf(u"right"_s, true),
                                          buttonOf(u"right"_s, false), keyOf(u"ShiftLeft"_s, false)};
        QCOMPARE(kvm->said(), expected);
        QCOMPARE(kvm->messagesOf(goodbye).size(), 1);
    }

    void itMayBeClosedFromInsideWhatItSays_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("in the clear") << false;
        QTest::newRow("over TLS") << true;
    }

    void itMayBeClosedFromInsideWhatItSays()
    {
        QFETCH(bool, encrypted);
        const std::unique_ptr<KvmByHand> kvm = kvmOver(encrypted);
        Console console(whereOf(*kvm));
        connect(&console.line, &KvmdConsoleLine::opened, &console.line, [&console] {
            console.line.key(keyA, true);
            console.line.close();
        });
        QTRY_COMPARE(console.opened.count(), 1);
        QTRY_VERIFY(!kvm->isOn());
        const QList<QJsonObject> expected{keyOf(u"KeyA"_s, true), keyOf(u"KeyA"_s, false)};
        QCOMPARE(kvm->said(), expected);
        QCOMPARE(kvm->messagesOf(goodbye).size(), 1);
        QCOMPARE(console.closed.count(), 0);
    }

    void itMayBeThrownAwayLaterFromInsideWhatItSays()
    {
        // Whoever hears that the line is gone lets go of it there and then, the way the house
        // does: a turn of the loop later.
        KvmByHand kvm;
        auto line = std::make_unique<KvmdConsoleLine>(whereOf(kvm), quick());
        const QPointer<KvmdConsoleLine> there(line.get());
        const QSignalSpy opened(line.get(), &KvmdConsoleLine::opened);
        int told = 0;
        connect(line.get(), &KvmdConsoleLine::closed, this, [&line, &told] {
            ++told;
            line.release()->deleteLater();
        });
        QTRY_COMPARE(opened.count(), 1);
        line->key(keyA, true);
        kvm.fallOver();
        QTRY_COMPARE(told, 1);
        QTRY_VERIFY(there.isNull());
        QCOMPARE(told, 1);
    }

    void theKvmIsAskedWhetherItIsThereEverySoOften()
    {
        KvmByHand kvm;
        KvmdConsoleLine::Pace pace = quick();
        pace.askEveryMs = 50;
        Console console(whereOf(kvm), pace);
        QTRY_COMPARE(console.opened.count(), 1);
        QTRY_VERIFY(kvm.messages().size() >= 3);
        const QList<QJsonObject> said = kvm.said();
        QVERIFY(said.size() >= 3);
        for (const QJsonObject& one : said) {
            QCOMPARE(one, areYouThereOf());
        }
        // Its answers are thrown away like everything else it says, and the line stays.
        kvm.send(text, R"({"event_type":"pong","event":{}})");
        QTest::qWait(100);
        QVERIFY(console.line.isOpen());
        // Not after the line was closed.
        console.line.close();
        QTRY_VERIFY(!kvm.isOn());
        const qsizetype asked = kvm.messagesOf(text).size();
        QTest::qWait(200);
        QCOMPARE(kvm.messagesOf(text).size(), asked);
    }

    void aKvmOfWhichNothingComesIsTakenToBeGone_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("in the clear") << false;
        QTest::newRow("over TLS") << true;
    }

    void aKvmOfWhichNothingComesIsTakenToBeGone()
    {
        // A way that stalls keeps the line and fails nothing: what is said to it is taken by
        // Windows, and played to the machine when the way comes back. So a line on which nothing
        // at all has come for too long is cut.
        QFETCH(bool, encrypted);
        const std::unique_ptr<KvmByHand> kvm = kvmOver(encrypted);
        KvmdConsoleLine::Pace pace = quick();
        pace.askEveryMs = 50;
        pace.silentAfterMs = 600;
        Console console(whereOf(*kvm), pace);
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(leftShift, true);
        // While it answers, the line stays: for far longer than it may be silent.
        QTimer answering;
        connect(&answering, &QTimer::timeout, kvm.get(), [&kvm] { kvm->send(text, iAmHere()); });
        answering.start(50);
        QTest::qWait(1500);
        QCOMPARE(console.closed.count(), 0);
        QVERIFY(console.line.isOpen());
        // It falls silent, and goes on taking what it is asked.
        answering.stop();
        QElapsedTimer silent;
        silent.start();
        QTRY_COMPARE_WITH_TIMEOUT(console.closed.count(), 1, 5000);
        // (Not sooner than it may be silent for: its last answer was at most one asking old.)
        QVERIFY2(silent.elapsed() >= 450, QByteArray::number(silent.elapsed()).constData());
        QCOMPARE(console.code(), u"timeout"_s);
        QCOMPARE(console.sentence(), u"Device did not respond in time"_s);
        // Shift is down on the machine, and cannot be let go from here: said so.
        QVERIFY(console.leftDown());
        QVERIFY(!console.line.isOpen());
        // Cut: no goodbye, no letting go, and nothing asked or said after it.
        QTRY_VERIFY(!kvm->isOn());
        QCOMPARE(kvm->messagesOf(goodbye).size(), 0);
        QVERIFY(!kvm->said().contains(keyOf(u"ShiftLeft"_s, false)));
        const qint64 heard = kvm->bytesHeard;
        console.line.key(keyB, true);
        console.line.releaseAll();
        console.line.close();
        QTest::qWait(200);
        QCOMPARE(kvm->bytesHeard, heard);
        QCOMPARE(console.closed.count(), 1);
    }

    void whatWasSaidUpSinceTheKvmWasLastHeardFromCountsAsLeftDownWhenTheLineEndsByItself_data()
    {
        QTest::addColumn<bool>("inTexts");
        QTest::addColumn<QString>("what");
        for (const bool inTexts : {false, true}) {
            const char* wording = inTexts ? "in texts" : "in bytes";
            QTest::addRow("a key let go while the way stalls (%s)", wording) << inTexts << u"key"_s;
            QTest::addRow("a button let go while the way stalls (%s)", wording) << inTexts << u"button"_s;
            QTest::addRow("everything let go while the way stalls (%s)", wording) << inTexts << u"all"_s;
            QTest::addRow("all let go, and the KVM heard from after that (%s)", wording) << inTexts << u"heard"_s;
        }
    }

    void whatWasSaidUpSinceTheKvmWasLastHeardFromCountsAsLeftDownWhenTheLineEndsByItself()
    {
        // A goes down, and the way stalls. A goes up: Windows takes that, so it is struck off
        // here. Then the watch cuts the line, and the "up" goes with the reset: the machine
        // still has A down, and whoever hears of the end is to hear that. (Here the made-up KVM
        // does get the "up": that it is thrown away shows only where the way itself stalls.)
        QFETCH(bool, inTexts);
        QFETCH(QString, what);
        KvmByHand kvm;
        KvmdConsoleLine::Where where = whereOf(kvm);
        where.inTexts = inTexts;
        KvmdConsoleLine::Pace pace = quick();
        pace.silentAfterMs = 500;
        Console console(where, pace);
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(keyA, true);
        console.line.button(1, true);
        QTRY_COMPARE(kvm.messages().size(), 2);
        // The KVM is there, and has said so after both went down.
        kvm.send(text, iAmHere());
        QTest::qWait(150);
        // From here on nothing comes from it.
        if (what == u"key"_s) {
            console.line.button(1, false);
            QTRY_COMPARE(kvm.messages().size(), 3);
            kvm.send(text, iAmHere());
            QTest::qWait(150);
            console.line.key(keyA, false);
        } else if (what == u"button"_s) {
            console.line.key(keyA, false);
            QTRY_COMPARE(kvm.messages().size(), 3);
            kvm.send(text, iAmHere());
            QTest::qWait(150);
            console.line.button(1, false);
        } else if (what == u"all"_s) {
            console.line.releaseAll();
        } else {
            console.line.key(keyA, false);
            console.line.button(1, false);
            QTRY_COMPARE(kvm.messages().size(), 4);
            kvm.send(text, iAmHere());
        }
        QTRY_COMPARE_WITH_TIMEOUT(console.closed.count(), 1, 5000);
        QCOMPARE(console.code(), u"timeout"_s);
        // Nothing is noted as down here in any of these: all of it was said up, and taken.
        // Only where the KVM was heard from after that is it taken to be up there.
        QCOMPARE(console.leftDown(), what != u"heard"_s);
    }

    void keysAndTheMouseAreToldInBytesUnlessTheKvmIsKnownToTakeOnlyTexts()
    {
        // What kvmd's own pages send, and all that a real KVM of one maker was seen to take.
        const auto hex = [](const char* written) { return QByteArray::fromHex(written); };
        constexpr int bytes = 2;
        KvmByHand kvm;
        KvmdConsoleLine::Where where = whereOf(kvm);
        where.inTexts = false;
        QVERIFY(!KvmdConsoleLine::Where{}.inTexts);
        KvmdConsoleLine::Pace pace = quick();
        pace.askEveryMs = 150;
        Console console(where, pace);
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(keyA, true);
        console.line.button(1, true);
        console.line.pointerAt(0, 32767);
        console.line.pointerBy(5, -3);
        console.line.wheel(0, -1);
        console.line.releaseAll();
        QTRY_COMPARE(kvm.messagesOf(bytes).size(), 7);
        const QList<KvmByHand::Message> said = kvm.messagesOf(bytes);
        for (const KvmByHand::Message& one : said) {
            QVERIFY(one.whole);
            QVERIFY(one.hidden);
            QVERIFY(!one.spare);
        }
        QCOMPARE(said[0].what, hex("0101") + "KeyA");
        QCOMPARE(said[1].what, hex("0201") + "left");
        QCOMPARE(said[2].what, hex("0380007fff"));
        QCOMPARE(said[3].what, hex("040005fd"));
        QCOMPARE(said[4].what, hex("050000ff"));
        // Everything let go: the button first, then the key.
        QCOMPARE(said[5].what, hex("0200") + "left");
        QCOMPARE(said[6].what, hex("0100") + "KeyA");
        // The asking whether the KVM is there is a text in either wording, as its page sends it;
        // and nothing of keys or the mouse went out as a text.
        QTRY_VERIFY(!kvm.messagesOf(text).isEmpty());
        for (const KvmByHand::Message& one : kvm.messagesOf(text)) {
            QCOMPARE(one.what, core::kvmd::pingWord());
        }
        // A key held across the opening is no press in bytes either (the same line, the other
        // wording): only what the line could say is said.
        QCOMPARE(kvm.messagesOf(bytes).size(), 7);
    }

    void aLineThatStallsIsEndedAndWhatWaitedIsNotPlayedLater_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("in the clear") << false;
        QTest::newRow("over TLS") << true;
    }

    void aLineThatStallsIsEndedAndWhatWaitedIsNotPlayedLater()
    {
        // The KVM, or the way to it, stops taking what is said and keeps the line. What is said
        // piles up, with Windows first and then in this program. What has waited here for longer
        // than it may ends the line, long before as much waits as may: and what waited, here and
        // with Windows, is thrown away, not played to the machine when the way comes back.
        QFETCH(bool, encrypted);
        const std::unique_ptr<KvmByHand> kvm = kvmOver(encrypted);
        KvmdConsoleLine::Pace pace = quick();
        pace.lateAfterMs = 100;
        pace.mostUnsentBytes = qint64{64} * 1024 * 1024; // never what ends this line
        Console console(whereOf(*kvm), pace);
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(keyA, true);
        QTRY_COMPARE(kvm->messages().size(), 1);
        kvm->stopReading();
        int said = 0;
        for (; said < 300'000 && console.closed.count() == 0; ++said) {
            console.line.pointerAt(said % 32768, 7);
            if (said % 50 == 49) {
                QCoreApplication::processEvents();
            }
        }
        QCOMPARE(console.closed.count(), 1);
        QCOMPARE(console.code(), u"timeout"_s);
        QCOMPARE(console.sentence(), u"The device did not take keyboard and mouse in time"_s);
        QVERIFY(console.leftDown());
        QVERIFY(!console.line.isOpen());
        // Thousands of messages were said, of which Windows had taken more than a thousand.
        QVERIFY2(said > 1000, QByteArray::number(said).constData());
        // The way comes back. What waited in this program never arrives, and no goodbye. Nor
        // does what Windows had taken and the KVM's program had not: the line was reset when it
        // ended. All that arrives is what the made-up KVM had taken before it stopped, the
        // kilobyte or two it keeps: some dozens of messages.
        // (This had it the other way round, and said so: the line was closed the ordinary way
        // then, and more than a thousand messages arrived after it had ended. On this PC the far
        // end is Windows too, and throws away with a line that is reset what its program has not
        // taken. A KVM's own system may not: what it has taken is its own, DeviceLine says.)
        kvm->readAgain();
        QTRY_VERIFY_WITH_TIMEOUT(!kvm->isOn(), 10'000);
        const QList<KvmByHand::Message> arrived = kvm->messages();
        QVERIFY2(arrived.size() < 100, QByteArray::number(arrived.size()).constData());
        QCOMPARE(kvm->messagesOf(goodbye).size(), 0);
    }

    void aLineThatIsTakenFromIsNotEndedHoweverMuchIsSaid()
    {
        // The limit is on how long what was said waits to leave, not on how much is said.
        KvmByHand kvm;
        KvmdConsoleLine::Pace pace = quick();
        pace.lateAfterMs = 100;
        Console console(whereOf(kvm), pace);
        QTRY_COMPARE(console.opened.count(), 1);
        QElapsedTimer going;
        going.start();
        int said = 0;
        while (going.elapsed() < 700) {
            for (int one = 0; one < 10; ++one, ++said) {
                console.line.pointerAt(said % 32768, 7);
            }
            QTest::qWait(1);
        }
        QCOMPARE(console.closed.count(), 0);
        QVERIFY(console.line.isOpen());
        QVERIFY(said > 100);
        QTRY_COMPARE(kvm.messages().size(), said);
    }

    void aKvmIsLetInByTheCertificateItIsKnownByAsTheDriverLetsItIn_data()
    {
        QTest::addColumn<QString>("shows");
        QTest::addColumn<bool>("vouchedFor");
        // As the driver's own tests have them (test_kvmd_device.cpp): what it takes, the line takes.
        QTest::newRow("one a KVM made for itself, wrongly dated and named for no place") << u"comet"_s << false;
        QTest::newRow("one that somebody signed whom Windows does not know, and who is not sent along")
            << u"twin-one"_s << false;
        QTest::newRow("one that Windows would trust anyway") << u"signed"_s << true;
    }

    void aKvmIsLetInByTheCertificateItIsKnownByAsTheDriverLetsItIn()
    {
        QFETCH(QString, shows);
        QFETCH(bool, vouchedFor);
        std::optional<VouchedFor> vouched;
        if (vouchedFor) {
            vouched.emplace(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        }
        KvmByHand kvm(tlsOf(shows.toLatin1().constData()));
        KvmdConsoleLine::Where where = whereOf(kvm);
        where.certificate = sha256Of(shows.toLatin1().constData());
        Console console(where);
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(keyB, true);
        QTRY_COMPARE(kvm.said().size(), 1);
        QCOMPARE(console.closed.count(), 0);
    }

    void aLineWhoseAskingWouldBeSplitIsNotAskedFor_data()
    {
        QTest::addColumn<QByteArray>("query");
        QTest::addColumn<QByteArray>("cookie");
        const QByteArray query = "stream=0";
        const QByteArray cookie = "auth_token=pass1";
        QTest::newRow("a line's end behind the question mark") << query + "\r\nX-Slipped-In: 1" << cookie;
        QTest::newRow("a line's end in the pass") << query << cookie + "\r\nX-Slipped-In: 1";
        QTest::newRow("a pass that is none") << query << "auth_token=pass 1"_ba;
    }

    void aLineWhoseAskingWouldBeSplitIsNotAskedFor()
    {
        QFETCH(QByteArray, query);
        QFETCH(QByteArray, cookie);
        KvmByHand kvm;
        KvmdConsoleLine::Where where = whereOf(kvm);
        where.query = query;
        where.cookie = cookie;
        Console console(where);
        console.line.key(keyA, true);
        QTest::qWait(200);
        QVERIFY2(!kvm.headers.contains("x-slipped-in"), "a header of somebody else's making went out with the asking");
        QCOMPARE(kvm.lines, 0);
        QCOMPARE(kvm.bytesHeard, 0);
        QCOMPARE(console.closed.count(), 1);
        QCOMPARE(console.code(), u"bad_response"_s);
        QCOMPARE(console.sentence(),
                 u"What was to be asked of the device for keyboard and mouse is not fit to be sent"_s);
        QVERIFY(!console.leftDown());
        QCOMPARE(console.opened.count(), 0);
    }

    void aKvmThatShowsAnotherCertificateHearsNothingAtAll_data()
    {
        QTest::addColumn<QString>("shows");
        QTest::addColumn<bool>("known");
        QTest::addColumn<QByteArray>("knownBy");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("sentence");
        const QString changed = u"Certificate does not match the one saved for this device"_s;
        QTest::newRow("another than it is known by, which Windows would trust")
            << u"signed"_s << true << sha256Of("first") << u"cert_mismatch"_s << changed;
        QTest::newRow("another than it is known by, which Windows does not trust either")
            << u"second"_s << true << sha256Of("first") << u"cert_mismatch"_s << changed;
        QTest::newRow("known by none, and one that Windows does not trust")
            << u"first"_s << false << QByteArray() << u"cert_invalid"_s << u"The device's certificate is not trusted"_s;
        QTest::newRow("its own, and what is saved as its certificate is none")
            << u"first"_s << true << sha256Of("first").left(20) << u"cert_invalid"_s
            << u"The certificate saved for this device cannot be read. Look at its certificate and trust it again."_s;
        QTest::newRow("its own, and nothing is saved as its certificate")
            << u"first"_s << true << QByteArray() << u"cert_invalid"_s
            << u"The certificate saved for this device cannot be read. Look at its certificate and trust it again."_s;
    }

    void aKvmThatShowsAnotherCertificateHearsNothingAtAll()
    {
        // The pass goes out with the asking for the line, and keys after it. So neither goes out
        // to whatever shows another certificate than the KVM is known by, also where Windows
        // would trust that certificate.
        QFETCH(QString, shows);
        QFETCH(bool, known);
        QFETCH(QByteArray, knownBy);
        QFETCH(QString, code);
        QFETCH(QString, sentence);
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        KvmByHand kvm(tlsOf(shows.toLatin1().constData()));
        KvmdConsoleLine::Where where = whereOf(kvm);
        where.knownByCertificate = known;
        where.certificate = knownBy;
        Console console(where);
        console.line.key(keyA, true);
        QTRY_COMPARE(console.closed.count(), 1);
        QCOMPARE(console.code(), code);
        QCOMPARE(console.sentence(), sentence);
        QVERIFY(!console.line.isOpen());
        console.line.key(keyB, true);
        console.line.close();
        QTest::qWait(300);
        // Not a byte of the asking, and so not the pass: nothing was written at all.
        QCOMPARE(kvm.bytesHeard, 0);
        QVERIFY(!kvm.wasAsked());
        QCOMPARE(console.opened.count(), 0);
        QCOMPARE(console.closed.count(), 1);
    }

    void aKvmThatTakesNothingOffTheLineIsNotSaidToWithoutEnd_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("in the clear") << false;
        QTest::newRow("over TLS") << true;
    }

    void aKvmThatTakesNothingOffTheLineIsNotSaidToWithoutEnd()
    {
        // It keeps the line and reads nothing: what is said to it piles up on this PC. Not
        // without end.
        QFETCH(bool, encrypted);
        const std::unique_ptr<KvmByHand> kvm = kvmOver(encrypted);
        KvmdConsoleLine::Pace pace = quick();
        pace.mostUnsentBytes = 65'536;
        // (How much may wait is what this is about, and not for how long: said this fast, it
        // would be a matter of this PC's hurry which of the two ends the line.)
        pace.lateAfterMs = 600'000;
        Console console(whereOf(*kvm), pace);
        QTRY_COMPARE(console.opened.count(), 1);
        console.line.key(keyA, true);
        QTRY_COMPARE(kvm->messages().size(), 1);
        kvm->stopReading();
        // Far more than Windows keeps for a line that nobody reads, and never as much in one
        // turn of the loop as may wait: what ends the line is the KVM, not the hand.
        int said = 0;
        for (; said < 400'000 && console.closed.count() == 0; ++said) {
            console.line.pointerAt(said % 32768, 5);
            if (said % 200 == 199) {
                QCoreApplication::processEvents();
            }
        }
        QCOMPARE(console.closed.count(), 1);
        QCOMPARE(console.code(), u"dropped"_s);
        QCOMPARE(console.sentence(), u"The device stopped taking keyboard and mouse"_s);
        QVERIFY(console.leftDown());
        QVERIFY(!console.line.isOpen());
        // It was said to for as long as something still took it. (Whether the line is cut the
        // made-up KVM cannot tell: it has stopped listening for that, too.)
        QVERIFY(qint64{said} * 60 > pace.mostUnsentBytes);
        // What was down cannot be let go on that line any more, and is not tried.
        console.line.releaseAll();
        console.line.close();
        QCOMPARE(console.closed.count(), 1);
    }

    void whyALineEndedIsSaidInTheDriversWords_data()
    {
        QTest::addColumn<QByteArray>("answer"); // to the asking for the line; none: the KVM lets it in
        QTest::addColumn<QByteArray>("then");   // what the KVM sends after that; none: it falls over
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("sentence");
        const QString notOpened = u"The device did not open a line for keyboard and mouse"_s;
        const QString notSignedIn = u"The device did not accept the session for keyboard and mouse"_s;
        QTest::newRow("it does not know the pass") << "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n"_ba
                                                   << QByteArray() << u"auth_failed"_s << notSignedIn;
        QTest::newRow("it forbids the pass") << "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"_ba << QByteArray()
                                             << u"auth_failed"_s << notSignedIn;
        QTest::newRow("it asks for a Windows sign-on")
            << "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: NTLM\r\nContent-Length: 0\r\n\r\n"_ba << QByteArray()
            << u"auth_failed"_s << notSignedIn;
        QTest::newRow("it answers as to any question")
            << "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"_ba << QByteArray() << u"bad_response"_s << notOpened;
        QTest::newRow("it sends the caller elsewhere")
            << "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/api/ws\r\nContent-Length: 0\r\n\r\n"_ba
            << QByteArray() << u"bad_response"_s << notOpened;
        QTest::newRow("it lets in with a mark it could not have made")
            << "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n"_ba
            << QByteArray() << u"bad_response"_s << notOpened;
        QTest::newRow("it lets in with no mark")
            << "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n"_ba
            << QByteArray() << u"bad_response"_s << notOpened;
        QTest::newRow("its answer has no end")
            << QByteArray(9000, 'x') << QByteArray() << u"bad_response"_s << notOpened;
        QTest::newRow("it falls over") << QByteArray() << QByteArray() << u"dropped"_s
                                       << u"The device closed the connection"_s;
        QTest::newRow("it says goodbye") << QByteArray() << QByteArray("\x88\x00", 2) << u"dropped"_s
                                         << u"The device closed the connection"_s;
        QTest::newRow("it sends what no KVM sends") << QByteArray() << QByteArray("\x83\x00", 2) << u"dropped"_s
                                                    << u"The device sent what this program does not accept"_s;
    }

    void whyALineEndedIsSaidInTheDriversWords()
    {
        QFETCH(QByteArray, answer);
        QFETCH(QByteArray, then);
        QFETCH(QString, code);
        QFETCH(QString, sentence);
        KvmByHand kvm;
        kvm.instead = answer;
        Console console(whereOf(kvm));
        if (answer.isEmpty()) {
            QTRY_COMPARE(console.opened.count(), 1);
            console.line.key(keyA, true);
            QTRY_COMPARE(kvm.messages().size(), 1);
            if (then.isEmpty()) {
                kvm.fallOver();
            } else {
                kvm.sendRaw(then);
            }
        }
        QTRY_COMPARE(console.closed.count(), 1);
        QCOMPARE(console.code(), code);
        QCOMPARE(console.sentence(), sentence);
        QVERIFY(!console.line.isOpen());
        QCOMPARE(console.opened.count(), answer.isEmpty() ? 1 : 0);
        // Where the line had stood, a key was down on the machine when it went: said, since
        // nothing from here can let go of it any more.
        QCOMPARE(console.leftDown(), answer.isEmpty());
        // Asked once, and not a second time whatever the answer was; nothing is said after the end.
        const qint64 heard = kvm.bytesHeard;
        console.line.key(keyB, true);
        console.line.releaseAll();
        console.line.close();
        QTest::qWait(300);
        QCOMPARE(kvm.lines, 1);
        QCOMPARE(kvm.bytesHeard, heard);
        QCOMPARE(console.closed.count(), 1);
        QVERIFY(!kvm.headers.contains("authorization"));
    }

    void aKvmThatIsNotThereOrDoesNotAnswerIsSaidSo()
    {
        {
            const NobodyThere nobody;
            KvmdConsoleLine::Where where;
            where.address = QUrl(nobody.address());
            where.cookie = "auth_token=pass1";
            // (Windows takes its time to say that nobody is there: said by then, or sooner.)
            KvmdConsoleLine::Pace pace = quick();
            pace.letInWithinMs = 1500;
            const Console console(where, pace);
            QTRY_COMPARE_WITH_TIMEOUT(console.closed.count(), 1, 8000);
            QCOMPARE(console.code(), u"unreachable"_s);
            QCOMPARE(console.sentence(), u"Device is unreachable"_s);
        }
        // It takes the asking for the line and never answers it.
        KvmByHand kvm;
        kvm.answers = false;
        KvmdConsoleLine::Pace pace = quick();
        pace.letInWithinMs = 400;
        const Console console(whereOf(kvm), pace);
        QTRY_VERIFY(kvm.wasAsked());
        QCOMPARE(console.closed.count(), 0);
        QTRY_COMPARE(console.closed.count(), 1);
        QCOMPARE(console.code(), u"timeout"_s);
        QCOMPARE(console.sentence(), u"Device did not respond in time"_s);
        QTRY_VERIFY(!kvm.isOn());
        QCOMPARE(console.opened.count(), 0);
    }
};

QTEST_GUILESS_MAIN(TestKvmdConsoleLine)

#include "test_kvmd_console_line.moc"
