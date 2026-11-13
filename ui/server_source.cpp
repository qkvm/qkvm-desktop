#include "server_source.h"

#include <QCryptographicHash>

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QLocale>

#include "wall_key.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

QString ServerSource::liveIdentity() const
{
    return QString::fromLatin1(
        QCryptographicHash::hash(m_client.address().toEncoded() + '\n' + m_client.session(), QCryptographicHash::Sha256)
            .toHex());
}

using core::Connection;
using Event = core::ConnectionEvent;

namespace {

// What the server lets an account do, as the session view says it: its role, or "display" for a
// screen opened with a display link.
QString roleOf(const core::Account& account)
{
    return account.isDisplay ? u"display"_s : account.role;
}

// (In the words of the session, which said them before the server's conversation was a class of its
// own.)
QString words(const char* text)
{
    return QCoreApplication::translate("Session", text);
}

QString sentence(const base::Error& error)
{
    return QString::fromStdString(error.message);
}

// With every so many looks at the machines, who is signed in is asked again: as the page does it.
constexpr int looksBetweenAskingWho = 15;

// A QKVM's own word that a display link is not good: taken back, or run out. Nothing else that
// is answered where a link is handed in says anything of the link.
constexpr const char* linkNotGood = "bad_key";

} // namespace

// (Each answer of the server's below is taken by the turn it was asked in, and that is what guards
// it. The QPointer to this that each holds as well never finds it gone: the client is a member,
// which ends before this does, and an answer comes only through it. See turn().)

ServerSource::ServerSource(const QString& id, core::Settings& settings, core::SecretStore& secrets, int statusEveryMs,
                           QObject* parent)
    : Source(parent), m_id(id), m_settings(settings), m_secrets(secrets), m_feed(m_client, statusEveryMs)
{
    // (Whoever makes it gives it an id a key takes: MergedWall takes no source whose id is none.)
    Q_ASSERT_X(isSourceId(m_id), "ServerSource", "not a source's id");
    m_clock.start();
    // The server could not be reached with the link: it is on its way back up, and is asked again.
    m_awayEveryMs = statusEveryMs;
    m_again.setSingleShot(true);
    m_again.setInterval(statusEveryMs);
    connect(&m_again, &QTimer::timeout, this, [this] {
        if (m_state == Connection::Away && !m_displayKey.isEmpty()) {
            comeBackByTheLink();
        } else if (m_tryingAgain && m_state == Connection::None) {
            // A screen that waits where a server is named, for the server its link is kept
            // for: asked anew from the first question, as when it was named. (What was said of
            // why it is not there stays said meanwhile.)
            become(Event::Named, m_problem);
            askWhatAnswersThere();
        }
    });

    // (What the feed says of the wall, its machines and their pictures, the windows' wall hears,
    // and nobody else: it tells this when the machines were looked at, by looked().)
    connect(&m_feed, &core::WallFeed::contactChanged, this, [this] {
        if (!m_feed.isInContact()) {
            loseContact();
        } else if (!m_linkTakenUnproven && means(Event::ContactBack)) {
            // (Under a link that was taken again, what says so is the first look at the machines
            // that is answered: looked().)
            become(Event::ContactBack);
        }
    });
    connect(&m_feed, &core::WallFeed::signedOut, this, [this] {
        // A session the server has ended is not worth keeping.
        m_secrets.forget(sessionName());
        if (!m_displayKey.isEmpty()) {
            // A link that was taken, and whose session did not hold: no question that carried
            // it was answered before this one was refused. Such a taking counts as none. The
            // link is handed in again after a wait that is longer each time, as one that is
            // not taken (handTheLinkInLater), and the wall says that contact is lost meanwhile:
            // handed in at once, or at the wall's pace, it was posted every few seconds for ever.
            if (m_linkTakenUnproven) {
                loseContact();
                m_again.start(handTheLinkInLater(base::Error{core::ServerClient::signedOut, std::string()}));
                return;
            }
            // A session that held, and is gone a moment after its link was handed in, would be
            // come back to at the pace of the answers. Not sooner after the last time than a
            // server that is away is asked again: the wall says that contact is lost meanwhile.
            if (handedTheLinkInJustNow()) {
                loseContact();
                m_again.start(m_awayEveryMs);
                return;
            }
            comeBackByTheLink();
            return;
        }
        leaveWall(Event::SignedOut, words("You have been signed out. Sign in again to go on."));
    });
    connect(&m_feed, &core::WallFeed::certificateRefused, this, [this] { leaveWall(Event::CertificateShown, {}); });
    // Contact was lost, and what answers at the server's address now is a QKVM that speaks
    // another version of its API. One of the two has to be updated, which somebody has to do:
    // said where a server is named, as when such a one is named first, and not by a wall that
    // says "contact lost" for ever. Nothing was sent to it. What is kept for that server stays
    // kept, a display link too: it is that QKVM's, and nothing to the next one that is named.
    connect(&m_feed, &core::WallFeed::otherVersion, this, [this](const QString& sentence) {
        m_displayKey.clear();
        leaveWall(Event::Left, sentence);
    });
}

ServerSource::~ServerSource()
{
    m_feed.stop();
}

bool ServerSource::isAtTheWall() const
{
    return m_state == Connection::SignedIn || m_state == Connection::Away;
}

void ServerSource::looked()
{
    if (++m_looks % looksBetweenAskingWho == 0 && isAtTheWall()) {
        askWhoItIsNow();
    }
    // A look at the machines was answered under the session a display link opened: that taking
    // has held, and a link that is not taken after it waits anew (handTheLinkInLater).
    // Contact that was lost is back with it, and not before: not when the link was taken again.
    if (isAtTheWall() && std::exchange(m_linkTakenUnproven, false)) {
        m_linkNotTaken = 0;
        if (means(Event::ContactBack)) {
            become(Event::ContactBack);
        }
    }
}

void ServerSource::signOut()
{
    if (!means(Event::SignedOut)) {
        return;
    }
    // Whatever the server says to it: here nobody is signed in any more, and nothing is kept
    // that could sign in again, a display link no more than a session.
    m_secrets.forget(sessionName());
    forgetTheLink();
    auto remembered = m_settings.remembered();
    remembered.servers.removeAll(m_client.address().toString());
    static_cast<void>(m_settings.remember(remembered));
    // A person at this PC signed the screen of this server out: of this server, it is no wall
    // screen any more. (Before the wall is left: by then nothing of it has the program only show.)
    if (m_screen != nullptr) {
        m_screen->signedOutAt(m_client.address().toString());
    }
    m_client.signOut([](const base::Result<void>&) {});
    leaveWall(Event::SignedOut, {});
}

void ServerSource::leave()
{
    // The link is that server's, and stays kept for it; it is nothing to the next one.
    m_displayKey.clear();
    leaveWall(Event::Left, {});
}

void ServerSource::standAside()
{
    if (isAtTheWall()) {
        leaveWall(Event::Left, {});
    }
    // Also where no wall of the server's was up, and one was only being asked: its answer is to
    // nobody's question from here on. (Taken for one, it had the kept session sent after it,
    // and the server's wall come up under this PC's own.)
    ++m_turn;
    m_again.stop();
    m_tryingAgain = false;
    m_displayKey.clear();
    m_state = Connection::None;
    m_problem.clear();
    m_looks = 0;
    // (A code that a line for a password was asking for is asked anew where the server is named
    // again: its answer then says whether one is wanted.)
    m_asksCode = false;
}

// Whether whoever asks may is the server's to say, as for everything: it lets an admin, and asks
// for their password once more before it hands the wall's secrets out. What is asked while nobody
// is at the wall goes nowhere, with what was typed for it. Each answer is told under the number it
// was asked under: the view knows whether it still waits for it.
//
// Each of the three posts something that is not for whatever answers at the address to have: an
// export the password of whoever is signed in, once more; a reading and a taking the file, which
// may hold the passwords of machines, and its passphrase. So each goes, as a password does where
// somebody signs in, only to what was just asked what it is and is a QKVM
// (ifAQkvmStillAnswersThere), and is held here for that one question.
void ServerSource::exportWall(const QPointer<FilesView>& asked, quint64 turn, const QStringList& parts,
                              const QString& passphrase, const QString& password)
{
    if (!isAtTheWall()) {
        asked->reset();
        return;
    }
    ifAQkvmStillAnswersThere(
        [this, asked, turn, parts, passphrase, password] {
            m_client.exportWall(password, passphrase, parts, [asked, turn](const base::Result<core::WallFile>& file) {
                if (asked != nullptr) {
                    asked->exported(turn, file);
                }
            });
        },
        [asked, turn](const base::Error& why) {
            if (asked != nullptr) {
                asked->exported(turn, why);
            }
        });
}

void ServerSource::readFile(const QPointer<FilesView>& asked, quint64 turn, const QByteArray& file,
                            const QString& passphrase)
{
    if (!isAtTheWall()) {
        asked->reset();
        return;
    }
    ifAQkvmStillAnswersThere(
        [this, asked, turn, file, passphrase] {
            m_client.readFile(file, passphrase, [asked, turn](const base::Result<core::FileContents>& read) {
                if (asked != nullptr) {
                    asked->fileWasRead(turn, read);
                }
            });
        },
        [asked, turn](const base::Error& why) {
            if (asked != nullptr) {
                asked->fileWasRead(turn, why);
            }
        });
}

void ServerSource::takeFile(const QPointer<FilesView>& asked, quint64 turn, const QByteArray& file,
                            const QString& passphrase, const QStringList& parts)
{
    if (!isAtTheWall()) {
        asked->reset();
        return;
    }
    ifAQkvmStillAnswersThere(
        [this, asked, turn, file, passphrase, parts] {
            m_client.takeFile(file, passphrase, parts, [asked, turn](const base::Result<core::Taken>& taken) {
                if (asked != nullptr) {
                    asked->fileWasTaken(turn, taken);
                }
            });
        },
        [asked, turn](const base::Error& why) {
            if (asked != nullptr) {
                asked->fileWasTaken(turn, why);
            }
        });
}

void ServerSource::tellTheTimeWith(std::function<QDateTime()> clock)
{
    m_now = std::move(clock);
}

bool ServerSource::means(Event event) const
{
    return core::after(m_state, event) != m_state;
}

void ServerSource::become(Event event, const QString& problem)
{
    m_state = core::after(m_state, event);
    m_problem = problem;
    // (A server is asked again by itself only where one is named: at a wall, at a line for a
    // password and before a certificate, that is over.)
    if (m_state != Connection::None && m_state != Connection::Connecting) {
        m_tryingAgain = false;
    }
    emit changed();
    // (Whether a machine may be had live goes by where the connection is.)
    emit stateChanged();
}

SessionView::Shown ServerSource::shown() const
{
    SessionView::Shown shown;
    switch (m_state) {
    case Connection::None:
    case Connection::Connecting:
        shown.stage = u"address"_s;
        shown.tryingAgain = m_tryingAgain;
        break;
    case Connection::WantsTrust:
        shown.stage = u"trust"_s;
        shown.certificate = core::ServerClient::written(m_client.shownCertificate());
        // Only where it is another: a server that is known by none is seen for the first time.
        if (const QByteArray before = m_settings.remembered().certificates.value(m_client.address().authority());
            !before.isEmpty() && before != m_client.shownCertificate()) {
            shown.certificateBefore = core::ServerClient::written(before);
        }
        break;
    case Connection::WantsSignIn:
    case Connection::SigningIn:
        shown.stage = u"signIn"_s;
        shown.locked = m_locked;
        break;
    case Connection::SignedIn:
    case Connection::Away:
        shown.stage = u"wall"_s;
        break;
    }
    // (A server that is asked again by itself is asked beside whoever may type there: not busy.)
    shown.busy = (m_state == Connection::Connecting && !m_tryingAgain) || m_state == Connection::SigningIn;
    shown.problem = m_problem;
    // (In the signs of the address: a name in its xn-- form is not shown in the letters Qt
    // would decode it to, which can read as another name.)
    shown.address = m_client.address().authority(QUrl::FullyEncoded);
    // The server that was used last, as it would be typed again. What the settings hold is read
    // as what somebody typed would be: it is a file, and anybody may have written into it.
    if (const base::Result<QUrl> last = core::ServerClient::addressFrom(m_settings.remembered().server)) {
        shown.suggested = core::ServerClient::asTyped(last.value());
    }
    shown.encrypted = m_client.isEncrypted();
    shown.asksName = m_asksName;
    shown.asksCode = m_asksCode;
    shown.account = m_account;
    shown.manages = m_manages && (m_state == Connection::SignedIn || m_state == Connection::Away);
    shown.role = m_state == Connection::SignedIn || m_state == Connection::Away ? m_role : QString();
    shown.inContact = m_state != Connection::Away;
    shown.awaySince = shown.inContact ? QString() : m_awaySince;
    return shown;
}

QString ServerSource::sessionName() const
{
    return u"session at "_s + m_client.address().toString();
}

QString ServerSource::displayName() const
{
    return u"display link at "_s + m_client.address().toString();
}

void ServerSource::forgetTheLink()
{
    m_secrets.forget(displayName());
    m_displayKey.clear();
}

void ServerSource::openByTheLink()
{
    // Still on the way to the server, as far as anybody sees: a screen nobody sits at is not shown
    // a line for a password while its link is being handed in.
    m_linkHandedInAt = m_clock.elapsed();
    m_client.openDisplay(m_displayKey, [this, source = QPointer<ServerSource>(this),
                                        turn = ++m_turn](const base::Result<core::Account>& screen) {
        if (source == nullptr || turn != m_turn) {
            return;
        }
        if (screen) {
            // Kept from the first time it worked: it is what the screen comes back in with. Where
            // Windows does not keep it, the wall is shown all the same.
            static_cast<void>(m_secrets.keep(displayName(), m_displayKey));
            // (Taken. Whether its session holds, the first look at the machines says.)
            m_linkTakenUnproven = true;
            enterWall(Event::SomebodySignedIn, screen.value());
            return;
        }
        if (stoppedForItsCertificate(screen.error())) {
            return;
        }
        // Not reached, or answered with anything that is not the QKVM's word about the link: a
        // gateway's error while the server is on its way up, too many questions at once, what
        // is not understood. Said where the server was named, and the link stays kept for it;
        // and where it is the link that is kept, it is handed in again later by itself, after
        // a wait that is longer each time.
        if (screen.error().code != linkNotGood) {
            notComeTo(sentence(screen.error()), true, handTheLinkInLater(screen.error()));
            return;
        }
        // The server's own word: the link was taken back, or has run out. A person signs in, or
        // brings a new link.
        forgetTheLink();
        become(Event::NobodySignedIn, sentence(screen.error()));
    });
}

void ServerSource::loseContact()
{
    if (!means(Event::ContactLost)) {
        return;
    }
    // With the day: a wall that nobody sits at may be read days later.
    m_awaySince = QLocale::system().toString(m_now(), QLocale::ShortFormat);
    become(Event::ContactLost);
}

void ServerSource::comeBackByTheLink()
{
    loseContact();
    // A server that was restarted may be another program than it was: what answers there now is
    // asked what it is before the link is handed to it.
    m_client.askHealth(
        [this, source = QPointer<ServerSource>(this), turn = ++m_turn](const base::Result<core::Health>& health) {
            if (source == nullptr || turn != m_turn) {
                return;
            }
            if (health) {
                m_locked = health.value().locked;
                handTheLinkInAgain();
                return;
            }
            if (stoppedForItsCertificate(health.error())) {
                return;
            }
            // Nothing answers there, or something that is no QKVM: a gateway's page while the server
            // is on its way up, as likely as not. Nobody sits at a wall screen to name its server
            // again, so it is asked again later, with nothing that says who asks, for as long as it
            // takes; the link is handed to nothing but a QKVM this program speaks to.
            if (health.error().code != core::ServerClient::otherVersion) {
                m_again.start(m_awayEveryMs);
                return;
            }
            // A QKVM that speaks another version of its API: one of the two has to be updated, and
            // that is said where a server is named. The link was not handed to it, and stays kept.
            m_displayKey.clear();
            leaveWall(Event::Left, sentence(health.error()));
        });
}

void ServerSource::handTheLinkInAgain()
{
    m_linkHandedInAt = m_clock.elapsed();
    m_client.openDisplay(m_displayKey, [this, source = QPointer<ServerSource>(this),
                                        turn = ++m_turn](const base::Result<core::Account>& screen) {
        if (source == nullptr || turn != m_turn) {
            return;
        }
        if (screen) {
            // (Taken. Whether its session holds, the first look at the machines says: only
            // then does a link that is not taken wait anew, and only then is contact back. A
            // taking that did not hold said "in contact" for the length of one question, and
            // began "away since" anew each time.)
            m_linkTakenUnproven = true;
            m_account = screen.value().name;
            m_manages = screen.value().role == "admin"_L1;
            m_role = roleOf(screen.value());
            m_locked = false;
            noteWhoIsThere(screen.value());
            emit whoChanged(screen.value());
            m_feed.start();
            return;
        }
        if (stoppedForItsCertificate(screen.error())) {
            return;
        }
        // Only the QKVM's own word that the link is not good ends this. Whatever else comes of
        // handing it in (nothing, a gateway's 502, "too many", what is not understood) is tried
        // again later, after a wait that is longer each time: nobody sits at a wall screen to
        // sign in instead.
        if (screen.error().code != linkNotGood) {
            m_again.start(handTheLinkInLater(screen.error()));
            return;
        }
        forgetTheLink();
        leaveWall(Event::SignedOut, sentence(screen.error()));
    });
}

void ServerSource::rememberTheServer()
{
    core::Remembered remembered = m_settings.remembered();
    if (!m_background)
        remembered.server = m_client.address().toString();
    if (m_client.isEncrypted() && !remembered.reachedEncrypted.contains(m_client.address().host())) {
        remembered.reachedEncrypted.append(m_client.address().host());
    }
    if (remembered != m_settings.remembered()) {
        // A PC whose settings cannot be written still shows its wall: the address is typed again
        // the next time.
        static_cast<void>(m_settings.remember(remembered));
    }
}

void ServerSource::connectTo(const QString& address)
{
    // Somebody names a server: one that was being asked again by itself is asked no more, and
    // the one that is named is taken, also while such a question is under way.
    stopTryingAgain();
    if (!means(Event::Named)) {
        return; // one is being asked already, or this is not where a server is named
    }
    const base::Result<QUrl> named = core::ServerClient::addressFrom(address);
    if (!named) {
        m_problem = sentence(named.error());
        emit changed();
        return;
    }
    // A server that was reached by HTTPS once is not spoken to in the clear again: what was typed
    // without saying how is taken to mean HTTPS, and what says "http://" is refused.
    QUrl server = named.value();
    if (server.scheme() == "http"_L1 && m_settings.remembered().reachedEncrypted.contains(server.host())) {
        if (address.trimmed().startsWith("http://"_L1, Qt::CaseInsensitive)) {
            m_problem = words("This qKVM was reached with https:// before, and is not spoken to unencrypted. "
                              "Type its address with https:// in front.");
            emit changed();
            return;
        }
        server.setScheme(u"https"_s);
    }
    if (const base::Result<void> taken = m_client.setAddress(server.toString()); !taken) {
        m_problem = sentence(taken.error());
        emit changed();
        return;
    }
    // The certificate this server is known by, where somebody said yes to one: it is spoken to only
    // if it shows that one. Whoever asked to stay signed in on this PC is, as long as the server
    // still knows the session; the session goes out only after the certificate was looked at.
    m_client.setKnownCertificate(m_settings.remembered().certificates.value(m_client.address().authority()));
    m_client.setSession(m_secrets.kept(sessionName()));
    if (m_background && m_client.session().isEmpty()) {
        become(Event::Named);
        become(Event::NobodySignedIn);
        return;
    }
    // A display link: the one that was pasted with the address, or the one kept for this server.
    // What is kept is read as what was pasted is: only a key of the server's making is one.
    const QByteArray pasted = core::ServerClient::displayKeyIn(address);
    m_displayKey =
        !pasted.isEmpty()
            ? pasted
            : core::ServerClient::displayKeyIn(u"#key="_s + QString::fromLatin1(m_secrets.kept(displayName())));
    if (m_background)
        m_displayKey.clear();
    // (Named by somebody: how often a link was not taken before is not this naming's.)
    m_linkNotTaken = 0;
    m_linkTakenUnproven = false;
    become(Event::Named);
    askWhatAnswersThere();
}

bool ServerSource::stoppedForItsCertificate(const base::Error& error)
{
    if (error.code != core::ServerClient::certificateUnknown && error.code != core::ServerClient::certificateChanged) {
        return false;
    }
    leaveWall(Event::CertificateShown, {});
    return true;
}

void ServerSource::trust()
{
    const QByteArray shown = m_client.shownCertificate();
    if (!means(Event::Trusted) || shown.isEmpty()) {
        return;
    }
    core::Remembered remembered = m_settings.remembered();
    remembered.certificates.insert(m_client.address().authority(), shown);
    // Where the settings cannot be written, the yes holds while the program runs, and is asked for
    // again the next time.
    static_cast<void>(m_settings.remember(remembered));
    m_client.setKnownCertificate(shown);
    become(Event::Trusted);
    askWhatAnswersThere();
}

void ServerSource::askWhatAnswersThere()
{
    m_locked = false;
    ifAQkvmAnswersThere([this] { askWhoIsSignedIn(); });
}

void ServerSource::askWhoIsSignedIn()
{
    m_client.askSession([this, source = QPointer<ServerSource>(this),
                         turn = ++m_turn](const base::Result<core::Session>& session) {
        if (source == nullptr || turn != m_turn) {
            return;
        }
        if (!session) {
            if (!stoppedForItsCertificate(session.error())) {
                notComeTo(sentence(session.error()), true);
            }
            return;
        }
        if (!session.value().setUp) {
            notComeTo(
                words("This qKVM has not been set up yet. Open its page in a browser to set it up, then come back."),
                true);
            return;
        }
        m_asksName = session.value().asksName;
        m_asksCode = false;
        if (session.value().account) {
            enterWall(Event::SomebodySignedIn, *session.value().account);
        } else {
            // What was kept, if anything, is a session the server no longer knows.
            m_secrets.forget(sessionName());
            if (m_displayKey.isEmpty()) {
                become(Event::NobodySignedIn);
            } else {
                // A screen nobody sits at signs in with its link, and asks nobody for a password.
                openByTheLink();
            }
        }
    });
}

void ServerSource::askWhoItIsNow()
{
    // (Beside whatever else is asked, so no turn of its own: its answer is nobody's once the wall
    // was left, or come back to by a display link.)
    m_client.askSession(
        [this, source = QPointer<ServerSource>(this), turn = m_turn](const base::Result<core::Session>& session) {
            // What is wrong with the server, or that the session is gone, the looks at the machines
            // say.
            if (source == nullptr || turn != m_turn || !session || !session.value().account) {
                return;
            }
            const core::Account& account = *session.value().account;
            if (stopForEnrollment(account))
                return;
            if (m_background && account.isDisplay) {
                leaveWall(Event::Left, words("Open this display wall to use its saved link."));
                return;
            }
            m_account = account.name;
            m_manages = account.role == "admin"_L1;
            m_role = roleOf(account);
            noteWhoIsThere(account);
            emit whoChanged(account);
            emit changed();
        });
}

void ServerSource::verifyAccount(std::function<void(const QString&)> done)
{
    m_client.askSession([this, source = QPointer<ServerSource>(this), turn = m_turn,
                         done = std::move(done)](const base::Result<core::Session>& session) {
        if (source == nullptr || turn != m_turn || !isAtTheWall() || !session || !session.value().account) {
            done({});
            return;
        }
        const auto& account = *session.value().account;
        if (stopForEnrollment(account)) {
            done({});
            return;
        }
        m_account = account.name;
        m_manages = account.role == "admin"_L1;
        m_role = roleOf(account);
        noteWhoIsThere(account);
        emit whoChanged(account);
        emit changed();
        done(account.id);
    });
}

void ServerSource::signIn(const QString& name, const QString& password, const QString& code, bool keepSignedIn)
{
    if (!means(Event::SignInSent)) {
        return;
    }
    become(Event::SignInSent);
    // The QKVM that was asked what it is when the server was named may have made way for
    // something else since: its session ended, and this line for a password came up. So what
    // answers there now is asked what it is before the password goes to it, each time, and the
    // password goes to nothing but a QKVM this program speaks to. It is held here for that one
    // question, and let go of with it.
    ifAQkvmAnswersThere([this, name, password, code, keepSignedIn] { sendSignIn(name, password, code, keepSignedIn); });
}

void ServerSource::sendSignIn(const QString& name, const QString& password, const QString& code, bool keepSignedIn)
{
    // Kept signed in: a session of thirty days, kept with the secrets. Otherwise an ordinary one,
    // which lasts while the program runs and asks the server now and then, and is kept nowhere.
    m_client.signIn(
        m_asksName ? name.trimmed() : QString(), password, m_asksCode ? code.trimmed() : QString(), keepSignedIn,
        [this, source = QPointer<ServerSource>(this), keepSignedIn,
         turn = ++m_turn](const base::Result<core::Account>& account) {
            if (source == nullptr || turn != m_turn) {
                return;
            }
            if (account) {
                if (keepSignedIn) {
                    // Where Windows does not keep it, the wall is shown all the same, and the
                    // password is asked for the next time.
                    static_cast<void>(m_secrets.keep(sessionName(), m_client.session()));
                } else {
                    m_secrets.forget(sessionName());
                }
                enterWall(Event::SignInTaken, account.value());
                return;
            }
            if (stoppedForItsCertificate(account.error())) {
                return;
            }
            // The server asks for more than was typed: that is the next thing to ask, and nobody's
            // mistake.
            QString problem = sentence(account.error());
            if (account.error().code == "name_required" && !m_asksName) {
                m_asksName = true;
                problem.clear();
            } else if (account.error().code == "code_required" && !m_asksCode) {
                m_asksCode = true;
                problem.clear();
            }
            become(account.error().code == core::ServerClient::unreachable ? Event::NoAnswer : Event::SignInRefused,
                   problem);
        });
}

bool ServerSource::stopForEnrollment(const core::Account& account)
{
    if (!account.mustEnroll)
        return false;
    m_secrets.forget(sessionName());
    m_client.setSession({});
    leaveWall(Event::Left, words("This account must set up two-factor sign-in. Open this qKVM's page in a browser, "
                                 "complete enrollment, then sign in here again."));
    return true;
}

void ServerSource::enterWall(Event event, const core::Account& account)
{
    if (stopForEnrollment(account))
        return;
    if (m_background && account.isDisplay) {
        leaveWall(Event::Left, words("Open this display wall to use its saved link."));
        return;
    }
    if (!m_background && !account.isDisplay) {
        auto remembered = m_settings.remembered();
        const auto address = m_client.address().toString();
        if (!m_secrets.kept(sessionName()).isEmpty()) {
            remembered.servers.removeAll(address);
            remembered.servers.prepend(address);
            remembered.servers =
                remembered.servers.first(std::min(remembered.servers.size(), core::Remembered::mostMonitoredServers));
        } else {
            remembered.servers.removeAll(address);
        }
        if (remembered != m_settings.remembered())
            static_cast<void>(m_settings.remember(remembered));
    }
    rememberTheServer();
    m_account = account.name;
    m_manages = account.role == "admin"_L1;
    m_role = roleOf(account);
    m_asksCode = false;
    // (Somebody has signed in now: what the server said of itself before that is no longer so.)
    m_locked = false;
    m_looks = 0;
    // (And a screen that was opened with a display link is one nobody sits at: the wall, and no
    // bar, in every window; and this PC is a wall screen from now on, before anybody hears that
    // the wall was come to.)
    noteWhoIsThere(account);
    emit entered(account);
    become(event);
    m_feed.start();
}

void ServerSource::noteWhoIsThere(const core::Account& account)
{
    if (account.isDisplay && m_screen != nullptr) {
        m_screen->becameOneFor(m_client.address().toString());
    }
}

void ServerSource::leaveWall(Event event, const QString& problem)
{
    ++m_turn;
    m_again.stop();
    m_tryingAgain = false;
    m_feed.stop();
    emptyTheWall();
    become(event, problem);
}

void ServerSource::emptyTheWall()
{
    emit left();
    m_account.clear();
    m_manages = false;
    m_role.clear();
    m_asksCode = false;
    emit emptied();
}

void ServerSource::ifAQkvmAnswersThere(std::function<void()> then)
{
    m_client.askHealth([this, source = QPointer<ServerSource>(this), turn = ++m_turn,
                        then = std::move(then)](const base::Result<core::Health>& health) {
        if (source == nullptr || turn != m_turn) {
            return;
        }
        if (!health) {
            // Nothing there, no QKVM, or one that speaks another version of its API: said where
            // a server is named, and nothing more is asked of it. Also where a password was
            // waiting to go: there "no answer" would mean the line for it again, as for a server
            // that fell silent with a password, and this one was sent none.
            if (stoppedForItsCertificate(health.error())) {
                return;
            }
            if (m_state == Connection::Connecting) {
                // (A screen that came by its kept link asks again later. Not a QKVM of another
                // version: one of the two has to be updated first, and somebody has to do that.)
                notComeTo(sentence(health.error()), health.error().code != core::ServerClient::otherVersion);
            } else {
                become(Event::Left, sentence(health.error()));
            }
            return;
        }
        m_locked = health.value().locked;
        then();
    });
}

bool ServerSource::handedTheLinkInJustNow() const
{
    return m_linkHandedInAt && m_clock.elapsed() - *m_linkHandedInAt < m_awayEveryMs;
}

void ServerSource::ifAQkvmStillAnswersThere(std::function<void()> then,
                                            std::function<void(const base::Error&)> otherwise)
{
    m_client.askHealth([this, source = QPointer<ServerSource>(this), turn = m_turn, then = std::move(then),
                        otherwise = std::move(otherwise)](const base::Result<core::Health>& health) {
        const auto notSent = [&otherwise](const QString& why) {
            otherwise(base::Error{FilesView::nothingWasSent, why.toStdString()});
        };
        if (source == nullptr || turn != m_turn || !isAtTheWall()) {
            // The wall was left meanwhile, or its session went and was come back to: what was
            // to be sent was typed by whoever was at the wall then, and goes nowhere.
            notSent(words("The wall was left before this could be sent. Nothing was sent."));
            return;
        }
        if (!health) {
            // Nothing there, no QKVM, or one that speaks another version of its API: nothing is
            // sent to it, and whoever asked is told why. (Not the wall's end by itself: the
            // looks at the machines find a server that is away, or another, and say so.)
            notSent(sentence(health.error()));
            return;
        }
        then();
    });
}

void ServerSource::notComeTo(const QString& problem, bool mayComeYet, int afterMs)
{
    m_tryingAgain = mayComeYet && (m_background || cameByItsKeptLink());
    become(Event::NoAnswer, problem);
    if (m_tryingAgain) {
        m_again.start(std::max(afterMs, m_awayEveryMs));
    }
}

bool ServerSource::cameByItsKeptLink() const
{
    // (What is kept is read as what was pasted is: only a key of the server's making is one.)
    return !m_displayKey.isEmpty() &&
           m_displayKey ==
               core::ServerClient::displayKeyIn(u"#key="_s + QString::fromLatin1(m_secrets.kept(displayName())));
}

void ServerSource::stopTryingAgain()
{
    if (!m_tryingAgain) {
        return;
    }
    m_tryingAgain = false;
    m_again.stop();
    // What the server still answers to a question that was asked by itself is nobody's. Why it
    // was not come to stays said.
    ++m_turn;
    if (m_state == Connection::Connecting) {
        become(Event::Left, m_problem);
    } else {
        emit changed();
    }
}

int ServerSource::handTheLinkInLater(const base::Error& why)
{
    // Not handed in at all, since what answers there had not said again what it is: nothing
    // was refused, and the pace is the wall's own. (Set, and not shown by a test: what answers
    // there is asked what it is before each handing-in, so the client has none to hold back.)
    if (why.code == core::ServerClient::notSent) {
        return m_awayEveryMs;
    }
    // (Twenty doublings are longer than the longest wait from any pace. The longest is the
    // client's, which takes no longer one from a server either.)
    constexpr int longestMs = core::ServerClient::longestLinkWaitMs;
    const qint64 doubled = qint64{std::max(1, m_awayEveryMs)} << std::min(m_linkNotTaken, 20);
    ++m_linkNotTaken;
    return std::max(static_cast<int>(std::min<qint64>(doubled, longestMs)), m_client.linkWaitSaidMs());
}

} // namespace qkvm::ui
