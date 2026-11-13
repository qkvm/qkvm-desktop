#include "machine_menu.h"

#include <algorithm>
#include <array>
#include <utility>

#include <QCoreApplication>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/own_actions.h>

#include "wall_cards.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// The words of the menu are the page's, and are translated where the page's are: under one heading.
QString words(const char* text)
{
    return QCoreApplication::translate("Actions", text);
}

// Shortcuts worth one click, as the page offers them. `confirm` asks first.
struct QuickKey {
    const char* icon;
    const char* label;
    const char* keys;
    bool confirm = false;
    bool danger = false;
};
constexpr std::array quickKeys{
    QuickKey{"sun", QT_TRANSLATE_NOOP("Actions", "Wake screen"), "ShiftLeft"},
    QuickKey{"command", QT_TRANSLATE_NOOP("Actions", "Ctrl+Alt+Del"), "ControlLeft,AltLeft,Delete", true},
    QuickKey{"lock", QT_TRANSLATE_NOOP("Actions", "Lock screen"), "MetaLeft,KeyL"},
    QuickKey{"activity", QT_TRANSLATE_NOOP("Actions", "Task Manager"), "ControlLeft,ShiftLeft,Escape"},
    QuickKey{"grid", QT_TRANSLATE_NOOP("Actions", "Show desktop"), "MetaLeft,KeyD"},
    QuickKey{"enter", QT_TRANSLATE_NOOP("Actions", "Enter"), "Enter"},
    QuickKey{"chevronLeft", QT_TRANSLATE_NOOP("Actions", "Esc"), "Escape"},
    QuickKey{"x", QT_TRANSLATE_NOOP("Actions", "Close window"), "AltLeft,F4", true, true},
};

// What a row sends and says, where it is sent at once and asks nothing of its own.
Ask asking(const QString& title, const core::Action& action, const QString& done, const QString& who)
{
    Ask ask;
    ask.title = title;
    ask.action = action;
    ask.done = done;
    ask.question = words("%1 on %2?").arg(title, who);
    return ask;
}

// A power button: always asked about first, and told what it does.
Ask pressing(const QString& title, const char* action, const QString& done, const QString& warning, bool danger,
             const QString& who)
{
    Ask ask = asking(title, core::Action::plain(QString::fromLatin1(action)), done, who);
    ask.confirm = true;
    ask.danger = danger;
    ask.tick = true;
    ask.warning = warning;
    return ask;
}

// The row that sends it: called what it sends is called.
MenuRow row(const QString& id, const char* icon, Ask ask, const QString& hint = {})
{
    MenuRow made;
    made.id = id;
    made.icon = QString::fromLatin1(icon);
    made.label = ask.title;
    made.hint = hint;
    made.danger = ask.danger;
    made.ask = std::move(ask);
    return made;
}

MenuSection portsOf(const core::Machine& machine, const core::Status& status)
{
    MenuSection section;
    section.title = words("Computer on screen");
    for (int index = 0; index < status.ports.size(); ++index) {
        const core::Port& port = status.ports[index];
        const bool onScreen = index == status.shownPort;
        MenuRow made;
        made.id = u"port:%1"_s.arg(index);
        made.icon = u"system"_s;
        made.label = portName(port, index);
        // (Its number, where it has a name of its own. One without a name is called by its number already.)
        made.hint = onScreen               ? words("on screen")
                    : port.empty           ? words("empty")
                    : !port.name.isEmpty() ? words("port %1").arg(index + 1)
                                           : words("switch");
        made.enabled = !onScreen;
        made.ask = askToShow(machine, status, index);
        section.rows.append(made);
    }
    section.note = words("This KVM shows one computer at a time. Switching takes a few seconds and also changes a "
                         "monitor plugged into the KVM. Keys and text go to the computer on screen.");
    return section;
}

MenuSection keyboardOf(const QString& who)
{
    MenuSection section;
    section.title = words("Keyboard");
    for (const QuickKey& key : quickKeys) {
        const QString keys = QString::fromLatin1(key.keys);
        const QString label = words(key.label);
        const QString pressed = keysLabel(keys);
        Ask ask = asking(label, core::Action::press(keys), words("%1 sent").arg(pressed), who);
        ask.confirm = key.confirm;
        ask.danger = key.danger;
        // The keys are shown next to the name, unless the name already is the keys.
        section.rows.append(row(u"keys:"_s + keys, key.icon, ask, pressed == label ? QString() : pressed));
    }
    // And what is chosen or typed in a box of its own.
    MenuRow choose;
    choose.id = u"sendKeys"_s;
    choose.icon = u"command"_s;
    choose.label = words("Send keys…");
    section.rows.append(choose);
    MenuRow type;
    type.id = u"typeText"_s;
    type.icon = u"type"_s;
    type.label = words("Type text…");
    section.rows.append(type);
    return section;
}

QVariantMap choice(const QString& value, const QString& label)
{
    return {{u"value"_s, value}, {u"label"_s, label}};
}

MenuSection savedOf(const QList<core::SavedAction>& saved, const QString& who)
{
    MenuSection section;
    section.title = words("Your actions");
    for (const core::SavedAction& action : saved) {
        Ask ask = asking(action.name, core::Action::run(action), words("%1 sent").arg(action.name), who);
        // Typing text into the wrong window can do anything, so that always asks first.
        ask.confirm = action.danger || action.types;
        ask.danger = action.danger;
        ask.tick = action.types;
        section.rows.append(row(u"saved:"_s + action.id, action.types ? "type" : "star", ask,
                                action.types ? QString() : keysLabel(action.keys)));
    }
    if (saved.isEmpty()) {
        section.note = words("None yet. An admin can add some on the page, under Settings → Actions.");
    }
    return section;
}

// Whether the server lists this word in what the machine can be asked (`can`, the status's fresher than the
// wall's), and does not merely leave it unsaid. Where nothing says what it can be asked (this PC's own Comet: its
// driver says no `can`), a Fingerbot is there where its driver heard the KVM say one is paired (Status::fingerbot).
bool lists(const core::Machine& machine, const std::optional<core::Status>& status, const QString& word)
{
    const std::optional<QStringList>& can =
        status && status->capabilities ? status->capabilities : machine.capabilities;
    if (!can) {
        return word == "fingerbot"_L1 && status && status->fingerbot.has_value();
    }
    return can->contains(word);
}

// A Fingerbot paired with a KVM presses its power button with an arm: under its own name, beside a power board's
// buttons where the KVM has both (the board's stay the power buttons). Nothing of the computer's power comes from
// it: it presses, and the card shows what follows.
void fingerbotOf(MenuSection& section, const QString& who)
{
    section.rows.append(
        row(u"power:fingerbot_power"_s, "power",
            pressing(words("Press with Fingerbot"), "fingerbot_power", words("pressed by the Fingerbot"),
                     words("The Fingerbot presses the power button for half a second: a computer that is off comes "
                           "on, a running system is asked to shut down."),
                     false, who)));
    section.rows.append(
        row(u"power:fingerbot_power_long"_s, "power",
            pressing(words("Hold with Fingerbot"), "fingerbot_power_long", words("held by the Fingerbot"),
                     words("The Fingerbot holds the power button for 8 seconds, which cuts the power. "
                           "Unsaved work is lost."),
                     true, who)));
}

// Start into the BIOS setup, once, then on (where it is off) or restart (where it is on): one machine at a time, asked
// first with the page's question (app.js, bootIntoSetup). Its large view opens to show the setup screen from the
// start (MachineActions::send).
MenuRow setupOf(const QString& who, bool on)
{
    Ask ask = asking(words("Start into BIOS setup"), core::Action::plain(u"bios_setup"_s),
                     words("starting into BIOS setup"), who);
    ask.confirm = true;
    ask.danger = on;
    ask.question = on ? words("%1 will restart now and start straight into its BIOS setup screen, once. Any unsaved "
                              "work on it is lost. Its large view opens so you can watch the setup.")
                            .arg(who)
                      : words("%1 will be switched on and start straight into its BIOS setup screen, once. Its large "
                              "view opens so you can watch the setup.")
                            .arg(who);
    ask.yes = on ? words("Restart into setup") : words("Switch on into setup");
    MenuRow made = row(u"power:bios_setup"_s, "sliders", ask, on ? words("restart") : words("power on"));
    // (Shown as one that can lose work either way, as the page's button is.)
    made.danger = true;
    return made;
}

MenuSection powerOf(const MenuOf& of, const core::Status& status)
{
    const bool isServer = !of.machine.hasPicture;
    const bool bot = lists(of.machine, of.status, u"fingerbot"_s);
    const QString& who = of.machine.name;
    MenuSection section;
    section.title = words("Power");
    if (!isServer) {
        section.rows.append(
            row(u"power:jiggler_on"_s, "coffee",
                asking(words("Keep awake"), core::Action::plain(u"jiggler_on"_s), words("mouse jiggler on"), who)));
        section.rows.append(
            row(u"power:jiggler_off"_s, "moon",
                asking(words("Allow sleep"), core::Action::plain(u"jiggler_off"_s), words("mouse jiggler off"), who)));
    }
    if (status.powerIsKnown) {
        // A server's controller knows whether the server is on: it is asked for what is wanted, and
        // leaves alone a server that is there already. A server is offered the one that applies.
        if (isServer && status.power != std::optional<bool>(true)) {
            section.rows.append(row(
                u"power:power_on"_s, "power",
                pressing(words("Power on"), "power_on", words("switched on"),
                         words("Switches on what is off. A server that is on already is left as it is."), false, who)));
        }
        if (isServer && status.power != std::optional<bool>(false)) {
            section.rows.append(
                row(u"power:power_off"_s, "moon",
                    pressing(
                        words("Shut down"), "power_off", words("asked to shut down"),
                        words("Asks the operating system to shut down. A server that is off already is left as it is."),
                        false, who)));
        }
        // A KVM presses a button, and what that does depends on whether the computer is on, which it
        // cannot vouch for.
        if (!isServer) {
            section.rows.append(
                row(u"power:atx_power"_s, "power",
                    pressing(words("Press power button"), "atx_power", words("power button pressed"),
                             words("A short press turns the computer on, or asks a running system to shut down."),
                             false, who)));
        }
        section.rows.append(row(u"power:atx_power_long"_s, "power",
                                pressing(words("Hold power button"), "atx_power_long", words("power button held"),
                                         words("A long press cuts the power. Unsaved work is lost."), true, who)));
        section.rows.append(row(u"power:atx_reset"_s, "rotate",
                                pressing(words("Press reset button"), "atx_reset", words("reset button pressed"),
                                         words("A hard reset. Unsaved work is lost."), true, who)));
        // Where the server says the machine offers it (a PC with Intel AMT, a server over Redfish). Never in a menu for
        // several (menuForSeveral): one machine at a time.
        if (lists(of.machine, of.status, u"bios_setup"_s)) {
            section.rows.append(setupOf(who, status.power != std::optional<bool>(false)));
        }
    }
    if (bot) {
        fingerbotOf(section, who);
    }
    if (!status.powerIsKnown && !bot) {
        section.note = words("Power and reset buttons appear here when the KVM has an ATX power board, or a Fingerbot "
                             "paired with it.");
    }
    for (const core::WakeTarget& target : of.wake) {
        Ask ask = asking(words("Wake %1").arg(target.name.isEmpty() ? target.mac : target.name),
                         core::Action::wake(target.mac), words("Wake-on-LAN sent"), who);
        ask.confirm = true;
        section.rows.append(row(u"wake:"_s + target.mac, "activity", ask));
    }
    return section;
}

MenuSection lightOf(bool lit, const QString& who)
{
    MenuSection section;
    section.title = words("Find it in the rack");
    // The one that applies.
    section.rows.append(lit ? row(u"light:off"_s, "moon",
                                  asking(words("Locator light off"), core::Action::plain(u"locate_off"_s),
                                         words("locator light off"), who))
                            : row(u"light:on"_s, "sun",
                                  asking(words("Locator light on"), core::Action::plain(u"locate_on"_s),
                                         words("locator light on"), who)));
    section.note =
        words("The light on the machine itself, front and back, for whoever is standing in front of the rack.");
    return section;
}

// A PC with Intel AMT: its three views, as the page's Actions box has them (app.js, "View"): the one it
// is in said "now" and not offered. Each row leads to the view's own call (MachineActions). Nothing
// for any other machine.
std::optional<MenuSection> viewsOf(const core::Machine& machine, const core::Status* status)
{
    const QString now = core::amtViewOf(machine, status);
    if (now.isEmpty()) {
        return std::nullopt;
    }
    MenuSection section;
    section.title = words("View");
    const QList<std::pair<QString, QString>> views{
        {u"health"_s, u"activity"_s}, {u"stills"_s, u"camera"_s}, {u"live"_s, u"tv"_s}};
    for (const auto& [view, icon] : views) {
        MenuRow made;
        made.id = u"view:"_s + view;
        made.icon = icon;
        made.label = amtViewWord(view);
        made.enabled = view != now;
        made.hint = view == now ? words("now") : QString();
        section.rows.append(made);
    }
    section.note = words("Health only opens no KVM session: Intel's border never shows on the PC, and its screen, "
                         "keys and text are not offered. Stills take a picture every five minutes (the border shows "
                         "for a few seconds); the large view is live. Live KVM holds a session while the tile is "
                         "shown: the border stays on the PC's monitor.");
    return section;
}

MenuSection itselfOf(bool isServer, const QString& who)
{
    MenuSection section;
    section.title = isServer ? words("This controller") : words("This KVM");
    Ask ask = asking(isServer ? words("Restart controller") : words("Reboot KVM"), core::Action::plain(u"reboot"_s),
                     isServer ? words("restarting") : words("rebooting"), who);
    ask.confirm = true;
    ask.danger = true;
    ask.question = isServer ? words("Restart the management controller of %1?").arg(who) : words("Reboot %1?").arg(who);
    ask.warning =
        isServer ? words("This restarts the management controller, not the server, which keeps running. The "
                         "controller is away for a minute or two.")
                 : words("This restarts the KVM itself, not the computer. Anyone using its console is disconnected "
                         "for about a minute.");
    section.rows.append(row(u"reboot"_s, "rotate", ask));
    return section;
}

} // namespace

QString keysLabel(const QString& keys)
{
    static const QHash<QString, QString> labels{
        {u"ControlLeft"_s, u"Ctrl"_s}, {u"ControlRight"_s, u"Ctrl"_s}, {u"AltLeft"_s, u"Alt"_s},
        {u"AltRight"_s, u"AltGr"_s},   {u"ShiftLeft"_s, u"Shift"_s},   {u"ShiftRight"_s, u"Shift"_s},
        {u"MetaLeft"_s, u"Win"_s},     {u"MetaRight"_s, u"Win"_s},     {u"Escape"_s, u"Esc"_s},
        {u"Delete"_s, u"Del"_s},       {u"Insert"_s, u"Ins"_s},        {u"PageUp"_s, u"PgUp"_s},
        {u"PageDown"_s, u"PgDn"_s},    {u"ArrowUp"_s, u"↑"_s},         {u"ArrowDown"_s, u"↓"_s},
        {u"ArrowLeft"_s, u"←"_s},      {u"ArrowRight"_s, u"→"_s},      {u"PrintScreen"_s, u"PrtSc"_s},
    };
    QStringList read;
    const QStringList codes = keys.split(u',', Qt::SkipEmptyParts);
    for (const QString& code : codes) {
        if (const auto known = labels.constFind(code); known != labels.constEnd()) {
            read.append(*known);
        } else if (code.startsWith("Key"_L1) && code.size() > 3) {
            read.append(code.mid(3));
        } else if (code.startsWith("Digit"_L1) && code.size() > 5) {
            read.append(code.mid(5));
        } else {
            read.append(code);
        }
    }
    return read.join(u'+');
}

std::optional<Ask> askToShow(const core::Machine& machine, const core::Status& status, int port)
{
    if (port < 0 || port >= status.ports.size()) {
        return std::nullopt;
    }
    const QString name = portName(status.ports[port], port);
    return asking(words("Show %1").arg(name), core::Action::show(port), words("showing %1").arg(name), machine.name);
}

QString severalKvms(qsizetype count)
{
    return words("%1 KVMs").arg(count);
}

std::optional<Ask> askToPress(const QString& who, const QString& keys)
{
    // Up to eight key names, as the server takes them: "ControlLeft,AltLeft,Delete".
    static const QRegularExpression areKeys(u"^[A-Z][A-Za-z0-9]{1,15}(,[A-Z][A-Za-z0-9]{1,15}){0,7}$"_s);
    // Nor the keyboard's Power key, wherever it would go: the server refuses it (422), and so does
    // this PC's wall (kvmd::callFor).
    if (!areKeys.match(keys).hasMatch() || core::kvmd::namesThePowerKey(keys)) {
        return std::nullopt;
    }
    const QString pressed = keysLabel(keys);
    Ask ask = asking(words("Send %1").arg(pressed), core::Action::press(keys), words("%1 sent").arg(pressed), who);
    ask.confirm = true;
    return ask;
}

std::optional<Ask> askToType(const QString& who, const QString& text, bool enter)
{
    if (text.isEmpty() || !text.isValidUtf16() || !core::action::isTypedAsOne(text + (enter ? u"\n"_s : QString()))) {
        return std::nullopt;
    }
    Ask ask = asking(words("Type text"), core::Action::type(text, enter), words("text typed"), who);
    ask.confirm = true;
    ask.tick = true;
    ask.question = words("Type this on %1?").arg(who);
    ask.detail = enter ? text + u" ⏎"_s : text;
    return ask;
}

QVariantList heldKeys()
{
    return {choice(u"ControlLeft"_s, words("Ctrl")), choice(u"AltLeft"_s, words("Alt")),
            choice(u"ShiftLeft"_s, words("Shift")), choice(u"MetaLeft"_s, words("Win"))};
}

QVariantList otherKeys()
{
    QVariantList keys{choice(QString(), words("No other key"))};
    for (char16_t letter = u'A'; letter <= u'Z'; ++letter) {
        keys.append(choice(u"Key"_s + QChar(letter), QString(QChar(letter))));
    }
    for (char16_t digit = u'0'; digit <= u'9'; ++digit) {
        keys.append(choice(u"Digit"_s + QChar(digit), QString(QChar(digit))));
    }
    for (int number = 1; number <= 12; ++number) {
        keys.append(choice(u"F%1"_s.arg(number), u"F%1"_s.arg(number)));
    }
    for (const char* code : {"Enter", "Escape", "Tab", "Space", "Backspace", "Delete", "Insert", "Home", "End",
                             "PageUp", "PageDown", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "PrintScreen"}) {
        const QString key = QString::fromLatin1(code);
        keys.append(choice(key, keysLabel(key)));
    }
    return keys;
}

const MenuRow* Menu::row(const QString& id) const
{
    for (const MenuSection& section : sections) {
        for (const MenuRow& one : section.rows) {
            if (one.id == id) {
                return &one;
            }
        }
    }
    return nullptr;
}

QString abilityFor(const core::Action& action)
{
    const QString& kind = action.kind;
    if (kind == "custom"_L1)
        return action.savedTypes ? u"text"_s : u"keys"_s;
    if (kind == "ctrl_alt_del"_L1)
        return u"keys"_s;
    if (kind == "atx_power"_L1 || kind == "atx_power_long"_L1)
        return u"power"_s;
    if (kind == "power_on"_L1 || kind == "power_off"_L1)
        return u"power_state"_s;
    if (kind == "atx_reset"_L1)
        return u"reset"_s;
    if (kind == "fingerbot_power"_L1 || kind == "fingerbot_power_long"_L1)
        return u"fingerbot"_s;
    if (kind.startsWith("jiggler_"_L1))
        return u"jiggler"_s;
    if (kind.startsWith("locate_"_L1))
        return u"locator"_s;
    if (kind == "port"_L1)
        return u"ports"_s;
    return kind;
}

namespace {
// Whether the server says what this machine can be asked (`can`, the status's fresher than the wall's). Where it
// does, that word is all that is offered; where it does not (an older server, this PC's own wall), the machine is
// taken by whether it shows a picture of its own.
bool saysWhatItCan(const MenuOf& of)
{
    return of.machine.capabilities || (of.status && of.status->capabilities);
}

// Whether it takes keys or a text: by `can` where the server says it, as the page has it (able('keys'),
// able('text')): a server's console, a PC with Intel AMT and a Proxmox guest show no picture of a KVM's and take
// both. Otherwise a machine with a picture of its own: a server's controller has a power button and no keyboard.
bool takesKeys(const MenuOf& of)
{
    if (!saysWhatItCan(of)) {
        return of.machine.hasPicture;
    }
    const core::Status* status = of.status ? &*of.status : nullptr;
    return core::supports(of.machine, status, u"keys"_s) || core::supports(of.machine, status, u"text"_s);
}

bool supportsRow(const MenuOf& of, const MenuRow& row)
{
    QString ability;
    if (row.ask)
        ability = abilityFor(row.ask->action);
    else if (row.id == "sendKeys"_L1)
        ability = u"keys"_s;
    else if (row.id == "typeText"_L1)
        ability = u"text"_s;
    return ability.isEmpty() || core::supports(of.machine, of.status ? &*of.status : nullptr, ability);
}

void limitToCapabilities(Menu& menu, const QList<MenuOf>& targets)
{
    for (auto& section : menu.sections) {
        section.rows.removeIf([&](const MenuRow& row) {
            return !std::ranges::any_of(targets, [&](const MenuOf& of) { return supportsRow(of, row); });
        });
    }
    menu.sections.removeIf([](const MenuSection& section) { return section.rows.isEmpty(); });
}
} // namespace

Menu menuFor(const MenuOf& of)
{
    const core::Machine& machine = of.machine;
    const core::Status status = of.status.value_or(core::Status{});
    const bool isServer = !machine.hasPicture;

    Menu menu;
    menu.title = machine.name;
    QStringList about{machine.host, status.model, status.version};
    about.removeAll(QString());
    menu.about = about.join(u" · "_s);
    // How it is, in the words its card has for it.
    const WallTile card = cardFor(machine, of.status ? &*of.status : nullptr, 0, false);
    if (card.state != "online"_L1) {
        menu.warning = card.text.isEmpty() ? card.label : card.text;
    }
    // The battery of its Fingerbot, where that is low: a press may fail.
    if (lists(machine, of.status, u"fingerbot"_s) && status.fingerbot && status.fingerbot->low &&
        status.fingerbot->battery) {
        menu.battery = words("The battery of its Fingerbot is low (%1 %). A press may fail: charge it.")
                           .arg(*status.fingerbot->battery);
    }
    if (machine.kind == "jetkvm"_L1) {
        // As the page says it. A server types in the layout the JetKVM says its computer uses (its W14d), and so does
        // this program on a JetKVM of its own (jetkvm_layouts.h).
        menu.kindNote = words("A JetKVM shows its picture to one viewer at a time. Opening its console takes the "
                              "picture from this wall until you choose Show here on the tile. Text is typed in the "
                              "keyboard layout set on its own page.");
    }

    // The rows at the top lead elsewhere: into the machine's own console, in a browser, to the
    // machine shown large, and to the box that changes it.
    MenuSection ways;
    if (!machine.page.isEmpty()) {
        MenuRow console;
        console.id = u"console"_s;
        console.icon = u"external"_s;
        console.label = words("Open console");
        // Straight in, where the machine's controller hands out a pass for that.
        console.hint =
            status.opensSignedIn && core::supports(machine, &status, u"console"_s) ? words("signed in") : QString();
        ways.rows.append(console);
    }
    if (!of.enlarged) {
        MenuRow enlarge;
        enlarge.id = u"enlarge"_s;
        enlarge.icon = u"maximize"_s;
        enlarge.label = words("Enlarge");
        ways.rows.append(enlarge);
    }
    // A KVM of this PC's own wall is changed, and taken off the wall, in a box of this program's.
    if (machine.own) {
        MenuRow edit;
        edit.id = u"edit"_s;
        edit.icon = u"sliders"_s;
        edit.label = words("Edit…");
        ways.rows.append(edit);
    }
    // Its virtual media, in a box of their own (MediaView), as the page's "Media...": for whoever may have them (an
    // admin of its qKVM, in person: MenuOf::media), where the server says the machine has some. An older server, which
    // does not say what a machine can, has none.
    if (of.media && lists(machine, of.status, u"media"_s)) {
        MenuRow media;
        media.id = u"media"_s;
        media.icon = u"disc"_s;
        media.label = words("Media…");
        ways.rows.append(media);
    }
    if (!ways.rows.isEmpty()) {
        menu.sections.append(ways);
    }
    if (auto views = viewsOf(machine, of.status ? &*of.status : nullptr)) {
        menu.sections.append(*views);
    }
    if (!status.ports.isEmpty()) {
        menu.sections.append(portsOf(machine, status));
    }
    // What takes keys or a text has a keyboard, and the actions saved for it (takesKeys).
    if (takesKeys(of)) {
        menu.sections.append(keyboardOf(machine.name));
        menu.sections.append(savedOf(of.saved, machine.name));
    }
    menu.sections.append(powerOf(of, status));
    // Only a machine that has a light to find it by.
    if (const std::optional<bool> light = status.server ? status.server->locator : std::nullopt; light) {
        menu.sections.append(lightOf(*light, machine.name));
    }
    menu.sections.append(itselfOf(isServer, machine.name));
    // Leave old servers/direct adapters unchanged. Explicit lists, including empty, are authoritative.
    if (saysWhatItCan(of))
        limitToCapabilities(menu, {of});
    return menu;
}

Menu menuForSeveral(const QList<MenuOf>& of)
{
    const QString who = severalKvms(of.size());
    const auto isServer = [](const MenuOf& one) { return !one.machine.hasPicture; };
    const auto statusOf = [](const MenuOf& one) { return one.status.value_or(core::Status{}); };
    // With nothing but servers picked, what only a KVM can do is left out; in a mixed pick each
    // machine answers for itself.
    const bool servers = std::ranges::all_of(of, isServer);
    const bool anyServer = std::ranges::any_of(of, isServer);
    const bool hasAtx = std::ranges::any_of(of, [&](const MenuOf& one) { return statusOf(one).powerIsKnown; });
    const bool kvmAtx =
        std::ranges::any_of(of, [&](const MenuOf& one) { return !isServer(one) && statusOf(one).powerIsKnown; });
    const bool lights = std::ranges::any_of(of, [&](const MenuOf& one) {
        const core::Status status = statusOf(one);
        return status.server && status.server->locator.has_value();
    });

    Menu menu;
    menu.title = words("%1 KVMs selected").arg(of.size());
    menu.about = words("The action you choose runs on every selected KVM. You are asked to confirm first.");
    if (std::ranges::any_of(of, takesKeys)) {
        menu.sections.append(keyboardOf(who));
        // The saved actions are the server's, the same for every machine; none, and no place for
        // them, where the machines are of more than one source.
        if (of.isEmpty() || !of.constFirst().savedLeftOut) {
            menu.sections.append(savedOf(of.isEmpty() ? QList<core::SavedAction>() : of.constFirst().saved, who));
        }
    }

    MenuSection power;
    power.title = words("Power");
    if (!servers) {
        power.rows.append(
            row(u"power:jiggler_on"_s, "coffee",
                asking(words("Keep awake"), core::Action::plain(u"jiggler_on"_s), words("mouse jiggler on"), who)));
        power.rows.append(
            row(u"power:jiggler_off"_s, "moon",
                asking(words("Allow sleep"), core::Action::plain(u"jiggler_off"_s), words("mouse jiggler off"), who)));
    }
    if (hasAtx) {
        if (anyServer) {
            // Some of them may be on and some off: both are offered, and a server that is there
            // already is left as it is.
            power.rows.append(row(
                u"power:power_on"_s, "power",
                pressing(servers ? words("Power on") : words("Power on (servers)"), "power_on", words("switched on"),
                         words("Switches on what is off. A server that is on already is left as it is."), false, who)));
            power.rows.append(
                row(u"power:power_off"_s, "moon",
                    pressing(
                        servers ? words("Shut down") : words("Shut down (servers)"), "power_off",
                        words("asked to shut down"),
                        words("Asks the operating system to shut down. A server that is off already is left as it is."),
                        false, who)));
        }
        if (kvmAtx) {
            power.rows.append(
                row(u"power:atx_power"_s, "power",
                    pressing(words("Press power button"), "atx_power", words("power button pressed"),
                             words("A short press turns the computer on, or asks a running system to shut down."),
                             false, who)));
        }
        power.rows.append(row(u"power:atx_power_long"_s, "power",
                              pressing(words("Hold power button"), "atx_power_long", words("power button held"),
                                       words("A long press cuts the power. Unsaved work is lost."), true, who)));
        power.rows.append(row(u"power:atx_reset"_s, "rotate",
                              pressing(words("Press reset button"), "atx_reset", words("reset button pressed"),
                                       words("A hard reset. Unsaved work is lost."), true, who)));
    }
    const bool bots =
        std::ranges::any_of(of, [](const MenuOf& one) { return lists(one.machine, one.status, u"fingerbot"_s); });
    if (bots) {
        fingerbotOf(power, who);
    }
    if (!hasAtx && !bots) {
        power.note =
            words("None of the selected KVMs has an ATX power board or a Fingerbot, so there are no power buttons.");
    }
    menu.sections.append(power);

    if (lights) {
        // A selection may hold some of each: both.
        MenuSection light;
        light.title = words("Find it in the rack");
        light.rows.append(row(
            u"light:on"_s, "sun",
            asking(words("Locator light on"), core::Action::plain(u"locate_on"_s), words("locator light on"), who)));
        light.rows.append(row(
            u"light:off"_s, "moon",
            asking(words("Locator light off"), core::Action::plain(u"locate_off"_s), words("locator light off"), who)));
        light.note =
            words("The light on the machine itself, front and back, for whoever is standing in front of the rack.");
        menu.sections.append(light);
    }

    MenuSection itself = itselfOf(servers, who);
    itself.title = servers ? words("The controllers themselves") : words("The KVMs themselves");
    menu.sections.append(itself);
    if (std::ranges::any_of(of, saysWhatItCan))
        limitToCapabilities(menu, of);
    return menu;
}

QVariantMap shown(const Menu& menu)
{
    QVariantList sections;
    for (const MenuSection& section : menu.sections) {
        QVariantList rows;
        for (const MenuRow& one : section.rows) {
            rows.append(QVariantMap{
                {u"id"_s, one.id},
                {u"icon"_s, one.icon},
                {u"label"_s, one.label},
                {u"hint"_s, one.hint},
                {u"danger"_s, one.danger},
                {u"enabled"_s, one.enabled},
            });
        }
        sections.append(QVariantMap{{u"title"_s, section.title}, {u"note"_s, section.note}, {u"rows"_s, rows}});
    }
    return {
        {u"title"_s, menu.title},       {u"about"_s, menu.about},     {u"warning"_s, menu.warning},
        {u"kindNote"_s, menu.kindNote}, {u"battery"_s, menu.battery}, {u"sections"_s, sections},
    };
}

QVariantMap shown(const Ask& ask)
{
    return {
        {u"title"_s, ask.title},        {u"text"_s, ask.question}, {u"detail"_s, ask.detail},
        {u"warning"_s, ask.warning},    {u"danger"_s, ask.danger}, {u"yes"_s, ask.yes.isEmpty() ? ask.title : ask.yes},
        {u"targets"_s, QVariantList()}, {u"tick"_s, QString()},    {u"paces"_s, QVariantList()},
    };
}

QVariantMap shown(const Ask& ask, const QList<Target>& targets)
{
    QVariantMap question = shown(ask);
    QVariantList listed;
    for (const Target& target : targets) {
        listed.append(QVariantMap{{u"name"_s, target.name},
                                  {u"host"_s, target.host},
                                  {u"state"_s, target.state},
                                  {u"label"_s, target.label}});
    }
    question[u"targets"_s] = listed;
    question[u"yes"_s] = words("Run on %1").arg(severalKvms(targets.size()));
    if (ask.danger || ask.tick) {
        question[u"tick"_s] = words("Yes, do this on all %1 machines").arg(targets.size());
    }
    const auto pace = [](int seconds, const QString& label) {
        return QVariantMap{{u"value"_s, seconds}, {u"label"_s, label}};
    };
    question[u"paces"_s] = QVariantList{
        pace(-1, words("A few at a time")),
        pace(0, words("One after the other")),
        pace(10, words("One after the other, 10 seconds apart")),
        pace(30, words("One after the other, 30 seconds apart")),
        pace(120, words("One after the other, 2 minutes apart")),
    };
    return question;
}

} // namespace qkvm::ui
