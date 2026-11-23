#pragma once

// A place where a certificate's signer is said to be (its AIA, "where to fetch the one who signed me"), at this PC's
// own address, that takes Windows' asking for it and never answers. Windows, judging a line whose certificate names it
// (tls\stalls-<program>, which tools/make_testdata.ps1 makes: signed by a maker that is in no store, and that a TLS
// server does not send, being a root), fetches the signer from there while it builds the certificate's chain, and
// waits for the answer on the thread that judges the line, as on a PC without internet a fetch from a real signer's
// address waits. Qt speaks HTTP on a thread of its own for each manager of it (QNetworkAccessManager): that thread is
// held meanwhile, and a manager that ends then waits up to five seconds for it (seen: 5000 ms, every time).
//
// And a server that shows that certificate, and a watch on the thread that runs a test: the longest it was held still.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

#include <qkvm/core/tls.h>

#include "stand_in.h"

namespace qkvm::core::testing {

// The certificate of one stall case (tls\<name>, tools/make_testdata.ps1, whose table of names and ports this is), and
// the three ports it names as its signer's place, in that order (Windows asks the next where nobody listens at one).
// Each case has its own and its own ports:
// - CTest runs four programs at once, and with one for all, a program's fetch landed on whichever listened first: a
//   case could take another program's fetch for its own and go on before its own line was held at all, and pass
//   without proving the wait.
// - In one program, Windows does not fetch from a place again once a fetch from it came to nothing (a second run of a
//   case in one program, 2026-11-11: no fetch in 60 s, from the program or from Windows' LSASS).
struct Stalls {
    const char* name;
    std::array<quint16, 3> ports;
};
// Nor is any of them a port another test takes: 47391 and 47392 are the profile fixtures' stand-in servers', where
// every published version made its fixture and which the upgrade tests listen at again (ui/tests/profile_maker.cpp).
// The asker's were 47391-47393 until 2026-11-21: run beside test_ui_upgrade_continuity, whichever listened first had
// it, and the other failed, "port 47391 of this PC is taken" (review arm 1, L7). tools/test_review_guards.ps1 holds the
// table to that, and to tools/make_testdata.ps1's.
inline constexpr Stalls stallsOfTheAsker{"stalls-asker-own", {47448, 47449, 47450}};
inline constexpr Stalls stallsOfTheProbe{"stalls-probe", {47394, 47395, 47396}};
inline constexpr Stalls stallsOfTheClient{"stalls-client", {47397, 47398, 47399}};
inline constexpr Stalls stallsOfAnotherServer{"stalls-client-address", {47400, 47401, 47402}};
inline constexpr Stalls stallsOfAnotherCertificate{"stalls-client-certificate", {47403, 47404, 47405}};
inline constexpr Stalls stallsOfAVncCertificate{"stalls-vnc-certificate", {47406, 47407, 47408}};
inline constexpr Stalls stallsOfAVncSession{"stalls-vnc-session", {47409, 47410, 47411}};
// test_device_line_tls: a line of each kind, and the program's end while a line or an asking waits.
inline constexpr Stalls stallsOfAnAwakeLine{"stalls-line-awake", {47412, 47413, 47414}};
inline constexpr Stalls stallsOfAConsoleLine{"stalls-line-console", {47415, 47416, 47417}};
inline constexpr Stalls stallsOfJanus{"stalls-line-janus", {47418, 47419, 47420}};
inline constexpr Stalls stallsOfAJetKvm{"stalls-line-jetkvm", {47421, 47422, 47423}};
inline constexpr Stalls stallsOfALiveLink{"stalls-line-live", {47424, 47425, 47426}};
inline constexpr Stalls stallsOfOpenBmc{"stalls-line-openbmc", {47427, 47428, 47429}};
inline constexpr Stalls stallsOfAProxmoxGuest{"stalls-line-proxmox", {47430, 47431, 47432}};
inline constexpr Stalls stallsOfAmi{"stalls-line-ami", {47433, 47434, 47435}};
inline constexpr Stalls stallsOfAnRfbScreen{"stalls-line-rfb", {47436, 47437, 47438}};
inline constexpr Stalls stallsOfAQuitLine{"stalls-line-quit", {47439, 47440, 47441}};
inline constexpr Stalls stallsOfAQuitAsking{"stalls-vnc-quit", {47442, 47443, 47444}};
inline constexpr Stalls stallsOfAJudgementAgain{"stalls-line-again", {47445, 47446, 47447}};

// The program a line on this PC's own address comes from, by Windows' table of lines and their programs: the one whose
// end of it is at theirPort and calls ourPort. 0 where there is none (it has gone meanwhile). (signer_far_away.cpp:
// Windows' headers, which make "far" a word of their own, stay there.)
qint64 programOfALine(quint16 theirPort, quint16 ourPort);

class SignerFarAway : public QObject {
public:
    // At the first of the stalls' ports that is free (one is taken where a second run of the same program listens).
    // `toldFirst`: this program's first so many fetches are told at once that there is nothing here, as another
    // program's are, and only the ones after them wait (a judgement after the handshake's, which fetches anew).
    explicit SignerFarAway(const Stalls& stalls, int toldFirst = 0) : m_stalls(stalls), m_toldFirst(toldFirst)
    {
        m_clock.start();
        for (const quint16 port : stalls.ports) {
            if (m_server.listen(QHostAddress::LocalHost, port)) {
                break;
            }
        }
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* one = m_server.nextPendingConnection()) {
                auto fetch = std::make_shared<Fetch>();
                fetch->line = one;
                fetch->program = programOfALine(one->peerPort(), m_server.serverPort());
                fetch->atMs = m_clock.elapsed();
                connect(one, &QTcpSocket::disconnected, this, [this, fetch] { fetch->endedAtMs = m_clock.elapsed(); });
                m_fetches.append(fetch);
                if (fetch->program == QCoreApplication::applicationPid() && m_toldFirst-- <= 0) {
                    continue; // this program's own: taken, answered never
                }
                // Another program's: Windows' own side of the test's server (LSASS, which does a server's TLS for it)
                // builds the chain of the certificate the server shows as well, and fetches its signer too: the signer
                // the server was given stays in this program. Held, it held the server's handshake (17 s, seen), and
                // the line being judged never came to be judged here; the client's thread was held in Windows' TLS
                // meanwhile, not by its own fetch. So it hears at once that there is nothing here, and the server shows
                // its certificate: what is held is this program's own fetch, on the line being judged, as on a PC
                // without internet. (And a second run of the same program at once, whose fetch may come here, goes on
                // to its own ports.)
                connect(one, &QTcpSocket::readyRead, one, [one, fetch] {
                    fetch->asking += one->readAll();
                    if (fetch->asking.contains("\r\n\r\n") && !fetch->answered) {
                        fetch->answered = true;
                        one->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        one->disconnectFromHost();
                    }
                });
            }
        });
    }

    // tls\<this>: the certificate that names this place.
    QString certificate() const { return QString::fromLatin1(m_stalls.name); }
    bool isListening() const { return m_server.isListening(); }
    quint16 port() const { return m_server.serverPort(); }
    // How often Windows came to fetch the signer here for this program, judging one of its lines: for this program's
    // certificate (whose ports no other program's names), from this program.
    int asked() const { return static_cast<int>(std::count_if(m_fetches.begin(), m_fetches.end(), own)); }
    // And how many of those it still waits on (its end of the line is open).
    int waitedOn() const
    {
        return static_cast<int>(std::count_if(m_fetches.begin(), m_fetches.end(), [](const auto& fetch) {
            return own(fetch) && fetch->line != nullptr && fetch->line->state() == QAbstractSocket::ConnectedState;
        }));
    }
    // What a case says of it: each fetch, by which program, when, and when its line ended.
    QString said() const
    {
        QStringList fetches;
        for (const auto& fetch : m_fetches) {
            const QString who = own(fetch) ? QStringLiteral("this program")
                                           : QStringLiteral("program %1 (told: none here)").arg(fetch->program);
            fetches.append(QStringLiteral("%1 at %2 ms, %3")
                               .arg(who)
                               .arg(fetch->atMs)
                               .arg(fetch->endedAtMs < 0
                                        ? QStringLiteral("still waiting")
                                        : QStringLiteral("ended after %1 ms").arg(fetch->endedAtMs - fetch->atMs)));
        }
        return QStringLiteral("%1 at port %2, this program's fetches %3, %4 still waited on: [%5]")
            .arg(certificate())
            .arg(m_server.serverPort())
            .arg(asked())
            .arg(waitedOn())
            .arg(fetches.join(u"; "));
    }

private:
    struct Fetch {
        QPointer<QTcpSocket> line;
        qint64 program = 0;
        qint64 atMs = 0;
        qint64 endedAtMs = -1;
        QByteArray asking;
        bool answered = false;
    };

    static bool own(const std::shared_ptr<Fetch>& fetch)
    {
        return fetch->program == QCoreApplication::applicationPid();
    }

    Stalls m_stalls;
    int m_toldFirst = 0;
    QElapsedTimer m_clock;
    QTcpServer m_server;
    QList<std::shared_ptr<Fetch>> m_fetches;
};

// A server over TLS, showing a certificate (a tls\stalls-<program>, with its signer, which Windows' own side of the
// server does not see: it fetches the signer all the same, and SignerFarAway tells it at once that there is none), that
// lets lines in, counts what they say, and answers nothing on them. On a thread of its own: while Windows' side of it
// fetches, it holds the thread that lets the line in.
class TlsDoorElsewhere {
public:
    // (Windows' own TLS, as the program's: chosen before the server is made, which would take Qt's own pick.)
    explicit TlsDoorElsewhere(const Tls& tls) : m_windowsOwnTls(useWindowsOwnTls())
    {
        m_here.moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(&m_here, [this, tls] { open(tls); }, Qt::BlockingQueuedConnection);
    }
    // (Waits for its thread, which Windows may still hold for a while.)
    ~TlsDoorElsewhere()
    {
        QMetaObject::invokeMethod(&m_here, [this] { m_server.reset(); }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    TlsDoorElsewhere(const TlsDoorElsewhere&) = delete;
    TlsDoorElsewhere& operator=(const TlsDoorElsewhere&) = delete;
    TlsDoorElsewhere(TlsDoorElsewhere&&) = delete;
    TlsDoorElsewhere& operator=(TlsDoorElsewhere&&) = delete;

    QString address() const { return QStringLiteral("https://127.0.0.1:%1").arg(m_port.load()); }
    bool speaksWindowsOwnTls() const { return m_windowsOwnTls; }
    // How many lines it has let in: whose TLS was spoken to its end, the caller's judgement of the certificate among it
    // (its last word comes after that). And how many bytes all of them said to it since.
    int letIn() const { return m_letIn.load(); }
    qint64 heard() const { return m_heard.load(); }

private:
    void open(const Tls& tls)
    {
        m_server = std::make_unique<QSslServer>();
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificateChain(QSslCertificate::fromPath(tls.certificateFile, QSsl::Pem));
        QFile key(tls.keyFile);
        if (key.open(QIODevice::ReadOnly)) {
            configuration.setPrivateKey(QSslKey(key.readAll(), QSsl::Rsa, QSsl::Pem));
        }
        m_server->setSslConfiguration(configuration);
        // (A caller that takes its time over the certificate is not dropped for that.)
        m_server->setHandshakeTimeout(120'000);
        QObject::connect(m_server.get(), &QTcpServer::pendingConnectionAvailable, &m_here, [this] {
            while (QTcpSocket* one = m_server->nextPendingConnection()) {
                ++m_letIn;
                QObject::connect(one, &QTcpSocket::readyRead, &m_here,
                                 [this, one] { m_heard += one->readAll().size(); });
            }
        });
        if (m_server->listen(QHostAddress::LocalHost)) {
            m_port = m_server->serverPort();
        }
    }

    bool m_windowsOwnTls;
    QThread m_thread;
    QObject m_here;
    std::unique_ptr<QSslServer> m_server;
    std::atomic<quint16> m_port{0};
    std::atomic<int> m_letIn{0};
    std::atomic<qint64> m_heard{0};
};

// Looks every 10 ms whether the thread it was made on runs, and keeps the longest it did not.
class Stillness : public QObject {
public:
    Stillness()
    {
        m_clock.start();
        m_tick.setInterval(10);
        m_tick.setTimerType(Qt::PreciseTimer);
        connect(&m_tick, &QTimer::timeout, this, [this] {
            const qint64 now = m_clock.elapsed();
            m_longest = std::max(m_longest, now - m_last);
            m_last = now;
        });
        m_tick.start();
    }

    // From now on.
    void again()
    {
        m_last = m_clock.elapsed();
        m_longest = 0;
    }
    qint64 longestMs() const { return std::max(m_longest, m_clock.elapsed() - m_last); }

private:
    QElapsedTimer m_clock;
    QTimer m_tick;
    qint64 m_last = 0;
    qint64 m_longest = 0;
};

} // namespace qkvm::core::testing
