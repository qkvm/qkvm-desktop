#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>

#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/kvmd_stream.h>

#include "made_up_kvm.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmdDevice;
using core::KvmdStream;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::jpeg;
using core::testing::MadeUpKvm;
using core::testing::refusal;
using core::testing::StandIn;

Q_DECLARE_METATYPE(qkvm::core::testing::Answered)

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

// Where a KVM has its stream of pictures.
QByteArray place()
{
    return "/streamer/stream"_ba;
}

// The word ustreamer puts between its pictures, and how it says so.
QByteArray word()
{
    return "boundarydonotcross"_ba;
}
QByteArray kind()
{
    return "multipart/x-mixed-replace;boundary=" + word();
}
QByteArray boundary()
{
    return "--" + word() + "\r\n";
}

// One picture as ustreamer sends it: its lines, the picture, and the boundary of the next.
QByteArray part(const QByteArray& picture)
{
    return "Content-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(picture.size()) +
           "\r\nX-Timestamp: 1728000000.123456\r\n\r\n" + picture + "\r\n" + boundary();
}

// How a KVM begins its stream: the first boundary, and what the test wants sent with it.
Answered streaming(const QByteArray& first = {})
{
    Answered answer(200, boundary() + first, kind());
    answer.streams = true;
    return answer;
}

// One piece of an answer that is sent in pieces, as the web server before kvmd's video sends it.
QByteArray chunk(const QByteArray& bytes)
{
    return QByteArray::number(bytes.size(), 16) + "\r\n" + bytes + "\r\n";
}

// A KVM that sends pictures as fast as its line takes them, in a thread of its own: the test's
// thread is then the only thing that is slow. It signs in whoever asks, and counts what it has
// handed to Windows to send.
class Flood : public QThread {
public:
    Flood(QByteArray piece, int pieces) : m_piece(std::move(piece)), m_pieces(pieces)
    {
        start();
        m_listening.acquire();
    }
    ~Flood() override
    {
        requestInterruption();
        wait();
    }
    Flood(const Flood&) = delete;
    Flood& operator=(const Flood&) = delete;
    Flood(Flood&&) = delete;
    Flood& operator=(Flood&&) = delete;

    QString address() const { return u"http://127.0.0.1:%1"_s.arg(m_port.load()); }
    qint64 sent() const { return m_sent.load(); }
    qint64 all() const { return m_piece.size() * m_pieces; }

protected:
    void run() override
    {
        QTcpServer server;
        server.listen(QHostAddress::LocalHost, 0);
        m_port.store(server.serverPort());
        m_listening.release();
        while (!isInterruptionRequested()) {
            if (!server.waitForNewConnection(50)) {
                continue;
            }
            QTcpSocket* line = server.nextPendingConnection();
            // The question, and what a sign-in carries behind it.
            QByteArray asked;
            const auto whole = [&asked] {
                const qsizetype end = asked.indexOf("\r\n\r\n");
                const qsizetype at = asked.toLower().indexOf("content-length:");
                const qsizetype stated = at < 0 ? 0 : asked.indexOf('\r', at) - at - 15;
                const qsizetype behind = asked.mid(at + 15, stated).trimmed().toLongLong();
                return end >= 0 && asked.size() >= end + 4 + behind;
            };
            while (!whole() && line->waitForReadyRead(2000)) {
                asked += line->readAll();
            }
            if (asked.startsWith("GET /streamer/stream ")) {
                line->write("HTTP/1.1 200 X\r\nContent-Type: " + kind() + "\r\nConnection: close\r\n\r\n" + boundary());
                for (int piece = 0; piece < m_pieces && pushed(line, m_piece); ++piece) {
                    m_sent.fetch_add(m_piece.size());
                }
                // The line is held until the test is over, or whoever asked has gone.
                while (!isInterruptionRequested() && line->state() == QAbstractSocket::ConnectedState) {
                    line->waitForReadyRead(50);
                }
                continue;
            }
            const QByteArray answer = asked.startsWith("POST /api/auth/login ")
                                          ? "{\"ok\": true, \"result\": {\"token\": \"pass1\"}}"_ba
                                          : QByteArray();
            pushed(line, "HTTP/1.1 " + (answer.isEmpty() ? "404"_ba : "200"_ba) +
                             " X\r\nContent-Type: application/json\r\nContent-Length: " +
                             QByteArray::number(answer.size()) + "\r\nConnection: close\r\n\r\n" + answer);
            line->disconnectFromHost();
            if (line->state() != QAbstractSocket::UnconnectedState) {
                line->waitForDisconnected(2000);
            }
        }
    }

private:
    // Hands the bytes to Windows, waiting for as long as its line takes none; false: the line
    // is gone, or the test is over.
    bool pushed(QTcpSocket* line, const QByteArray& bytes) const
    {
        line->write(bytes);
        while (line->bytesToWrite() > 0) {
            if (isInterruptionRequested() || line->state() != QAbstractSocket::ConnectedState) {
                return false;
            }
            line->waitForBytesWritten(50);
        }
        return true;
    }

    QByteArray m_piece;
    int m_pieces;
    QSemaphore m_listening;
    std::atomic<quint16> m_port{0};
    std::atomic<qint64> m_sent{0};
};

// What a stream said.
struct Heard {
    explicit Heard(const KvmdStream* stream)
        : pictures(stream, &KvmdStream::pictureCame), ended(stream, &KvmdStream::ended)
    {}

    QSignalSpy pictures;
    QSignalSpy ended;

    QByteArray picture(qsizetype index) const { return pictures.value(index).value(0).toByteArray(); }
    QString code() const { return ended.value(0).value(0).toString(); }
    QString message() const { return ended.value(0).value(1).toString(); }
};

} // namespace

// A KVM's stream of pictures on the driver's own line, against a made-up KVM that streams.
class TestKvmdStream : public QObject {
    Q_OBJECT

private:
    // The times a real KVM is given, made short.
    static KvmdDevice::Pace quick()
    {
        KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.holdMs = 400;
        pace.longestHoldMs = 1600;
        pace.refusedHoldMs = 1200;
        pace.signInRetryMs = 800;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        pace.restAfterMs = 900;
        pace.streamQuietMs = 3000;
        return pace;
    }
    // The same, with far longer to be let in: for a test of how a line that is there ends. Under
    // load, being let in has taken longer than quick() gives, and such a test then heard
    // "unreachable" of a KVM that was there, where it looks for "dropped".
    static KvmdDevice::Pace patient()
    {
        KvmdDevice::Pace pace = quick();
        pace.connectWithinMs = 10'000;
        return pace;
    }
    // The same as quick(), for a test in which a picture that came is to say that the video is
    // awake: what was seen of a KVM counts for far longer than a busy PC takes over the test.
    static KvmdDevice::Pace awakeForLong()
    {
        KvmdDevice::Pace pace = quick();
        pace.freshMs = 20'000;
        return pace;
    }

    // Has the KVM looked at, and says how it is once that look is over.
    static core::Status lookedAt(KvmdDevice& device)
    {
        QSignalSpy over(&device, &KvmdDevice::looked);
        device.look();
        if (!over.wait(20000)) {
            core::Status never;
            never.state = u"the look never ended"_s;
            return never;
        }
        return device.status();
    }
    // Asks for a picture the other way, lets the loop run until the answer is there, and gives it.
    static base::Result<core::Snapshot> pictureOf(KvmdDevice& device)
    {
        QList<base::Result<core::Snapshot>> answers;
        device.askSnapshot(400, [&answers](const base::Result<core::Snapshot>& answer) { answers.append(answer); });
        if (!QTest::qWaitFor([&answers] { return !answers.isEmpty(); }, 20000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return answers.constFirst();
    }
    static base::Result<core::Done> doneBy(KvmdDevice& device, const core::Action& what)
    {
        QList<base::Result<core::Done>> answers;
        device.act(what, [&answers](const base::Result<core::Done>& answer) { answers.append(answer); });
        if (!QTest::qWaitFor([&answers] { return !answers.isEmpty(); }, 20000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return answers.constFirst();
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

private slots:
    void initTestCase()
    {
        // The device chooses Windows' own TLS, which is refused once TLS is in use: so one is
        // made before anything else touches TLS (the stand-ins do).
        const KvmdDevice first(KvmdDevice::Where{}, QString());
    }

    void picturesArriveInTheOrderTheyWereSent()
    {
        MadeUpKvm kvm;
        const QByteArray first = jpeg(1920, 1080, 500);
        const QByteArray second = jpeg(1920, 1080, 90'000);
        const QByteArray third = jpeg(640, 480, 1);
        // The first of them in the same breath as the beginning of the answer.
        kvm.instead.insert(place(), streaming(part(first)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        // It says nothing before whoever asked for it holds it.
        QVERIFY(heard.pictures.isEmpty());
        QVERIFY(!stream->isOver());
        QTRY_COMPARE(heard.pictures.size(), 1);
        QVERIFY(kvm.stand->sendMore(part(second) + part(third)));
        QTRY_COMPARE(heard.pictures.size(), 3);
        QCOMPARE(heard.picture(0), first);
        QCOMPARE(heard.picture(1), second);
        QCOMPARE(heard.picture(2), third);
        QVERIFY(heard.ended.isEmpty());
        QVERIFY(!stream->isOver());

        // Signed in first, once, and asked as the driver asks everything.
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(kvm.asked(place()), 1);
        const Asked* asked = kvm.last(place());
        QCOMPARE(asked->method, "GET"_ba);
        QCOMPARE(asked->path, place());
        QCOMPARE(asked->headers.value("cookie"), "auth_token=pass1"_ba);
        QCOMPARE(asked->headers.value("accept-encoding"), "identity"_ba);
        QVERIFY(!asked->headers.contains("authorization"));
    }

    void aPartCutAcrossWritesIsOnePicture()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming());
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_VERIFY(kvm.stand->isStreaming());

        // In its lines, in the picture, in the line end behind it and in the boundary.
        const QByteArray small = jpeg(800, 600, 300);
        const QByteArray sent = part(small);
        const QList<qsizetype> cuts{1, 9, 30, 70, 100, 250, sent.size() - 23, sent.size() - 10, sent.size() - 1};
        qsizetype from = 0;
        for (const qsizetype cut : cuts) {
            QVERIFY(kvm.stand->sendMore(sent.mid(from, cut - from)));
            from = cut;
            QTest::qWait(20);
        }
        // (All of the picture has come by now, and it is handed on before its line end has.)
        QTRY_COMPARE(heard.pictures.size(), 1);
        QVERIFY(kvm.stand->sendMore(sent.mid(from)));
        QCOMPARE(heard.picture(0), small);

        // And a large one, a piece at a time.
        const QByteArray large = jpeg(1920, 1080, 700'000);
        const QByteArray whole = part(large);
        for (qsizetype at = 0; at < whole.size(); at += 50'000) {
            QVERIFY(kvm.stand->sendMore(whole.mid(at, 50'000)));
            QTest::qWait(5);
        }
        QTRY_COMPARE(heard.pictures.size(), 2);
        QCOMPARE(heard.picture(1), large);
        QVERIFY(heard.ended.isEmpty());
    }

    void itComesInPiecesAsTheKvmsWebServerSendsIt()
    {
        // kvmd's video is passed on by a web server that sends an answer without end in pieces
        // ("chunked"). Qt puts them together; where they are cut is nothing to the pictures.
        MadeUpKvm kvm;
        const QByteArray first = jpeg(1920, 1080, 2000);
        const QByteArray second = jpeg(1920, 1080, 3000);
        const QByteArray sent = boundary() + part(first) + part(second);
        Answered answer(200, chunk(sent.first(700)), kind());
        answer.streams = true;
        answer.more = {"Transfer-Encoding: chunked"_ba};
        kvm.instead.insert(place(), answer);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_VERIFY(kvm.stand->isStreaming());
        QVERIFY(kvm.stand->sendMore(chunk(sent.mid(700, 1900)) + chunk(sent.mid(2600, 1)) + chunk(sent.mid(2601))));
        QTRY_COMPARE(heard.pictures.size(), 2);
        QCOMPARE(heard.picture(0), first);
        QCOMPARE(heard.picture(1), second);
        // A line that falls in the middle of such an answer is a line that fell.
        kvm.stand->endStream();
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"dropped"_s);
        QCOMPARE(heard.message(), u"The device closed the connection"_s);
    }

    void overTlsItIsTheKvmThatIsKnownByItsCertificate()
    {
        MadeUpKvm kvm(tls("first"));
        const QByteArray picture = jpeg(1920, 1080, 4000);
        kvm.instead.insert(place(), streaming(part(picture)));
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        KvmdDevice device(where, u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.pictures.size(), 1);
        QCOMPARE(heard.picture(0), picture);
        QVERIFY(kvm.stand->sendMore(part(picture)));
        QTRY_COMPARE(heard.pictures.size(), 2);
        QVERIFY(heard.ended.isEmpty());
        QVERIFY(device.shownCertificate().isEmpty());
    }

    void whatShowsAnotherCertificateHearsNothing()
    {
        // Not the sign-in, and not the question for the stream.
        MadeUpKvm kvm(tls("second"));
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        KvmdDevice device(where, u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"cert_mismatch"_s);
        QCOMPARE(heard.message(), u"Certificate does not match the one saved for this device"_s);
        QVERIFY(stream->isOver());
        QVERIFY(heard.pictures.isEmpty());
        QCOMPARE(device.shownCertificate(), sha256Of("second"));
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvm.signIns, 0);
    }

    void theSessionGoesOnlyToTheKvmItself()
    {
        // The driver is signed in, so the question for the stream would carry its session. Then
        // something else answers at the KVM's address, with a certificate that Windows trusts:
        // it is not the one the KVM is known by, and the question does not go out.
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        auto kvm = std::make_unique<MadeUpKvm>(tls("first"));
        kvm->instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice::Where where = kvm->where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        KvmdDevice device(where, u"secret"_s, quick());
        {
            const std::unique_ptr<KvmdStream> stream = device.watch();
            const Heard heard(stream.get());
            QTRY_COMPARE(heard.pictures.size(), 1);
        }
        const quint16 port = kvm->stand->serverPort();
        kvm.reset();
        StandIn other([](const Asked&) { return streaming(part(jpeg(1920, 1080))); }, tls("signed"), port);
        QVERIFY(other.isListening());

        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"cert_mismatch"_s);
        QVERIFY(heard.pictures.isEmpty());
        QCOMPARE(device.shownCertificate(), sha256Of("signed"));
        // No question, no session; and nobody asked for the line that keeps its video awake.
        QTest::qWait(300);
        QVERIFY(other.asked.isEmpty());
        QCOMPARE(other.live.lines, 0);
    }

    void aSessionTheKvmForgotIsSignedInToAnewAndThePicturesCome_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("401") << 401;
        QTest::newRow("403") << 403;
    }

    void aSessionTheKvmForgotIsSignedInToAnewAndThePicturesCome()
    {
        QFETCH(int, status);
        MadeUpKvm kvm;
        const QByteArray picture = jpeg(1920, 1080, 1000);
        kvm.instead.insert(place(), streaming(part(picture)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        {
            const std::unique_ptr<KvmdStream> stream = device.watch();
            const Heard heard(stream.get());
            QTRY_COMPARE(heard.pictures.size(), 1);
        }
        QCOMPARE(kvm.signIns, 1);
        // kvmd was started anew: the session it gave is refused, once.
        kvm.first[place()].append(refusal(status, u"UnauthorizedError"_s));
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.pictures.size(), 1);
        QCOMPARE(heard.picture(0), picture);
        QVERIFY(heard.ended.isEmpty());
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.asked(place()), 3);
        QCOMPARE(kvm.last(place())->headers.value("cookie"), "auth_token=pass2"_ba);
    }

    void aKvmThatKeepsRefusingTheSessionIsAskedTwiceAndNoMore()
    {
        MadeUpKvm kvm;
        // (Asked to sign on in a way Qt would answer by itself, with the Windows account.)
        Answered refuses = refusal(401, u"UnauthorizedError"_s);
        refuses.more = {"WWW-Authenticate: NTLM"_ba};
        kvm.instead.insert(place(), refuses);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        // The stream's own failure, and not the session's: the KVM had only just given that one.
        // (This looked for "auth_failed" and "Device keeps rejecting the session" here: the words
        // with which the driver threw away a session that every other question was served under.)
        QCOMPARE(heard.code(), u"not_allowed"_s);
        QCOMPARE(heard.message(),
                 u"The device signed this program in and refuses its stream of pictures all the same"_s);
        QVERIFY(heard.pictures.isEmpty());
        // Signed in, refused, signed in once more, refused: and that is all.
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.asked(place()), 2);
        // Nor is the password sent again by whoever asks next, nor the KVM asked.
        const std::unique_ptr<KvmdStream> next = device.watch();
        const Heard again(next.get());
        QTRY_COMPARE(again.ended.size(), 1);
        QCOMPARE(again.code(), u"not_allowed"_s);
        QTest::qWait(200);
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.asked(place()), 2);
        for (const Asked& one : std::as_const(kvm.stand->asked)) {
            QVERIFY(!one.headers.contains("authorization"));
        }
        // After a while it is asked once more, under the session the driver has: no password goes
        // out for a stream that is refused under a session it was refused under before.
        QTest::qWait(quick().signInRetryMs);
        const std::unique_ptr<KvmdStream> later = device.watch();
        const Heard third(later.get());
        QTRY_COMPARE(third.ended.size(), 1);
        QCOMPARE(third.code(), u"not_allowed"_s);
        QCOMPARE(kvm.asked(place()), 3);
        QCOMPARE(kvm.last(place())->headers.value("cookie"), "auth_token=pass2"_ba);
        QCOMPARE(kvm.signIns, 2);
    }

    void aStreamThatIsRefusedCostsNoOtherQuestionItsSession_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("401") << 401;
        QTest::newRow("403") << 403;
    }

    void aStreamThatIsRefusedCostsNoOtherQuestionItsSession()
    {
        // A KVM that refuses its stream for another reason than the session: whoever is signed
        // in may not watch one, or what stands before the KVM guards that one place by itself.
        // The driver signed in anew, was refused again, and gave the session up for lost: every
        // look, picture and button then heard "auth_failed" until the next sign-in was due, of a
        // KVM that had refused none of them anything.
        QFETCH(int, status);
        MadeUpKvm kvm;
        kvm.atx = true;
        kvm.instead.insert(place(), refusal(status, u"ForbiddenError"_s));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(kvm.signIns, 1);
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        // (Signed in anew once, as for any question: the session might have run out.)
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.asked(place()), 2);
        // The session the driver has is left alone: the look, the picture and the button after
        // it go out under it, and are answered.
        const core::Status after = lookedAt(device);
        QCOMPARE(after.errorCode, QString());
        QCOMPARE(after.state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
        QVERIFY(doneBy(device, core::Action::plain(u"atx_power"_s)).ok());
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.last("/api/atx/click")->headers.value("cookie"), "auth_token=pass2"_ba);
        // And the stream ended as its own failure.
        QCOMPARE(heard.code(), u"not_allowed"_s);
        QCOMPARE(heard.message(),
                 u"The device signed this program in and refuses its stream of pictures all the same"_s);
    }

    void aSessionTheKvmForgotAndAPasswordItNoLongerTakesIsAsEver()
    {
        // The sign-in itself is refused: that is the password's, and is said as for every question.
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        kvm.first[place()].append(refusal(401, u"UnauthorizedError"_s));
        kvm.password = u"another one"_s;
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"auth_failed"_s);
        QCOMPARE(heard.message(), u"Login rejected - check the username and password"_s);
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.asked(place()), 1);
    }

    void withoutAPasswordNothingIsAsked()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice device(kvm.where(), QString(), quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"no_password"_s);
        QVERIFY(kvm.stand->asked.isEmpty());
    }

    void aRefusedPasswordEndsItAndTheStreamIsNotAsked()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice device(kvm.where(), u"not it"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"auth_failed"_s);
        QCOMPARE(heard.message(), u"Login rejected - check the username and password"_s);
        QCOMPARE(kvm.asked(place()), 0);
    }

    void anotherAnswerThanAStreamIsSaid_data()
    {
        // (Its row "not there", 404, is with the KVMs that have no such stream now, below.)
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("calledItself");
        // With what would be a stream behind it: the status decides.
        const QByteArray stream = boundary() + part(jpeg(1920, 1080));
        const QByteArray kvmds = refusal(500, u"ServerError"_s).body;
        QTest::newRow("an error of its own") << 500 << stream << kind();
        QTest::newRow("an error of its own, in kvmd's words") << 500 << kvmds << "application/json"_ba;
        QTest::newRow("nothing to send") << 204 << stream << kind();
        // What a gateway says of what is behind it, said in kvmd's own shape: by nobody known.
        QTest::newRow("a gateway's word in kvmd's shape") << 502 << kvmds << "application/json"_ba;
        QTest::newRow("a gateway's other word in kvmd's shape") << 504 << kvmds << "application/json"_ba;
    }

    void anotherAnswerThanAStreamIsSaid()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QByteArray, calledItself);
        MadeUpKvm kvm;
        kvm.instead.insert(place(), Answered(status, body, calledItself));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"bad_response"_s);
        QCOMPARE(heard.message(), u"Stream failed (HTTP %1)"_s.arg(status));
        QVERIFY(heard.pictures.isEmpty());
        QCOMPARE(kvm.asked(place()), 1);
    }

    void aKvmWithoutSuchAStreamSaysSo()
    {
        // The made-up KVM as it is: it has nothing at that place. (This looked for "bad_response"
        // and "Stream failed (HTTP 404)" here, which is what a KVM that is broken is told as.)
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"no_stream_here"_s);
        QCOMPARE(heard.message(), u"This device does not send its picture as a stream of JPEGs"_s);
    }

    void aKvmThatHasNoSuchStreamIsToldApartFromOneThatRestsAndOneThatIsBroken_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("calledItself");
        const QByteArray page =
            "<html>\r\n<head><title>502 Bad Gateway</title></head>\r\n<body>\r\n<center><h1>502 Bad "
            "Gateway</h1></center>\r\n</body>\r\n</html>\r\n"_ba;
        const QByteArray html = "text/html"_ba;
        const QByteArray json = "application/json"_ba;
        QTest::newRow("no such place, in kvmd's words") << 404 << refusal(404, u"NotFoundError"_s).body << json;
        QTest::newRow("no such place, on a page") << 404 << page << html;
        // As seen on a real KVM of one maker, also while its video was awake.
        QTest::newRow("a gateway's page: nothing behind it") << 502 << page << html;
        QTest::newRow("a gateway's page: not to be had") << 503 << page << html;
        QTest::newRow("a gateway's page: it gave up waiting") << 504 << page << html;
        QTest::newRow("a gateway with nothing to say") << 502 << QByteArray() << html;
        // (This was "resting" for 502 and for 503, whatever came with them: the test of a KVM
        // whose video rests had both rows, with these few words.)
        QTest::newRow("a gateway's few words") << 502 << "resting"_ba << "text/plain"_ba;
        QTest::newRow("a gateway's few words, 503") << 503 << "resting"_ba << "text/plain"_ba;
        QTest::newRow("JSON that is not kvmd's") << 503 << R"({"error": "upstream"})"_ba << json;
        QTest::newRow("what calls itself JSON and is a page") << 503 << page << json;
        // (Far more than kvmd says in a refusal: told before the last of it has come.)
        QTest::newRow("a gateway's page without end") << 502 << QByteArray(3'000'000, 'x') << html;
        QTest::newRow("kvmd's words with far more behind them")
            << 503 << refusal(503, u"UnavailableError"_s).body + QByteArray(3'000'000, ' ') << json;
    }

    void aKvmThatHasNoSuchStreamIsToldApartFromOneThatRestsAndOneThatIsBroken()
    {
        // One maker's KVM serves no stream of JPEGs at all: its live picture comes another way,
        // and what stands before it answers this place with 502 and a page, also while its video
        // is awake. That is not "its video rests" (it was woken and asked again for ever), and
        // not "it is broken": whoever asked takes its pictures another way, one at a time, and
        // does not ask for this again.
        // (This test had every gateway's page told so, whatever was known of the video: the
        // defect. A stock KVM's gateway answers the same while its video only rests, and was
        // then never asked for its stream again. So a picture comes first here: its video is
        // awake, and what a gateway's page says of one that may rest has tests of its own, below.)
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QByteArray, calledItself);
        MadeUpKvm kvm;
        kvm.instead.insert(place(), Answered(status, body, calledItself));
        KvmdDevice device(kvm.where(), u"secret"_s, awakeForLong());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"no_stream_here"_s);
        QCOMPARE(heard.message(), u"This device does not send its picture as a stream of JPEGs"_s);
        QVERIFY(heard.pictures.isEmpty());
        QVERIFY(stream->isOver());
        QCOMPARE(kvm.asked(place()), 1);
        QCOMPARE(kvm.signIns, 1);
        // The KVM is there, and is not left alone for it: its pictures are to be had the other way.
        QVERIFY(pictureOf(device).ok());
        QCOMPARE(kvm.signIns, 1);
    }

    void aGatewaysPageSaysThatTheVideoRestsUnlessAPictureHasJustCome_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        const QByteArray page =
            "<html>\r\n<head><title>502 Bad Gateway</title></head>\r\n<body>\r\n<center><h1>502 Bad "
            "Gateway</h1></center>\r\n</body>\r\n</html>\r\n"_ba;
        // What stands before kvmd on a stock KVM, while the program that makes the stream is not
        // running: which is whenever nobody watches.
        QTest::newRow("nothing behind it") << 502 << page;
        QTest::newRow("not to be had") << 503 << page;
        QTest::newRow("it gave up waiting") << 504 << page;
        QTest::newRow("nothing to say") << 502 << QByteArray();
        QTest::newRow("a page without end") << 502 << QByteArray(3'000'000, 'x');
    }

    void aGatewaysPageSaysThatTheVideoRestsUnlessAPictureHasJustCome()
    {
        // A gateway's page alone cannot tell a KVM that has no such stream from one whose video
        // rests; whether the video is awake can. Nothing has been seen of it here: it rests, is
        // woken, and whoever asked is told to ask again. Nothing is remembered against it.
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        MadeUpKvm kvm;
        kvm.instead.insert(place(), Answered(status, body, "text/html"_ba));
        KvmdDevice device(kvm.where(), u"secret"_s, awakeForLong());
        for (int time = 1; time <= 2; ++time) {
            const std::unique_ptr<KvmdStream> stream = device.watch();
            const Heard heard(stream.get());
            QTRY_COMPARE(heard.ended.size(), 1);
            QCOMPARE(heard.code(), u"no_stream"_s);
            QCOMPARE(heard.message(), u"Video stream is idle - waking it up"_s);
            QVERIFY(heard.pictures.isEmpty());
            QCOMPARE(kvm.asked(place()), time);
            // The line that wakes it is asked for, once.
            QTRY_VERIFY(kvm.stand->live.isConnected());
            QCOMPARE(kvm.stand->live.lines, 1);
        }
        // A picture of it comes: its video is awake. The same page then says that there is no
        // such stream here.
        QVERIFY(pictureOf(device).ok());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"no_stream_here"_s);
        QCOMPARE(kvm.asked(place()), 3);
        QCOMPARE(kvm.signIns, 1);
    }

    void aPictureThatCameAWhileAgoSaysNothingOfAVideoThatMayRestSince()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), Answered(502, "<html>502 Bad Gateway</html>"_ba, "text/html"_ba));
        KvmdDevice::Pace pace = quick();
        pace.freshMs = 150;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
        QTest::qWait(3 * pace.freshMs);
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"no_stream"_s);
        QCOMPARE(heard.message(), u"Video stream is idle - waking it up"_s);
    }

    void aRefusalWhoseWordsNeverComeWholeIsARefusalAllTheSame()
    {
        // What comes with a refusal is waited for, to see whose words it is: no longer than a
        // stream's first picture is. Then it is read as far as it came.
        // (This test was "... is given up on", and looked for "timeout": the word for a KVM that
        // did not answer, which this one had. It counted as away for that, and its looks and
        // pictures were held back.)
        MadeUpKvm kvm;
        Answered dripping(502, QByteArray(10'000, 'x'), "text/html"_ba);
        dripping.byteEveryMs = 100;
        kvm.instead.insert(place(), dripping);
        KvmdDevice::Pace pace = quick();
        pace.streamQuietMs = 600;
        // (Long enough that a KVM which counted as away would still be left alone below.)
        pace.holdMs = 5000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QElapsedTimer watch;
        watch.start();
        QTRY_COMPARE(heard.ended.size(), 1);
        // A gateway's few letters, of a video nothing was seen of: it rests.
        QCOMPARE(heard.code(), u"no_stream"_s);
        QCOMPARE(heard.message(), u"Video stream is idle - waking it up"_s);
        QVERIFY2(watch.elapsed() < qint64{4} * pace.streamQuietMs, qPrintable(QString::number(watch.elapsed())));
        QCOMPARE(kvm.asked(place()), 1);
        // It answered, so it is there: the look and the picture after it are asked at once, and
        // answered.
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
    }

    void whatComesWithARefusalHasItsTimeAlsoWhereTheRefusalItselfCameLate()
    {
        // kvmd's own word that the video rests, of a KVM whose video was just seen awake. Read
        // whole, it is kvmd's and says "it rests"; read before its words have come, it is taken
        // for a gateway's, which says "no stream here" of a video that is awake. The answer
        // begins late, and its words come a while after its beginning: later, from the question,
        // than a stream may be silent. They are waited for as long as a stream may be silent,
        // from the beginning of the answer. (That time ran from the question: an answer that
        // began late left its words none.)
        MadeUpKvm kvm;
        Answered late = refusal(503, u"UnavailableError"_s);
        late.afterMs = 500;
        late.bodyAfterMs = 450;
        kvm.instead.insert(place(), late);
        KvmdDevice::Pace pace = awakeForLong();
        pace.streamQuietMs = 750;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"no_stream"_s);
        QCOMPARE(heard.message(), u"Video stream is idle - waking it up"_s);
        QCOMPARE(kvm.asked(place()), 1);
    }

    void aRefusalThatComesPackedIsARefusalAllTheSame()
    {
        // Packed, what came with it is not unpacked and not read: it is somebody's refusal all
        // the same, and not a KVM that is away.
        MadeUpKvm kvm;
        Answered packed(502, "<html>502 Bad Gateway</html>"_ba, "text/html"_ba);
        packed.more = {"Content-Encoding: gzip"_ba};
        kvm.instead.insert(place(), packed);
        KvmdDevice::Pace pace = patient();
        pace.holdMs = 5000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"no_stream"_s);
        QCOMPARE(kvm.asked(place()), 1);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
    }

    void aRefusalThatBreaksOffIsReadAsFarAsItCame_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("calledItself");
        QTest::addColumn<QString>("code");
        // Its video is awake (a picture has just come), so the two read differently: kvmd's own
        // words say that the video rests, and a gateway's that there is no such stream.
        QTest::newRow("kvmd's words, all of them")
            << 503 << refusal(503, u"UnavailableError"_s).body << "application/json"_ba << u"no_stream"_s;
        QTest::newRow("a gateway's page")
            << 502 << "<html>502 Bad Gateway</html>"_ba << "text/html"_ba << u"no_stream_here"_s;
    }

    void aRefusalThatBreaksOffIsReadAsFarAsItCame()
    {
        // It says that more follows than does, and the line falls: a refusal that was begun is a
        // refusal, and no KVM that is away.
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QByteArray, calledItself);
        QFETCH(QString, code);
        MadeUpKvm kvm;
        Answered cut(status, body, calledItself);
        cut.shortBy = 40;
        kvm.instead.insert(place(), cut);
        KvmdDevice::Pace pace = awakeForLong();
        pace.connectWithinMs = 10'000;
        pace.holdMs = 5000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device).ok());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), code);
        QCOMPARE(kvm.asked(place()), 1);
        // And is asked for its next picture, which a KVM that counts as away is not.
        QTest::qWait(250); // (so that it is asked anew, and not handed the picture of a moment ago)
        const auto picture = pictureOf(device);
        QVERIFY2(picture.ok(), picture.ok() ? "" : picture.error().code.c_str());
        QCOMPARE(kvm.pictureAsks.size(), 2);
    }

    void aKvmThatSendsTheProgramElsewhereIsNotFollowed()
    {
        MadeUpKvm elsewhere;
        elsewhere.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        MadeUpKvm kvm;
        Answered moved(302, QByteArray());
        moved.more = {"Location: " + elsewhere.stand->address().toUtf8() + place()};
        kvm.instead.insert(place(), moved);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"bad_response"_s);
        QCOMPARE(heard.message(), u"Stream failed (HTTP 302)"_s);
        QTest::qWait(200);
        QVERIFY(elsewhere.stand->asked.isEmpty());
    }

    void whatIsNoStreamOfPicturesIsNotRead_data()
    {
        QTest::addColumn<QByteArray>("calledItself");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("sentence");
        // A yes with something of another kind than a stream of pictures: this KVM has none at
        // this place, and answers there with another thing (seen on one maker's real KVM while
        // its video was awake). Its pictures are to be had one at a time.
        // (These rows looked for "bad_response" here, which whoever has the KVM live takes for a
        // stream that broke and asks for again, for as long as the KVM is shown.)
        const QString none = u"no_stream_here"_s;
        const QString noStream = u"This device does not send its picture as a stream of JPEGs"_s;
        QTest::newRow("a picture") << "image/jpeg"_ba << none << noStream;
        QTest::newRow("a page") << "text/html; charset=utf-8"_ba << none << noStream;
        QTest::newRow("what kvmd answers with") << "application/json"_ba << none << noStream;
        QTest::newRow("several parts of another kind") << "multipart/mixed; boundary=" + word() << none << noStream;
        QTest::newRow("no kind at all") << QByteArray() << none << noStream;
        // What calls itself a stream of pictures and breaks the rules of one is a stream that
        // is not read here: that says nothing of whether the KVM has one.
        const QString broken = u"bad_response"_s;
        const QString notOne = u"The device sent something that is not a stream of pictures"_s;
        QTest::newRow("no word between the parts") << "multipart/x-mixed-replace"_ba << broken << notOne;
        QTest::newRow("a word that is none") << "multipart/x-mixed-replace; boundary=\"a\tb\""_ba << broken << notOne;
        QTest::newRow("the kind in capitals, and no word")
            << "Multipart/X-Mixed-Replace ; charset=utf-8"_ba << broken << notOne;
    }

    void whatIsNoStreamOfPicturesIsNotRead()
    {
        QFETCH(QByteArray, calledItself);
        QFETCH(QString, code);
        QFETCH(QString, sentence);
        MadeUpKvm kvm;
        Answered answer = streaming(part(jpeg(1920, 1080)));
        answer.type = calledItself;
        kvm.instead.insert(place(), answer);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), code);
        QCOMPARE(heard.message(), sentence);
        QVERIFY(heard.pictures.isEmpty());
        // And the line is given up: nothing more is taken of it.
        QTRY_VERIFY(!kvm.stand->isStreaming());
        QCOMPARE(kvm.asked(place()), 1);
    }

    void anAnswerThatIsPackedIsNotUnpacked()
    {
        MadeUpKvm kvm;
        Answered packed = streaming(part(jpeg(1920, 1080)));
        packed.more = {"Content-Encoding: gzip"_ba};
        kvm.instead.insert(place(), packed);
        KvmdDevice device(kvm.where(), u"secret"_s, patient());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"dropped"_s);
        QCOMPARE(heard.message(), u"The device packed its answer in a way this program does not unpack"_s);
        QVERIFY(heard.pictures.isEmpty());
    }

    void aStreamThatBreaksARuleIsEnded_data()
    {
        QTest::addColumn<QByteArray>("after"); // what follows a first picture that is right
        QTest::addColumn<int>("handedOn");     // the pictures that were whole before the rule was broken
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("message");
        const QString notRead = u"The device's stream is not one this program reads: "_s;
        const QByteArray picture = jpeg(1920, 1080);
        QTest::newRow("no length") << "Content-Type: image/jpeg\r\n\r\n" + picture + "\r\n" + boundary() << 1
                                   << u"bad_response"_s << notRead + u"it does not say how long a picture is"_s;
        QTest::newRow("a length that is none")
            << "Content-Length: -1\r\n\r\n" + picture + "\r\n" + boundary() << 1 << u"bad_response"_s
            << notRead + u"what it says a picture's length is, is none"_s;
        QTest::newRow("another word behind a picture")
            << "Content-Length: 4\r\n\r\nabcd\r\n--anotherword\r\n"_ba << 2 << u"bad_response"_s
            << notRead + u"its pictures are not set apart as it said they would be"_s;
        QTest::newRow("lines that end in LF alone")
            << "Content-Type: image/jpeg\nContent-Length: 4\n\nabcd\n"_ba << 1 << u"bad_response"_s
            << notRead + u"its lines do not end as lines do"_s;
        QTest::newRow("lines without end") << "X-More: " + QByteArray(6000, 'x') << 1 << u"bad_response"_s
                                           << notRead + u"it says far more of a picture than a KVM has reason to"_s;
        QTest::newRow("a picture larger than is taken")
            << "Content-Length: " + QByteArray::number(core::kvmd::largestPictureBytes + 1) + "\r\n\r\n" + picture << 1
            << u"dropped"_s << u"The device sent far more data than this program accepts"_s;
    }

    void aStreamThatBreaksARuleIsEnded()
    {
        QFETCH(QByteArray, after);
        QFETCH(int, handedOn);
        QFETCH(QString, code);
        QFETCH(QString, message);
        MadeUpKvm kvm;
        const QByteArray good = jpeg(1920, 1080, 800);
        kvm.instead.insert(place(), streaming(part(good)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.pictures.size(), 1);
        QVERIFY(kvm.stand->sendMore(after));
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), code);
        QCOMPARE(heard.message(), message);
        // What was whole before it had been handed on, and nothing after it is.
        QCOMPARE(heard.pictures.size(), handedOn);
        QCOMPARE(heard.picture(0), good);
        // The line is given up there and then: a KVM is not heard out.
        QTRY_VERIFY(!kvm.stand->isStreaming());
        // And the KVM does not count as away for it.
        const std::unique_ptr<KvmdStream> next = device.watch();
        const Heard again(next.get());
        QTRY_COMPARE(again.pictures.size(), 1);
    }

    void partsSetApartByAnotherWordAreNoPictures()
    {
        // By another word than the answer named: none of them is a picture here.
        MadeUpKvm kvm;
        Answered answer = streaming();
        answer.body = QByteArray();
        kvm.instead.insert(place(), answer);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_VERIFY(kvm.stand->isStreaming());
        const QByteArray picture = jpeg(1920, 1080, 1500);
        const QByteArray other =
            "--anotherword\r\nContent-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(picture.size()) +
            "\r\n\r\n" + picture + "\r\n";
        QVERIFY(kvm.stand->sendMore(other + other + other));
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"bad_response"_s);
        QCOMPARE(heard.message(),
                 u"The device's stream is not one this program reads: it does not begin with a picture"_s);
        QVERIFY(heard.pictures.isEmpty());
    }

    void theKvmSaysThatTheStreamIsOver()
    {
        MadeUpKvm kvm;
        const QByteArray last = jpeg(1920, 1080);
        // (The boundary behind the picture, with the two dashes that close.)
        QByteArray sent = part(last);
        sent.chop(2);
        kvm.instead.insert(place(), streaming(sent + "--\r\n"));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"dropped"_s);
        QCOMPARE(heard.message(), u"The device ended the stream"_s);
        QCOMPARE(heard.pictures.size(), 1);
        QCOMPARE(heard.picture(0), last);
        QTRY_VERIFY(!kvm.stand->isStreaming());
    }

    void silenceEndsIt()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming());
        KvmdDevice::Pace pace = quick();
        pace.streamQuietMs = 600;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_VERIFY(kvm.stand->isStreaming());
        // There is no limit on the whole of it: for far longer than nothing may come, pictures do.
        const QByteArray picture = jpeg(1920, 1080, 2000);
        QElapsedTimer watch;
        watch.start();
        int sent = 0;
        while (watch.elapsed() < qint64{4} * pace.streamQuietMs) {
            QVERIFY(kvm.stand->sendMore(part(picture)));
            ++sent;
            QTest::qWait(100);
        }
        QTRY_COMPARE(heard.pictures.size(), sent);
        QVERIFY(heard.ended.isEmpty());
        // Then nothing comes.
        watch.restart();
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"timeout"_s);
        QCOMPARE(heard.message(), u"Device did not respond in time"_s);
        QVERIFY2(watch.elapsed() >= pace.streamQuietMs - 250, qPrintable(QString::number(watch.elapsed())));
        QTRY_VERIFY(!kvm.stand->isStreaming());
    }

    void halfAPictureAndThenSilenceEndsItToo()
    {
        MadeUpKvm kvm;
        const QByteArray sent = part(jpeg(1920, 1080, 5000));
        kvm.instead.insert(place(), streaming(sent.first(3000)));
        KvmdDevice::Pace pace = quick();
        pace.streamQuietMs = 500;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"timeout"_s);
        QVERIFY(heard.pictures.isEmpty());
    }

    void aKvmThatTakesTheQuestionAndSaysNothingIsGivenUpOn()
    {
        MadeUpKvm kvm;
        Answered silent;
        silent.never = true;
        kvm.instead.insert(place(), silent);
        KvmdDevice::Pace pace = quick();
        pace.streamQuietMs = 500;
        pace.holdMs = 1500;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"timeout"_s);
        QCOMPARE(kvm.asked(place()), 1);
        // It did not answer, so it counts as away, as after every question: whoever asks at
        // once is told so, and the KVM is left alone.
        const std::unique_ptr<KvmdStream> next = device.watch();
        const Heard again(next.get());
        QElapsedTimer watch;
        watch.start();
        QTRY_COMPARE(again.ended.size(), 1);
        QCOMPARE(again.code(), u"timeout"_s);
        QVERIFY(watch.elapsed() < pace.streamQuietMs);
        QCOMPARE(kvm.asked(place()), 1);
    }

    void nobodyAtTheAddressIsUnreachable()
    {
        const core::testing::NobodyThere nobody;
        KvmdDevice::Where where;
        where.address = QUrl(nobody.address());
        KvmdDevice device(where, u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE_WITH_TIMEOUT(heard.ended.size(), 1, 15000);
        QCOMPARE(heard.code(), u"unreachable"_s);
        QCOMPARE(heard.message(), u"Device is unreachable"_s);
    }

    void aKvmThatHangsUpEndsIt()
    {
        MadeUpKvm kvm;
        const QByteArray picture = jpeg(1920, 1080, 3000);
        kvm.instead.insert(place(), streaming(part(picture)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        {
            // It falls over, in the middle of a picture.
            const std::unique_ptr<KvmdStream> stream = device.watch();
            const Heard heard(stream.get());
            QTRY_COMPARE(heard.pictures.size(), 1);
            QVERIFY(kvm.stand->sendMore(part(picture).first(1000)));
            QTest::qWait(50);
            kvm.stand->cutStream();
            QTRY_COMPARE(heard.ended.size(), 1);
            QCOMPARE(heard.code(), u"dropped"_s);
            QCOMPARE(heard.pictures.size(), 1);
            QVERIFY(stream->isOver());
        }
        {
            // It closes the line as a server that is done: the pictures it sent before that
            // arrive, and half a one is none.
            const std::unique_ptr<KvmdStream> stream = device.watch();
            const Heard heard(stream.get());
            QTRY_COMPARE(heard.pictures.size(), 1);
            QVERIFY(kvm.stand->sendMore(part(picture) + part(picture).first(1000)));
            kvm.stand->endStream();
            QTRY_COMPARE(heard.ended.size(), 1);
            QCOMPARE(heard.code(), u"dropped"_s);
            QCOMPARE(heard.message(), u"The device ended the stream"_s);
            QCOMPARE(heard.pictures.size(), 2);
        }
        // Its stream fell, the KVM answered: it is not left alone for that.
        const std::unique_ptr<KvmdStream> next = device.watch();
        const Heard again(next.get());
        QTRY_COMPARE(again.pictures.size(), 1);
    }

    void aKvmThatHangsUpOnTheQuestionEndsIt()
    {
        MadeUpKvm kvm;
        Answered gone;
        gone.hangUp = true;
        kvm.instead.insert(place(), gone);
        KvmdDevice device(kvm.where(), u"secret"_s, patient());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"dropped"_s);
        QCOMPARE(heard.message(), u"The device closed the connection"_s);
    }

    void closedItSaysNothingMore()
    {
        MadeUpKvm kvm;
        const QByteArray picture = jpeg(1920, 1080, 3000);
        kvm.instead.insert(place(), streaming(part(picture)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.pictures.size(), 1);
        // More is on its way when it is closed.
        QVERIFY(kvm.stand->sendMore(part(picture) + part(picture)));
        stream->close();
        QVERIFY(stream->isOver());
        // The line is given up, and nothing is said: no picture, and not that it ended.
        QTRY_VERIFY(!kvm.stand->isStreaming());
        QTest::qWait(300);
        QCOMPARE(heard.pictures.size(), 1);
        QVERIFY(heard.ended.isEmpty());
        stream->close(); // (a second time is nothing)
    }

    void closedBeforeItBeganNothingIsAsked()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        stream->close();
        QTest::qWait(400);
        QVERIFY(kvm.stand->asked.isEmpty());
        QVERIFY(heard.pictures.isEmpty());
        QVERIFY(heard.ended.isEmpty());

        // And one that is closed while it is being signed in asks for no stream after that.
        Answered late = core::testing::good({{u"token"_s, u"pass9"_s}});
        late.afterMs = 300;
        kvm.passes.insert("pass9"_ba);
        kvm.instead.insert("/api/auth/login"_ba, late);
        const std::unique_ptr<KvmdStream> next = device.watch();
        const Heard again(next.get());
        QTRY_COMPARE(kvm.stand->asked.size(), 2); // (the question asked first, and the sign-in, out)
        next->close();
        QTest::qWait(700);
        QCOMPARE(kvm.asked(place()), 0);
        QVERIFY(again.ended.isEmpty());
    }

    void whoeverIsHandedAPictureMayCloseItThere()
    {
        MadeUpKvm kvm;
        const QByteArray picture = jpeg(1920, 1080, 500);
        // Three pictures in one breath: the second and the third are in hand when the first is
        // handed on.
        kvm.instead.insert(place(), streaming(part(picture) + part(picture) + part(picture)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        {
            const std::unique_ptr<KvmdStream> stream = device.watch();
            const Heard heard(stream.get());
            connect(stream.get(), &KvmdStream::pictureCame, this, [&stream] { stream->close(); });
            QTRY_COMPARE(heard.pictures.size(), 1);
            QTest::qWait(300);
            QCOMPARE(heard.pictures.size(), 1);
            QVERIFY(heard.ended.isEmpty());
        }
        // Or let go of it.
        std::unique_ptr<KvmdStream> stream = device.watch();
        int came = 0;
        connect(stream.get(), &KvmdStream::pictureCame, this, [&stream, &came] {
            ++came;
            stream.reset();
        });
        QTRY_COMPARE(came, 1);
        QTest::qWait(300);
        QCOMPARE(came, 1);
        QTRY_VERIFY(!kvm.stand->isStreaming());
    }

    void whoeverHearsThatItEndedMayLetGoOfItThere()
    {
        MadeUpKvm kvm; // (it has no stream: the answer is its end)
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        std::unique_ptr<KvmdStream> stream = device.watch();
        int ends = 0;
        connect(stream.get(), &KvmdStream::ended, this, [&stream, &ends] {
            ++ends;
            stream.reset();
        });
        QTRY_COMPARE(ends, 1);
        QTest::qWait(200);
        QCOMPARE(ends, 1);
    }

    void aStreamOutlastsItsDeviceAndIsOverThen()
    {
        MadeUpKvm kvm;
        const QByteArray picture = jpeg(1920, 1080, 3000);
        kvm.instead.insert(place(), streaming(part(picture)));
        auto device = std::make_unique<KvmdDevice>(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device->watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.pictures.size(), 1);
        QVERIFY(kvm.stand->sendMore(part(picture) + part(picture)));
        device.reset();
        // Said once, a turn later, and nothing after it.
        QVERIFY(stream->isOver());
        QVERIFY(heard.ended.isEmpty());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"closed"_s);
        QCOMPARE(heard.message(), u"The connection to the device was closed"_s);
        // The line went with the device.
        QTRY_VERIFY(!kvm.stand->isStreaming());
        QTest::qWait(300);
        QCOMPARE(heard.pictures.size(), 1);
        QCOMPARE(heard.ended.size(), 1);
        stream->close(); // (touches nothing of what is gone)
    }

    void aDeviceThatGoesWhileItsStreamIsBeingOpenedEndsItToo()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        Answered late = core::testing::good({{u"token"_s, u"pass9"_s}});
        late.afterMs = 400;
        kvm.instead.insert("/api/auth/login"_ba, late);
        auto device = std::make_unique<KvmdDevice>(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device->watch();
        const Heard heard(stream.get());
        // One that had ended by itself before hears nothing of the device's end.
        MadeUpKvm without;
        auto other = std::make_unique<KvmdDevice>(without.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> over = other->watch();
        const Heard before(over.get());
        QTRY_COMPARE(before.ended.size(), 1);
        other.reset();

        QTRY_COMPARE(kvm.stand->asked.size(), 2); // (the question asked first, and the sign-in, out)
        device.reset();
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"closed"_s);
        QTest::qWait(700);
        QCOMPARE(kvm.asked(place()), 0);
        QCOMPARE(heard.ended.size(), 1);
        QCOMPARE(before.ended.size(), 1);
        // (What a KVM without such a stream is told as: "bad_response" before that had a word.)
        QCOMPARE(before.code(), u"no_stream_here"_s);
    }

    void aStreamThatIsClosedAsItsDeviceGoesSaysNothing()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        auto device = std::make_unique<KvmdDevice>(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device->watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.pictures.size(), 1);
        device.reset();
        stream->close();
        QTest::qWait(200);
        QVERIFY(heard.ended.isEmpty());

        // And a stream that goes before its device leaves the device as it was.
        KvmdDevice next(kvm.where(), u"secret"_s, quick());
        {
            const std::unique_ptr<KvmdStream> first = next.watch();
            const Heard one(first.get());
            QTRY_COMPARE(one.pictures.size(), 1);
        }
        const std::unique_ptr<KvmdStream> second = next.watch();
        const Heard two(second.get());
        QTRY_COMPARE(two.pictures.size(), 1);
    }

    void aKvmThatSendsFasterThanItIsReadIsHeldBackByTheLine()
    {
        // Sixty megabytes of pictures, sent as fast as the line takes them, to somebody who
        // takes a while over each. What the KVM has sent and nobody was handed yet is what waits
        // in this program and with Windows on the line: that stays small, and the rest of it
        // stays with the KVM. (Two megabytes when this was written; fifty-seven with the limit
        // on what waits to be read taken out.)
        const QByteArray picture = jpeg(1920, 1080, 250'000);
        Flood kvm(part(picture), 240);
        KvmdDevice::Where where;
        where.address = QUrl(kvm.address());
        KvmdDevice device(where, u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const qint64 each = part(picture).size();
        qint64 handed = 0;
        qint64 mostAhead = 0;
        qint64 mostHeld = 0;
        bool allTheSame = true;
        connect(stream.get(), &KvmdStream::pictureCame, this, [&](const QByteArray& came) {
            handed += each;
            mostAhead = std::max(mostAhead, kvm.sent() - handed);
            mostHeld = std::max(mostHeld, stream->held());
            allTheSame = allTheSame && came == picture;
            QThread::msleep(10);
            mostHeld = std::max(mostHeld, stream->held());
        });
        const Heard heard(stream.get());
        QTRY_VERIFY_WITH_TIMEOUT(heard.pictures.size() >= 100 || !heard.ended.isEmpty(), 60000);
        QVERIFY2(heard.ended.isEmpty(), qPrintable(heard.code()));
        QVERIFY(allTheSame);
        // Two things are shown, and they are not one. `mostAhead` counts what the made-up KVM
        // had handed to Windows and nobody here was handed yet: that is what waits in this
        // program, what Windows holds on the line at both ends, and what Qt holds on its own
        // thread, taken together, and it says that the KVM is held back. `mostHeld` is what this
        // program itself can count of it, asked of the stream while somebody is slow over a
        // picture: what has come and waits to be read, and the room made for the picture under
        // way. That stays under what the stream promises (KvmdStream::mostUnreadBytes), with one
        // picture's room beside it: 983 thousand bytes when this was written, of two megabytes
        // that were ahead. Not shown by either: how much Qt holds below what a reply says is
        // there to be read, which no call tells.
        QVERIFY2(mostAhead < qint64{8} * 1024 * 1024, qPrintable(QString::number(mostAhead)));
        QVERIFY2(mostHeld > 0 && mostHeld <= KvmdStream::mostUnreadBytes + each, qPrintable(QString::number(mostHeld)));
        QVERIFY(kvm.sent() < kvm.all());
    }

    void picturesByTheThousandAreNotHandedOn()
    {
        // Each picture is handed to somebody on the program's own thread. A KVM has reason to
        // send some dozens a second; one that sends them a byte long, by the thousand, is ended.
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming());
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_VERIFY(kvm.stand->isStreaming());
        // As many as a quick KVM sends in a second are taken, a second's worth in one breath.
        QByteArray many;
        for (int count = 0; count < 120; ++count) {
            many += part("x"_ba);
        }
        QVERIFY(kvm.stand->sendMore(many));
        QTRY_COMPARE(heard.pictures.size(), 120);
        QVERIFY(heard.ended.isEmpty());
        QTest::qWait(1100);
        for (int count = 0; count < 5000 - 120; ++count) {
            many += part("x"_ba);
        }
        QVERIFY(kvm.stand->sendMore(many));
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"bad_response"_s);
        QCOMPARE(heard.message(), u"The device sends more pictures than a KVM has reason to"_s);
        // (A second's worth, and another where the clock went on to the next second meanwhile.)
        QVERIFY2(heard.pictures.size() <= 120 + 2000, qPrintable(QString::number(heard.pictures.size())));
        QVERIFY(heard.pictures.size() >= 120 + 1000);
        QTRY_VERIFY(!kvm.stand->isStreaming());
    }

    void aCookieTheStreamHandsOutIsNotCarriedBack()
    {
        // ustreamer hands every caller a cookie with its stream. Qt's store of cookies is neither
        // written nor read: what the next question carries is the session, and nothing else.
        MadeUpKvm kvm;
        Answered answer = streaming(part(jpeg(1920, 1080)));
        answer.more = {"Set-Cookie: stream_client=abc/123; path=/; max-age=30"_ba,
                       "Set-Cookie: auth_token=another; path=/"_ba};
        kvm.instead.insert(place(), answer);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        for (int time = 0; time < 2; ++time) {
            const std::unique_ptr<KvmdStream> stream = device.watch();
            const Heard heard(stream.get());
            QTRY_COMPARE(heard.pictures.size(), 1);
            QCOMPARE(kvm.last(place())->headers.value("cookie"), "auth_token=pass1"_ba);
        }
        QCOMPARE(kvm.asked(place()), 2);
    }

    void whileAStreamIsOpenTheVideoIsKeptAwake()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 600;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QVERIFY(!device.isKeptAwake());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.pictures.size(), 1);
        // Somebody watches: the line that tells kvmd so is there, with the session.
        QTRY_VERIFY(kvm.stand->live.isConnected());
        QCOMPARE(kvm.stand->live.headers.value("cookie"), "auth_token=pass1"_ba);
        // And stays, for longer than a KVM is kept awake after a single picture.
        QTest::qWait(3 * pace.restAfterMs);
        QVERIFY(device.isKeptAwake());
        QVERIFY(kvm.stand->live.isConnected());
        QCOMPARE(kvm.stand->live.lines, 1);
        // Should it fall, it is asked for anew: kvmd would end the stream a little later.
        kvm.stand->live.fallOver();
        QTRY_COMPARE(kvm.stand->live.lines, 2);
        QVERIFY(heard.ended.isEmpty());
        // The stream closed, the KVM is let rest again.
        stream->close();
        QTRY_VERIFY(!device.isKeptAwake());
        QTRY_VERIFY(!kvm.stand->live.isConnected());
        QTest::qWait(400);
        QCOMPARE(kvm.stand->live.lines, 2);
    }

    void aSecondStreamOfOneKvmWhileOneIsOpenIsRefusedAtOnce()
    {
        // Qt keeps six lines to a place, and a stream holds one for as long as it is open. Six
        // streams of one KVM held them all: the driver's look then never left this PC and was
        // told as "unreachable", with six pictures playing, and a button waited out its limits
        // behind them and was told as "no answer, it may have been done". So one stream to a KVM
        // at a time. (This was "two streams of one KVM have a line each", and looked for the
        // second one's pictures.)
        MadeUpKvm kvm;
        const QByteArray picture = jpeg(1920, 1080, 2000);
        kvm.instead.insert(place(), streaming(part(picture)));
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 600;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const std::unique_ptr<KvmdStream> first = device.watch();
        const Heard one(first.get());
        // Also while the first is only being opened: nothing has been asked for it yet.
        const std::unique_ptr<KvmdStream> early = device.watch();
        const Heard hasty(early.get());
        QTRY_COMPARE(one.pictures.size(), 1);
        const qsizetype asked = kvm.stand->asked.size();
        const int lines = kvm.stand->lines;
        std::unique_ptr<KvmdStream> second = device.watch();
        const Heard other(second.get());
        // Refused a turn of the loop later, as everything is said: never from inside the asking.
        QVERIFY(other.ended.isEmpty());
        QTRY_COMPARE(other.ended.size(), 1);
        QCOMPARE(other.code(), u"busy"_s);
        QCOMPARE(other.message(), u"A stream of this device's pictures is open already. One is open at a time."_s);
        QVERIFY(second->isOver());
        QCOMPARE(hasty.ended.size(), 1);
        QCOMPARE(hasty.code(), u"busy"_s);
        // With nothing asked of the KVM for it, and on no line of its own.
        QTest::qWait(200);
        QCOMPARE(kvm.stand->asked.size(), asked);
        QCOMPARE(kvm.stand->lines, lines);
        QCOMPARE(kvm.asked(place()), 1);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(other.pictures.isEmpty() && hasty.pictures.isEmpty());
        // The ones that were refused take nothing from the first, also when they are let go of:
        // its pictures come on, and the KVM's video is kept awake for it.
        second.reset();
        early->close();
        QTest::qWait(2 * pace.restAfterMs);
        QVERIFY(kvm.stand->sendMore(part(picture)));
        QTRY_COMPARE(one.pictures.size(), 2);
        QVERIFY(one.ended.isEmpty());
        QVERIFY(device.isKeptAwake());
        QCOMPARE(kvm.stand->live.lines, 1);
        // Once the first is closed, the next that is asked for is opened.
        first->close();
        const std::unique_ptr<KvmdStream> third = device.watch();
        const Heard next(third.get());
        QTRY_COMPARE(next.pictures.size(), 1);
        QVERIFY(next.ended.isEmpty());
        QCOMPARE(kvm.asked(place()), 2);
        QCOMPARE(kvm.signIns, 1);
    }

    void aStreamThatIsRefusedOutlastsItsDeviceAndSaysSoAllTheSame()
    {
        // The device goes in the turn in which a second stream was asked of it: what was refused
        // knows of no device, and touches nothing of it.
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        auto device = std::make_unique<KvmdDevice>(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> first = device->watch();
        const std::unique_ptr<KvmdStream> second = device->watch();
        const Heard one(first.get());
        const Heard other(second.get());
        device.reset();
        QTRY_VERIFY(one.ended.size() == 1 && other.ended.size() == 1);
        QCOMPARE(one.code(), u"closed"_s);
        QCOMPARE(other.code(), u"busy"_s);
        QTest::qWait(200);
        QVERIFY(kvm.stand->asked.isEmpty());
        second->close();
    }

    void whenAStreamHasEndedTheVideoIsKeptAwakeAMomentLonger_data()
    {
        QTest::addColumn<bool>("byTheKvm");
        QTest::newRow("closed by whoever watched") << false;
        QTest::newRow("ended by the KVM") << true;
    }

    void whenAStreamHasEndedTheVideoIsKeptAwakeAMomentLonger()
    {
        // Whoever watched and is told to ask again does so, in a moment: a KVM's video is not let
        // rest at once when its stream has ended. The moment it was last wanted was noted where a
        // stream was asked for and nowhere after: when one that had been open for longer than the
        // rest ended, the line that keeps the video awake was let go at the timer's next look.
        QFETCH(bool, byTheKvm);
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 900;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.pictures.size(), 1);
        QTRY_VERIFY(kvm.stand->live.isConnected());
        QTest::qWait(2 * pace.restAfterMs);
        QVERIFY(device.isKeptAwake());
        if (byTheKvm) {
            kvm.stand->endStream();
            QTRY_COMPARE(heard.ended.size(), 1);
        } else {
            stream->close();
        }
        // (The timer looks every sixth of the rest: three looks by now.)
        QTest::qWait(pace.restAfterMs / 2);
        QVERIFY(device.isKeptAwake());
        QVERIFY(kvm.stand->live.isConnected());
        // And it is let rest once nobody has wanted its picture for that long.
        QTRY_VERIFY_WITH_TIMEOUT(!device.isKeptAwake(), 3 * pace.restAfterMs);
        QTRY_VERIFY(!kvm.stand->live.isConnected());
        QCOMPARE(kvm.stand->live.lines, 1);
    }

    void bytesThatMakeNoPictureEndItToo()
    {
        // A KVM that says a picture of thirty-two megabytes follows, and sends a byte of it now
        // and then, never ends and never gives a picture: it holds a line, and whoever watches
        // keeps the last picture up as if it were live. Such bytes are no silence, and it is
        // not the limit on silence that ends this: a picture has a time of its own to come
        // whole in, from its first byte, and that one does.
        // (This test was "... are silence too", and had the stream ended by the limit on
        // silence, which then stood for both: a picture that took longer than that to come
        // never arrived, however fast its bytes came. The next test has that one.)
        MadeUpKvm kvm;
        const QByteArray stated =
            "Content-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(core::kvmd::largestPictureBytes) +
            "\r\n\r\n";
        kvm.instead.insert(place(), streaming(stated));
        KvmdDevice::Pace pace = quick();
        pace.streamQuietMs = 400;
        pace.streamPictureMs = 1200;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const auto trickled = [&kvm, &pace](const Heard& heard) {
            // A byte far more often than nothing may come, for several times as long as a
            // picture may take.
            QElapsedTimer watch;
            watch.start();
            while (heard.ended.isEmpty() && watch.elapsed() < qint64{4} * pace.streamPictureMs) {
                kvm.stand->sendMore("x"_ba);
                QTest::qWait(50);
            }
            return watch.elapsed();
        };
        {
            const std::unique_ptr<KvmdStream> stream = device.watch();
            const Heard heard(stream.get());
            QTRY_VERIFY(kvm.stand->isStreaming());
            const qint64 took = trickled(heard);
            QCOMPARE(heard.ended.size(), 1);
            QCOMPARE(heard.code(), u"timeout"_s);
            QCOMPARE(heard.message(), u"Device did not respond in time"_s);
            QVERIFY2(took >= pace.streamPictureMs - 250 && took < qint64{2} * pace.streamPictureMs,
                     qPrintable(QString::number(took)));
            QVERIFY(heard.pictures.isEmpty());
            // The line is given up, and nothing is held for a picture that never came.
            QTRY_VERIFY(!kvm.stand->isStreaming());
            QCOMPARE(stream->held(), qint64{0});
        }
        // The same after pictures: the next has its own time, from its first byte.
        const QByteArray picture = jpeg(1920, 1080, 2000);
        kvm.instead.insert(place(), streaming(part(picture)));
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.pictures.size(), 1);
        QVERIFY(kvm.stand->sendMore(part(picture) + stated));
        const qint64 took = trickled(heard);
        QCOMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"timeout"_s);
        QVERIFY2(took >= pace.streamPictureMs - 250 && took < qint64{2} * pace.streamPictureMs,
                 qPrintable(QString::number(took)));
        QCOMPARE(heard.pictures.size(), 2);
        // The KVM answered all the while: it does not count as away for it.
        QCOMPARE(lookedAt(device).state, u"online"_s);
    }

    void aPictureThatTakesLongerToComeThanNothingMayComeForArrivesAllTheSame()
    {
        // A large picture over a slow way: its bytes come all the time, and it takes several
        // times as long to be whole as nothing may come for. That is no silence.
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming());
        KvmdDevice::Pace pace = quick();
        pace.streamQuietMs = 400;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_VERIFY(kvm.stand->isStreaming());
        const QByteArray picture = jpeg(1920, 1080, 60'000);
        const QByteArray sent = part(picture);
        const qsizetype piece = sent.size() / 15 + 1;
        QElapsedTimer watch;
        watch.start();
        for (qsizetype at = 0; at < sent.size(); at += piece) {
            QVERIFY2(heard.ended.isEmpty(), qPrintable(heard.code() + u" after "_s + QString::number(watch.elapsed())));
            QVERIFY(kvm.stand->sendMore(sent.mid(at, piece)));
            QTest::qWait(100);
        }
        QVERIFY(watch.elapsed() > qint64{3} * pace.streamQuietMs);
        QTRY_COMPARE(heard.pictures.size(), 1);
        QCOMPARE(heard.picture(0), picture);
        QVERIFY(heard.ended.isEmpty());
        // And the one after it. Then nothing comes at all: that is silence, as ever.
        QVERIFY(kvm.stand->sendMore(part(picture)));
        QTRY_COMPARE(heard.pictures.size(), 2);
        watch.restart();
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"timeout"_s);
        QCOMPARE(heard.message(), u"Device did not respond in time"_s);
        QVERIFY2(watch.elapsed() >= pace.streamQuietMs - 250, qPrintable(QString::number(watch.elapsed())));
    }

    void aKvmWhoseVideoRestsIsWokenAndWhoeverAskedIsToldToAskAgain_data()
    {
        QTest::addColumn<Answered>("resting");
        // As kvmd says it of a snapshot: 503, in its own shape. Seen at the stream's place of no
        // real KVM: there it is the gateway that answers while the video rests.
        QTest::newRow("kvmd's own word") << refusal(503, u"UnavailableError"_s);
        // As what stands before kvmd says it, on a stock KVM, while the program that makes the
        // stream is not running. (Once the test's second row, with the few words "resting"; then
        // taken for "no such stream", since one maker's KVM answers so always; back here, since
        // a page alone does not tell the two apart and nothing was seen of this video.)
        QTest::newRow("its gateway's page, nothing behind it")
            << Answered(502, "<html><head><title>502 Bad Gateway</title></head></html>"_ba, "text/html"_ba);
        QTest::newRow("its gateway's page, it gave up waiting")
            << Answered(504, "<html><head><title>504 Gateway Time-out</title></head></html>"_ba, "text/html"_ba);
    }

    void aKvmWhoseVideoRestsIsWokenAndWhoeverAskedIsToldToAskAgain()
    {
        QFETCH(Answered, resting);
        MadeUpKvm kvm;
        kvm.first[place()].append(resting);
        const QByteArray picture = jpeg(1920, 1080);
        kvm.instead.insert(place(), streaming(part(picture)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const std::unique_ptr<KvmdStream> stream = device.watch();
        const Heard heard(stream.get());
        QTRY_COMPARE(heard.ended.size(), 1);
        QCOMPARE(heard.code(), u"no_stream"_s);
        QCOMPARE(heard.message(), u"Video stream is idle - waking it up"_s);
        QTRY_VERIFY(kvm.stand->live.isConnected());
        // Asked again, the pictures come.
        const std::unique_ptr<KvmdStream> next = device.watch();
        const Heard again(next.get());
        QTRY_COMPARE(again.pictures.size(), 1);
        QCOMPARE(kvm.stand->live.lines, 1);
    }
};

QTEST_GUILESS_MAIN(TestKvmdStream)
#include "test_kvmd_stream.moc"
