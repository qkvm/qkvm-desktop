#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <variant>

#include <QByteArray>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include "stand_in.h"

namespace qkvm::core::testing {

// A Proxmox VE by hand, as the web's stand-in answers (../app/tests/fake_proxmox.py of web 0.3.1), which was typed
// from what a real Proxmox VE 9.2 answered: as much of its API as this program asks of one, over TLS on a StandIn.
//
// * Every question below /api2/json is signed in with the API token in the Authorization header
//   (`PVEAPIToken=<id>=<secret>`); without it, or with another, 401, held back a while where `refuseAfterMs` says
//   (a real one holds every 401 three seconds).
// * What the token may do is said per guest (VM.Audit to see it, VM.Console, VM.PowerMgmt); a refusal is 403
//   "Permission check failed (/vms/<id>, VM.Audit)"; a guest asked on a node it is not on is 500 "Configuration file
//   'nodes/<node>/qemu-server/<id>.conf' does not exist", and a node there is none of "hostname lookup '<node>'
//   failed". Every refusal is said as Proxmox's API server says one: its words in the status line, a body of
//   {"data":null} (and `errors` for a parameter it did not take); or, `wordsInTheBody`, as its other format does, in
//   the body's `message` (review c9b-2, H1).
// * GET /version, /cluster/resources?type=vm and a guest's status/current, as Proxmox answers them.
// * Its virtual media (step 7), as the web's stand-in has them from Proxmox's own source (qemu-server's Qemu.pm,
//   pve-storage's Storage.pm): GET /nodes/<node>/storage?content=iso lists the storages of that node that hold ISO
//   images and that the token may see (Datastore.Audit; one it may not is not listed at all), GET .../storage/<id>/
//   content?content=iso the images on one (403 where it may not, 500 where it is not online); a guest's settings
//   (GET .../config) hold its drives and its boot order, what is pending merged in, and `digest`, the SHA-1 of the
//   file; PUT (answered when done) and POST (with a task) .../config change them: a disc in a CD-ROM drive needs
//   VM.Config.CDROM, the boot order VM.Config.Disk, an image on a storage Datastore.Audit there, and a `digest` that is
//   not the file's is refused 500 in `digestSays`'s words. A running guest has a new disc at
//   once, and a new drive or boot order from its next start. download-url and upload are not served.
// * What it was asked is kept for the tests: every signed question, every 401 to a token, every question that carried
//   none, and every address that held the secret. And every change of a guest's settings it took (`changes`).
class ProxmoxByHand : public QObject {
public:
    struct Guest {
        QString name = QStringLiteral("web");
        QString node = QStringLiteral("pve1");
        QString status = QStringLiteral("running"); // "running" or "stopped"
        bool paused = false;
        int cpus = 2;
        qint64 maxmem = qint64{2} * 1024 * 1024 * 1024;
        QString uuid = fresh();
        bool maySee = true; // the token has VM.Audit on it
        bool mayConsole = true;
        bool mayPower = true;
        qint64 uptime = 1234;         // seconds its process has run, while it runs
        bool ignoresShutdown = false; // nothing in it answers ACPI: asked to shut down, it goes on running
        bool ha = false; // under Proxmox's high availability: started and stopped by it, powerTakesMs after the task
        // Its drives and its boot order, as its settings file has them; what is pending, for its next start; and what
        // the token may change of them (VM.Config.CDROM, VM.Config.Disk).
        QMap<QString, QString> config{
            {QStringLiteral("boot"), QStringLiteral("order=scsi0;ide2;net0")},
            {QStringLiteral("ide2"), QStringLiteral("none,media=cdrom")},
            {QStringLiteral("scsi0"), QStringLiteral("local-lvm:vm-disk-0,size=8G")},
            {QStringLiteral("net0"), QStringLiteral("virtio=BC:24:11:5E:1A:01,bridge=vmbr0")}};
        QMap<QString, QString> pending{};
        bool mayCdrom = true;
        bool mayDisk = true;
        // The boot order its running process was started with (empty: the one in its settings, as it ran from the
        // first); and what each start and reset began from: "ide2 local:iso/x.iso", "scsi0" (the web's).
        QString runningOrder{};
        QStringList booted{};
    };
    // A storage of Proxmox's, with the ISO images on it (name: bytes), on every node unless `nodes` names them.
    struct Storage {
        QMap<QString, qint64> images{};
        bool maySee = true; // the token has Datastore.Audit on it
        bool active = true;
        bool online = true;    // its images can be listed (false: it went away after it was listed as active)
        int takesMs = 0;       // its images are listed only after so long (a slow share)
        bool trickles = false; // ...or a byte at a time, a quarter second apart, never to the end (one that hangs)
        QString content = QStringLiteral("iso,vztmpl,backup");
        QStringList nodes{};
    };
    // A task Proxmox wrote into its node's log: its name (UPID), its kind, its guest, and how it stands ("running",
    // "OK", or why not).
    struct Task {
        QString upid;
        QString type;
        qint64 vmid = 0;
        QString status;
    };

    explicit ProxmoxByHand(const Tls& tls, quint16 port = 0)
    {
        m_clock.start();
        stand = std::make_unique<StandIn>(
            [this](const Asked& asked) {
                Answered answer = answered(asked);
                if (answering && asked.headers.contains("authorization")) {
                    const QString below = QUrl(QString::fromLatin1(asked.path)).path().mid(10);
                    answering(QString::fromLatin1(asked.method) + u' ' + below, answer);
                }
                return answer;
            },
            tls, port);
        stand->handsLinesOver = false;
        stand->keepsLines = true;
        guests.insert(100, Guest{});
    }

    // What it is. (Made up, as the web's: no lab's own token is in any file of the repository.)
    QString tokenId = QStringLiteral("wall@pve!qkvm");
    QString secret = QStringLiteral("0b9e4f2a-5c1d-4e8f-9a3b-7d6c5e4f3a2b");
    QStringList nodes{QStringLiteral("pve1")};
    QMap<qint64, Guest> guests;
    QList<qint64> containers; // containers it has besides (listed, never asked about)
    QMap<QString, Storage> storages{
        {QStringLiteral("local"),
         Storage{{{QStringLiteral("debian-13.iso"), 700'000'000}, {QStringLiteral("alpine-3.21.iso"), 62'000'000}}}}};
    // Every change of a guest's settings it took: "<vmid> <key>=<value> ..." (the digest left out), in its order.
    QStringList changes;
    // What somebody does on Proxmox's own page a moment after a guest's settings were read, or after the images of a
    // storage were listed (none: nothing).
    std::function<void()> afterConfigRead;
    std::function<void()> afterImagesListed;
    // A change of a guest's settings carried out and answered with this status instead (596, 597, a proxy's 502 to
    // 504); 0: as Proxmox answers it. And so many changes carried out, and the line then closed without a word.
    int configAnswer = 0;
    int configHangsUp = 0;
    // While it is set, every change of a guest's settings is refused 500 in these words, and nothing is changed (the
    // settings' lock held by a task that runs: "can't lock file ... got timeout").
    QString changeRefused;
    QString version = QStringLiteral("9.2.21");
    // Its refusals' words in the body's `message` (its extjs format), and not in the status line (its json format).
    bool wordsInTheBody = false;
    // What it says of a digest that is not its settings file's: the words of the web's stand-in (an older Proxmox's),
    // or a Proxmox VE 9's ("detected modified configuration - file changed by other user? Try again.", qemu-server's
    // assert_if_modified).
    QString digestSays = QStringLiteral("checksum mismatch (file change by other user?)");
    int refuseAfterMs = 0;       // a 401 is held back this long (a real one: 3000)
    int refuseSignInAfterMs = 0; // ...and one to a sign-in (GET /version) this long, where it is not 0
    // Somebody else answers what carries no token from now on, by this status (a 404 of another HTTPS service at its
    // address), where it is not 0: what asks whether a Proxmox answers, before the token goes, is told otherwise.
    int unsignedStatus = 0;
    // ...or, while it is set, every question that carries no token is closed without a word (nothing back at all).
    bool unsignedHangsUp = false;
    int versionTakesMs = 0; // its version (a sign-in) is answered only after this long
    // A way to misbehave: "deep" (its version nested beyond reading), "not_proxmox" (a version that is none), "echo"
    // (a status and a console answered 503 with the request's Authorization said back), "echo_spelled" (the same, the
    // secret without its dashes), "odd_status" (a status of a kind of its own), "null_status", "huge_list",
    // "refuses_after" (its version to the token, and 401 to everything else), "odd_images" and "odd_storages" (lists
    // of what is no image, and no storage, beside what is).
    QString hostile;
    // From the next question on, until this many more: every question below the API answered 500 (one of its moments).
    int unwellFor = 0;
    // The token's secret once a version was answered (the token made anew on Proxmox just then).
    QString secretAfterVersion;
    // Asked of each question that carries the token ("POST /nodes/pve1/qemu/100/status/reset"): where it says yes, the
    // question is refused 401 (none: none is).
    std::function<bool(const QString& said)> refuses;
    // Shown every answer to a question that carries the token, as it was made, right before it goes ("GET
    // /nodes/pve1/qemu/100/config"; none: none is): what it makes of the answer is what goes (held a while, `afterMs`;
    // the line closed without a word, `hangUp`; said to be longer than it is, `moreToCome`).
    std::function<void(const QString& said, Answered& answer)> answering;
    // So many readings of a guest's settings from now on are answered 500 (one of its moments).
    int configFails = 0;
    // Its power commands (fake_proxmox.py): the guest seen in its new state so long after its task ended; the task
    // running so long; ending with these words instead of "OK" (the guest stays as it was, but for "WARNINGS: n"); the
    // token may not read how a task stands; a command carried out and answered with this status instead (596, 597, a
    // proxy's 502 to 504), or not carried out (595); and every read of a guest's state after a command answered only
    // after so long.
    int powerTakesMs = 0;
    int taskTakesMs = 0;
    QString taskFails;
    bool tasksHidden = false;
    int powerAnswer = 0;
    int statusTakesMsAfterACommand = 0;
    // Every read of a guest's state answered only after so long (a busy node); and of its settings, and of the list
    // of the cluster's guests.
    int statusTakesMs = 0;
    int configTakesMs = 0;
    int resourcesTakesMs = 0;
    // ...or a byte every quarter second, never to its end; and a command answered with what is no task's name.
    bool statusTricklesAfterACommand = false;
    bool answersNoTask = false;
    // What happens on Proxmox right after a power command was taken, and right before a guest's state is read (none:
    // nothing).
    std::function<void()> afterCommand;
    std::function<void()> beforeStatusRead;
    QList<Task> tasks;
    // Its consoles (fake_proxmox.py's vncproxy): each ticket it handed out, for one connection within `windowMs` of
    // being made (after that, or used a second time, the upgrade gets no answer at all), refused after `ticketLifeMs`,
    // with the task it wrote; the upgrade itself is ProxmoxConsoles' (proxmox_console_by_hand.h). Misbehaving
    // (`hostile`): "bad_ticket", "bad_port", "no_one_time" (a ticket and no password), "silent_upgrade", "upgrade_500".
    struct Ticket {
        qint64 vmid = 0;
        QString port;
        QString password;
        QString ticket;
        qint64 madeMs = 0;
        qsizetype task = 0;
        bool used = false;
    };
    QList<Ticket> tickets;
    int windowMs = 10'000;
    int ticketLifeMs = 40'000;
    // How many consoles were asked for: what the guest's owner sees in the node's task log.
    int consolesOpened() const
    {
        return static_cast<int>(std::ranges::count(tasks, QStringLiteral("vncproxy"), &Task::type));
    }
    qint64 sinceMs() const { return m_clock.elapsed(); }

    // What happened.
    QStringList requests;      // "GET /path" of every question that carried the token, below the API
    int refused = 0;           // questions that carried a token, answered 401
    QStringList refusedPaths;  // ...and what each asked for ("GET /version": a sign-in)
    QStringList unsignedAsked; // what was asked below the API with no token at all (answered 401 too)
    QStringList inTheClear;    // any question whose address held the token's secret
    std::unique_ptr<StandIn> stand;

    QUrl address() const { return QUrl(stand->address()); }
    int asked(const QString& request) const { return static_cast<int>(requests.count(request)); }
    QString authorization() const { return QStringLiteral("PVEAPIToken=") + tokenId + u'=' + secret; }

    static QString fresh() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

private:
    static QByteArray written(const QJsonValue& said)
    {
        return said.isArray() ? QJsonDocument(said.toArray()).toJson(QJsonDocument::Compact)
                              : QJsonDocument(said.toObject()).toJson(QJsonDocument::Compact);
    }
    static Answered data(const QJsonValue& said)
    {
        Answered yes(200, written(QJsonObject{{QStringLiteral("data"), said}}));
        yes.reason = "OK";
        return yes;
    }
    // A refusal as Proxmox's API server writes one (pve-http-server, format json): its words in the status line (the
    // first line of them), and a body of {"data":null}; only its other format (extjs) puts a `message` in the body,
    // which `wordsInTheBody` has it do instead (review c9b-2, H1).
    Answered refusal(int status, const QString& message) const
    {
        if (wordsInTheBody) {
            Answered no(status, written(QJsonObject{{QStringLiteral("data"), QJsonValue::Null},
                                                    {QStringLiteral("message"), message + u'\n'}}));
            no.reason = status == 403 ? "Forbidden" : "Internal Server Error";
            return no;
        }
        Answered no(status, R"({"data":null})");
        no.reason = message.section(u'\n', 0, 0).toUtf8();
        return no;
    }
    // A parameter it did not take: 400 "Parameter verification failed.", and why, by the parameter, in `errors`.
    static Answered parameterRefused(const QString& parameter, const QString& why)
    {
        Answered no(400, written(QJsonObject{{QStringLiteral("data"), QJsonValue::Null},
                                             {QStringLiteral("errors"), QJsonObject{{parameter, why}}}}));
        no.reason = "Parameter verification failed.";
        return no;
    }
    // An answer of no body worth the name, as Proxmox hands one on from its HTTP library (595 to 597) or a proxy
    // before it answers (502 to 504): the words of its status line, as they say them.
    static Answered bare(int status)
    {
        Answered said(status, R"({"data": null})");
        said.reason = status == 502   ? "Bad Gateway"
                      : status == 503 ? "Service Unavailable"
                      : status == 504 ? "Gateway Timeout"
                      : status == 595 ? "Connection refused"
                      : status == 596 ? "Connection timed out"
                      : status == 597 ? "Connection reset by peer"
                                      : "";
        return said;
    }

    // The guest at a node, where the token may do `need` there; else Proxmox's refusal.
    std::variant<Guest*, Answered> guestAt(const QString& node, qint64 vmid, const QString& need)
    {
        const auto found = guests.find(vmid);
        const bool allowed = found != guests.end() && (need == QStringLiteral("VM.Audit")     ? found->maySee
                                                       : need == QStringLiteral("VM.Console") ? found->mayConsole
                                                                                              : found->mayPower);
        if (!allowed) {
            return refusal(403, QStringLiteral("Permission check failed (/vms/%1, %2)").arg(vmid).arg(need));
        }
        if (!nodes.contains(node)) {
            return refusal(500, QStringLiteral("hostname lookup '%1' failed - failed to get address info for: %1: Name "
                                               "or service not known")
                                    .arg(node));
        }
        if (found->node != node) {
            return refusal(
                500,
                QStringLiteral("Configuration file 'nodes/%1/qemu-server/%2.conf' does not exist").arg(node).arg(vmid));
        }
        return &found.value();
    }

    Answered answered(const Asked& asked)
    {
        const QUrl url(QString::fromLatin1(asked.path));
        const QString path = url.path();
        if (asked.path.contains(secret.toLatin1())) {
            inTheClear.append(path);
        }
        if (path == QStringLiteral("/")) {
            return {200, "<!doctype html><title>Proxmox VE (stand-in)</title>", "text/html"};
        }
        if (!path.startsWith(QStringLiteral("/api2/json"))) {
            return {404, {}};
        }
        const QString below = path.mid(10);
        const QString said = QString::fromLatin1(asked.method) + u' ' + below;
        // A question that carries no token at all (what asks whether a Proxmox answers, before the token goes): refused
        // as a real one refuses it, and counted apart from the questions that carried a token.
        if (!asked.headers.contains("authorization")) {
            unsignedAsked.append(said);
            if (unsignedHangsUp) {
                Answered gone;
                gone.hangUp = true;
                return gone;
            }
            if (unsignedStatus != 0) {
                return {unsignedStatus, "Not Found", "text/plain"};
            }
            Answered no(401, {});
            no.reason = "No ticket";
            no.afterMs = refuseAfterMs;
            return no;
        }
        // A question it refuses 401 whatever token it carries, where `refuses` says so of it (the token taken back for
        // a moment, Proxmox restarting): counted as a refusal of the token.
        if (refuses && refuses(said)) {
            ++refused;
            refusedPaths.append(said);
            Answered no(401, {});
            no.reason = "authentication failure";
            return no;
        }
        // ("refuses_after": it takes the token for its version and refuses it for everything else.)
        if (QString::fromLatin1(asked.headers.value("authorization")) != authorization() ||
            (hostile == QStringLiteral("refuses_after") && below != QStringLiteral("/version"))) {
            ++refused;
            refusedPaths.append(said);
            Answered no(401, {});
            no.reason = "authentication failure";
            no.afterMs =
                below == QStringLiteral("/version") && refuseSignInAfterMs > 0 ? refuseSignInAfterMs : refuseAfterMs;
            return no;
        }
        requests.append(said);
        if (unwellFor > 0) {
            --unwellFor;
            return refusal(500, QStringLiteral("unable to read file: busy"));
        }
        if (hostile.startsWith(QStringLiteral("echo")) &&
            (below.endsWith(QStringLiteral("/status/current")) || below.endsWith(QStringLiteral("/vncproxy")))) {
            QString back = QString::fromLatin1(asked.headers.value("authorization"));
            if (hostile == QStringLiteral("echo_spelled")) {
                back.remove(u'-');
            }
            return refusal(503, QStringLiteral("upstream failed; the request had Authorization: ") + back);
        }
        if (below == QStringLiteral("/version")) {
            return versionSaid();
        }
        if (below == QStringLiteral("/cluster/resources")) {
            return resources();
        }
        const QStringList steps = below.split(u'/', Qt::SkipEmptyParts);
        // nodes/<node>/qemu/<vmid>/<...>
        if (steps.size() >= 5 && steps.at(0) == QStringLiteral("nodes") && steps.at(2) == QStringLiteral("qemu")) {
            const QString& node = steps.at(1);
            const qint64 vmid = steps.at(3).toLongLong();
            const QString rest = steps.mid(4).join(u'/');
            if (asked.method == "GET" && rest == QStringLiteral("status/current")) {
                return current(node, vmid);
            }
            if (asked.method == "GET" && rest == QStringLiteral("config")) {
                Answered settings = settingsOf(node, vmid);
                if (afterConfigRead) {
                    afterConfigRead();
                }
                return settings;
            }
            if ((asked.method == "PUT" || asked.method == "POST") && rest == QStringLiteral("config")) {
                return changeConfig(node, vmid, asked.body, asked.method == "POST");
            }
            if (asked.method == "POST" && rest.startsWith(QStringLiteral("status/"))) {
                return power(node, vmid, rest.mid(7));
            }
            if (asked.method == "POST" && rest == QStringLiteral("vncproxy")) {
                return vncproxy(node, vmid, asked.body);
            }
        }
        // nodes/<node>/storage, nodes/<node>/storage/<storage>/content
        if (steps.size() == 3 && steps.at(0) == QStringLiteral("nodes") && steps.at(2) == QStringLiteral("storage") &&
            asked.method == "GET") {
            return storagesAt(steps.at(1), QUrlQuery(url).queryItemValue(QStringLiteral("content")));
        }
        if (steps.size() == 5 && steps.at(0) == QStringLiteral("nodes") && steps.at(2) == QStringLiteral("storage") &&
            steps.at(4) == QStringLiteral("content") && asked.method == "GET") {
            Answered images = imagesOn(steps.at(1), steps.at(3));
            if (afterImagesListed) {
                afterImagesListed();
            }
            return images;
        }
        // nodes/<node>/tasks/<upid>/status
        if (steps.size() == 5 && steps.at(0) == QStringLiteral("nodes") && steps.at(2) == QStringLiteral("tasks") &&
            steps.at(4) == QStringLiteral("status") && asked.method == "GET") {
            return taskStatus(steps.at(1), steps.at(3));
        }
        return refusal(501, QStringLiteral("Method '%1' not implemented").arg(said));
    }

    QString madeTask(const QString& kind, qint64 vmid, const QString& status)
    {
        const QString upid = QStringLiteral("UPID:%1:0000ABCD:%2:6A2B3C4D:%3:%4:%5:")
                                 .arg(nodes.value(0))
                                 .arg(tasks.size(), 8, 16, QChar(u'0'))
                                 .arg(kind)
                                 .arg(vmid)
                                 .arg(tokenId);
        tasks.append(Task{upid, kind, vmid, status});
        return upid;
    }

public:
    // What a start (or a reset) of the guest begins from, by a boot order: the first of its devices that holds
    // something to start from; an empty CD-ROM drive is passed over, as SeaBIOS and OVMF pass it over (the web's
    // began_from).
    static QString beganFrom(const Guest& guest, const QString& order)
    {
        for (const QString& device : QString(order).remove(QStringLiteral("order=")).split(u';')) {
            const QString said = guest.config.value(device);
            if (said.isEmpty()) {
                continue;
            }
            if (said.contains(QStringLiteral(",media=cdrom"))) {
                const QString disc = said.section(u',', 0, 0);
                if (disc != QStringLiteral("none")) {
                    return device + u' ' + disc;
                }
                continue;
            }
            return device;
        }
        return QStringLiteral("nothing");
    }
    static QString runningOrderOf(const Guest& guest)
    {
        return guest.runningOrder.isEmpty() ? guest.config.value(QStringLiteral("boot")) : guest.runningOrder;
    }

private:
    void become(qint64 vmid, const QString& status)
    {
        if (!guests.contains(vmid)) {
            return;
        }
        Guest& guest = guests[vmid];
        if (status == QStringLiteral("running") && guest.status != QStringLiteral("running")) {
            // A start by Proxmox: a new process, which takes what was pending (a drive added, the boot order), and
            // begins from its boot order.
            guest.uptime = 1;
            guest.config.insert(guest.pending);
            guest.pending.clear();
            guest.runningOrder = guest.config.value(QStringLiteral("boot"));
            guest.booted.append(beganFrom(guest, guest.runningOrder));
        }
        guest.status = status;
    }

    Answered power(const QString& node, qint64 vmid, const QString& verb)
    {
        const std::variant<Guest*, Answered> at = guestAt(node, vmid, QStringLiteral("VM.PowerMgmt"));
        if (std::holds_alternative<Answered>(at)) {
            return std::get<Answered>(at);
        }
        static const QStringList verbs{QStringLiteral("start"), QStringLiteral("stop"),   QStringLiteral("shutdown"),
                                       QStringLiteral("reset"), QStringLiteral("reboot"), QStringLiteral("suspend"),
                                       QStringLiteral("resume")};
        if (!verbs.contains(verb)) {
            return refusal(501, QStringLiteral("Method 'POST status/%1' not implemented").arg(verb));
        }
        if (powerAnswer == 595) {
            return bare(595); // (the node could not be connected to: nothing reached it)
        }
        m_commanded = true;
        const Guest& guest = *std::get<Guest*>(at);
        const bool handed = guest.ha && (verb == QStringLiteral("start") || verb == QStringLiteral("stop") ||
                                         verb == QStringLiteral("shutdown"));
        const QString kind =
            handed ? (verb == QStringLiteral("start") ? QStringLiteral("hastart") : QStringLiteral("hastop"))
                   : QStringLiteral("qm") + verb;
        const bool runs = taskTakesMs > 0 && !handed;
        const QString status = runs                              ? QStringLiteral("running")
                               : !taskFails.isEmpty() && !handed ? taskFails
                                                                 : QStringLiteral("OK");
        const QString upid = madeTask(kind, vmid, status);
        const QString becomes = verb == QStringLiteral("start")  ? QStringLiteral("running")
                                : verb == QStringLiteral("stop") ? QStringLiteral("stopped")
                                : verb == QStringLiteral("shutdown")
                                    ? (guest.ignoresShutdown ? QStringLiteral("running") : QStringLiteral("stopped"))
                                    : QString();
        const qsizetype index = tasks.size() - 1;
        const auto carriedOut = [this, index, vmid, becomes, verb] {
            const QString& ended = tasks.at(index).status;
            if (ended != QStringLiteral("OK") && !ended.startsWith(QStringLiteral("WARNINGS: "))) {
                return;
            }
            if (verb == QStringLiteral("reset") && guests.value(vmid).status == QStringLiteral("running")) {
                // (The same process begins again: what is pending stays pending, and it begins from the order it
                // started with.)
                Guest& guest = guests[vmid];
                guest.booted.append(beganFrom(guest, runningOrderOf(guest)));
            }
            if (becomes.isEmpty()) {
                return;
            }
            if (powerTakesMs > 0) {
                QTimer::singleShot(powerTakesMs, this, [this, vmid, becomes] { become(vmid, becomes); });
            } else {
                become(vmid, becomes);
            }
        };
        if (runs) {
            QTimer::singleShot(taskTakesMs, this, [this, index, carriedOut] {
                tasks[index].status = taskFails.isEmpty() ? QStringLiteral("OK") : taskFails;
                carriedOut();
            });
        } else {
            carriedOut();
        }
        if (afterCommand) {
            afterCommand();
        }
        if (powerAnswer != 0) {
            return bare(powerAnswer);
        }
        if (answersNoTask) {
            return data(QStringLiteral("started"));
        }
        return data(upid);
    }

    Answered vncproxy(const QString& node, qint64 vmid, const QByteArray& form)
    {
        const std::variant<Guest*, Answered> at = guestAt(node, vmid, QStringLiteral("VM.Console"));
        if (std::holds_alternative<Answered>(at)) {
            return std::get<Answered>(at);
        }
        if (!form.contains("websocket=1")) {
            return refusal(400, QStringLiteral("only the websocket way is spoken here"));
        }
        static const QString letters = QStringLiteral("abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789%<@+&");
        QString password;
        for (int one = 0; one < 8; ++one) {
            password += letters.at(static_cast<qsizetype>(QRandomGenerator::global()->bounded(letters.size())));
        }
        QByteArray signature(256, Qt::Uninitialized);
        for (char& one : signature) {
            one = static_cast<char>(QRandomGenerator::global()->bounded(256));
        }
        Ticket made;
        made.vmid = vmid;
        made.port = QString::number(5900 + tickets.size() % 100);
        made.password = password;
        made.ticket = password + QStringLiteral(":PVEVNC:6A2B3C4D::") + QString::fromLatin1(signature.toBase64());
        made.madeMs = m_clock.elapsed();
        made.task = tasks.size();
        madeTask(QStringLiteral("vncproxy"), vmid, QStringLiteral("running"));
        tickets.append(made);
        QJsonObject said{{QStringLiteral("ticket"), made.ticket},
                         {QStringLiteral("port"), made.port},
                         {QStringLiteral("password"), made.password},
                         {QStringLiteral("user"), tokenId},
                         {QStringLiteral("cert"), QStringLiteral("-----BEGIN CERTIFICATE-----\n...")},
                         {QStringLiteral("upid"), tasks.last().upid}};
        if (hostile == QStringLiteral("bad_ticket")) {
            said.insert(QStringLiteral("ticket"), QString(5000, u'x'));
        } else if (hostile == QStringLiteral("bad_port")) {
            said.insert(QStringLiteral("port"), QStringLiteral("99999"));
        } else if (hostile == QStringLiteral("no_one_time")) {
            said.remove(QStringLiteral("password"));
        }
        return data(said);
    }

    Answered taskStatus(const QString& node, const QString& upid)
    {
        if (tasksHidden) {
            return refusal(403, QStringLiteral("Permission check failed (/nodes/%1, Sys.Audit)").arg(node));
        }
        const auto found = std::ranges::find(tasks, upid, &Task::upid);
        if (found == tasks.end() || !nodes.contains(node)) {
            return refusal(500, QStringLiteral("no such task '%1'").arg(upid));
        }
        QJsonObject said{{QStringLiteral("upid"), upid},
                         {QStringLiteral("node"), node},
                         {QStringLiteral("type"), found->type},
                         {QStringLiteral("id"), QString::number(found->vmid)}};
        if (found->status == QStringLiteral("running")) {
            said.insert(QStringLiteral("status"), QStringLiteral("running"));
        } else {
            said.insert(QStringLiteral("status"), QStringLiteral("stopped"));
            said.insert(QStringLiteral("exitstatus"), found->status);
        }
        return data(said);
    }

    Answered settingsOf(const QString& node, qint64 vmid)
    {
        if (configFails > 0) {
            --configFails;
            return refusal(500, QStringLiteral("unable to read file: busy"));
        }
        const std::variant<Guest*, Answered> at = guestAt(node, vmid, QStringLiteral("VM.Audit"));
        if (std::holds_alternative<Answered>(at)) {
            return std::get<Answered>(at);
        }
        const Guest& guest = *std::get<Guest*>(at);
        QJsonObject said{{QStringLiteral("name"), guest.name},
                         {QStringLiteral("cores"), guest.cpus},
                         {QStringLiteral("memory"), QString::number(guest.maxmem / (qint64{1024} * 1024))},
                         {QStringLiteral("digest"), digestOf(guest)}};
        // (Its drives and its boot order, with what is pending merged in, as Proxmox says them.)
        for (auto one = guest.config.cbegin(); one != guest.config.cend(); ++one) {
            said.insert(one.key(), one.value());
        }
        for (auto one = guest.pending.cbegin(); one != guest.pending.cend(); ++one) {
            said.insert(one.key(), one.value());
        }
        // (A guest made before Proxmox gave each one a UUID has none.)
        if (!guest.uuid.isEmpty()) {
            said.insert(QStringLiteral("smbios1"), QStringLiteral("uuid=") + guest.uuid);
        }
        Answered settings = data(said);
        settings.afterMs = configTakesMs;
        return settings;
    }

public:
    // The SHA-1 of the guest's settings file, as Proxmox says it with them: anything of it changed changes it.
    static QString digestOf(const Guest& guest)
    {
        QJsonObject config;
        for (auto one = guest.config.cbegin(); one != guest.config.cend(); ++one) {
            config.insert(one.key(), one.value());
        }
        QJsonObject pending;
        for (auto one = guest.pending.cbegin(); one != guest.pending.cend(); ++one) {
            pending.insert(one.key(), one.value());
        }
        const QByteArray file = QJsonDocument(QJsonArray{guest.uuid, guest.name, config, pending}).toJson();
        return QString::fromLatin1(QCryptographicHash::hash(file, QCryptographicHash::Sha1).toHex());
    }

private:
    // PUT (answered when done) and POST (answered with a task) of a guest's settings: its drives and its boot order,
    // each checked as Proxmox checks it.
    Answered changeConfig(const QString& node, qint64 vmid, const QByteArray& form, bool asATask)
    {
        const std::variant<Guest*, Answered> at = guestAt(node, vmid, QStringLiteral("VM.Audit"));
        if (std::holds_alternative<Answered>(at)) {
            return std::get<Answered>(at);
        }
        Guest& guest = *std::get<Guest*>(at);
        if (!changeRefused.isEmpty()) {
            return refusal(500, changeRefused);
        }
        // (Read as a form is read: a plus is a blank, as Proxmox's server takes it.)
        const QList<std::pair<QString, QString>> items =
            QUrlQuery(QString::fromLatin1(form).replace(u'+', u' ')).queryItems(QUrl::FullyDecoded);
        QString digest;
        QMap<QString, QString> asked;
        static const QRegularExpression drive(
            QStringLiteral("\\A(ide[0-3]|sata[0-5]|scsi([0-9]|[12][0-9]|30)|virtio([0-9]|1[0-5]))\\z"));
        for (const auto& [key, value] : items) {
            if (key == QStringLiteral("digest")) {
                digest = value;
            } else if (key == QStringLiteral("boot") || drive.match(key).hasMatch()) {
                asked.insert(key, value);
            } else {
                return refusal(400, QStringLiteral("only drives and the boot order are spoken here, not %1").arg(key));
            }
        }
        if (asked.contains(QStringLiteral("boot")) && !guest.mayDisk) {
            return refusal(403, QStringLiteral("Permission check failed (/vms/%1, VM.Config.Disk)").arg(vmid));
        }
        if (!digest.isNull() && digest != digestOf(guest)) {
            return refusal(500, digestSays);
        }
        static const QRegularExpression order(QStringLiteral("\\Aorder=[a-z0-9]+(;[a-z0-9]+)*\\z"));
        for (auto one = asked.cbegin(); one != asked.cend(); ++one) {
            if (one.key() == QStringLiteral("boot")) {
                if (!order.match(one.value()).hasMatch()) {
                    return parameterRefused(
                        QStringLiteral("boot"),
                        QStringLiteral("invalid format - format error\nboot.order: invalid boot device '%1'")
                            .arg(one.value()));
                }
                continue;
            }
            const QStringList parts = one.value().split(u',');
            const QString& disc = parts.first();
            const bool cdrom = parts.contains(QStringLiteral("media=cdrom"));
            const QString was = guest.pending.value(one.key(), guest.config.value(one.key()));
            if (!was.isEmpty() && !was.contains(QStringLiteral(",media=cdrom")) && !guest.mayDisk) {
                return refusal(403, QStringLiteral("Permission check failed (/vms/%1, VM.Config.Disk)").arg(vmid));
            }
            if (!(cdrom ? guest.mayCdrom : guest.mayDisk)) {
                return refusal(403,
                               QStringLiteral("Permission check failed (/vms/%1, %2)")
                                   .arg(vmid)
                                   .arg(cdrom ? QStringLiteral("VM.Config.CDROM") : QStringLiteral("VM.Config.Disk")));
            }
            if (disc != QStringLiteral("none") && disc != QStringLiteral("cdrom")) {
                const QString storage = disc.section(u':', 0, 0);
                const QString volume = disc.section(u':', 1);
                const auto there = storages.constFind(storage);
                if (there == storages.cend() || !volume.startsWith(QStringLiteral("iso/")) ||
                    !there->images.contains(volume.mid(4))) {
                    return refusal(500, QStringLiteral("volume '%1' does not exist").arg(disc));
                }
                if (!there->maySee) {
                    return refusal(403, QStringLiteral("Permission check failed (/storage/%1, "
                                                       "Datastore.AllocateSpace|Datastore.Audit)")
                                            .arg(storage));
                }
                if (!cdrom) {
                    return refusal(
                        400, QStringLiteral("unable to use volume %1 - content type needs to be 'images'").arg(disc));
                }
            }
        }
        QStringList change{QString::number(vmid)};
        for (auto one = asked.cbegin(); one != asked.cend(); ++one) {
            // A disc goes into a running guest's drive at once; a new boot order, and a new drive, wait for its start.
            if (guest.status == QStringLiteral("running") &&
                (one.key() == QStringLiteral("boot") || !guest.config.contains(one.key()))) {
                guest.pending.insert(one.key(), one.value());
            } else {
                guest.config.insert(one.key(), one.value());
                guest.pending.remove(one.key());
            }
            change.append(one.key() + u'=' + one.value());
        }
        changes.append(change.join(u' '));
        if (configHangsUp > 0) {
            --configHangsUp;
            Answered gone;
            gone.hangUp = true;
            return gone;
        }
        if (configAnswer != 0) {
            return bare(configAnswer);
        }
        if (asATask) {
            return data(madeTask(QStringLiteral("qmconfig"), vmid, QStringLiteral("OK")));
        }
        return data(QJsonValue::Null);
    }

    Answered storagesAt(const QString& node, const QString& content)
    {
        if (!nodes.contains(node)) {
            return refusal(500, QStringLiteral("hostname lookup '%1' failed - failed to get address info for: %1: Name "
                                               "or service not known")
                                    .arg(node));
        }
        QJsonArray listed;
        for (auto one = storages.cbegin(); one != storages.cend(); ++one) {
            if (!one->maySee || (!one->nodes.isEmpty() && !one->nodes.contains(node)) ||
                (!content.isEmpty() && !one->content.split(u',').contains(content))) {
                continue;
            }
            listed.append(QJsonObject{{QStringLiteral("storage"), one.key()},
                                      {QStringLiteral("type"), QStringLiteral("dir")},
                                      {QStringLiteral("content"), one->content},
                                      {QStringLiteral("active"), one->active ? 1 : 0},
                                      {QStringLiteral("enabled"), 1},
                                      {QStringLiteral("shared"), one->nodes.isEmpty() ? 1 : 0}});
        }
        if (hostile == QStringLiteral("odd_storages")) {
            listed.append(QJsonValue::Null);
            listed.append(QStringLiteral("local"));
            listed.append(QJsonObject{{QStringLiteral("storage"), 5}});
            listed.append(QJsonObject{{QStringLiteral("storage"), QStringLiteral("../x")},
                                      {QStringLiteral("content"), QStringLiteral("iso")},
                                      {QStringLiteral("active"), 1}});
            listed.append(QJsonObject{{QStringLiteral("storage"), QStringLiteral("zz")},
                                      {QStringLiteral("content"), QStringLiteral("iso")},
                                      {QStringLiteral("active"), 0}});
        }
        return data(listed);
    }

    Answered imagesOn(const QString& node, const QString& storage)
    {
        const auto one = storages.constFind(storage);
        if (one == storages.cend() || (!one->nodes.isEmpty() && !one->nodes.contains(node))) {
            return refusal(500, QStringLiteral("storage '%1' does not exist").arg(storage));
        }
        if (!one->maySee) {
            return refusal(
                403, QStringLiteral("Permission check failed (/storage/%1, Datastore.AllocateSpace|Datastore.Audit)")
                         .arg(storage));
        }
        if (!one->active || !one->online) {
            return refusal(500, QStringLiteral("storage '%1' is not online").arg(storage));
        }
        QJsonArray listed;
        for (auto image = one->images.cbegin(); image != one->images.cend(); ++image) {
            listed.append(QJsonObject{{QStringLiteral("volid"), storage + QStringLiteral(":iso/") + image.key()},
                                      {QStringLiteral("format"), QStringLiteral("iso")},
                                      {QStringLiteral("size"), image.value()},
                                      {QStringLiteral("ctime"), 1762000000},
                                      {QStringLiteral("content"), QStringLiteral("iso")}});
        }
        if (hostile == QStringLiteral("odd_images")) {
            const auto volume = [](const QString& volid, const QString& content) {
                return QJsonObject{{QStringLiteral("volid"), volid}, {QStringLiteral("content"), content}};
            };
            listed.append(QJsonValue::Null);
            listed.append(QStringLiteral("x"));
            listed.append(QJsonObject{{QStringLiteral("volid"), 5}});
            listed.append(
                volume(QStringLiteral("other:iso/") + storage + QStringLiteral(".iso"), QStringLiteral("iso")));
            listed.append(volume(storage + QStringLiteral(":images/vm-100-disk-0.raw"), QStringLiteral("images")));
            listed.append(volume(storage + QStringLiteral(":iso/../../etc/passwd.iso"), QStringLiteral("iso")));
            listed.append(volume(storage + QStringLiteral(":iso/two.iso,media=disk"), QStringLiteral("iso")));
            QJsonObject odd = volume(storage + QStringLiteral(":iso/odd.iso"), QStringLiteral("iso"));
            odd.insert(QStringLiteral("size"), QStringLiteral("big"));
            listed.append(odd);
        }
        Answered said = data(listed);
        said.afterMs = one->takesMs;
        said.byteEveryMs = one->trickles ? 250 : 0;
        return said;
    }

public:
    // The guest is deleted (stopped and destroyed): its number is free.
    void deleteGuest(qint64 vmid) { guests.remove(vmid); }
    // The guest is started by another hand (a person on Proxmox's page, its high availability), as Proxmox starts one.
    void startedByAnotherHand(qint64 vmid) { become(vmid, QStringLiteral("running")); }
    // A new virtual machine, numbered as Proxmox numbers one (/cluster/nextid: the lowest free number from 100 on), so
    // that it has the number of a guest deleted before it; with an SMBIOS UUID of its own, as Proxmox gives every new
    // one. Its number.
    qint64 makeGuest(const Guest& made)
    {
        qint64 vmid = 100;
        while (guests.contains(vmid)) {
            ++vmid;
        }
        guests.insert(vmid, made);
        return vmid;
    }

private:
    Answered versionSaid()
    {
        if (hostile == QStringLiteral("deep")) {
            return {200, QByteArray(100'000, '[')};
        }
        if (hostile == QStringLiteral("not_proxmox")) {
            return data(QJsonObject{{QStringLiteral("product"), QStringLiteral("something else")}});
        }
        if (!secretAfterVersion.isEmpty()) {
            secret = secretAfterVersion;
        }
        Answered said = data(QJsonObject{{QStringLiteral("version"), version},
                                         {QStringLiteral("release"), version.section(u'.', 0, 1)},
                                         {QStringLiteral("repoid"), QStringLiteral("0123456789abcdef")}});
        said.afterMs = versionTakesMs;
        return said;
    }

    Answered resources()
    {
        QJsonArray listed;
        for (auto guest = guests.cbegin(); guest != guests.cend(); ++guest) {
            if (!guest->maySee) {
                continue;
            }
            listed.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("qemu/%1").arg(guest.key())},
                                      {QStringLiteral("type"), QStringLiteral("qemu")},
                                      {QStringLiteral("vmid"), guest.key()},
                                      {QStringLiteral("node"), guest->node},
                                      {QStringLiteral("name"), guest->name},
                                      {QStringLiteral("status"), guest->status},
                                      {QStringLiteral("template"), 0}});
        }
        for (const qint64 vmid : std::as_const(containers)) {
            listed.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("lxc")},
                                      {QStringLiteral("vmid"), vmid},
                                      {QStringLiteral("node"), nodes.value(0)},
                                      {QStringLiteral("status"), QStringLiteral("running")}});
        }
        // (Asked for guests, a real one lists other kinds now and then.)
        listed.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("node")},
                                  {QStringLiteral("node"), nodes.value(0)},
                                  {QStringLiteral("status"), QStringLiteral("online")}});
        if (hostile == QStringLiteral("huge_list")) {
            // Some sixteen megabytes of guests: more than is read of such a list.
            QByteArray body = "{\"data\": [";
            for (int one = 0; one < 60'000; ++one) {
                body += "{\"vmid\": " + QByteArray::number(1000 + one) +
                        ", \"type\": \"qemu\", \"node\": \"pve1\", \"name\": \"" + QByteArray(200, 'x') +
                        "\", \"status\": \"running\"},";
            }
            body.chop(1);
            return {200, body + "]}"};
        }
        Answered all = data(listed);
        all.afterMs = resourcesTakesMs;
        return all;
    }

    Answered current(const QString& node, qint64 vmid)
    {
        if (beforeStatusRead) {
            beforeStatusRead();
        }
        const std::variant<Guest*, Answered> at = guestAt(node, vmid, QStringLiteral("VM.Audit"));
        if (std::holds_alternative<Answered>(at)) {
            return std::get<Answered>(at);
        }
        const Guest& guest = *std::get<Guest*>(at);
        const int takes = m_commanded ? statusTakesMsAfterACommand : 0;
        if (hostile == QStringLiteral("lost_guest")) {
            // (It says the guest is not on its node, and lists it there all the same.)
            return refusal(
                500,
                QStringLiteral("Configuration file 'nodes/%1/qemu-server/%2.conf' does not exist").arg(node).arg(vmid));
        }
        if (hostile == QStringLiteral("null_status")) {
            return data(QJsonValue::Null);
        }
        if (hostile == QStringLiteral("odd_status")) {
            return data(QJsonObject{{QStringLiteral("status"), QJsonArray{}},
                                    {QStringLiteral("qmpstatus"), QJsonObject{}},
                                    {QStringLiteral("name"), QJsonArray{QStringLiteral("x")}},
                                    {QStringLiteral("cpus"), QStringLiteral("2")},
                                    {QStringLiteral("maxmem"), QJsonValue::Null}});
        }
        const bool running = guest.status == QStringLiteral("running");
        Answered said = data(QJsonObject{
            {QStringLiteral("vmid"), vmid},
            {QStringLiteral("name"), guest.name},
            {QStringLiteral("status"), guest.status},
            {QStringLiteral("qmpstatus"), running
                                              ? (guest.paused ? QStringLiteral("paused") : QStringLiteral("running"))
                                              : QStringLiteral("stopped")},
            {QStringLiteral("cpus"), guest.cpus},
            {QStringLiteral("maxmem"), guest.maxmem},
            {QStringLiteral("mem"), running ? guest.maxmem / 4 : 0},
            {QStringLiteral("cpu"), running ? 0.03 : 0},
            {QStringLiteral("uptime"), running ? guest.uptime : 0},
            {QStringLiteral("ha"), QJsonObject{{QStringLiteral("managed"), 0}}},
        });
        said.afterMs = takes + statusTakesMs;
        if (m_commanded && statusTricklesAfterACommand) {
            said.byteEveryMs = 250;
        }
        return said;
    }

    QElapsedTimer m_clock;
    bool m_commanded = false; // a power command came
};

} // namespace qkvm::core::testing
