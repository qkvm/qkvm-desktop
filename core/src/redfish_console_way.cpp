// The way into a server controller's console (M7 part 3a): its own web page signed in to, and the pass it hands out
// for one console session, as the web's redfish.py has it (_hold_console up to _watch_console). The rest of the driver
// is redfish_device.cpp.

#include <qkvm/core/redfish_device.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QPointer>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/tls.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The most of an answer of its web page (the web's MAX_REPLY, as for every answer of a controller's).
constexpr qsizetype largestWebBytes = qsizetype{2} * 1024 * 1024;
// What its firmware says among what it can do where it lets a console reconnect (ami.RECONNECTS).
constexpr QByteArrayView reconnects{"KVM_SESSION_RECONNECT"};

// What a controller hands out to be sent back to it in a header (a session, its token): what a cookie may be made
// of, which is nothing that could end the header line or begin another (the web's HANDED).
bool handed(const QString& said)
{
    static const QRegularExpression shape(u"\\A[!#-+\\--:<-\\[\\]-~]{1,512}\\z"_s);
    return shape.match(said).hasMatch();
}

// The pass for one console session, which travels in a message of the console's own (the web's CONSOLE_PASS).
bool aPass(const QString& said)
{
    static const QRegularExpression shape(u"\\A[!-~]{1,128}\\z"_s);
    return shape.match(said).hasMatch();
}

// How a controller said one of its two words for "the server's own monitor goes dark while a console is open" (the
// web's said_yes and not_said, and the owner's decision of 2026-11-05 for a word that is not said). A real board says
// 0 or 1. A plain no is a no; said some other way, or as something that cannot be read, it is not taken for a no (the
// console is the thing to leave alone when in doubt); and not said at all, or said as nothing (an empty text, or
// blanks), is no no either (web 0.3.2, its review's LOW: the empty text was taken for a no, and the console opened).
enum class MonitorWord { No, Yes, NotSaid };

// A blank as the web's strip() takes one off a text (Python's str.isspace): Qt's spaces, and the four separators
// 0x1C to 0x1F, which Qt's are not.
bool blank(QChar letter)
{
    return letter.isSpace() || (letter.unicode() >= 0x1C && letter.unicode() <= 0x1F);
}

MonitorWord monitorWordOf(const QJsonValue& value)
{
    if (value.isUndefined() || value.isNull()) {
        return MonitorWord::NotSaid;
    }
    if (value.isBool()) {
        return value.toBool() ? MonitorWord::Yes : MonitorWord::No;
    }
    if (value.isDouble()) {
        return value.toDouble() != 0 ? MonitorWord::Yes : MonitorWord::No;
    }
    if (value.isString()) {
        const QString said = value.toString();
        qsizetype from = 0;
        qsizetype to = said.size();
        while (from < to && blank(said[from])) {
            ++from;
        }
        while (to > from && blank(said[to - 1])) {
            --to;
        }
        if (from == to) {
            return MonitorWord::NotSaid;
        }
        // (Lowered as the web's lower() lowers it, by Unicode's full mapping: a capital I with a dot becomes two
        // letters, and no letter outside ASCII becomes one of these.)
        static const QStringList noes{u"0"_s, u"false"_s, u"off"_s, u"no"_s, u"disabled"_s};
        return noes.contains(said.sliced(from, to - from).toLower()) ? MonitorWord::No : MonitorWord::Yes;
    }
    return MonitorWord::Yes;
}

// What a controller sent where a text or a number belongs, as a short line (the web's as_text); anything else is none.
QByteArray asText(const QJsonValue& value)
{
    return kvmd::plain(value, 64).toUtf8();
}

// The controller's words where they are the web's (redfish.py).
base::Error noConsole()
{
    return {"no_picture", "This controller has no console that qKVM reads yet"};
}
base::Error webPageRefused()
{
    return {"no_picture", "The controller's web page rejected the saved login: it is not offered again yet"};
}
base::Error notSigningIn(const std::string& why)
{
    return {"no_sign_in", "The device does not sign qKVM in (" + why + "). It is asked again shortly."};
}
base::Error darkens()
{
    return {"no_picture", "The controller is set to switch the server's own monitor off while its console is open, so "
                          "qKVM leaves the console alone"};
}
base::Error mayDarken()
{
    return {"no_picture", "The controller does not say whether it switches the server's own monitor off while its "
                          "console is open, so qKVM leaves the console alone"};
}
// (Web 0.3.2, its independent review's L2: one of the two a plain no, and the other not. That it "is set to switch"
// the monitor off may be untrue then: the owner's real boards were taken to darken it only where both were 1.)
base::Error oneOff()
{
    return {"no_picture", "Of the controller's two settings that switch the server's own monitor off while its console "
                          "is open, only one is off, so qKVM leaves the console alone"};
}
base::Error stoppedWay()
{
    return {"stopped", "The connection to the controller was let go of before its console could be opened"};
}
// (The web's CONSOLE_LINE_CLOSES, without its exception for a certificate that no other card shows: outside review of
// 0.2.0, area 5, MEDIUM 5.4.)
base::Error consoleLineCloses()
{
    return {"no_picture",
            "The controller closes the connection after every answer, so what its console needs (the "
            "sign-in of its web page, the console's pass) cannot go on the connection its number was asked "
            "on: none of it is sent, and the console is not opened. It is asked again later."};
}

// What stops the question which server answers for the console at once; a moment of its own is asked through again.
// (The web's: auth_failed, no_password, no_sign_in, another_machine, and DOWN_CODES.)
bool stopsTheQuestion(const std::string& code)
{
    static const QStringList stops{u"auth_failed"_s,  u"no_password"_s, u"no_sign_in"_s, u"another_machine"_s,
                                   u"unreachable"_s,  u"timeout"_s,     u"dropped"_s,    u"cert_mismatch"_s,
                                   u"cert_invalid"_s, u"stopped"_s};
    return stops.contains(QString::fromStdString(code)) || code == tlsDamaged;
}

} // namespace

// One asking for a way in, from whether it has a console to the way handed on (or why there is none).
struct RedfishDevice::WayAsking {
    std::function<void(base::Result<ConsoleWay>)> done;
    QPointer<DeviceAsker> line; // its web page's, held to what the controller is known by
    QByteArray session;         // its web page's once signed in: its cookies and its token, as the line carries them
    int linesGone = 0;          // times the line its root was read on was gone before what was to follow (wayLineGone)
};

// Its web page's sign-in that is out, on the asking's line, which can outlive the driver (~RedfishDevice).
struct RedfishDevice::WebSignIn {
    QPointer<RedfishDevice> driver; // none: let go of while this was out
    std::weak_ptr<WayAsking> asking;
    QPointer<DeviceAsker> line;
    std::shared_ptr<SignInPacing> pacing; // none: the program's
    SignInPacing::Mark mark;
    SignInPacing::Steps steps;
    SignInPacing::Sent sent;
    // Where a session it brings after its driver went is given back, and by whom (none: the program's goodbyes).
    DeviceAsker::Where where;
    int connectWithinMs = 0;
    int signOutWithinMs = 0;
    std::shared_ptr<ControllerGoodbyes> goodbyes;
};

SignInPacing::Mark RedfishDevice::webOffered() const
{
    return SignInPacing::what(u"web", m_where.address, m_where.user, m_password.view());
}

void RedfishDevice::consoleWay(std::function<void(base::Result<ConsoleWay>)> done)
{
    auto asking = std::make_shared<WayAsking>();
    asking->done = std::move(done);
    m_wayAsked.append(asking);
    if (m_password.isEmpty()) {
        noWay(asking, base::Error{"no_password", "No password saved for this device"});
        return;
    }
    // Its web page is spoken to on a line of its own, held to what the controller is known by, its session in the
    // cookies that page hands out and the token it wants beside them.
    freshWayLine(asking);
    hasConsole(asking);
}

void RedfishDevice::freshWayLine(const std::shared_ptr<WayAsking>& asking)
{
    // (A line, here, is a manager of HTTP of its own, which keeps the connections its questions made, and no other: one
    // made for the question about the root, and for what follows on its connection, has made no other connection that a
    // question could go on unasked. Outside review of 0.2.0, 5.4.)
    if (asking->line != nullptr && (!m_webSignInOut || m_webSignInOut->line != asking->line)) {
        asking->line->deleteLater();
    }
    const DeviceAsker::Where where{m_where.address, m_where.knownByCertificate, m_where.certificate};
    asking->line =
        new DeviceAsker(where, // NOLINT(cppcoreguidelines-owning-memory): this driver's, or the goodbyes'
                        DeviceAsker::Times{m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs},
                        DeviceAsker::Carried{DeviceAsker::Carried::In::CookiesAndHeader, "X-CSRFTOKEN"_ba}, this);
    asking->line->keepToOneConnection();
    connect(asking->line, &DeviceAsker::showedItsCertificate, this, &RedfishDevice::showed);
}

void RedfishDevice::hasConsole(const std::shared_ptr<WayAsking>& asking)
{
    const auto then = [this, asking] {
        // Which server answers, with the session of the controller's own held (the password went there already, and
        // was taken): before its web page is offered the password.
        whichServerForTheConsole(1, [this, asking](const std::optional<base::Error>& why) {
            if (why) {
                noWay(asking, *why);
                return;
            }
            if (const std::optional<base::Error> notAmis = notAmisConsole()) {
                noWay(asking, *notAmis);
                return;
            }
            webSignIn(asking);
        });
    };
    if (m_hasConsole) {
        if (!*m_hasConsole) {
            noWay(asking, noConsole());
            return;
        }
        then();
        return;
    }
    // AMI's firmware has this place and wants a sign-in for it; a controller that does not know it has another console.
    // Asked with no session.
    DeviceAsker::Ask asked;
    asked.path = u"/api/kvm/token"_s;
    asked.largest = largestWebBytes;
    asking->line->send(asked, {}, [this, asking, then](const base::Result<Reply>& reply) {
        if (!reply) {
            noWay(asking, reply.error());
            return;
        }
        const int status = reply.value().status;
        if (status != 401 && status != 404 && status != 405 && status != 410) {
            // (Neither "sign in first" nor "there is no such thing here": nothing is settled by that.)
            noWay(asking,
                  base::Error{
                      "bad_response",
                      u"The controller did not say whether it has a console (HTTP %1)"_s.arg(status).toStdString()});
            return;
        }
        m_hasConsole = status == 401;
        if (!*m_hasConsole) {
            noWay(asking, noConsole());
            return;
        }
        then();
    });
}

void RedfishDevice::whichServerForTheConsole(int asked, const std::function<void(std::optional<base::Error>)>& then)
{
    // Its computer read and judged under the session of the controller's own (the web's
    // `_which_server_for_the_console`). Where a number is kept, one of its moments is asked through again a few times,
    // a second apart: nothing goes on the strength of nothing. Where none is kept, a moment is no reason to stop.
    const auto failed = [this, asked, then](const base::Error& why) {
        if (stopsTheQuestion(why.code)) {
            then(why);
            return;
        }
        if (m_machine.isEmpty()) {
            then(std::nullopt);
            return;
        }
        if (asked >= m_pace.questionTries) {
            then(why);
            return;
        }
        QTimer::singleShot(m_pace.questionAgainMs, this,
                           [this, asked, then] { whichServerForTheConsole(asked + 1, then); });
    };
    find([this, then, failed](const base::Result<Places>& places) {
        if (!places) {
            failed(places.error());
            return;
        }
        checkedRead(places.value().system, false,
                    [this, then, failed](const base::Result<QJsonObject>& system, const Checked& /*checked*/) {
                        if (!system) {
                            failed(system.error());
                            return;
                        }
                        // (Nor where its card asks whether the computer there, which gives a number of its own now,
                        // is its server: no key and no picture of it until that is answered, the owner's decision of
                        // 2026-11-22.)
                        if (m_checkedAsking) {
                            then(asksFirst());
                            return;
                        }
                        then(std::nullopt);
                    });
    });
}

void RedfishDevice::webSignIn(const std::shared_ptr<WayAsking>& asking)
{
    // Its web page is signed in to with the same password, and a wrong one there counts against the same account: this
    // road keeps the waits of a refused password too, on a count of its own (an account may be let in by the one road
    // and not by the other).
    const SignInPacing::Mark mark = webOffered();
    if (pacing().waitLeftMs(mark) > 0) {
        if (mayWaitForTheOtherWeb()) {
            afterTheOtherWebSignIn(asking);
            return;
        }
        noWay(asking, pacing().times(mark) > 0 ? webPageRefused()
                                               : notSigningIn("the last sign-in to its web page came to nothing"));
        return;
    }
    DeviceAsker::Ask login;
    login.method = "POST"_ba;
    login.path = u"/api/session"_s;
    // (Its body is made as it goes, below: nothing of the password waits in the lambda.)
    login.bodyIs = "application/x-www-form-urlencoded"_ba;
    login.largest = largestWebBytes;
    login.doesSomething = true;
    login.quietMs = m_pace.signInQuietMs;
    // On its web page's own line, its root anybody may read, compared with the controller's number where one is kept,
    // before the password goes: the question asked about the server went on another line, and in a clash of leases
    // under one certificate another controller may answer this one. (And so the line is shown to be up.) What answers
    // is to be a Redfish service root, whether a number is kept or not (sameController; outside review of 0.2.0, 5.1).
    // Where no number is kept, nothing more is compared here: what holds the password to this controller then is its
    // certificate, as on the web, which has the same gap. On a line made for the two, the root's and the password's (a
    // line that asked before may hold a connection nobody asked the root on: freshWayLine).
    freshWayLine(asking);
    sameController(*asking->line, [this, asking, login, mark](const base::Result<bool>& compared, bool lineEnds) {
        if (!compared) {
            noWay(asking, compared.error());
            return;
        }
        if (lineEnds) {
            // (Its root's answer said that its line ends with it: the password would go on a new one, unasked.)
            wayLineGone(asking, [this, asking] { webSignIn(asking); });
            return;
        }
        // Looked at once more just before the password goes: a look that landed while the controller's number was
        // asked may have learned this is OpenBMC (finding 4). The password is never posted to its web page then.
        if (const std::optional<base::Error> notAmis = notAmisConsole()) {
            noWay(asking, *notAmis);
            return;
        }
        // Looked at again just before it is counted: another asking, of this driver or another of this controller's,
        // may have sent the password meanwhile, or have it out still. One web sign-in a driver, whatever the pacing
        // holds (a password typed anew is forgotten there): an asking while one is out waits for what it comes to, and
        // is then asked through again. (A second, sent beside it, took its place, and the first's answer dropped the
        // second's record: its line was then the goodbyes' at no driver's going.)
        if (m_webSignInOut) {
            m_afterTheWebSignIn.append(asking);
            return;
        }
        if (pacing().waitLeftMs(mark) > 0) {
            if (mayWaitForTheOtherWeb()) {
                afterTheOtherWebSignIn(asking);
                return;
            }
            noWay(asking, pacing().times(mark) > 0 ? webPageRefused()
                                                   : notSigningIn("a sign-in to its web page is under way, or the last "
                                                                  "one came to nothing"));
            return;
        }
        auto out = std::make_shared<WebSignIn>();
        out->driver = this;
        out->asking = asking;
        out->line = asking->line;
        out->pacing = m_pace.pacing;
        out->mark = mark;
        out->steps = steps();
        out->where = DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate};
        out->connectWithinMs = m_pace.connectWithinMs;
        out->signOutWithinMs = m_pace.signOutWithinMs;
        out->goodbyes = m_pace.goodbyes;
        // Counted as it goes out: a driver let go of while it is out leaves its mark, until what it came to is told.
        out->sent = pacing().sent(mark, out->steps);
        m_webSignInOut = out;
        // Made once, of the password as it is held, as it goes: nothing of it in a text made on the way (F5), and no
        // copy of it held but by what goes, which writes it over once its answer came (DeviceAsker; from the merged
        // review, 3). This one is written over here where nothing went.
        DeviceAsker::Ask sent = login;
        // On the connection its root was just read and compared on, and on no other (outside review of 0.2.0, 5.4).
        sent.onItsLine = true;
        sent.body = secretForm({{"username", m_where.user}, {"password", m_password.view()}});
        const WipedOnLeaving overTheBody(sent.body);
        asking->line->send(sent, {}, [out](const base::Result<Reply>& reply) {
            if (RedfishDevice* driver = out->driver) {
                if (driver->m_webSignInOut == out) {
                    driver->m_webSignInOut.reset();
                }
                const base::Result<QByteArray> session = webCameOf(*out, reply, false);
                if (const std::shared_ptr<WayAsking> waiting = out->asking.lock()) {
                    driver->webSignInCame(waiting, session);
                } else if (session) {
                    // (Nobody waits for it any more: given back all the same.)
                    ControllerGoodbyes& goodbyes = out->goodbyes ? *out->goodbyes : ControllerGoodbyes::program();
                    goodbyes.leaveWebPage(out->where, out->connectWithinMs, session.value(), out->signOutWithinMs);
                }
                // Whoever waited for it is asked through again, from the loop: a no is theirs too; after a yes each
                // signs in itself, one at a time.
                driver->later([driver] {
                    for (const std::shared_ptr<WayAsking>& next : std::exchange(driver->m_afterTheWebSignIn, {})) {
                        if (driver->m_wayAsked.contains(next)) {
                            driver->webSignIn(next);
                        }
                    }
                });
                return;
            }
            // Its driver was let go of while it was out: told to the pacing as the driver would have told it, and a
            // session it brought given back (a controller has few, and it makes one when it answers, wanted or not).
            const base::Result<QByteArray> session = webCameOf(*out, reply, true);
            if (session) {
                ControllerGoodbyes& goodbyes = out->goodbyes ? *out->goodbyes : ControllerGoodbyes::program();
                goodbyes.leaveWebPage(out->where, out->connectWithinMs, session.value(), out->signOutWithinMs);
            }
            if (out->line != nullptr) {
                out->line->deleteLater();
            }
        });
    });
}

bool RedfishDevice::mayWaitForTheOtherWeb() const
{
    // (As a sign-in of the controller's own road: again where, after a yes, another that waited went first.)
    return pacing().onlyOut(webOffered());
}

void RedfishDevice::afterTheOtherWebSignIn(const std::shared_ptr<WayAsking>& asking)
{
    // (Then asked through again, where it is still asked, as the other's line's end tells.)
    const SignInPacing::Mark mark = webOffered();
    pacing().whenSettled(mark, this, pacing().waitLeftMs(mark) + 5, [this, weak = std::weak_ptr<WayAsking>(asking)] {
        if (const std::shared_ptr<WayAsking> again = weak.lock(); again && m_wayAsked.contains(again)) {
            webSignIn(again);
        }
    });
}

base::Result<QByteArray> RedfishDevice::webCameOf(const WebSignIn& out, const base::Result<Reply>& reply, bool late)
{
    SignInPacing& pacing = out.pacing ? *out.pacing : SignInPacing::program();
    // (Late, nothing standing for the password: another driver was let in with it meanwhile, which a sign-in that came
    // to nothing, after its driver went, does not undo.)
    const bool noted = !late || pacing.standing(out.mark).has_value();
    if (!reply) {
        // What went out and came to nothing is held back (an answer nothing is made of among it); what found no line
        // to be had was not sent.
        const std::string& code = reply.error().code;
        if (code == "timeout" || code == "dropped" || code == "bad_response") {
            if (noted) {
                pacing.unanswered(out.mark, out.steps, out.sent);
            }
        } else {
            pacing.putBack(out.mark, out.sent);
        }
        return reply.error();
    }
    const int status = reply.value().status;
    const QJsonValue token =
        status == 200 ? QJsonDocument::fromJson(reply.value().body).object().value("CSRFToken"_L1) : QJsonValue();
    QByteArray cookies;
    bool fit = !reply.value().cookies.isEmpty();
    for (const QNetworkCookie& cookie : reply.value().cookies) {
        if (!handed(QString::fromLatin1(cookie.name())) || !handed(QString::fromLatin1(cookie.value()))) {
            fit = false;
            break;
        }
        cookies += (cookies.isEmpty() ? QByteArray() : "; "_ba) + cookie.name() + "="_ba + cookie.value();
    }
    if (!token.isString() || !handed(token.toString()) || !fit) {
        // A no, in the numbers a wrong password is answered with (one says 400). Anything else is none: too many
        // sessions, a moment of its own, a page that is not what was expected; asked again later, and not as a no.
        if (status == 400 || status == 401 || status == 403) {
            pacing.saidNo(out.mark, out.steps);
        } else if (noted) {
            pacing.unanswered(out.mark, out.steps, out.sent);
        }
        return base::Error{
            "no_picture", u"The controller's web page did not sign this user in (HTTP %1)"_s.arg(status).toStdString()};
    }
    pacing.accepted(out.mark);
    return cookies + '\n' + token.toString().toLatin1();
}

void RedfishDevice::webSignInCame(const std::shared_ptr<WayAsking>& asking, const base::Result<QByteArray>& session)
{
    if (!session && session.error().code == DeviceAsker::lineMadeAnewCode) {
        // (The connection its root was read on was gone, and the password was not sent on a new one.)
        wayLineGone(asking, [this, asking] { webSignIn(asking); });
        return;
    }
    if (!session) {
        noWay(asking, session.error());
        return;
    }
    asking->session = session.value();
    asking->linesGone = 0;
    // Asked again now that its web page has signed this program in, before it hands out the pass: the address that
    // answered that sign-in may have changed hands since the question before it, and its console is not opened to
    // another server.
    whichServerForTheConsole(1, [this, asking](const std::optional<base::Error>& why) {
        if (why) {
            noWay(asking, *why);
            return;
        }
        consolePass(asking);
    });
}

void RedfishDevice::consolePass(const std::shared_ptr<WayAsking>& asking)
{
    // Its root on the web page's own line compared again, before the pass is asked for on it (as before the password),
    // and the pass asked for on that very connection (outside review of 0.2.0, 5.4), on a line made for the two.
    freshWayLine(asking);
    sameController(*asking->line, [this, asking](const base::Result<bool>& compared, bool lineEnds) {
        if (!compared) {
            noWay(asking, compared.error());
            return;
        }
        if (lineEnds) {
            wayLineGone(asking, [this, asking] { consolePass(asking); });
            return;
        }
        askThePass(asking);
    });
}

void RedfishDevice::wayLineGone(const std::shared_ptr<WayAsking>& asking, const std::function<void()>& again)
{
    // Asked once more, on a new line, its root read and compared on it first; where that one is gone too, the
    // controller closes the connection after every answer, and nothing that needs it is sent.
    if (++asking->linesGone < 2) {
        again();
        return;
    }
    noWay(asking, consoleLineCloses());
}

void RedfishDevice::askThePass(const std::shared_ptr<WayAsking>& asking)
{
    DeviceAsker::Ask asked;
    asked.path = u"/api/kvm/token"_s;
    asked.largest = largestWebBytes;
    asked.onItsLine = true;
    asking->line->send(asked, asking->session, [this, asking](const base::Result<Reply>& reply) {
        if (!reply && reply.error().code == DeviceAsker::lineMadeAnewCode) {
            wayLineGone(asking, [this, asking] { consolePass(asking); });
            return;
        }
        if (!reply) {
            noWay(asking, reply.error());
            return;
        }
        const int status = reply.value().status;
        const QJsonObject given = status == 200 ? QJsonDocument::fromJson(reply.value().body).object() : QJsonObject();
        const QJsonValue pass = given.value("token"_L1);
        if (!pass.isString() || !aPass(pass.toString())) {
            noWay(asking,
                  base::Error{"no_picture",
                              u"The controller gave no pass for its console (HTTP %1)"_s.arg(status).toStdString()});
            return;
        }
        // Opened only where both words are said, and both are a plain no. Anywhere else whoever stands at the server
        // may see its monitor go dark: that is not "only looking" any more, and the card says why, in one of three
        // sentences as the web's monitor_goes_dark does: either not said; both not a plain no; only one of them off.
        const MonitorWord automatic = monitorWordOf(given.value("automatic_off"_L1));
        const MonitorWord local = monitorWordOf(given.value("local_monitor_off"_L1));
        if (automatic == MonitorWord::NotSaid || local == MonitorWord::NotSaid) {
            noWay(asking, mayDarken());
            return;
        }
        if (automatic == MonitorWord::Yes || local == MonitorWord::Yes) {
            noWay(asking, automatic == local ? darkens() : oneOff());
            return;
        }
        ConsoleWay way;
        way.address = m_where.address;
        way.knownByCertificate = m_where.knownByCertificate;
        way.certificate = m_where.certificate;
        way.cookie = asking->session.left(asking->session.indexOf('\n'));
        way.pass = pass.toString().toLatin1();
        way.ownAddress = asText(given.value("client_ip"_L1));
        way.controllerAddress = asText(given.value("server_ip"_L1));
        if (way.controllerAddress.isEmpty()) {
            way.controllerAddress = (m_where.address.host() +
                                     (m_where.address.port() > 0 ? u":%1"_s.arg(m_where.address.port()) : QString()))
                                        .toUtf8();
        }
        way.webSession = asText(given.value("session"_L1));
        if (m_completeFirst) {
            way.completeFirst = m_completeFirst;
            wayHanded(asking, std::move(way));
            return;
        }
        // The firmware says what it can do: whether a console may reconnect decides how it wants to be greeted. Asked
        // once for this driver; what cannot be read is no.
        DeviceAsker::Ask can;
        can.path = u"/api/configuration/project"_s;
        can.largest = largestWebBytes;
        asking->line->send(
            can, asking->session, [this, asking, way = std::move(way)](const base::Result<Reply>& said) mutable {
                m_completeFirst = said && said.value().status == 200 && said.value().body.contains(reconnects);
                way.completeFirst = m_completeFirst;
                wayHanded(asking, std::move(way));
            });
    });
}

// A way's web page's session, given back once: when the way's `done` is called (from whichever thread), or where it
// never is, when the last copy of the way goes. The goodbye is said on the goodbyes' own loop, while they are there,
// through their door (ControllerGoodbyes::Door: a QPointer to them, read on another thread, could be to goodbyes that
// were going that very moment). (Not an owner of them: a goodbye queued with one kept them until the queue was cleared
// at the program's end, and destroying them then, under the queue's own lock, hung the program.)
class RedfishDevice::HeldWebSession {
public:
    HeldWebSession(std::shared_ptr<ControllerGoodbyes::Door> door, DeviceAsker::Where where, int connectWithinMs,
                   QByteArray session, int withinMs)
        : m_door(std::move(door)), m_where(std::move(where)), m_connectWithinMs(connectWithinMs),
          m_session(std::move(session)), m_withinMs(withinMs)
    {}
    // NOLINTNEXTLINE(bugprone-exception-escape): (the words of an answer out of memory: nothing of ours throws)
    ~HeldWebSession() { giveBack(); }
    HeldWebSession(const HeldWebSession&) = delete;
    HeldWebSession& operator=(const HeldWebSession&) = delete;
    HeldWebSession(HeldWebSession&&) = delete;
    HeldWebSession& operator=(HeldWebSession&&) = delete;

    void giveBack() noexcept
    {
        if (m_given.exchange(true)) {
            return;
        }
        // (Where the goodbyes are gone, the door says so.)
        // NOLINTNEXTLINE(bugprone-exception-escape): (the words of an answer out of memory: nothing of ours throws)
        m_door->leaveWebPage(m_where, m_connectWithinMs, m_session, m_withinMs);
    }

private:
    std::shared_ptr<ControllerGoodbyes::Door> m_door;
    DeviceAsker::Where m_where;
    int m_connectWithinMs = 0;
    QByteArray m_session;
    int m_withinMs = 0;
    std::atomic<bool> m_given{false};
};

void RedfishDevice::wayHanded(const std::shared_ptr<WayAsking>& asking, ConsoleWay way)
{
    auto held = std::make_shared<HeldWebSession>(
        (m_pace.goodbyes ? *m_pace.goodbyes : ControllerGoodbyes::program()).door(),
        DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate}, m_pace.connectWithinMs,
        std::exchange(asking->session, {}), m_pace.signOutWithinMs);
    way.done = [held] { held->giveBack(); };
    m_wayAsked.removeOne(asking);
    if (asking->line != nullptr) {
        asking->line->deleteLater();
    }
    // (On the program's loop, as what came of a press: a driver let go of in this very turn hands it all the same.)
    // NOLINTNEXTLINE(bugprone-exception-escape)
    QTimer::singleShot(0, QCoreApplication::instance(), [asking, way = std::move(way)]() mutable {
        if (const auto done = std::exchange(asking->done, nullptr)) {
            done(std::move(way));
        }
    });
}

void RedfishDevice::noWay(const std::shared_ptr<WayAsking>& asking, const base::Error& why)
{
    // Its web page's session, where one was made, is given back on every road that hands no way on.
    if (!asking->session.isEmpty()) {
        ControllerGoodbyes& goodbyes = m_pace.goodbyes ? *m_pace.goodbyes : ControllerGoodbyes::program();
        goodbyes.leaveWebPage(DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
                              m_pace.connectWithinMs, std::exchange(asking->session, {}), m_pace.signOutWithinMs);
    }
    m_wayAsked.removeOne(asking);
    if (asking->line != nullptr && (!m_webSignInOut || m_webSignInOut->line != asking->line)) {
        asking->line->deleteLater();
    }
    // NOLINTNEXTLINE(bugprone-exception-escape)
    QTimer::singleShot(0, QCoreApplication::instance(), [asking, why] {
        if (const auto done = std::exchange(asking->done, nullptr)) {
            done(why);
        }
    });
}

void RedfishDevice::letGoOfTheWays()
{
    ControllerGoodbyes& goodbyes = m_pace.goodbyes ? *m_pace.goodbyes : ControllerGoodbyes::program();
    // A web sign-in that is out goes on without this driver: its line is the goodbyes' from now on, and what it comes
    // to is told and given back there (webSignIn).
    if (m_webSignInOut && m_webSignInOut->line != nullptr) {
        goodbyes.waitFor(m_webSignInOut->line);
    }
    for (const std::shared_ptr<WayAsking>& asking : std::as_const(m_wayAsked)) {
        if (!asking->session.isEmpty()) {
            goodbyes.leaveWebPage(DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
                                  m_pace.connectWithinMs, std::exchange(asking->session, {}), m_pace.signOutWithinMs);
        }
        if (const auto done = std::exchange(asking->done, nullptr)) {
            // NOLINTNEXTLINE(bugprone-exception-escape)
            QTimer::singleShot(0, QCoreApplication::instance(), [done] { done(stoppedWay()); });
        }
    }
    m_wayAsked.clear();
}

} // namespace qkvm::core
