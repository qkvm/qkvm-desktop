#include <QCryptographicHash>
#include <QFileInfo>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <qkvm/core/device_line.h>
#include <qkvm/core/video_diagnostics.h>

using namespace Qt::StringLiterals;
using qkvm::core::DeviceLine;

namespace {

// A device's line made by hand, for what one that speaks WebSocket properly cannot be made to do: it
// takes the asking, answers it with "you are in" and whatever a test puts beside that, and sends what a
// test gives it as WebSocket messages.
class HandMadeLine : public QObject {
public:
    HandMadeLine()
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* line = m_server.nextPendingConnection()) {
                m_line = line;
                connect(line, &QTcpSocket::readyRead, this, [this, line] { hear(line); });
            }
        });
    }

    // What stands beside "you are in" and its mark.
    QByteArray besides = "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    // The asking, as it came.
    QByteArray asking;

    QUrl address() const { return QUrl(u"http://127.0.0.1:%1"_s.arg(m_server.serverPort())); }

    // One whole message, of text (0x81) or of bytes (0x82), of up to 65535 bytes.
    void send(char kind, const QByteArray& message)
    {
        QByteArray frame;
        frame.append(kind);
        if (message.size() < 126) {
            frame.append(static_cast<char>(message.size()));
        } else {
            frame.append(static_cast<char>(126));
            frame.append(static_cast<char>((message.size() >> 8) & 0xFF));
            frame.append(static_cast<char>(message.size() & 0xFF));
        }
        if (m_line != nullptr) {
            m_line->write(frame + message);
        }
    }

    // What it sent that has not left for the line yet (Windows took the rest).
    qint64 waiting() const { return m_line != nullptr ? m_line->bytesToWrite() : 0; }

    // Bytes as they are, whatever they say of themselves.
    void sendRaw(const QByteArray& bytes)
    {
        if (m_line != nullptr) {
            m_line->write(bytes);
        }
    }

    // Takes nothing more off the line (what Windows keeps for it fills, and then what is said waits in the line's
    // program), until it reads again.
    void stopReading()
    {
        m_reads = false;
        if (m_line != nullptr) {
            m_line->setReadBufferSize(1024);
            m_line->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        }
    }
    void readAgain()
    {
        m_reads = true;
        if (m_line != nullptr) {
            m_line->setReadBufferSize(0);
            m_line->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1024 * 1024);
            hear(m_line);
        }
    }

private:
    void hear(QTcpSocket* line)
    {
        if (!m_reads) {
            return;
        }
        asking += line->readAll();
        if (m_answered || !asking.contains("\r\n\r\n")) {
            return;
        }
        m_answered = true;
        const qsizetype at = asking.toLower().indexOf("sec-websocket-key:");
        const QByteArray key = asking.mid(at + 18, asking.indexOf("\r\n", at) - at - 18).trimmed();
        const QByteArray mark =
            QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
        line->write("HTTP/1.1 101 Switching Protocols\r\n" + besides + "Sec-WebSocket-Accept: " + mark + "\r\n\r\n");
    }

    QTcpServer m_server;
    QPointer<QTcpSocket> m_line;
    bool m_answered = false;
    bool m_reads = true;
};

// A JetKVM's signalling line, as its driver asks for it.
DeviceLine::Where signalling(const QUrl& address)
{
    DeviceLine::Where where;
    where.address = address;
    where.asking = "/webrtc/signaling/client"_ba;
    where.cookie = "authToken=a.pass-1"_ba;
    return where;
}

DeviceLine::End endOf(const QSignalSpy& ended)
{
    return ended.first().first().value<DeviceLine::End>();
}

} // namespace

class TestDeviceLine : public QObject {
    Q_OBJECT

private slots:
    void aLineThatReadsTextWithoutASubprotocolHearsWhatTheDeviceSays()
    {
        HandMadeLine device;
        DeviceLine line(signalling(device.address()), {});
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy text(&line, &DeviceLine::textCame);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        // (It asked for none.)
        QVERIFY(!device.asking.toLower().contains("sec-websocket-protocol"));
        QVERIFY(device.asking.contains("\r\nCookie: authToken=a.pass-1\r\n"));
        device.send('\x81', R"({"type":"device-metadata"})"_ba);
        QTRY_COMPARE(text.size(), 1);
        QCOMPARE(text.first().first().toByteArray(), R"({"type":"device-metadata"})"_ba);
    }

    void aLineThatReadsNoTextThrowsItAway()
    {
        HandMadeLine device;
        DeviceLine line(signalling(device.address()), {});
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy text(&line, &DeviceLine::textCame);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send('\x81', "said"_ba);
        QTest::qWait(200);
        QVERIFY(text.isEmpty());
        QVERIFY(line.isIn());
    }

    void whatNobodyAskedForEndsALineThatReadsTextWithoutASubprotocol_data()
    {
        QTest::addColumn<QByteArray>("besides");
        QTest::newRow("a subprotocol")
            << "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Protocol: janus-protocol\r\n"_ba;
        QTest::newRow("an extension")
            << "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Extensions: permessage-deflate\r\n"_ba;
        QTest::newRow("no word that it upgrades") << "Connection: Upgrade\r\n"_ba;
    }

    void whatNobodyAskedForEndsALineThatReadsTextWithoutASubprotocol()
    {
        QFETCH(QByteArray, besides);
        HandMadeLine device;
        device.besides = besides;
        DeviceLine line(signalling(device.address()), {});
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::NotLetIn);
        QVERIFY(in.isEmpty());
    }

    void bytesOrTooMuchTextEndALineThatReadsText_data()
    {
        QTest::addColumn<char>("kind");
        QTest::addColumn<QByteArray>("message");
        QTest::newRow("bytes") << '\x82' << "\x01\x02"_ba;
        QTest::newRow("a text longer than may be") << '\x81' << QByteArray(301, 'a');
    }

    void bytesOrTooMuchTextEndALineThatReadsText()
    {
        QFETCH(char, kind);
        QFETCH(QByteArray, message);
        HandMadeLine device;
        // (Room for the answer that lets it in, which is read through the same buffer.)
        DeviceLine::Limits limits;
        limits.largestFrameBytes = 300;
        DeviceLine line(signalling(device.address()), limits);
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send(kind, message);
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void aTextInPartsIsHeardWholeAndHeldToTheLimitWhole_data()
    {
        QTest::addColumn<int>("first");
        QTest::addColumn<int>("second");
        QTest::addColumn<bool>("heard");
        // (Each part within the limit of 300: only the whole of the two is over it in the second row.)
        QTest::newRow("within the limit") << 150 << 100 << true;
        QTest::newRow("over the limit together") << 200 << 200 << false;
    }

    void aTextInPartsIsHeardWholeAndHeldToTheLimitWhole()
    {
        QFETCH(int, first);
        QFETCH(int, second);
        QFETCH(bool, heard);
        HandMadeLine device;
        DeviceLine::Limits limits;
        limits.largestFrameBytes = 300;
        DeviceLine line(signalling(device.address()), limits);
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy text(&line, &DeviceLine::textCame);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send('\x01', QByteArray(first, 'a'));  // text, more to come
        device.send('\x80', QByteArray(second, 'b')); // the rest of it
        if (heard) {
            QTRY_COMPARE(text.size(), 1);
            QCOMPARE(text.first().first().toByteArray(), QByteArray(first, 'a') + QByteArray(second, 'b'));
            QVERIFY(ended.isEmpty());
        } else {
            QTRY_COMPARE(ended.size(), 1);
            QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
            QVERIFY(text.isEmpty());
        }
    }

    void aLimitSmallerThanTheAnswerThatLetsItInStillLetsItIn()
    {
        // (What may wait to be read is never less than that answer, whatever a message may be.)
        HandMadeLine device;
        DeviceLine::Limits limits;
        limits.largestFrameBytes = 32;
        DeviceLine line(signalling(device.address()), limits);
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send('\x81', QByteArray(33, 'a'));
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void aLineThatReadsTextIsNoServersLiveLine()
    {
        // Asked to be one after it was asked to read text, it stays what it was: bytes end it.
        HandMadeLine device;
        DeviceLine line(signalling(device.address()), {});
        line.receiveText();
        line.receiveLive();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        QVERIFY(device.asking.contains("\r\nCookie: authToken=a.pass-1\r\n"));
        device.send('\x82', "\x01\x02"_ba);
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void aLineAskedForWithoutACookieCarriesNone()
    {
        HandMadeLine device;
        DeviceLine::Where where = signalling(device.address());
        where.cookie.clear();
        where.withoutCookie = true;
        DeviceLine line(where, {});
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        QVERIFY(!device.asking.toLower().contains("cookie"));
        QVERIFY(!device.asking.toLower().contains("x-qkvm"));
    }

    void aLineIsAskedForWithACookieOrExpresslyWithoutOne_data()
    {
        QTest::addColumn<QByteArray>("cookie");
        QTest::addColumn<bool>("withoutCookie");
        QTest::addColumn<QByteArray>("asking");
        const QByteArray line = "/webrtc/signaling/client"_ba;
        QTest::newRow("none, and not said to be without") << QByteArray() << false << line;
        QTest::newRow("one, though said to be without") << "authToken=a.pass-1"_ba << true << line;
        QTest::newRow("none, as said, with an asking that splits")
            << QByteArray() << true << "/webrtc/signaling/client\r\nX-More: 1"_ba;
        QTest::newRow("none, as said, with an asking that is no path") << QByteArray() << true << "webrtc"_ba;
    }

    void aLineIsAskedForWithACookieOrExpresslyWithoutOne()
    {
        QFETCH(QByteArray, cookie);
        QFETCH(bool, withoutCookie);
        QFETCH(QByteArray, asking);
        HandMadeLine device;
        DeviceLine::Where where = signalling(device.address());
        where.cookie = cookie;
        where.withoutCookie = withoutCookie;
        where.asking = asking;
        DeviceLine line(where, {});
        line.receiveText();
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::NotAsked);
        QTest::qWait(100);
        QVERIFY(device.asking.isEmpty());
    }

    // --- a line that reads bytes: a management controller's console (AMI's, at /kvm) ----------------------------

    void aLineThatReadsBytesHearsThemWholeAndSaysWhatItIsAndWhereFrom()
    {
        HandMadeLine device;
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Where where = signalling(device.address());
        where.asking = "/kvm"_ba;
        where.cookie = "QSESSIONID=a1b2c3"_ba;
        DeviceLine line(where, {});
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy bytes(&line, &DeviceLine::bytesCame);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        QVERIFY(device.asking.contains("\r\nSec-WebSocket-Protocol: binary\r\n"));
        QVERIFY(device.asking.contains("\r\nOrigin: " + device.address().toString().toLatin1() + "\r\n"));
        QVERIFY(device.asking.contains("\r\nCookie: QSESSIONID=a1b2c3\r\n"));
        device.send('\x82', "\x17\x00\x00\x00\x00\x00\x02\x00"_ba);
        QTRY_COMPARE(bytes.size(), 1);
        QCOMPARE(bytes.first().first().toByteArray(), "\x17\x00\x00\x00\x00\x00\x02\x00"_ba);
        // ...and one in parts, whole.
        device.send('\x02', QByteArray(300, 'a'));
        device.send('\x00', QByteArray(300, 'b'));
        device.send('\x80', "end"_ba);
        QTRY_COMPARE(bytes.size(), 2);
        QCOMPARE(bytes.last().first().toByteArray(), QByteArray(300, 'a') + QByteArray(300, 'b') + "end"_ba);
        QVERIFY(ended.isEmpty());
    }

    void aLineHeldBackTakesNothingMoreUntilItIsLetGo()
    {
        // (Review 3's HIGH 1, 2026-11-06: a server's console whose frames wait to be read on a thread of their own is
        // held back, as it was while the interface thread read them.) Held back from inside what it hands out, a line
        // hands out nothing more: what comes waits, in here no more than the line takes of one message, the rest with
        // Windows and then with whoever sends it. Let go, all of it comes in its order, a turn of the loop later.
        HandMadeLine device;
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Limits limits;
        limits.mostBytesASecond = qint64{64} * 1024 * 1024; // (as much as it may send: a console's)
        limits.mostMessagesASecond = 20000;
        DeviceLine line(signalling(device.address()), limits);
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        QList<QByteArray> came;
        connect(&line, &DeviceLine::bytesCame, this, [&came, &line](const QByteArray& bytes) {
            came.append(bytes.first(std::min<qsizetype>(bytes.size(), 8)));
            if (came.size() == 1) {
                line.holdBack(true);
            }
        });
        line.open();
        QTRY_COMPARE(in.size(), 1);
        constexpr int messages = 512; // (of 64000 bytes each: 32 MB)
        for (int one = 0; one < messages; ++one) {
            device.send('\x82', QByteArray::number(one).rightJustified(8, '0') + QByteArray(64000 - 8, 'x'));
        }
        QTRY_COMPARE(came.size(), 1);
        QTest::qWait(400);
        QCOMPARE(came.size(), 1);
        QVERIFY2(device.waiting() > qint64{16} * 1024 * 1024, QByteArray::number(device.waiting()));
        line.holdBack(false);
        QCOMPARE(came.size(), 1); // (not from inside)
        QTRY_COMPARE_WITH_TIMEOUT(came.size(), messages, 20000);
        for (int one = 0; one < messages; ++one) {
            QCOMPARE(came.at(one), QByteArray::number(one).rightJustified(8, '0'));
        }
        QVERIFY(ended.isEmpty());
    }

    void aLineSaysHowLongWhatWasSaidHasWaitedToLeave()
    {
        // (For a server's console, where keys are not said behind what has waited too long, and the line goes on.)
        // What Windows takes of a line leaves at once as far as the line knows; what it does not take waits in the
        // line's program, and how long the oldest of it has waited is said.
        HandMadeLine device;
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Where where = signalling(device.address());
        where.asking = "/kvm"_ba;
        where.cookie = "QSESSIONID=a1b2c3"_ba;
        DeviceLine::Limits limits;
        limits.mostUnsentBytes = qint64{64} * 1024 * 1024;
        limits.lateAfterMs = 600'000;
        DeviceLine line(where, limits);
        line.receiveBytes("binary"_ba);
        line.open();
        QTRY_VERIFY(line.isIn());
        QCOMPARE(line.waitedMs(), qint64{0});
        QVERIFY(line.sayBytes("x"_ba));
        QCOMPARE(line.waitedMs(), qint64{0});
        device.stopReading();
        // Two megabytes: far more than Windows keeps for a line that nobody reads.
        const QByteArray words(1000, 'k');
        for (int one = 0; one < 2000; ++one) {
            QVERIFY(line.sayBytes(words));
        }
        QTest::qWait(300);
        QVERIFY2(line.waitedMs() >= 250, QByteArray::number(line.waitedMs()).constData());
        QVERIFY(line.isIn());
        // A way that comes back takes it, and nothing waits any more.
        device.readAgain();
        QTRY_COMPARE_WITH_TIMEOUT(line.waitedMs(), qint64{0}, 10000);
        QVERIFY(line.isIn());
    }

    void aLineThatReadsBytesEndsAtAText()
    {
        HandMadeLine device;
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine line(signalling(device.address()), {});
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send('\x81', "AAAA"_ba);
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void aLineThatReadsBytesIsNotLetInWithoutItsProtocol()
    {
        HandMadeLine device;
        DeviceLine line(signalling(device.address()), {});
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::NotLetIn);
        QVERIFY(in.isEmpty());
    }

    void aConsolesLineIsNoSignallingInTheVideoDiagnostics()
    {
        // (A review of the desktop's.) The video's diagnostics are the signalling's: a console's line that is not let
        // in, or ends, writes nothing there.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        {
            qkvm::core::VideoDiagnostics log(path);
            QVERIFY(log.isOpen());
            HandMadeLine refusing;
            DeviceLine notIn(signalling(refusing.address()), {});
            notIn.receiveBytes("binary"_ba);
            const QSignalSpy refused(&notIn, &DeviceLine::ended);
            notIn.open();
            QTRY_COMPARE(refused.size(), 1);
            HandMadeLine device;
            device.besides += "Sec-WebSocket-Protocol: binary\r\n";
            DeviceLine line(signalling(device.address()), {});
            line.receiveBytes("binary"_ba);
            const QSignalSpy in(&line, &DeviceLine::letIn);
            const QSignalSpy ended(&line, &DeviceLine::ended);
            line.open();
            QTRY_COMPARE(in.size(), 1);
            device.send('\x81', "AAAA"_ba);
            QTRY_COMPARE(ended.size(), 1);
        }
        QCOMPARE(QFileInfo(path).size(), qint64{0});
    }

    void aLineTakesAsManyMessagesASecondAsItIsToldItMay()
    {
        HandMadeLine device;
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Limits limits;
        limits.mostMessagesASecond = 5;
        DeviceLine line(signalling(device.address()), limits);
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy bytes(&line, &DeviceLine::bytesCame);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        for (int one = 0; one < 8; ++one) { // (in one breath: in one second)
            device.send('\x82', "x"_ba);
        }
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
        QVERIFY(bytes.size() <= 5);
    }

    void aLineThatReadsBytesTakesWhateverComesInItsStride()
    {
        // (By seeded chance, the same at every run.) WebSocket's frames of every kind and none, of any length and
        // saying any length, whole, in parts and cut anywhere: what is handed out is a whole message no longer than
        // the line takes, and a line given what it does not take ends, for a reason there is.
        quint64 state = 2026;
        auto below = [&state](int limit) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<int>((state >> 33) % static_cast<quint64>(limit));
        };
        int handed = 0;
        int ends = 0;
        for (int round = 0; round < 40; ++round) {
            HandMadeLine device;
            device.besides += "Sec-WebSocket-Protocol: binary\r\n";
            DeviceLine::Limits limits;
            limits.largestFrameBytes = 4096;
            DeviceLine line(signalling(device.address()), limits);
            line.receiveBytes("binary"_ba);
            const QSignalSpy in(&line, &DeviceLine::letIn);
            const QSignalSpy bytes(&line, &DeviceLine::bytesCame);
            const QSignalSpy ended(&line, &DeviceLine::ended);
            line.open();
            QTRY_COMPARE(in.size(), 1);
            QByteArray stream;
            for (int frame = below(30); frame > 0; --frame) {
                static constexpr std::array<char, 8> firsts = {'\x82', '\x02', '\x00', '\x80',
                                                               '\x81', '\x89', '\x8A', '\xC2'};
                // (Mostly a whole message of bytes, which it takes, and now and then anything at all.)
                stream +=
                    below(3) != 0 ? '\x82' : firsts[static_cast<std::size_t>(below(static_cast<int>(firsts.size())))];
                const int length = below(8) == 0 ? below(70000) : below(300);
                const int said = below(40) == 0 ? below(70000) : length; // (now and then not what follows)
                if (said < 126) {
                    stream += static_cast<char>(said);
                } else if (said < 65536) {
                    stream += '\x7E';
                    stream += static_cast<char>(said >> 8);
                    stream += static_cast<char>(said);
                } else {
                    stream += '\x7F';
                    for (int shift = 56; shift >= 0; shift -= 8) {
                        stream += static_cast<char>(static_cast<quint64>(said) >> shift);
                    }
                }
                for (int one = 0; one < length; ++one) {
                    stream += static_cast<char>(below(256));
                }
            }
            if (below(3) == 0) {
                stream.truncate(below(static_cast<int>(stream.size()) + 1));
            }
            for (qsizetype at = 0; at < stream.size();) {
                const qsizetype step = 1 + below(5000);
                device.sendRaw(stream.mid(at, step));
                at += step;
            }
            QTest::qWait(150);
            for (const auto& one : bytes) {
                QVERIFY(one.first().toByteArray().size() <= limits.largestFrameBytes);
            }
            for (const auto& one : ended) {
                const auto why = one.first().value<DeviceLine::End>();
                QVERIFY(why == DeviceLine::End::Misbehaved || why == DeviceLine::End::Fell);
            }
            handed += static_cast<int>(bytes.size());
            ends += static_cast<int>(ended.size());
        }
        qInfo("40 lines: %d messages handed out, %d lines ended", handed, ends);
    }
};

QTEST_GUILESS_MAIN(TestDeviceLine)
#include "test_device_line.moc"
