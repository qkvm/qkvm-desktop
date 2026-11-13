#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>

#include <qkvm/core/server_client.h>

#include "far_away_file.h"
#include "media_by_hand.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::FarAwayFile;
using core::testing::ThreadWatch;
using core::testing::UploadByHand;

namespace {

// How much memory this program holds as its own just now (Windows' private bytes).
qint64 heldNow()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    return static_cast<qint64>(counters.PrivateUsage);
}

// What claims to be a file of this many bytes, all of them zeros, and holds none of them: an image as large as the
// server's limit, and one larger, without sixteen gigabytes on a disk.
class Zeros final : public QIODevice {
public:
    explicit Zeros(qint64 size) : m_size(size) { open(QIODevice::ReadOnly); }
    qint64 size() const override { return m_size; }
    bool isSequential() const override { return false; }

protected:
    qint64 readData(char* data, qint64 most) override
    {
        const qint64 some = std::min(most, m_size - pos());
        if (some <= 0) {
            return some == 0 ? 0 : -1;
        }
        std::fill_n(data, some, '\0');
        return some;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    qint64 m_size;
};

// What a call answered, once it did.
template <typename T>
struct Heard {
    bool came = false;
    base::Result<T> what = base::Error{"never", "Nothing was answered."};

    std::function<void(const base::Result<T>&)> taker()
    {
        return [this](const base::Result<T>& said) {
            what = said;
            came = true;
        };
    }
};

// The stand-in's ways: about 2 MB a second; one that stops reading; one that refuses.
core::testing::UploadByHand::Way paced()
{
    core::testing::UploadByHand::Way way;
    way.bytesPerTick = 20'000;
    way.tickMs = 10;
    return way;
}

core::testing::UploadByHand::Way stoppingAfter(qint64 bytes)
{
    core::testing::UploadByHand::Way way;
    way.stopAfter = bytes;
    return way;
}

core::testing::UploadByHand::Way refusing(int status, const QByteArray& body, bool atTheEnd)
{
    core::testing::UploadByHand::Way way;
    way.refusal = status;
    way.refusalBody = body;
    way.refusesAtTheEnd = atTheEnd;
    return way;
}

} // namespace

// An image carried through a QKVM to a machine's device (0.3.0, POST .../media/upload): from the file as it is read,
// a piece at a time, with its length said first and 16 GB at the most, and never held whole in this program; the
// program's own thread goes on meanwhile, however slowly the server takes it; given as long as it moves, cut off
// where it stops; and stopped by whoever began it. These wait for slow uploads: a program of their own.
class TestServerClientUpload : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_folder{QCoreApplication::applicationDirPath() + u"/upload-test-XXXXXX"_s};

    // A file of this many bytes, of a pattern; its SHA-256.
    QByteArray fileOf(const QString& name, qint64 size, QString& path)
    {
        path = m_folder.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return {};
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray piece(1 << 20, '\0');
        for (qsizetype at = 0; at < piece.size(); ++at) {
            piece[at] = static_cast<char>((at * 7 + 3) & 0xFF);
        }
        for (qint64 left = size; left > 0; left -= piece.size()) {
            const QByteArray part = piece.first(std::min<qint64>(left, piece.size()));
            file.write(part);
            hash.addData(part);
        }
        return hash.result().toHex();
    }

    static std::unique_ptr<QFile> opened(const QString& path)
    {
        auto file = std::make_unique<QFile>(path);
        static_cast<void>(file->open(QIODevice::ReadOnly));
        return file;
    }

    static void sessionFor(core::ServerClient& client, const UploadByHand& server)
    {
        static_cast<void>(client.setAddress(server.address()));
        client.setSession(QByteArray(43, 's'));
    }

    // Uploads, and waits for the answer as the program's own loop waits: for what comes next, and not in steps of a
    // nap between two looks, as QTest::qWaitFor does (the file is read as the line takes it, on this thread: a nap
    // after every few pieces would be the test's slowness, not the program's).
    static base::Result<core::MediaUploaded> upload(core::ServerClient& client, std::unique_ptr<QIODevice> image,
                                                    const QString& name = u"debian 13+.iso"_s, int withinMs = 60000)
    {
        Heard<core::MediaUploaded> answer;
        QEventLoop waiting;
        client.uploadMedia(u"kvm"_s, name, u"Build server"_s, std::move(image),
                           [&](const base::Result<core::MediaUploaded>& done) {
                               answer.taker()(done);
                               waiting.quit();
                           });
        QTimer::singleShot(withinMs, &waiting, &QEventLoop::quit);
        if (!answer.came) {
            waiting.exec();
        }
        return answer.what;
    }

private slots:
    void theImageGoesWholeAsItIsReadWithItsNameAndTheMachineAsNamed()
    {
        UploadByHand server({});
        core::ServerClient client;
        sessionFor(client, server);
        QString path;
        const QByteArray sha = fileOf(u"a.iso"_s, 8'000'001, path);
        const base::Result<core::MediaUploaded> done = upload(client, opened(path));
        QVERIFY2(done.ok(), done.ok() ? "" : done.error().message.c_str());
        QCOMPARE(server.received(), qint64{8'000'001});
        QCOMPARE(server.sha256(), sha);
        QCOMPARE(done.value().sha256, QString::fromLatin1(sha));
        QCOMPARE(server.asked().first(),
                 QByteArray("POST /api/v1/devices/kvm/media/upload?name=debian%2013%2B.iso&machine=Build%20server "
                            "HTTP/1.1"));
    }

    void itIsNeverHeldWholeInThisProgram()
    {
        // A quarter of a gigabyte through a server that takes it as fast as it comes: this program holds no more
        // than a few pieces of it at any moment (Qt reads a file in pieces of 16 kB as its line takes them).
        UploadByHand server({});
        core::ServerClient client;
        sessionFor(client, server);
        const QString path = m_folder.filePath(u"large.img"_s);
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.resize(qint64{256} << 20));
        }
        const qint64 before = heldNow();
        qint64 most = before;
        QTimer look;
        QObject::connect(&look, &QTimer::timeout, &look, [&] { most = std::max(most, heldNow()); });
        look.start(5);
        const base::Result<core::MediaUploaded> done = upload(client, opened(path), u"large.img"_s, 120000);
        look.stop();
        QVERIFY2(done.ok(), done.ok() ? "" : done.error().message.c_str());
        QCOMPARE(server.received(), qint64{256} << 20);
#if !defined(__SANITIZE_ADDRESS__)
        // (Not under the address sanitizer, which keeps what is freed for a while: what is committed says nothing
        // there. The upload itself, whole, is held to there too.)
        QVERIFY2(most - before < (qint64{48} << 20), qPrintable(u"held %1 MB more"_s.arg((most - before) >> 20)));
#endif
    }

    void theProgramsOwnThreadGoesOnWhileAServerTakesItSlowly()
    {
        // About 2 MB a second: three seconds for six megabytes. The thread the program draws its windows on turns
        // the whole time, and is asked something else and answers it meanwhile.
        UploadByHand server(paced());
        core::ServerClient client;
        sessionFor(client, server);
        QString path;
        fileOf(u"slow.iso"_s, 6'000'000, path);
        QElapsedTimer clock;
        clock.start();
        qint64 last = 0;
        qint64 longest = 0;
        QTimer beat;
        QObject::connect(&beat, &QTimer::timeout, &beat, [&] {
            longest = std::max(longest, clock.elapsed() - last);
            last = clock.elapsed();
        });
        beat.start(10);
        Heard<std::optional<core::MediaUploading>> meanwhile;
        QTimer::singleShot(1000, &beat, [&] { client.askUpload(u"kvm"_s, meanwhile.taker()); });
        const base::Result<core::MediaUploaded> done = upload(client, opened(path), u"slow.iso"_s);
        beat.stop();
        QVERIFY2(done.ok(), done.ok() ? "" : done.error().message.c_str());
        QVERIFY2(clock.elapsed() > 2000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY2(longest < 500, qPrintable(u"the thread stood still for %1 ms"_s.arg(longest)));
        QVERIFY(meanwhile.came && meanwhile.what.ok() && meanwhile.what.value().has_value());
        const core::MediaUploading up = meanwhile.what.value().value_or(core::MediaUploading{});
        QCOMPARE(up.name, u"slow.iso"_s);
        QCOMPARE(up.size, qint64{6'000'000});
        QVERIFY2(up.sent > 0 && up.sent < up.size, qPrintable(QString::number(up.sent)));
    }

    void theFileIsReadBesideTheProgramsThreadHoweverSlowlyItAnswers()
    {
        // A share far away, each read of it two seconds: the thread the program draws its windows on turns the whole
        // time, and the image goes whole and in order.
        UploadByHand server({});
        core::ServerClient client;
        sessionFor(client, server);
        QString path;
        const QByteArray sha = fileOf(u"far.iso"_s, 600'000, path);
        const ThreadWatch watched;
        const base::Result<core::MediaUploaded> done =
            upload(client, std::make_unique<FarAwayFile>(600'000, 2000), u"far.iso"_s, 30000);
        QVERIFY2(done.ok(), done.ok() ? "" : done.error().message.c_str());
        QCOMPARE(server.received(), qint64{600'000});
        QCOMPARE(server.sha256(), sha);
        QVERIFY2(watched.longest < 500, qPrintable(u"the thread stood still for %1 ms"_s.arg(watched.longest)));
    }

    void aFileThatCannotBeReadOnTheWayEndsItWithWhy()
    {
        UploadByHand server({});
        core::ServerClient client;
        sessionFor(client, server);
        const ThreadWatch watched;
        const base::Result<core::MediaUploaded> done =
            upload(client, std::make_unique<FarAwayFile>(2'000'000, 0, 700'000), u"gone.iso"_s, 30000);
        QVERIFY(!done.ok());
        QCOMPARE(done.error().code, std::string(core::ServerClient::imageUnreadable));
        QVERIFY2(QString::fromStdString(done.error().message)
                     .contains(u"The specified network name is no longer available."_s),
                 done.error().message.c_str());
        QVERIFY2(watched.longest < 500, qPrintable(u"the thread stood still for %1 ms"_s.arg(watched.longest)));
    }

    void aShareThatStopsAnsweringLeavesTheProgramGoingAndTheUploadIsCutOff()
    {
        // Nothing more comes of the file: the program goes on, and the upload, which moves no more, is cut off as one
        // whose line stops. (The read that stands still is let go of at the end, and comes back to nobody.)
        UploadByHand server({});
        core::ServerClient client;
        client.setUploadQuietMs(1500);
        sessionFor(client, server);
        const auto gate = std::make_shared<FarAwayFile::Gate>();
        // (Where the program's thread would stand in the read itself, it is let go of later, by another.)
        std::thread later([gate] {
            std::this_thread::sleep_for(std::chrono::seconds(8));
            gate->letGo();
        });
        const ThreadWatch watched;
        QElapsedTimer clock;
        clock.start();
        const base::Result<core::MediaUploaded> done =
            upload(client, std::make_unique<FarAwayFile>(4'000'000, 0, -1, 1'000'000, gate), u"stops.iso"_s, 30000);
        const qint64 took = clock.elapsed();
        gate->letGo();
        later.join();
        QVERIFY(!done.ok());
        QCOMPARE(done.error().code, std::string(core::ServerClient::unreachable));
        QVERIFY2(took < 7000, qPrintable(QString::number(took)));
        QVERIFY2(watched.longest < 500, qPrintable(u"the thread stood still for %1 ms"_s.arg(watched.longest)));
        QVERIFY(server.received() < 1'000'000);
    }

    void anUploadIsGivenAsLongAsItMovesAndCutOffWhereItStops()
    {
        UploadByHand slow(paced());
        core::ServerClient client;
        client.setUploadQuietMs(1000);
        sessionFor(client, slow);
        QString path;
        fileOf(u"moves.iso"_s, 6'000'000, path);
        // Three seconds: more than the silence it may have, and more than twice that.
        const base::Result<core::MediaUploaded> done = upload(client, opened(path), u"moves.iso"_s);
        QVERIFY2(done.ok(), done.ok() ? "" : done.error().message.c_str());

        UploadByHand stuck(stoppingAfter(1'000'000));
        core::ServerClient other;
        other.setUploadQuietMs(1500);
        sessionFor(other, stuck);
        QElapsedTimer clock;
        clock.start();
        const base::Result<core::MediaUploaded> cut = upload(other, opened(path), u"moves.iso"_s);
        QVERIFY(!cut.ok());
        QCOMPARE(cut.error().code, std::string(core::ServerClient::unreachable));
        QVERIFY2(clock.elapsed() < 10000, qPrintable(QString::number(clock.elapsed())));
    }

    void whoeverBeganItStopsItAndNothingMoreGoes()
    {
        UploadByHand server(paced());
        core::ServerClient client;
        sessionFor(client, server);
        QString path;
        fileOf(u"stop.iso"_s, 6'000'000, path);
        Heard<core::MediaUploaded> answer;
        const std::function<void()> stop =
            client.uploadMedia(u"kvm"_s, u"stop.iso"_s, u"Build server"_s, opened(path), answer.taker());
        QVERIFY(QTest::qWaitFor([&] { return server.received() > 100'000; }, 10000));
        stop();
        QVERIFY(QTest::qWaitFor([&] { return answer.came; }, 5000));
        QVERIFY(!answer.what.ok());
        QCOMPARE(answer.what.error().code, std::string(core::ServerClient::uploadStopped));
        QVERIFY(QTest::qWaitFor([&] { return server.cutBySender(); }, 5000));
        const qint64 got = server.received();
        QTest::qWait(500);
        QCOMPARE(server.received(), got);
        // Whoever stopped it is still in contact: the next question goes as it is.
        Heard<std::optional<core::MediaUploading>> now;
        client.askUpload(u"kvm"_s, now.taker());
        QVERIFY(QTest::qWaitFor([&] { return now.came; }, 5000));
        QVERIFY2(now.what.ok(), now.what.ok() ? "" : now.what.error().message.c_str());
    }

    void whatTheServerRefusesIsSaidInItsWords_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("code");
        QTest::newRow("no space")
            << 507
            << R"({"code": "no_space", "message": "The storage of the KVM has 1,000 bytes free, and the image is 8,000,000: delete an image there first."})"_ba
            << "no_space"_ba;
        QTest::newRow("no virtual media")
            << 404 << R"({"code": "no_virtual_media", "message": "Build server has no virtual media"})"_ba
            << "no_virtual_media"_ba;
        QTest::newRow("a token")
            << 403 << R"({"code": "not_allowed", "message": "Virtual media is for a person, not for a token"})"_ba
            << "not_allowed"_ba;
    }

    void whatTheServerRefusesIsSaidInItsWords()
    {
        // Where its answer reaches this program: once the image went (a device that refuses at its end), or before
        // (the line kept for the answer to be read). A refusal before a byte was read is answered by a server that
        // closes the line under what is still being sent, and that answer is lost on the way, often or always (seen
        // here, as the page says of its own: app.js, refusedAtOnce): it is told as no answer, and nothing is sent
        // again. So the box says beforehand, by its last look, what the server would refuse (MediaView).
        QFETCH(const int, status);
        QFETCH(const QByteArray, body);
        QFETCH(const QByteArray, code);
        QString path;
        fileOf(u"refused.iso"_s, 8'000'000, path);
        {
            UploadByHand server(refusing(status, body, true));
            core::ServerClient client;
            sessionFor(client, server);
            const base::Result<core::MediaUploaded> done = upload(client, opened(path), u"refused.iso"_s);
            QVERIFY(!done.ok());
            QCOMPARE(QByteArray::fromStdString(done.error().code), code);
            QCOMPARE(QString::fromStdString(done.error().message),
                     QJsonDocument::fromJson(body).object().value(u"message"_s).toString());
        }
        UploadByHand early(refusing(status, body, false));
        core::ServerClient client;
        sessionFor(client, early);
        const base::Result<core::MediaUploaded> done = upload(client, opened(path), u"refused.iso"_s);
        QVERIFY(!done.ok());
        const QByteArray said = QByteArray::fromStdString(done.error().code);
        QVERIFY2(said == code || said == core::ServerClient::unreachable, said.constData());
        QTest::qWait(300);
        QCOMPARE(early.asked().size(), 1);
    }

    void anUploadUnderWayGoesOnWhereContactIsLostByAnotherCall()
    {
        // While an image is on its way, another call finds nothing answering (a look at how the machines are, on a
        // line that falls): what had begun to go with the session is not taken back for it (ServerClient::
        // lostContact), and the image goes whole.
        UploadByHand server(paced());
        core::ServerClient client;
        sessionFor(client, server);
        QString path;
        fileOf(u"on.iso"_s, 4'000'000, path);
        Heard<core::MediaUploaded> answer;
        client.uploadMedia(u"kvm"_s, u"on.iso"_s, u"Build server"_s, opened(path), answer.taker());
        QVERIFY(QTest::qWaitFor([&] { return server.received() > 200'000; }, 10000));
        Heard<QHash<QString, core::Status>> status;
        client.askStatus(status.taker());
        QVERIFY(QTest::qWaitFor([&] { return status.came; }, 10000));
        QVERIFY(!status.what.ok());
        QVERIFY(QTest::qWaitFor([&] { return answer.came; }, 30000));
        QVERIFY2(answer.what.ok(), answer.what.ok() ? "" : answer.what.error().message.c_str());
        QCOMPARE(server.received(), qint64{4'000'000});
    }

    void anImageIsRefusedHereWhereTheServerWouldRefuseIt()
    {
        UploadByHand server({});
        core::ServerClient client;
        sessionFor(client, server);
        // Larger than 16 GB, empty, of a name the server does not take: refused here, and nothing goes.
        base::Result<core::MediaUploaded> done = upload(client, std::make_unique<Zeros>(16'000'000'001));
        QVERIFY(!done.ok());
        QCOMPARE(done.error().code, std::string("too_large"));
        done = upload(client, std::make_unique<Zeros>(0));
        QVERIFY(!done.ok());
        QCOMPARE(done.error().code, std::string("invalid"));
        for (const QString& name :
             {u"../x.iso"_s, u".hidden.iso"_s, u"x.iso "_s, u"x.iso.incomplete"_s, u"c:x.iso"_s}) {
            done = upload(client, std::make_unique<Zeros>(1000), name);
            QVERIFY2(!done.ok(), qPrintable(name));
            QCOMPARE(done.error().code, std::string("invalid"));
        }
        QTest::qWait(200);
        QCOMPARE(server.uploads(), 0);
        QVERIFY(server.asked().isEmpty());
        // Sixteen gigabytes, exactly, go.
        Heard<core::MediaUploaded> answer;
        const std::function<void()> stop = client.uploadMedia(u"kvm"_s, u"sixteen.img"_s, u"Build server"_s,
                                                              std::make_unique<Zeros>(16'000'000'000), answer.taker());
        QVERIFY(QTest::qWaitFor([&] { return server.received() > 0; }, 10000));
        stop();
        QVERIFY(QTest::qWaitFor([&] { return answer.came; }, 5000));
        QCOMPARE(answer.what.error().code, std::string(core::ServerClient::uploadStopped));
    }
};

QTEST_MAIN(TestServerClientUpload)
#include "test_server_client_upload.moc"
