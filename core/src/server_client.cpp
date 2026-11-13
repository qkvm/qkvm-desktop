#include <qkvm/core/server_client.h>
#include <qkvm/core/wiped.h>

#include <algorithm>
#include <memory>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimer>
#include <QUrlQuery>

#include <qkvm/core/read_ahead.h>
#include <qkvm/core/tls.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// How long a call may take before the server counts as not answering. A picture is given longer: the
// server may have to fetch it from the machine first.
constexpr int answerWithinMs = 10'000;
// A file of the wall is sealed and opened with a passphrase, which is slow on purpose: the page
// gives the server a minute for either, and two to take one in.
constexpr int fileWithinMs = 60'000;
constexpr int takeWithinMs = 120'000;
constexpr int pictureWithinMs = 15'000;
// An action is over when the machine says so, and typing a long text takes it more than a minute.
// Giving up sooner would invite a second try, and the text typed twice. As long as the page waits.
constexpr int doneWithinMs = 150'000;
// Start into BIOS setup over a slow controller: two minutes to come to its restart, and what is under way then (a
// request, a clearing of the boot override) is finished before the server answers, so that the answer can say what
// is left set. As long as the page waits for it (SETUP_PATIENCE).
constexpr int setupWithinMs = 240'000;
// A controller takes its time to make out a pass into its console. As long as the page waits.
constexpr int consoleWithinMs = 30'000;
// Virtual media (0.3.0). A list is asked of the machine there and then: as long as the page waits for it. A mount, an
// unmount and a deletion: a server's mount may wait for its drive to show (90 s), its answer (60 s) and its task
// (120 s), and one that restarts the machine for it to stop and to start again; as long as the page waits (its
// MEDIA_PATIENCE).
constexpr int mediaListWithinMs = 30'000;
constexpr int mediaWithinMs = 420'000;
// An upload may take hours: it is given as long as something comes or goes (ServerClient::m_uploadQuietMs), and all
// told as long as the server gives it (UPLOAD_MOST, a day), and its answer after that.
constexpr int uploadWholeWithinMs = 25 * 3600 * 1000;
// How long a line the server kept may be idle here before it is let go of, in seconds (b11): well within qKVM's own
// keep-alive (uvicorn's five seconds), so that nothing is written on a line the server may be closing just then, and
// within what a proxy before it is likely to keep; and long enough that the wall's own looks, a second or two apart,
// go on one line and not on a new one (and a new TLS handshake) each time.
constexpr int idleLineSeconds = 2;
// What the server says of an image's name where it does not take it (kvm.NAME_RULE).
constexpr const char* imageNameRule =
    "An image's name is of letters, digits, spaces and . _ + - ( ) [ ] ~ , (200 at the most), and neither begins nor "
    "ends with a space or a dot. Rename the file and upload it again.";

// Each of these is how long nothing may come. The whole of a call, from the question to the last
// byte of its answer, may take this many times as long (finish()).
constexpr int wholeWithinTimes = 2;

// The longest the server is taken at its word on how long to be left alone (Retry-After), whoever says so: a sign-in
// as long as the server locks an address out at the most (fifteen minutes); a live picture's pass as long as a display
// link's key waits at the longest; a history read as long as the page backs off at the longest (a minute).
constexpr int longestSignInWaitMs = 900'000;
constexpr int longestLiveWaitMs = 300'000;
constexpr int longestReadWaitMs = 60'000;

// How long the server asked to be left alone with a refusal, in milliseconds: what Retry-After says in whole seconds,
// on a refusal in the server's own shape (what a gateway's page names is the gateway's guess), never more than
// `longestMs`. 0: it named no time, or what answered was not the server, or there was no answer.
int waitSaidMs(const QNetworkReply* reply, int longestMs)
{
    if (reply == nullptr || !reply->property("itsOwnWords").toBool()) {
        return 0;
    }
    bool isNumber = false;
    const qlonglong seconds = reply->rawHeader("Retry-After"_ba).trimmed().toLongLong(&isNumber);
    return isNumber ? static_cast<int>(std::clamp<qlonglong>(seconds, 0, longestMs / 1000) * 1000) : 0;
}

// The server's name for its session, and the most a session is long: more is not one of the server's.
constexpr const char* sessionCookie = "qkvm_session";
constexpr qsizetype longestSession = 512;

// Whether this could be a session of the server's: only what a cookie's value can be is ever sent.
bool isSession(const QByteArray& value)
{
    return !value.isEmpty() && value.size() <= longestSession && std::ranges::all_of(value, [](char letter) {
        return letter > 0x20 && letter < 0x7F && letter != '"' && letter != ',' && letter != ';' && letter != '\\';
    });
}

base::Error notReached(const QNetworkReply& reply)
{
    return {ServerClient::unreachable, "qKVM could not be reached: " + reply.errorString().toStdString()};
}

// Whether this is something the program found itself (ServerClient::unreachable and the others):
// those are written with a colon, and no word of a server's is.
bool isOwnFinding(const base::Error& error)
{
    return error.code.find(':') != std::string::npos;
}

// A question that sends something along, which is JSON for every call that does.
QNetworkRequest asJson(QNetworkRequest asked)
{
    asked.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    return asked;
}

// What is said of a QKVM that does not speak this program's version of the API: with the versions
// it does speak, so that whoever reads it sees which of the two is behind.
base::Error speaksOtherVersions(const QStringList& apis)
{
    QString theirs = apis.join(u", "_s);
    if (const qsizetype last = theirs.lastIndexOf(u", "_s); last >= 0) {
        theirs.replace(last, 2, u" and "_s);
    }
    const QString said =
        apis.isEmpty() ? u"This qKVM names no version of its API"_s : u"This qKVM speaks %1 of its API"_s.arg(theirs);
    return {ServerClient::otherVersion,
            (said + u", and this program speaks %1. One of the two has to be updated."_s.arg(spokenApi)).toStdString()};
}

// What of these has not left this PC yet is taken back (ServerClient::lostContact), and so is what
// has left, has no answer begun, and is of what Qt sends again by itself where its line falls (all
// but a POST): a question that only asks, to be asked anew, anything else to be given up.
void takeBack(const QList<QPointer<QNetworkReply>>& out)
{
    for (const QPointer<QNetworkReply>& reply : out) {
        if (reply == nullptr || reply->isFinished()) {
            continue;
        }
        const QNetworkAccessManager::Operation operation = reply->operation();
        const bool sentAgainByQt =
            operation != QNetworkAccessManager::PostOperation && operation != QNetworkAccessManager::CustomOperation;
        if (reply->property("wentOut").toBool() && (!sentAgainByQt || reply->property("answerBegun").toBool())) {
            continue;
        }
        // (One that has failed by itself already, and only has not said so yet, is left to
        // say it: it goes nowhere any more, and Qt complains of a second end to it.)
        if (reply->error() != QNetworkReply::NoError) {
            continue;
        }
        const bool onlyAsks = operation == QNetworkAccessManager::GetOperation;
        reply->setProperty(onlyAsks ? "takenBack" : "lostMeanwhile", true);
        reply->abort();
    }
}

} // namespace

ServerClient::ServerClient(QObject* parent) : QObject(parent)
{
    prepare(m_network);
    m_clock.start();
    m_historyReads.later.setSingleShot(true);
    connect(&m_historyReads.later, &QTimer::timeout, this, [this] { nextInTurn(m_historyReads); });
}

void ServerClient::prepare(QNetworkAccessManager& network)
{
    // Never somewhere else by itself: a server that answers "go there" is not followed.
    network.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    // And straight to it, as the live line goes (M-D3): no proxy the application or Windows names
    // (WPAD, PAC), so none that could have Windows sign on to it with the user's account.
    network.setProxy(QNetworkProxy::NoProxy);
}

QNetworkReply* ServerClient::onALineOfItsOwn(const std::function<QNetworkReply*(QNetworkAccessManager&)>& send)
{
    // (Its own, until its answer is over and the reply gone: with this client at the latest.)
    auto* line = new QNetworkAccessManager(this); // NOLINT(cppcoreguidelines-owning-memory): this client's child
    prepare(*line);
    QNetworkReply* reply = send(*line);
    connect(reply, &QObject::destroyed, line, &QObject::deleteLater);
    return reply;
}

base::Result<QUrl> ServerClient::addressFrom(const QString& typed)
{
    QString text = typed.trimmed();
    if (text.isEmpty()) {
        return base::Error{"invalid", "Type the address of your qKVM, for example 192.168.1.10:8787."};
    }
    if (!text.contains("://"_L1)) {
        text.prepend("http://"_L1);
    }
    QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme();
    if (!url.isValid() || url.host().isEmpty() || (scheme != "http"_L1 && scheme != "https"_L1)) {
        return base::Error{
            "invalid", "That is not the address of a qKVM. It looks like 192.168.1.10:8787 or https://qkvm.example."};
    }
    if (!url.userInfo().isEmpty()) {
        return base::Error{
            "invalid",
            "An address with a name or a password in it is not taken: they are typed where they are asked for."};
    }
    if (!url.path().isEmpty() && url.path() != "/"_L1) {
        return base::Error{"invalid",
                           "Server URL path prefixes are not supported. Use the qKVM server's root address."};
    }
    // A display-link fragment is read separately; never turn a prefixed URL into another server.
    url.setPath(u"/"_s);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

base::Result<void> ServerClient::setAddress(const QString& typed)
{
    const base::Result<QUrl> url = addressFrom(typed);
    if (!url) {
        return url.error();
    }
    if (refusedForItsTls(m_windowsOwnTls, url.value())) {
        // Whoever would judge its certificate is not Windows: it is not named, and so nothing
        // of a call ever goes to it, the session and a password least of all.
        return damagedTls();
    }
    if (url.value() != m_address) {
        // Another server: what the last one's session was is nothing to it.
        letGoOfTheSession();
        // Nor is the last one's certificate this one's; and no line to the last one is used again.
        m_knownCertificate.clear();
        m_shownCertificate.clear();
        m_network.clearConnectionCache();
        // Nor is what was known of contact with the last one, or asked of it. The calls that
        // waited to hear what answers there are not sent, and are told so a turn later.
        m_unsure = false;
        m_askingWhatAnswers = false;
        m_linkWaitSaidMs = 0;
        // Nor is how long the last one asked to be left alone.
        m_signInHeldUntilMs = 0;
        m_liveHeldUntilMs = 0;
        m_liveHeld = {};
        ++m_serverTurn;
        forgetTheTurns(m_historyReads);
        const auto waiting = std::exchange(m_waiting, {});
        for (const auto& told : waiting) {
            QTimer::singleShot(0, this, [told] {
                told(base::Error{notSent, "Another qKVM was named before this was sent. Nothing was sent."});
            });
        }
    }
    m_address = url.value();
    return {};
}

QString ServerClient::asTyped(const QUrl& address)
{
    return address.scheme() == "https"_L1 ? address.toString(QUrl::RemovePath) : address.authority();
}

void ServerClient::setSession(const QByteArray& session)
{
    // What was kept comes from outside the program.
    if (isSession(session) && !m_address.isEmpty()) {
        m_session = session;
    }
}

void ServerClient::letGoOfTheSession()
{
    m_session.clear();
    ++m_sessionTurn;
}

void ServerClient::takeSessionFrom(const QNetworkReply& reply, quint64 turn)
{
    if (turn != m_sessionTurn) {
        return;
    }
    const auto given = reply.header(QNetworkRequest::SetCookieHeader).value<QList<QNetworkCookie>>();
    for (const QNetworkCookie& cookie : given) {
        // What a server hands out comes from outside the program as well.
        if (cookie.name() == sessionCookie && isSession(cookie.value())) {
            m_session = cookie.value();
        }
    }
}

QNetworkRequest ServerClient::request(const QString& path, int limitMs, bool withSession) const
{
    QNetworkRequest request(m_address.resolved(QUrl(path)));
    request.setTransferTimeout(limitMs);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    // The server's guard against other sites' pages: a call that changes something has to carry it,
    // and the others may.
    request.setRawHeader("X-QKVM"_ba, "1"_ba);
    request.setRawHeader("Accept"_ba, "application/json, image/jpeg"_ba);
    // The session is carried by hand. Qt's store of cookies is never written, and so has nothing
    // to add to a question.
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    if (withSession && !m_session.isEmpty()) {
        request.setRawHeader("Cookie"_ba, QByteArray(sessionCookie) + '=' + m_session);
    }
    // Nor does Qt sign on to anything by itself. Asked to in some ways (NTLM, Negotiate), it would
    // answer with the account of whoever is signed in to Windows, and send the question again, the
    // session with it: a server, or whatever answers in its place, is not handed that.
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    // One way of speaking, the plain one, as the KVM's driver has it: that a question has gone out
    // is told for it (sendOut), and on that hangs what is given up where contact is lost, and
    // whether something may have been done. Behind something that offers the other way, a
    // question that had left would count as not left until its answer began. (Set, and not shown
    // by a test: the tests' server speaks no other way.)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    // A line the server kept is let go of here once it has been idle idleLineSeconds (b11): a server closes a line it
    // keeps idle after a while of its own (qKVM's web server after five seconds, a proxy before it maybe sooner), and a
    // question written on it just as it closes never reaches it. Told as "could not be reached", and for an action
    // as something that may have been done, though nothing arrived; and nothing is sent twice to find out.
    request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, idleLineSeconds);
    if (isEncrypted() && !m_knownCertificate.isEmpty()) {
        // Known by its certificate alone: Windows vouches for nobody here and objects to every
        // certificate, the server's own included, so that the look in watchCertificate is made for
        // every line that is made, also for one that comes up when no question is waiting for it.
        QSslConfiguration line = QSslConfiguration::defaultConfiguration();
        line.setCaCertificates({});
        request.setSslConfiguration(line);
    }
    return request;
}

void ServerClient::setKnownCertificate(const QByteArray& sha256)
{
    if (sha256 != m_knownCertificate) {
        m_knownCertificate = sha256;
        // A line that was opened under the old rule is not used under the new one.
        m_network.clearConnectionCache();
    }
}

QString ServerClient::written(const QByteArray& sha256)
{
    return QString::fromLatin1(sha256.toHex(':')).toUpper();
}

void ServerClient::watchCertificate(QNetworkReply* reply)
{
    const auto shownBy = [](const QNetworkReply* from) {
        const QSslCertificate shown = from->sslConfiguration().peerCertificate();
        return shown.isNull() ? QByteArray() : shown.digest(QCryptographicHash::Sha256);
    };
    // Windows does not trust what the server showed. Where the server is known by a certificate,
    // that says nothing: Windows was given nobody to vouch for anybody (request), and objects to
    // every certificate there. Where it is known by none, that is the end of it.
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply, shownBy](const QList<QSslError>& errors) {
        // Decided here, by the certificate: what Qt is told to pass over holds for every line to
        // this server from now on, the ones that come up with no question waiting among them, and
        // those are looked at by nobody later. So it is told that only where the server showed the
        // certificate it is known by, and only where every one of Windows' objections names that
        // certificate itself. Each objection names a certificate: for one that nobody signed but
        // its own maker, that one; for one that somebody else signed, the signer's. Passing over
        // an objection to a signer would pass it over for every other certificate of that
        // signer's that is good for the same name, on a line that no question waits for (tried).
        // So a certificate that somebody else signed is not one a server is known by here: it is
        // let in where Windows trusts its signer, and by that.
        const QSslCertificate certificate = reply->sslConfiguration().peerCertificate();
        const QByteArray shown = shownBy(reply);
        const bool isTheOne = !m_knownCertificate.isEmpty() && shown == m_knownCertificate;
        const bool allNameIt = std::ranges::all_of(
            errors, [&certificate](const QSslError& objection) { return objection.certificate() == certificate; });
        if (isTheOne && allNameIt) {
            reply->ignoreSslErrors(errors);
            return;
        }
        m_shownCertificate = shown;
        if (isTheOne) {
            reply->setProperty("certificate", QByteArray(certificateSignerUnknown));
        } else {
            reply->setProperty("certificate",
                               QByteArray(m_knownCertificate.isEmpty() ? certificateUnknown : certificateChanged));
        }
    });
    // The two have agreed on how to speak, and nothing has been sent yet: the second look. A server
    // that is known by a certificate has to have shown exactly that one, whoever else vouches for
    // the one it showed.
    connect(reply, &QNetworkReply::encrypted, this, [this, reply, shownBy] {
        if (m_knownCertificate.isEmpty()) {
            return; // one that Windows trusts for its name
        }
        const QByteArray sha256 = shownBy(reply);
        if (sha256 != m_knownCertificate) {
            m_shownCertificate = sha256;
            reply->setProperty("certificate", QByteArray(certificateChanged));
            reply->abort();
        }
    });
}

void ServerClient::sendOut(const std::function<QNetworkReply*()>& send, qsizetype largest, bool signInCall, Answer done,
                           int takenBack)
{
    QNetworkReply* reply = send();
    // (Those that are over are forgotten here, and not when each goes: the last of them go
    // with this object, when the list is no longer there to be written to.)
    m_out.removeIf([](const QPointer<QNetworkReply>& was) { return was == nullptr || was->isFinished(); });
    m_out.append(reply);
    const auto wentOut = [reply] { reply->setProperty("wentOut", true); };
    connect(reply, &QNetworkReply::requestSent, reply, wentOut);
    connect(reply, &QNetworkReply::metaDataChanged, reply, wentOut);
    // (And its answer has begun: Qt sends nothing again by itself from then on.)
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply] { reply->setProperty("answerBegun", true); });
    if (reply->operation() != QNetworkAccessManager::GetOperation) {
        finish(reply, largest, signInCall, std::move(done));
        return;
    }
    // A question that only asks, and that was taken back before it left this PC because contact
    // was lost (lostContact), is not over for whoever asked: it is asked anew, which puts it
    // behind the question for what answers there now, and it goes out where that is a QKVM.
    // Whoever asked is told then, as of any call that was held back. Not where another server
    // was named meanwhile, or whoever was signed in was signed out (`send` makes the question
    // for the server and with the session there are when it is called); not where the question
    // had been stopped for the server's certificate, which is told as that; and not for ever:
    // then it is told as given up.
    finish(reply, largest, signInCall,
           [this, send, largest, signInCall, takenBack, of = m_serverTurn, turn = m_sessionTurn,
            done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply* from) {
               const bool again = from != nullptr && from->property("takenBack").toBool() &&
                                  from->property("certificate").toByteArray().isEmpty();
               if (again && takenBack < mostTakenBack && of == m_serverTurn && turn == m_sessionTurn) {
                   ask(send, largest, signInCall, done, takenBack + 1);
                   return;
               }
               done(answer, from);
           });
}

void ServerClient::lostContact(quint64 of)
{
    if (of != m_serverTurn || std::exchange(m_unsure, true)) {
        return;
    }
    ++m_lost;
    // What has left, and is answered already, or is a POST (which Qt never sends twice), stays out:
    // its session went with it, giving it up would take nothing back, and it would end what a QKVM
    // is answering, an action that takes its time among it. What has left and is not answered yet,
    // and is of what Qt sends again by itself where its line falls (a GET, a PUT), is taken back
    // as what has not left is (below): Qt would send it once more, on a line it makes to the
    // address by then, which Windows goes on trying to make for seconds, and that line may reach
    // whatever begins to listen there meanwhile (seen: a picture, session and all). A question
    // that only asks is asked anew, and whoever asked it hears nothing of that; the wall's own
    // question for how the machines are among them.
    // What has NOT left this PC yet is taken back, whatever it is: Windows goes on
    // trying to reach a place for seconds on behalf of a question, and Qt keeps what it has no
    // line for, and either would hand it, session and all, to whatever answers at the address
    // by then. What does something (a POST, a PUT) is given up with that: it does not follow a
    // server that was replaced with a password or an action. A question that only asks is asked
    // anew behind the question for what answers there now (sendOut), so that the wall's own look
    // is not lost for it. (For a while only what does something was taken back, and a look or a
    // picture that waited for a line went to a program that had taken the server's place.)
    // A turn later: giving one up tells whoever asked there and then, and this is called from
    // inside the answer to somebody else. Nothing more that carries the session is handed to Qt
    // meanwhile (ask). What Qt gets a line for before the taking back has reached it, it sends:
    // within that turn, and for as long after it as the word takes to reach Qt's own thread,
    // where its questions wait. That much is not closed, nor is the moment between a question's
    // leaving and Qt's word of it.
    QMetaObject::invokeMethod(this, [out = m_out] { takeBack(out); }, Qt::QueuedConnection);
}

void ServerClient::ask(std::function<QNetworkReply*()> send, qsizetype largest, bool signInCall, Answer done,
                       int takenBack)
{
    if (!m_unsure) {
        sendOut(send, largest, signInCall, std::move(done), takenBack);
        return;
    }
    // Contact was lost: not sent before what answers there now has said that it is a QKVM this
    // program speaks to. Nor after that, where the session it was to carry was let go of
    // meanwhile.
    m_waiting.append([this, send = std::move(send), largest, signInCall, done = std::move(done), takenBack,
                      turn = m_sessionTurn](const std::optional<base::Error>& why) {
        if (why) {
            done(*why, nullptr);
        } else if (turn != m_sessionTurn) {
            done(base::Error{notSent, "You were signed out before this was sent. Nothing was sent."}, nullptr);
        } else {
            sendOut(send, largest, signInCall, done, takenBack);
        }
    });
    if (!m_askingWhatAnswers) {
        askWhatAnswersNow();
    }
}

void ServerClient::askWhatAnswersNow()
{
    m_askingWhatAnswers = true;
    askHealth([this, of = m_serverTurn](const base::Result<Health>& health) {
        if (of != m_serverTurn) {
            return; // another server was named meanwhile: who waited was told there
        }
        m_askingWhatAnswers = false;
        std::optional<base::Error> why;
        if (!health) {
            // A certificate that is not the server's is told as that: whoever keeps a wall asks
            // about it. So is a QKVM that speaks another version of its API: it is there, and
            // will not be this program's however long it is asked, which whoever keeps a wall
            // says where a server is named. Anything else is "not sent", with what was found.
            const std::string& code = health.error().code;
            const bool toldAsItIs = code == certificateUnknown || code == certificateChanged ||
                                    code == certificateSignerUnknown || code == otherVersion;
            why = toldAsItIs ? health.error()
                             : base::Error{notSent, "qKVM is not there to be asked. Nothing was sent. (" +
                                                        health.error().message + ")"};
        } else if (m_unsure) {
            // A QKVM answered, to a question that somebody else's answer has overtaken: contact
            // was there again by that one, and was lost once more since this was asked. Asked
            // anew, for the calls that wait.
            askWhatAnswersNow();
            return;
        }
        // (Whoever is told may ask again there and then: that one waits for the next asking.)
        const QPointer<ServerClient> self(this);
        const auto waiting = std::exchange(m_waiting, {});
        for (const auto& told : waiting) {
            if (self == nullptr) {
                return;
            }
            told(why);
        }
    });
}

void ServerClient::finish(QNetworkReply* reply, qsizetype largest, bool signInCall, Answer done)
{
    watchCertificate(reply);
    // An answer that says it is too large, or that turns out to be, is cut off: nothing of it is kept.
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, largest] {
        const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
        if (length.isValid() && length.toLongLong() > largest) {
            reply->setProperty("tooLarge", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::readyRead, reply, [reply, largest] {
        if (reply->bytesAvailable() > largest) {
            reply->setProperty("tooLarge", true);
            reply->abort();
        }
    });
    // Nor may an answer take as long as it likes, a byte at a time. The limit a question is sent
    // with (request()) is on how long nothing comes, and what dribbles never reaches it: whoever
    // waits for the answer would wait for hours. So the whole of a call has a limit too, twice
    // as long as it may be silent for, and every call alike. What keeps that time goes with its
    // answer: it is held by what hears the answer, and by nothing else. (A timer that only fires
    // once lives out its whole time whether or not anybody waits for it: thirty seconds for
    // every picture of a wall.)
    const auto whole = std::make_shared<QTimer>();
    whole->setSingleShot(true);
    ++*m_limits;
    connect(whole.get(), &QObject::destroyed, whole.get(), [alive = m_limits] { --*alive; });
    connect(whole.get(), &QTimer::timeout, reply, [reply] {
        if (!reply->isFinished()) {
            reply->setProperty("gaveUp", true);
            reply->abort();
        }
    });
    // (An upload names its own: it may take hours, and is given as long as it moves: uploadMedia.)
    const QVariant wholeMs = reply->property("wholeWithinMs");
    whole->start(wholeMs.isValid() ? wholeMs.toInt() : wholeWithinTimes * reply->request().transferTimeout());
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, whole, signInCall, of = m_serverTurn, done = std::move(done)] {
            whole->stop();
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool saidYes = status >= 200 && status < 300;
            // A picture of a machine's screen may be larger than is taken, and the server fetches
            // it from the machine while it is asked: an answer to that question that is too large
            // or too slow says nothing of who answers. That picture fails, and nothing else.
            const bool aPicture = reply->property("picture").toBool();
            // Nothing came for as long as nothing may: the limit the question was sent with, which
            // Qt keeps and tells so (seen). It tells a line that did not come up in time the
            // same way; of that one the question never went.
            const bool nothingCameInTime = reply->error() == QNetworkReply::TimeoutError;
            if (const QByteArray certificate = reply->property("certificate").toByteArray(); !certificate.isEmpty()) {
                // (No matter of contact: nothing was sent, and whoever keeps the wall asks about
                // the certificate.)
                const char* sentence = "This qKVM shows a certificate that Windows does not know.";
                if (certificate == certificateChanged) {
                    sentence = "This qKVM shows another certificate than the one it is known by.";
                } else if (certificate == certificateSignerUnknown) {
                    sentence = "This qKVM's certificate was signed by a maker of certificates that Windows does not "
                               "know. Have Windows trust that maker, and this qKVM is let in by that.";
                }
                done(base::Error{certificate.toStdString(), sentence}, reply);
            } else if (reply->property("stopped").toBool()) {
                // Stopped by whoever began it (an upload): nothing of who answers, and nothing to wait for.
                done(base::Error{uploadStopped, "The upload was stopped before all of it had gone."}, reply);
            } else if (reply->property("tooLarge").toBool()) {
                // Cut off, and read by nobody: not the server's own words, whatever they were
                // (noted for whoever asked something to be done: act).
                reply->setProperty("notTheServers", true);
                // Of who answers: nothing, where it is a picture that was begun with a yes. Any
                // other answer of a QKVM's has a shape, and none of them is that large: said by
                // something else.
                if (!aPicture || !saidYes) {
                    lostContact(of);
                }
                done(base::Error{"too_large", "The server's answer is larger than this program takes."}, reply);
            } else if (reply->property("gaveUp").toBool()) {
                // Whatever had come of it by then is no answer: not a part of one either. Of who
                // answers: no QKVM takes twice a call's time over an answer, a byte now and then.
                // Unless it is a picture, which only is too slow.
                if (!aPicture) {
                    lostContact(of);
                }
                done(base::Error{unreachable, "qKVM did not answer in time."}, reply);
            } else if (reply->property("lostMeanwhile").toBool() || reply->property("takenBack").toBool()) {
                // Given up here (lostContact). Told as a server that did not answer, and not as
                // something that was never sent: Qt's word that a question went comes a moment
                // after it went. (A question that only asks is asked anew by who sent it out,
                // sendOut, and whoever asked it hears this only where that is not done.)
                done(base::Error{unreachable, "qKVM did not answer: contact with it was lost meanwhile."}, reply);
            } else if (status == 0 && aPicture && nothingCameInTime && reply->property("wentOut").toBool()) {
                // The question for a picture left, on a line that stood, and nothing came in a
                // picture's time: the server is still fetching it. Too slow, and no more.
                done(base::Error{unreachable, "qKVM did not answer in time."}, reply);
            } else if (status == 0) {
                // No answer at all: refused, no route, the line fell, nothing came in time, a
                // certificate that is not trusted. Of who answers: nobody may, or anybody.
                lostContact(of);
                done(notReached(*reply), reply);
            } else if (!saidYes) {
                // In the server's own shape it is the server's word, and its 401 the news that the
                // session is gone (but for a sign-in, where it is the server's word about what was
                // typed). In any other shape it was said by something else in the server's place:
                // noted on the answer for whoever asked something to be done (act), and contact
                // lost. A 401 among them: what wants a sign-in of its own for everything signs
                // nobody out here.
                const QByteArray said = reply->readAll();
                const bool itsOwnWords = isRefusalOfAServers(said);
                reply->setProperty(itsOwnWords ? "itsOwnWords" : "notTheServers", true);
                if (!itsOwnWords) {
                    lostContact(of);
                }
                done(itsOwnWords && status == 401 && !signInCall ? base::Error{signedOut, "You are signed out."}
                                                                 : refusalFrom(status, said),
                     reply);
            } else if (reply->error() != QNetworkReply::NoError) {
                // A good answer was begun, and not all of it came: the line fell under it, or it
                // fell silent. What came is not read as the answer: half a picture would be handed
                // on as a picture, and words that happen to read whole would be taken for the
                // server's. Told as a server that did not answer, which is what leaves open whether
                // something that was asked of a machine was done. Of who answers: a line that
                // fell may have fallen with the server. A picture that fell silent is too slow.
                if (!aPicture || !nothingCameInTime) {
                    lostContact(of);
                }
                done(base::Error{unreachable, "qKVM's answer broke off before all of it had come."}, reply);
            } else {
                done(reply->readAll(), reply);
            }
        });
}

void ServerClient::askHealth(std::function<void(base::Result<Health>)> done)
{
    // (Its 401 would not be the news that a session is gone: none went with the question.)
    finish(m_network.get(request(u"healthz"_s, answerWithinMs, false)), largestHealthBytes, true,
           [this, of = m_serverTurn, lost = m_lost, done = std::move(done)](const base::Result<QByteArray>& answer,
                                                                            QNetworkReply*) {
               if (!answer && isOwnFinding(answer.error())) {
                   done(answer.error());
                   return;
               }
               // Something answered. What is no QKVM is said in one way, whatever it sent: a page, a
               // refusal of its own, more than a QKVM says of itself.
               const base::Result<Health> health = healthFrom(answer ? answer.value() : QByteArray());
               const bool speaksOurs = health && health.value().apis.contains(spokenApi);
               // What is no QKVM this program speaks to, where it says what it is: contact is
               // lost, or stays lost, whatever it answered with. A QKVM this program speaks to
               // brings contact back, and what carries the session goes out again, where it was
               // asked after contact was last lost: whoever asked, the wall's keeper before a
               // password as well as this class for a call that waits, since what counts is that
               // the question left after that moment. Asked before it, the answer may be the
               // last word of what was there then, and says nothing of what answers now.
               if (!speaksOurs) {
                   lostContact(of);
               } else if (of == m_serverTurn && lost == m_lost) {
                   m_unsure = false;
               }
               if (health && !speaksOurs) {
                   done(speaksOtherVersions(health.value().apis));
                   return;
               }
               done(health);
           });
}

void ServerClient::askSession(std::function<void(base::Result<Session>)> done)
{
    // (Where a proxy in front of the server vouches for somebody, this answer signs them in.)
    ask([this] { return m_network.get(request(u"api/v1/session"_s, answerWithinMs)); }, largestAnswerBytes, true,
        [this, turn = m_sessionTurn, of = m_serverTurn, done = std::move(done)](base::Result<QByteArray> answer,
                                                                                QNetworkReply* reply) {
            if (!answer) {
                done(answer.error());
                return;
            }
            // A QKVM always answers this with who is signed in, or that nobody is. What is none
            // of that was said by something else: contact is lost, and no session is taken
            // from it.
            base::Result<Session> session = sessionFrom(answer.value());
            if (session) {
                takeSessionFrom(*reply, turn);
            } else {
                lostContact(of);
            }
            done(std::move(session));
        });
}

void ServerClient::signIn(const QString& name, const QString& password, const QString& code, bool keepSignedIn,
                          std::function<void(base::Result<Account>)> done)
{
    // Locked out for as long as the server said: nothing goes to it before that time.
    if (const qint64 left = m_signInHeldUntilMs - m_clock.elapsed(); left > 0) {
        base::Error held{"rate_limited",
                         "Too many attempts. Try again in " + std::to_string((left + 999) / 1000) + " s."};
        QTimer::singleShot(0, this, [done = std::move(done), held = std::move(held)] { done(held); });
        return;
    }
    const QJsonObject body{
        {u"name"_s, name}, {u"password"_s, password}, {u"code"_s, code}, {u"remember"_s, keepSignedIn}};
    ask(
        [this, said = QJsonDocument(body).toJson(QJsonDocument::Compact)] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.post(asJson(request(u"api/v1/login"_s, answerWithinMs)), said);
            });
        },
        largestAnswerBytes, true,
        [this, turn = m_sessionTurn, of = m_serverTurn, done = std::move(done)](base::Result<QByteArray> answer,
                                                                                QNetworkReply* reply) {
            if (answer) {
                takeSessionFrom(*reply, turn);
            } else if (const int wait = waitSaidMs(reply, longestSignInWaitMs); wait > 0 && of == m_serverTurn) {
                m_signInHeldUntilMs = m_clock.elapsed() + wait;
            }
            done(answer ? accountFromSignIn(answer.value()) : base::Result<Account>(answer.error()));
        });
}

void ServerClient::signOut(std::function<void(base::Result<void>)> done)
{
    // The question carries the session that is to be ended. Here it is let go of before the answer:
    // a sign-out that never reaches the server leaves nothing behind to come back in with.
    QNetworkRequest asked = request(u"api/v1/logout"_s, answerWithinMs);
    asked.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    letGoOfTheSession();
    ask([this,
         asked] { return onALineOfItsOwn([&](QNetworkAccessManager& line) { return line.post(asked, "{}"_ba); }); },
        largestAnswerBytes, true,
        [done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply*) {
            done(answer ? base::Result<void>() : base::Result<void>(answer.error()));
        });
}

void ServerClient::openDisplay(const QByteArray& key, std::function<void(base::Result<Account>)> done)
{
    const QJsonObject body{{u"key"_s, QString::fromLatin1(key)}};
    // Its refusal is the server's own word about the link, and not the news that a session is gone.
    ask(
        [this, said = QJsonDocument(body).toJson(QJsonDocument::Compact)] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.post(asJson(request(u"api/v1/display"_s, answerWithinMs)), said);
            });
        },
        largestAnswerBytes, true,
        [this, turn = m_sessionTurn, of = m_serverTurn, done = std::move(done)](base::Result<QByteArray> answer,
                                                                                QNetworkReply* reply) {
            if (answer) {
                takeSessionFrom(*reply, turn);
            }
            // How long it asks to be left alone, where it says so with a refusal: in whole
            // seconds, as a server says it (a date there is not read). Only where the refusal is
            // in the server's own shape: what a gateway's page names is the gateway's guess, and
            // would hold a wall screen for as long while its QKVM is back in a minute. And no
            // longer than this program waits by itself at the longest, whoever says so. (Of the
            // server that is spoken to now: not of one before it.)
            if (of == m_serverTurn) {
                const bool itsOwnWords = !answer && reply != nullptr && reply->property("itsOwnWords").toBool();
                bool isNumber = false;
                const qlonglong seconds =
                    itsOwnWords ? reply->rawHeader("Retry-After"_ba).trimmed().toLongLong(&isNumber) : 0;
                m_linkWaitSaidMs =
                    isNumber ? static_cast<int>(std::clamp<qlonglong>(seconds, 0, longestLinkWaitMs / 1000) * 1000) : 0;
            }
            done(answer ? accountFromSignIn(answer.value()) : base::Result<Account>(answer.error()));
        });
}

QByteArray ServerClient::displayKeyIn(const QString& typed)
{
    // As the page reads it: behind the "#", a key of the server's making and of no other shape.
    static const QRegularExpression key(u"#(?:.*&)?key=(qkvm_[A-Za-z0-9_-]{20,80})(?:&.*)?$"_s);
    const QRegularExpressionMatch found = key.match(typed.trimmed());
    return found.hasMatch() ? found.captured(1).toLatin1() : QByteArray();
}

void ServerClient::askWall(std::function<void(base::Result<Wall>)> done)
{
    ask([this] { return m_network.get(request(u"api/v1/wall"_s, answerWithinMs)); }, largestAnswerBytes, false,
        [this, of = m_serverTurn, done = std::move(done)](base::Result<QByteArray> answer, QNetworkReply*) {
            if (!answer) {
                done(answer.error());
                return;
            }
            // A wall keeps asking this by itself, and a QKVM always answers it with its
            // machines. An answer that is no wall was not a QKVM's: contact is lost, and the
            // next asking has what answers there asked what it is.
            base::Result<Wall> wall = wallFrom(answer.value());
            if (!wall) {
                lostContact(of);
            }
            done(std::move(wall));
        });
}

void ServerClient::askHistory(const QString& machine, std::function<void(base::Result<HistorySummary>)> done)
{
    auto unsent = [done](const base::Error& why) { done(why); };
    readInTurn(
        m_historyReads,
        [this, machine, done = std::move(done)](const std::function<void(QNetworkReply*)>& over) {
            ask([this, machine] { return m_network.get(request(ofMachine(machine, u"history"_s), answerWithinMs)); },
                largestAnswerBytes, false,
                [over, done](const base::Result<QByteArray>& answer, QNetworkReply* reply) {
                    over(reply);
                    done(answer ? historySummaryFrom(answer.value()) : base::Result<HistorySummary>(answer.error()));
                });
        },
        std::move(unsent));
}

void ServerClient::askHistorySamples(const QString& machine, std::function<void(base::Result<HistorySamples>)> done)
{
    auto unsent = [done](const base::Error& why) { done(why); };
    readInTurn(
        m_historyReads,
        [this, machine, done = std::move(done)](const std::function<void(QNetworkReply*)>& over) {
            ask(
                [this, machine] {
                    return m_network.get(
                        request(ofMachine(machine, u"history?what=samples&every=900&limit=5000"_s), answerWithinMs));
                },
                largestAnswerBytes, false,
                [over, done](const base::Result<QByteArray>& answer, QNetworkReply* reply) {
                    over(reply);
                    done(answer ? historySamplesFrom(answer.value()) : base::Result<HistorySamples>(answer.error()));
                });
        },
        std::move(unsent));
}

void ServerClient::readInTurn(InTurn& reads, std::function<void(std::function<void(QNetworkReply*)>)> go,
                              std::function<void(const base::Error&)> unsent)
{
    reads.waiting.append(InTurn::Waiting{std::move(go), std::move(unsent), m_serverTurn, m_sessionTurn});
    nextInTurn(reads);
}

void ServerClient::nextInTurn(InTurn& reads)
{
    while (!reads.out && !reads.waiting.isEmpty()) {
        // Not before the time the server named.
        if (const qint64 left = reads.heldUntilMs - m_clock.elapsed(); left > 0) {
            reads.later.start(static_cast<int>(left));
            return;
        }
        InTurn::Waiting one = reads.waiting.takeFirst();
        if (one.serverTurn != m_serverTurn || one.sessionTurn != m_sessionTurn) {
            // Asked of another server, or of a session that is gone: nothing of it goes now.
            QTimer::singleShot(0, this, [told = std::move(one.unsent)] {
                told(base::Error{notSent, "Another qKVM was named, or you were signed out, before this was asked. "
                                          "Nothing was sent."});
            });
            continue;
        }
        reads.out = true;
        one.go([this, &reads, of = m_serverTurn](QNetworkReply* reply) {
            reads.out = false;
            if (const int wait = waitSaidMs(reply, longestReadWaitMs); wait > 0 && of == m_serverTurn) {
                reads.heldUntilMs = std::max(reads.heldUntilMs, m_clock.elapsed() + wait);
            }
            // (A turn later: this is said from inside an answer, whose own word comes after it.)
            QTimer::singleShot(0, this, [this, &reads] { nextInTurn(reads); });
        });
    }
}

void ServerClient::forgetTheTurns(InTurn& reads)
{
    reads.heldUntilMs = 0;
    reads.later.stop();
    const auto waiting = std::exchange(reads.waiting, {});
    for (const InTurn::Waiting& one : waiting) {
        QTimer::singleShot(0, this, [told = one.unsent] {
            told(base::Error{notSent, "Another qKVM was named before this was sent. Nothing was sent."});
        });
    }
}

void ServerClient::askDeviceActivity(const QString& machine, std::function<void(base::Result<DeviceActivity>)> done)
{
    ask(
        [this, machine] {
            return m_network.get(request(u"api/v1/audit?device="_s +
                                             QString::fromLatin1(QUrl::toPercentEncoding(machine)) + u"&limit=100"_s,
                                         answerWithinMs));
        },
        largestAnswerBytes, false,
        [done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply*) {
            done(answer ? deviceActivityFrom(answer.value()) : base::Result<DeviceActivity>(answer.error()));
        });
}

void ServerClient::askStatus(std::function<void(base::Result<QHash<QString, Status>>)> done)
{
    ask([this] { return m_network.get(request(u"api/v1/status"_s, answerWithinMs)); }, largestAnswerBytes, false,
        [this, of = m_serverTurn, done = std::move(done)](base::Result<QByteArray> answer, QNetworkReply*) {
            if (!answer) {
                done(answer.error());
                return;
            }
            // (As for the wall: asked by itself every few seconds, and what is no list of how
            // the machines are was not a QKVM's answer.)
            base::Result<QHash<QString, Status>> status = statusFrom(answer.value());
            if (!status) {
                lostContact(of);
            }
            done(std::move(status));
        });
}

QString ServerClient::ofMachine(const QString& machine, const QString& what)
{
    // The machine's name in the address is the server's own, and still not trusted to be only letters.
    return u"api/v1/devices/"_s + QString::fromLatin1(QUrl::toPercentEncoding(machine)) + u'/' + what;
}

void ServerClient::askLive(const QString& machine, std::function<void(base::Result<QByteArray>)> done)
{
    // An account's lines are counted together: where the server said it has as many as it may, and for how long, no
    // pass of any machine is asked for before that time. Told in the words it was refused in.
    if (m_clock.elapsed() < m_liveHeldUntilMs) {
        QTimer::singleShot(0, this, [done = std::move(done), said = m_liveHeld] { done(said); });
        return;
    }
    ask(
        [this, machine] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.post(asJson(request(ofMachine(machine, u"live"_s), answerWithinMs)), "{}"_ba);
            });
        },
        largestAnswerBytes, false,
        [this, of = m_serverTurn, done = std::move(done)](const base::Result<QByteArray>& answer,
                                                          QNetworkReply* reply) {
            if (!answer) {
                if (const int wait = waitSaidMs(reply, longestLiveWaitMs); wait > 0 && of == m_serverTurn) {
                    m_liveHeldUntilMs = m_clock.elapsed() + wait;
                    m_liveHeld = answer.error();
                }
            }
            done(answer ? livePassFrom(answer.value()) : base::Result<QByteArray>(answer.error()));
        });
}

std::unique_ptr<LiveLink> ServerClient::openLive(const QString& machine, QByteArray pass, LiveLink::Need need,
                                                 LiveLink::Watch watch)
{
    QUrl where = m_address.resolved(QUrl(ofMachine(machine, u"live"_s)));
    where.setScheme(isEncrypted() ? u"wss"_s : u"ws"_s);
    QNetworkRequest asked(where);
    // What every call carries to say that it is this program's; and not the session, which is
    // what the pass is for.
    asked.setRawHeader("X-QKVM"_ba, "1"_ba);
    return std::make_unique<LiveLink>(asked, std::move(pass), need, m_knownCertificate, watch);
}

void ServerClient::openLive(const QString& machine, Live::Need need,
                            std::function<void(base::Result<std::unique_ptr<Live>>)> done)
{
    askLive(machine, [this, machine, need, of = m_address, done = std::move(done)](base::Result<QByteArray> pass) {
        if (!pass) {
            done(pass.error());
            return;
        }
        WipedOnLeaving ticket(pass.value());
        if (of != m_address) {
            // Another server was named meanwhile. The pass is the one before's, and is said to nobody.
            done(base::Error{unreachable, "qKVM was left before its live picture could be opened."});
            return;
        }
        done(std::unique_ptr<Live>(openLive(machine, ticket.handOn(), need)));
    });
}

void ServerClient::exportWall(const QString& password, const QString& passphrase, const QStringList& parts,
                              std::function<void(base::Result<WallFile>)> done)
{
    // (A password that is not right is answered with 401, as a sign-in is: the server's word for
    // it is what is told, and nobody is signed out by it.)
    ask(
        [this, said = exportBody(password, passphrase, parts)] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.post(asJson(request(u"api/v1/export"_s, fileWithinMs)), said);
            });
        },
        largestFileBytes, true,
        [done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply* reply) {
            done(answer ? wallFileFrom(answer.value(), reply->rawHeader("X-QKVM-Not-Opened"_ba))
                        : base::Result<WallFile>(answer.error()));
        });
}

void ServerClient::readFile(const QByteArray& file, const QString& passphrase,
                            std::function<void(base::Result<FileContents>)> done)
{
    ask(
        [this, said = importBody(file, passphrase, nullptr)] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.post(asJson(request(u"api/v1/import/read"_s, fileWithinMs)), said);
            });
        },
        largestFileBytes, false,
        [done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply*) {
            done(answer ? fileContentsFrom(answer.value()) : base::Result<FileContents>(answer.error()));
        });
}

void ServerClient::takeFile(const QByteArray& file, const QString& passphrase, const QStringList& parts,
                            std::function<void(base::Result<Taken>)> done)
{
    ask(
        [this, said = importBody(file, passphrase, &parts)] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.post(asJson(request(u"api/v1/import"_s, takeWithinMs)), said);
            });
        },
        largestAnswerBytes, false,
        [done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply*) {
            done(answer ? takenFrom(answer.value()) : base::Result<Taken>(answer.error()));
        });
}

void ServerClient::arrange(const QStringList& machines, std::function<void(base::Result<QStringList>)> done)
{
    const QJsonObject body{{u"ids"_s, QJsonArray::fromStringList(machines)}};
    ask(
        [this, said = QJsonDocument(body).toJson(QJsonDocument::Compact)] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.put(asJson(request(u"api/v1/wall/order"_s, answerWithinMs)), said);
            });
        },
        largestAnswerBytes, false,
        [done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply*) {
            done(answer ? orderFrom(answer.value()) : base::Result<QStringList>(answer.error()));
        });
}

void ServerClient::act(const QString& machine, const Action& what, std::function<void(base::Result<Done>)> done)
{
    ask(
        [this, machine, said = bodyOf(what), within = what.kind == "bios_setup"_L1 ? setupWithinMs : doneWithinMs] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.post(asJson(request(ofMachine(machine, u"actions"_s), within)), said);
            });
        },
        largestAnswerBytes, false,
        [this, of = m_serverTurn, done = std::move(done)](base::Result<QByteArray> answer, QNetworkReply* reply) {
            // Only the server's own no is a no to something that was to be done. Whatever else
            // came of a question that went out leaves open whether the machine did it, and is
            // told as no answer of the server's: never as a refusal, after which the button
            // would be offered again.
            const int status =
                reply == nullptr ? 0 : reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool saidYes = status >= 200 && status < 300;
            if (answer) {
                base::Result<Done> read = doneFrom(answer.value());
                if (read) {
                    done(std::move(read));
                    return;
                }
                // It said yes, and what it said with that is not understood. A QKVM's yes to
                // this always reads as one: said by something else, and contact is lost.
                lostContact(of);
                done(base::Error{unreachable, "qKVM said yes, and what it said with that is not understood."});
            } else if (reply != nullptr && reply->property("tooLarge").toBool()) {
                // The answer was cut off here for its size, and nobody read it: a yes, or
                // whatever else its number says, it is no refusal of anybody's.
                done(base::Error{unreachable,
                                 saidYes ? "qKVM said yes, and its answer was cut off: it is larger than is taken."
                                         : "The answer was cut off, since it is larger than is taken: whether it "
                                           "was a refusal is not known."});
            } else if (status >= 500 && reply->property("notTheServers").toBool()) {
                // An error of what stands before the server, in the middle of what was asked: a
                // gateway that gave up on a long action, or lost the server under it.
                done(base::Error{unreachable, "qKVM did not answer: what stands before it reported an error (" +
                                                  std::to_string(status) + ")."});
            } else if (status == 500) {
                done(itsOwnError(answer.error()));
            } else {
                done(answer.error());
            }
        });
}

base::Error ServerClient::itsOwnError(const base::Error& said)
{
    // The server's own 500: an error inside it, which its middleware answers for any error in a handler, one raised
    // after what it sent to the machine went out among them (the web's server.py, `guard`). Whatever its word, it may
    // have been done, as the page has it (its `unanswered` takes `internal`): never a refusal (from the merged review,
    // 5, 2026-11-06). Its sentence is kept.
    return {"internal", said.message};
}

bool ServerClient::mayHaveBeenDone(const QString& /*machine*/, const base::Error& error) const
{
    // "timeout" and "dropped" are the server's own words for it: what it sent to the machine went out,
    // and no answer came, or a broken one (on a line that was taken away meanwhile, or that broke off).
    // Neither is a refusal, and neither is offered again. Nor is its own 500, "internal": an error inside
    // it, after which what it sent may have gone out (itsOwnError). Its other words for the machine
    // (unreachable, no_stream and the like) say that nothing was sent, and are refusals.
    return error.code == unreachable || error.code == "timeout" || error.code == "dropped" || error.code == "internal";
}

void ServerClient::askConsole(const QString& machine, std::function<void(base::Result<QUrl>)> done)
{
    ask(
        [this, machine] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.post(asJson(request(ofMachine(machine, u"console"_s), consoleWithinMs)), "{}"_ba);
            });
        },
        largestAnswerBytes, false,
        [done = std::move(done)](base::Result<QByteArray> answer, QNetworkReply*) {
            done(answer ? consoleFrom(answer.value()) : base::Result<QUrl>(answer.error()));
        });
}

void ServerClient::askSavedActions(std::function<void(base::Result<QList<SavedAction>>)> done)
{
    ask([this] { return m_network.get(request(u"api/v1/actions"_s, answerWithinMs)); }, largestAnswerBytes, false,
        [done = std::move(done)](base::Result<QByteArray> answer, QNetworkReply*) {
            done(answer ? savedActionsFrom(answer.value()) : base::Result<QList<SavedAction>>(answer.error()));
        });
}

void ServerClient::askWakeTargets(const QString& machine, std::function<void(base::Result<QList<WakeTarget>>)> done)
{
    ask([this, machine] { return m_network.get(request(ofMachine(machine, u"wol"_s), answerWithinMs)); },
        largestAnswerBytes, false,
        [done = std::move(done)](base::Result<QByteArray> answer, QNetworkReply*) {
            done(answer ? wakeTargetsFrom(answer.value()) : base::Result<QList<WakeTarget>>(answer.error()));
        });
}

void ServerClient::setView(const QString& machine, const QString& view, std::function<void(base::Result<QString>)> done)
{
    const QJsonObject body{{u"view"_s, view}};
    ask(
        [this, machine, said = QJsonDocument(body).toJson(QJsonDocument::Compact)] {
            return onALineOfItsOwn([&](QNetworkAccessManager& line) {
                return line.put(asJson(request(ofMachine(machine, u"view"_s), answerWithinMs)), said);
            });
        },
        largestAnswerBytes, false,
        [done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply*) {
            done(answer ? amtViewFrom(answer.value()) : base::Result<QString>(answer.error()));
        });
}

void ServerClient::askAlertPicture(const QString& machine, const QString& stamp,
                                   std::function<void(base::Result<QByteArray>)> done)
{
    // Only a stamp of the server's making goes into the address: digits, as many as it writes.
    static const QRegularExpression isStamp(u"\\A[0-9]{1,16}\\z"_s);
    if (!isStamp.match(stamp).hasMatch()) {
        QTimer::singleShot(0, this, [done = std::move(done)] {
            done(base::Error{"invalid", "That picture is not one the server names. Nothing was asked."});
        });
        return;
    }
    ask(
        [this, asked = ofMachine(machine, u"alert-picture/"_s + stamp)] {
            QNetworkReply* reply = m_network.get(request(asked, pictureWithinMs));
            // (What is too large or too slow of this one says nothing of who answers, as of a snapshot.)
            reply->setProperty("picture", true);
            return reply;
        },
        largestPictureBytes, false,
        [this, of = m_serverTurn, done = std::move(done)](base::Result<QByteArray> answer, QNetworkReply* reply) {
            if (!answer) {
                done(answer.error());
                return;
            }
            // A picture is a picture, as a snapshot is one: what says otherwise was said by
            // something else, and contact is lost.
            base::Result<Snapshot> picture =
                snapshotFrom(std::move(answer).value(), reply->rawHeader("Content-Type"_ba), {});
            if (!picture) {
                lostContact(of);
                done(picture.error());
                return;
            }
            done(std::move(picture.value().picture));
        });
}

void ServerClient::askSnapshot(const QString& machine, int width, double mayBeOldSeconds,
                               std::function<void(base::Result<Snapshot>)> done)
{
    QUrl path(ofMachine(machine, u"snapshot"_s));
    QUrlQuery query;
    query.addQueryItem(u"w"_s, QString::number(std::clamp(width, 1, 16384)));
    query.addQueryItem(u"age"_s, QString::number(std::clamp(mayBeOldSeconds, 0.0, 3600.0), 'f', 2));
    path.setQuery(query);
    ask(
        [this, asked = path.toString(QUrl::FullyEncoded)] {
            QNetworkReply* reply = m_network.get(request(asked, pictureWithinMs));
            // (What is too large or too slow of this one says nothing of who answers: finish.)
            reply->setProperty("picture", true);
            return reply;
        },
        largestPictureBytes, false,
        [this, of = m_serverTurn, done = std::move(done)](base::Result<QByteArray> answer, QNetworkReply* reply) {
            if (!answer) {
                done(answer.error());
                return;
            }
            // A picture is a picture. A QKVM answers this with one, which says what it is and
            // begins as that; a yes with anything else (a page, another program's words) was said
            // by something else: contact is lost.
            base::Result<Snapshot> snapshot = snapshotFrom(
                std::move(answer).value(), reply->rawHeader("Content-Type"_ba), reply->rawHeader("X-KVM-Signal"_ba));
            if (!snapshot) {
                lostContact(of);
            } else {
                // (A still kept between sessions, and how old it is: the tile says it.)
                readStill(snapshot.value(), reply->rawHeader("X-KVM-Still"_ba), reply->rawHeader("X-KVM-Age"_ba));
            }
            done(std::move(snapshot));
        });
}

base::Result<QByteArray> ServerClient::doneOrNot(base::Result<QByteArray> answer, const QNetworkReply* reply,
                                                 quint64 of, const std::function<bool(const QByteArray&)>& understood)
{
    // As act() reads what came back to something that was to be done: only the server's own no is a no.
    const int status = reply == nullptr ? 0 : reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool saidYes = status >= 200 && status < 300;
    if (answer) {
        if (understood(answer.value())) {
            return answer;
        }
        lostContact(of);
        return base::Error{unreachable, "qKVM said yes, and what it said with that is not understood."};
    }
    if (reply != nullptr && reply->property("tooLarge").toBool()) {
        return base::Error{unreachable, saidYes ? "qKVM said yes, and its answer was cut off: it is larger than is "
                                                  "taken."
                                                : "The answer was cut off, since it is larger than is taken: whether "
                                                  "it was a refusal is not known."};
    }
    if (status >= 500 && reply->property("notTheServers").toBool()) {
        return base::Error{unreachable, "qKVM did not answer: what stands before it reported an error (" +
                                            std::to_string(status) + ")."};
    }
    if (status == 500) {
        return itsOwnError(answer.error());
    }
    return answer.error();
}

void ServerClient::askMedia(const QString& machine, std::function<void(base::Result<MediaList>)> done)
{
    ask([this, machine] { return m_network.get(request(ofMachine(machine, u"media"_s), mediaListWithinMs)); },
        largestAnswerBytes, false,
        [done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply*) {
            done(answer ? mediaListFrom(answer.value()) : base::Result<MediaList>(answer.error()));
        });
}

void ServerClient::askUpload(const QString& machine,
                             std::function<void(base::Result<std::optional<MediaUploading>>)> done)
{
    ask([this, machine] { return m_network.get(request(ofMachine(machine, u"media/upload"_s), answerWithinMs)); },
        largestAnswerBytes, false,
        [done = std::move(done)](const base::Result<QByteArray>& answer, QNetworkReply*) {
            done(answer ? uploadingFrom(answer.value()) : base::Result<std::optional<MediaUploading>>(answer.error()));
        });
}

void ServerClient::changeMedia(const QString& path, const QByteArray& body,
                               std::function<void(base::Result<MediaDone>)> done)
{
    ask(
        [this, path, body] {
            return onALineOfItsOwn(
                [&](QNetworkAccessManager& line) { return line.post(asJson(request(path, mediaWithinMs)), body); });
        },
        largestAnswerBytes, false,
        [this, of = m_serverTurn, done = std::move(done)](base::Result<QByteArray> answer, QNetworkReply* reply) {
            const base::Result<QByteArray> said =
                doneOrNot(std::move(answer), reply, of, [](const QByteArray& yes) { return mediaDoneFrom(yes).ok(); });
            done(said ? mediaDoneFrom(said.value()) : base::Result<MediaDone>(said.error()));
        });
}

void ServerClient::mountMedia(const QString& machine, const MediaMount& mount,
                              std::function<void(base::Result<MediaDone>)> done)
{
    changeMedia(ofMachine(machine, u"media"_s), mountBody(mount), std::move(done));
}

void ServerClient::unmountMedia(const QString& machine, const QString& image, const QString& named,
                                std::function<void(base::Result<MediaDone>)> done)
{
    changeMedia(ofMachine(machine, u"media/unmount"_s), imageBody(image, named), std::move(done));
}

void ServerClient::deleteMedia(const QString& machine, const QString& image, const QString& named,
                               std::function<void(base::Result<MediaDone>)> done)
{
    changeMedia(ofMachine(machine, u"media/delete"_s), imageBody(image, named), std::move(done));
}

std::function<void()> ServerClient::uploadMedia(const QString& machine, const QString& name, const QString& named,
                                                std::unique_ptr<QIODevice> image,
                                                std::function<void(base::Result<MediaUploaded>)> done)
{
    // Only from a file that is read at a place, whose length is known before a byte goes: what has none is not
    // taken (Qt would read all of it in first, to know how long it is). And read on a thread of its own, never on
    // this one, which sends it: Qt reads what it sends on the thread of its reply, and a read of a share that answers
    // slowly, or not at all, would hold everything here still (ReadAhead). One of those as it is; any other moved to
    // one.
    std::shared_ptr<QIODevice> file;
    if (qobject_cast<ReadAhead*>(image.get()) != nullptr) {
        file = std::move(image);
    } else {
        file = ReadAhead::of(std::move(image));
    }
    // (Held with the file, by the answer: what it says of reading it, where it is one that reads ahead.)
    const auto* reader = qobject_cast<const ReadAhead*>(file.get());
    const bool readable = file != nullptr && file->isOpen() && file->isReadable() && !file->isSequential();
    const qint64 size = readable ? file->size() - file->pos() : 0;
    std::optional<base::Error> refused;
    if (!readable) {
        refused = base::Error{"invalid", "An image is carried from a file."};
    } else if (size <= 0) {
        refused = base::Error{"invalid", "An empty file is no image."};
    } else if (size > mostUploadBytes) {
        refused = base::Error{"too_large", "An image carried through qKVM may be 16 GB at the most."};
    } else if (!isImageName(name)) {
        refused = base::Error{"invalid", imageNameRule};
    }
    if (refused) {
        QTimer::singleShot(0, this, [done = std::move(done), why = *refused] { done(why); });
        return [] {};
    }
    struct Carrying {
        bool stopped = false;
        QPointer<QNetworkReply> reply;
    };
    const auto carrying = std::make_shared<Carrying>();
    const QString path = ofMachine(machine, u"media/upload"_s) + u"?name="_s +
                         QString::fromLatin1(QUrl::toPercentEncoding(name)) + u"&machine="_s +
                         QString::fromLatin1(QUrl::toPercentEncoding(named));
    ask(
        [this, path, file, size, carrying] {
            QNetworkRequest asked = request(path, m_uploadQuietMs);
            asked.setHeader(QNetworkRequest::ContentTypeHeader, u"application/octet-stream"_s);
            asked.setHeader(QNetworkRequest::ContentLengthHeader, size);
            asked.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
            QNetworkReply* reply =
                onALineOfItsOwn([&](QNetworkAccessManager& line) { return line.post(asked, file.get()); });
            // (The file is the reply's for as long as it is out.)
            connect(reply, &QObject::destroyed, reply, [file] {});
            reply->setProperty("wholeWithinMs", uploadWholeWithinMs);
            // (Once it has begun to go, Qt says that it went (sendOut), before all of it has: from then on it is not
            // taken back where contact is lost, as no POST that went is. Held by test_core_server_client_upload,
            // anUploadUnderWayGoesOnWhereContactIsLostByAnotherCall.)
            carrying->reply = reply;
            if (carrying->stopped) {
                // (A turn later: whoever waits for it hears that it was stopped.)
                reply->setProperty("stopped", true);
                QMetaObject::invokeMethod(reply, &QNetworkReply::abort, Qt::QueuedConnection);
            }
            return reply;
        },
        largestAnswerBytes, false,
        [this, of = m_serverTurn, file, reader, done = std::move(done)](base::Result<QByteArray> answer,
                                                                        QNetworkReply* reply) {
            if (!answer && answer.error().code == uploadStopped) {
                done(answer.error());
                return;
            }
            // (Where its file could not be read on the way, that is why, whatever the line said of it after.)
            if (const QString failed = reader != nullptr ? reader->failure() : QString();
                !answer && !failed.isEmpty()) {
                done(base::Error{imageUnreadable,
                                 "The image could not be read on the way, and nothing more of it went: " +
                                     failed.toStdString()});
                return;
            }
            const base::Result<QByteArray> said =
                doneOrNot(std::move(answer), reply, of, [](const QByteArray& yes) { return uploadedFrom(yes).ok(); });
            done(said ? uploadedFrom(said.value()) : base::Result<MediaUploaded>(said.error()));
        });
    return [carrying] {
        carrying->stopped = true;
        if (carrying->reply != nullptr && !carrying->reply->isFinished()) {
            carrying->reply->setProperty("stopped", true);
            carrying->reply->abort();
        }
    };
}
} // namespace qkvm::core
