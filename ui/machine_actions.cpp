#include "machine_actions.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
#include <QRandomGenerator>

#include <qkvm/core/kvmd_answers.h>

#include "wall_cards.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// What is told of an action is said as the page says it, and translated where the menu's words are.
QString words(const char* text)
{
    return QCoreApplication::translate("Actions", text);
}

} // namespace

MachineActions::MachineActions(ActionsView& view, WallView& wall, WallMachines& machines, UnderWay& underWay,
                               QObject* parent)
    : QObject(parent), m_view(view), m_media(view.media()), m_wall(wall), m_machines(machines), m_underWay(underWay),
      m_bulk(waysOfARun())
{
    connect(&m_view, &ActionsView::pickedAsked, this, &MachineActions::openPicked);
    connect(&m_view, &ActionsView::runStopAsked, &m_bulk, &BulkRun::stop);
    connect(&m_view, &ActionsView::runRetryAsked, &m_bulk, &BulkRun::retry);
    connect(&m_view, &ActionsView::runCloseAsked, this, &MachineActions::closeRun);
    connect(&m_bulk, &BulkRun::changed, this, &MachineActions::showRun);
    connect(&m_bulk, &BulkRun::finished, this, &MachineActions::runFinished);
    connect(&m_view, &ActionsView::openAsked, this, &MachineActions::open);
    connect(&m_view, &ActionsView::closeAsked, this, &MachineActions::close);
    connect(&m_view, &ActionsView::chosen, this, &MachineActions::choose);
    connect(&m_view, &ActionsView::answered, this, &MachineActions::answer);
    connect(&m_view, &ActionsView::portAsked, this, &MachineActions::showPort);
    connect(&m_view, &ActionsView::consoleAsked, this, &MachineActions::openConsole);
    connect(&m_view, &ActionsView::cardActionAsked, this, &MachineActions::cardAction);
    connect(&m_view, &ActionsView::keysAsked, this, &MachineActions::sendKeys);
    connect(&m_view, &ActionsView::textAsked, this, &MachineActions::typeText);
    connect(&m_view, &ActionsView::formCloseAsked, this, [this] { closeForm(); });
    connect(&m_view, &ActionsView::textFormAsked, this, [this](const QString& key) {
        const auto* machine = machineOf(key);
        if (machine != nullptr && mayActOn(key) && isFree(*machine)) {
            openForm(*machine, u"text"_s);
        }
    });
    connect(&m_machines, &WallMachines::wallChanged, this, &MachineActions::followTheWall);
    // An open menu is of the machine as it is now: a server that was switched on meanwhile is
    // offered "Shut down", and a machine that left the wall has no menu.
    connect(&m_machines, &WallMachines::wallChanged, this, &MachineActions::fill);
    connect(&m_machines, &WallMachines::statusChanged, this, &MachineActions::fill);
    // What the media box did may show on the machine's card: it is asked again soon.
    connect(m_view.media(), &MediaView::done, this, [this](const QString& key) { m_machines.lookAgainSoon(key); });
}

MachineActions::~MachineActions()
{
    // (Nothing of this wall's is held by the media box once this is gone.)
    if (m_media != nullptr) {
        m_media->letGo();
    }
    if (m_control != nullptr) {
        m_control->cancelText(false);
    }
    endAll();
}

// --- each source by itself -----------------------------------------------------------------------------

MachineActions::Side& MachineActions::sideOf(const QString& source)
{
    auto side = m_sides.find(source);
    if (side == m_sides.end()) {
        side = m_sides.insert(source, Side{});
        side->turn = ++m_turns;
    }
    return *side;
}

quint64 MachineActions::turnOf(const QString& source) const
{
    const auto side = m_sides.constFind(source);
    return side == m_sides.constEnd() ? 0 : side->turn;
}

bool MachineActions::mayActAt(const QString& source) const
{
    const auto side = m_sides.constFind(source);
    return side != m_sides.constEnd() && side->mayAct;
}

bool MachineActions::anyMayAct() const
{
    return std::any_of(m_sides.cbegin(), m_sides.cend(), [](const Side& side) { return side.mayAct; });
}

bool MachineActions::mayActOn(const QString& key) const
{
    const Reached at = m_machines.reach(key);
    return at && mayActAt(at.source);
}

bool MachineActions::mayActAs(const core::Account& account)
{
    return !account.mustEnroll && (account.role == "operator"_L1 || account.role == "admin"_L1);
}

bool MachineActions::supportsOn(const QString& key, const QString& ability) const
{
    const auto* machine = machineOf(key);
    const auto status = m_machines.status().constFind(key);
    return machine != nullptr &&
           core::supports(*machine, status == m_machines.status().constEnd() ? nullptr : &*status, ability);
}

void MachineActions::began(const QString& machine, const QString& title, const QString& source)
{
    const auto identity = m_machines.actionKey(machine);
    m_underWay.insert(identity, title);
    m_pendingKeys.insert(machine, identity);
    m_mine.insert(machine, source);
}

void MachineActions::ended(const QString& machine)
{
    m_mine.remove(machine);
    m_underWay.remove(m_pendingKeys.take(machine));
}

void MachineActions::endAll()
{
    for (auto mine = m_mine.cbegin(); mine != m_mine.cend(); ++mine) {
        m_underWay.remove(m_pendingKeys.take(mine.key()));
    }
    m_mine.clear();
}

void MachineActions::endAllOf(const QString& source)
{
    m_mine.removeIf([&](const auto& mine) {
        if (mine.value() != source) {
            return false;
        }
        m_underWay.remove(m_pendingKeys.take(mine.key()));
        return true;
    });
}

int MachineActions::underWay(const QString& text, const QString& source)
{
    const int notice = m_view.notices()->underWay(text);
    m_noticesOf.insert(notice, source);
    return notice;
}

void MachineActions::over(int notice)
{
    m_noticesOf.remove(notice);
    m_view.notices()->over(notice);
}

void MachineActions::enter(const QString& source, bool mayAct)
{
    if (sideOf(source).mayAct && !mayAct) {
        letGoOf(source);
        // And nothing of it stays picked; where nobody may act on anything any more, machines are
        // not being picked at all.
        if (anyMayAct()) {
            unpick(source);
        } else {
            m_wall.setSelecting(false);
        }
    }
    sideOf(source).mayAct = mayAct;
    m_view.setAllowed(anyMayAct());
    // The media box goes where whoever sits here may have that source's media no longer.
    keepTheMediaBox();
}

void MachineActions::enter(const core::Account& account)
{
    enter(QString(), mayActAs(account));
}

void MachineActions::leave(const QString& source)
{
    letGoOf(source);
}

void MachineActions::forgetSource(const QString& source)
{
    letGoOf(source);
    m_sides.remove(source);
    m_view.setAllowed(anyMayAct());
}

void MachineActions::leave()
{
    for (Side& side : m_sides) {
        side.turn = ++m_turns;
        side.mayAct = false;
        side.saved.clear();
    }
    forget(std::nullopt);
}

void MachineActions::letGoOf(const QString& source)
{
    Side& side = sideOf(source);
    side.turn = ++m_turns;
    side.mayAct = false;
    side.saved.clear();
    forget(anyMayAct() ? std::optional<QString>(source) : std::nullopt);
}

void MachineActions::forget(const std::optional<QString>& only)
{
    const auto isOf = [&](const QString& source) { return !only || source == *only; };
    const auto isOfAny = [&](const QSet<QString>& sources) { return !only || sources.contains(*only); };
    m_view.setAllowed(anyMayAct());
    const bool asked = m_asked && isOf(m_asked->source);
    const bool askedOfSeveral = m_askedOfSeveral && isOfAny(m_askedOfSeveral->sources);
    if (m_control != nullptr && (!only || asked || (m_formFor && isOf(m_formOf)))) {
        m_control->cancelText(false);
    }
    if (asked) {
        m_asked.reset();
    }
    if (askedOfSeveral) {
        m_askedOfSeveral.reset();
    }
    if (!only || asked || askedOfSeveral) {
        m_view.showQuestion({});
    }
    if (!only || (!m_open.isEmpty() && isOf(m_openOf)) || (!m_several.isEmpty() && isOfAny(m_severalOf))) {
        close();
    }
    if (!only || (m_formFor && isOf(m_formOf)) || (!m_formForSeveral.isEmpty() && isOfAny(m_formSeveralOf))) {
        closeForm();
    }
    // What is being done to several is nobody's any more where one of them is of the source: nothing
    // more is sent, and nothing is told. (What it has out to the others frees them when answered.)
    if (!only || std::any_of(m_runAt.cbegin(), m_runAt.cend(), [&](const Reached& at) { return at.source == *only; })) {
        forgetTheRun();
    }
    if (m_entering && isOf(m_enteringOf)) {
        m_entering = false;
    }
    if (m_view.media()->opened() && isOf(m_mediaFor)) {
        m_view.media()->letGo();
    }
    if (!only) {
        endAll();
        m_noticesOf.clear();
        m_view.notices()->clear();
        return;
    }
    endAllOf(*only);
    // (Its answers are nobody's: what says that one is on its way is over.)
    QList<int> notices;
    for (auto notice = m_noticesOf.cbegin(); notice != m_noticesOf.cend(); ++notice) {
        if (notice.value() == *only) {
            notices.append(notice.key());
        }
    }
    for (const int notice : std::as_const(notices)) {
        over(notice);
    }
}

void MachineActions::unpick(const QString& source)
{
    const QStringList picked = m_wall.pickedMachines();
    for (const QString& key : picked) {
        if (sourceOf(key) == source) {
            m_wall.machines()->setPicked(key, false);
        }
    }
}

void MachineActions::forgetTheRun()
{
    m_runShown = false;
    m_bulk.forget();
    m_runAt.clear();
    m_runSavedOf.reset();
    m_view.showRun({});
}

// --- one machine ---------------------------------------------------------------------------------------

const core::Machine* MachineActions::machineOf(const QString& id) const
{
    const QList<core::Machine>& machines = m_machines.machines();
    const auto found = std::ranges::find_if(machines, [&](const core::Machine& one) { return one.id == id; });
    return found == machines.end() ? nullptr : &*found;
}

void MachineActions::holdStill(bool still)
{
    m_view.setBusy(still);
}

void MachineActions::open(const QString& machine)
{
    const core::Machine* known = machineOf(machine);
    const Reached at = m_machines.reach(machine);
    if (known == nullptr || !at || !mayActAt(at.source)) {
        return;
    }
    close();
    m_open = machine;
    m_openOf = at.source;
    m_menuSavedOf = at.source;
    fill();
    askForSaved(*at.place, at.source);
    // A server's controller wakes nobody: only a KVM is asked whom it can wake.
    if (known->hasPicture && supportsOn(machine, u"wol"_s)) {
        at.place->askWakeTargets(at.machine, [guard = QPointer<MachineActions>(this), source = at.source,
                                              turn = turnOf(at.source),
                                              machine](const base::Result<QList<core::WakeTarget>>& targets) {
            // Where they cannot be had, the menu is without them, as the page's is.
            if (guard == nullptr || turn != guard->turnOf(source) || guard->m_open != machine || !targets) {
                return;
            }
            guard->m_wake = targets.value();
            guard->fill();
        });
    }
}

void MachineActions::askForSaved(core::WallSource& place, const QString& source)
{
    place.askSavedActions([guard = QPointer<MachineActions>(this), source,
                           turn = turnOf(source)](const base::Result<QList<core::SavedAction>>& saved) {
        // Where they cannot be had, the list that was there is kept.
        if (guard == nullptr || turn != guard->turnOf(source) || !saved) {
            return;
        }
        guard->sideOf(source).saved = saved.value();
        guard->fill();
    });
}

QList<core::SavedAction> MachineActions::savedOfTheMenu() const
{
    return m_menuSavedOf ? m_sides.value(*m_menuSavedOf).saved : QList<core::SavedAction>();
}

void MachineActions::close()
{
    // A question that was asked from the menu goes with it.
    if ((m_asked && m_asked->fromMenu) || m_askedOfSeveral) {
        m_asked.reset();
        m_askedOfSeveral.reset();
        m_view.showQuestion({});
    }
    m_open.clear();
    m_openOf.clear();
    m_several.clear();
    m_severalOf.clear();
    m_menuSavedOf.reset();
    m_menu = {};
    m_wake.clear();
    m_still = 0;
    holdStill(false);
    m_view.showMenu({});
}

void MachineActions::fill()
{
    if (!m_several.isEmpty()) {
        fillForSeveral();
        return;
    }
    if (m_open.isEmpty()) {
        return;
    }
    const core::Machine* machine = machineOf(m_open);
    if (machine == nullptr) {
        close();
        return;
    }
    MenuOf of;
    of.machine = *machine;
    if (const auto status = m_machines.status().constFind(m_open); status != m_machines.status().constEnd()) {
        of.status = *status;
    }
    of.saved = savedOfTheMenu();
    of.wake = m_wake;
    of.enlarged = m_wall.enlarged() == m_open;
    of.media = mediaOf(m_openOf) != nullptr;
    m_menu = menuFor(of);
    m_view.showMenu(shown(m_menu));
}

void MachineActions::choose(const QString& row)
{
    if (!m_several.isEmpty()) {
        chooseForSeveral(row);
        return;
    }
    // While one of its rows is being answered the menu holds still: a second click, or a key held
    // down, is not a second request.
    if (m_open.isEmpty() || m_view.busy()) {
        return;
    }
    const MenuRow* chosen = m_menu.row(row);
    const core::Machine* machine = machineOf(m_open);
    if (chosen == nullptr || !chosen->enabled || machine == nullptr) {
        return;
    }
    if (!chosen->ask) {
        // A row that leads elsewhere in the program.
        if (chosen->id == "console"_L1) {
            // (The menu stays: a console opens beside the program, not in its place.)
            openConsole(m_open);
        } else if (chosen->id == "enlarge"_L1) {
            const QString which = m_open;
            close();
            m_wall.enlarge(which);
        } else if (chosen->id == "edit"_L1) {
            // (The menu goes: the box that changes the KVM opens in its place, for the KVM by its
            // own id, which is what this PC's own wall knows it by.)
            const Reached at = m_machines.reach(m_open);
            close();
            if (at) {
                emit m_view.editAsked(at.machine, false);
            }
        } else if (chosen->id == "media"_L1) {
            // (The menu goes: the box opens in its place, as the page's does.)
            openMedia(m_open);
        } else if (chosen->id == "sendKeys"_L1 || chosen->id == "typeText"_L1) {
            const core::Machine whom = *machine;
            openForm(whom, chosen->id == "sendKeys"_L1 ? u"keys"_s : u"text"_s);
        } else if (chosen->id.startsWith("view:"_L1)) {
            const core::Machine whom = *machine;
            putInView(whom, chosen->id.sliced(5));
        }
        return;
    }
    // (Its own: the menu may be filled anew while this is under way.)
    const core::Machine whom = *machine;
    const Ask ask = *chosen->ask;
    const std::optional<QString> savedOf = m_menuSavedOf;
    perform(whom, ask, true, savedOf);
}

void MachineActions::showPort(const QString& machine, int port)
{
    const core::Machine* known = machineOf(machine);
    const auto status = m_machines.status().constFind(machine);
    // The one that is on the screen already is not asked for again.
    if (!mayActAt(sourceOf(machine)) || known == nullptr || status == m_machines.status().constEnd() ||
        port == status->shownPort) {
        return;
    }
    if (const std::optional<Ask> ask = askToShow(*known, *status, port)) {
        const core::Machine whom = *known;
        perform(whom, *ask, false, std::nullopt);
    }
}

void MachineActions::cardAction(const QString& machine)
{
    const core::Machine* known = machineOf(machine);
    const Reached at = m_machines.reach(machine);
    if (known == nullptr || !at || !mayActAt(at.source)) {
        return;
    }
    const auto status = m_machines.status().constFind(machine);
    // By the rule that put the button on the card, for the machine as it is now: a card that
    // has none any more leads nowhere. (The box knows the KVM by its own id.)
    const CardWay way = cardWayOf(*known, status == m_machines.status().constEnd() ? nullptr : &*status);
    if (way == CardWay::ShowHere) {
        // Asked first, as the page asks it (app.js, showHere): taking it back throws whoever has its own
        // console open out of it.
        Ask ask;
        ask.title = words("Show here");
        ask.action = core::Action::plain(u"resume"_s);
        ask.done = words("shown here");
        ask.confirm = true;
        ask.question = words("Show %1 here?").arg(known->name);
        ask.warning = words("Whoever has its console open is disconnected from it.");
        const core::Machine whom = *known;
        perform(whom, ask, false, std::nullopt);
        return;
    }
    if (way != CardWay::None) {
        emit m_view.editAsked(at.machine, way == CardWay::ReviewCertificate);
    }
}

void MachineActions::openPagesWith(std::function<bool(const QUrl&)> opener)
{
    m_opener = std::move(opener);
}

void MachineActions::openThePage(const QUrl& address)
{
    const QUrl page = core::webAddress(address.toString());
    if (page.isEmpty() || !m_opener || !m_opener(page)) {
        m_view.notices()->tell(Notices::Kind::Failed, words("No browser could be opened for the qKVM page."));
    }
}

void MachineActions::browseTo(const core::Machine& machine, const QUrl& address)
{
    if (!m_opener || !m_opener(address)) {
        m_view.notices()->tell(Notices::Kind::Failed,
                               words("%1: no browser could be opened for its console.").arg(machine.name));
    }
}

void MachineActions::openConsole(const QString& machine)
{
    const core::Machine* known = machineOf(machine);
    const Reached at = m_machines.reach(machine);
    if (known == nullptr || known->page.isEmpty() || !at) {
        return;
    }
    const auto status = m_machines.status().constFind(machine);
    // Straight in, signed in, only for whoever may act, and only where the machine hands out a pass
    // for it. Everybody else is sent to the console's door.
    if (!mayActAt(at.source) || status == m_machines.status().constEnd() || !status->opensSignedIn ||
        !supportsOn(machine, u"console"_s)) {
        browseTo(*known, known->page);
        return;
    }
    if (m_entering) {
        return;
    }
    m_entering = true;
    m_enteringOf = at.source;
    const core::Machine whom = *known;
    const int notice = underWay(words("%1: %2…").arg(whom.name, words("Open console")), at.source);
    at.place->askConsole(at.machine, [guard = QPointer<MachineActions>(this), source = at.source,
                                      turn = turnOf(at.source), whom, notice](const base::Result<QUrl>& way) {
        if (guard == nullptr || turn != guard->turnOf(source)) {
            return;
        }
        guard->m_entering = false;
        guard->over(notice);
        Notices& notices = *guard->m_view.notices();
        if (!way) {
            notices.tell(Notices::Kind::Failed,
                         words("%1: %2").arg(whom.name, QString::fromStdString(way.error().message)));
            return;
        }
        // The pass leads into this machine's console, at the address the machine is opened at,
        // and nowhere else: whatever the answer names, a browser is not sent elsewhere with it.
        if (!core::sameSite(way.value(), whom.page)) {
            notices.tell(
                Notices::Kind::Failed,
                words("%1: the server named a console that is not this machine's. Nothing was opened.").arg(whom.name));
            return;
        }
        guard->browseTo(whom, way.value());
    });
}

void MachineActions::putInView(const core::Machine& machine, const QString& view)
{
    const Reached at = m_machines.reach(machine.id);
    if (!at || !mayActAt(at.source) || !core::isAmtView(view) || m_viewing) {
        return;
    }
    m_viewing = true;
    const int notice = underWay(words("%1: %2…").arg(machine.name, amtViewWord(view)), at.source);
    at.place->setView(at.machine, view,
                      [guard = QPointer<MachineActions>(this), source = at.source, turn = turnOf(at.source),
                       key = machine.id, name = machine.name, notice](const base::Result<QString>& now) {
                          if (guard == nullptr) {
                              return;
                          }
                          guard->m_viewing = false;
                          if (turn != guard->turnOf(source)) {
                              return;
                          }
                          guard->over(notice);
                          Notices& notices = *guard->m_view.notices();
                          if (!now) {
                              // (The menu stays, as the page's box does.)
                              notices.tell(Notices::Kind::Failed,
                                           words("%1: %2").arg(name, QString::fromStdString(now.error().message)));
                              return;
                          }
                          // Said as the page's toast says it; the menu goes, and the tile follows at
                          // its next looks, which come sooner.
                          notices.tell(Notices::Kind::Done, words("%1: %2").arg(name, amtViewWord(now.value())));
                          if (guard->m_open == key) {
                              guard->close();
                          }
                          guard->m_machines.lookAgainSoon(key);
                      });
}

void MachineActions::openForm(const core::Machine& machine, const QString& kind)
{
    if (!supportsOn(machine.id, kind))
        return;
    close();
    if (kind == "text"_L1 && m_control != nullptr) {
        m_control->pauseForText(machine.id);
    }
    m_formFor = machine;
    m_formOf = sourceOf(machine.id);
    m_formKind = kind;
    m_view.showForm({{u"kind"_s, kind}, {u"machine"_s, machine.name}, {u"problem"_s, QString()}});
}

void MachineActions::closeForm(bool continuing)
{
    if (!continuing && m_formKind == "text"_L1 && m_control != nullptr) {
        m_control->cancelText();
    }
    m_formFor.reset();
    m_formOf.clear();
    m_formForSeveral.clear();
    m_formSeveralOf.clear();
    m_formKind.clear();
    m_view.showForm({});
}

void MachineActions::openMedia(const QString& machine)
{
    // (A copy: what names it may be the open menu's, which close() empties before it is used.)
    const QString key = machine; // NOLINT(performance-unnecessary-copy-initialization): see above
    const core::Machine* known = machineOf(key);
    const Reached at = m_machines.reach(key);
    core::ServerClient* client = at ? mediaOf(at.source) : nullptr;
    if (known == nullptr || client == nullptr || !mayActAt(at.source)) {
        return;
    }
    const QString name = known->name;
    close();
    m_mediaFor = at.source;
    m_view.media()->open(client, at.machine, name, key, &m_underWay, m_machines.actionKey(key));
}

core::ServerClient* MachineActions::mediaOf(const QString& source) const
{
    return m_mediaOf && mayActAt(source) ? m_mediaOf(source) : nullptr;
}

void MachineActions::keepTheMediaBox()
{
    MediaView& media = *m_view.media();
    if (!media.opened()) {
        return;
    }
    // Of a source whose media whoever sits here may have no longer, or through another client by now: it goes.
    const core::ServerClient* client = mediaOf(m_mediaFor);
    const Reached at = m_machines.reach(media.key());
    if (client == nullptr || client != media.client() || !at || at.source != m_mediaFor) {
        media.letGo();
    }
}

void MachineActions::followTheWall()
{
    // The media box is of a machine on the wall, by the name it is called now: one that left takes it with it. (What
    // was sent from it goes on, and is told in the corner: the server holds to the machine it was sent to.)
    if (MediaView& media = *m_view.media(); media.opened()) {
        if (const core::Machine* machine = machineOf(media.key()); machine != nullptr) {
            media.rename(machine->name);
            keepTheMediaBox();
        } else {
            media.close();
        }
    }
    // A machine that left the wall takes the box that was open for it with it; and where the box
    // was for several, they are the several they were, or the box goes.
    const bool oneLeft = m_formFor && machineOf(m_formFor->id) == nullptr;
    const bool ofSeveralLeft = std::ranges::any_of(
        m_formForSeveral, [this](const core::Machine& machine) { return machineOf(machine.id) == nullptr; });
    if (oneLeft || ofSeveralLeft) {
        closeForm();
    }
}

QString MachineActions::formIsFor() const
{
    return m_formFor ? m_formFor->name : severalKvms(m_formForSeveral.size());
}

void MachineActions::sendKeys(const QString& keys)
{
    if ((m_formFor || !m_formForSeveral.isEmpty()) && m_formKind == "keys"_L1) {
        fromTheForm(askToPress(formIsFor(), keys), core::kvmd::namesThePowerKey(keys)
                                                       ? words(core::kvmd::powerKeyRefused)
                                                       : words("Choose at least one key."));
    }
}

void MachineActions::typeText(const QString& text, bool enter)
{
    if ((m_formFor || !m_formForSeveral.isEmpty()) && m_formKind == "text"_L1) {
        fromTheForm(askToType(formIsFor(), text, enter),
                    text.isEmpty() ? words("Enter the text to type.")
                                   : words("Use at most 2,000 characters, including Enter afterwards, and complete "
                                           "Unicode characters. Nothing was sent."));
    }
}

void MachineActions::fromTheForm(const std::optional<Ask>& ask, const QString& amiss)
{
    if (!ask) {
        m_view.showForm({{u"kind"_s, m_formKind}, {u"machine"_s, formIsFor()}, {u"problem"_s, amiss}});
        return;
    }
    if (m_formFor) {
        const core::Machine machine = *m_formFor;
        closeForm(true);
        perform(machine, *ask, false, std::nullopt);
        return;
    }
    // One run at a time: said before anything is asked about, as where a row is chosen for several
    // (chooseForSeveral). The box stays as it was, with what was typed in it, to be sent once the
    // run that goes on is over.
    if (refusedWhileARunGoes()) {
        return;
    }
    const QList<core::Machine> targets = m_formForSeveral;
    closeForm();
    askOfSeveral(targets, *ask, std::nullopt);
}

// --- several machines ----------------------------------------------------------------------------------

void MachineActions::openPicked()
{
    // Of those that are picked, none whose source lets nobody act. (One that reaches nothing is
    // kept: what is done to it is refused as to one that is not on the wall.)
    QStringList picked;
    for (const QString& key : m_wall.pickedMachines()) {
        if (const Reached at = m_machines.reach(key); !at || mayActAt(at.source)) {
            picked.append(key);
        }
    }
    if (!anyMayAct() || picked.isEmpty()) {
        return;
    }
    // One machine alone has its own menu, with all that is its own.
    if (picked.size() == 1) {
        open(picked.constFirst());
        return;
    }
    close();
    m_several = picked;
    // Their saved actions are asked of the place they are of, where every one of them that is on
    // the wall is of one source; of more than one, none is offered.
    Reached first;
    bool ofOne = true;
    for (const QString& key : picked) {
        const Reached at = m_machines.reach(key);
        if (!at) {
            continue;
        }
        m_severalOf.insert(at.source);
        if (!first) {
            first = at;
        } else if (at.source != first.source) {
            ofOne = false;
        }
    }
    if (first && ofOne) {
        m_menuSavedOf = first.source;
    }
    fill();
    if (m_menuSavedOf) {
        askForSaved(*first.place, *m_menuSavedOf);
    }
}

QList<core::Machine> MachineActions::severalThere() const
{
    QList<core::Machine> there;
    for (const core::Machine& machine : m_machines.machines()) {
        if (m_several.contains(machine.id)) {
            there.append(machine);
        }
    }
    return there;
}

QSet<QString> MachineActions::sourcesOf(const QList<core::Machine>& machines) const
{
    QSet<QString> sources;
    for (const core::Machine& machine : machines) {
        if (const Reached at = m_machines.reach(machine.id)) {
            sources.insert(at.source);
        }
    }
    return sources;
}

void MachineActions::fillForSeveral()
{
    const QList<core::Machine> there = severalThere();
    // Machines have left the wall since: what is left is no longer several.
    if (there.size() < 2) {
        close();
        return;
    }
    QList<MenuOf> of;
    const QList<core::SavedAction> saved = savedOfTheMenu();
    for (const core::Machine& machine : there) {
        MenuOf one;
        one.machine = machine;
        if (const auto status = m_machines.status().constFind(machine.id); status != m_machines.status().constEnd()) {
            one.status = *status;
        }
        one.saved = saved;
        // Of more than one source: no saved action is any of theirs, and the menu has no place
        // for them. (How that is said is the screens' round's to word.)
        one.savedLeftOut = !m_menuSavedOf.has_value();
        of.append(one);
    }
    m_menu = menuForSeveral(of);
    m_view.showMenu(shown(m_menu));
}

void MachineActions::chooseForSeveral(const QString& row)
{
    if (m_view.busy()) {
        return;
    }
    const MenuRow* chosen = m_menu.row(row);
    const QList<core::Machine> targets = severalThere();
    if (chosen == nullptr || !chosen->enabled || targets.size() < 2) {
        return;
    }
    // One run at a time. Said before anything is asked about, or put into a box, for what would
    // not be begun: the run whose box was closed is waiting for the answers that are still out.
    if (refusedWhileARunGoes()) {
        return;
    }
    if (!chosen->ask) {
        if (chosen->id == "sendKeys"_L1 || chosen->id == "typeText"_L1) {
            const QString kind = chosen->id == "sendKeys"_L1 ? u"keys"_s : u"text"_s;
            close();
            m_formForSeveral = targets;
            m_formSeveralOf = sourcesOf(targets);
            m_formKind = kind;
            m_view.showForm({{u"kind"_s, kind}, {u"machine"_s, formIsFor()}, {u"problem"_s, QString()}});
        }
        return;
    }
    const Ask ask = *chosen->ask;
    const std::optional<QString> savedOf = m_menuSavedOf;
    askOfSeveral(targets, ask, savedOf);
}

void MachineActions::askOfSeveral(const QList<core::Machine>& targets, const Ask& ask,
                                  const std::optional<QString>& savedOf)
{
    if (!anyMayAct() || targets.isEmpty()) {
        return;
    }
    // Whatever it is: done to several at once, it is asked about first, and they are named.
    m_askedOfSeveral = AskedOfSeveral{targets, ask, sourcesOf(targets), savedOf};
    if (!m_several.isEmpty()) {
        holdStill(true);
    }
    QList<Target> listed;
    for (const core::Machine& machine : targets) {
        const auto status = m_machines.status().constFind(machine.id);
        const WallTile card = cardFor(machine, status == m_machines.status().constEnd() ? nullptr : &*status, 0, false);
        listed.append(Target{machine.name, machine.host, card.state, card.label});
    }
    m_view.showQuestion(shown(ask, listed));
}

void MachineActions::answerOfSeveral(bool yes, int paceSeconds, bool ticked)
{
    const AskedOfSeveral asked = *m_askedOfSeveral;
    m_askedOfSeveral.reset();
    m_view.showQuestion({});
    holdStill(false);
    // What can lose somebody's work on several machines at once takes a tick as well as a yes.
    if (!yes || ((asked.ask.danger || asked.ask.tick) && !ticked)) {
        return;
    }
    // (Machines may have left the wall while the question stood.)
    QList<core::Machine> targets;
    for (const core::Machine& machine : asked.targets) {
        if (machineOf(machine.id) != nullptr) {
            targets.append(machine);
        }
    }
    if (targets.isEmpty()) {
        return;
    }
    // One run at a time, here too: a question that stood while a run was sent once more begins
    // nothing. (A box for several that is sent while one goes is refused before it asks anything:
    // fromTheForm.) Where each machine of the run that goes on is sent, and whose word its answers
    // are, stays that run's.
    if (refusedWhileARunGoes()) {
        return;
    }
    // The menu goes, and how it goes is shown in its place, as on the page.
    close();
    // What ties this run's entries together in the server's activity log: when, and a few letters
    // of chance.
    const QString batch = QString::number(QDateTime::currentMSecsSinceEpoch(), 36) + u'-' +
                          QString::number(QRandomGenerator::global()->bounded(36 * 36 * 36 * 36 * 36), 36);
    m_runShown = true;
    // (Each of them is on the wall: what its key reaches is where the run sends to it, now and
    // when it is sent once more.)
    m_runAt.clear();
    for (const core::Machine& machine : targets) {
        m_runAt.insert(machine.id, m_machines.reach(machine.id));
    }
    m_runSavedOf = asked.savedOf;
    m_bulk.start(targets, asked.ask, paceSeconds < 0 ? std::nullopt : std::optional<int>(std::min(paceSeconds, 3600)),
                 batch);
}

bool MachineActions::refusedWhileARunGoes()
{
    if (!m_bulk.isRunning()) {
        return false;
    }
    m_view.notices()->tell(
        Notices::Kind::Info,
        words("\"%1\" is still being done to several machines. Wait for it to finish.").arg(m_bulk.title()));
    return true;
}

BulkRun::Ways MachineActions::waysOfARun()
{
    BulkRun::Ways ways;
    ways.underWay = [this](const QString& machine) { return m_underWay.value(m_machines.actionKey(machine)); };
    ways.began = [this](const QString& machine, const QString& title) {
        began(machine, title, m_runAt.value(machine).source);
    };
    ways.ended = [this](const QString& machine) { ended(machine); };
    ways.send = [this](const core::Machine& machine, const core::Action& action,
                       std::function<void(base::Result<core::Done>)> told) {
        const Reached at = m_runAt.value(machine.id);
        if (!at) {
            // (Not so: every machine of a run is noted where the run begins. Refused, and so not
            // done, rather than left out of the run's count for ever.)
            told(base::Error{"not_found", words("Not on the wall any more.").toStdString()});
            return;
        }
        // A saved action is its source's: what has its id elsewhere is another action, and is not
        // run on a machine of another source, whatever the menu offered.
        if (!action.savedId.isEmpty() && (!m_runSavedOf || *m_runSavedOf != at.source)) {
            told(base::Error{"another_place", words("Not sent: this saved action is another place's.").toStdString()});
            return;
        }
        if (!mayActAt(at.source) || !supportsOn(machine.id, abilityFor(action))) {
            told(base::Error{"not_supported", "This machine no longer allows that action. Nothing was sent."});
            return;
        }
        at.place->act(at.machine, action,
                      [guard = QPointer<MachineActions>(this), source = at.source, turn = turnOf(at.source),
                       told = std::move(told)](const base::Result<core::Done>& answer) {
                          // (An answer that comes after its source's wall was left is nobody's.)
                          if (guard != nullptr && turn == guard->turnOf(source)) {
                              told(answer);
                          }
                      });
    };
    // The word of the place the machine was sent to, which is what its key reached when the run
    // began. (An answer reaches the run only in the turn of its source it was sent in, and another
    // place is another source: so that is the place that was asked.)
    ways.mayHaveBeenDone = [this](const core::Machine& machine, const base::Error& error) {
        const Reached at = m_runAt.value(machine.id);
        return at && at.place->mayHaveBeenDone(at.machine, error);
    };
    ways.done = [this](const core::Machine& machine, const Ask& ask) {
        m_machines.lookAgainSoon(machine.id);
        if (ask.action.kind == "reboot"_L1) {
            emit restarted(machine.id);
        }
    };
    return ways;
}

void MachineActions::showRun()
{
    if (m_runShown) {
        m_view.showRun(m_bulk.shown());
    }
}

void MachineActions::runFinished()
{
    // Its box was closed while it ran: how it ended is said where it can still be seen. And with
    // the box, what it would have offered to send once more is gone.
    if (!m_runShown) {
        m_view.notices()->tell(m_bulk.allDone() ? Notices::Kind::Done : Notices::Kind::Failed,
                               words("%1: %2").arg(m_bulk.title(), m_bulk.told()));
        m_bulk.forget();
        m_runAt.clear();
        m_runSavedOf.reset();
    }
}

void MachineActions::closeRun()
{
    m_runShown = false;
    m_view.showRun({});
    if (m_bulk.isRunning()) {
        // Closing it while it runs is the fastest way to say "stop": nothing more is sent, and how
        // it ended is said in the corner, at once or when what is on its way was answered.
        m_bulk.stop();
    } else {
        m_bulk.forget();
        m_runAt.clear();
        m_runSavedOf.reset();
    }
}

bool MachineActions::isFree(const core::Machine& machine)
{
    const auto out = m_underWay.constFind(m_machines.actionKey(machine.id));
    if (out == m_underWay.constEnd()) {
        return true;
    }
    m_view.notices()->tell(Notices::Kind::Info,
                           words("%1: \"%2\" is still under way. Wait for its answer.").arg(machine.name, *out));
    return false;
}

void MachineActions::perform(const core::Machine& machine, const Ask& ask, bool fromMenu,
                             const std::optional<QString>& savedOf)
{
    // Said before asking "are you sure" about something that will not be sent.
    const QString source = sourceOf(machine.id);
    if (!mayActAt(source) || !supportsOn(machine.id, abilityFor(ask.action)) || !isFree(machine)) {
        if (ask.action.kind == "text"_L1 && m_control != nullptr) {
            m_control->cancelText(mayActAt(source));
        }
        return;
    }
    if (!ask.confirm) {
        send(machine, ask, fromMenu, savedOf);
        return;
    }
    Ask question = ask;
    if (ask.action.kind == "text"_L1 && m_control != nullptr && m_control->textHoldsControl()) {
        question.warning = words("Control is briefly released to type this text, then requested again. Another "
                                 "session holding control will still block this action.");
    }
    m_asked = Asked{machine, question, fromMenu, source, savedOf};
    if (fromMenu) {
        holdStill(true);
    }
    m_view.showQuestion(shown(question));
}

void MachineActions::answer(bool yes, int paceSeconds, bool ticked)
{
    if (m_askedOfSeveral) {
        answerOfSeveral(yes, paceSeconds, ticked);
        return;
    }
    if (!m_asked) {
        return;
    }
    const Asked asked = *m_asked;
    m_asked.reset();
    m_view.showQuestion({});
    // (The machine may have left the wall while the question stood.)
    if (yes && machineOf(asked.machine.id) != nullptr) {
        send(asked.machine, asked.ask, asked.fromMenu, asked.savedOf);
    } else {
        if (asked.ask.action.kind == "text"_L1 && m_control != nullptr) {
            m_control->cancelText();
        }
        if (asked.fromMenu) {
            holdStill(false);
        }
    }
}

bool MachineActions::goesTo(const Ask& ask, const std::optional<QString>& savedOf, const QString& source)
{
    return ask.action.savedId.isEmpty() || (savedOf && *savedOf == source);
}

void MachineActions::send(const core::Machine& machine, const Ask& ask, bool fromMenu,
                          const std::optional<QString>& savedOf)
{
    // Something may have been sent from elsewhere while the question stood. (And whoever sends
    // found the machine on the wall a moment ago: its key reaches it. A saved action goes only to a
    // machine of the source it is of.)
    const Reached at = m_machines.reach(machine.id);
    if (!isFree(machine) || !at || !goesTo(ask, savedOf, at.source) || !mayActAt(at.source) ||
        !supportsOn(machine.id, abilityFor(ask.action))) {
        if (ask.action.kind == "text"_L1 && m_control != nullptr) {
            m_control->cancelText();
        }
        if (fromMenu) {
            holdStill(false);
        }
        return;
    }
    core::Action action = ask.action;
    bool holds = fromMenu;
    if (action.kind == "bios_setup"_L1) {
        // As the page starts into the BIOS setup (app.js, bootIntoSetup): the menu goes, and the machine's large view
        // opens first, so that the setup screen shows from the start. Not opened again where it is open already:
        // that would end its live line and give its keyboard back.
        if (holds) {
            close();
            holds = false;
        }
        if (m_wall.enlarged() != machine.id) {
            m_wall.enlarge(machine.id);
        }
    }
    // From the machine's large view the server is told so, as the page tells it (`watching`): a PC with Intel AMT then
    // opens a session before it switches on, to show the start.
    action.watching = m_wall.enlarged() == machine.id;
    began(machine.id, ask.title, at.source);
    const int notice = underWay(words("%1: %2…").arg(machine.name, ask.title), at.source);
    const quint64 sent = ++m_lastSent;
    if (holds) {
        m_still = sent;
        holdStill(true);
    }
    const auto completed = [guard = QPointer<MachineActions>(this), turn = turnOf(at.source), machine, ask, notice,
                            sent, at](const base::Result<core::Done>& answer) {
        // (Leaving its source's wall took the notice away, and whatever else was this
        // one's.)
        if (guard == nullptr || turn != guard->turnOf(at.source)) {
            return;
        }
        guard->ended(machine.id);
        guard->over(notice);
        if (guard->m_still == sent) {
            guard->m_still = 0;
            guard->holdStill(false);
        }
        guard->told(machine, ask, answer, at);
    };
    auto start = [guard = QPointer<MachineActions>(this), turn = turnOf(at.source), at, action, key = machine.id,
                  completed](std::function<void(bool)> finish) {
        if (guard == nullptr || turn != guard->turnOf(at.source) || !guard->mayActAt(at.source)) {
            finish(false);
            return;
        }
        if (!guard->supportsOn(key, abilityFor(action))) {
            completed(base::Error{"not_supported", "This machine no longer allows that action. Nothing was sent."});
            finish(false);
            return;
        }
        at.place->act(at.machine, action,
                      [completed, finish = std::move(finish)](const base::Result<core::Done>& answer) {
                          completed(answer);
                          finish(answer.ok());
                      });
    };
    // A typed text, keys and a saved action (which presses keys or types) wait for this window's
    // keyboard and mouse on that machine to be given back, and for that to be confirmed: an action
    // sent right after waits for the letting go, or a key combination lands beside a Ctrl that is
    // still on its way up (rule 27 of the web's FOR-THE-DESKTOP-APP). They are asked for again
    // after it. (Where this window holds nothing there, it goes at once.)
    const QString& kind = ask.action.kind;
    if ((kind == "text"_L1 || kind == "keys"_L1 || kind == "custom"_L1) && m_control != nullptr) {
        m_control->textAfterRelease(
            machine.id, std::move(start),
            [completed, typed = kind == "text"_L1, control = m_control, key = machine.id] {
                // (Where something may still be down on it, that is why: what was picked could have landed beside it.)
                if (control != nullptr && control->mayBeDownOn(key)) {
                    completed(
                        base::Error{"desktop:may_be_down",
                                    typed ? "Something that was pressed may still be down on this machine; no text "
                                            "was sent. Take keyboard and mouse again, and press and let go of it."
                                          : "Something that was pressed may still be down on this machine; nothing "
                                            "was sent. Take keyboard and mouse again, and press and let go of it."});
                    return;
                }
                completed(base::Error{"desktop:control_unconfirmed",
                                      typed ? "Control release was not confirmed; no text was sent."
                                            : "Control release was not confirmed; nothing was sent."});
            });
    } else {
        start([](bool) {});
    }
}

void MachineActions::told(const core::Machine& machine, const Ask& ask, const base::Result<core::Done>& answer,
                          const Reached& sentTo)
{
    Notices& notices = *m_view.notices();
    if (!answer) {
        const base::Error& error = answer.error();
        // No answer is not a refusal: what was asked may have arrived, and been done. Which errors
        // are no answer is for the place it was sent to to say.
        const bool mayHaveBeenDone = sentTo.place->mayHaveBeenDone(sentTo.machine, error);
        if (ask.action.kind == "bios_setup"_L1) {
            // As the page tells it: where the server answered, its own words, which say whether the setup boot is left
            // set or was cleared again (a `504 timeout` says that it may be). Where no answer of its came (this
            // program's own finding: a code with a colon), it may have been done, and the boot may be set.
            const bool unanswered = mayHaveBeenDone && error.code.find(':') != std::string::npos;
            notices.tell(Notices::Kind::Failed,
                         unanswered ? words("%1: no answer to \"%2\". It may have been done, and its setup boot may be "
                                            "set for the next boot: look at the machine before sending it again (an "
                                            "admin finds how it ended in the activity log).")
                                          .arg(machine.name, ask.title)
                                    : words("%1: %2").arg(machine.name, QString::fromStdString(error.message)));
            return;
        }
        notices.tell(Notices::Kind::Failed,
                     mayHaveBeenDone ? words("%1: no answer to \"%2\". It may have been done all the same: look before "
                                             "sending it again.")
                                           .arg(machine.name, ask.title)
                                     : words("%1: %2").arg(machine.name, QString::fromStdString(error.message)));
        if (error.code == "changed") {
            // A saved action is not what it was when the menu listed it: that list is out of date.
            close();
            askForSaved(*sentTo.place, sentTo.source);
        }
        return;
    }
    // ("already on": left as it was, and that is no failure.)
    const QString& note = answer.value().note;
    notices.tell(Notices::Kind::Done, words("%1: %2").arg(machine.name, note.isEmpty() ? ask.done : note));
    // How the machine is now is worth knowing before the wall's next look at it.
    m_machines.lookAgainSoon(machine.id);
    if (ask.action.kind == "reboot"_L1) {
        emit restarted(machine.id);
        close();
    }
    if (ask.action.kind == "resume"_L1) {
        // Every window that shows it, and stopped on its "in use", asks for it again (MergedWall).
        emit resumed(machine.id);
    }
}

} // namespace qkvm::ui
