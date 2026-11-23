// endOffThisThread (network_end.h), and what a manager it ended leaves behind it on the thread it was made on; and what
// is kept with an object (kept_with.h).

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSemaphore>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslServer>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <qkvm/core/kept_with.h>
#include <qkvm/core/network_end.h>
#include <qkvm/core/tls.h>

#include "stand_in.h"

using namespace Qt::StringLiterals;
using qkvm::core::testing::Tls;

namespace {

Tls tls(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

// What Qt says, from any thread, of threads or timers (Qt's words for an object used from another thread than its
// own: "Timers cannot be stopped from another thread", "Cannot create children for a parent that is in a different
// thread", and their like): kept, and handed on to nobody, so that the case says it and the program goes on. The rest
// goes where it went.
class HeardOfThreads {
public:
    HeardOfThreads()
    {
        const QMutexLocker locked(&mutex());
        before() = qInstallMessageHandler(heard);
    }
    ~HeardOfThreads()
    {
        const QMutexLocker locked(&mutex());
        qInstallMessageHandler(before());
    }
    HeardOfThreads(const HeardOfThreads&) = delete;
    HeardOfThreads& operator=(const HeardOfThreads&) = delete;
    HeardOfThreads(HeardOfThreads&&) = delete;
    HeardOfThreads& operator=(HeardOfThreads&&) = delete;

    static QStringList said()
    {
        const QMutexLocker locked(&mutex());
        return kept();
    }
    static void forget()
    {
        const QMutexLocker locked(&mutex());
        kept().clear();
    }

private:
    static void heard(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        const QMutexLocker locked(&mutex());
        if (message.contains("thread"_L1, Qt::CaseInsensitive) || message.contains("timer"_L1, Qt::CaseInsensitive)) {
            kept().append(message);
            return;
        }
        if (before() != nullptr) {
            before()(type, context, message);
        }
    }
    static QMutex& mutex()
    {
        static QMutex one;
        return one;
    }
    static QtMessageHandler& before()
    {
        static QtMessageHandler one = nullptr;
        return one;
    }
    static QStringList& kept()
    {
        static QStringList one;
        return one;
    }
};

// A server that answers every question at once, briefly, and keeps the line for the next, as a web server keeps one
// (keep-alive), for as long as the caller keeps it; over TLS where it is given a certificate. And how many lines
// callers made, and how many of those have ended since: the caller lets go of an idle line when it no longer keeps it.
class KeepsItsLines : public QObject {
public:
    int lines = 0;
    int linesEnded = 0;

    explicit KeepsItsLines(const std::optional<Tls>& tls = std::nullopt)
    {
        if (tls) {
            static_cast<void>(qkvm::core::useWindowsOwnTls()); // (as the program's: chosen before a server is made)
            auto server = std::make_unique<QSslServer>();
            QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
            configuration.setLocalCertificateChain(QSslCertificate::fromPath(tls->certificateFile, QSsl::Pem));
            QFile key(tls->keyFile);
            if (key.open(QIODevice::ReadOnly)) {
                configuration.setPrivateKey(QSslKey(key.readAll(), QSsl::Rsa, QSsl::Pem));
            }
            server->setSslConfiguration(configuration);
            m_server = std::move(server);
        } else {
            m_server = std::make_unique<QTcpServer>();
        }
        m_server->listen(QHostAddress::LocalHost);
        // (Over TLS a caller is there once the two have agreed on how they speak.)
        connect(m_server.get(), &QTcpServer::pendingConnectionAvailable, this, [this] {
            while (QTcpSocket* line = m_server->nextPendingConnection()) {
                ++lines;
                auto asking = std::make_shared<QByteArray>();
                connect(line, &QTcpSocket::readyRead, line, [line, asking] {
                    *asking += line->readAll();
                    for (qsizetype end = asking->indexOf("\r\n\r\n"); end >= 0; end = asking->indexOf("\r\n\r\n")) {
                        asking->remove(0, end + 4);
                        line->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}");
                    }
                });
                connect(line, &QTcpSocket::disconnected, this, [this, line] {
                    ++linesEnded;
                    line->deleteLater();
                });
            }
        });
    }

    bool isListening() const { return m_server->isListening(); }
    QString address() const
    {
        return u"%1://127.0.0.1:%2"_s.arg(qobject_cast<QSslServer*>(m_server.get()) != nullptr ? u"https"_s : u"http"_s)
            .arg(m_server->serverPort());
    }

private:
    std::unique_ptr<QTcpServer> m_server;
};

// Asked over a manager, with keep-alive (HTTP/1.1's own), and answered: the line is kept, in the manager's cache, for
// as long as Qt keeps an idle line. A certificate of the server's own (tls\first), which nobody signed, is passed over.
bool askedAndAnswered(QNetworkAccessManager& network, const QString& address)
{
    QNetworkReply* reply = network.get(QNetworkRequest(QUrl(address + u"/api/v1/wall"_s)));
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>& errors) { reply->ignoreSslErrors(errors); });
    const bool answered =
        QTest::qWaitFor([reply] { return reply->isFinished(); }, 20'000) && reply->error() == QNetworkReply::NoError;
    reply->deleteLater();
    return answered;
}

} // namespace

class TestNetworkEnd : public QObject {
    Q_OBJECT

private slots:
    void whatIsKeptWithAnObjectEndsOnItsThreadWhereverQtLetsGoOfItsConnections()
    {
        // (kept_with.h.) Qt lets go of a connection's slot on whatever thread cleans up after the connection. Here
        // another thread is inside a signal of the object, and so holds its connections, while the object ends on its
        // own thread: Qt lets go of them there, as the signal ends. What a slot captured ends there; what was kept with
        // the object ends here, with it. (Qt's own network thread cleans up after a reply's connections so, as it ends
        // what it had of the reply: what a slot on a reply captures may end there.)
        std::atomic<QThread*> capturedEndedOn{nullptr};
        std::atomic<QThread*> keptEndedOn{nullptr};
        auto* owner = new QBuffer; // NOLINT(cppcoreguidelines-owning-memory): ended below
        {
            const std::shared_ptr<void> captured(
                nullptr, [&capturedEndedOn](void*) { capturedEndedOn = QThread::currentThread(); });
            const std::shared_ptr<void> kept(nullptr,
                                             [&keptEndedOn](void*) { keptEndedOn = QThread::currentThread(); });
            const std::shared_ptr<void>* const held = &qkvm::core::keptWith(*owner, kept);
            QObject::connect(owner, &QIODevice::readyRead, owner, [captured, held] {
                static_cast<void>(captured);
                static_cast<void>(held);
            });
        }
        QSemaphore inside;
        QSemaphore ended;
        std::thread elsewhere([owner, &inside, &ended] {
            const QObject here; // (of this thread)
            QObject::connect(
                owner, &QIODevice::channelBytesWritten, &here,
                [&inside, &ended] {
                    inside.release();
                    ended.acquire();
                },
                Qt::DirectConnection);
            emit owner->channelBytesWritten(0, 0);
        });
        if (!inside.tryAcquire(1, 10'000)) {
            ended.release();
            elsewhere.join();
            delete owner; // NOLINT(cppcoreguidelines-owning-memory): made above
            QFAIL("The other thread never came inside the object's signal.");
        }
        delete owner; // NOLINT(cppcoreguidelines-owning-memory): made above
        ended.release();
        elsewhere.join();
        QVERIFY2(keptEndedOn.load() == QThread::currentThread(), "What was kept ended on another thread.");
        // And what the slot captured ended on the other thread: why kept_with.h is there. (Where Qt lets go of it here
        // one day, this says so.)
        QVERIFY(capturedEndedOn.load() != nullptr);
        QVERIFY(capturedEndedOn.load() != QThread::currentThread());
    }

    void aManagerEndedOffThisThreadLeavesNothingBehindPastQtsLongestKeepingOfALine()
    {
        // endOffThisThread moves a manager to the thread kept for its end, but what it holds that is not its child
        // keeps the thread it was made on (Qt's private cache of its objects among them, a QObject with a timer, and
        // the QThread object of the thread it speaks HTTP on), and is deleted on that other thread. Where such an
        // object has a timer running then, Qt says that timers cannot be stopped from another thread, and this thread
        // may later hand a timer's turn to an object that is gone. No Qt sources here to read it in: tried instead. A
        // manager speaks over HTTP and HTTPS, its lines are kept (keep-alive), it ends through endOffThisThread, and
        // this thread runs on past the longest Qt keeps a line, which another manager, kept, shows: its lines end when
        // Qt lets them go, measured here. Nothing is said of threads or timers meanwhile, and under the address
        // sanitizer nothing is used after it was freed.
        std::optional<HeardOfThreads> heard;
        heard.emplace();
        {
            // (The watch hears what Qt says from another thread: a timer of this thread's, stopped from there.)
            QTimer mine;
            mine.start(60'000);
            std::thread elsewhere([&mine] { mine.stop(); });
            elsewhere.join();
            QVERIFY2(HeardOfThreads::said().size() == 1, qPrintable(HeardOfThreads::said().join(u" | "_s)));
            HeardOfThreads::forget();
        }
        KeepsItsLines plain;
        KeepsItsLines encrypted(tls("first"));
        QVERIFY(plain.isListening());
        QVERIFY(encrypted.isListening());

        QNetworkAccessManager measure; // kept: how long Qt keeps an idle line
        QVERIFY(askedAndAnswered(measure, plain.address()));
        QVERIFY(askedAndAnswered(measure, encrypted.address()));
        QElapsedTimer idle;
        idle.start();

        auto* ended = new QNetworkAccessManager; // NOLINT(cppcoreguidelines-owning-memory): ended below
        QVERIFY(askedAndAnswered(*ended, plain.address()));
        QVERIFY(askedAndAnswered(*ended, encrypted.address()));
        QVERIFY(askedAndAnswered(*ended, plain.address())); // (again on the line it kept)
        QCOMPARE(plain.lines, 2);
        QCOMPARE(encrypted.lines, 2);
        QCOMPARE(plain.linesEnded + encrypted.linesEnded, 0);
        qkvm::core::endOffThisThread(ended);
        // Its lines end with it, on its own thread.
        QTRY_COMPARE_WITH_TIMEOUT(plain.linesEnded + encrypted.linesEnded, 2, 10'000);

        // The kept manager's lines end when Qt lets go of them, each: the longest it keeps one (120 s is possible).
        qint64 plainKeptMs = -1;
        qint64 encryptedKeptMs = -1;
        QVERIFY2(QTest::qWaitFor(
                     [&] {
                         if (plainKeptMs < 0 && plain.linesEnded == 2) {
                             plainKeptMs = idle.elapsed();
                         }
                         if (encryptedKeptMs < 0 && encrypted.linesEnded == 2) {
                             encryptedKeptMs = idle.elapsed();
                         }
                         return plainKeptMs >= 0 && encryptedKeptMs >= 0;
                     },
                     200'000),
                 "Qt kept an idle line for more than 200 s.");
        // And on, past every turn of a timer that was due by then.
        QTest::qWait(10'000);
        const QStringList said = HeardOfThreads::said();
        QVERIFY2(said.isEmpty(), qPrintable(u"Qt said: "_s + said.join(u" | "_s)));
        heard.reset(); // (what is said below is heard as before)
        qInfo(
            "Qt kept an idle line over HTTP %lld ms, over HTTPS %lld ms; nothing was said of threads or timers in %lld "
            "ms after the end.",
            plainKeptMs, encryptedKeptMs, idle.elapsed());
    }
};

QTEST_GUILESS_MAIN(TestNetworkEnd)
#include "test_network_end.moc"
