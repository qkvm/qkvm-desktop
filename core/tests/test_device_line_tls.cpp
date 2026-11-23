// Every kind of device line, over TLS, while Windows judges the certificate its device shows and fetches the signer
// that certificate names from an address that does not answer (outside review 2.2): the interface thread goes on, and
// every line ends in its time, closed, cut or thrown away at once. And the program's end, while Windows still holds a
// line's thread, or a certificate's asking: under a second. (signer_far_away.h: the far signer, the certificates and
// their ports; each case its own, as Windows asks a place once in a program.)
#include "signer_far_away.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QTcpServer>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

#include <qkvm/core/device_line.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/vnc_device.h>

#include "line_threads.h"

using namespace qkvm::core;
using namespace qkvm::core::testing;
using namespace Qt::StringLiterals;

namespace {

Tls tlsOf(const Stalls& stalls)
{
    const QString name = QString::fromLatin1(stalls.name);
    return {QStringLiteral(QKVM_TESTDATA "/tls/") + name + u".crt"_s,
            QStringLiteral(QKVM_TESTDATA "/tls/") + name + u".key"_s};
}

// The SHA-256 of the certificate a stall case's server shows: what its lines know their device by, as a KVM, a
// server's controller or a Proxmox is known by its certificate on the wall.
QByteArray sha256Of(const Stalls& stalls)
{
    const QList<QSslCertificate> chain = QSslCertificate::fromPath(tlsOf(stalls).certificateFile, QSsl::Pem);
    return chain.isEmpty() ? QByteArray() : chain.first().digest(QCryptographicHash::Sha256);
}

// Every kind of line the program speaks over TLS, each set up as its user sets it up (core/src).
enum class Kind {
    AwakeLine,        // a kvmd KVM's line that keeps its video awake, and hears its board's state (awake_line)
    ConsoleLine,      // a kvmd KVM's keyboard and mouse (kvmd_console_line)
    JanusSignalling,  // a kvmd KVM's video's signalling (kvmd_device, kvmd_webrtc)
    JetKvmSignalling, // a JetKVM's signalling (jetkvm_device)
    LiveLink,         // a machine live at a qKVM server (live_link)
    OpenBmcConsole,   // OpenBMC's console (rfb_screen, redfish_console_way)
    ProxmoxConsole,   // a Proxmox guest's console (rfb_screen, proxmox_screen)
    AmiConsole,       // AMI's console (server_screen)
    RfbScreen,        // a vncs:// server's RFB (rfb_screen, vnc_device)
};

std::unique_ptr<DeviceLine> lineOf(Kind kind, const QUrl& address, const QByteArray& certificate,
                                   const DeviceLine::Limits& limits)
{
    DeviceLine::Where where;
    where.address = address;
    where.knownByCertificate = true;
    where.certificate = certificate;
    switch (kind) {
    case Kind::AwakeLine:
        where.asking = "/api/ws?stream=0"_ba;
        where.cookie = "auth_token=a-pass-1"_ba;
        break;
    case Kind::ConsoleLine:
        where.asking = "/api/ws?stream=0&legacy=0"_ba;
        where.cookie = "auth_token=a-pass-1"_ba;
        break;
    case Kind::JanusSignalling:
        where.asking = "/janus/ws"_ba;
        where.cookie = "auth_token=a-pass-1"_ba;
        break;
    case Kind::JetKvmSignalling:
        where.asking = "/webrtc/signaling/client"_ba;
        where.cookie = "authToken=a.pass-1"_ba;
        break;
    case Kind::LiveLink:
        where.asking = "/api/live/v2/1"_ba;
        break;
    case Kind::OpenBmcConsole:
        where.asking = "/kvm/0"_ba;
        where.authToken = "a-session-token"_ba;
        break;
    case Kind::ProxmoxConsole:
        where.asking = "/api2/json/nodes/one/qemu/100/vncwebsocket?port=5900&vncticket=a-ticket"_ba;
        where.authorization = "PVEAPIToken=root@pam!qkvm=a-secret"_ba;
        break;
    case Kind::AmiConsole:
        where.asking = "/kvm"_ba;
        where.cookie = "QSESSIONID=a1b2c3"_ba;
        break;
    case Kind::RfbScreen:
        break;
    }
    auto line = std::make_unique<DeviceLine>(where, limits);
    switch (kind) {
    case Kind::AwakeLine:
        line->hearShortTexts(512);
        break;
    case Kind::ConsoleLine:
        break;
    case Kind::JanusSignalling:
        line->receiveText("janus-protocol"_ba);
        break;
    case Kind::JetKvmSignalling:
        line->receiveText();
        break;
    case Kind::LiveLink:
        line->receiveLive();
        break;
    case Kind::OpenBmcConsole:
        line->receiveBytes();
        break;
    case Kind::ProxmoxConsole:
    case Kind::AmiConsole:
        line->receiveBytes("binary"_ba);
        break;
    case Kind::RfbScreen:
        line->receiveRaw();
        break;
    }
    return line;
}

// Git's openssl.exe, where Git is (its own build first: its s_server waits on its standard input and the line at once,
// where the other waits for its input alone): none where it is not.
QString gitsOpenSsl()
{
    for (const QString& one :
         {u"C:/Program Files/Git/usr/bin/openssl.exe"_s, u"C:/Program Files/Git/mingw64/bin/openssl.exe"_s}) {
        if (QFileInfo(one).isExecutable()) {
            return one;
        }
    }
    return {};
}

// How a case's line ends: by its own time, or from here while Windows still holds what judges its certificate.
enum class Ending { ByItsTime, Closed, Cut, Destroyed, DeletedLater };

// The certificates and ports of the program that quits while a line's certificate is judged, and of the one that quits
// while a certificate is asked for (programs of their own: this program, started again, quitMain).
constexpr const char* quitWhileALineWaits = "--quit-while-a-line-is-judged";
constexpr const char* quitWhileAnAskingWaits = "--quit-while-a-certificate-is-asked-for";

// That program: a far signer, a server showing a certificate that names it, and a line to it (or a certificate's
// asking); once Windows fetches the signer here, judging that line, the line is thrown away and the program quits.
// What it says, on its standard output: the longest the interface thread was held still from the opening to the quit,
// how long throwing the line away took, how long the application's end took (its post routines), and when (on the
// clock of the PC, in milliseconds) Windows was seen fetching: the program's whole end, to the end of its process, is
// timed from there by the program that started it (outside review of tls, L5). Its far signer and server are its own,
// never let go of: a thread Windows still holds may be theirs too, and that is not the program's to wait for.
int quitMain(int argc, char** argv, bool asking)
{
    auto application = std::make_unique<QCoreApplication>(argc, argv);
    if (!useWindowsOwnTls()) {
        std::puts("NOT_WINDOWS_TLS");
        return 3;
    }
    const Stalls& stalls = asking ? stallsOfAQuitAsking : stallsOfAQuitLine;
    auto* far = new SignerFarAway(stalls);            // NOLINT(cppcoreguidelines-owning-memory): as above
    auto* door = new TlsDoorElsewhere(tlsOf(stalls)); // NOLINT(cppcoreguidelines-owning-memory): as above
    auto still = std::make_unique<Stillness>();
    auto context = std::make_unique<QObject>();
    std::unique_ptr<DeviceLine> line;
    if (asking) {
        vnc::askCertificate(u"vncs://127.0.0.1:%1"_s.arg(QUrl(door->address()).port()), 30'000, context.get(),
                            [](const qkvm::base::Result<vnc::Certificate>&) {});
    } else {
        DeviceLine::Limits limits;
        limits.letInWithinMs = 30'000;
        line = lineOf(Kind::ConsoleLine, QUrl(door->address()), sha256Of(stalls), limits);
        line->open();
    }
    const bool fetched = QTest::qWaitFor([far] { return far->asked() > 0 && far->waitedOn() > 0; }, 60'000);
    const qint64 fetchedAt = QDateTime::currentMSecsSinceEpoch();
    const qint64 held = still->longestMs();
    QElapsedTimer clock;
    clock.start();
    line.reset();
    context.reset();
    const qint64 ending = clock.elapsed();
    still.reset();
    clock.restart();
    application.reset();
    const qint64 quitting = clock.elapsed();
    std::printf("FETCHED=%d STILL=%lld ENDING=%lld QUIT=%lld FETCHED_AT=%lld\n", fetched ? 1 : 0,
                static_cast<long long>(held), static_cast<long long>(ending), static_cast<long long>(quitting),
                static_cast<long long>(fetchedAt));
    std::fflush(stdout);
    static_cast<void>(far);
    static_cast<void>(door);
    return 0;
}

} // namespace

class TestDeviceLineTls : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(useWindowsOwnTls()); }

    void everyKindOfLineWhileWindowsJudgesItsCertificateLeavesTheInterfaceThreadFree()
    {
        // Each kind of line at once, to a server of its own whose certificate names a signer far away: Windows judges
        // each on the thread that speaks its TLS, fetches the signer, and waits. Meanwhile the interface thread goes
        // on; a line that waits ends by its own time, as a line that was not reached (a vncs:// one as one that did
        // not answer in time, as the web says of a VNC server's connection that timed out); and one closed, cut or
        // thrown away from here goes at once. (Windows' wait on each is seen: its fetch here waits.)
        struct Case {
            Case(const char* named, Kind of, const Stalls* by, Ending how)
                : what(named), kind(of), stalls(by), ending(how)
            {}
            const char* what;
            Kind kind;
            const Stalls* stalls;
            Ending ending;
            std::unique_ptr<SignerFarAway> far;
            std::unique_ptr<TlsDoorElsewhere> door;
            std::unique_ptr<DeviceLine> owned;
            QPointer<DeviceLine> line;
            qint64 endedAtMs = -1;
            qint64 goneAtMs = -1;
            qint64 endedFromHereAtMs = -1;
            std::optional<DeviceLine::End> why;
            int doorLetInAtTheEnd = -1;
            int waitedOnAtTheEnd = -1;
        };
        const int threadsBefore = lines::running();
        DeviceLine::Limits limits;
        limits.letInWithinMs = 4000;
        limits.goodbyeWithinMs = 1000;
        std::vector<Case> cases;
        cases.emplace_back("kvmd's awake line", Kind::AwakeLine, &stallsOfAnAwakeLine, Ending::ByItsTime);
        cases.emplace_back("kvmd's console line", Kind::ConsoleLine, &stallsOfAConsoleLine, Ending::Closed);
        cases.emplace_back("kvmd's Janus signalling", Kind::JanusSignalling, &stallsOfJanus, Ending::Cut);
        cases.emplace_back("a JetKVM's signalling", Kind::JetKvmSignalling, &stallsOfAJetKvm, Ending::Destroyed);
        cases.emplace_back("a qKVM server's live link", Kind::LiveLink, &stallsOfALiveLink, Ending::DeletedLater);
        cases.emplace_back("OpenBMC's console", Kind::OpenBmcConsole, &stallsOfOpenBmc, Ending::ByItsTime);
        cases.emplace_back("a Proxmox guest's console", Kind::ProxmoxConsole, &stallsOfAProxmoxGuest, Ending::Closed);
        cases.emplace_back("AMI's console", Kind::AmiConsole, &stallsOfAmi, Ending::Cut);
        cases.emplace_back("a vncs:// server's RFB", Kind::RfbScreen, &stallsOfAnRfbScreen, Ending::ByItsTime);
        for (Case& one : cases) {
            one.far = std::make_unique<SignerFarAway>(*one.stalls);
            QVERIFY2(one.far->isListening(), qPrintable(one.far->said()));
            one.door = std::make_unique<TlsDoorElsewhere>(tlsOf(*one.stalls));
            QVERIFY(one.door->speaksWindowsOwnTls());
        }
        QElapsedTimer clock;
        clock.start();
        const Stillness still;
        for (Case& one : cases) {
            one.owned = lineOf(one.kind, QUrl(one.door->address()), sha256Of(*one.stalls), limits);
            one.line = one.owned.get();
            Case* const it = &one;
            connect(one.line, &DeviceLine::ended, this, [it, &clock](DeviceLine::End why) {
                it->why = why;
                it->endedAtMs = clock.elapsed();
                it->doorLetInAtTheEnd = it->door->letIn();
                it->waitedOnAtTheEnd = it->far->waitedOn();
            });
            connect(one.line, &DeviceLine::gone, this, [it, &clock] { it->goneAtMs = clock.elapsed(); });
            one.line->open();
        }
        // Windows fetches each one's signer, and waits.
        const bool fetching = QTest::qWaitFor(
            [&cases] { return std::ranges::all_of(cases, [](const Case& one) { return one.far->waitedOn() > 0; }); },
            60'000);
        const qint64 fetchingAtMs = clock.elapsed();
        QVERIFY2(still.longestMs() < 1000,
                 qPrintable(u"The interface thread was held still for %1 ms while the lines were opened"_s.arg(
                     still.longestMs())));
        QVERIFY2(fetching, "Windows did not fetch every line's signer here");
        // Ended from here, every way there is, while Windows still holds each.
        for (Case& one : cases) {
            if (one.ending == Ending::ByItsTime) {
                continue;
            }
            QVERIFY2(one.far->waitedOn() > 0, one.what);
            QVERIFY2(one.why == std::nullopt, one.what);
            one.endedFromHereAtMs = clock.elapsed();
            switch (one.ending) {
            case Ending::Closed:
                QVERIFY(!one.line->close()); // (never let in: nothing to say goodbye on)
                break;
            case Ending::Cut:
                one.line->cut();
                break;
            case Ending::Destroyed:
                one.owned.reset();
                break;
            case Ending::DeletedLater:
                one.owned.release()->deleteLater();
                break;
            case Ending::ByItsTime:
                break;
            }
        }
        const bool over = QTest::qWaitFor(
            [&cases] {
                return std::ranges::all_of(cases, [](const Case& one) {
                    switch (one.ending) {
                    case Ending::ByItsTime:
                        return one.why.has_value();
                    case Ending::Closed:
                        return one.goneAtMs >= 0;
                    case Ending::DeletedLater:
                        return one.line.isNull();
                    case Ending::Cut:
                    case Ending::Destroyed:
                        return true;
                    }
                    return true;
                });
            },
            20'000);
        QTest::qWait(1500);
        const qint64 longest = still.longestMs();
        QStringList said;
        for (const Case& one : cases) {
            said.append(
                u"%1: ended %2 at %3 ms (from here at %4 ms, gone at %5 ms; the server had let in %6, the fetch "
                "here waited on %7); %8"_s.arg(QString::fromLatin1(one.what))
                    .arg(one.why ? QString::number(static_cast<int>(*one.why)) : u"never"_s)
                    .arg(one.endedAtMs)
                    .arg(one.endedFromHereAtMs)
                    .arg(one.goneAtMs)
                    .arg(one.doorLetInAtTheEnd)
                    .arg(one.waitedOnAtTheEnd)
                    .arg(one.far->said()));
        }
        qInfo("fetched for every line by %lld ms; the interface thread held at the most %lld ms\n%s",
              static_cast<long long>(fetchingAtMs), static_cast<long long>(longest), qPrintable(said.join(u"\n"_s)));
        QVERIFY2(longest < 1000, qPrintable(u"The interface thread was held still for %1 ms"_s.arg(longest)));
        QVERIFY(over);
        for (const Case& one : cases) {
            switch (one.ending) {
            case Ending::ByItsTime:
                // In its time, while Windows still judged it: the server had let nothing in, and the fetch waited.
                QVERIFY2(one.why ==
                             (one.kind == Kind::RfbScreen ? DeviceLine::End::Silent : DeviceLine::End::NotReached),
                         one.what);
                QVERIFY2(one.endedAtMs >= limits.letInWithinMs - 50 && one.endedAtMs < limits.letInWithinMs + 1000,
                         qPrintable(u"%1 ended at %2 ms"_s.arg(QString::fromLatin1(one.what)).arg(one.endedAtMs)));
                QVERIFY2(one.doorLetInAtTheEnd == 0, one.what);
                QVERIFY2(one.waitedOnAtTheEnd > 0, one.what);
                break;
            case Ending::Closed:
                // Gone a turn of the loop later, nothing said of an end.
                QVERIFY2(one.goneAtMs >= one.endedFromHereAtMs && one.goneAtMs < one.endedFromHereAtMs + 500, one.what);
                QVERIFY2(!one.why.has_value(), one.what);
                break;
            case Ending::Cut:
                QVERIFY2(!one.why.has_value() && one.goneAtMs < 0, one.what);
                break;
            case Ending::Destroyed:
            case Ending::DeletedLater:
                QVERIFY2(one.line.isNull(), one.what);
                break;
            }
        }
        // And none of their threads is left once the lines are gone and Windows lets go (what it waited for here
        // answers nothing any more).
        for (Case& one : cases) {
            one.owned.reset();
            one.far.reset();
        }
        QVERIFY2(QTest::qWaitFor([threadsBefore] { return lines::running() <= threadsBefore; }, 60'000),
                 qPrintable(u"%1 lines' threads were left"_s.arg(lines::running() - threadsBefore)));
    }

    void everyLinesThreadEndsWithItsLine()
    {
        // Lines in the clear, let in and not, ended every way there is: the thread each had is gone with it.
        const int threadsBefore = lines::running();
        QTcpServer nobody;
        QVERIFY(nobody.listen(QHostAddress::LocalHost));
        const quint16 there = nobody.serverPort();
        nobody.close();
        QTcpServer taking;
        QVERIFY(taking.listen(QHostAddress::LocalHost));
        QList<QTcpSocket*> taken;
        connect(&taking, &QTcpServer::newConnection, this, [&taking, &taken] {
            while (QTcpSocket* one = taking.nextPendingConnection()) {
                taken.append(one);
            }
        });
        DeviceLine::Limits limits;
        limits.letInWithinMs = 500;
        limits.goodbyeWithinMs = 300;
        std::vector<std::unique_ptr<DeviceLine>> made;
        for (int one = 0; one < 12; ++one) {
            DeviceLine::Where where;
            where.address = QUrl(u"http://127.0.0.1:%1"_s.arg(one % 2 == 0 ? there : taking.serverPort()));
            made.push_back(std::make_unique<DeviceLine>(where, limits));
            made.back()->receiveRaw();
            made.back()->open();
        }
        QVERIFY(QTest::qWaitFor([] { return lines::running() > 0; }, 5000));
        QTest::qWait(100);
        for (std::size_t one = 0; one < made.size(); ++one) {
            switch (one % 4) {
            case 0:
                static_cast<void>(made[one]->close());
                break;
            case 1:
                made[one]->cut();
                break;
            case 2:
                made[one].release()->deleteLater();
                break;
            default:
                break; // (by its time, or in: thrown away below)
            }
        }
        QTest::qWait(1500);
        made.clear();
        // And a VNC server's certificate asked for, of a server that shows one at once, and of nobody: each asking's
        // thread goes with it.
        const TlsDoorElsewhere door(
            {QStringLiteral(QKVM_TESTDATA "/tls/first.crt"), QStringLiteral(QKVM_TESTDATA "/tls/first.key")});
        QObject context;
        int answered = 0;
        bool shown = false;
        vnc::askCertificate(u"vncs://127.0.0.1:%1"_s.arg(QUrl(door.address()).port()), 5000, &context,
                            [&answered, &shown](const qkvm::base::Result<vnc::Certificate>& said) {
                                ++answered;
                                shown = said.ok();
                            });
        vnc::askCertificate(u"vncs://127.0.0.1:%1"_s.arg(there), 2000, &context,
                            [&answered](const qkvm::base::Result<vnc::Certificate>&) { ++answered; });
        QVERIFY(QTest::qWaitFor([&answered] { return answered == 2; }, 10'000));
        QVERIFY(shown);
        QVERIFY2(QTest::qWaitFor([threadsBefore] { return lines::running() <= threadsBefore; }, 10'000),
                 qPrintable(u"%1 lines' threads were left"_s.arg(lines::running() - threadsBefore)));
        qDeleteAll(taken);
    }

    void aLineWhoseCertificateIsJudgedAgainWhileWindowsFetchesEndsInItsTimeAndSaysNothingAfter()
    {
        // After the handshake Qt has Windows judge the certificate again: at the session ticket of TLS 1.3 that a
        // server of Windows' own sends right after it (measured: on every line). Where that judgement fetches the
        // signer anew and waits (here the first fetch is told at once that there is nothing, as by a place that
        // answers, and the next one waits), the line's socket's thread is held while the line is asked for, with a
        // secret: the interface thread goes on, the line ends in its time, and what it said and had not handed to
        // Windows by then never goes, then or once Windows lets that thread go. Nor is any of its threads left.
        //
        // It never runs on the build box, nor on any PC tried (outside review of tls, L4): Windows marks a place whose
        // first fetch was answered with nothing as answering nothing, and its judgement after the handshake fetches
        // nothing then (the skip below). Kept for a Windows that fetches again. What stands for it where the gate
        // runs: the line's own thread held past the limits in test_device_line (lines::askEach, lines::afterALook),
        // and the judgements after the handshake of an OpenSSL server, below.
        const int threadsBefore = lines::running();
        const auto& stalls = stallsOfAJudgementAgain;
        auto far = std::make_unique<SignerFarAway>(stalls, 1);
        QVERIFY2(far->isListening(), qPrintable(far->said()));
        const TlsDoorElsewhere door(tlsOf(stalls));
        DeviceLine::Limits limits;
        limits.letInWithinMs = 10'000;
        QElapsedTimer clock;
        clock.start();
        const Stillness still;
        auto line = lineOf(Kind::ProxmoxConsole, QUrl(door.address()), sha256Of(stalls), limits);
        std::optional<DeviceLine::End> why;
        qint64 endedAtMs = -1;
        qint64 heardAtTheEnd = -1;
        connect(line.get(), &DeviceLine::ended, this, [&](DeviceLine::End ended) {
            why = ended;
            endedAtMs = clock.elapsed();
            heardAtTheEnd = door.heard();
        });
        line->open();
        const bool again = QTest::qWaitFor(
            [&far, &why] { return (far->asked() >= 2 && far->waitedOn() > 0) || why.has_value(); }, 6000);
        const qint64 againAtMs = clock.elapsed();
        QVERIFY2(still.longestMs() < 1000,
                 qPrintable(
                     u"The interface thread was held still for %1 ms (%2)"_s.arg(still.longestMs()).arg(far->said())));
        if (!again || why.has_value() || door.letIn() == 0) {
            qInfo("%s; the server let in %d", qPrintable(far->said()), door.letIn());
            QSKIP("Windows did not judge the certificate again with a fetch of its own after the handshake here");
        }
        QVERIFY(QTest::qWaitFor([&why] { return why.has_value(); }, 5000));
        QCOMPARE(*why, DeviceLine::End::Silent); // (asked for, and never answered)
        QVERIFY2(endedAtMs >= limits.letInWithinMs - 50 && endedAtMs < limits.letInWithinMs + 1000,
                 QByteArray::number(endedAtMs).constData());
        QVERIFY2(far->waitedOn() > 0, qPrintable(far->said())); // (still held then)
        line.reset();
        // Windows lets go: what it waited for here answers nothing any more.
        const QString said = far->said();
        far.reset();
        QVERIFY2(QTest::qWaitFor([threadsBefore] { return lines::running() <= threadsBefore; }, 40'000),
                 "The line's thread did not end once Windows let it go");
        QTest::qWait(500);
        qInfo("fetched again at %lld ms, ended at %lld ms; the server heard %lld bytes by then, %lld in all; %s",
              static_cast<long long>(againAtMs), static_cast<long long>(endedAtMs),
              static_cast<long long>(heardAtTheEnd), static_cast<long long>(door.heard()), qPrintable(said));
        QCOMPARE(door.heard(), heardAtTheEnd);
        QVERIFY2(still.longestMs() < 1000, QByteArray::number(still.longestMs()).constData());
    }

    void aJudgementAfterTheHandshakeKeepsALineKnownByItsCertificateGoing_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QByteArray>("command");
        QTest::addColumn<QByteArray>("seen");
        // (s_server's own commands: "r" has TLS 1.2 go through its handshake again, asked for by the server; "k" sends
        // a key update of TLS 1.3. What its own words show of each, on its standard error with -state.)
        QTest::newRow("a renegotiation of TLS 1.2") << u"-tls1_2"_s << "r\n"_ba << "write finished"_ba;
        QTest::newRow("a key update of TLS 1.3") << u"-tls1_3"_s << "k\n"_ba << "key update"_ba;
    }

    void aJudgementAfterTheHandshakeKeepsALineKnownByItsCertificateGoing()
    {
        // (Outside review of tls, L4: the case above never runs where the gate runs.) A server of OpenSSL's (Git's
        // openssl.exe, its s_server, on this PC's own address) takes the line's TLS through its handshake again, or a
        // key update, mid-line: Qt has Windows judge the certificate again there (measured: after either), on the
        // line's socket's thread, and the driver's rule passes it for the certificate the device is known by, as at
        // the handshake. The line goes on both ways, and does not end. (What s_server cannot do is show another
        // certificate there, the one road of a judgement after the handshake that a test on this PC cannot make: the
        // same handler of Windows' objections ends the line for it as at the handshake.)
        QFETCH(QString, protocol);
        QFETCH(QByteArray, command);
        QFETCH(QByteArray, seen);
        const QString openssl = gitsOpenSsl();
        if (openssl.isEmpty()) {
            QSKIP("No openssl.exe of Git's on this PC");
        }
        quint16 port = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost));
            port = probe.serverPort();
        }
        const Tls first{QStringLiteral(QKVM_TESTDATA "/tls/first.crt"), QStringLiteral(QKVM_TESTDATA "/tls/first.key")};
        QProcess server;
        server.setProgram(openssl);
        server.setArguments({u"s_server"_s, u"-accept"_s, u"127.0.0.1:%1"_s.arg(port), u"-cert"_s,
                             first.certificateFile, u"-key"_s, first.keyFile, protocol, u"-state"_s});
        server.setProcessChannelMode(QProcess::MergedChannels);
        QByteArray said;
        connect(&server, &QProcess::readyRead, this, [&server, &said] { said += server.readAll(); });
        server.start();
        QVERIFY(server.waitForStarted(10'000));
        const auto stop = qScopeGuard([&server] {
            server.write("Q\n");
            if (!server.waitForFinished(3000)) {
                server.kill();
                server.waitForFinished(3000);
            }
        });
        QTRY_VERIFY2_WITH_TIMEOUT(said.contains("ACCEPT"), said.constData(), 10'000);
        const QList<QSslCertificate> chain = QSslCertificate::fromPath(first.certificateFile, QSsl::Pem);
        QVERIFY(!chain.isEmpty());
        DeviceLine::Where where;
        where.address = QUrl(u"https://127.0.0.1:%1"_s.arg(port));
        where.knownByCertificate = true;
        where.certificate = chain.first().digest(QCryptographicHash::Sha256);
        DeviceLine::Limits limits;
        limits.letInWithinMs = 10'000;
        DeviceLine line(where, limits);
        line.receiveRaw();
        QByteArray came;
        connect(&line, &DeviceLine::bytesCame, this, [&came](const QByteArray& bytes) { came += bytes; });
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY2_WITH_TIMEOUT(line.isIn() || !ended.isEmpty(), said.constData(), 10'000);
        QVERIFY2(ended.isEmpty(), said.constData());
        QVERIFY(line.sayBytes("before it\n"_ba));
        QTRY_VERIFY2_WITH_TIMEOUT(said.contains("before it"), said.constData(), 10'000);
        const qsizetype seenBefore = said.count(seen);
        // The server's command; once its own words show it done (the handshake again was answered by the line's TLS,
        // the key update sent), words of its own behind it, which reach the line through its TLS as it stands now.
        // (One line of its input at a time: what follows a command in the same read is thrown away.)
        server.write(command);
        QTRY_VERIFY2_WITH_TIMEOUT(said.count(seen) > seenBefore || !ended.isEmpty(), said.constData(), 10'000);
        server.write("after it\n");
        QTRY_VERIFY2_WITH_TIMEOUT(came.contains("after it") || !ended.isEmpty(), said.constData(), 10'000);
        QVERIFY2(ended.isEmpty(), said.constData());
        QVERIFY(line.sayBytes("and after\n"_ba));
        QTRY_VERIFY2_WITH_TIMEOUT(said.contains("and after"), said.constData(), 10'000);
        QTest::qWait(300);
        QVERIFY(ended.isEmpty());
        QVERIFY(line.isIn());
    }

    void quittingWhileALinesCertificateIsJudgedHoldsTheInterfaceThreadLessThanASecond()
    {
        // The program quits while Windows judges a line's certificate and waits for its signer: nothing of its end
        // waits for that judgement (the line's thread is left to the end of the process).
        quitAndLook(quitWhileALineWaits);
    }

    void quittingWhileACertificateIsAskedForHoldsTheInterfaceThreadLessThanASecond()
    {
        // The same of a vncs:// server's certificate asked for by the adding, changing or reviewing box.
        quitAndLook(quitWhileAnAskingWaits);
    }

private:
    static void quitAndLook(const char* how)
    {
        QProcess program;
        program.setProgram(QCoreApplication::applicationFilePath());
        program.setArguments({QString::fromLatin1(how)});
        program.setProcessChannelMode(QProcess::SeparateChannels);
        QElapsedTimer clock;
        clock.start();
        program.start();
        QVERIFY(program.waitForStarted(10'000));
        const bool ended = program.waitForFinished(120'000);
        const qint64 endedAt = QDateTime::currentMSecsSinceEpoch();
        const qint64 tookMs = clock.elapsed();
        const QByteArray said = program.readAllStandardOutput();
        const QByteArray errors = program.readAllStandardError();
        if (!ended) {
            program.kill();
            program.waitForFinished(5000);
        }
        qInfo("the program said %s (it ran %lld ms, exit %d)", said.trimmed().constData(),
              static_cast<long long>(tookMs), program.exitCode());
        QVERIFY2(ended, errors.right(2000).constData());
        QCOMPARE(program.exitStatus(), QProcess::NormalExit);
        QCOMPARE(program.exitCode(), 0);
        const auto value = [&said](const char* name) {
            const QByteArray key = QByteArray(name) + '=';
            const qsizetype at = said.indexOf(key);
            return at < 0 ? qint64{-1} : said.mid(at + key.size()).split(' ').value(0).trimmed().toLongLong();
        };
        QVERIFY2(value("FETCHED") == 1, "Windows did not fetch the signer there for that program");
        QVERIFY2(value("STILL") >= 0 && value("STILL") < 1000, said.constData());
        QVERIFY2(value("ENDING") >= 0 && value("ENDING") < 1000, said.constData());
        QVERIFY2(value("QUIT") >= 0 && value("QUIT") < 1000, said.constData());
        // And the whole program's end, from Windows seen fetching to its process's end, the statics that go after the
        // application and the runtime's own end among it, as this program saw it end: under a second as well.
        const qint64 fetchedAt = value("FETCHED_AT");
        QVERIFY2(fetchedAt > 0 && endedAt - fetchedAt < 1000,
                 qPrintable(u"the program ended %1 ms after Windows was seen fetching (%2)"_s.arg(endedAt - fetchedAt)
                                .arg(QString::fromLatin1(said.trimmed()))));
    }
};

int main(int argc, char** argv)
{
    if (argc == 2 && qstrcmp(argv[1], quitWhileALineWaits) == 0) {
        return quitMain(argc, argv, false);
    }
    if (argc == 2 && qstrcmp(argv[1], quitWhileAnAskingWaits) == 0) {
        return quitMain(argc, argv, true);
    }
    QCoreApplication application(argc, argv);
    TestDeviceLineTls test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_device_line_tls.moc"
