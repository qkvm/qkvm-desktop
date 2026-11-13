#include "own_wall.h"
#include "app_lock.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QStringList>
#include <QUrl>

#include <qkvm/core/server_client.h>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// The one that was to tell these boxes to wait was ended before it had: nobody tells them now,
// and nothing came of what they asked.
void nothingCameOfItFor(const QList<QPointer<EditKvmView>>& boxes)
{
    for (const QPointer<EditKvmView>& box : boxes) {
        if (box != nullptr) {
            box->gaveUp();
        }
    }
}

// Why not, as a box shows it: the sentence it comes with, whoever said it.
QString sentenceOf(const base::Error& why)
{
    return QString::fromStdString(why.message);
}

// What a KVM is known by: its certificate's SHA-256, or nothing where it is known by none.
QByteArray knownBy(const core::OwnKvm& kvm)
{
    return kvm.knownByCertificate ? kvm.certificate : QByteArray();
}

bool isPlain(const core::OwnKvm& kvm)
{
    return QUrl(kvm.address).scheme() == "http"_L1;
}

// Whether a saving left what a KVM is known by, or how it is spoken to, another than its box
// showed: that is to be seen where it was saved.
bool isToBeSeen(const core::KvmEditing::Changed& changed, const core::OwnKvm& shown)
{
    // (And that it answers over HTTPS now, where it was spoken to the plain way: a JetKVM.)
    return changed.certificate != knownBy(shown) || (changed.overPlainHttp && !isPlain(shown)) || changed.ontoHttps;
}

// Whether an address as it was typed is the one something was offered for: that host and port,
// over HTTPS where a certificate was shown there, and over plain HTTP where it is that way of
// speaking to it that was offered.
bool isTypedFor(const QString& typed, const EditKvmView::Offer& offered)
{
    const base::Result<QUrl> address = core::kvm::addressFrom(typed);
    return address && address.value().scheme() == (offered.plainHttp ? "http"_L1 : "https"_L1) &&
           address.value().authority(QUrl::FullyEncoded) == offered.at;
}

} // namespace

OwnWall::OwnWall(core::Settings& settings, core::SecretStore& secrets, QObject* parent)
    : OwnWall(settings, secrets, core::KvmAdding::Pace{}, parent)
{}

OwnWall::OwnWall(core::Settings& settings, core::SecretStore& secrets, const core::KvmAdding::Pace& pace,
                 QObject* parent)
    : QObject(parent), m_wall(settings, secrets, pace.drivers()), m_adding(m_wall, pace), m_pace(pace),
      m_editing(std::make_unique<core::KvmEditing>(m_wall, pace)),
      m_review(std::make_unique<core::KvmCertificateReview>(m_wall, pace.probe))
{
    connect(&m_wall, &core::LocalWall::changed, this, &OwnWall::followTheWallInTheBoxes);
    // The saving under way waits for an earlier sign-in to its KVM: its box says so.
    connect(m_editing.get(), &core::KvmEditing::signInWaits, this, [this] {
        for (const QPointer<EditKvmView>& box : std::as_const(m_editBoxes)) {
            if (box != nullptr && box.data() == m_changing.underWayFor) {
                box->signInWaits();
            }
        }
    });
}

void OwnWall::letGoOfEverything()
{
    m_adding.giveUp();
    if (m_editing != nullptr) {
        m_editing->giveUp();
    }
    m_wall.letGoOfEveryDriver();
}

void OwnWall::followTheWallInTheBoxes()
{
    m_editBoxes.removeIf([](const QPointer<EditKvmView>& box) { return box == nullptr; });
    // (Each of them as they are now: one that is told may close, and its window with it.)
    const QList<QPointer<EditKvmView>> boxes = m_editBoxes;
    for (const QPointer<EditKvmView>& box : boxes) {
        if (box == nullptr || box->machine().isEmpty()) {
            continue;
        }
        if (const std::optional<core::KvmEditing::Seen> now = seenNow(box->machine())) {
            const EditKvmView::Shown shown = shownOf(now->kvm);
            box->renamed(shown.called, shown.inASentence);
        } else {
            box->wentAway();
        }
    }
}

void OwnWall::serve(AddKvmView& view)
{
    m_addBoxes.append(&view);
    const QPointer<AddKvmView> box(&view);
    connect(&view, &AddKvmView::addAsked, this,
            [this, box](const QString& address, const QString& user, const QString& password, const QString& group,
                        bool keepBoth) {
                if (box == nullptr || !mayChange()) {
                    return;
                }
                core::KvmAdding::Typed typed;
                typed.address = address;
                typed.user = user;
                typed.password = password;
                typed.group = group;
                typed.keepBoth = keepBoth;
                // Whose KVM is being asked, where it is this box's: another box's stays that one's,
                // and this one is told to wait (the adding says so itself).
                const bool isThisOnes = !m_adding.isUnderWay();
                if (isThisOnes) {
                    m_asking = box.data();
                }
                box->began();
                m_adding.add(typed, [this, box, isThisOnes](const base::Result<core::KvmAdding::Added>& added) {
                    if (isThisOnes) {
                        m_asking = nullptr;
                    }
                    if (box == nullptr) {
                        return;
                    }
                    if (added) {
                        box->took(saidOf(added.value(), calledInASentence(added.value().id, added.value().name)),
                                  core::ServerClient::written(added.value().certificate));
                    } else if (added.error().code == "given_up") {
                        box->gaveUp();
                    } else {
                        // (Where it says by its number that it is a KVM of the wall, the person may keep both.)
                        box->refused(QString::fromStdString(added.error().message),
                                     core::LocalWall::saysItIsTheSame(added.error()));
                    }
                });
            });
    connect(&view, &AddKvmView::giveUpAsked, this,
            [this, which = static_cast<const QObject*>(&view)] { giveUpFor(which); });
    // A window that is closed over its box: nobody is there to be told, and nothing is added
    // behind anybody's back.
    connect(&view, &QObject::destroyed, this, [this](const QObject* gone) { giveUpFor(gone); });
}

void OwnWall::giveUpFor(const QObject* box)
{
    if (m_asking != nullptr && m_asking == box) {
        m_adding.giveUp();
    }
}

bool OwnWall::Asked::begin(bool isUnderWay, EditKvmView& box)
{
    if (isUnderWay) {
        toldToWait.append(&box);
        return false;
    }
    underWayFor = &box;
    return true;
}

bool OwnWall::Asked::end(bool wasThisOnes, const QPointer<EditKvmView>& box)
{
    if (wasThisOnes) {
        // (What a box that was closed had asked was ended with the one that did it, and has no
        // answer; a box that gave it up and stays is answered, that it was given up. So this
        // is still that box's.)
        underWayFor = nullptr;
        return box != nullptr;
    }
    return toldToWait.removeOne(box) && box != nullptr;
}

bool OwnWall::Asked::letGo(const QObject* box)
{
    // (Boxes that are gone stand in the list as nothing, and go out of it here.)
    toldToWait.removeIf([box](const QPointer<EditKvmView>& one) { return one == nullptr || one.data() == box; });
    if (underWayFor == nullptr || underWayFor != box) {
        return false;
    }
    underWayFor = nullptr;
    return true;
}

void OwnWall::serve(EditKvmView& view)
{
    const QPointer<EditKvmView> box(&view);
    m_editBoxes.append(box);
    connect(&view, &EditKvmView::openAsked, this, [this, box](const QString& machine, bool withReview) {
        if (box != nullptr) {
            openFor(*box, machine, withReview);
        }
    });
    connect(&view, &EditKvmView::saveAsked, this,
            [this, box](const QString& machine, const QString& name, const QString& address, const QString& user,
                        const QString& password, const QString& group, const QByteArray& saidYesTo, bool keepBoth) {
                // Of the KVM this box was filled with, whatever the wish names, as a look at a
                // certificate and the yes to it are: a saving for another is nobody's, and fills
                // this box with nothing.
                if (box == nullptr || !mayChange() || !filledWith(box.data(), machine)) {
                    return;
                }
                core::KvmEditing::Typed typed;
                typed.name = name;
                typed.address = address;
                typed.user = user;
                typed.password = password;
                typed.group = group;
                typed.keepBoth = keepBoth;
                // Its screen watches, as the box had them when it was saved (c7).
                if (const std::optional<std::pair<int, int>> watches = box->typedWatches()) {
                    typed.noSignalMinutes = watches->first;
                    typed.unchangedMinutes = watches->second;
                }
                // A yes is one only to what this box was told to show, for this KVM and at the
                // address that is typed now; whatever certificate came with the wish. (Whatever
                // was offered is offered no longer: to this saving, or to none.)
                const std::optional<Offered> offered = takeOfferedTo(*box);
                const bool saidYes = offered && !offered->at.isEmpty() && offered->machine == machine &&
                                     offered->certificate == saidYesTo && isTypedFor(address, *offered);
                if (saidYes && offered->plainHttp) {
                    typed.saidYesToPlainHttpAt = offered->at;
                } else if (saidYes) {
                    typed.saidYesTo = offered->certificate;
                    typed.saidYesOnWindowsWord = offered->onWindowsWord;
                }
                // Typed over the KVM as this box was filled with it. (A box that was filled
                // with nothing has seen no KVM: none is what it was, and nothing is saved.)
                const core::KvmEditing::Seen seen = m_seen.value(box.data());
                typed.seen = seen;
                // Whose change is under way, where it is this box's: another box's stays that
                // one's, and this one is told to wait (the editing says so itself).
                const bool isThisOnes = m_changing.begin(m_editing->isUnderWay(), *box);
                box->began(u"saving"_s);
                m_editing->change(machine, std::move(typed),
                                  [this, box, isThisOnes, machine, seen,
                                   yes = saidYes ? offered : std::nullopt](const core::KvmEditing::Outcome& outcome) {
                                      if (!m_changing.end(isThisOnes, box)) {
                                          return;
                                      }
                                      const std::optional<core::KvmEditing::Seen> now = seenNow(machine);
                                      if (outcome.result) {
                                          // What it is known by now, or that it is spoken to in the clear now,
                                          // is shown where it is another than this box showed: the box stays,
                                          // on the KVM as it is. Otherwise it closes.
                                          const core::KvmEditing::Changed& changed = outcome.result.value();
                                          if (isToBeSeen(changed, seen.kvm) && now) {
                                              m_seen.insert(box.data(), *now);
                                              box->savedAndStays(shownOf(now->kvm), changed.ontoHttps);
                                          } else {
                                              box->saved(calledInASentence(machine, changed.name));
                                          }
                                      } else if (outcome.result.error().code == "certificate_differs") {
                                          // What the box shows from here on is what a yes can be given to.
                                          box->differing(outcome.remembered, outcome.presented, outcome.presentedAt,
                                                         outcome.onWindowsWord);
                                          // (Over HTTPS at that address, as it is typed: the yes goes with the
                                          // address that was asked, which a JetKVM's two ways may have made
                                          // another than the one typed.)
                                          box->retype(outcome.presentedAt);
                                          keepAsOffered(*box, Offered{machine, outcome.presented, outcome.presentedAt,
                                                                      outcome.onWindowsWord, false});
                                      } else if (outcome.result.error().code == "plain_http_there") {
                                          // So where it is plain HTTP at that address that a yes is given to.
                                          box->givingUpHttps(outcome.remembered, outcome.presentedAt);
                                          // (And the plain way at that address, as it is typed.)
                                          box->retype(u"http://"_s + outcome.presentedAt);
                                          keepAsOffered(
                                              *box, Offered{machine, QByteArray(), outcome.presentedAt, false, true});
                                      } else if (outcome.result.error().code == "given_up") {
                                          // (The box stays on what it showed: a yes can be given to that again.)
                                          box->gaveUp();
                                          if (yes) {
                                              keepAsOffered(*box, *yes);
                                          }
                                      } else if (!fillAnewOver(*box, machine, outcome.result.error())) {
                                          box->refused(sentenceOf(outcome.result.error()),
                                                       core::LocalWall::saysItIsTheSame(outcome.result.error()));
                                      }
                                  });
            });
    connect(&view, &EditKvmView::removeAsked, this, [this, box](const QString& machine) {
        if (box == nullptr || !mayChange()) {
            return;
        }
        const QString called = box->called();
        box->began(u"removing"_s);
        // (The wall says aloud that it has changed while it takes the KVM off, and whoever hears
        // that may close the box, or end its window.)
        const base::Result<void> taken = m_wall.remove(machine);
        if (box == nullptr) {
            return;
        }
        if (taken) {
            box->removed(called);
        } else {
            box->refused(sentenceOf(taken.error()));
        }
    });
    connect(&view, &EditKvmView::reviewAsked, this, [this, box](const QString& machine) {
        if (box == nullptr || !mayChange()) {
            return;
        }
        // Of the KVM this box was filled with, as it was filled with it, whatever the wish
        // names: what is read stands in the box beside that KVM. A wish for another is nobody's.
        const std::optional<core::KvmEditing::Seen> seen = filledWith(box.data(), machine);
        if (!seen) {
            return;
        }
        // (What was offered before this look is offered no longer.)
        m_offered.remove(box.data());
        const bool isThisOnes = m_looking.begin(m_review->isUnderWay(), *box);
        box->began(u"looking"_s);
        m_review->askCertificates(
            machine,
            [this, box, isThisOnes, machine](const base::Result<core::KvmCertificateReview::Shown>& shown) {
                if (!m_looking.end(isThisOnes, box)) {
                    return;
                }
                if (shown) {
                    showTheTwoTo(*box, machine, shown.value());
                } else if (shown.error().code == "given_up") {
                    box->gaveUp();
                } else if (!fillAnewOver(*box, machine, shown.error())) {
                    box->refused(sentenceOf(shown.error()));
                }
            },
            seen);
    });
    connect(&view, &EditKvmView::trustAsked, this, [this, box](const QString& machine, const QByteArray& presented) {
        if (box == nullptr || !mayChange()) {
            return;
        }
        // A yes is one only to what this box was told to show, for the KVM it was filled with,
        // at the address that one has; and only where the box says that this very thing is
        // what its yes was said to, having been looked at for its moment. Anything else is
        // none: nothing is asked for it, and what is offered stays offered.
        const std::optional<core::KvmEditing::Seen> seen = filledWith(box.data(), machine);
        const auto kept = m_offered.constFind(box.data());
        if (!seen || kept == m_offered.constEnd() || !kept->at.isEmpty() || kept->machine != machine ||
            kept->certificate != presented || box->saysYesTo() != *kept) {
            return;
        }
        const Offered yes = m_offered.take(box.data());
        const bool isThisOnes = m_looking.begin(m_review->isUnderWay(), *box);
        box->began(u"trusting"_s);
        // (Exactly what the box showed: the review reads the KVM once more, and remembers nothing
        // that is not this very one. And over the KVM as the box was filled with it: the yes
        // was said to the one beside the other.)
        m_review->trust(
            machine, yes.certificate,
            [this, box, isThisOnes, machine, yes](const core::KvmCertificateReview::Outcome& came) {
                if (!m_looking.end(isThisOnes, box)) {
                    return;
                }
                if (came.result) {
                    box->trusted(calledInASentence(machine, box->called()));
                } else if (came.result.error().code == "cert_mismatch" && !came.shownNow.presented.isEmpty()) {
                    // Nobody has seen what it shows by now: the box shows that one in the place
                    // of the one the yes was given to, and says so.
                    showTheTwoTo(*box, machine, came.shownNow);
                } else if (came.result.error().code == "given_up") {
                    // (The box stays on what it showed: a yes can be given again.)
                    box->gaveUp();
                    keepAsOffered(*box, yes);
                } else if (!fillAnewOver(*box, machine, came.result.error())) {
                    box->refused(sentenceOf(came.result.error()));
                    keepAsOffered(*box, yes);
                }
            },
            seen);
    });
    connect(&view, &EditKvmView::giveUpAsked, this, [this, box](bool closing) {
        if (box == nullptr) {
            return;
        }
        if (!closing) {
            giveUpChangingFor(box.data());
            return;
        }
        // (Before the one that did it is ended: it knows what it had done already, and would
        // tell nobody.)
        sayWhatWasDoneFor(*box);
        endChangingFor(box.data());
        m_offered.remove(box.data());
    });
    // A window that is closed over its box: nobody is there to be told, and nothing is changed
    // or trusted behind anybody's back.
    connect(&view, &QObject::destroyed, this, [this](const QObject* gone) {
        endChangingFor(gone);
        m_seen.remove(gone);
        m_offered.remove(gone);
    });
}

void OwnWall::showTheTwoTo(EditKvmView& box, const QString& machine, const core::KvmCertificateReview::Shown& shown)
{
    // What the box shows from here on is what a yes can be given to; unless it is the
    // certificate of another KVM of the wall, to which none is taken (the box offers none).
    // (That one is named in a sentence of the box's: as a sentence calls it.)
    const bool isAnothers = !shown.presentedIsOf.isEmpty() || !shown.presentedIsCalled.isEmpty();
    box.reviewed(shown.remembered, shown.presented,
                 isAnothers ? calledInASentence(shown.presentedIsOf, shown.presentedIsCalled) : QString());
    keepAsOffered(box, Offered{machine, shown.presented, QString(), false, false});
}

void OwnWall::keepAsOffered(const EditKvmView& box, const Offered& what)
{
    // What is offered is what is shown. A box that did not take what it was told (it had been
    // told to wait meanwhile, and waits for nothing) shows something else, or nothing: then
    // nothing is offered to it, and not what it shows either, which was offered before this.
    if (box.offers() == what) {
        m_offered.insert(&box, what);
    } else {
        m_offered.remove(&box);
    }
}

std::optional<OwnWall::Offered> OwnWall::takeOfferedTo(const EditKvmView& box)
{
    if (!m_offered.contains(&box)) {
        return std::nullopt;
    }
    Offered offered = m_offered.take(&box);
    // Only where it is the very thing the box says its yes was said to: what stood there when
    // its button was pressed, looked at for its moment. A saving that comes without that word
    // of the box's (its lines' own button; the signal, sent by whoever can) is one without a
    // yes, whatever certificate and address it names.
    return box.saysYesTo() == offered ? std::optional<Offered>(std::move(offered)) : std::nullopt;
}

std::optional<core::KvmEditing::Seen> OwnWall::filledWith(const QObject* box, const QString& machine) const
{
    const auto seen = m_seen.constFind(box);
    return seen != m_seen.constEnd() && seen->kvm.id == machine ? std::optional<core::KvmEditing::Seen>(*seen)
                                                                : std::nullopt;
}

bool OwnWall::fillAnewOver(EditKvmView& box, const QString& machine, const base::Error& why)
{
    const std::optional<core::KvmEditing::Seen> now = seenNow(machine);
    if (why.code != "conflict" || !now) {
        return false;
    }
    // Another window changed it: the box is filled anew with the KVM as it is, and what it
    // asks next is asked over that.
    m_seen.insert(&box, *now);
    box.refusedOver(shownOf(now->kvm), sentenceOf(why) + u' ' +
                                           QCoreApplication::translate(
                                               "OwnWall", "The fields were filled in again with what is saved now."));
    return true;
}

void OwnWall::sayWhatWasDoneFor(EditKvmView& box)
{
    if (m_changing.underWayFor == &box) {
        if (const std::optional<core::KvmEditing::Outcome> cameTo = m_editing->cameTo(); cameTo && cameTo->result) {
            // Saved, and the box not told yet: with what the box would have stayed open to
            // show, where it would have stayed.
            const core::KvmEditing::Changed& changed = cameTo->result.value();
            const bool known = isToBeSeen(changed, m_seen.value(&box).kvm);
            box.savedAsItClosed(calledInASentence(changed.id, changed.name), known,
                                known ? core::kvm::fingerprint(changed.certificate) : QString(),
                                known && changed.overPlainHttp);
        }
    }
    if (m_looking.underWayFor == &box) {
        if (const std::optional<core::KvmCertificateReview::Outcome> cameTo = m_review->cameTo();
            cameTo && cameTo->result) {
            box.trustedAsItClosed(calledInASentence(box.machine(), box.called()));
        }
    }
}

void OwnWall::giveUpChangingFor(const QObject* box)
{
    // The box stays, and waits to hear: the one that does it gives up, and says what came of
    // it. Where it was done in this very turn (the wall has taken it, and the box has not heard
    // yet), that is what the box is told, and not that it was given up. (A box that only waits
    // to be told to wait hears that.)
    if (box != nullptr && m_changing.underWayFor == box) {
        m_editing->giveUp();
    }
    if (box != nullptr && m_looking.underWayFor == box) {
        m_review->giveUp();
    }
}

void OwnWall::endChangingFor(const QObject* box)
{
    // Ended there and then, and not a turn of the loop later: what is given up does nothing
    // more, whatever a KVM answers in between. (What the wall had taken before that stays: the
    // wall has said so.)
    if (m_changing.letGo(box)) {
        m_editing = std::make_unique<core::KvmEditing>(m_wall, m_pace);
        nothingCameOfItFor(std::exchange(m_changing.toldToWait, {}));
    }
    if (m_looking.letGo(box)) {
        m_review = std::make_unique<core::KvmCertificateReview>(m_wall, m_pace.probe);
        nothingCameOfItFor(std::exchange(m_looking.toldToWait, {}));
    }
}

std::optional<core::KvmEditing::Seen> OwnWall::seenNow(const QString& machine) const
{
    const QList<core::OwnKvm>& kvms = m_wall.kvms();
    const auto known = std::ranges::find_if(kvms, [&machine](const core::OwnKvm& one) { return one.id == machine; });
    if (known == kvms.end()) {
        return std::nullopt;
    }
    return core::KvmEditing::Seen{*known, m_wall.secretWrites(machine)};
}

EditKvmView::Shown OwnWall::shownOf(const core::OwnKvm& kvm)
{
    const QUrl address(kvm.address);
    const QString scheme = address.scheme();
    EditKvmView::Shown shown;
    shown.id = kvm.id;
    shown.called = kvm.name.isEmpty() ? core::LocalWall::hostOf(kvm) : kvm.name;
    shown.inASentence = core::LocalWall::calledInASentence(kvm);
    shown.name = kvm.name;
    shown.https = scheme == "https"_L1;
    shown.overPlainHttp = scheme == "http"_L1;
    // As it is typed, and as what is typed is read back (kvm::addressFrom): an address without
    // "http://" before it is one that is spoken to over HTTPS. Its host stands as the address
    // has it, and not as it reads best: a name that is written "xn--" is typed so.
    shown.address = shown.https ? address.authority(QUrl::FullyEncoded) : kvm.address;
    shown.user = kvm.user;
    shown.group = kvm.group;
    shown.family = kvm.kind;
    shown.certificate = kvm.knownByCertificate ? core::kvm::fingerprint(kvm.certificate) : QString();
    // (A server's controller hands out no picture of a KVM's here: nothing to watch.)
    shown.watchable = kvm.kind != "redfish"_L1;
    shown.noSignalMinutes = kvm.noSignalMinutes;
    shown.unchangedMinutes = kvm.unchangedMinutes;
    return shown;
}

void OwnWall::openFor(EditKvmView& view, const QString& machine, bool withReview)
{
    if (!mayChange()) {
        return;
    }
    const std::optional<core::KvmEditing::Seen> now = seenNow(machine);
    if (!now || view.busy()) {
        return;
    }
    // What the box is filled with is what it saves over: kept here, where the screens do not
    // reach it. (And nothing is offered to it for a yes yet.)
    m_seen.insert(&view, *now);
    m_offered.remove(&view);
    const EditKvmView::Shown shown = shownOf(now->kvm);
    view.show(shown, withReview && shown.https);
}

QString OwnWall::saidOfSettingsThatCouldNotBeRead(core::Settings& settings)
{
    if (!settings.wasUnreadable()) {
        return {};
    }
    QString said =
        QCoreApplication::translate("OwnWall", "What this program remembered could not be read, so it starts as new.");
    if (!settings.putAsideWhatCouldNotBeRead()) {
        said += u' ' + QCoreApplication::translate("OwnWall", "The file could not be put aside either, so nothing is "
                                                              "remembered from here on: something else holds it.");
    } else if (!settings.putAside().isEmpty()) {
        // (Empty: somebody moved it away meanwhile, and there is nothing to say of where it is.)
        said += u' ' + QCoreApplication::translate("OwnWall", "The file is kept, beside the new one, as %1.")
                           .arg(QFileInfo(settings.putAside()).fileName());
    }
    return said;
}

QString OwnWall::calledInASentence(const QString& machine, const QString& otherwise) const
{
    const QList<core::OwnKvm>& kvms = m_wall.kvms();
    const auto known = std::ranges::find_if(kvms, [&machine](const core::OwnKvm& one) { return one.id == machine; });
    if (known != kvms.end()) {
        return core::LocalWall::calledInASentence(*known);
    }
    // Not on the wall (any more): what its card called it, set off by the same function.
    const QString setOff = core::exchange::nameSetOff(otherwise);
    return setOff.isEmpty() ? QCoreApplication::translate("OwnWall", "this KVM") : setOff;
}

void OwnWall::serve(OwnFilesView& view)
{
    if (m_lock != nullptr) {
        view.useAppLock(*m_lock);
    }
    // One of its own for each view, kept here for as long as the view is there: what it laid
    // out goes with it, each password written over.
    std::unique_ptr<core::WallExchange>& exchange = m_exchanges[&view];
    exchange = std::make_unique<core::WallExchange>(m_wall, m_sealing);
    view.use(exchange.get());
    connect(&view, &QObject::destroyed, this, [this](const QObject* gone) { m_exchanges.erase(gone); });
}

bool OwnWall::mayChange() const
{
    return m_lock == nullptr || !m_lock->restricted();
}

void OwnWall::useAppLock(AppLock& lock)
{
    m_lock = &lock;
    connect(
        &lock, &AppLock::changed, this,
        [this] {
            if (!mayChange()) {
                m_adding.giveUp();
                for (const QPointer<AddKvmView>& box : m_addBoxes) {
                    if (box != nullptr) {
                        box->close();
                    }
                }
                for (const QPointer<EditKvmView>& box : m_editBoxes) {
                    if (box != nullptr) {
                        box->close();
                    }
                }
            }
        },
        Qt::DirectConnection);
}

QString OwnWall::saidOf(const core::KvmAdding::Added& added, const QString& called)
{
    // "Build server" added, and after a dash what else is to be said, with a dot between: the
    // page's own two signs, made here from their numbers. (What it is called stands as a
    // sentence calls it, and not as it was typed: calledInASentence.)
    QStringList more;
    if (!added.model.isEmpty()) {
        more.append(added.model);
    }
    if (added.overPlainHttp) {
        more.append(u"over plain HTTP"_s);
    }
    QString said = u"%1 added"_s.arg(called);
    if (!more.isEmpty()) {
        said += u' ' + QChar(0x2014) + u' ' + more.join(u' ' + QString(QChar(0x00B7)) + u' ');
    }
    return said;
}

} // namespace qkvm::ui
