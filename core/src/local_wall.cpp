#include <qkvm/core/local_wall.h>

#include <algorithm>
#include <initializer_list>
#include <list>
#include <optional>
#include <utility>

#include <QCoreApplication>
#include <QPointer>
#include <QRandomGenerator>
#include <QSet>
#include <QStringDecoder>
#include <QUrl>

#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/wiped.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// How often it is looked whether pictures are still kept for somebody.
constexpr int tidyEveryMs = 30'000;
// The pace a wall's pictures are asked at where nobody chose another: the server's own.
constexpr double everySeconds = 1;

// A sentence of the wall's own, where the server has none.
QString words(const char* text)
{
    return QCoreApplication::translate("LocalWall", text);
}

base::Error unknown()
{
    return {"not_found", "Unknown device"};
}

// The server's words for all of these: a card and a notice show them as they are. (Which KVM, as
// a sentence names one: LocalWall::calledInASentence.)
base::Error busyWith(const QString& called)
{
    return {"busy",
            u"Something else is being done to %1 at this moment. Wait for it to finish."_s.arg(called).toStdString()};
}

base::Error notSaved(const char* what, const base::Error& why)
{
    return {"not_saved", std::string(what) + " could not be saved (" + why.message + "), so nothing was changed."};
}

base::Error onTheWall()
{
    return {"duplicate", "This device is already on the wall"};
}

base::Error keptThere()
{
    return {"duplicate", "A KVM that another version of qKVM added is kept at this address. This version does not "
                         "show it, and puts no other there."};
}

// An address as the program writes one (kvm::addressFrom), which is the only kind the settings
// read back at the next start: a KVM under any other would be on the wall until the program ends,
// and its password in the store without it after that. Whoever adds or changes a KVM has made its
// address so already; the wall holds the rule whoever asks.
std::optional<base::Error> notAnAddress(const QString& address)
{
    const base::Result<QUrl> asWritten = kvm::addressFrom(address);
    if (!asWritten) {
        return asWritten.error();
    }
    if (asWritten.value().toString(QUrl::FullyEncoded) != address) {
        return base::Error{"invalid", "This address is not written the way the wall keeps one"};
    }
    return std::nullopt;
}

base::Error passwordNeeded()
{
    return {"password_needed",
            "There is nothing to know this device by at another address. Type its password to use it there."};
}

// A KVM's driver says this of one it has no password for (kvmd_device.cpp), before it makes a
// line. Said for it where a kept password was to go along with a KVM, and there is none.
base::Error noPassword()
{
    return {"no_password", "No password saved for this device"};
}

// What a driver that has only just been made says to whoever asks it for a picture, said for
// it to those who had asked the one before: asking again soon is the thing to do.
base::Error connecting()
{
    return {"no_stream", "Connecting to the KVM"};
}

// Whoever is owed an answer by a driver that was let go of at the program's end.
base::Error stopped()
{
    return {"stopped", "The program is ending: nothing more is asked of this device."};
}

// What is said beside the web's sentence where a second card of a server is refused: the web's page would offer to
// keep both, and this program does not (the owner's word, M7 Q2).
constexpr QLatin1StringView oneCardOfAServer{" A server has one card on this PC's wall."};

// And this program's own, where the server has no words: there a certificate is looked at only
// where a device has one.
base::Error noCertificate()
{
    return {"no_certificate", "This device is reached over plain HTTP, where there is no certificate"};
}

base::Error noSha256()
{
    return {"invalid", "This is not the SHA-256 of a certificate"};
}

// A KVM's driver says this of one that is said to be known by a certificate and names none that
// is one (kvmd_device.cpp), with what to do about it. Said for it where a password was to be kept
// for such a KVM: none is.
base::Error certificateNotRead()
{
    return {"cert_invalid",
            "The certificate saved for this device cannot be read. Look at its certificate and trust it again."};
}

// A KVM's driver says these of what shows a certificate that is not the KVM's (kvmd_device.cpp).
// Said for it of a KVM that was not asked yet what it is, whose driver is asked nothing: a card
// shows them as it shows the driver's, with the way on that it has for each.
base::Error certificateChanged()
{
    return {"cert_mismatch", "Certificate does not match the one saved for this device"};
}

base::Error certificateOfAnUnknownSigner()
{
    return {"cert_invalid", "The device's certificate was signed by a maker of certificates that Windows does not "
                            "know. Have Windows trust that maker, and the device is let in by that."};
}

// And of one that is known by Windows' word, and shows what Windows does not trust: the driver's
// code, and more than its sentence, since this is where a KVM from a file stands whose file
// named no certificate for it. Nothing it shows first is taken on trust. (None of these
// sentences says "its password": a KVM that was taken from a file without one has none.)
base::Error certificateNotTrustedAndNothingSent()
{
    return {"cert_invalid", words("Windows does not trust the certificate this device shows, and no certificate is "
                                  "saved for it. Nothing was sent to it. Review its certificate and trust it; no "
                                  "password goes to it before that.")
                                .toStdString()};
}

// The times an asking of what answers keeps: what the KVMs' drivers are given, step for step,
// and never less (a test makes those short): of the two families, the longer each time. An
// asking that gave up sooner than the driver would said "nothing answers" of a KVM the driver
// reaches, and nothing is signed in to that was not asked.
KvmProbe::Pace askingPace(const DriverPaces& paces)
{
    return KvmProbe::Pace::beforeASignInWith(std::max(paces.kvmd.connectWithinMs, paces.jetkvm.connectWithinMs),
                                             std::max(paces.kvmd.quietMs, paces.jetkvm.quietMs),
                                             std::max(paces.kvmd.transferMs, paces.jetkvm.transferMs));
}

// And of a server's controller: as long as its driver too, and its service root as long as the driver waits for that
// before a sign-in (an older board takes nine to twenty seconds to show it). Only for a card of that family: a KVM
// that is off is given up on in a KVM's time.
KvmProbe::Pace serverAskingPace(const DriverPaces& paces)
{
    const KvmProbe::Pace asKvms = askingPace(paces);
    return KvmProbe::Pace::beforeASignInWith(
        std::max(asKvms.connectWithinMs, paces.redfish.connectWithinMs),
        std::max(asKvms.quietMs, paces.redfish.quietMs),
        std::max({paces.kvmd.transferMs, paces.jetkvm.transferMs, paces.redfish.transferMs}),
        paces.redfish.rootUnsignedMs);
}

// (What to do instead is said with it: a KVM that is taken off the wall and added anew is spoken
// to in the clear only where somebody typed "http://" for it.)
base::Error notInTheClear()
{
    return {"not_in_the_clear",
            "This device is on the wall over HTTPS. It is not changed to plain HTTP at the same address, where its "
            "password would travel unencrypted. To use it that way, remove it from the wall and add it again."};
}

// A certificate's SHA-256 is this long. What is said to be one and is not that long is none.
constexpr qsizetype sha256Bytes = 32;

// Where the password that is kept for a KVM may go without being given anew (SPEC.md section 8),
// for whoever signs in with it and for whoever changes the KVM alike: to the KVM as it is known,
// at the address it has; or to another address only where both are HTTPS and the KVM is known by
// a certificate, and then to what shows that certificate and to nothing else. (Under which user
// name it goes is none of this.) The one exception is not made here: a JetKVM over plain HTTP taken
// onto HTTPS at its own host, where a JetKVM answered there (LocalWall::ontoHttps, by change() and
// signerIn alone).
bool keptPasswordMayGoTo(const OwnKvm& there, const OwnKvm& known)
{
    if (there.knownByCertificate != known.knownByCertificate || there.certificate != known.certificate) {
        return false;
    }
    if (there.address == known.address) {
        return true;
    }
    // A server's controller: to its own address, and to no other, whatever certificate shows there. Its maker may
    // have put one certificate, and one key, on every board: what shows it is not known to be this server (the
    // web's known_again leaves out "redfish").
    if (there.kind == "redfish"_L1 || known.kind == "redfish"_L1) {
        return false;
    }
    // Somewhere else, the KVM is the one that shows the certificate it is known by, and nothing
    // else is: so there has to be such a certificate, and a line on which one is shown, here
    // and there.
    const auto encrypted = [](const OwnKvm& kvm) { return QUrl(kvm.address).scheme() == "https"_L1; };
    return encrypted(known) && encrypted(there) && known.knownByCertificate && known.certificate.size() == sha256Bytes;
}

// The first line of a KVM's secret: what it is, and which form of it (local_wall.h).
constexpr QByteArrayView secretMark = "QKVM-KVM-2";
// Its third line: what the KVM is known by. A word for "by none", and a word before a
// certificate's SHA-256, so that neither can be read as the other, whatever the hex is.
constexpr QByteArrayView knownByNone = "none";
constexpr QByteArrayView knownBySha256 = "sha256:";

// The three lines a KVM's secret begins with: the mark, and what the password behind them is
// saved for. No line of them can end early. An address stands there as it is written, for every
// one the program writes (letters, digits and ".-_:/[]"), and anything else in one as a "%" and
// two hex digits; a certificate's SHA-256 is hex.
//
// Nothing for a KVM that is said to be known by a certificate and names none that is one: there
// are no such lines, so nothing is saved for it and nothing that is kept is its password.
std::optional<QByteArray> savedFor(const OwnKvm& kvm)
{
    if (kvm.knownByCertificate && kvm.certificate.size() != sha256Bytes) {
        return std::nullopt;
    }
    QByteArray lines = secretMark.toByteArray();
    lines += '\n';
    lines += kvm.address.toUtf8().toPercentEncoding(":/[]"_ba);
    lines += '\n';
    lines.append(kvm.knownByCertificate ? knownBySha256 : knownByNone);
    lines += kvm.knownByCertificate ? kvm.certificate.toHex() : QByteArray();
    lines += '\n';
    return lines;
}

// What is kept in the store for a KVM's password: the lines it is saved for (savedFor), and the
// password behind them in UTF-8. Put together in room that is asked for once, so that nothing of it
// is left behind where it grew, and written over when this goes, on every way out of where it
// stands (an exception's too); and so is the password's UTF-8 on its way there. What the store
// holds of it is the store's (wipeInPlace): whoever keeps a password holds one of these until it
// returns, so that what the store lets go of again meanwhile (told to forget it, or to keep what
// was kept before) is this one's to write over.
class KeptAs {
public:
    KeptAs(QByteArrayView lines, const QString& password)
    {
        QByteArray signs = password.toUtf8();
        const WipedOnLeaving overSigns(signs);
        m_bytes.reserve(lines.size() + signs.size());
        m_bytes.append(lines);
        m_bytes.append(QByteArrayView(signs));
    }
    KeptAs(const KeptAs&) = delete;
    KeptAs& operator=(const KeptAs&) = delete;
    KeptAs(KeptAs&&) = delete;
    KeptAs& operator=(KeptAs&&) = delete;
    ~KeptAs() = default;

    const QByteArray& bytes() const noexcept { return m_bytes; }

private:
    QByteArray m_bytes;
    // (After what it writes over, so that it goes first.)
    WipedOnLeaving<QByteArray> m_over{m_bytes};
};

// The password in a secret, where the secret was saved for this KVM as it is. Nothing where it
// was saved for another address or another certificate, is of another form, or is none at all:
// then the KVM has no password, and nothing is made of what is there.
QString passwordIn(const QByteArray& secret, const OwnKvm& kvm)
{
    const std::optional<QByteArray> lines = savedFor(kvm);
    if (!lines || !secret.startsWith(*lines)) {
        return {};
    }
    // Every sign of it, as it was given. (QString::fromUtf8 takes a first U+FEFF for a mark of
    // the order of the bytes and drops it: a password that began with one came back without.)
    // Bytes that are no text are no password this program wrote.
    QStringDecoder signs(QStringConverter::Utf8,
                         QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
    QString password = signs.decode(QByteArrayView(secret).sliced(lines->size()));
    // What was read is written over on every way out of here but the one that hands it on: where
    // its signs are found wrong, and where something throws. (What Qt's decoder holds while it
    // decodes is Qt's, and is not reached from here.)
    WipedOnLeaving over(password);
    if (signs.hasError()) {
        return {};
    }
    return over.handOn();
}

// The password that is kept for a KVM, read from the store for one use of it, as passwordIn reads
// it: what the store handed over is written over before this returns, on every way out. (Where
// the store shares those bytes with what it holds itself, as one in memory does, they are the
// store's: wipeInPlace.) Every reading of a KVM's password goes through here, but the one of a
// change, which holds what it read to put it back where the list cannot be written (alter), and
// writes it over itself.
QString keptPasswordOf(SecretStore& secrets, const OwnKvm& kvm)
{
    QByteArray secret = secrets.kept(LocalWall::secretOf(kvm.id));
    const WipedOnLeaving over(secret);
    return passwordIn(secret, kvm);
}

// Whether a KVM's card says another thing for what an asking came to than it said before.
bool saysAnother(const std::optional<base::Error>& said, const base::Error& now)
{
    return !said || said->code != now.code || said->message != now.message;
}

QString newId(const QList<OwnKvm>& beside, const QJsonArray& keptAside)
{
    // Eight of 0-9 and a-f, and not one that is taken: by a KVM of the wall, or by one the file keeps
    // that this build does not read (its secret is kept under it).
    for (;;) {
        const QString id = u"%1"_s.arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
        const bool taken = std::ranges::any_of(beside, [&id](const OwnKvm& one) { return one.id == id; }) ||
                           std::ranges::any_of(keptAside, [&id](const QJsonValue& one) {
                               return one.toObject()["id"_L1].toString() == id;
                           });
        if (!taken) {
            return id;
        }
    }
}

} // namespace

LocalWall::LocalWall(Settings& settings, SecretStore& secrets, QObject* parent)
    : LocalWall(settings, secrets, DriverPaces{}, parent)
{}

LocalWall::LocalWall(Settings& settings, SecretStore& secrets, DriverPaces paces, QObject* parent)
    : LocalWall(settings, secrets, std::move(paces), Own{}, parent)
{}

LocalWall::LocalWall(Settings& settings, SecretStore& secrets, KvmdDevice::Pace pace, QObject* parent)
    : LocalWall(settings, secrets, DriverPaces::alike(std::move(pace)), Own{}, parent)
{}

LocalWall::LocalWall(Settings& settings, SecretStore& secrets, KvmdDevice::Pace pace, Own own, QObject* parent)
    : LocalWall(settings, secrets, DriverPaces::alike(std::move(pace)), std::move(own), parent)
{}

LocalWall::LocalWall(Settings& settings, SecretStore& secrets, DriverPaces paces, Own own, QObject* parent)
    : QObject(parent), m_settings(settings), m_secrets(secrets), m_actions(settings, secrets), m_screens(own.screens),
      m_paces(std::move(paces)), m_own(std::move(own))
{
    m_screens.watch([this] { return watchedScreens(); });
    // A change of the wall's saved actions is a change of the wall, to whoever shows it.
    connect(&m_actions, &OwnActions::changed, this, &LocalWall::changed);
    m_clock.start();
    for (const OwnKvm& kvm : m_settings.remembered().kvms) {
        // Held back for a twin when the program last ran: so from the start, its card saying so, until a sign lets go
        // of it, as in that run (from the third reading of the twins, L2). Its first asking comes as any first one.
        const std::optional<OwnKvm> twin = heldFor(kvm.id);
        // Asked what it is: where the list says so, of a KVM of which that is kept at all. One
        // over plain HTTP is asked once in every run, whatever a file says of it.
        const bool asked = !twin && kvm.askedWhatItIs && keepsThatItWasAsked(kvm);
        // (With the password that was saved for it as it is. One for which there is none is on
        // the wall all the same, and its driver says that it has none. Of one that was not
        // asked yet what it is, nothing is read from the store.)
        // (What was read is written over once the driver has its own copy: wiped.h, SecretText. F5.)
        QString kept = asked ? keptPasswordOf(m_secrets, kvm) : QString();
        const WipedOnLeaving overKept(kept);
        hold(kvm, asked, kept);
        if (twin) {
            Held& one = m_held.back();
            one.heldBackByTwin = true;
            one.saidWhenAsked = heldBackFor(*twin);
        }
    }
    m_tidy.setInterval(tidyEveryMs);
    connect(&m_tidy, &QTimer::timeout, this, [this] {
        for (const Held& one : m_held) {
            one.device->tidy();
        }
    });
    m_tidy.start();
    m_writeSoon.setSingleShot(true);
    connect(&m_writeSoon, &QTimer::timeout, this, [this] {
        // (Where it cannot be written, it stays noted, and goes with the next writing or at the
        // end. What is lost with it is found out again by one asking.)
        static_cast<void>(m_settings.writeDown());
    });
    // What the wall says by itself of its KVMs: once for all that came to something within a
    // short while of the first.
    m_saySoon.setSingleShot(true);
    connect(&m_saySoon, &QTimer::timeout, this, &LocalWall::cameToSomething);
}

LocalWall::~LocalWall()
{
    // At the latest when the wall goes: what it noted by itself and has not written down yet.
    static_cast<void>(m_settings.writeDown());
}

bool LocalWall::keepsThatItWasAsked(const OwnKvm& kvm)
{
    // Over HTTPS a certificate (the KVM's own, or one Windows trusts for its name) says at every
    // line who answers. Over plain HTTP nothing does: there it is asked anew in every run.
    return kvm.address.startsWith("https://"_L1);
}

void LocalWall::noteByItself(const QList<OwnKvm>& kvms, const QHash<QString, QString>& certificates)
{
    Remembered with = m_settings.remembered();
    with.kvms = kvms;
    with.kvmCertificates = certificates;
    m_settings.note(with);
    // Written a short while after the last of what is noted, and no later than a while after
    // the first: one writing for a wall of KVMs that are asked within moments of each other.
    if (!m_writeSoon.isActive()) {
        m_notedSince = m_clock.elapsed();
    }
    const qint64 atTheLatest = m_notedSince + m_own.writtenAtTheLatestMs - m_clock.elapsed();
    m_writeSoon.start(static_cast<int>(std::clamp<qint64>(atTheLatest, 0, std::max(0, m_own.writtenAfterMs))));
}

void LocalWall::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

const QList<OwnKvm>& LocalWall::kvms() const
{
    return m_settings.remembered().kvms;
}

QString LocalWall::hostOf(const OwnKvm& kvm)
{
    // In the signs of the address, as it was taken: Qt would decode a name in its xn-- form
    // under the endings it knows, into letters of another script that can read as a well-known
    // name. What a tile shows, and what a KVM is called where no name was typed, is what
    // somebody typed or a file held, and nothing that only looks like it.
    return QUrl(kvm.address).authority(QUrl::FullyEncoded);
}

QString LocalWall::secretOf(const QString& id)
{
    return u"kvm:"_s + id;
}

LocalWall::Held* LocalWall::held(const QString& id)
{
    const auto found = std::ranges::find_if(m_held, [&id](const Held& one) { return one.id == id; });
    return found == m_held.end() ? nullptr : &*found;
}

std::unique_ptr<OwnDevice> LocalWall::driverFor(Held& one, const OwnKvm& kvm, const QString& password) const
{
    // Of its family: a JetKVM's, a server's controller's, or a kvmd's. (Those three are the only ones a KVM of
    // the wall is ever of, add(), takeFromAFile() and the settings' file seeing to that: OwnKvm::namesAFamily.)
    if (kvm.kind == "jetkvm"_L1) {
        // (A JetKVM has a password and no user name. And none of its password where it is said to be
        // known by what is no certificate, whoever hands one over, as for a kvmd's: and a JetKVM's
        // driver that has none asks as one that needs none.)
        // (And the number it is known by, which what answers at its address is held to before every connection: J5.)
        JetKvmDevice::Where where{QUrl(kvm.address), kvm.knownByCertificate, kvm.certificate, kvm.number};
        // What outlives each of its drivers goes to the next one (design 3.4): that the program stepped
        // aside for another viewer, and the connection the next one waits for.
        if (!one.jetkvm) {
            one.jetkvm = std::make_shared<JetKvmKept>();
        }
        return std::make_unique<JetKvmDevice>(std::move(where), savedFor(kvm) ? password : QString(), m_paces.jetkvm,
                                              one.jetkvm);
    }
    if (kvm.kind == "redfish"_L1) {
        // (Held to the server it is kept by: which server answers is asked by that before anything goes.)
        RedfishDevice::Where where{QUrl(kvm.address), kvm.user, kvm.knownByCertificate, kvm.certificate, kvm.machine};
        return std::make_unique<RedfishDevice>(std::move(where), savedFor(kvm) ? password : QString(), m_paces.redfish);
    }
    KvmdDevice::Where where;
    where.address = QUrl(kvm.address);
    where.user = kvm.user;
    where.knownByCertificate = kvm.knownByCertificate;
    where.certificate = kvm.certificate;
    // (No password for a KVM that is said to be known by what is no certificate, whoever hands
    // one over: that is the wall's to keep, whatever a driver does with one.)
    return std::make_unique<KvmdDevice>(std::move(where), savedFor(kvm) ? password : QString(), m_paces.kvmd);
}

void LocalWall::hold(const OwnKvm& kvm, bool asked, const QString& password)
{
    if (m_ended) {
        return;
    }
    Held one;
    one.id = kvm.id;
    one.asked = asked;
    // No password for the driver of a KVM that was not asked yet what it is: none goes anywhere
    // before that, whatever is asked of that driver.
    one.device = driverFor(one, kvm, one.asked ? password : QString());
    listenTo(one);
    m_held.push_back(std::move(one));
}

void LocalWall::listenTo(const Held& one)
{
    connect(one.device.get(), &OwnDevice::showedItsCertificate, this, [this, id = one.id](const QByteArray& der) {
        // A turn of the loop later: the driver is in the middle of making a line, and what is
        // kept is written to a file.
        later([this, id, der] { keepCertificate(id, der); });
    });
    // The first look of a driver that was only just made: what it finds is what the KVM's card
    // says from then on, where it said "connecting" until then. The wall says so by itself, so
    // that nobody has to wait for their next turn to ask. (A driver that has no password
    // because its KVM was not asked yet is asked nothing, and takes no look.)
    connect(one.device.get(), &OwnDevice::looked, this, &LocalWall::noteThatOneCameToSomething,
            Qt::SingleShotConnection);
    // A JetKVM's other way offers HTTPS: a turn of the loop later, since the driver is in the middle of its
    // asking, and following it there lets this driver go.
    if (const auto* jet = qobject_cast<const JetKvmDevice*>(one.device.get())) {
        connect(jet, &JetKvmDevice::offersHttps, this,
                [this, id = one.id](const QUrl& from, const QUrl& at, const KvmProbe::Found& found) {
                    later([this, id, from, at, found] {
                        const base::Result<bool> came = followOntoHttps(id, from, at, found);
                        Held* one = held(id);
                        const auto known =
                            std::ranges::find_if(kvms(), [&id](const OwnKvm& kvm) { return kvm.id == id; });
                        if (came || one == nullptr || known == kvms().end()) {
                            return;
                        }
                        // Its HTTPS is another of the wall's: held back for it (the one judgement of such a look), its
                        // driver with no password from here on, and asked what it is anew.
                        static_cast<void>(judgeItsHttps(id, from, found));
                    });
                });
        // Every sign-in of its own (for its viewer: a card, a window, a session refused) looks at its HTTPS first, on
        // that very sign-in, and sends nothing where that shows the certificate another KVM of the wall is known by:
        // the KVM is held back then, as below (from the reading of 2g/2h, M1).
        if (auto* signsIn = qobject_cast<JetKvmDevice*>(one.device.get())) {
            signsIn->lookAtItsHttpsBeforeEverySignIn(twinCheckFor(one.id));
        }
        // Its driver took the number the JetKVM at its address gives (none was kept): kept with the KVM, for its next
        // driver and the next start (J5, the web's `known`). Only where it is still the driver of the KVM, at the
        // address it was seen at, and nothing else was kept meanwhile.
        connect(jet, &JetKvmDevice::knownAs, this,
                [this, id = one.id, by = QPointer<const JetKvmDevice>(jet)](const QUrl& from, const QString& before,
                                                                            const QString& now) {
                    Held* still = held(id);
                    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& kvm) { return kvm.id == id; });
                    if (by == nullptr || still == nullptr || still->device.get() != by.data() ||
                        known == kvms().end() || known->kind != "jetkvm"_L1 ||
                        known->address != from.toString(QUrl::FullyEncoded) || known->number != before ||
                        !exchange::isADevicesOwnNumber(now)) {
                        return;
                    }
                    QList<OwnKvm> with = kvms();
                    with[known - kvms().begin()].number = now;
                    // (Noted, and written down with what else the wall notes by itself, as a first certificate is.)
                    noteByItself(with, m_settings.remembered().kvmCertificates);
                });
        // And whatever else its HTTPS answered, what that line showed: the certificate another KVM of the wall is
        // known by is that one, answering otherwise just then (an error, an answer past its time). Of this driver,
        // still where it spoke to it.
        connect(
            jet, &JetKvmDevice::httpsShowed, this,
            [this, id = one.id, by = QPointer<const JetKvmDevice>(jet)](const QUrl& from, const QUrl& /*at*/,
                                                                        const QByteArray& shown) {
                later([this, id, by, from, shown] {
                    Held* one = held(id);
                    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& kvm) { return kvm.id == id; });
                    if (by == nullptr || one == nullptr || one->device.get() != by.data() || known == kvms().end() ||
                        known->address != from.toString(QUrl::FullyEncoded) || !one->asked) {
                        return;
                    }
                    // (The one judgement of such a look: held back where it is the certificate another is known by.)
                    KvmProbe::Found seen;
                    seen.shown = shown;
                    static_cast<void>(judgeItsHttps(id, from, seen));
                });
            });
    }
    // A controller's driver took the server that answered for its own: noted with the KVM, a turn of the loop later
    // (the driver is in the middle of its look, and what is kept is written to a file).
    if (const auto* server = qobject_cast<const RedfishDevice*>(one.device.get())) {
        connect(
            server, &RedfishDevice::knownAs, this,
            [this, id = one.id, by = QPointer<const RedfishDevice>(server)](const QString& before, const QString& now) {
                later([this, id, by, before, now] { knowsItsServer(id, by.data(), before, now); });
            });
    }
}

void LocalWall::knowsItsServer(const QString& id, const OwnDevice* by, const QString& before, const QString& now)
{
    Held* one = held(id);
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& kvm) { return kvm.id == id; });
    // Of the KVM that driver was made for, as it is now: a driver is made anew with every change of where it is, who
    // signs in, what it is known by and its password; and the mark is the one it had.
    if (by == nullptr || one == nullptr || one->device.get() != by || known == kvms().end() ||
        known->machine != before) {
        return;
    }
    QList<OwnKvm> asTheyAre = kvms();
    asTheyAre[known - kvms().begin()].machine = now;
    noteByItself(asTheyAre, m_settings.remembered().kvmCertificates);
}

void LocalWall::letGoOfEveryDriver()
{
    m_ended = true;
    m_tidy.stop();
    for (Held& one : m_held) {
        letGo(one, stopped());
    }
    m_held.clear();
}

JetKvmDevice::TwinCheck LocalWall::twinCheckFor(const QString& id)
{
    return [wall = QPointer<LocalWall>(this), id](const QUrl& from,
                                                  const KvmProbe::Found& found) -> std::optional<base::Error> {
        if (wall == nullptr) {
            return base::Error{"stopped", "The wall is gone: nothing is sent."};
        }
        return wall->judgeItsHttps(id, from, found);
    };
}

std::optional<OwnKvm> LocalWall::twinBehind(const QString& id, const KvmProbe::Found& found) const
{
    // What showed the certificate another KVM of the wall is known by: that one, whatever it answered.
    if (std::optional<OwnKvm> twin = anotherKnownBy(id, found.shown)) {
        return twin;
    }
    // Held back already: let go of only where a look showed a certificate no other KVM of the wall is known by, or
    // nothing listens there. In doubt (something listens, and showed nothing), it stays held, as kvm::wayOf takes what
    // listens for something (from the second reading of the twins, MEDIUM), for as long as the one it is held for is
    // on the wall.
    const auto one = std::ranges::find_if(m_held, [&id](const Held& held) { return held.id == id; });
    if (!id.isEmpty() && one != m_held.end() && one->heldBackByTwin && found.shown.isEmpty() && !found.nothingListens) {
        return heldFor(id);
    }
    return std::nullopt;
}

std::optional<base::Error> LocalWall::judgeItsHttps(const QString& id, const QUrl& from, const KvmProbe::Found& found)
{
    const std::optional<OwnKvm> twin = twinBehind(id, found);
    if (!twin) {
        return std::nullopt;
    }
    const bool shown = !found.shown.isEmpty();
    // Seen at its own HTTPS, at the address it has on the wall: held back for that one from now on, on the wall and
    // for the next start (from the outside review, F1: a box that saw it, and looked once more before it signed in and
    // found nothing there that time, forgot what it had seen and sent the password the plain way).
    Held* one = id.isEmpty() ? nullptr : held(id);
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& kvm) { return kvm.id == id; });
    if (shown && one != nullptr && known != kvms().end() && known->kind == "jetkvm"_L1 &&
        known->address.startsWith("http://"_L1) && known->address == from.toString(QUrl::FullyEncoded)) {
        heldBackOnALook(*one, *twin);
    }
    return shown ? sameDevice(*twin) : heldBackFor(*twin);
}

void LocalWall::heldBackOnALook(Held& one, const OwnKvm& twin)
{
    if (const std::optional<OwnKvm> already = heldFor(one.id);
        one.heldBackByTwin && already && already->id == twin.id) {
        return;
    }
    if (!one.asked) {
        // (Its driver has no password: nothing of it to let go of.)
        heldBackForItsTwin(one, twin);
        return;
    }
    // Noted at once: whatever signs in to it from now on is held to that (judgeItsHttps). Its driver, which may be the
    // very one whose look this is, is let go of a turn of the loop later, and one with no password made.
    one.heldBackByTwin = true;
    markHeldFor(one.id, twin.id);
    later([this, id = one.id, twinId = twin.id] {
        Held* still = held(id);
        const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& kvm) { return kvm.id == id; });
        const std::optional<OwnKvm> heldForTwin = heldFor(id);
        if (still == nullptr || known == kvms().end() || !still->asked || !still->heldBackByTwin || !heldForTwin ||
            heldForTwin->id != twinId) {
            return;
        }
        heldBackFromItsDriver(*still, *known, *heldForTwin);
    });
}

void LocalWall::heldBackFromItsDriver(Held& one, const OwnKvm& known, const OwnKvm& twin)
{
    // (Whoever its driver still owed an answer is told why none comes: the other one is at this address.)
    const base::Error why = heldBackFor(twin);
    letGo(one, why);
    one.device = driverFor(one, known, QString());
    listenTo(one);
    one.asking = 0;
    one.afterAsking.clear();
    heldBackForItsTwin(one, twin);
}

std::optional<OwnKvm> LocalWall::heldFor(const QString& id) const
{
    const QString twin = m_settings.remembered().kvmsHeldForATwin.value(id);
    if (twin.isEmpty() || twin == id) {
        return std::nullopt;
    }
    const auto there = std::ranges::find_if(kvms(), [&twin](const OwnKvm& other) { return other.id == twin; });
    return there == kvms().end() ? std::nullopt : std::optional<OwnKvm>(*there);
}

void LocalWall::markHeldFor(const QString& id, const QString& twin)
{
    Remembered with = m_settings.remembered();
    if (with.kvmsHeldForATwin.value(id) == twin) {
        return;
    }
    if (twin.isEmpty()) {
        with.kvmsHeldForATwin.remove(id);
    } else {
        with.kvmsHeldForATwin.insert(id, twin);
    }
    m_settings.note(with);
    // (Written soon, as what the wall notes by itself: one writing for as long as it stays so, not one per asking.)
    noteByItself(with.kvms, with.kvmCertificates);
}

std::optional<OwnKvm> LocalWall::anotherKnownBy(const QString& id, const QByteArray& shown) const
{
    if (shown.isEmpty()) {
        return std::nullopt;
    }
    OwnKvm showing;
    showing.knownByCertificate = true;
    showing.certificate = shown;
    const auto twin = std::ranges::find_if(
        kvms(), [&id, &showing](const OwnKvm& other) { return other.id != id && sameCertificate(other, showing); });
    return twin == kvms().end() ? std::nullopt : std::optional<OwnKvm>(*twin);
}

base::Result<bool> LocalWall::followOntoHttps(const QString& id, const QUrl& from, const QUrl& at,
                                              const KvmProbe::Found& found)
{
    Held* one = held(id);
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& kvm) { return kvm.id == id; });
    // Still where its driver spoke to it, and asked what it is: else nothing (moved meanwhile, or taken off).
    if (one == nullptr || known == kvms().end() || known->address != from.toString(QUrl::FullyEncoded) || !one->asked) {
        return false;
    }
    // Onto HTTPS at the same host, a JetKVM's answer there, and nowhere else (ontoHttps); and the HTTPS that
    // was offered, which is that of where it is now (an offer waits a turn of the loop: it may be of where it
    // was). The rest of the rules, the place and its twins among them, are alter's, with that one exception
    // for the password.
    const std::optional<OwnKvm> onto = ontoHttps(*known, found, m_paces.jetkvm.otherWay);
    if (!onto || onto->address != at.toString(QUrl::FullyEncoded)) {
        return false;
    }
    // What shows the certificate another KVM of the wall is known by is that one (it has the key to it): not
    // followed there, and this one may be reached at that one's place (an address handed out anew): said so. Also
    // where Windows trusts that certificate there, and this one would be known there by Windows' word (from the third
    // reading of the twins, L5).
    if (const std::optional<OwnKvm> twin = anotherKnownBy(id, found.shown)) {
        return sameDevice(*twin);
    }
    // Said in its details until whoever sits here has seen it, also after a start: noted here, and written
    // with the move itself (which writes all that is remembered), or not at all.
    Remembered marked = m_settings.remembered();
    marked.kvmsFollowedOntoHttps.insert(id, onto->address);
    m_settings.note(marked);
    if (!alter(id, *onto, std::nullopt, "Following this device onto HTTPS", false, found)) {
        Remembered unmarked = m_settings.remembered();
        unmarked.kvmsFollowedOntoHttps.remove(id);
        m_settings.note(unmarked);
        return false;
    }
    return true;
}

void LocalWall::heldBackForItsTwin(Held& one, const OwnKvm& twin)
{
    one.asked = false;
    one.heldBackByTwin = true;
    one.waitsToBeAsked = false;
    // (Kept with it, and which one it is held for, for the next start: from the third reading of the twins, L2.)
    markHeldFor(one.id, twin.id);
    base::Error why = heldBackFor(twin);
    if (saysAnother(one.saidWhenAsked, why)) {
        noteThatOneCameToSomething();
    }
    one.saidWhenAsked = std::move(why);
    ++one.askedInVain;
    const qint64 rest = qint64{m_paces.kvmd.holdMs} << std::min(one.askedInVain - 1, 10);
    one.askAgainAt = m_clock.elapsed() + std::min<qint64>(m_paces.kvmd.longestHoldMs, rest);
}

void LocalWall::noteThatOneCameToSomething()
{
    // Said a short while after the first, for all that have come to something by then. (And
    // never from inside whatever brought it about: a driver is in the middle of ending a look,
    // or an asking has only just been taken in.)
    if (!m_saySoon.isActive()) {
        m_saySoon.start(std::max(0, m_own.saidAfterMs));
    }
}

void LocalWall::keepCertificate(const QString& id, const QByteArray& der)
{
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& one) { return one.id == id; });
    if (known == kvms().end() || !known->knownByCertificate || !known->address.startsWith("https://"_L1) ||
        m_settings.remembered().kvmCertificates.contains(id)) {
        return;
    }
    // Never another than the one the KVM is known by, as it is known now: what a driver was
    // shown a moment ago may be the certificate of the KVM as it was. And only what a file of
    // the wall would hold: one that is too long for a file, or none to Windows, is not kept,
    // and whoever writes the wall to a file is told that only its fingerprint is known.
    const std::optional<exchange::KeptCertificate> kept = exchange::certificateOf(der);
    if (!kept || kept->sha256 != known->certificate) {
        return;
    }
    const QList<OwnKvm> asTheyAre = kvms();
    QHash<QString, QString> certificates = m_settings.remembered().kvmCertificates;
    certificates.insert(id, kept->pem);
    // Noted, and written down with what else the wall notes by itself. (Where the program ends
    // before that, or it cannot be written, the first line of the KVM's next driver brings the
    // certificate again.)
    noteByItself(asTheyAre, certificates);
}

template <typename Answer>
std::function<void(base::Result<Answer>)> LocalWall::owedBy(Held& one, std::function<void(base::Result<Answer>)> done)
{
    // Under a number that no other asking has, of whichever driver: a driver that takes the
    // place of this one owes nothing of what this one was asked.
    const quint64 number = ++m_asked;
    const auto tell = std::make_shared<std::function<void(base::Result<Answer>)>>(std::move(done));
    one.owed.insert(number, [tell](const base::Error& why) { (*tell)(why); });
    // (A driver that was let go is there for a turn of the loop longer, and so may be longer
    // than the wall.)
    return [wall = QPointer<LocalWall>(this), id = one.id, number, tell](base::Result<Answer> answer) {
        Held* still = wall != nullptr ? wall->held(id) : nullptr;
        if (still != nullptr && still->owed.remove(number)) {
            (*tell)(std::move(answer));
        }
    };
}

void LocalWall::letGo(Held& one, const base::Error& why)
{
    // Its driver a turn of the loop later: something of it may be in the middle of its work, and
    // an answer of it may be on its way already. What it was asked is owed by nobody from here
    // on, so that answer is nobody's; and those who asked are told by the wall, later, as
    // everything is answered.
    const auto owed = std::exchange(one.owed, {});
    one.device.release()->deleteLater();
    for (const auto& tell : owed) {
        later([tell = std::function<void(const base::Error&)>(tell), why = base::Error(why)] { tell(why); });
    }
    // Whoever has the KVM live is told the same, a turn later as those above. Keyboard and
    // mouse are let go here and now, while their line can still say so.
    if (one.live != nullptr) {
        one.live->end(why);
        one.live.release()->deleteLater();
    }
}

base::Result<void> LocalWall::remember(const QList<OwnKvm>& kvms, const char* what)
{
    const QHash<QString, QString> certificates = m_settings.remembered().kvmCertificates;
    const QSet<QString> marks = m_settings.remembered().kvmPasswordsKept;
    return remember(kvms, certificates, marks, what);
}

base::Result<void> LocalWall::remember(const QList<OwnKvm>& kvms, const QHash<QString, QString>& certificates,
                                       const QSet<QString>& marks, const char* what)
{
    const Remembered before = m_settings.remembered();
    Remembered after = before;
    after.kvms = kvms;
    after.kvmCertificates = certificates;
    after.kvmPasswordsKept = marks;
    // A hold for a twin goes with the KVM it was of (taken off, or no longer a JetKVM over plain HTTP: moved onto
    // HTTPS), and with the one it was held for (taken off: no KVM of the wall is known by that certificate any more,
    // and a look anew would find no twin either). As the file keeps it (settings.cpp).
    const auto ofTheList = [&kvms](const QString& id) -> const OwnKvm* {
        const auto there = std::ranges::find_if(kvms, [&id](const OwnKvm& kvm) { return kvm.id == id; });
        return there == kvms.end() ? nullptr : &*there;
    };
    after.kvmsHeldForATwin.removeIf([&ofTheList](const QHash<QString, QString>::iterator& held) {
        const OwnKvm* kvm = ofTheList(held.key());
        if (kvm == nullptr) {
            return true;
        }
        return kvm->kind != "jetkvm"_L1 || !kvm->address.startsWith("http://"_L1) || held.value() == held.key() ||
               ofTheList(held.value()) == nullptr;
    });
    if (const base::Result<void> written = m_settings.remember(after); !written) {
        // What is remembered changes even when the writing fails: so it is changed back, and
        // the wall is what the file still says.
        static_cast<void>(m_settings.remember(before));
        return notSaved(what, written.error());
    }
    return {};
}

void LocalWall::askWall(std::function<void(base::Result<Wall>)> done)
{
    Wall wall;
    wall.everySeconds = m_settings.remembered().wallPictureEveryMs > 0
                            ? m_settings.remembered().wallPictureEveryMs / 1000.0
                            : everySeconds;
    for (const OwnKvm& kvm : kvms()) {
        Machine machine;
        machine.id = kvm.id;
        machine.host = hostOf(kvm);
        machine.name = kvm.name.isEmpty() ? machine.host : kvm.name;
        machine.kind = kvm.kind;
        // (A server's controller hands out no picture of its screen here: its card shows how it is. Its screen comes
        // in M7 part 3d.)
        machine.hasPicture = kvm.kind != "redfish"_L1;
        machine.group = kvm.group;
        machine.page = QUrl(kvm.address);
        machine.plain = machine.page.scheme() == "http"_L1;
        machine.own = true;
        machine.noSignalMinutes = kvm.noSignalMinutes;
        machine.unchangedMinutes = kvm.unchangedMinutes;
        if (followedOntoHttps(kvm.id)) {
            machine.note =
                kvm.knownByCertificate
                    ? words("Over HTTPS now, known by its certificate %1").arg(kvm::fingerprint(kvm.certificate))
                    : words("Over HTTPS now, on Windows' word for its certificate");
        }
        wall.machines.append(machine);
    }
    // Each of them can be had live (openLive), which a wall says by the form it speaks: the one
    // this program has.
    wall.liveForm = spokenLiveForm;
    later([done = std::move(done), wall = std::move(wall)] { done(wall); });
}

void LocalWall::askStatus(std::function<void(base::Result<QHash<QString, Status>>)> done)
{
    // A KVM whose state is no longer fresh is looked at, and the answer waits for those looks.
    // (Answered at once, an asking told what the look before it had found: a new KVM was
    // "connecting" until the wall asked again, seconds later, and every change came that late.)
    // For a moment and no longer: a KVM that is slow or away is told as it was last known, and
    // nobody waits for it beyond that.
    struct Waiting {
        std::function<void(base::Result<QHash<QString, Status>>)> done;
        int looks = 0;
        bool over = false;
    };
    const auto waiting = std::make_shared<Waiting>();
    waiting->done = std::move(done);
    // What is known of each by then, of the KVMs that are the wall's by then. Once: whichever
    // comes first, the last of the looks or the limit. (A driver that was let go is there for a
    // turn of the loop longer than its wall may be.)
    const auto answer = [wall = QPointer<LocalWall>(this), waiting] {
        if (wall == nullptr || std::exchange(waiting->over, true)) {
            return;
        }
        QHash<QString, Status> all;
        for (const Held& one : wall->m_held) {
            all.insert(one.id, one.asked ? one.device->status() : wall->howItIsUnasked(one));
        }
        waiting->done(all);
    };
    // Those that were not asked yet what they are, are asked that first, with nothing that says
    // who asks, and their drivers not at all: a few at a time, and the others when their turn
    // comes (askThoseThatWait).
    for (Held& one : m_held) {
        one.waitsToBeAsked = !one.asked && one.asking == 0;
    }
    askThoseThatWait();
    for (Held& one : m_held) {
        if (!one.asked) {
            // The answer waits for an asking that is under way and holds a place, as it waits
            // for a look: such a one may well end within the moment. And where the KVM has
            // answered as one, for the first look of the driver it has from then on, which has
            // its password. Not for one whose asking is not due yet, or waits its turn, or has
            // gone on without its place (what is off does, for seconds): that one is told as
            // it was last known, and the wall says by itself when it has come to something.
            if (one.asking == 0 || !m_inAPlace.contains(one.asking)) {
                continue;
            }
            ++waiting->looks;
            one.afterAsking.append([wall = QPointer<LocalWall>(this), id = one.id, waiting, answer,
                                    patience = m_paces.kvmd.lookPatienceMs] {
                Held* now = wall != nullptr ? wall->held(id) : nullptr;
                const auto over = [waiting, answer](bool /*stillLooking*/) {
                    if (--waiting->looks == 0) {
                        answer();
                    }
                };
                if (now == nullptr || !now->asked) {
                    over(false);
                    return;
                }
                now->device->look();
                now->device->afterLook(patience, over);
            });
            continue;
        }
        if (!one.device->isStale()) {
            continue;
        }
        one.device->look();
        ++waiting->looks;
        one.device->afterLook(m_paces.kvmd.lookPatienceMs, [waiting, answer](bool /*stillLooking*/) {
            if (--waiting->looks == 0) {
                answer();
            }
        });
    }
    if (waiting->looks == 0) {
        later(answer);
        return;
    }
    // The limit is kept here too: a driver that is let go meanwhile says nothing of its look.
    QTimer::singleShot(m_paces.kvmd.lookPatienceMs, this, answer);
}

void LocalWall::askSnapshot(const QString& machine, int width, double /*mayBeOldSeconds*/,
                            std::function<void(base::Result<Snapshot>)> done)
{
    // (How old a picture may be is for a place that many look at through one program. Here one
    // wall asks, at its own pace.)
    if (Held* one = held(machine)) {
        if (!one->asked) {
            later([done = std::move(done), why = whyNothingYet(*one)] { done(why); });
            return;
        }
        // (And shown to its screen watches, as it is handed out.)
        one->device->askSnapshot(
            width, owedBy<Snapshot>(*one, [wall = QPointer<LocalWall>(this), machine,
                                           done = std::move(done)](const base::Result<Snapshot>& picture) {
                if (picture && wall != nullptr) {
                    wall->m_screens.saw(machine, picture.value().picture, picture.value().signal);
                }
                done(picture);
            }));
        return;
    }
    later([done = std::move(done)] { done(unknown()); });
}

QList<ScreenWatch::Watched> LocalWall::watchedScreens() const
{
    QList<ScreenWatch::Watched> watched;
    for (const OwnKvm& kvm : kvms()) {
        // (A server's controller hands out no picture of a KVM's here: its card shows how it is.)
        if ((kvm.noSignalMinutes <= 0 && kvm.unchangedMinutes <= 0) || kvm.kind == "redfish"_L1) {
            continue;
        }
        const auto one = std::ranges::find_if(m_held, [&kvm](const Held& held) { return held.id == kvm.id; });
        QString state;
        if (one != m_held.end() && one->asked && one->device != nullptr) {
            state = one->device->status().state;
        }
        watched.append({kvm.id, state, kvm.noSignalMinutes, kvm.unchangedMinutes});
    }
    return watched;
}

void LocalWall::act(const QString& machine, const Action& what, std::function<void(base::Result<Done>)> done)
{
    Held* one = held(machine);
    if (one == nullptr) {
        later([done = std::move(done)] { done(unknown()); });
        return;
    }
    if (!one->asked) {
        // Nothing is done to what was never asked what it is: nothing is sent to it at all.
        later([done = std::move(done)] { done(notAskedYet()); });
        return;
    }
    // A saved action of this wall: the action as it is now, where that is still the state
    // whoever runs it looked at. Nothing of the store is read for that.
    std::optional<OwnAction> saved;
    if (what.kind == "custom"_L1) {
        base::Result<OwnAction> asItIs = m_actions.asLookedAt(what.savedId, what.savedChanged);
        if (!asItIs) {
            later([done = std::move(done), refusal = asItIs.error()] { done(refusal); });
            return;
        }
        saved = std::move(asItIs).value();
    }
    if (one->busy) {
        const auto kvm = std::ranges::find_if(kvms(), [&machine](const OwnKvm& known) { return known.id == machine; });
        const QString called = kvm == kvms().end() ? words("this device") : calledInASentence(*kvm);
        later([done = std::move(done), refusal = busyWith(called)] { done(refusal); });
        return;
    }
    // What a saved action is to the KVM's driver: its keys pressed, or its text typed. The text
    // is read from the store here, for this run and only now that the run goes out; and what
    // was made of it here is written over when this function is left, by when the driver has
    // taken what it sends.
    std::optional<OwnActions::ForOneRun> asSaved;
    if (saved) {
        base::Result<OwnActions::ForOneRun> made = m_actions.forOneRun(*saved);
        if (!made) {
            later([done = std::move(done), refusal = made.error()] { done(refusal); });
            return;
        }
        asSaved.emplace(std::move(made).value());
    }
    one->busy = true;
    // Answered once, whatever replaces its driver or lets it go meanwhile (a KVM held back for its twin, the
    // program's end): owed by that driver, so the wall tells why where the driver is gone first, and what the
    // driver says after that reaches nobody. And the KVM is busy no longer than that (from the reading of 2g/2h,
    // M2: a "Show here" that its driver's going never answered left the KVM busy until the program ended).
    one->device->act(asSaved ? asSaved->action() : what,
                     owedBy<Done>(*one, [wall = QPointer<LocalWall>(this), machine,
                                         done = std::move(done)](base::Result<Done> result) {
                         // (Looked up anew: the list may have changed while the KVM was at it.)
                         if (Held* still = wall != nullptr ? wall->held(machine) : nullptr) {
                             still->busy = false;
                         }
                         done(std::move(result));
                     }));
}

bool LocalWall::mayHaveBeenDone(const QString& machine, const base::Error& error) const
{
    // Its family's word. Of a machine that is not on the wall (taken off meanwhile, or never there)
    // the family is not known: then the word of every family the program speaks to, yes where any
    // of them says yes (a KVM is not taken off while something is done to it, so what such a one
    // is told is a refusal from before anything went out, in the wall's own words, which no family
    // calls "may have been done").
    const auto known = std::ranges::find_if(kvms(), [&machine](const OwnKvm& one) { return one.id == machine; });
    if (known == kvms().end()) {
        return KvmdDevice::mayHaveBeenDone(error) || JetKvmDevice::mayHaveBeenDone(error) ||
               RedfishDevice::mayHaveBeenDone(error);
    }
    if (known->kind == "redfish"_L1) {
        return RedfishDevice::mayHaveBeenDone(error);
    }
    return known->kind == "jetkvm"_L1 ? JetKvmDevice::mayHaveBeenDone(error) : KvmdDevice::mayHaveBeenDone(error);
}

void LocalWall::askSavedActions(std::function<void(base::Result<QList<SavedAction>>)> done)
{
    // (As they are when the answer is handed over: a change that came in between is in it.)
    later([this, done = std::move(done)] { done(m_actions.handedOut()); });
}

void LocalWall::askWakeTargets(const QString& machine, std::function<void(base::Result<QList<WakeTarget>>)> done)
{
    if (Held* one = held(machine)) {
        if (!one->asked) {
            later([done = std::move(done)] { done(notAskedYet()); });
            return;
        }
        one->device->askWakeTargets(owedBy<QList<WakeTarget>>(*one, std::move(done)));
        return;
    }
    later([done = std::move(done)] { done(unknown()); });
}

void LocalWall::askConsole(const QString& /*machine*/, std::function<void(base::Result<QUrl>)> done)
{
    // (Whichever KVM it is: no driver of this PC's has such a pass made out.)
    later([done = std::move(done)] {
        done(base::Error{"no_console", "This device does not hand out a way into its console"});
    });
}

void LocalWall::arrange(const QStringList& machines, std::function<void(base::Result<QStringList>)> done)
{
    // Those named first, as named and each once; the rest after them as they stood; a name that
    // is nobody's is passed over.
    QList<OwnKvm> ordered;
    QSet<QString> placed;
    for (const QString& id : machines) {
        const auto kvm = std::ranges::find_if(kvms(), [&id](const OwnKvm& known) { return known.id == id; });
        if (kvm != kvms().end() && !placed.contains(id)) {
            placed.insert(id);
            ordered.append(*kvm);
        }
    }
    for (const OwnKvm& kvm : kvms()) {
        if (!placed.contains(kvm.id)) {
            ordered.append(kvm);
        }
    }
    if (const base::Result<void> kept = remember(ordered, "The order"); !kept) {
        later([done = std::move(done), error = kept.error()] { done(error); });
        return;
    }
    QStringList order;
    for (const OwnKvm& kvm : std::as_const(ordered)) {
        order.append(kvm.id);
    }
    emit changed();
    later([done = std::move(done), order = std::move(order)] { done(order); });
}

void LocalWall::openLive(const QString& machine, Live::Need need,
                         std::function<void(base::Result<std::unique_ptr<Live>>)> done)
{
    // Handed over a turn later, as everything is answered, from the KVM as it is on the wall
    // then: one that was taken off meanwhile is not there, and one whose driver was replaced
    // is had live through the new one.
    later([this, machine, need, done = std::move(done)] {
        Held* one = held(machine);
        if (one == nullptr) {
            done(unknown());
            return;
        }
        if (!one->asked) {
            done(whyNothingYet(*one));
            return;
        }
        // One for each driver, made when it is first asked for, and shared by all who ask.
        if (one->live == nullptr) {
            one->live = one->device->makeLive();
        }
        done(one->live->open(need));
    });
}

base::Result<QString> LocalWall::add(OwnKvm kvm, const QString& password)
{
    if (m_ended) {
        return stopped();
    }
    if (kvms().size() + m_settings.remembered().kvmsItDoesNotRead.size() >= Remembered::mostKvms) {
        return base::Error{"too_many", "This wall is full."};
    }
    // (Only of a family this build speaks to: one of another would be written and not read back.)
    if (!OwnKvm::namesAFamily(kvm.kind)) {
        return base::Error{"invalid", "This program does not speak to that family of device itself."};
    }
    if (const std::optional<base::Error> refusal = notAnAddress(kvm.address)) {
        return *refusal;
    }
    const bool there = std::ranges::any_of(kvms(), [&kvm](const OwnKvm& known) { return atOnePlace(known, kvm); });
    if (there) {
        return onTheWall();
    }
    if (keptAt(kvm)) {
        return keptThere();
    }
    // What its password is saved for. None is kept for a KVM that is to be known by what is no
    // certificate: it could never be used.
    const std::optional<QByteArray> lines = savedFor(kvm);
    if (!lines) {
        return certificateNotRead();
    }
    kvm.id = newId(kvms(), m_settings.remembered().kvmsItDoesNotRead);
    // Whoever adds a KVM here has asked what answers at its address, and signed in once: it is
    // one that was asked, in this run. That is kept for the next only of a KVM over HTTPS.
    kvm.askedWhatItIs = keepsThatItWasAsked(kvm);
    // The password first: a KVM on the wall without one is a KVM that cannot be signed in to. (What
    // it is kept as is written over when this is left: KeptAs.)
    const KeptAs secret(*lines, password);
    if (const base::Result<void> kept = m_secrets.keep(secretOf(kvm.id), secret.bytes()); !kept) {
        return notSaved("The new device", kept.error());
    }
    QList<OwnKvm> with = kvms();
    with.append(kvm);
    // And beside it the mark that a password is kept for it, in the same writing: one that was
    // given empty is none.
    const QHash<QString, QString> certificates = m_settings.remembered().kvmCertificates;
    QSet<QString> marks = m_settings.remembered().kvmPasswordsKept;
    if (password.isEmpty()) {
        marks.remove(kvm.id);
    } else {
        marks.insert(kvm.id);
    }
    if (const base::Result<void> kept = remember(with, certificates, marks, "The new device"); !kept) {
        m_secrets.forget(secretOf(kvm.id));
        return kept.error();
    }
    hold(kvm, true, password);
    emit changed();
    return kvm.id;
}

bool LocalWall::atOnePlace(const OwnKvm& one, const OwnKvm& other)
{
    return atOnePlace(placeOf(one), placeOf(other));
}

QList<OwnKvm> LocalWall::keptAside() const
{
    QList<OwnKvm> all;
    for (const auto& kept : m_settings.remembered().kvmsItDoesNotRead) {
        OwnKvm kvm;
        kvm.id = kept.toObject()["id"_L1].toString();
        kvm.address = kept.toObject()["address"_L1].toString();
        kvm.name = words("a KVM that another version of qKVM added, which this one does not show");
        all.append(kvm);
    }
    return all;
}

bool LocalWall::keptAt(const OwnKvm& kvm) const
{
    return std::ranges::any_of(keptAside(), [&kvm](const OwnKvm& kept) { return atOnePlace(kept, kvm); });
}

LocalWall::Place LocalWall::placeOf(const OwnKvm& kvm)
{
    const QUrl address(kvm.address);
    return Place{address.host(), address.port(), address.port(address.scheme() == "http"_L1 ? 80 : 443)};
}

bool LocalWall::atOnePlace(const Place& one, const Place& other)
{
    if (one.host != other.host) {
        return false;
    }
    // Neither with a port of its own: the same KVM over HTTPS and in the clear. Otherwise by
    // the port that is spoken to, written or not ("http://kvm:443" is where "https://kvm" is).
    return (one.written < 0 && other.written < 0) || one.spokenTo == other.spokenTo;
}

std::optional<base::Error> LocalWall::intoTheClear(const OwnKvm& from, const OwnKvm& to)
{
    const bool lessGuarded = QUrl(from.address).scheme() == "https"_L1 && QUrl(to.address).scheme() == "http"_L1;
    return lessGuarded && atOnePlace(from, to) ? std::optional<base::Error>(notInTheClear()) : std::nullopt;
}

bool LocalWall::sameCertificate(const OwnKvm& one, const OwnKvm& other)
{
    // (Never a server's controller, whichever of the two: its maker may have put one certificate on every board.)
    return one.knownByCertificate && other.knownByCertificate && one.certificate == other.certificate &&
           one.kind != "redfish"_L1 && other.kind != "redfish"_L1;
}

std::optional<OwnKvm> LocalWall::twinOf(const QList<OwnKvm>& there, const OwnKvm& kvm)
{
    // What shows the certificate another is known by first: that is proven (it has the key to it).
    const auto proven =
        std::ranges::find_if(there, [&kvm](const OwnKvm& known) { return sameCertificate(known, kvm); });
    if (proven != there.end()) {
        return *proven;
    }
    // Then the number it gives for itself, which is said: of two that give none, neither is the other; and
    // neither is one that somebody said is to stay beside its twin (the web's keep_both), nor that twin of it.
    if (kvm.number.isEmpty() || isKeptBeside(kvm)) {
        return std::nullopt;
    }
    const auto said = std::ranges::find_if(
        there, [&kvm](const OwnKvm& known) { return !isKeptBeside(known) && known.number == kvm.number; });
    return said == there.end() ? std::nullopt : std::optional<OwnKvm>(*said);
}

bool LocalWall::isKeptBeside(const OwnKvm& kvm)
{
    return kvm.keptBesideItsTwin && kvm.kind != "redfish"_L1;
}

bool LocalWall::saysItIsTheSame(const base::Error& refusal)
{
    // (sameDevice's own sentence, built in that one place: its `saidSo`.)
    const QString said = QString::fromStdString(refusal.message);
    return refusal.code == "same_device" && said.startsWith(u"This says it is the same device as "_s) &&
           !said.endsWith(oneCardOfAServer);
}

base::Error LocalWall::sameDevice(const OwnKvm& twin, bool saidSo)
{
    // The server's sentence: what it is called, and where it is behind that unless it is called
    // by where it is. (The name set off: it may come from a file, and stood here as it was once.)
    // Known again by a number it gives, it "says it is" (the web's same_device, said_so).
    const QString host = hostOf(twin);
    const QString called = calledInASentence(twin);
    QString sentence = (saidSo ? u"This says it is the same device as %1, which is on the wall already"_s
                               : u"This is the same device as %1, which is on the wall already"_s)
                           .arg(called);
    if (called != host) {
        sentence += u" ("_s + host + u')';
    }
    // (A server's second card is not kept beside it either: nothing to answer, and so said.)
    if (twin.kind == "redfish"_L1) {
        sentence += u"."_s + oneCardOfAServer;
    }
    return {"same_device", sentence.toStdString()};
}

base::Error LocalWall::heldBackFor(const OwnKvm& twin)
{
    // Why (the other one may be what answers here), and the deliberate acts there are for it, in plain words: where it
    // is that device, one of the two goes; where it is another (a port 443 that is forwarded elsewhere, an HTTPS that
    // hangs), it is taken off and added again (the Add box looks anew), or moved onto its own HTTPS. No act sends a
    // password in the clear beside that doubt (from the third reading of the twins).
    const QString host = hostOf(twin);
    const QString called = calledInASentence(twin);
    const QString there = called != host ? u" ("_s + host + u')' : QString();
    return {"same_device",
            words("This may be the same device as %1, which is on the wall already%2: its HTTPS showed the certificate "
                  "that one is known by, and no password is sent here while that lasts. If it is that device, one of "
                  "the two can go: remove this one or that one. If it is another device, remove this one and add it "
                  "again, or turn on HTTPS on the JetKVM and change its address here to https://.")
                .arg(called, there)
                .toStdString()};
}

QString LocalWall::calledInASentence(const OwnKvm& kvm)
{
    // By where it is, where it has no name, or none to show (one of nothing to see, one that is
    // no line), or is named by where it is: the box that adds a KVM names it so where nothing
    // was typed and the KVM calls itself nothing, and "the same device as "kvm.lan" (kvm.lan)"
    // says one thing twice.
    const QString host = hostOf(kvm);
    const QString shown = exchange::nameSetOff(kvm.name);
    return shown.isEmpty() || kvm.name == host ? host : shown;
}

bool LocalWall::isStillTheOne(const OwnKvm& now, const OwnKvm& seen)
{
    OwnKvm asSeen = now;
    asSeen.askedWhatItIs = seen.askedWhatItIs;
    asSeen.machine = seen.machine;
    return asSeen == seen;
}

quint64 LocalWall::secretWrites(const QString& id) const
{
    const auto found = std::ranges::find_if(m_held, [&id](const Held& one) { return one.id == id; });
    return found == m_held.end() ? 0 : found->secretWrites;
}

bool LocalWall::marksAPasswordFor(const QString& id) const
{
    // (The marks are those of the KVMs of the list, and of no other: each goes with its KVM.)
    return m_settings.remembered().kvmPasswordsKept.contains(id);
}

QSet<QString> LocalWall::foundWithoutAPassword() const
{
    QSet<QString> found;
    for (const Held& one : m_held) {
        // (What its driver said when it last looked, as its card says it. One that was not asked
        // yet what it is has a driver that looks at nothing.)
        if (one.asked && one.device->status().errorCode == "no_password"_L1) {
            found.insert(one.id);
        }
    }
    return found;
}

base::Result<void> LocalWall::remove(QString id)
{
    const auto kvm = std::ranges::find_if(kvms(), [&id](const OwnKvm& known) { return known.id == id; });
    Held* one = held(id);
    if (kvm == kvms().end() || one == nullptr) {
        return unknown();
    }
    if (one->busy) {
        // What is under way to it is not cut off, and not left without anybody to hear how it ended.
        return busyWith(calledInASentence(*kvm));
    }
    const QString name = calledInASentence(*kvm);
    const QUrl address(kvm->address);
    QList<OwnKvm> without = kvms();
    without.removeIf([&id](const OwnKvm& known) { return known.id == id; });
    // (And the certificate itself that was kept beside it goes with it, and so does the mark that
    // a password is kept for it: in the same writing, before the password is forgotten.)
    QHash<QString, QString> certificates = m_settings.remembered().kvmCertificates;
    certificates.remove(id);
    QSet<QString> marks = m_settings.remembered().kvmPasswordsKept;
    marks.remove(id);
    if (const base::Result<void> kept =
            remember(without, certificates, marks, ("Removing " + name.toStdString()).c_str());
        !kept) {
        return kept.error();
    }
    m_secrets.forget(secretOf(id));
    // And what was kept of its sign-ins (its wrong-password counts and their waits, in memory and on disk: a9), but a
    // sign-in to it that is out still, which its line's end settles.
    forgetSignInsAt(address);
    const auto gone = std::ranges::find_if(m_held, [&id](const Held& one) { return one.id == id; });
    letGo(*gone, unknown());
    m_held.erase(gone);
    // (And what its screen watches knew of it, and its picture.)
    m_screens.forget(id);
    emit changed();
    return {};
}

void LocalWall::forgetSignInsAt(const QUrl& address)
{
    SignInPacing& kvmds = m_paces.kvmd.pacing ? *m_paces.kvmd.pacing : SignInPacing::program();
    SignInPacing& jetKvms = m_paces.jetkvm.pacing ? *m_paces.jetkvm.pacing : SignInPacing::program();
    kvmds.forgetPlace(address);
    if (&jetKvms != &kvmds) {
        jetKvms.forgetPlace(address);
    }
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): its own copy of the id, as the header says
base::Result<void> LocalWall::change(QString id, OwnKvm kvm, const std::optional<QString>& password,
                                     const std::optional<KvmProbe::Found>& overHttps)
{
    return alter(id, std::move(kvm), password, "The change to this device", false, overHttps);
}

std::optional<OwnKvm> LocalWall::ontoHttps(const OwnKvm& known, const KvmProbe::Found& found,
                                           const std::function<std::optional<QUrl>(const QUrl&)>& otherWay)
{
    // A JetKVM spoken to the plain way, and a JetKVM's answer there (one that says `isSetup`, on a line
    // that was not refused): a router's own page on port 443 is no JetKVM.
    const QUrl from(known.address);
    if (known.kind != "jetkvm"_L1 || from.scheme() != "http"_L1 || found.kind != KvmProbe::Kind::JetKvm ||
        found.refused || !otherWay) {
        return std::nullopt;
    }
    // Onto HTTPS at the same host, and never anywhere else.
    const std::optional<QUrl> to = otherWay(from);
    if (!to || to->scheme() != "https"_L1 || to->host() != from.host()) {
        return std::nullopt;
    }
    OwnKvm onto = known;
    onto.address = to->toString(QUrl::FullyEncoded);
    // Known by what it showed there, as a KVM that is added is known.
    kvm::knowByWhatItShowed(onto, *to, found);
    return onto;
}

base::Result<void> LocalWall::knowBy(QString id, const QByteArray& certificate)
{
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& one) { return one.id == id; });
    if (known == kvms().end()) {
        return unknown();
    }
    if (QUrl(known->address).scheme() != "https"_L1) {
        return noCertificate();
    }
    if (certificate.size() != sha256Bytes) {
        // Nothing is remembered in the place of a certificate that is not one.
        return noSha256();
    }
    OwnKvm with = *known;
    with.knownByCertificate = true;
    with.certificate = certificate;
    // What shows the certificate another KVM of the wall is known by is that KVM, at this one's
    // address: this one's kept password does not go to it, whoever said yes.
    QList<OwnKvm> others = kvms();
    others.removeIf([&id](const OwnKvm& one) { return one.id == id; });
    if (const std::optional<OwnKvm> twin = twinOf(others, with)) {
        return sameDevice(*twin, !sameCertificate(*twin, with));
    }
    // The one way the kept password goes to what shows another certificate than it was saved
    // for: somebody was shown this certificate and said yes to it.
    return alter(id, std::move(with), std::nullopt, "Trusting this certificate", true);
}

base::Result<void> LocalWall::alter(const QString& id, OwnKvm kvm, const std::optional<QString>& password,
                                    const char* what, bool saidYesTo, const std::optional<KvmProbe::Found>& overHttps)
{
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& one) { return one.id == id; });
    Held* one = held(id);
    if (known == kvms().end() || one == nullptr) {
        return unknown();
    }
    const qsizetype place = known - kvms().begin();
    kvm.id = id;
    // (Its family is what it is: no change makes a KVM of another.)
    kvm.kind = known->kind;
    // The server a controller is kept by stays the one it was, unless its password is typed with the change: whoever
    // types it says "this is the server" (the web's `take`), and the box hands the mark it signed in to then. No box
    // sets a mark it did not sign in for.
    if (!password) {
        kvm.machine = known->machine;
    }
    // Whether it was asked what it is, is the wall's own to say, whatever stands in what was
    // handed in: a KVM is not made one that was asked by being changed. And it is one that was
    // NOT asked from here on where somebody said yes to another certificate than it was known
    // by: whoever looked at that certificate did not ask what kind of thing shows it, and the
    // kept password does not go to it before the wall has. (What is kept of it for the next
    // run: only of a KVM over HTTPS.)
    const bool byAnotherCertificate =
        kvm.knownByCertificate != known->knownByCertificate || kvm.certificate != known->certificate;
    const bool askedFromNowOn = one->asked && !(saidYesTo && byAnotherCertificate);
    kvm.askedWhatItIs = askedFromNowOn && keepsThatItWasAsked(kvm);
    if (const std::optional<base::Error> refusal = notAnAddress(kvm.address)) {
        return *refusal;
    }
    const bool taken = std::ranges::any_of(
        kvms(), [&kvm](const OwnKvm& other) { return other.id != kvm.id && atOnePlace(other, kvm); });
    if (taken) {
        return onTheWall();
    }
    // Only where it comes to the place of a kept one that it was not at before: a KVM that shares
    // its place with a kept one, as a file of an earlier build can have it, is changed as ever where
    // it stays. (Asked of each kept one: two places with no port written are one place whatever the
    // scheme, so "it stays" is no answer by itself: http://h changed to https://h stays, and comes to
    // a kept one at http://h:443 that it was not at.)
    const bool keptNewly = std::ranges::any_of(
        keptAside(), [&kvm, &known](const OwnKvm& kept) { return atOnePlace(kept, kvm) && !atOnePlace(kept, *known); });
    if (keptNewly) {
        return keptThere();
    }
    if (const std::optional<base::Error> refusal = intoTheClear(*known, kvm)) {
        return *refusal;
    }
    // What its password is saved for from now on. None is kept for a KVM that is to be known by
    // what is no certificate: it could never be used.
    const std::optional<QByteArray> lines = savedFor(kvm);
    if (password && !lines) {
        return certificateNotRead();
    }
    // The one exception to that rule (change()): a JetKVM over plain HTTP taken onto HTTPS at the
    // same host, where a JetKVM answered there, exactly as ontoHttps() makes it of the KVM as it is:
    // its name and group may be new, and nothing else.
    // (By where the wall has a JetKVM's other way. A JetKVM has no user name: what stands there for one
    // is not a sign-in's.)
    bool ontoItsHttps = false;
    if (!password && !saidYesTo && overHttps) {
        if (std::optional<OwnKvm> onto = ontoHttps(*known, *overHttps, m_paces.jetkvm.otherWay)) {
            onto->name = kvm.name;
            onto->group = kvm.group;
            onto->user = kvm.user;
            onto->askedWhatItIs = kvm.askedWhatItIs;
            // (Nor is the number it gives for itself, as it says it now: that tells it from another, and lets
            // no password go anywhere.)
            onto->number = kvm.number;
            ontoItsHttps = *onto == kvm;
        }
    }
    // And not onto what is another KVM of the wall (twinOf: it shows the certificate that one is known by, or
    // gives its number): the name of this one may reach that one by now, and this one's kept password does not
    // go to it.
    if (ontoItsHttps) {
        // (By what its HTTPS showed first, whoever vouches for that there: from the third reading of the twins.)
        if (const std::optional<OwnKvm> twin = anotherKnownBy(id, overHttps->shown)) {
            return sameDevice(*twin);
        }
        QList<OwnKvm> others = kvms();
        others.removeIf([&id](const OwnKvm& other) { return other.id == id; });
        if (const std::optional<OwnKvm> twin = twinOf(others, kvm)) {
            return sameDevice(*twin, !sameCertificate(*twin, kvm));
        }
    }
    // The rule for the kept password is the wall's own, whoever asks: with none given, a KVM is
    // changed only to where the kept one may go.
    if (!password && !saidYesTo && !keptPasswordMayGoTo(kvm, *known) && !ontoItsHttps) {
        return passwordNeeded();
    }
    // What its driver is made of. A new name or group is none of it.
    const bool anew = password.has_value() || kvm.address != known->address || kvm.user != known->user ||
                      kvm.knownByCertificate != known->knownByCertificate || kvm.certificate != known->certificate;
    if (!anew && kvm == *known) {
        return {};
    }
    if (anew && one->busy) {
        // What is under way to it is not cut off, and not left without anybody to hear how it ended.
        return busyWith(calledInASentence(*known));
    }
    // The kept one goes with the KVM to another address or another certificate, saved anew for
    // those. Where it has none, there is none to take along. To another address the KVM is not
    // moved without one then: it would stand there with what the store still holds for the place
    // it left, or with what Windows could not read back this once. A certificate somebody said
    // yes to is remembered all the same, and the KVM has no password under that one either.
    const bool goesAlong = !password && lines != savedFor(*known);
    // The password it has from now on: the one that is given, or the one that is kept. What is
    // kept is its password only where it was saved for the KVM as it was; otherwise it has none.
    // The store is read only where that is used: for the driver that is made anew, for the one
    // that goes along, and to be put back where the list cannot be written. (A new name or group
    // reads nothing.) What was read, and what was made of it, is written over when this is left.
    const QString secret = secretOf(id);
    QByteArray keptBefore = anew || goesAlong ? m_secrets.kept(secret) : QByteArray();
    const WipedOnLeaving overKept(keptBefore);
    QString fromNowOn = password ? *password : passwordIn(keptBefore, *known);
    const WipedOnLeaving overFromNowOn(fromNowOn);
    // (A JetKVM that runs without one, taken onto HTTPS, has none to take along, and needs none: one that has
    // none kept by the wall's mark. One whose password is kept, and was not read back this once, is not moved
    // without it.)
    if (goesAlong && fromNowOn.isEmpty() && !saidYesTo && !(ontoItsHttps && !marksAPasswordFor(id))) {
        return noPassword();
    }
    // Written where one is given, and where the kept one goes along. Where none does, what the
    // store holds stays what it is: saved for something else, and used for nothing. (There is
    // something to save it for wherever there is one to write: looked at above for one that is
    // given, and a kept one goes only with a KVM that is known by what can be.)
    const bool written = lines.has_value() && (password.has_value() || (goesAlong && !fromNowOn.isEmpty()));
    // The password first, as when a KVM is added; and what was kept before is kept again where
    // the rest cannot be written down. Where that fails too, what stays in the store was saved
    // for the KVM as it was to be, and is not used for the KVM as it is. (What it is kept as is
    // written over when this is left: KeptAs.)
    std::optional<KeptAs> keptAs;
    if (written) {
        keptAs.emplace(*lines, fromNowOn);
        if (const base::Result<void> kept = m_secrets.keep(secret, keptAs->bytes()); !kept) {
            return notSaved(what, kept.error());
        }
    }
    // (What is remembered is another list once it has been written down: the KVM as it was is
    // not looked at after this.)
    QList<OwnKvm> with = kvms();
    with[place] = kvm;
    // The certificate itself that is kept beside it stays only while the KVM is known by the
    // certificate it is the text of: known by another, or by none, it is no longer this KVM's.
    QHash<QString, QString> certificates = m_settings.remembered().kvmCertificates;
    if (!kvm.knownByCertificate || !known->knownByCertificate || kvm.certificate != known->certificate) {
        certificates.remove(id);
    }
    // The mark beside it, in the same writing: a password is kept for it where one was written
    // that is one. Where what it is saved for went on without one (a certificate somebody said yes
    // to, with none kept for the KVM as it was), what the store holds is used for nothing from here
    // on, and no password is kept for it either. Otherwise the mark stays what it was.
    QSet<QString> marks = m_settings.remembered().kvmPasswordsKept;
    if ((written && fromNowOn.isEmpty()) || (!written && goesAlong)) {
        marks.remove(id);
    } else if (written) {
        marks.insert(id);
    }
    if (const base::Result<void> kept = remember(with, certificates, marks, what); !kept) {
        // (The list and the marks are what the file still says: remember() put them back.)
        if (written && keptBefore.isEmpty()) {
            m_secrets.forget(secret);
        } else if (written) {
            static_cast<void>(m_secrets.keep(secret, keptBefore));
        }
        // A writing is counted when it has happened: here only where what was kept before did
        // not go back. Whoever read the KVM before this, and nothing else was changed, still
        // has it as it is. (Read once more for that, and written over.)
        if (written) {
            QByteArray stays = m_secrets.kept(secret);
            const WipedOnLeaving overStays(stays);
            if (stays != keptBefore) {
                ++one->secretWrites;
            }
        }
        return kept.error();
    }
    if (written) {
        ++one->secretWrites;
    }
    if (anew) {
        // Whoever waits for a picture of it asks again, and is answered by the new driver.
        letGo(*one, connecting());
        // (Which has no password where the KVM was not asked yet what it is, or is to be asked
        // again under a certificate somebody said yes to: the one that is kept, or was given,
        // stays in the store until it was.)
        one->asked = askedFromNowOn;
        one->device = driverFor(*one, kvm, one->asked ? fromNowOn : QString());
        listenTo(*one);
        if (!one->asked) {
            // It is another to ask from now on: what an asking that is under way finds was
            // asked of the KVM as it was, and counts for nothing; and it is asked anew at once.
            one->asking = 0;
            one->waitsToBeAsked = false;
            one->afterAsking.clear();
            one->saidWhenAsked.reset();
            one->askAgainAt = 0;
            one->askedInVain = 0;
        }
    }
    emit changed();
    return {};
}

base::Result<std::unique_ptr<OneSignIn>> LocalWall::signerIn(const QString& id, const QUrl& address,
                                                             const QString& user, const DriverPaces& paces,
                                                             const std::optional<KvmProbe::Found>& overHttps)
{
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& one) { return one.id == id; });
    if (known == kvms().end()) {
        return unknown();
    }
    // The kept password goes nowhere before the KVM was asked what it is, whoever asks for it.
    if (!wasAskedWhatItIs(id)) {
        return notAskedYet();
    }
    // Every sign-in of it held to what the wall's own driver is held to before its password goes in the clear: its
    // HTTPS looked at first, where that shows another KVM of the wall nothing goes (from the third reading of the
    // twins, MEDIUM 1). (Over HTTPS the line itself is held to what the KVM is known by, and the driver looks no
    // further.)
    const JetKvmDevice::TwinCheck twins = twinCheckFor(id);
    // The one exception (change()'s): a JetKVM over plain HTTP at its other way, HTTPS at its own host, where
    // a JetKVM answered: to what shows what that answer showed, by the wall's own other way.
    if (overHttps) {
        if (const std::optional<OwnKvm> onto = ontoHttps(*known, *overHttps, m_paces.jetkvm.otherWay);
            onto && onto->address == address.toString(QUrl::FullyEncoded)) {
            // (Not onto what shows the certificate another KVM of the wall is known by: that is the other one, also
            // where Windows trusts that certificate there. From the third reading of the twins, L5.)
            if (const std::optional<OwnKvm> twin = anotherKnownBy(id, overHttps->shown)) {
                return sameDevice(*twin);
            }
            const OneSignIn::Where where{address, user, onto->knownByCertificate, onto->certificate, onto->number};
            QString kept = keptPasswordOf(m_secrets, *known);
            const WipedOnLeaving overKept(kept);
            return OneSignIn::of(known->kind, where, kept, paces, twins);
        }
    }
    if (const std::optional<base::Error> refusal = keptPasswordStaysBehind(id, address)) {
        return *refusal;
    }
    // By a driver of its family, held to what the KVM is known by: its certificate, a JetKVM's number (J5), and a
    // controller's to the server it is kept by (another server there is not sent the kept password where its
    // controller's number tells it).
    const OneSignIn::Where where{address, user, known->knownByCertificate, known->certificate, markHeldTo(*known)};
    // (What was read is written over once the sign-in's driver has its own copy. F5.)
    QString kept = keptPasswordOf(m_secrets, *known);
    const WipedOnLeaving overKept(kept);
    return OneSignIn::of(known->kind, where, kept, paces, twins);
}

QString LocalWall::markHeldTo(const OwnKvm& kvm)
{
    return kvm.kind == "redfish"_L1 ? kvm.machine : kvm.number;
}

std::optional<base::Error> LocalWall::keptPasswordStaysBehind(const QString& id, const QUrl& address) const
{
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& one) { return one.id == id; });
    if (known == kvms().end()) {
        return unknown();
    }
    // Where it would be signed in to, known as it is known. (One address is written one way, so
    // the same text it is: kvm::addressFrom.)
    OwnKvm there = *known;
    there.address = address.toString(QUrl::FullyEncoded);
    return keptPasswordMayGoTo(there, *known) ? std::nullopt : std::optional<base::Error>(passwordNeeded());
}

bool LocalWall::ask(Held& one)
{
    if (one.asking != 0) {
        return true;
    }
    if (m_clock.elapsed() < one.askAgainAt) {
        return false;
    }
    const auto known = std::ranges::find_if(kvms(), [&one](const OwnKvm& kvm) { return kvm.id == one.id; });
    if (known == kvms().end()) {
        return false;
    }
    if (!savedFor(*known)) {
        // Said to be known by a certificate, and names none that is one: there is nothing to
        // hold a line to, and nothing is asked. Its card says what to do, as its driver would
        // (and the wall says by itself that it does, the first time).
        base::Error why = certificateNotRead();
        if (saysAnother(one.saidWhenAsked, why)) {
            noteThatOneCameToSomething();
        }
        one.saidWhenAsked = std::move(why);
        one.waitsToBeAsked = false;
        return false;
    }
    if (!hasRoomToAsk()) {
        // No place is free, or as many askings are under way as there may be: this one waits
        // its turn.
        return false;
    }
    one.waitsToBeAsked = false;
    one.asking = ++m_asked;
    m_askingsOut.insert(one.asking);
    // It holds a place for a short while. What has not ended by then goes on without it.
    m_inAPlace.insert(one.asking);
    QTimer::singleShot(std::max(0, m_own.placeHeldMs), this, [this, number = one.asking] { leavesItsPlace(number); });
    // On lines that are held to what the KVM is known by, as its driver's are: what shows
    // another certificate than the one it came with, or one that Windows does not trust where
    // it is known by Windows' word, hears nothing, not the three questions either.
    const OwnKvm& askedAs = *known;
    if (m_probe == nullptr) {
        m_probe = std::make_unique<KvmProbe>(askingPace(m_paces));
    }
    if (askedAs.kind == "redfish"_L1 && m_serverProbe == nullptr) {
        m_serverProbe = std::make_unique<KvmProbe>(serverAskingPace(m_paces));
    }
    KvmProbe& probe = askedAs.kind == "redfish"_L1 ? *m_serverProbe : *m_probe;
    // A JetKVM over plain HTTP: its HTTPS at its own host is asked beside the three questions, the
    // JetKVM's question alone (design 3.5: before the first sign-in of a run). The asking has ended once
    // both have answered.
    std::optional<QUrl> overHttps;
    // (At its first asking, and then no more often than a driver asks it while it is away.)
    const bool overHttpsDue = !one.overHttpsAskedAt || one.heldBackByTwin ||
                              m_clock.elapsed() - *one.overHttpsAskedAt >= std::max(0, m_paces.jetkvm.wayWhenDownMs);
    if (askedAs.kind == "jetkvm"_L1 && QUrl(askedAs.address).scheme() == "http"_L1 && m_paces.jetkvm.otherWay &&
        overHttpsDue) {
        overHttps = m_paces.jetkvm.otherWay(QUrl(askedAs.address));
        if (overHttps && (overHttps->scheme() != "https"_L1 || overHttps->host() != QUrl(askedAs.address).host())) {
            overHttps.reset();
        }
    }
    struct Answers {
        std::optional<KvmProbe::Found> atItsAddress;
        std::optional<KvmProbe::Found> overHttps;
        int out = 1;
    };
    auto answers = std::make_shared<Answers>();
    answers->out = overHttps ? 2 : 1;
    const auto oneAnswered = [this, here = QPointer<LocalWall>(this), id = one.id, number = one.asking, askedAs,
                              answers] {
        if (--answers->out > 0) {
            return;
        }
        // There is room for one more, whoever the answer is still for: the next that wait are asked.
        // (Whoever is told what was found may do anything.)
        m_askingsOut.remove(number);
        m_inAPlace.remove(number);
        wasAsked(id, number, askedAs, answers->atItsAddress.value_or(KvmProbe::Found{}), answers->overHttps);
        if (here != nullptr) {
            askThoseThatWait();
        }
    };
    probe.askAsKnown(QUrl(askedAs.address), KvmProbe::Known{askedAs.knownByCertificate, askedAs.certificate},
                     [answers, oneAnswered](const KvmProbe::Found& found) {
                         answers->atItsAddress = found;
                         oneAnswered();
                     });
    if (overHttps) {
        one.overHttpsAskedAt = m_clock.elapsed();
        m_probe->askForAJetKvm(*overHttps, [answers, oneAnswered](const KvmProbe::Found& found) {
            answers->overHttps = found;
            oneAnswered();
        });
    }
    return true;
}

bool LocalWall::hasRoomToAsk() const
{
    return m_inAPlace.size() < std::max(1, m_own.askedAtOnce) && m_askingsOut.size() < std::max(1, m_own.askedInAll);
}

void LocalWall::askThoseThatWait()
{
    // Those first that were never asked in vain, in the wall's order: a KVM that waits for its
    // first asking does not wait behind the second asking of one that was not found. (Nor
    // behind the first asking of one that is off, for longer than that one holds its place:
    // leavesItsPlace.)
    for (Held& one : m_held) {
        if (!hasRoomToAsk()) {
            return;
        }
        if (!one.asked && one.waitsToBeAsked && one.askedInVain == 0) {
            static_cast<void>(ask(one));
        }
    }
    // Then those that were asked in vain: the one first that has been due for the longest, and
    // NOT in the wall's order. Where more of them stay off than can be asked at once, the
    // first of the wall are due again each time before the last have had their turn: in the
    // wall's order those at the end were never asked a second time, and said "nothing answers"
    // for ever, also once they were well again. By turns, each is asked again, later the more
    // of them there are.
    std::vector<Held*> again;
    for (Held& one : m_held) {
        if (!one.asked && one.waitsToBeAsked && one.askedInVain > 0) {
            again.push_back(&one);
        }
    }
    std::ranges::stable_sort(again, {}, [](const Held* one) { return one->askAgainAt; });
    for (Held* one : again) {
        if (!hasRoomToAsk()) {
            return;
        }
        static_cast<void>(ask(*one));
    }
}

void LocalWall::leavesItsPlace(quint64 number)
{
    if (!m_inAPlace.remove(number)) {
        // It has ended meanwhile, as the asking of a KVM that is well does.
        return;
    }
    // Still out, and its place is another's from here on: it goes on for as long as an asking
    // may take, which is the time the KVM's driver would give the KVM, and counts among all
    // that are under way until it has ended. Whoever waits for it waits no longer: the KVM is
    // told as it was last known, and the wall says when it has come to something.
    const auto asked = std::ranges::find_if(m_held, [number](const Held& one) { return one.asking == number; });
    const QList<std::function<void()>> waiting =
        asked == m_held.end() ? QList<std::function<void()>>() : std::exchange(asked->afterAsking, {});
    askThoseThatWait();
    // (Last: whoever is told may do anything.)
    for (const std::function<void()>& tell : waiting) {
        tell();
    }
}

void LocalWall::wasAsked(const QString& id, quint64 number, const OwnKvm& askedAs, const KvmProbe::Found& found,
                         const std::optional<KvmProbe::Found>& overHttps)
{
    Held* one = held(id);
    if (one == nullptr || one->asking != number) {
        // Taken off the wall meanwhile, or another to ask by now: what was asked is nobody's.
        return;
    }
    one->asking = 0;
    const QList<std::function<void()>> waiting = std::exchange(one->afterAsking, {});
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& kvm) { return kvm.id == id; });
    const bool asItIs = known != kvms().end() && known->address == askedAs.address &&
                        known->knownByCertificate == askedAs.knownByCertificate &&
                        known->certificate == askedAs.certificate;
    // A JetKVM over plain HTTP that answers over HTTPS at its own host (with `isSetup`): followed there
    // before any driver has its password. That answer is its asking (design 3.5), and it is held to what
    // it showed there from then on. Where that cannot be saved, on as its plain way's answer says.
    const std::optional<OwnKvm> movedOnto =
        asItIs && overHttps ? ontoHttps(*known, *overHttps, m_paces.jetkvm.otherWay) : std::nullopt;
    if (movedOnto && overHttps) {
        one->asked = true;
        // (Not followed onto what shows the certificate another KVM of the wall is known by: that is held back below,
        // as whatever else its HTTPS showed.)
        const base::Result<bool> came = followOntoHttps(id, QUrl(known->address), QUrl(movedOnto->address), *overHttps);
        if (came.ok() && came.value()) {
            if (Held* followed = held(id)) {
                followed->heldBackByTwin = false;
                markHeldFor(id, QString());
                followed->waitsToBeAsked = false;
                followed->saidWhenAsked.reset();
                followed->askedInVain = 0;
                // And it is looked at there and then (listenTo says when that look has ended).
                followed->device->look();
            }
            for (const std::function<void()>& tell : waiting) {
                tell();
            }
            return;
        }
        one->asked = false;
    }
    // Its HTTPS was asked this time, and did not take it there. What that line showed, whatever it answered (a
    // JetKVM's answer, an error, an answer past its time, a page that is no JetKVM's): the certificate another KVM of
    // the wall is known by is that one, and this one is held back: this asking did not find it, whatever its plain
    // way says, and no password goes there in the clear. Also at its first asking after a start. Held back, it is let
    // go of that only where its HTTPS showed one no other KVM of the wall is known by, or nothing listens there; in
    // doubt (something listens, and showed nothing), it stays held, as kvm::wayOf takes what listens for something
    // (from the second reading of the twins, MEDIUM), for as long as the one it is held for is on the wall. A start
    // keeps the hold (heldFor; from the third reading, L2): in doubt at its first asking, it stays held.
    bool holdLifted = false;
    if (overHttps && asItIs) {
        // (The one rule for what holds it back, judgeItsHttps's: twinBehind.)
        if (const std::optional<OwnKvm> twin = twinBehind(id, *overHttps)) {
            heldBackForItsTwin(*one, *twin);
            for (const std::function<void()>& tell : waiting) {
                tell();
            }
            return;
        }
        holdLifted = std::exchange(one->heldBackByTwin, false);
    }
    // (What it was found to be, against the family it is kept as: a KVM of one family that answers
    // as another is not the one.)
    const std::optional<KvmProbe::Kind> family = asItIs ? kvm::foundAs(known->kind) : std::nullopt;
    if (family && found.kind == *family && !found.refused) {
        // What answers there is the KVM, by what it is known by, and says that it is one of the
        // family: from here on it has a driver with its password. Of a KVM over HTTPS that is
        // noted, so that it is asked once (written down a short while later, with what else the
        // wall notes by itself; where the program ends before that, it is asked again at the
        // next start). Of one over plain HTTP nothing is kept: it is asked in every run.
        one->asked = true;
        one->waitsToBeAsked = false;
        one->saidWhenAsked.reset();
        OwnKvm asked = *known;
        asked.askedWhatItIs = keepsThatItWasAsked(asked);
        if (asked.askedWhatItIs != known->askedWhatItIs) {
            QList<OwnKvm> with = kvms();
            with[known - kvms().begin()] = asked;
            // (The list is another from here on: the KVM as it stood there is not looked at.)
            noteByItself(with, m_settings.remembered().kvmCertificates);
        }
        letGo(*one, connecting());
        // (What was read is written over once the driver has its own copy. F5.)
        QString kept = keptPasswordOf(m_secrets, asked);
        const WipedOnLeaving overKept(kept);
        one->device = driverFor(*one, asked, kept);
        listenTo(*one);
        // Its HTTPS was asked by this asking a moment ago: the driver asks it at its own pace, not at once. Not where
        // that asking has just let go of a hold: the driver asks it at its first look, and so looks once more.
        if (overHttps && !holdLifted) {
            if (auto* jet = qobject_cast<JetKvmDevice*>(one->device.get())) {
                jet->otherWayAskedJustNow();
            }
        }
        // And it is looked at there and then: how it is, is known when the wall is next asked,
        // and the wall says by itself when that look has ended (listenTo).
        one->device->look();
    } else if (asItIs) {
        // Not the KVM, or nothing: said on its card, with its password where it was. Asked
        // again later, and later each time, as a driver leaves a KVM alone that is away.
        base::Error why = notTheKvm(found, known->kind, kvm::AskedAt::ItsAddress);
        // Where its card says another thing for that than it did, the wall says so by itself.
        // (Not at every asking of a KVM that stays away: nothing has come of that.)
        if (saysAnother(one->saidWhenAsked, why)) {
            noteThatOneCameToSomething();
        }
        one->saidWhenAsked = std::move(why);
        ++one->askedInVain;
        const qint64 rest = qint64{m_paces.kvmd.holdMs} << std::min(one->askedInVain - 1, 10);
        one->askAgainAt = m_clock.elapsed() + std::min<qint64>(m_paces.kvmd.longestHoldMs, rest);
    }
    // A hold let go of: and so is what was kept of it for the next start. (Last: the list is another from here on.)
    if (holdLifted) {
        markHeldFor(id, QString());
    }
    // (Changed while it was asked: nothing was learned of the KVM as it is, and it is asked anew.)
    for (const std::function<void()>& tell : waiting) {
        tell();
    }
}

base::Error LocalWall::whyNothingYet(const Held& one)
{
    // Not asked yet, and no asking has found anything amiss: as a driver that was only just
    // made says it, "asking again soon is the thing to do". A wall asks for the picture of each
    // of its KVMs as soon as it is shown, a second before or after the KVM was asked what it
    // is; told "not asked yet" for that, it leaves the KVM alone for seconds as one whose
    // picture failed, and a wall that is well had its pictures that much later at its start.
    return one.saidWhenAsked ? notAskedYet() : connecting();
}

Status LocalWall::howItIsUnasked(const Held& one) const
{
    Status status;
    if (!one.saidWhenAsked) {
        // Not asked yet, or being asked for the first time: as any KVM is that has not answered
        // once, "connecting", and nothing else. Nothing is wrong with it, and its card does not
        // read as if something were: every KVM of a wall that is well stands here for a moment
        // at the first start, and one that is slow to let a line in for some seconds. (The
        // sentence for a KVM that was not asked yet stood here once, under "connecting". It is
        // for whoever asks something of such a KVM that cannot be done yet: notAskedYet.)
        return status;
    }
    // As a driver's look says each of these: what does not answer is away, and everything else
    // (a certificate that is not the KVM's, something that is no KVM) is for whoever sits here
    // to look at.
    const base::Error& why = *one.saidWhenAsked;
    status.state = why.code == "unreachable" ? u"offline"_s : u"error"_s;
    status.errorCode = QString::fromStdString(why.code);
    status.errorMessage = QString::fromStdString(why.message);
    return status;
}

bool LocalWall::followedOntoHttps(const QString& id) const
{
    const auto known = std::ranges::find_if(kvms(), [&id](const OwnKvm& one) { return one.id == id; });
    const QString followedTo = m_settings.remembered().kvmsFollowedOntoHttps.value(id);
    return known != kvms().end() && !followedTo.isEmpty() && followedTo == known->address;
}

void LocalWall::sawNote(const QString& machine)
{
    Remembered with = m_settings.remembered();
    if (!with.kvmsFollowedOntoHttps.remove(machine)) {
        return;
    }
    m_settings.note(with);
    // (Written soon, as what the wall notes by itself: whoever saw it did nothing to the wall.)
    noteByItself(with.kvms, with.kvmCertificates);
    emit changed();
}

bool LocalWall::wasAskedWhatItIs(const QString& id) const
{
    const auto found = std::ranges::find_if(m_held, [&id](const Held& one) { return one.id == id; });
    return found != m_held.end() && found->asked;
}

base::Error LocalWall::notAskedYet()
{
    return {"not_asked_yet", words("This device has not been asked yet what it is. Nothing is sent to it, and no "
                                   "password goes anywhere, before what answers at its address has said that it is a "
                                   "KVM.")
                                 .toStdString()};
}

base::Error LocalWall::notTheKvm(const KvmProbe::Found& found, const QString& family, kvm::AskedAt at)
{
    if (found.refused) {
        switch (*found.refused) {
        case kvm::Objected::Changed:
            return certificateChanged();
        case kvm::Objected::BySigner:
            return certificateOfAnUnknownSigner();
        case kvm::Objected::Untrusted:
            return certificateNotTrustedAndNothingSent();
        case kvm::Objected::PassedOver:
            break;
        }
    }
    if (found.kind == KvmProbe::Kind::NotAsked) {
        return damagedTls();
    }
    // (Of its own family, and not refused, it is the KVM, and this is not asked of it: whoever would
    // anyway is told what is told of nothing.)
    const std::optional<KvmProbe::Kind> its = kvm::foundAs(family);
    if (found.kind == KvmProbe::Kind::Nothing || (its && found.kind == *its)) {
        return {"unreachable", words("Nothing answers at this address. No password is sent there before a KVM has "
                                     "answered.")
                                   .toStdString()};
    }
    if (found.saidByAGatewayAlone) {
        // All that answered was a gateway, saying that what is behind it does not answer: a
        // reverse proxy before a KVM that restarts, a tunnel whose far end is away. That may
        // well be a KVM that is away, and is said as an address where nothing answers is
        // ("unreachable": offline on its card, and asked again as that is), in a sentence that
        // says who answered and who did not. "Not a KVM" would be untrue of such a one, and
        // reads as if somebody had added the wrong thing. What is behind it is not known, and
        // the sentence does not say that it is a KVM: a web server that is being mended says
        // 503 at an address that was mistyped, too.
        return {"unreachable", words("Only a gateway answers at this address, and it says that what is behind it "
                                     "does not answer just now. No password was sent.")
                                   .toStdString()};
    }
    // (The sentence a box that adds a KVM has for it, ended as a sentence, and one more. "No
    // password", and not "its password": a KVM that was taken without one has none. A KVM of the
    // other family that this program speaks to is another KVM than this one, and said to be.)
    base::Error why = kvm::familyOf(found.kind).isEmpty() ? kvm::whyNotSignedInTo(found.kind)
                                                          : kvm::ofAnotherFamily(found.kind, family, at);
    if (!why.message.ends_with('.')) {
        why.message += '.';
    }
    why.message += ' ' + words("No password was sent to it.").toStdString();
    return why;
}

QString LocalWall::certificateOf(const QString& id) const
{
    return m_settings.remembered().kvmCertificates.value(id);
}

QList<base::Result<QString>> LocalWall::takeFromAFile(const QList<FromAFile>& those)
{
    // The list as it would be, with each that is taken; and what was kept in the store for
    // them, to be taken out again where the list cannot be written down.
    QList<OwnKvm> with = kvms();
    QHash<QString, QString> certificates = m_settings.remembered().kvmCertificates;
    // And the marks of those for which a password is kept, which are written with them.
    QSet<QString> marks = m_settings.remembered().kvmPasswordsKept;
    QList<base::Result<QString>> came;
    QList<qsizetype> taken; // their places in `came`
    QList<OwnKvm> tookIn;
    // What each password was kept as, written over when this is left (KeptAs): held in one place
    // each, never moved, until then.
    std::list<KeptAs> keptAs;
    // Who is at which host, who is known by which certificate, and who gives which number for
    // itself (twinOf), of the wall and of those that were just taken: a list of two thousand is not
    // held against two thousand one by one, with every address read anew for every pair.
    QHash<QString, QList<Place>> atHost;
    QHash<QByteArray, qsizetype> knownBy; // where in `with`
    QHash<QString, qsizetype> givesNumber;
    const auto note = [&atHost, &knownBy, &givesNumber](const OwnKvm& kvm, qsizetype index) {
        const Place place = placeOf(kvm);
        atHost[place.host].append(place);
        // (A server's controller is no twin by a certificate, whichever of the two: sameCertificate.)
        if (kvm.knownByCertificate && kvm.kind != "redfish"_L1 && !knownBy.contains(kvm.certificate)) {
            knownBy.insert(kvm.certificate, index);
        }
        if (!kvm.number.isEmpty() && !isKeptBeside(kvm) && !givesNumber.contains(kvm.number)) {
            givesNumber.insert(kvm.number, index);
        }
    };
    for (qsizetype index = 0; index < with.size(); ++index) {
        note(with.at(index), index);
    }
    // And where the KVMs are that the settings keep and this build does not read, once.
    QList<Place> keptPlaces;
    for (const OwnKvm& kept : keptAside()) {
        keptPlaces.append(placeOf(kept));
    }
    for (const FromAFile& one : those) {
        OwnKvm kvm = one.kvm;
        // Nothing was asked at its address, whatever stands in what was handed in.
        kvm.askedWhatItIs = false;
        // (Only of a family this build speaks to: one of another would be written and not read back.)
        if (!OwnKvm::namesAFamily(kvm.kind)) {
            came.append(base::Result<QString>(
                base::Error{"invalid", "This program does not speak to that family of device itself."}));
            continue;
        }
        if (with.size() + m_settings.remembered().kvmsItDoesNotRead.size() >= Remembered::mostKvms) {
            came.append(base::Result<QString>(base::Error{"too_many", "This wall is full."}));
            continue;
        }
        if (const std::optional<base::Error> refusal = notAnAddress(kvm.address)) {
            came.append(base::Result<QString>(*refusal));
            continue;
        }
        // Never twice: not where a KVM of the wall is, or one that was just taken from this
        // list; and not as the twin of one of those, which is that KVM under another address.
        const Place place = placeOf(kvm);
        const QList<Place> thereAlready = atHost.value(place.host);
        if (std::ranges::any_of(thereAlready, [&place](const Place& known) { return atOnePlace(known, place); })) {
            came.append(base::Result<QString>(onTheWall()));
            continue;
        }
        if (std::ranges::any_of(keptPlaces, [&place](const Place& kept) { return atOnePlace(kept, place); })) {
            came.append(base::Result<QString>(keptThere()));
            continue;
        }
        if (const auto twin = knownBy.constFind(kvm.certificate);
            kvm.knownByCertificate && kvm.kind != "redfish"_L1 && twin != knownBy.constEnd()) {
            came.append(base::Result<QString>(sameDevice(with.at(*twin))));
            continue;
        }
        if (const auto twin = givesNumber.constFind(kvm.number);
            !kvm.number.isEmpty() && !isKeptBeside(kvm) && twin != givesNumber.constEnd()) {
            came.append(base::Result<QString>(sameDevice(with.at(*twin), true)));
            continue;
        }
        const std::optional<QByteArray> lines = savedFor(kvm);
        if (!lines) {
            came.append(base::Result<QString>(certificateNotRead()));
            continue;
        }
        kvm.id = newId(with, m_settings.remembered().kvmsItDoesNotRead);
        const QString secret = secretOf(kvm.id);
        if (one.password.isEmpty()) {
            // Taken without one: its card asks for it. (And nothing that was ever left in the
            // store under this name is its password.)
            m_secrets.forget(secret);
            marks.remove(kvm.id);
        } else if (const base::Result<void> kept =
                       m_secrets.keep(secret, keptAs.emplace_back(*lines, one.password).bytes());
                   !kept) {
            // Where Windows will not keep its password, this one is not taken; the others are.
            came.append(base::Result<QString>(notSaved("This device", kept.error())));
            continue;
        } else {
            marks.insert(kvm.id);
        }
        // The certificate itself, where it is the one the KVM is to be known by; another is
        // none of this KVM's, and is dropped.
        if (kvm.knownByCertificate && !one.certificatePem.isEmpty()) {
            const std::optional<exchange::KeptCertificate> kept = exchange::certificateFrom(one.certificatePem);
            if (kept && kept->sha256 == kvm.certificate) {
                certificates.insert(kvm.id, kept->pem);
            }
        }
        with.append(kvm);
        note(kvm, with.size() - 1);
        tookIn.append(kvm);
        taken.append(came.size());
        came.append(base::Result<QString>(kvm.id));
    }
    if (tookIn.isEmpty()) {
        return came;
    }
    // Written down once, for all of them, with their marks: a wall is never half written.
    if (const base::Result<void> kept = remember(with, certificates, marks, "The devices from the file"); !kept) {
        for (qsizetype index = 0; index < tookIn.size(); ++index) {
            m_secrets.forget(secretOf(tookIn.at(index).id));
            came[taken.at(index)] = base::Result<QString>(kept.error());
        }
        return came;
    }
    for (const OwnKvm& kvm : std::as_const(tookIn)) {
        // Not asked yet: its driver gets no password, and its password is not read here. It is
        // read when an asking has found the KVM (wasAsked), for its driver; for a sealed file of
        // the wall (asAFile); and for a sign-in of somebody's change, once it was asked
        // (signerIn). Whoever lists the KVMs is told by the mark, and reads nothing.
        hold(kvm, false, QString());
    }
    emit changed();
    return came;
}

LocalWall::AsAFile LocalWall::asAFile(bool withPasswords)
{
    AsAFile made;
    QList<exchange::Device> devices;
    const QList<OwnKvm> all = kvms();
    for (qsizetype index = 0; index < all.size(); ++index) {
        const OwnKvm& kvm = all.at(index);
        exchange::Device device;
        device.name = kvm.name;
        device.address = kvm.address;
        device.user = kvm.user;
        device.kind = kvm.kind;
        device.group = kvm.group;
        // (The number it gives for itself, which tells it from another on the wall that takes the file.)
        device.ident = kvm.number;
        device.keepBoth = isKeptBeside(kvm);
        // (And the machine its card was added for: a server's, by the numbers it gave for itself.)
        device.machine = kvm.machine;
        const bool encrypted = kvm.address.startsWith("https://"_L1);
        // Over HTTPS by its certificate, or by Windows' word. (Over plain HTTP there is nothing
        // to check, and a file says "pinned" of that.)
        device.tls = encrypted && !kvm.knownByCertificate ? u"system"_s : u"pinned"_s;
        if (encrypted && kvm.knownByCertificate) {
            device.certificate = kvm.certificate;
            device.certificatePem = certificateOf(kvm.id);
        }
        if (withPasswords) {
            // As it is kept for the KVM as it is, and none that was saved for something else.
            device.password = keptPasswordOf(m_secrets, kvm);
            if (device.password.isEmpty()) {
                made.withoutPassword.append(index);
            }
        }
        devices.append(device);
    }
    made.content.devices = devices;
    return made;
}

exchange::WallSettings LocalWall::portableSettings() const
{
    const Remembered& kept = m_settings.remembered();
    return {kept.wallPictureEveryMs > 0 ? kept.wallPictureEveryMs / 1000.0 : 1.0, kept.checkUpdates};
}

base::Result<void> LocalWall::takeSettings(const exchange::WallSettings& settings)
{
    const Remembered before = m_settings.remembered();
    Remembered after = before;
    if (settings.pictureEverySeconds) {
        const double seconds = *settings.pictureEverySeconds;
        if (!(seconds >= 0.2 && seconds <= 60.0)) {
            return base::Error{"invalid", "The picture interval must be between 0.2 and 60 seconds."};
        }
        after.wallPictureEveryMs = qRound(seconds * 1000.0);
    }
    if (settings.asksForNewerVersions) {
        after.checkUpdates = *settings.asksForNewerVersions;
    }
    if (const base::Result<void> saved = m_settings.remember(after); !saved) {
        static_cast<void>(m_settings.remember(before));
        return saved.error();
    }
    const QPointer<LocalWall> here(this);
    emit portableSettingsChanged();
    if (here != nullptr) {
        emit changed();
    }
    return {};
}

} // namespace qkvm::core
