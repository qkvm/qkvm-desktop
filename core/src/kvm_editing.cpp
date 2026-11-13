#include <qkvm/core/kvm_editing.h>

#include <qkvm/core/screen_watch.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include <QList>
#include <QPointer>
#include <QTimer>

#include <qkvm/core/server_client.h>
#include <qkvm/core/tls.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The server's words for these: a box shows them as they are, whoever said them.
base::Error unknown()
{
    return {"not_found", "Unknown device"};
}

base::Error unreachable()
{
    return {"unreachable", "Device is unreachable"};
}

base::Error onTheWall()
{
    return {"duplicate", "This device is already on the wall"};
}

base::Error showsAnotherNow()
{
    return {"cert_mismatch", "The device now presents a different certificate than the one shown"};
}

base::Error notCheckedThere(const base::Error& why)
{
    return {"password_needed", "This device could not be checked at its new address (" + why.message +
                                   "). Type its password to save it there all the same."};
}

// And this program's own, where the server has no words: it changes as many devices at a time as
// are asked of it, says of a device somebody else changed that it was "by someone else just now"
// (there is nobody else here), and looks at a certificate only where a device has one.
base::Error underWay()
{
    return {"busy", "Another KVM is being changed at this moment. Wait for it to finish."};
}

base::Error underReview()
{
    return {"busy", "A certificate is being looked at at this moment. Wait for it to finish."};
}

base::Error changedMeanwhile()
{
    return {"conflict", "This device was changed meanwhile. Look at it again."};
}

base::Error noCertificate()
{
    return {"no_certificate", "This device is reached over plain HTTP, where there is no certificate"};
}

// (Which two, is not in the sentence: whoever asked is handed them, to be shown and compared.)
base::Error certificateDiffers()
{
    return {"certificate_differs",
            "What answers at the new address shows another certificate than the one saved for this device"};
}

// The same code where it is not two certificates that differ: one is shown where the device is
// known by none, or Windows vouches where it is known by one, or by nothing.
base::Error knownOtherwiseThere()
{
    return {"certificate_differs", "At the new address this device would be known otherwise than it is known now"};
}

// (What is given up there is said by whoever asked, beside what the device is known by now.)
base::Error plainHttpThere()
{
    return {"plain_http_there", "At the new address this device would be spoken to over plain HTTP, where nothing "
                                "shows who answers and its password travels unencrypted"};
}

base::Error changeGivenUp()
{
    return {"given_up", "The change was given up. Nothing was changed."};
}

base::Error reviewGivenUp()
{
    return {"given_up", "The look at the certificate was given up. Nothing was trusted."};
}

// Whether the kept password failed at a new address for a reason that a typed one gets over: the
// KVM shows another certificate there than it is known by (a typed password goes there once
// somebody has seen that one beside the one it is known by, and said yes), or what is kept is not
// its password. Where nothing answers there, or the KVM answers amiss, typing changes nothing.
bool typingGetsOver(const base::Error& why)
{
    return why.code == "cert_mismatch" || why.code == "auth_failed" || why.code == "no_password";
}

// The KVM of a wall by the wall's name for it; nothing: it is not on the wall.
std::optional<OwnKvm> kvmOf(const LocalWall& wall, const QString& id)
{
    const auto known = std::ranges::find_if(wall.kvms(), [&id](const OwnKvm& one) { return one.id == id; });
    return known == wall.kvms().end() ? std::nullopt : std::optional<OwnKvm>(*known);
}

// The KVMs of a wall beside the one that is being changed.
QList<OwnKvm> besides(const LocalWall& wall, const QString& id)
{
    QList<OwnKvm> others = wall.kvms();
    others.removeIf([&id](const OwnKvm& one) { return one.id == id; });
    return others;
}

// A KVM as it would be once it is known by this certificate.
OwnKvm knownBy(OwnKvm kvm, const QByteArray& certificate)
{
    kvm.knownByCertificate = true;
    kvm.certificate = certificate;
    return kvm;
}

} // namespace

namespace kvm {

QString fingerprint(const QByteArray& sha256)
{
    return ServerClient::written(sha256);
}

} // namespace kvm

KvmEditing::KvmEditing(LocalWall& wall, QObject* parent) : KvmEditing(wall, Pace{}, parent) {}

KvmEditing::KvmEditing(LocalWall& wall, Pace pace, QObject* parent)
    : QObject(parent), m_wall(wall), m_pace(std::move(pace)), m_probe(m_pace.probe)
{}

KvmEditing::~KvmEditing() = default;

void KvmEditing::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

void KvmEditing::change(const QString& id, Typed typed, std::function<void(Outcome)> done)
{
    if (m_underWay) {
        later([done = std::move(done)] { done(Outcome{underWay(), {}, {}, {}}); });
        return;
    }
    m_underWay = true;
    m_over = false;
    m_done = std::move(done);

    const std::optional<OwnKvm> known = kvmOf(m_wall, id);
    if (!known) {
        finish(unknown());
        return;
    }
    // What was typed stood over the KVM as somebody saw it a while ago: where it is not that any
    // more, nothing of what was typed is looked at. (Its password too, as below. And compared
    // without what the wall records by itself, which is no change that somebody made: a box
    // that was opened before the wall had asked what the KVM is, is not one over a KVM that
    // "was changed meanwhile".)
    if (typed.seen &&
        (!LocalWall::isStillTheOne(*known, typed.seen->kvm) || typed.seen->secretWrites != m_wall.secretWrites(id))) {
        finish(changedMeanwhile());
        return;
    }
    // What was typed, in the order the server looks at it.
    const base::Result<QString> name = kvm::nameFrom(typed.name);
    if (!name) {
        finish(name.error());
        return;
    }
    const base::Result<QString> user = kvm::userFrom(typed.user);
    if (!user) {
        finish(user.error());
        return;
    }
    if (const base::Result<void> password = kvm::passwordFrom(typed.password); !password) {
        finish(password.error());
        return;
    }
    const base::Result<QString> group = kvm::groupFrom(typed.group);
    if (!group) {
        finish(group.error());
        return;
    }
    const base::Result<QUrl> address = kvm::addressFrom(typed.address);
    if (!address) {
        finish(address.error());
        return;
    }
    m_address = address.value();
    m_before = *known;
    m_secretWrites = m_wall.secretWrites(id);
    m_after = m_before;
    if (!name.value().isEmpty()) {
        m_after.name = name.value();
    }
    m_after.user = user.value();
    m_after.group = group.value();
    // (Nothing a sign-in guards: saved with the rest.)
    if (typed.noSignalMinutes) {
        m_after.noSignalMinutes = ScreenWatch::minutesFrom(*typed.noSignalMinutes);
    }
    if (typed.unchangedMinutes) {
        m_after.unchangedMinutes = ScreenWatch::minutesFrom(*typed.unchangedMinutes);
    }
    m_after.address = m_address.toString(QUrl::FullyEncoded);
    m_password = SecretText::takenFrom(typed.password);
    m_keepBoth = typed.keepBoth;
    m_saidYesTo = std::move(typed.saidYesTo);
    m_saidYesOnWindowsWord = typed.saidYesOnWindowsWord;
    m_saidYesToPlainHttpAt = std::move(typed.saidYesToPlainHttpAt);

    const bool moved = m_after.address != m_before.address;
    // Where another KVM of the wall is, this one is not: and that one is asked nothing for it.
    const bool taken = std::ranges::any_of(m_wall.kvms(), [this](const OwnKvm& other) {
        return other.id != m_before.id && LocalWall::atOnePlace(other, m_after);
    });
    if (moved && taken) {
        finish(onTheWall());
        return;
    }
    // Not from HTTPS to the plain way at the place it is at, whatever was typed with it: and
    // whatever answers there in the clear is asked nothing.
    if (const std::optional<base::Error> refusal = LocalWall::intoTheClear(m_before, m_after)) {
        finish(*refusal);
        return;
    }
    m_typed = typed.address;
    goOn(moved);
}

void KvmEditing::goOn(bool moved)
{
    const QString id = m_before.id;
    const bool aJetKvm = m_before.kind == "jetkvm"_L1;
    if (!moved && m_password.isEmpty() && m_after.user == m_before.user) {
        // The name or the group, and nothing a sign-in guards: saved with nothing asked of the
        // KVM, which may be away. (The server signs in at every edit; why not here is said in
        // the header.)
        save(false);
        return;
    }
    // A JetKVM out of its box runs without a password, and has none kept (the wall's mark): it is not
    // moved by typing one it does not have. What answers at the new address is asked what it is first,
    // as for a password that was typed, and signed in to there as one that needs none.
    m_typedNone = moved && m_password.isEmpty() && aJetKvm && !m_wall.marksAPasswordFor(id);
    if (moved && (!m_password.isEmpty() || m_typedNone)) {
        // From HTTPS to plain HTTP at another place: nothing can be shown of what answers
        // there, so what is given up is put before somebody first, and not one line is made
        // to that address until that somebody has said yes for it. (At the place it is at
        // there is no yes to say: refused above.)
        const bool givesUp = QUrl(m_before.address).scheme() == "https"_L1 && m_address.scheme() == "http"_L1;
        const QString there = m_address.authority(QUrl::FullyEncoded);
        if (givesUp && m_saidYesToPlainHttpAt != there) {
            const QByteArray remembered = m_before.knownByCertificate ? m_before.certificate : QByteArray();
            finish(Outcome{plainHttpThere(), remembered, {}, there, false});
            return;
        }
        // A JetKVM: both ways, as when one is added (and the way chosen is held to the same).
        if (aJetKvm) {
            askBothWays();
            return;
        }
        // Another address, and its password typed for it: as when a KVM is added. First what
        // answers there, with nothing in the question that says who asks.
        m_probe.ask(m_address, [this, turn = m_turn](const KvmProbe::Found& found) {
            if (turn == m_turn) {
                probed(found);
            }
        });
        return;
    }
    // A JetKVM spoken to the plain way that is about to be signed in to: with a password typed at the
    // address it has, or with the one that is kept, under another user name or at its own HTTPS (as the
    // web has it: whatever it is signed in to with). What goes is about to go, and where to is looked at
    // first: its other way, HTTPS at the same host, is asked the JetKVM's question alone, before anything
    // else. A JetKVM there is signed in to there, and that answer is the asking of what it is. (Not asked
    // twice: where both ways were asked already, and the HTTPS one answered no JetKVM.)
    if (const std::optional<QUrl> other = ownHttps(); other && !m_overHttpsAsked && (!moved || m_address == *other)) {
        m_probe.askForAJetKvm(*other, [this, turn = m_turn, at = *other, moved](const KvmProbe::Found& found) {
            if (turn == m_turn) {
                askedOverHttps(at, found, moved, false);
            }
        });
        return;
    }
    signInWhereItIs(moved);
}

std::optional<QUrl> KvmEditing::otherWayOf(const QUrl& address) const
{
    return m_pace.otherWay ? m_pace.otherWay(address) : std::nullopt;
}

std::optional<QUrl> KvmEditing::ownHttps() const
{
    // A JetKVM spoken to the plain way, and its other way HTTPS at the same host: as LocalWall::ontoHttps has it.
    const QUrl from(m_before.address);
    if (m_before.kind != "jetkvm"_L1 || from.scheme() != "http"_L1) {
        return std::nullopt;
    }
    std::optional<QUrl> to = otherWayOf(from);
    if (!to || to->scheme() != "https"_L1 || to->host() != from.host()) {
        return std::nullopt;
    }
    return to;
}

void KvmEditing::signInWhereItIs(bool moved)
{
    const QString id = m_before.id;
    // A KVM that was not asked yet what it is (one that was taken from a file; every KVM of an
    // older settings file, until the wall has asked it once; one whose new certificate somebody
    // said yes to). What is left of the changes here is one of two: something that would sign in
    // to it at the address it has (another user name, or a password that was typed for that
    // address), or another address with the password that is kept.
    if (!m_wall.wasAskedWhatItIs(id)) {
        if (!moved) {
            // At the address it has it is signed in to by nobody: not with the kept password,
            // which the wall refuses by itself, and not with one that was typed here. Nothing
            // goes to it before the wall has asked what answers there.
            finish(LocalWall::notAskedYet());
            return;
        }
        // Another address, and its password left as it is kept: moved as the wall moves it
        // (LocalWall::change), which is the road there is for a KVM whose address is stale, and
        // whose password somebody who took it from a file may never have seen. The kept
        // password goes along only where the KVM can be known there (both addresses HTTPS, and
        // known by a certificate); and nothing is signed in to here, since nothing may be.
        // What answers at the new address is asked what it is first, on lines that are held to
        // that certificate: what shows another hears nothing, and what is no KVM is not saved.
        // Saved, it stays one that was not asked: the wall asks it there itself, and its
        // password goes to it only then.
        if (const std::optional<base::Error> refusal = m_wall.keptPasswordStaysBehind(id, m_address)) {
            finish(*refusal);
            return;
        }
        m_probe.askAsKnown(m_address, KvmProbe::Known{m_before.knownByCertificate, m_before.certificate},
                           [this, turn = m_turn](const KvmProbe::Found& found) {
                               if (turn == m_turn) {
                                   askedAsKnown(found);
                               }
                           });
        return;
    }
    const DriverPaces paces = m_pace.drivers();
    if (m_password.isEmpty()) {
        // With the password that is kept: under another user name, or at another address. The
        // wall makes the driver for that, of the KVM's family, and says where that password may go.
        m_keptElsewhere = moved;
        base::Result<std::unique_ptr<OneSignIn>> signer = m_wall.signerIn(id, m_address, m_after.user, paces);
        if (!signer) {
            finish(signer.error());
            return;
        }
        m_device = std::move(signer).value();
    } else {
        // A password typed for the KVM at the address it has: to the KVM as it is known there,
        // and to nothing that shows another certificate in its place; by a driver of its family.
        const OneSignIn::Where where{m_address, m_after.user, m_before.knownByCertificate, m_before.certificate,
                                     QString()};
        base::Result<std::unique_ptr<OneSignIn>> made = signInWithWhatWasTyped(where, paces);
        if (!made) {
            finish(made.error());
            return;
        }
        m_device = std::move(made).value();
    }
    heedItsSignIn();
    m_device->identify([this, turn = m_turn](const base::Result<OneSignIn::Said>& who) {
        if (turn == m_turn) {
            identified(who);
        }
    });
}

std::optional<KvmEditing::Outcome> KvmEditing::refusalAtTheWayChosen(const LocalWall& wall, const OwnKvm& before,
                                                                     const OwnKvm& after,
                                                                     const QString& saidYesToPlainHttpAt)
{
    // Where another KVM of the wall is, this one is not.
    const bool taken = std::ranges::any_of(wall.kvms(), [&before, &after](const OwnKvm& other) {
        return other.id != before.id && LocalWall::atOnePlace(other, after);
    });
    if (taken) {
        return Outcome{onTheWall(), {}, {}, {}, false};
    }
    // Not from HTTPS to the plain way at the place it is at.
    if (const std::optional<base::Error> refusal = LocalWall::intoTheClear(before, after)) {
        return Outcome{*refusal, {}, {}, {}, false};
    }
    // From HTTPS to plain HTTP at another place: only with the yes to that very address.
    const QUrl at(after.address);
    const bool givesUp = QUrl(before.address).scheme() == "https"_L1 && at.scheme() == "http"_L1;
    const QString there = at.authority(QUrl::FullyEncoded);
    if (givesUp && saidYesToPlainHttpAt != there) {
        const QByteArray remembered = before.knownByCertificate ? before.certificate : QByteArray();
        return Outcome{plainHttpThere(), remembered, {}, there, false};
    }
    return std::nullopt;
}

void KvmEditing::askBothWays()
{
    m_ways = kvm::waysFor(m_typed, m_address, m_pace.otherWay);
    m_waysOut = m_ways.twin ? 2 : 1;
    m_probe.ask(m_address, [this, turn = m_turn](const KvmProbe::Found& found) {
        if (turn == m_turn) {
            m_ways.atAddress = found;
            askedOneWay();
        }
    });
    if (m_ways.twin) {
        m_probe.askForAJetKvm(*m_ways.twin, [this, turn = m_turn](const KvmProbe::Found& found) {
            if (turn == m_turn) {
                m_ways.atTwin = found;
                askedOneWay();
            }
        });
    }
}

void KvmEditing::askedOneWay()
{
    if (--m_waysOut > 0) {
        return;
    }
    // What each HTTPS that was asked showed is judged first, whatever it answered and as whatever family, before a way
    // is chosen (the wall's one judgement, LocalWall::judgeItsHttps; from the outside review, F1): what shows another
    // KVM of the wall is that one; its own HTTPS that shows that holds it back; and one held back stays so in doubt.
    const std::optional<QUrl> own = ownHttps();
    if (const std::optional<base::Error> refusal = judged(m_ways, own)) {
        finish(*refusal);
        return;
    }
    // (In the change box's words where it is refused: to keep it where it is, or to move it.)
    const base::Result<kvm::Way> way = kvm::wayOf(m_ways, QUrl(m_before.address));
    if (!way) {
        finish(way.error());
        return;
    }
    // Where its HTTPS was asked by this (at the address or beside it), it is not asked again.
    if (own && (*own == m_ways.address || (m_ways.twin && *own == *m_ways.twin))) {
        m_overHttpsAsked = true;
    }
    m_address = way.value().address;
    m_after.address = m_address.toString(QUrl::FullyEncoded);
    // Moved or not by the address chosen, and not by how it was typed: chosen where it is, it is as if
    // its address had been typed as it stands (nothing is signed in to that would not be then).
    if (m_after.address == m_before.address) {
        goOn(false);
        return;
    }
    // The way chosen is the address it is moved to, and held to every rule the typed one was held to
    // before anything goes there.
    if (std::optional<Outcome> refusal = refusalAtTheWayChosen(m_wall, m_before, m_after, m_saidYesToPlainHttpAt)) {
        finish(std::move(*refusal));
        return;
    }
    // Chosen at its own HTTPS, where a JetKVM answered: the same device at the same host, as the password kept or
    // one typed at the address it has would take it there (goOn), known by what it showed on this first contact.
    // Any other address chosen is a new one (probed: what it shows is put before somebody first).
    const KvmProbe::Found& found = way.value().found;
    if (own && m_address == *own && found.kind == KvmProbe::Kind::JetKvm && !found.refused) {
        askedOverHttps(*own, found, true, true);
        return;
    }
    probed(found);
}

std::optional<base::Error> KvmEditing::judged(const kvm::Ways& ways, const std::optional<QUrl>& own)
{
    const auto judge = [this, &own](const QUrl& at, const KvmProbe::Found& found) -> std::optional<base::Error> {
        if (at.scheme() != "https"_L1) {
            return std::nullopt;
        }
        // (Its own HTTPS, of the address it has: what shows another there holds it back. Any other: refused alone.)
        const QUrl from = own && *own == at ? QUrl(m_before.address) : QUrl();
        return m_wall.judgeItsHttps(m_before.id, from, found);
    };
    if (std::optional<base::Error> refusal = judge(ways.address, ways.atAddress)) {
        return refusal;
    }
    return ways.twin ? judge(*ways.twin, ways.atTwin) : std::nullopt;
}

void KvmEditing::askedOverHttps(const QUrl& at, const KvmProbe::Found& found, bool moved, bool chosen)
{
    // Its own HTTPS, looked at before its password goes anywhere: what that look saw is judged first, whatever it
    // answered and as whatever family (the wall's one judgement, from the outside review, F1). What shows the
    // certificate another KVM of the wall is known by holds it back for that one, on the wall and for the next start,
    // and a look in doubt keeps a hold: a later look that sees nothing does not undo what this one saw. (The way chosen
    // of both was judged where both were asked.)
    if (!chosen) {
        if (const std::optional<base::Error> refusal =
                m_wall.judgeItsHttps(m_before.id, QUrl(m_before.address), found)) {
            finish(*refusal);
            return;
        }
    }
    if (found.kind != KvmProbe::Kind::JetKvm || found.refused) {
        if (chosen) {
            // The way chosen of both, and no JetKVM there: an address it is moved to, asked as any is (probed:
            // what is of another family is said so, and hears nothing).
            probed(found);
            return;
        }
        // No JetKVM over HTTPS there: on as where it is (or as where it was typed to go).
        m_overHttpsAsked = true;
        signInWhereItIs(moved);
        return;
    }
    // Over HTTPS at its own host, known by what it showed there on this first contact (as a KVM that
    // is added is known: there is nothing it was known by over plain HTTP to put beside it).
    OwnKvm onto = m_after;
    onto.address = at.toString(QUrl::FullyEncoded);
    kvm::knowByWhatItShowed(onto, at, found);
    // (By what it showed there, whoever vouches for that: from the third reading of the twins.)
    m_shown = found.shown;
    if (const std::optional<base::Error> refusal = kvm::inTheWay(besides(m_wall, m_before.id), onto, m_shown)) {
        finish(*refusal);
        return;
    }
    m_after = onto;
    m_address = at;
    m_overHttps = found;
    m_knownAnew = true;
    const DriverPaces paces = m_pace.drivers();
    if (m_password.isEmpty() && !m_typedNone) {
        // With the password that is kept: the wall makes the driver, and lets that password go onto the
        // KVM's own HTTPS only by its own rule (LocalWall::signerIn, ontoHttps), to what shows what showed
        // there; and not to a KVM that was not asked yet what it is.
        m_keptElsewhere = true;
        base::Result<std::unique_ptr<OneSignIn>> signer = m_wall.signerIn(m_before.id, at, m_after.user, paces, found);
        if (!signer) {
            finish(signer.error());
            return;
        }
        m_device = std::move(signer).value();
    } else {
        // One sign-in there, by a JetKVM's driver held to that: what shows another certificate by now
        // hears nothing. (With the password typed; or none, where it has none kept: m_typedNone.)
        const OneSignIn::Where where{m_address, m_after.user, m_after.knownByCertificate, m_after.certificate,
                                     QString()};
        base::Result<std::unique_ptr<OneSignIn>> made = signInWithWhatWasTyped(where, paces);
        if (!made) {
            finish(made.error());
            return;
        }
        m_device = std::move(made).value();
    }
    heedItsSignIn();
    m_device->identify([this, turn = m_turn](const base::Result<OneSignIn::Said>& who) {
        if (turn == m_turn) {
            identified(who);
        }
    });
}

void KvmEditing::probed(const KvmProbe::Found& found)
{
    // Before any password goes anywhere: only what answered as a KVM of its family is signed in to.
    // A KVM of the other family that this program speaks to is another KVM, and said to be.
    if (const std::optional<KvmProbe::Kind> family = kvm::foundAs(m_before.kind); !family || found.kind != *family) {
        const bool ofAnother = family && !kvm::familyOf(found.kind).isEmpty();
        // (Where the new address is the place this KVM is at already, http and https of one host, what answers
        // there can only be added once this KVM is gone from it: said as on its card.)
        const kvm::AskedAt where =
            LocalWall::atOnePlace(m_before, m_after) ? kvm::AskedAt::ItsAddress : kvm::AskedAt::ANewAddress;
        finish(ofAnother ? kvm::ofAnotherFamily(found.kind, m_before.kind, where) : kvm::whyNotSignedInTo(found.kind));
        return;
    }
    // What it would be known by at this address, found anew: what it shows there, or Windows'
    // word, or nothing where it is spoken to in the clear.
    kvm::knowByWhatItShowed(m_after, m_address, found);
    const bool encrypted = m_address.scheme() == "https"_L1;
    // What shows the very certificate the KVM is known by is that KVM, reached another way: it
    // stays known by that one, also where Windows would vouch for it there.
    if (encrypted && m_before.knownByCertificate && !found.shown.isEmpty() && found.shown == m_before.certificate) {
        m_after = knownBy(std::move(m_after), m_before.certificate);
    }
    m_knownAnew = true;
    // What shows the certificate another KVM of the wall is known by is that KVM, under this
    // address: said before a password goes out to it a second way, as when adding. (Two entries
    // for one KVM would be two drivers, and "one thing at a time" is kept by each for itself.)
    // Also where Windows vouches for it there (from the third reading of the twins).
    m_shown = found.shown;
    if (const std::optional<base::Error> refusal = kvm::inTheWay(besides(m_wall, m_before.id), m_after, m_shown)) {
        finish(*refusal);
        return;
    }
    if (encrypted) {
        // As it is known now: by the same certificate, or by Windows' word here and there. Known
        // otherwise there, the password that was typed stays here until somebody has looked at
        // both and said yes to this very thing: this certificate, to be known by it, or by
        // Windows' word for it. (The server forgets the certificate with the address, and
        // knows the device by what it shows there. In the clear nothing is shown: step 4.)
        const bool asNow = QUrl(m_before.address).scheme() == "https"_L1 &&
                           m_after.knownByCertificate == m_before.knownByCertificate &&
                           m_after.certificate == m_before.certificate;
        const bool onWindowsWord = !m_after.knownByCertificate;
        const bool saidYes =
            !m_saidYesTo.isEmpty() && m_saidYesTo == found.shown && m_saidYesOnWindowsWord == onWindowsWord;
        if (!asNow && !saidYes) {
            if (found.shown.isEmpty()) {
                // (No line that Qt makes: there is nothing to put before anybody.)
                finish(unreachable());
                return;
            }
            const QByteArray remembered = m_before.knownByCertificate ? m_before.certificate : QByteArray();
            const bool twoDiffer = !remembered.isEmpty() && !onWindowsWord;
            Outcome differs{twoDiffer ? certificateDiffers() : knownOtherwiseThere(), remembered, found.shown,
                            m_address.authority(QUrl::FullyEncoded), onWindowsWord};
            finish(std::move(differs));
            return;
        }
        m_heldToAYes = !asNow;
    }
    // One sign-in, by a driver of its family of its own: on lines of its own, none of them the
    // probe's, and each looked at for the certificate before a byte goes out on it. So what was
    // said yes to is all that hears the password: something else that answers by now hears nothing.
    const OneSignIn::Where where{m_address, m_after.user, m_after.knownByCertificate, m_after.certificate, QString()};
    base::Result<std::unique_ptr<OneSignIn>> made = signInWithWhatWasTyped(where, m_pace.drivers());
    if (!made) {
        finish(made.error());
        return;
    }
    m_device = std::move(made).value();
    heedItsSignIn();
    m_device->identify([this, turn = m_turn](const base::Result<OneSignIn::Said>& who) {
        if (turn == m_turn) {
            identified(who);
        }
    });
}

base::Result<std::unique_ptr<OneSignIn>> KvmEditing::signInWithWhatWasTyped(const OneSignIn::Where& where,
                                                                            const DriverPaces& paces) const
{
    // (A text of it for the driver's making, which takes a copy of its own: written over here once it has.)
    QString typed = m_password.view().toString();
    const WipedOnLeaving overTyped(typed);
    // (Typed just now: tried as for the first time, whatever was refused before.)
    OneSignIn::tryAfresh(m_before.kind, where, typed, paces);
    // A JetKVM over plain HTTP has its HTTPS looked at on that very sign-in, as the wall's own driver has: where that
    // shows the certificate another KVM of the wall is known by, whatever it answers, that one may be what answers the
    // plain way too, and nothing goes to it (from the third reading of the twins, MEDIUM 1). Over HTTPS the line itself
    // is held to what the KVM is known by.
    // And which JetKVM answers (J5, the web's connect and its `take`): whoever types the password says "this is it",
    // and the one that answers now is taken (none that could not be read); with none typed (one that runs without),
    // what answers is held to the number the card is known by. (A server's controller likewise: with its password
    // typed for this address, the server that answers is taken, the web's `take`.)
    OneSignIn::Where heldTo = where;
    heldTo.machine = m_password.isEmpty() ? LocalWall::markHeldTo(m_before) : QString();
    return OneSignIn::of(m_before.kind, heldTo, typed, paces, m_wall.twinCheckFor(m_before.id));
}

void KvmEditing::askedAsKnown(const KvmProbe::Found& found)
{
    if (const std::optional<KvmProbe::Kind> family = kvm::foundAs(m_before.kind);
        family && found.kind == *family && !found.refused) {
        // The KVM, by the certificate it is known by, at its new address: saved there, with
        // nothing sent to it but the three questions.
        save(false);
        return;
    }
    // Not the KVM, or nothing: in the wall's words for what an asking found. Where it is the
    // certificate shown there that is not the KVM's, a password that is typed gets over it (it
    // goes there once somebody has seen that certificate beside the one it is known by): said
    // in the server's sentence for that, as where the kept password was tried at a new address.
    const base::Error why = LocalWall::notTheKvm(found, m_before.kind, kvm::AskedAt::ANewAddress);
    finish(typingGetsOver(why) ? notCheckedThere(why) : why);
}

void KvmEditing::heedItsSignIn()
{
    // (Said for this change alone: what one given up meanwhile would say reaches nothing.)
    connect(m_device.get(), &OneSignIn::waitsForAnotherSignIn, this, [this, turn = m_turn] {
        if (turn == m_turn) {
            emit signInWaits();
        }
    });
}

void KvmEditing::identified(const base::Result<OneSignIn::Said>& who)
{
    if (!who) {
        // Its lines were held to a certificate somebody had just said yes to, and what answered
        // on them showed another: said as a yes to a certificate says it. (The driver's own
        // sentence speaks of the one "saved for this device", which this was held to nowhere.)
        if (m_heldToAYes && who.error().code == "cert_mismatch") {
            finish(showsAnotherNow());
            return;
        }
        // What the driver says, as it says it. Only where the kept password was tried at a new
        // address, and a typed one would do: the server's sentence, which says so.
        const bool typeIt = m_keptElsewhere && typingGetsOver(who.error());
        finish(typeIt ? notCheckedThere(who.error()) : who.error());
        return;
    }
    // A server's: the mark its server is known by after this sign-in. Kept only where the password was typed for it
    // (LocalWall::change): whoever types it says "this is the server", and with the kept one it was asked by the mark
    // kept, which stays.
    if (m_before.kind == "redfish"_L1) {
        m_after.machine = who.value().machine;
        if (const std::optional<QString>& number = who.value().number) {
            m_after.number = *number;
        }
    }
    // A JetKVM's number, as it gives it now (none, where it gives none fit to be one: as the web keeps it).
    if (m_before.kind == "jetkvm"_L1) {
        // (A JetKVM's identity always has its number, or the none it gives: one that could not be read is refused
        // before this, by JetKvmDevice::identify, J5.)
        m_after.number = who.value().number.value_or(QString());
        // Where it says by its number that it is another KVM of the wall, and the person said to keep both: kept
        // beside it, as when it is added (KvmAdding::Typed::keepBoth).
        if (m_keepBoth) {
            OwnKvm plain = m_after;
            plain.keptBesideItsTwin = false;
            if (const std::optional<OwnKvm> twin = LocalWall::twinOf(besides(m_wall, m_before.id), plain);
                twin && !LocalWall::sameCertificate(*twin, plain)) {
                m_after.keptBesideItsTwin = true;
            }
        }
    }
    save(true);
}

void KvmEditing::save(bool signedIn)
{
    // The KVM may have been spoken to for a while, and the wall may have changed meanwhile: what
    // was looked at before that is looked at again. A change made in another box is not saved
    // over with what this one read before it.
    const QString id = m_before.id;
    const std::optional<OwnKvm> now = kvmOf(m_wall, id);
    if (!now) {
        finish(unknown());
        return;
    }
    // (Its password too: one that another box saved meanwhile leaves the KVM in the list as it
    // stood, and is not the one this was signed in with. What the wall recorded by itself
    // meanwhile, that it has asked what the KVM is, is no such change.)
    if (!LocalWall::isStillTheOne(*now, m_before) || m_wall.secretWrites(id) != m_secretWrites) {
        finish(changedMeanwhile());
        return;
    }
    if (m_knownAnew || m_after.number != now->number || m_after.keptBesideItsTwin != now->keptBesideItsTwin) {
        // And the KVM it turned out to be may have come onto the wall meanwhile; or it says, by the number it
        // gives, that it is another of the wall.
        if (const std::optional<base::Error> refusal = kvm::inTheWay(besides(m_wall, id), m_after, m_shown)) {
            finish(*refusal);
            return;
        }
    }
    Changed changed;
    changed.id = id;
    changed.name = m_after.name.isEmpty() ? LocalWall::hostOf(m_after) : m_after.name;
    changed.signedIn = signedIn;
    changed.overPlainHttp = m_address.scheme() == "http"_L1;
    changed.certificate = m_after.knownByCertificate ? m_after.certificate : QByteArray();
    changed.ontoHttps = m_overHttps.has_value();
    // The wall says aloud that it has changed while it saves, and whoever hears that may end this
    // there and then. So the driver is its own before that, the password is out of here, and
    // nothing of this is touched after it unless this is still there.
    letTheDriverGo();
    // (A text of it for the wall, which keeps what it keeps of it: written over here once the wall has it.)
    QString typed = m_password.view().toString();
    const WipedOnLeaving overTyped(typed);
    m_password = SecretText();
    const QPointer<KvmEditing> here(this);
    // (And from here on it is not given up: it is on the wall in a moment. Whoever ends this at
    // the wall's word finds here that it is: the wall says that word last of all, and only of
    // what it has saved.)
    m_over = true;
    m_cameTo = Outcome{changed, {}, {}, {}};
    // (A JetKVM that is moved with no password, having none, is saved with none: so given.)
    const std::optional<QString> given = typed.isEmpty() && !m_typedNone ? std::nullopt : std::optional<QString>(typed);
    const base::Result<void> kept = m_wall.change(id, m_after, given, m_overHttps);
    if (!here) {
        return;
    }
    if (!kept) {
        finish(kept.error());
        return;
    }
    finish(std::move(changed));
}

void KvmEditing::letTheDriverGo()
{
    // The driver that signed in was made for this, and goes with it: a turn of the loop later,
    // since it may be in the middle of handing over what it found.
    if (OneSignIn* over = m_device.release()) {
        over->deleteLater();
    }
}

void KvmEditing::giveUp()
{
    if (m_underWay && !m_over) {
        finish(changeGivenUp());
    }
}

void KvmEditing::finish(base::Result<Changed> result)
{
    finish(Outcome{std::move(result), {}, {}, {}});
}

void KvmEditing::finish(Outcome outcome)
{
    // Over: whatever of it is still to answer, answers nobody.
    m_over = true;
    ++m_turn;
    // Nothing of what was typed is kept here any longer, the password least of all.
    letTheDriverGo();
    m_password = SecretText();
    m_saidYesTo.clear();
    m_saidYesOnWindowsWord = false;
    m_heldToAYes = false;
    m_before = OwnKvm{};
    m_after = OwnKvm{};
    m_address.clear();
    m_keptElsewhere = false;
    m_knownAnew = false;
    m_typed.clear();
    m_saidYesToPlainHttpAt.clear();
    m_overHttpsAsked = false;
    m_typedNone = false;
    m_keepBoth = false;
    m_ways = kvm::Ways{};
    m_waysOut = 0;
    m_overHttps.reset();
    m_shown.clear();
    m_secretWrites = 0;
    // Always a turn of the loop later, whichever way it came about, and not at all once this
    // is gone. Until then it is kept here, for whoever ends this before that (cameTo()).
    m_cameTo = std::move(outcome);
    later([this] {
        // Over before it is said, so that whoever is told may begin the next.
        m_underWay = false;
        const std::function<void(Outcome)> done = std::exchange(m_done, nullptr);
        const std::optional<Outcome> told = std::exchange(m_cameTo, std::nullopt);
        done(*told);
    });
}

KvmCertificateReview::KvmCertificateReview(LocalWall& wall, QObject* parent)
    : KvmCertificateReview(wall, KvmProbe::Pace{}, parent)
{}

KvmCertificateReview::KvmCertificateReview(LocalWall& wall, KvmProbe::Pace pace, QObject* parent)
    : QObject(parent), m_wall(wall), m_pace(pace)
{}

KvmCertificateReview::~KvmCertificateReview() = default;

void KvmCertificateReview::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

void KvmCertificateReview::giveUp()
{
    if (m_underWay && !m_over) {
        // (Its probe stays where it is, and is not ended in the middle of its work: what it
        // still hears is nobody's.)
        const std::function<void(const base::Error&)> refuse = std::exchange(m_refuse, nullptr);
        refuse(reviewGivenUp());
    }
}

void KvmCertificateReview::finish(std::function<void()> tell)
{
    // Over: whatever of it is still to answer, answers nobody.
    m_over = true;
    ++m_turn;
    m_refuse = nullptr;
    // Always a turn of the loop later, whichever way it came about, and not at all once this
    // is gone.
    later([this, tell = std::move(tell)] {
        // Over before it is said, so that whoever is told may ask the next.
        m_underWay = false;
        tell();
    });
}

void KvmCertificateReview::read(const QString& id, const std::optional<KvmEditing::Seen>& seen,
                                std::function<void(const base::Error&)> refuse,
                                std::function<void(const OwnKvm&, const QByteArray&)> then)
{
    const std::optional<OwnKvm> known = kvmOf(m_wall, id);
    if (!known) {
        refuse(unknown());
        return;
    }
    // What would stand beside the two is the KVM as somebody saw it a while ago: where it is
    // not that any more, nothing is asked of it. (Its password too, as where one is changed.)
    const auto isAsSeen = [this, id, seen](const OwnKvm& kvm) {
        return !seen || (LocalWall::isStillTheOne(kvm, seen->kvm) && seen->secretWrites == m_wall.secretWrites(id));
    };
    if (!isAsSeen(*known)) {
        refuse(changedMeanwhile());
        return;
    }
    const QUrl address(known->address);
    if (address.scheme() != "https"_L1) {
        // The plain way shows no certificate, and a probe would only find that nothing does.
        refuse(noCertificate());
        return;
    }
    // A probe of its own for this one reading: on lines of its own, with nothing in its questions
    // that says who asks, and with what was shown told as it is, whoever vouches for it or nobody.
    // (The one before it, where that was given up, ends here.)
    m_refuse = refuse;
    m_probe = std::make_unique<KvmProbe>(m_pace);
    m_probe->ask(address, [this, turn = m_turn, id, asked = known->address, isAsSeen, refuse = std::move(refuse),
                           then = std::move(then)](const KvmProbe::Found& found) {
        if (turn != m_turn) {
            return;
        }
        // The probe is its own from here on, and goes a turn of the loop later: it is in the
        // middle of handing this over, and what follows may end with the wall saying aloud that
        // it has changed, which whoever hears may take as the end of this.
        m_probe.release()->deleteLater();
        // It was asked for a while, and the wall may have changed meanwhile: what was read is the
        // certificate of what is at the address that was asked, and of no other.
        const std::optional<OwnKvm> now = kvmOf(m_wall, id);
        if (!now) {
            refuse(unknown());
        } else if (now->address != asked || !isAsSeen(*now)) {
            refuse(changedMeanwhile());
        } else if (found.kind == KvmProbe::Kind::NotAsked) {
            // Nothing was asked there: this copy of the program speaks to nothing over HTTPS.
            refuse(damagedTls());
        } else if (found.shown.isEmpty()) {
            // Nothing answered there, or what did showed nothing: as the driver says it.
            refuse(unreachable());
        } else {
            then(*now, found.shown);
        }
    });
}

KvmCertificateReview::Shown KvmCertificateReview::theTwo(const OwnKvm& kvm, const QByteArray& presented) const
{
    Shown shown;
    shown.remembered = kvm.knownByCertificate ? kvm.certificate : QByteArray();
    shown.presented = presented;
    // What shows the certificate another KVM of the wall is known by is that KVM, at this one's
    // address: said with the two, before anybody compares them.
    const QList<OwnKvm> others = besides(m_wall, kvm.id);
    // (What shows that certificate: the certificate alone, and not the number a KVM gives, LocalWall::twinOf.)
    const OwnKvm byIt = knownBy(kvm, presented);
    if (const auto twin = std::ranges::find_if(
            others, [&byIt](const OwnKvm& other) { return LocalWall::sameCertificate(other, byIt); });
        twin != others.end()) {
        shown.presentedIsOf = twin->id;
        shown.presentedIsCalled = twin->name.isEmpty() ? LocalWall::hostOf(*twin) : twin->name;
    }
    return shown;
}

void KvmCertificateReview::askCertificates(const QString& id, std::function<void(base::Result<Shown>)> done,
                                           const std::optional<KvmEditing::Seen>& seen)
{
    if (m_underWay) {
        later([done = std::move(done)] { done(underReview()); });
        return;
    }
    m_underWay = true;
    m_over = false;
    const auto tell = std::make_shared<std::function<void(base::Result<Shown>)>>(std::move(done));
    read(
        id, seen, [this, tell](const base::Error& why) { finish([tell, why = base::Error(why)] { (*tell)(why); }); },
        [this, tell](const OwnKvm& kvm, const QByteArray& presented) {
            finish([tell, shown = theTwo(kvm, presented)] { (*tell)(shown); });
        });
}

void KvmCertificateReview::finishYes(Outcome outcome, std::shared_ptr<std::function<void(Outcome)>> tell)
{
    m_cameTo = std::move(outcome);
    finish([this, tell = std::move(tell)] {
        // Out of here before it is said: whoever is told may ask the next there and then.
        const std::optional<Outcome> told = std::exchange(m_cameTo, std::nullopt);
        (*tell)(*told);
    });
}

void KvmCertificateReview::trust(const QString& id, const QByteArray& presentedAsShown,
                                 std::function<void(Outcome)> done, const std::optional<KvmEditing::Seen>& seen)
{
    if (m_underWay) {
        later([done = std::move(done)] { done(Outcome{underReview(), {}}); });
        return;
    }
    m_underWay = true;
    m_over = false;
    const auto tell = std::make_shared<std::function<void(Outcome)>>(std::move(done));
    read(
        id, seen, [this, tell](const base::Error& why) { finishYes(Outcome{why, {}}, tell); },
        [this, id, presentedAsShown, tell](const OwnKvm& kvm, const QByteArray& presented) {
            if (presented != presentedAsShown) {
                // Not the one somebody looked at: nobody has said yes to this one. It is handed
                // over to be shown, in the place of the one that was.
                finishYes(Outcome{showsAnotherNow(), theTwo(kvm, presented)}, tell);
                return;
            }
            // What shows the certificate another KVM of the wall is known by is that KVM, at
            // this one's address (two change places when their addresses are handed out anew):
            // this one's kept password does not go to it, and two entries do not hold one KVM.
            // Whoever compared found the two the same, since it is that KVM; so it is refused
            // here, in the sentence adding has for it, and by the wall once more.
            if (const std::optional<base::Error> refusal =
                    kvm::inTheWay(besides(m_wall, id), knownBy(kvm, presented))) {
                finishYes(Outcome{*refusal, {}}, tell);
                return;
            }
            // The wall says aloud that it has changed while it remembers, and whoever hears that
            // may end this there and then: nothing of this is touched after it unless this is
            // still there.
            const QPointer<KvmCertificateReview> here(this);
            // (And from here on it is not given up: the certificate is the KVM's in a moment.
            // Whoever ends this at the wall's word finds here that it is: the wall says that
            // word last of all, and only of what it has remembered.)
            m_over = true;
            m_cameTo = Outcome{base::Result<void>(), {}};
            const base::Result<void> kept = m_wall.knowBy(id, presented);
            if (!here) {
                return;
            }
            finishYes(Outcome{kept, {}}, tell);
        });
}

} // namespace qkvm::core
