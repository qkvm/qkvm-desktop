#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <qkvm/base/result.h>
#include <qkvm/core/network_end.h>
#include <qkvm/core/tls.h>

class QIODevice;
class QNetworkReply;

namespace qkvm::core {

// How the driver of a KVM of this PC's own asks its KVM a question over HTTP or HTTPS, the same for
// every family (SPEC.md section 8): straight to it, whatever proxy this PC uses for the web, and
// followed nowhere; Qt's store of cookies neither written nor read, the session carried by hand in
// the one cookie the family names, or in a header of its own (a server's controller's); of the
// caller's headers only the few that sign on to nothing; Qt signs on to nothing by itself; over HTTPS
// the certificate looked at when the two have agreed on how to speak and before a byte of the
// question goes out (one Windows trusts for the KVM's name, or exactly the one the KVM is known by:
// kvm_certificate.h's rule); every answer within a size and a time; and whether a question went out,
// on which hangs whether something may have been done. Taken out of KvmdDevice as it was (M6,
// increment 1a), so that a second family's driver asks as the first does, and not by a copy of it.
//
// On the interface thread, as its driver is. Its questions are its own network manager's: whoever
// holds one of them lets go of it before this goes (KvmdDevice's streams do).
class DeviceAsker : public QObject {
    Q_OBJECT

public:
    // Where the KVM is and what it is known by.
    struct Where {
        QUrl address; // http or https, a host, a port where one was typed, and nothing else
        // Over HTTPS: whether it is known by a certificate, and that certificate's SHA-256. One that
        // is known by none has to be trusted by Windows for its name.
        bool knownByCertificate = false;
        QByteArray certificate;
    };
    // The times of a question (kvm_times.h): to be let in at all; the longest nothing may come while
    // an answer is waited for; and how much longer than that a whole answer may take.
    struct Times {
        int connectWithinMs = 0;
        int quietMs = 0;
        int transferMs = 0;
    };
    struct Ask {
        // "GET", "POST", "PATCH", "PUT" or "DELETE": a question asked any other way is refused before
        // anything goes ("invalid"). A PATCH, a PUT and a DELETE do something, whatever `doesSomething` says. Qt
        // asks a PUT once more by itself where the line falls before an answer, as it does a GET (HTTP holds a PUT
        // to be idempotent): only what cannot be done twice by a second one goes as a PUT.
        QByteArray method = QByteArrayLiteral("GET");
        QString path;
        QUrlQuery query;
        QByteArray body;
        // A body read from this device as it goes, in the place of `body`: of the length it has from where it stands,
        // said first (Content-Length), and read as the line takes it, never gathered whole first (an image carried to
        // the KVM: a file read at a place). Whoever asks keeps it until the answer; Qt reads it on this thread.
        QIODevice* streamed = nullptr;
        QByteArray bodyIs;          // the body's kind, where there is a body
        qsizetype largest = 0;      // of the answer: whoever asks says how much it takes
        int quietMs = 0;            // 0: the usual
        bool doesSomething = false; // to the computer: never told as "not there" on a guess
        // The answer never ends: no limit on its whole, neither in time nor in size. Whoever asks
        // reads it as it comes and keeps limits of its own.
        bool endless = false;
        // The longest the whole may take, from the asking on; 0: to be let in, quietMs and transferMs, as for any.
        int wholeMs = 0;
        // Told the reply as it is handed to the line: for whoever follows how far its body has gone (uploadProgress),
        // or stops it (abort).
        std::function<void(QNetworkReply*)> handed;
        // It goes only on a line this asker has open already, the one the question before it was answered on (a
        // sign-in's password, after the question that asked which device answers there). Over HTTPS, where Qt makes a
        // new line for it, it is stopped as the two agree on how to speak, before a byte of it goes (lineMadeAnew).
        // Over plain HTTP nothing tells that before it goes: whoever asks looks at what the answer before said of its
        // line (Reply::lineEnds). (Outside review of 0.2.0, area 5, MEDIUM 5.4.)
        bool onItsLine = false;
        // Headers of the question beside the asker's own: only "If-Match" and "Accept", with a
        // value that prints and is one line. Any other, "Authorization", "Cookie" and "Host" among
        // them, and the question is refused before anything goes ("invalid"): nothing of the
        // caller's signs this program on to anything, and no secret leaves in a header of its.
        QList<std::pair<QByteArray, QByteArray>> sent;
        // Headers of the answer that whoever asks reads (their names as a KVM sends them).
        QList<QByteArray> headers;
        // And those whose case matters (a session's pass, where to sign out of it, a mark on a
        // state): read as they were sent.
        QList<QByteArray> headersAsSent;
    };
    struct Reply {
        int status = 0;
        QByteArray body;
        QByteArray kind; // what it says its body is, in small letters and without what follows a ";"
        QHash<QByteArray, QByteArray> headers; // those that were asked for: trimmed, in small letters
        // Those that were asked for as sent: by their names in small letters, trimmed, as they came.
        QHash<QByteArray, QByteArray> headersAsSent;
        QList<QNetworkCookie> cookies;
        // The words of its status line after the status ("Not Found"), as they came, mostReasonBytes of them at the
        // most: where a Proxmox says why it refused (its API's own format puts nothing in the body), and read as
        // hostile words by whoever reads them (proxmox::saidBy). Empty where there were none.
        QByteArray reason;
        // It said that its line ends with it ("Connection: close"): a question after it goes on a new line.
        bool lineEnds = false;
    };
    static constexpr qsizetype mostReasonBytes = 1024;
    // How the session goes with every question: in the one cookie the family names (a kvmd's
    // "auth_token", a JetKVM's "authToken"), or in a header of its own (a server's controller's
    // "X-Auth-Token"), with no cookie at all; or, a controller's own web page's (AMI's), in the cookies
    // it handed out and a header of its own beside them (`name`: its "X-CSRFTOKEN"), the session handed
    // to send() then being those cookies as a Cookie header says them and that header's value, on two
    // lines (each one that prints, or nothing is sent).
    struct Carried {
        enum class In { Cookie, Header, CookiesAndHeader };
        In in = In::Cookie;
        QByteArray name;
    };

    // `cookie`: the name the session goes under ("auth_token" for a kvmd, "authToken" for a JetKVM).
    DeviceAsker(Where where, Times times, QByteArray cookie, QObject* parent = nullptr);
    DeviceAsker(Where where, Times times, Carried session, QObject* parent = nullptr);
    ~DeviceAsker() override;
    DeviceAsker(const DeviceAsker&) = delete;
    DeviceAsker& operator=(const DeviceAsker&) = delete;
    DeviceAsker(DeviceAsker&&) = delete;
    DeviceAsker& operator=(DeviceAsker&&) = delete;

    const Where& where() const { return m_where; }
    // From now on every question of this asker goes to `at` and to no other address (one its address's name was found
    // at), whatever Windows would find that name at for a line of its own: its Host header is its address's name and
    // port as ever, and over HTTPS the certificate is held to that name and the name said to it (SNI), as by its name.
    // Whether it can be: false for an address no address of the web can be written with (an IPv6 one with a zone), and
    // then nothing changes.
    bool onlyTo(const QHostAddress& at);
    // Every question of this asker on one connection at a time (Qt's manager of HTTP keeps up to six to a device): what
    // goes only on its line (Ask::onItsLine) then never finds another connection open that a question before made, and
    // nobody asked anything on. From the first question on.
    void keepToOneConnection() { m_oneConnection = true; }

    // Sends one question out, as every question goes: straight to the KVM, within its limits, the
    // certificate looked at first. The answer is the caller's to read, and to end (deleteLater) when
    // it is over. Not sent at all: what is saved as its certificate is none; or the KVM's address is
    // an HTTPS one and TLS is not Windows' own (tls.h).
    base::Result<QNetworkReply*> sendOut(const Ask& question, const QByteArray& token);
    // Why a question that is over got no answer of the KVM's own, in the drivers' words: the line,
    // the certificate, or a limit that was marked on it. Nothing where the KVM answered, whatever
    // it said.
    static std::optional<base::Error> whyNoAnswer(const QNetworkReply* reply);
    // One question and its whole answer, handed to `done` later, on this thread, exactly once,
    // unless this is gone by then.
    void send(const Ask& question, const QByteArray& token, std::function<void(base::Result<Reply>)> done);
    // Whether the KVM answered on this asker within the last `ms`, whatever it said: it is there.
    bool heardWithin(qint64 ms) const { return m_heard.isValid() && m_heard.elapsed() <= ms; }
    // For a question that carries what a KVM counts (a sign-in's password), asked only on a line shown to be up:
    // where the KVM was not heard on this asker just now (within the time a line has to be let in), `first`, a
    // question that carries nothing, is asked before it, under the guess that tells a KVM that is not there; and
    // `then` is told nothing where the KVM answered it (whatever it said) or was heard just now, and why not where
    // it did not. Nothing of the other has gone then; once it goes, as one that does something, it is never told
    // "not there" on a guess, and goes on the line just shown up where the KVM keeps it.
    void whenUp(const Ask& first, std::function<void(std::optional<base::Error>)> then);

    // What it showed the last time it was refused for its certificate; empty before that.
    QByteArray shownCertificate() const { return m_shownCertificate; }

    // The words of what came of a question that got no answer of the KVM's own (whyNoAnswer), which a
    // driver says too: the server's driver's words, which a card shows as they are.
    static base::Error unreachable();
    static base::Error tookTooLong();
    static base::Error lineFell();
    static base::Error tooMuch();
    static base::Error packed();
    static base::Error certificateChanged();
    static base::Error certificateNotTrusted();
    static base::Error certificateOfAnUnknownSigner();
    static base::Error certificateNotRead();
    // A question this program does not ask: another way of asking than its four, or a header that
    // is not one of the few it sends. Nothing went.
    static base::Error notFitToBeSent();
    // A question that goes only on its line (Ask::onItsLine) found that line gone, and a new one made for it: stopped
    // before a byte of it went.
    static base::Error lineMadeAnew();
    static constexpr const char* lineMadeAnewCode = "line_made_anew";

signals:
    // The KVM has shown, on a line of this asker's, the certificate it is known by: the certificate
    // itself, as its bytes (DER), which have the SHA-256 its lines are held to. Said once, the first
    // time, and never of another certificate: of a KVM that is known by none, and of one that shows
    // another, nothing is said.
    void showedItsCertificate(const QByteArray& der);

private:
    void later(std::function<void()> what);

    Where m_where;
    Times m_times;
    Carried m_session;
    // Where every question goes (onlyTo), in the place of its address's host; empty: its address as it is.
    QString m_onlyTo;
    // Windows' own TLS, as for a QKVM server (ServerClient): the same trust on every PC. Chosen before
    // anything of Qt's that speaks is made, which is why it stands before that here; and where it is
    // not what is in use, nothing goes out to an HTTPS address (sendOut, tls.h).
    const bool m_windowsOwnTls = useWindowsOwnTls();
    // Ended off the interface thread, its questions with it here (network_end.h): its end may wait five seconds.
    OwnNetwork m_network = ownNetwork();
    QByteArray m_shownCertificate;
    bool m_toldItsCertificate = false; // showedItsCertificate was said
    bool m_oneConnection = false;      // keepToOneConnection
    QElapsedTimer m_heard;             // since the KVM last answered on this asker; not started: never
};

} // namespace qkvm::core
