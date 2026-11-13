#include <qkvm/core/device_asker.h>

#include <algorithm>
#include <utility>

#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimer>

#include <qkvm/core/kvm_certificate.h>
#include <qkvm/core/wiped.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

QByteArray shownBy(const QSslConfiguration& configuration)
{
    const QSslCertificate shown = configuration.peerCertificate();
    return shown.isNull() ? QByteArray() : shown.digest(QCryptographicHash::Sha256);
}

// The ways a question is asked: a look, and what a server's controller is asked to do (POST), to change
// (PATCH) and to forget (DELETE: a session given back). As written, in capitals: nothing else is a way.
bool isAWayOfAsking(const QByteArray& method)
{
    return method == "GET"_ba || method == "POST"_ba || method == "PATCH"_ba || method == "DELETE"_ba;
}

// The headers a caller may send, by the names they are sent under: none that signs this program on to
// anything (Authorization, Cookie), says where the question goes (Host) or what goes with it, or carries
// the session (the asker's own). Nothing where `name` is none of them.
QByteArray headerSentAs(const QByteArray& name)
{
    for (const QByteArray& one : {"If-Match"_ba, "Accept"_ba}) {
        if (name.compare(one, Qt::CaseInsensitive) == 0) {
            return one;
        }
    }
    return {};
}

// A header's value as one line that prints: nothing that could end the line, or begin another.
bool isOneLine(const QByteArray& value)
{
    return std::ranges::all_of(value, [](char sign) { return sign >= ' ' && sign <= '~'; });
}

// What a question sent, held until Qt has let go of it, and then written over (wipeInPlace): of a sign-in, the last
// of its form or JSON that this program holds, whoever made it having let go of theirs by then (from the merged review,
// 3). Qt holds what it was handed for as long as the reply is, and lets go of it as the reply goes, after the reply's
// own children: so this goes a turn of the loop after the reply did, or with the asker.
class SentBody final : public QObject {
public:
    SentBody(QByteArray body, QObject* parent) : QObject(parent), m_body(std::move(body)) {}
    SentBody(const SentBody&) = delete;
    SentBody& operator=(const SentBody&) = delete;
    SentBody(SentBody&&) = delete;
    SentBody& operator=(SentBody&&) = delete;
    ~SentBody() override { wipeInPlace(m_body); }

private:
    QByteArray m_body;
};

} // namespace

// The words are the server's driver's: a card shows them as they are, whoever said them.
base::Error DeviceAsker::unreachable()
{
    return {"unreachable", "Device is unreachable"};
}
base::Error DeviceAsker::tookTooLong()
{
    return {"timeout", "Device did not respond in time"};
}
base::Error DeviceAsker::lineFell()
{
    return {"dropped", "The device closed the connection"};
}
base::Error DeviceAsker::tooMuch()
{
    return {"dropped", "The device sent far more data than this program accepts"};
}
base::Error DeviceAsker::packed()
{
    return {"dropped", "The device packed its answer in a way this program does not unpack"};
}
base::Error DeviceAsker::certificateChanged()
{
    return {"cert_mismatch", "Certificate does not match the one saved for this device"};
}
base::Error DeviceAsker::certificateNotTrusted()
{
    return {"cert_invalid", "The device's certificate is not trusted"};
}
base::Error DeviceAsker::certificateOfAnUnknownSigner()
{
    return {"cert_invalid", "The device's certificate was signed by a maker of certificates that Windows does not "
                            "know. Have Windows trust that maker, and the device is let in by that."};
}
base::Error DeviceAsker::certificateNotRead()
{
    return {"cert_invalid",
            "The certificate saved for this device cannot be read. Look at its certificate and trust it again."};
}
base::Error DeviceAsker::notFitToBeSent()
{
    return {"invalid", "What was to be asked of the device is not fit to be sent"};
}

DeviceAsker::DeviceAsker(Where where, Times times, QByteArray cookie, QObject* parent)
    : DeviceAsker(std::move(where), times, Carried{Carried::In::Cookie, std::move(cookie)}, parent)
{}

DeviceAsker::DeviceAsker(Where where, Times times, Carried session, QObject* parent)
    : QObject(parent), m_where(std::move(where)), m_times(times), m_session(std::move(session))
{
    // Never somewhere else by itself, and straight to the KVM: it is on the local network, where
    // the proxy this PC may use for the web is not, and its password is not for a proxy to see.
    m_network.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    m_network.setProxy(QNetworkProxy::NoProxy);
}

DeviceAsker::~DeviceAsker() = default;

void DeviceAsker::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

base::Result<QNetworkReply*> DeviceAsker::sendOut(const Ask& question, const QByteArray& token)
{
    // (Every question names how much of an answer it takes, or that its answer never ends: one that
    // names none would be refused every answer as too much, for no reason anybody could see.)
    Q_ASSERT(question.endless || question.largest > 0);
    // What this program does not ask, or asks with a header it does not send, goes nowhere at all.
    if (!isAWayOfAsking(question.method)) {
        return notFitToBeSent();
    }
    for (const auto& [name, value] : question.sent) {
        if (headerSentAs(name).isEmpty() || !isOneLine(value)) {
            return notFitToBeSent();
        }
    }
    if (refusedForItsTls(m_windowsOwnTls, m_where.address)) {
        // Whoever would judge its certificate is not Windows: nothing goes out, the password
        // least of all.
        return damagedTls();
    }
    const bool encrypted = m_where.address.scheme() == "https"_L1;
    if (encrypted && m_where.knownByCertificate && m_where.certificate.size() != 32) {
        // What is saved as its certificate is none. Nothing is trusted in its place.
        return certificateNotRead();
    }
    QUrl url = m_where.address;
    // The path as it is written, escapes and all: a place a server's controller points to is one
    // already (redfish::link), and is not escaped a second time.
    url.setPath(question.path, QUrl::TolerantMode);
    url.setQuery(question.query);
    QNetworkRequest request(url);
    const int quietMs = question.quietMs > 0 ? question.quietMs : m_times.quietMs;
    // The limits on an answer's whole are for an answer that has an end: one that has none is
    // read as it comes by whoever asked, who keeps the limits that fit it.
    const bool whole = !question.endless;
    if (whole) {
        request.setTransferTimeout(quietMs);
    }
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    // The session is carried by hand: Qt's store of cookies is neither written nor read.
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    // Nor does Qt sign on to anything by itself. Asked to in some ways (NTLM, Negotiate), it
    // would answer with the account of whoever is signed in to Windows, and send the question
    // again: a KVM, or whatever answers in its place, is not handed that.
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    if (encrypted && m_where.knownByCertificate) {
        // Known by its certificate alone: Windows vouches for nobody here and objects to every
        // certificate, the KVM's own included, so that the look below is made for every line
        // that is made, also for one that comes up when no question is waiting for it.
        request.setSslConfiguration(kvm::knownByItsCertificateAlone(QSslConfiguration::defaultConfiguration()));
    }
    // One way of speaking, the plain one: that a question has gone out is told for it (below),
    // and on that hangs whether something may have been done.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    // The KVM is asked not to pack its answer: what unpacks to how much is then nobody's guess.
    request.setRawHeader("Accept-Encoding"_ba, "identity"_ba);
    for (const auto& [name, value] : question.sent) {
        request.setRawHeader(headerSentAs(name), value);
    }
    if (!token.isEmpty()) {
        if (m_session.in == Carried::In::Header) {
            request.setRawHeader(m_session.name, token);
        } else if (m_session.in == Carried::In::CookiesAndHeader) {
            const qsizetype cut = token.indexOf('\n');
            const QByteArray cookies = cut < 0 ? QByteArray() : token.left(cut);
            const QByteArray beside = cut < 0 ? QByteArray() : token.mid(cut + 1);
            if (cookies.isEmpty() || beside.isEmpty() || !isOneLine(cookies) || !isOneLine(beside)) {
                return notFitToBeSent();
            }
            request.setRawHeader("Cookie"_ba, cookies);
            request.setRawHeader(m_session.name, beside);
        } else {
            request.setRawHeader("Cookie"_ba, m_session.name + "="_ba + token);
        }
    }
    QNetworkReply* reply = nullptr;
    // Whether Qt says when this question has gone out. It does not for a POST or a PATCH that has
    // nothing to send along (the tests try both), and does for one that sends no body at all (a
    // GET, a DELETE); and a POST sent Qt's other way has what stands behind its question mark
    // moved into it, where a KVM does not look.
    const bool sendsABody = question.method == "POST"_ba || question.method == "PATCH"_ba;
    const bool saysWhenOut = !sendsABody || !question.body.isEmpty();
    if (sendsABody) {
        request.setRawHeader("Content-Type"_ba,
                             question.body.isEmpty() ? "application/octet-stream"_ba : question.bodyIs);
        reply = question.method == "POST"_ba ? m_network.post(request, question.body)
                                             : m_network.sendCustomRequest(request, "PATCH"_ba, question.body);
        // What it sent, written over once Qt has let go of it (SentBody): a turn of the loop after the reply went.
        auto* sent = new SentBody(question.body, this); // NOLINT(cppcoreguidelines-owning-memory): this asker's
        connect(reply, &QObject::destroyed, sent, &QObject::deleteLater);
    } else if (question.method == "DELETE"_ba) {
        reply = m_network.deleteResource(request);
    } else {
        reply = m_network.get(request);
    }

    reply->setProperty("saysWhenOut", saysWhenOut);
    if (whole) {
        // An answer is not gathered beyond what is taken while the interface is busy elsewhere.
        reply->setReadBufferSize(question.largest + 1);
    }
    if (saysWhenOut) {
        const auto wentOut = [reply] { reply->setProperty("wentOut", true); };
        connect(reply, &QNetworkReply::requestSent, reply, wentOut);
        connect(reply, &QNetworkReply::metaDataChanged, reply, wentOut);
    }
    // To be let in at all it has a few seconds: a KVM that is not there takes Windows far longer
    // to give up on. Only for a question that does nothing to the computer: Qt's word that a
    // question went comes a moment after it went, and in that moment this would call "not
    // there" what is on its way. A look that is told so wrongly is a look; a button is not.
    // A PATCH changes something and a DELETE ends something, whatever their caller says of them.
    const bool doesSomething =
        question.doesSomething || question.method == "PATCH"_ba || question.method == "DELETE"_ba;
    if (saysWhenOut && !doesSomething) {
        QTimer::singleShot(m_times.connectWithinMs, reply, [reply] {
            // (Nor once the two have agreed on how to speak over HTTPS, which says the line was let in: below.)
            if (reply->property("saysWhenOut").toBool() && !reply->property("wentOut").toBool() &&
                !reply->isFinished()) {
                reply->setProperty("gaveUp", "unreachable"_ba);
                reply->abort();
            }
        });
    }
    if (whole) {
        // Nor may it keep a question open for as long as it likes, a byte at a time. (A precise timer: a coarse one,
        // as Qt makes one of over twenty seconds, comes up to five in a hundred late, a sign-in's 48.5 s by 2.4 s, and
        // a sign-in's hold is told by when its line gives up: SignInPacing.)
        QTimer::singleShot(m_times.connectWithinMs + quietMs + m_times.transferMs, Qt::PreciseTimer, reply, [reply] {
            if (!reply->isFinished()) {
                reply->setProperty("gaveUp", "timeout"_ba);
                reply->abort();
            }
        });
    }

    // Windows does not trust what the KVM showed. Where the KVM is known by a certificate that
    // says nothing yet: whether it showed that one is looked at below. Where it is known by none,
    // that is the end of it.
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError>& errors) {
        // Decided here, by the certificate: what Qt is told to pass over holds for every line to
        // this KVM from now on, the ones that come up with no question waiting among them. So
        // it is told that only for the certificate the KVM is known by, and for no other.
        // And only where every objection names that certificate itself. One that somebody else
        // signed is objected to by its signer's name, and passing that over would pass it over
        // for every other certificate of that signer's: such a KVM is let in where Windows trusts
        // the signer, and by that.
        // (The rule itself is kvm::objectedTo, which the lines the program speaks itself keep too.)
        const QSslCertificate certificate = reply->sslConfiguration().peerCertificate();
        const kvm::Objected objected =
            kvm::objectedTo(certificate, errors, m_where.knownByCertificate, m_where.certificate);
        if (objected == kvm::Objected::PassedOver) {
            reply->ignoreSslErrors(errors);
            return;
        }
        m_shownCertificate = shownBy(reply->sslConfiguration());
        if (objected == kvm::Objected::BySigner) {
            reply->setProperty("certificate", "cert_signer"_ba);
        } else {
            reply->setProperty("certificate", m_where.knownByCertificate ? "cert_mismatch"_ba : "cert_invalid"_ba);
        }
    });
    // The two have agreed on how to speak, and nothing has been sent yet.
    connect(reply, &QNetworkReply::encrypted, this, [this, reply] {
        // The line was let in: from here on whether the question went out is no longer what Qt says of it, and
        // it is never told as not there. (Over plain HTTP Qt says nothing between being let in and the question
        // going out.)
        reply->setProperty("saysWhenOut", false);
        if (!m_where.knownByCertificate) {
            return; // one that Windows trusts for its name
        }
        const QSslCertificate shown = reply->sslConfiguration().peerCertificate();
        const QByteArray sha256 = shownBy(reply->sslConfiguration());
        if (sha256 != m_where.certificate) {
            m_shownCertificate = sha256;
            reply->setProperty("certificate", "cert_mismatch"_ba);
            reply->abort();
            return;
        }
        // The one it is known by, and the certificate itself this time: told once, for whoever
        // keeps it beside the fingerprint. (What is said is what was just hashed, and no other.)
        if (!std::exchange(m_toldItsCertificate, true)) {
            emit showedItsCertificate(shown.toDer());
        }
    });
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, whole, largest = question.largest] {
        const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
        if (whole && length.isValid() && length.toLongLong() > largest) {
            reply->setProperty("tooMuch", true);
            reply->abort();
        }
        const QByteArray packing = reply->rawHeader("Content-Encoding"_ba).trimmed().toLower();
        if (!packing.isEmpty() && packing != "identity"_ba) {
            reply->setProperty("packed", true);
            reply->abort();
        }
    });
    if (whole) {
        connect(reply, &QNetworkReply::readyRead, reply, [reply, largest = question.largest] {
            if (reply->bytesAvailable() > largest) {
                reply->setProperty("tooMuch", true);
                reply->abort();
            }
        });
    }
    return reply;
}

std::optional<base::Error> DeviceAsker::whyNoAnswer(const QNetworkReply* reply)
{
    // The send notification helps distinguish admission from response timeouts.
    // Its absence is not proof that nothing reached the peer: a remote-close error
    // can arrive without it, and the request may have been acted on all the same.
    const bool neverWent = reply->property("saysWhenOut").toBool() && !reply->property("wentOut").toBool();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray certificate = reply->property("certificate").toByteArray();
    const QByteArray gaveUp = reply->property("gaveUp").toByteArray();
    const QNetworkReply::NetworkError error = reply->error();
    // Of the line, and not what the KVM answered: Qt numbers those below 200, and one more.
    const bool ofTheLine =
        error != QNetworkReply::NoError && (static_cast<int>(error) < 200 || error == QNetworkReply::ProtocolFailure);
    if (certificate == "cert_mismatch"_ba) {
        return certificateChanged();
    }
    if (certificate == "cert_signer"_ba) {
        return certificateOfAnUnknownSigner();
    }
    if (!certificate.isEmpty()) {
        return certificateNotTrusted();
    }
    if (reply->property("tooMuch").toBool()) {
        return tooMuch();
    }
    if (reply->property("packed").toBool()) {
        return packed();
    }
    if (gaveUp == "unreachable"_ba) {
        return unreachable();
    }
    if (gaveUp == "timeout"_ba) {
        return neverWent ? unreachable() : tookTooLong();
    }
    if (!ofTheLine && status != 0) {
        return std::nullopt;
    }
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::HostNotFoundError:
        // Before a line was there at all.
        return unreachable();
    case QNetworkReply::RemoteHostClosedError:
        // A peer closed an established line, even if its send notification is missing.
        return lineFell();
    case QNetworkReply::TimeoutError:
        // Nothing came for too long.
        return neverWent ? unreachable() : tookTooLong();
    default:
        // Usually a line the KVM closed while it sat unused, or because it restarted. Qt
        // has by then asked a GET again by itself, once, and nothing else: not a POST, a
        // PATCH or a DELETE (all tried, on a new line and on a kept one): a power button is
        // not pressed twice. And Qt's words for much else, a line that broke after the
        // question went among them: so none of them is "not there" by itself.
        return neverWent ? unreachable() : lineFell();
    }
}

void DeviceAsker::send(const Ask& question, const QByteArray& token, std::function<void(base::Result<Reply>)> done)
{
    const base::Result<QNetworkReply*> asked = sendOut(question, token);
    if (!asked) {
        later([done = std::move(done), error = asked.error()] { done(error); });
        return;
    }
    QNetworkReply* reply = asked.value();
    // (What it asks of the answer is the question's: kept with it. What it sent is written over by SentBody.)
    connect(reply, &QNetworkReply::finished, this, [this, reply, question, done = std::move(done)] {
        reply->deleteLater();
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 0) {
            m_heard.start(); // (it answered, whatever it said and whatever became of the answer)
        }
        if (const std::optional<base::Error> why = whyNoAnswer(reply)) {
            done(*why);
            return;
        }
        Reply said;
        said.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        said.body = reply->readAll();
        said.kind =
            reply->header(QNetworkRequest::ContentTypeHeader).toByteArray().split(';').value(0).trimmed().toLower();
        for (const QByteArray& name : question.headers) {
            said.headers.insert(name.toLower(), reply->rawHeader(name).trimmed().toLower());
        }
        for (const QByteArray& name : question.headersAsSent) {
            said.headersAsSent.insert(name.toLower(), reply->rawHeader(name).trimmed());
        }
        said.cookies = reply->header(QNetworkRequest::SetCookieHeader).value<QList<QNetworkCookie>>();
        done(std::move(said));
    });
}

void DeviceAsker::whenUp(const Ask& first, std::function<void(std::optional<base::Error>)> then)
{
    if (heardWithin(m_times.connectWithinMs)) {
        later([then = std::move(then)] { then(std::nullopt); });
        return;
    }
    // (A question that does nothing, and carries no session: under the guess, as every such question is.)
    Q_ASSERT(!first.doesSomething && first.method == "GET"_ba);
    send(first, {}, [then = std::move(then)](const base::Result<Reply>& reply) {
        then(reply ? std::nullopt : std::optional<base::Error>(reply.error()));
    });
}

} // namespace qkvm::core
