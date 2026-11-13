#include <algorithm>
#include <optional>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/server_types.h>

#include "machine_menu.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using ui::Menu;
using ui::MenuOf;
using ui::MenuRow;
using ui::MenuSection;

// What can be done to a machine: which rows a machine's menu has, what each sends, and what is
// asked first. The words are the page's (app/qkvm/static/app.js, actionsDialog).
class TestMachineMenu : public QObject {
    Q_OBJECT

private:
    static core::Machine kvm()
    {
        core::Machine machine;
        machine.id = u"k1"_s;
        machine.name = u"NAS"_s;
        machine.host = u"192.168.1.22"_s;
        machine.kind = u"kvmd"_s;
        return machine;
    }

    static core::Machine server()
    {
        core::Machine machine;
        machine.id = u"s1"_s;
        machine.name = u"Rack server"_s;
        machine.host = u"192.168.1.60"_s;
        machine.kind = u"redfish"_s;
        machine.hasPicture = false;
        return machine;
    }

    static core::Status online()
    {
        core::Status status;
        status.state = u"online"_s;
        return status;
    }

    // A server's status, as its controller gives it: it knows whether the server is on.
    static core::Status serverThatIs(std::optional<bool> on, std::optional<bool> lit)
    {
        core::Status status = online();
        status.hasPicture = false;
        status.powerIsKnown = true;
        status.power = on;
        core::ServerReadings readings;
        readings.health = u"OK"_s;
        readings.locator = lit;
        status.server = readings;
        return status;
    }

    static QStringList titles(const Menu& menu)
    {
        QStringList all;
        for (const MenuSection& section : menu.sections) {
            all.append(section.title);
        }
        return all;
    }

    static QStringList rows(const Menu& menu, const QString& title)
    {
        QStringList all;
        for (const MenuSection& section : menu.sections) {
            for (const MenuRow& row : section.rows) {
                if (section.title == title) {
                    all.append(row.id);
                }
            }
        }
        return all;
    }

    static const MenuSection* section(const Menu& menu, const QString& title)
    {
        for (const MenuSection& one : menu.sections) {
            if (one.title == title) {
                return &one;
            }
        }
        return nullptr;
    }

    // What a row sends, as it goes to the server.
    static QJsonObject sent(const Menu& menu, const QString& row)
    {
        const MenuRow* found = menu.row(row);
        if (found == nullptr || !found->ask) {
            return {};
        }
        return QJsonDocument::fromJson(core::bodyOf(found->ask.value_or(ui::Ask{}).action)).object();
    }

    static ui::Ask ask(const Menu& menu, const QString& row)
    {
        const MenuRow* found = menu.row(row);
        return found == nullptr ? ui::Ask{} : found->ask.value_or(ui::Ask{});
    }

private slots:
    void advertisedCapabilitiesLimitActionsAndFreshStatusOverridesInventory()
    {
        const auto wall = core::wallFrom(R"({"devices":[{"id":"one","kind":"kvmd","can":["keys","screen"]}]})");
        QVERIFY(wall);
        MenuOf of;
        of.machine = wall.value().machines.first();
        auto menu = ui::menuFor(of);
        QVERIFY(menu.row(u"sendKeys"_s) != nullptr);
        QVERIFY(menu.row(u"typeText"_s) == nullptr);
        QVERIFY(menu.row(u"power:jiggler_on"_s) == nullptr);
        const auto status = core::statusFrom(R"({"one":{"state":"online","can":["text"]}})");
        QVERIFY(status);
        of.status = status.value().value(u"one"_s);
        menu = ui::menuFor(of);
        QVERIFY(menu.row(u"sendKeys"_s) == nullptr);
        QVERIFY(menu.row(u"typeText"_s) != nullptr);
        const auto none = core::statusFrom(R"({"one":{"state":"online","can":[]}})");
        QVERIFY(none);
        of.status = none.value().value(u"one"_s);
        menu = ui::menuFor(of);
        QVERIFY(menu.row(u"typeText"_s) == nullptr);
        QVERIFY(ui::menuForSeveral({of, of}).row(u"typeText"_s) == nullptr);
    }

    void aKvmHasAKeyboardAndItsOwnRestart()
    {
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        of.status->model = u"KVM-A"_s;
        of.status->version = u"4.82"_s;
        const Menu menu = ui::menuFor(of);
        QCOMPARE(menu.title, u"NAS"_s);
        QCOMPARE(menu.about, u"192.168.1.22 · KVM-A · 4.82"_s);
        // It is simply there: nothing to warn of, and nothing to be known of its kind.
        QCOMPARE(menu.warning, QString());
        QCOMPARE(menu.kindNote, QString());
        QCOMPARE(titles(menu), (QStringList{QString(), u"Keyboard"_s, u"Your actions"_s, u"Power"_s, u"This KVM"_s}));
        QCOMPARE(rows(menu, QString()), QStringList{u"enlarge"_s});
        QCOMPARE(rows(menu, u"Keyboard"_s),
                 (QStringList{u"keys:ShiftLeft"_s, u"keys:ControlLeft,AltLeft,Delete"_s, u"keys:MetaLeft,KeyL"_s,
                              u"keys:ControlLeft,ShiftLeft,Escape"_s, u"keys:MetaLeft,KeyD"_s, u"keys:Enter"_s,
                              u"keys:Escape"_s, u"keys:AltLeft,F4"_s, u"sendKeys"_s, u"typeText"_s}));
        // The last two lead to a box of their own, and send nothing by themselves.
        QCOMPARE(menu.row(u"sendKeys"_s)->label, u"Send keys…"_s);
        QCOMPARE(menu.row(u"typeText"_s)->label, u"Type text…"_s);
        QVERIFY(!menu.row(u"sendKeys"_s)->ask.has_value());
        QVERIFY(!menu.row(u"typeText"_s)->ask.has_value());
        QCOMPARE(rows(menu, u"This KVM"_s), QStringList{u"reboot"_s});
        // Without a power board it keeps the computer awake and no more, and says why.
        QCOMPARE(rows(menu, u"Power"_s), (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s}));
        QCOMPARE(section(menu, u"Power"_s)->note,
                 u"Power and reset buttons appear here when the KVM has an ATX power board, or a Fingerbot paired with "
                 "it."_s);
        // No action of its own is saved yet.
        QVERIFY(rows(menu, u"Your actions"_s).isEmpty());
        QCOMPARE(section(menu, u"Your actions"_s)->note,
                 u"None yet. An admin can add some on the page, under Settings → Actions."_s);
    }

    void whatTakesKeysWithoutAPictureHasAKeyboardByWhatItCanDo()
    {
        // A server's console (AMI, OpenBMC), a PC with Intel AMT and a Proxmox guest show no picture of a KVM's and
        // take keys and text where the server says so (`can`): the keyboard and the saved actions are offered by that
        // word and not by the picture, as the page offers them (app.js, actionsDialog: able('keys'), able('text')).
        core::SavedAction pressing;
        pressing.id = u"p1"_s;
        pressing.name = u"Login screen"_s;
        pressing.keys = u"ControlLeft,AltLeft,Delete"_s;
        core::SavedAction typing;
        typing.id = u"t1"_s;
        typing.name = u"Who am I"_s;
        typing.types = true;
        const QStringList keyboard{u"keys:ShiftLeft"_s,     u"keys:ControlLeft,AltLeft,Delete"_s,
                                   u"keys:MetaLeft,KeyL"_s, u"keys:ControlLeft,ShiftLeft,Escape"_s,
                                   u"keys:MetaLeft,KeyD"_s, u"keys:Enter"_s,
                                   u"keys:Escape"_s,        u"keys:AltLeft,F4"_s,
                                   u"sendKeys"_s,           u"typeText"_s};
        for (const QString& kind : {u"redfish"_s, u"amt"_s, u"proxmox"_s}) {
            MenuOf of;
            of.machine = server();
            of.machine.kind = kind;
            // (A PC with Intel AMT as a server that has the views lists it: in one of them.)
            of.machine.amtView = kind == u"amt"_s ? u"stills"_s : QString();
            of.status = serverThatIs(true, std::nullopt);
            of.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"screen"_s,
                                                  u"keys"_s,  u"text"_s,  u"live"_s,        u"control"_s};
            of.saved = {pressing, typing};
            Menu menu = ui::menuFor(of);
            QVERIFY2(titles(menu).contains(u"Keyboard"_s), qPrintable(kind));
            QCOMPARE(rows(menu, u"Keyboard"_s), keyboard);
            QCOMPARE(rows(menu, u"Your actions"_s), (QStringList{u"saved:p1"_s, u"saved:t1"_s}));
            QCOMPARE(sent(menu, u"keys:ControlLeft,AltLeft,Delete"_s),
                     QJsonObject({{u"action"_s, u"keys"_s}, {u"keys"_s, u"ControlLeft,AltLeft,Delete"_s}}));

            // Keys and no text: no box to type in, and no saved text.
            of.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"keys"_s};
            menu = ui::menuFor(of);
            QVERIFY(rows(menu, u"Keyboard"_s).contains(u"sendKeys"_s));
            QVERIFY(!rows(menu, u"Keyboard"_s).contains(u"typeText"_s));
            QCOMPARE(rows(menu, u"Your actions"_s), QStringList{u"saved:p1"_s});
            // Text and no keys: the box to type in, and the saved text.
            of.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"text"_s};
            menu = ui::menuFor(of);
            QCOMPARE(rows(menu, u"Keyboard"_s), QStringList{u"typeText"_s});
            QCOMPARE(rows(menu, u"Your actions"_s), QStringList{u"saved:t1"_s});

            // Neither: no keyboard, and none of the actions, which press keys or type.
            of.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s};
            menu = ui::menuFor(of);
            QVERIFY(!titles(menu).contains(u"Keyboard"_s));
            QVERIFY(!titles(menu).contains(u"Your actions"_s));
            // And nothing else that it does not list: its controller's restart among them. (A PC with Intel AMT has
            // its three views besides, b5's: whatever it can.)
            QStringList shownTitles = titles(menu);
            QCOMPARE(shownTitles.removeAll(u"View"_s), kind == u"amt"_s ? 1 : 0);
            QCOMPARE(shownTitles, (QStringList{QString(), u"Power"_s}));
        }

        // A VNC server shows its picture and takes keys and text: its keyboard, and nothing it does not list (no
        // mouse jiggler, no restart of a KVM).
        MenuOf vnc;
        vnc.machine = kvm();
        vnc.machine.kind = u"vnc"_s;
        vnc.machine.capabilities = QStringList{u"screen"_s, u"keys"_s, u"text"_s, u"live"_s, u"control"_s};
        vnc.status = online();
        vnc.saved = {pressing, typing};
        const Menu shown = ui::menuFor(vnc);
        QCOMPARE(titles(shown), (QStringList{QString(), u"Keyboard"_s, u"Your actions"_s}));
        QCOMPARE(rows(shown, u"Keyboard"_s), keyboard);

        // Several of them: what any of them can.
        MenuOf one;
        one.machine = server();
        one.status = serverThatIs(true, std::nullopt);
        one.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"keys"_s};
        one.saved = {pressing, typing};
        MenuOf other = one;
        other.machine.id = u"s2"_s;
        other.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"text"_s};
        const Menu several = ui::menuForSeveral({one, other});
        QCOMPARE(rows(several, u"Keyboard"_s), keyboard);
        QCOMPARE(rows(several, u"Your actions"_s), (QStringList{u"saved:p1"_s, u"saved:t1"_s}));
        // And none of them: none.
        one.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s};
        other.status->capabilities = one.status->capabilities;
        QVERIFY(!titles(ui::menuForSeveral({one, other})).contains(u"Keyboard"_s));
    }

    void aKeyIsSentAsItIsCalledAndSomeAreAskedAboutFirst()
    {
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        const Menu menu = ui::menuFor(of);

        const MenuRow* wake = menu.row(u"keys:ShiftLeft"_s);
        QVERIFY(wake != nullptr);
        QCOMPARE(wake->label, u"Wake screen"_s);
        QCOMPARE(wake->icon, u"sun"_s);
        QCOMPARE(wake->hint, u"Shift"_s);
        QVERIFY(!wake->danger);
        QCOMPARE(sent(menu, u"keys:ShiftLeft"_s), (QJsonObject{{u"action"_s, u"keys"_s}, {u"keys"_s, u"ShiftLeft"_s}}));
        QCOMPARE(ask(menu, u"keys:ShiftLeft"_s).title, u"Wake screen"_s);
        QCOMPARE(ask(menu, u"keys:ShiftLeft"_s).done, u"Shift sent"_s);
        QVERIFY(!ask(menu, u"keys:ShiftLeft"_s).confirm);

        // The keys stand beside the name, unless the name already is the keys.
        QCOMPARE(menu.row(u"keys:ControlLeft,AltLeft,Delete"_s)->label, u"Ctrl+Alt+Del"_s);
        QCOMPARE(menu.row(u"keys:ControlLeft,AltLeft,Delete"_s)->hint, QString());
        QCOMPARE(menu.row(u"keys:MetaLeft,KeyL"_s)->hint, u"Win+L"_s);
        QCOMPARE(menu.row(u"keys:Enter"_s)->hint, QString());
        QCOMPARE(menu.row(u"keys:Escape"_s)->label, u"Esc"_s);
        QCOMPARE(menu.row(u"keys:Escape"_s)->hint, QString());

        // Ctrl+Alt+Del is asked about; closing a window too, and that one can lose work.
        const ui::Ask cad = ask(menu, u"keys:ControlLeft,AltLeft,Delete"_s);
        QVERIFY(cad.confirm);
        QVERIFY(!cad.danger);
        QCOMPARE(cad.question, u"Ctrl+Alt+Del on NAS?"_s);
        QCOMPARE(cad.done, u"Ctrl+Alt+Del sent"_s);
        const ui::Ask closing = ask(menu, u"keys:AltLeft,F4"_s);
        QVERIFY(closing.confirm);
        QVERIFY(closing.danger);
        QVERIFY(menu.row(u"keys:AltLeft,F4"_s)->danger);
        QCOMPARE(closing.question, u"Close window on NAS?"_s);
        QCOMPARE(closing.done, u"Alt+F4 sent"_s);
    }

    void keysAreReadAsAPersonReadsThem_data()
    {
        QTest::addColumn<QString>("keys");
        QTest::addColumn<QString>("read");
        QTest::newRow("three") << u"ControlLeft,AltLeft,Delete"_s << u"Ctrl+Alt+Del"_s;
        QTest::newRow("a letter") << u"MetaLeft,KeyL"_s << u"Win+L"_s;
        QTest::newRow("a digit") << u"ControlRight,Digit7"_s << u"Ctrl+7"_s;
        QTest::newRow("as it is") << u"F4"_s << u"F4"_s;
        QTest::newRow("arrows") << u"ShiftRight,ArrowUp"_s << u"Shift+↑"_s;
        QTest::newRow("the other Alt") << u"AltRight,KeyQ"_s << u"AltGr+Q"_s;
        QTest::newRow("a name that only begins so") << u"Key"_s << u"Key"_s;
        QTest::newRow("nothing") << QString() << QString();
    }

    void keysAreReadAsAPersonReadsThem()
    {
        QFETCH(const QString, keys);
        QFETCH(const QString, read);
        QCOMPARE(ui::keysLabel(keys), read);
    }

    void aKvmWithAPowerBoardHasTheComputersButtons()
    {
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        of.status->powerIsKnown = true;
        of.status->power = true;
        const Menu menu = ui::menuFor(of);
        // A KVM presses a button: it cannot vouch for whether the computer is on, so it is not
        // offered "Power on" or "Shut down".
        QCOMPARE(rows(menu, u"Power"_s),
                 (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s, u"power:atx_power"_s,
                              u"power:atx_power_long"_s, u"power:atx_reset"_s}));
        QCOMPARE(section(menu, u"Power"_s)->note, QString());

        // Keeping it awake is sent at once.
        QVERIFY(!ask(menu, u"power:jiggler_on"_s).confirm);
        QCOMPARE(sent(menu, u"power:jiggler_on"_s), QJsonObject({{u"action"_s, u"jiggler_on"_s}}));
        QCOMPARE(ask(menu, u"power:jiggler_on"_s).done, u"mouse jiggler on"_s);
        QCOMPARE(ask(menu, u"power:jiggler_off"_s).done, u"mouse jiggler off"_s);

        // Every power button is asked about first, and says what it does.
        const ui::Ask press = ask(menu, u"power:atx_power"_s);
        QCOMPARE(press.title, u"Press power button"_s);
        QVERIFY(press.confirm);
        QVERIFY(!press.danger);
        QCOMPARE(press.question, u"Press power button on NAS?"_s);
        QCOMPARE(press.warning, u"A short press turns the computer on, or asks a running system to shut down."_s);
        QCOMPARE(press.done, u"power button pressed"_s);
        QCOMPARE(sent(menu, u"power:atx_power"_s), QJsonObject({{u"action"_s, u"atx_power"_s}}));

        const ui::Ask hold = ask(menu, u"power:atx_power_long"_s);
        QVERIFY(hold.confirm);
        QVERIFY(hold.danger);
        QVERIFY(menu.row(u"power:atx_power_long"_s)->danger);
        QCOMPARE(hold.warning, u"A long press cuts the power. Unsaved work is lost."_s);
        QCOMPARE(hold.done, u"power button held"_s);
        QCOMPARE(sent(menu, u"power:atx_power_long"_s), QJsonObject({{u"action"_s, u"atx_power_long"_s}}));

        const ui::Ask reset = ask(menu, u"power:atx_reset"_s);
        QVERIFY(reset.confirm);
        QVERIFY(reset.danger);
        QCOMPARE(reset.warning, u"A hard reset. Unsaved work is lost."_s);
        QCOMPARE(reset.done, u"reset button pressed"_s);
        QCOMPARE(sent(menu, u"power:atx_reset"_s), QJsonObject({{u"action"_s, u"atx_reset"_s}}));
    }

    void aFingerbotPressesThePowerButtonWhereTheServerSaysThereIsOne()
    {
        // A Fingerbot paired with the KVM presses its power button with an arm (`fingerbot` in `can`): offered under
        // its own name, beside a power board's buttons where the KVM has both, in the page's words (app.js,
        // actionsDialog).
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        of.status->capabilities = QStringList{u"screen"_s, u"keys"_s, u"text"_s, u"jiggler"_s, u"fingerbot"_s};
        Menu menu = ui::menuFor(of);
        QCOMPARE(rows(menu, u"Power"_s), (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s,
                                                      u"power:fingerbot_power"_s, u"power:fingerbot_power_long"_s}));
        // (There are buttons: nothing is said of what would bring some.)
        QCOMPARE(section(menu, u"Power"_s)->note, QString());
        const ui::Ask press = ask(menu, u"power:fingerbot_power"_s);
        QCOMPARE(press.title, u"Press with Fingerbot"_s);
        QVERIFY(press.confirm);
        QVERIFY(press.tick);
        QVERIFY(!press.danger);
        QCOMPARE(press.question, u"Press with Fingerbot on NAS?"_s);
        QCOMPARE(press.warning, u"The Fingerbot presses the power button for half a second: a computer that is off "
                                "comes on, a running system is asked to shut down."_s);
        QCOMPARE(press.done, u"pressed by the Fingerbot"_s);
        QCOMPARE(sent(menu, u"power:fingerbot_power"_s), QJsonObject({{u"action"_s, u"fingerbot_power"_s}}));
        const ui::Ask hold = ask(menu, u"power:fingerbot_power_long"_s);
        QCOMPARE(hold.title, u"Hold with Fingerbot"_s);
        QVERIFY(hold.confirm);
        QVERIFY(hold.danger);
        QVERIFY(menu.row(u"power:fingerbot_power_long"_s)->danger);
        QCOMPARE(hold.warning,
                 u"The Fingerbot holds the power button for 8 seconds, which cuts the power. Unsaved work is lost."_s);
        QCOMPARE(hold.done, u"held by the Fingerbot"_s);
        QCOMPARE(sent(menu, u"power:fingerbot_power_long"_s), QJsonObject({{u"action"_s, u"fingerbot_power_long"_s}}));
        QCOMPARE(ui::abilityFor(hold.action), u"fingerbot"_s);
        QCOMPARE(ui::abilityFor(press.action), u"fingerbot"_s);
        // Its battery is not low: nothing is said of it.
        QCOMPARE(menu.battery, QString());

        // A power board as well: its buttons stay the power buttons, and the Fingerbot's are beside them.
        of.status->powerIsKnown = true;
        of.status->capabilities->append({u"power"_s, u"reset"_s});
        menu = ui::menuFor(of);
        QCOMPARE(
            rows(menu, u"Power"_s),
            (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s, u"power:atx_power"_s, u"power:atx_power_long"_s,
                         u"power:atx_reset"_s, u"power:fingerbot_power"_s, u"power:fingerbot_power_long"_s}));

        // Its battery low: said at the top, as the page says it.
        of.status->fingerbot = core::Fingerbot{9, true};
        QCOMPARE(ui::menuFor(of).battery, u"The battery of its Fingerbot is low (9 %). A press may fail: charge it."_s);
        QCOMPARE(ui::shown(ui::menuFor(of)).value(u"battery"_s).toString(),
                 u"The battery of its Fingerbot is low (9 %). A press may fail: charge it."_s);

        // The server does not say it has one: none of it, battery or not, and the note says what would bring buttons.
        of.status->powerIsKnown = false;
        of.status->capabilities = QStringList{u"screen"_s, u"keys"_s, u"text"_s, u"jiggler"_s};
        menu = ui::menuFor(of);
        QCOMPARE(rows(menu, u"Power"_s), (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s}));
        QCOMPARE(menu.battery, QString());
        QCOMPARE(section(menu, u"Power"_s)->note,
                 u"Power and reset buttons appear here when the KVM has an ATX power board, or a Fingerbot paired with "
                 "it."_s);
        // Where nothing says what the machine can be asked: not on an older server's word, which has no Fingerbot's
        // record either. This PC's own Comet has no `can` and its driver's record (turned at c4: rule 29 on this PC's
        // wall; test_ui_own_machine_menu).
        of.status->capabilities.reset();
        of.status->fingerbot.reset();
        QVERIFY(ui::menuFor(of).row(u"power:fingerbot_power"_s) == nullptr);
        of.status->fingerbot = core::Fingerbot{9, true};
        QVERIFY(ui::menuFor(of).row(u"power:fingerbot_power"_s) != nullptr);

        // Several: where any of them has one, and each answers for itself.
        MenuOf bot;
        bot.machine = kvm();
        bot.status = online();
        bot.status->capabilities = QStringList{u"jiggler"_s, u"fingerbot"_s};
        MenuOf none = bot;
        none.machine.id = u"k2"_s;
        none.status->capabilities = QStringList{u"jiggler"_s};
        const Menu several = ui::menuForSeveral({bot, none});
        QCOMPARE(rows(several, u"Power"_s), (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s,
                                                         u"power:fingerbot_power"_s, u"power:fingerbot_power_long"_s}));
        QCOMPARE(ask(several, u"power:fingerbot_power"_s).question, u"Press with Fingerbot on 2 KVMs?"_s);
        QCOMPARE(section(ui::menuForSeveral({none, none}), u"Power"_s)->note,
                 u"None of the selected KVMs has an ATX power board or a Fingerbot, so there are no power buttons."_s);
    }

    void theVirtualMediaAreAnAdminsInPersonWhereTheServerSaysTheMachineHasThem()
    {
        // "Media...", among the rows that lead elsewhere, after the large view (the page's, an admin's: app.js,
        // actionsDialog): where whoever sits here may have the media of the machine's server (MenuOf::media) and the
        // server says the machine has some (`media` in `can`). It opens a box of its own: it sends nothing itself.
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        of.status->capabilities = QStringList{u"keys"_s, u"power"_s, u"media"_s};
        Menu menu = ui::menuFor(of);
        QVERIFY(menu.row(u"media"_s) == nullptr);
        of.media = true;
        menu = ui::menuFor(of);
        QCOMPARE(rows(menu, QString()), (QStringList{u"enlarge"_s, u"media"_s}));
        const MenuRow* media = menu.row(u"media"_s);
        QVERIFY(media != nullptr);
        QCOMPARE(media->label, u"Media…"_s);
        QCOMPARE(media->icon, u"disc"_s);
        QVERIFY(!media->ask);
        // A server's too.
        of.machine = server();
        of.status = serverThatIs(true, std::nullopt);
        of.status->capabilities = QStringList{u"power"_s, u"media"_s};
        QVERIFY(ui::menuFor(of).row(u"media"_s) != nullptr);
        // Not where the server does not list it, nor where it says nothing of what the machine can (an older one).
        of.status->capabilities = QStringList{u"power"_s};
        QVERIFY(ui::menuFor(of).row(u"media"_s) == nullptr);
        of.status->capabilities.reset();
        QVERIFY(ui::menuFor(of).row(u"media"_s) == nullptr);
        // Nor among what several machines are offered: one machine's box.
        of.status->capabilities = QStringList{u"power"_s, u"media"_s};
        QVERIFY(ui::menuForSeveral({of, of}).row(u"media"_s) == nullptr);
    }

    void theBiosSetupIsOfferedForOneMachineAndAskedAboutFirst()
    {
        // "Start into BIOS setup", where the server says the machine offers it (a PC with Intel AMT, a server over
        // Redfish): one machine at a time, asked first with the page's question (app.js, bootIntoSetup).
        MenuOf of;
        of.machine = server();
        of.status = serverThatIs(true, std::nullopt);
        of.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"bios_setup"_s};
        Menu menu = ui::menuFor(of);
        QCOMPARE(rows(menu, u"Power"_s), (QStringList{u"power:power_off"_s, u"power:atx_power_long"_s,
                                                      u"power:atx_reset"_s, u"power:bios_setup"_s}));
        const MenuRow* setup = menu.row(u"power:bios_setup"_s);
        QVERIFY(setup != nullptr);
        QCOMPARE(setup->label, u"Start into BIOS setup"_s);
        QCOMPARE(setup->icon, u"sliders"_s);
        QCOMPARE(setup->hint, u"restart"_s);
        QVERIFY(setup->danger);
        // It runs: it restarts into the setup, and what is not saved on it is lost.
        ui::Ask asked = ask(menu, u"power:bios_setup"_s);
        QCOMPARE(asked.title, u"Start into BIOS setup"_s);
        QVERIFY(asked.confirm);
        QVERIFY(asked.danger);
        QCOMPARE(asked.question, u"Rack server will restart now and start straight into its BIOS setup screen, once. "
                                 "Any unsaved work on it is lost. Its large view opens so you can watch the setup."_s);
        QCOMPARE(asked.yes, u"Restart into setup"_s);
        QCOMPARE(ui::shown(asked).value(u"yes"_s).toString(), u"Restart into setup"_s);
        QCOMPARE(asked.done, u"starting into BIOS setup"_s);
        QCOMPARE(sent(menu, u"power:bios_setup"_s), QJsonObject({{u"action"_s, u"bios_setup"_s}}));
        QCOMPARE(ui::abilityFor(asked.action), u"bios_setup"_s);

        // It is off: it is switched on into the setup.
        of.status->power = false;
        menu = ui::menuFor(of);
        QVERIFY(menu.row(u"power:bios_setup"_s) != nullptr);
        QCOMPARE(menu.row(u"power:bios_setup"_s)->hint, u"power on"_s);
        asked = ask(menu, u"power:bios_setup"_s);
        QVERIFY(!asked.danger);
        QCOMPARE(asked.question, u"Rack server will be switched on and start straight into its BIOS setup screen, "
                                 "once. Its large view opens so you can watch the setup."_s);
        QCOMPARE(asked.yes, u"Switch on into setup"_s);
        // A question of any other row says yes with its own name.
        QCOMPARE(ui::shown(ask(menu, u"power:atx_reset"_s)).value(u"yes"_s).toString(), u"Press reset button"_s);

        // Never for several at once, though every one of them offers it.
        MenuOf other = of;
        other.machine.id = u"s2"_s;
        QVERIFY(ui::menuForSeveral({of, other}).row(u"power:bios_setup"_s) == nullptr);

        // Where the server does not list it, it is not offered.
        of.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s};
        QVERIFY(ui::menuFor(of).row(u"power:bios_setup"_s) == nullptr);
        of.status->capabilities.reset();
        QVERIFY(ui::menuFor(of).row(u"power:bios_setup"_s) == nullptr);
    }

    void aServerIsOfferedTheSwitchThatApplies()
    {
        MenuOf of;
        of.machine = server();

        // It runs: it can be shut down.
        of.status = serverThatIs(true, std::nullopt);
        Menu menu = ui::menuFor(of);
        // A controller has a power button and no keyboard; and no light where the server has none.
        QCOMPARE(titles(menu), (QStringList{QString(), u"Power"_s, u"This controller"_s}));
        QCOMPARE(rows(menu, u"Power"_s),
                 (QStringList{u"power:power_off"_s, u"power:atx_power_long"_s, u"power:atx_reset"_s}));
        const ui::Ask down = ask(menu, u"power:power_off"_s);
        QCOMPARE(down.title, u"Shut down"_s);
        QVERIFY(down.confirm);
        QVERIFY(!down.danger);
        QCOMPARE(down.question, u"Shut down on Rack server?"_s);
        QCOMPARE(down.warning,
                 u"Asks the operating system to shut down. A server that is off already is left as it is."_s);
        QCOMPARE(down.done, u"asked to shut down"_s);
        QCOMPARE(sent(menu, u"power:power_off"_s), QJsonObject({{u"action"_s, u"power_off"_s}}));

        // It is off: it can be switched on.
        of.status = serverThatIs(false, std::nullopt);
        menu = ui::menuFor(of);
        QCOMPARE(rows(menu, u"Power"_s),
                 (QStringList{u"power:power_on"_s, u"power:atx_power_long"_s, u"power:atx_reset"_s}));
        const ui::Ask on = ask(menu, u"power:power_on"_s);
        QCOMPARE(on.title, u"Power on"_s);
        QVERIFY(on.confirm);
        QCOMPARE(on.warning, u"Switches on what is off. A server that is on already is left as it is."_s);
        QCOMPARE(on.done, u"switched on"_s);
        QCOMPARE(sent(menu, u"power:power_on"_s), QJsonObject({{u"action"_s, u"power_on"_s}}));

        // Its controller does not say: both.
        of.status = serverThatIs(std::nullopt, std::nullopt);
        menu = ui::menuFor(of);
        QCOMPARE(rows(menu, u"Power"_s), (QStringList{u"power:power_on"_s, u"power:power_off"_s,
                                                      u"power:atx_power_long"_s, u"power:atx_reset"_s}));
    }

    void aServersControllerIsRestartedAndNotTheServer()
    {
        MenuOf of;
        of.machine = server();
        of.status = serverThatIs(true, std::nullopt);
        const Menu menu = ui::menuFor(of);
        const MenuRow* row = menu.row(u"reboot"_s);
        QVERIFY(row != nullptr);
        QCOMPARE(row->label, u"Restart controller"_s);
        QVERIFY(row->danger);
        const ui::Ask restart = ask(menu, u"reboot"_s);
        QVERIFY(restart.confirm);
        QVERIFY(restart.danger);
        QCOMPARE(restart.question, u"Restart the management controller of Rack server?"_s);
        QCOMPARE(restart.warning, u"This restarts the management controller, not the server, which keeps running. The "
                                  "controller is away for a minute or two."_s);
        QCOMPARE(restart.done, u"restarting"_s);
        QCOMPARE(sent(menu, u"reboot"_s), QJsonObject({{u"action"_s, u"reboot"_s}}));

        // And a KVM's own restart is the KVM's.
        MenuOf other;
        other.machine = kvm();
        other.status = online();
        const Menu kvmMenu = ui::menuFor(other);
        QCOMPARE(kvmMenu.row(u"reboot"_s)->label, u"Reboot KVM"_s);
        const ui::Ask reboot = ask(kvmMenu, u"reboot"_s);
        QVERIFY(reboot.confirm);
        QVERIFY(reboot.danger);
        QCOMPARE(reboot.question, u"Reboot NAS?"_s);
        QCOMPARE(reboot.warning, u"This restarts the KVM itself, not the computer. Anyone using its console is "
                                 "disconnected for about a minute."_s);
        QCOMPARE(reboot.done, u"rebooting"_s);
    }

    void aServerWithALightIsOfferedItsSwitch()
    {
        MenuOf of;
        of.machine = server();
        of.status = serverThatIs(true, false);
        Menu menu = ui::menuFor(of);
        QCOMPARE(titles(menu), (QStringList{QString(), u"Power"_s, u"Find it in the rack"_s, u"This controller"_s}));
        QCOMPARE(rows(menu, u"Find it in the rack"_s), QStringList{u"light:on"_s});
        QCOMPARE(menu.row(u"light:on"_s)->label, u"Locator light on"_s);
        QVERIFY(!ask(menu, u"light:on"_s).confirm);
        QCOMPARE(ask(menu, u"light:on"_s).done, u"locator light on"_s);
        QCOMPARE(sent(menu, u"light:on"_s), QJsonObject({{u"action"_s, u"locate_on"_s}}));
        QCOMPARE(section(menu, u"Find it in the rack"_s)->note,
                 u"The light on the machine itself, front and back, for whoever is standing in front of the rack."_s);

        // It is lit: it can be put out.
        of.status = serverThatIs(true, true);
        menu = ui::menuFor(of);
        QCOMPARE(rows(menu, u"Find it in the rack"_s), QStringList{u"light:off"_s});
        QCOMPARE(menu.row(u"light:off"_s)->label, u"Locator light off"_s);
        QCOMPARE(ask(menu, u"light:off"_s).done, u"locator light off"_s);
        QCOMPARE(sent(menu, u"light:off"_s), QJsonObject({{u"action"_s, u"locate_off"_s}}));
    }

    void aKvmWithSeveralComputersShowsWhichIsOnScreen()
    {
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        of.status->ports = {core::Port{QString(), false}, core::Port{u"Build box"_s, false},
                            core::Port{QString(), true}};
        of.status->shownPort = 0;
        const Menu menu = ui::menuFor(of);
        QCOMPARE(titles(menu).value(1), u"Computer on screen"_s);
        QCOMPARE(rows(menu, u"Computer on screen"_s), (QStringList{u"port:0"_s, u"port:1"_s, u"port:2"_s}));

        // The one on the screen cannot be chosen again.
        const MenuRow* first = menu.row(u"port:0"_s);
        QCOMPARE(first->label, u"Port 1"_s);
        QCOMPARE(first->hint, u"on screen"_s);
        QVERIFY(!first->enabled);
        // One with a name of its own is called by it, and its number stands beside it.
        const MenuRow* second = menu.row(u"port:1"_s);
        QCOMPARE(second->label, u"Build box"_s);
        QCOMPARE(second->hint, u"port 2"_s);
        QVERIFY(second->enabled);
        QCOMPARE(sent(menu, u"port:1"_s), (QJsonObject{{u"action"_s, u"port"_s}, {u"port"_s, 1}}));
        QVERIFY(!ask(menu, u"port:1"_s).confirm);
        QCOMPARE(ask(menu, u"port:1"_s).title, u"Show Build box"_s);
        QCOMPARE(ask(menu, u"port:1"_s).done, u"showing Build box"_s);
        // One with nothing plugged in says so, and can be chosen all the same.
        const MenuRow* third = menu.row(u"port:2"_s);
        QCOMPARE(third->label, u"Port 3"_s);
        QCOMPARE(third->hint, u"empty"_s);
        QVERIFY(third->enabled);

        // The numbers on the machine's card send the same.
        const std::optional<ui::Ask> fromTheCard = ui::askToShow(kvm(), of.status.value_or(core::Status{}), 1);
        QVERIFY(fromTheCard.has_value());
        QCOMPARE(QJsonDocument::fromJson(core::bodyOf(fromTheCard.value_or(ui::Ask{}).action)).object(),
                 (QJsonObject{{u"action"_s, u"port"_s}, {u"port"_s, 1}}));
        QCOMPARE(fromTheCard.value_or(ui::Ask{}).done, u"showing Build box"_s);
        // A number the machine does not have is none.
        QVERIFY(!ui::askToShow(kvm(), of.status.value_or(core::Status{}), 3).has_value());
        QVERIFY(!ui::askToShow(kvm(), of.status.value_or(core::Status{}), -1).has_value());

        // A KVM with one computer has no such rows.
        MenuOf plain;
        plain.machine = kvm();
        plain.status = online();
        QVERIFY(!titles(ui::menuFor(plain)).contains(u"Computer on screen"_s));
    }

    void savedActionsAndComputersToWakeAreOffered()
    {
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        core::SavedAction lock;
        lock.id = u"a1"_s;
        lock.name = u"Lock it"_s;
        lock.keys = u"MetaLeft,KeyL"_s;
        lock.changed = 2;
        core::SavedAction unlock;
        unlock.id = u"b2"_s;
        unlock.name = u"Unlock the disks"_s;
        unlock.types = true;
        core::SavedAction wipe;
        wipe.id = u"c3"_s;
        wipe.name = u"Close everything"_s;
        wipe.keys = u"AltLeft,F4"_s;
        wipe.danger = true;
        of.saved = {lock, unlock, wipe};
        of.wake = {core::WakeTarget{u"Backup box"_s, u"00:11:22:aa:bb:cc"_s},
                   core::WakeTarget{QString(), u"00:11:22:33:44:55"_s}};
        const Menu menu = ui::menuFor(of);

        QCOMPARE(rows(menu, u"Your actions"_s), (QStringList{u"saved:a1"_s, u"saved:b2"_s, u"saved:c3"_s}));
        QCOMPARE(section(menu, u"Your actions"_s)->note, QString());
        // Keys: sent at once, with the keys beside the name. The state it was looked at goes along.
        QCOMPARE(menu.row(u"saved:a1"_s)->label, u"Lock it"_s);
        QCOMPARE(menu.row(u"saved:a1"_s)->icon, u"star"_s);
        QCOMPARE(menu.row(u"saved:a1"_s)->hint, u"Win+L"_s);
        QVERIFY(!ask(menu, u"saved:a1"_s).confirm);
        QCOMPARE(ask(menu, u"saved:a1"_s).done, u"Lock it sent"_s);
        QCOMPARE(sent(menu, u"saved:a1"_s),
                 (QJsonObject{{u"action"_s, u"custom"_s}, {u"custom_id"_s, u"a1"_s}, {u"custom_changed"_s, 2}}));
        // Text: typed into the wrong window it can do anything, so that always asks first.
        QCOMPARE(menu.row(u"saved:b2"_s)->icon, u"type"_s);
        QCOMPARE(menu.row(u"saved:b2"_s)->hint, QString());
        QVERIFY(ask(menu, u"saved:b2"_s).confirm);
        QVERIFY(!ask(menu, u"saved:b2"_s).danger);
        QCOMPARE(ask(menu, u"saved:b2"_s).question, u"Unlock the disks on NAS?"_s);
        // One that was marked as dangerous asks first, in red.
        QVERIFY(ask(menu, u"saved:c3"_s).confirm);
        QVERIFY(ask(menu, u"saved:c3"_s).danger);
        QVERIFY(menu.row(u"saved:c3"_s)->danger);

        // Whom the KVM can wake: by name, or by address where it has none. Asked about first.
        QCOMPARE(rows(menu, u"Power"_s).mid(2),
                 (QStringList{u"wake:00:11:22:aa:bb:cc"_s, u"wake:00:11:22:33:44:55"_s}));
        QCOMPARE(menu.row(u"wake:00:11:22:aa:bb:cc"_s)->label, u"Wake Backup box"_s);
        QCOMPARE(menu.row(u"wake:00:11:22:33:44:55"_s)->label, u"Wake 00:11:22:33:44:55"_s);
        QVERIFY(ask(menu, u"wake:00:11:22:aa:bb:cc"_s).confirm);
        QCOMPARE(ask(menu, u"wake:00:11:22:aa:bb:cc"_s).done, u"Wake-on-LAN sent"_s);
        QCOMPARE(sent(menu, u"wake:00:11:22:aa:bb:cc"_s),
                 (QJsonObject{{u"action"_s, u"wol"_s}, {u"mac"_s, u"00:11:22:aa:bb:cc"_s}}));
    }

    void keysOfOnesOwnChoosingAreAskedAboutFirst()
    {
        const std::optional<ui::Ask> made = ui::askToPress(u"NAS"_s, u"ControlLeft,KeyC"_s);
        QVERIFY(made.has_value());
        const ui::Ask ask = made.value_or(ui::Ask{});
        QCOMPARE(ask.title, u"Send Ctrl+C"_s);
        QVERIFY(ask.confirm);
        QVERIFY(!ask.danger);
        QCOMPARE(ask.question, u"Send Ctrl+C on NAS?"_s);
        QCOMPARE(ask.done, u"Ctrl+C sent"_s);
        QCOMPARE(QJsonDocument::fromJson(core::bodyOf(ask.action)).object(),
                 (QJsonObject{{u"action"_s, u"keys"_s}, {u"keys"_s, u"ControlLeft,KeyC"_s}}));
        // One key alone is keys too.
        QVERIFY(ui::askToPress(u"NAS"_s, u"F5"_s).has_value());
    }

    void whatAreNoKeysIsNotSent_data()
    {
        QTest::addColumn<QString>("keys");
        QTest::newRow("nothing") << QString();
        QTest::newRow("a comma alone") << u","_s;
        QTest::newRow("a name that begins small") << u"controlLeft"_s;
        QTest::newRow("something else in it") << u"ControlLeft,Key C"_s;
        QTest::newRow("a path") << u"ControlLeft,../x"_s;
        QTest::newRow("more than eight") << u"A1,B1,C1,D1,E1,F1,G1,H1,I1"_s;
        QTest::newRow("a name too long") << u"Abcdefghijklmnopq"_s;
        QTest::newRow("a comma at its end") << u"ControlLeft,"_s;
        // The keyboard's Power key shuts a machine down without the power question: whatever
        // place it is for, it is not offered to be sent (the server's 422, and this PC's wall's).
        QTest::newRow("the keyboard's Power key") << u"Power"_s;
        QTest::newRow("the keyboard's Power key held with others") << u"ControlLeft,AltLeft,Power"_s;
    }

    void whatAreNoKeysIsNotSent()
    {
        QFETCH(const QString, keys);
        QVERIFY(!ui::askToPress(u"NAS"_s, keys).has_value());
    }

    void aTextIsShownOnceMoreBeforeItIsTyped()
    {
        const std::optional<ui::Ask> made = ui::askToType(u"NAS"_s, u"ipconfig /all"_s, true);
        QVERIFY(made.has_value());
        const ui::Ask ask = made.value_or(ui::Ask{});
        QCOMPARE(ask.title, u"Type text"_s);
        QVERIFY(ask.confirm);
        QCOMPARE(ask.question, u"Type this on NAS?"_s);
        // What would be typed, with the sign of the Enter that follows it.
        QCOMPARE(ask.detail, u"ipconfig /all ⏎"_s);
        QCOMPARE(ask.done, u"text typed"_s);
        QCOMPARE(QJsonDocument::fromJson(core::bodyOf(ask.action)).object(),
                 (QJsonObject{{u"action"_s, u"text"_s}, {u"text"_s, u"ipconfig /all"_s}, {u"enter"_s, true}}));
        QCOMPARE(ui::shown(ask).value(u"detail"_s).toString(), u"ipconfig /all ⏎"_s);

        // Without Enter: as it is, spaces and line ends and all.
        const ui::Ask plain = ui::askToType(u"NAS"_s, u" two\nlines "_s, false).value_or(ui::Ask{});
        QCOMPARE(plain.detail, u" two\nlines "_s);
        QCOMPARE(QJsonDocument::fromJson(core::bodyOf(plain.action)).object(),
                 (QJsonObject{{u"action"_s, u"text"_s}, {u"text"_s, u" two\nlines "_s}, {u"enter"_s, false}}));

        // Nothing is not typed, and no more than the server takes.
        QVERIFY(!ui::askToType(u"NAS"_s, QString(), true).has_value());
        QVERIFY(ui::askToType(u"NAS"_s, QString(core::longestTypedText, u'x'), false).has_value());
        QVERIFY(!ui::askToType(u"NAS"_s, QString(core::longestTypedText, u'x'), true).has_value());
        QVERIFY(ui::askToType(u"NAS"_s, QString(core::longestTypedText - 1, u'x'), true).has_value());
        QVERIFY(!ui::askToType(u"NAS"_s, QString(core::longestTypedText + 1, u'x'), false).has_value());
    }

    void theBoxForKeysOffersWhatThePagesDoes()
    {
        const QVariantList held = ui::heldKeys();
        QCOMPARE(held.size(), 4);
        QCOMPARE(held[0].toMap(), (QVariantMap{{u"value"_s, u"ControlLeft"_s}, {u"label"_s, u"Ctrl"_s}}));
        QCOMPARE(held[3].toMap(), (QVariantMap{{u"value"_s, u"MetaLeft"_s}, {u"label"_s, u"Win"_s}}));

        const QVariantList other = ui::otherKeys();
        // None; the letters, the digits, twelve function keys, and sixteen more.
        QCOMPARE(other.size(), 1 + 26 + 10 + 12 + 16);
        QCOMPARE(other[0].toMap(), (QVariantMap{{u"value"_s, QString()}, {u"label"_s, u"No other key"_s}}));
        QCOMPARE(other[1].toMap(), (QVariantMap{{u"value"_s, u"KeyA"_s}, {u"label"_s, u"A"_s}}));
        QCOMPARE(other[26].toMap().value(u"value"_s).toString(), u"KeyZ"_s);
        QCOMPARE(other[27].toMap(), (QVariantMap{{u"value"_s, u"Digit0"_s}, {u"label"_s, u"0"_s}}));
        QCOMPARE(other[37].toMap().value(u"value"_s).toString(), u"F1"_s);
        QCOMPARE(other[48].toMap().value(u"value"_s).toString(), u"F12"_s);
        QCOMPARE(other[49].toMap().value(u"value"_s).toString(), u"Enter"_s);
        QCOMPARE(other[50].toMap(), (QVariantMap{{u"value"_s, u"Escape"_s}, {u"label"_s, u"Esc"_s}}));
        QCOMPARE(other.constLast().toMap(), (QVariantMap{{u"value"_s, u"PrintScreen"_s}, {u"label"_s, u"PrtSc"_s}}));
        // Every one of them, alone and with every held key, is something the server takes.
        QStringList all;
        for (const QVariant& key : held) {
            all.append(key.toMap().value(u"value"_s).toString());
        }
        for (const QVariant& key : other.mid(1)) {
            const QString value = key.toMap().value(u"value"_s).toString();
            QVERIFY2(ui::askToPress(u"NAS"_s, value).has_value(), qPrintable(value));
            QVERIFY2(ui::askToPress(u"NAS"_s, (all + QStringList{value}).join(u',')).has_value(), qPrintable(value));
        }
        QVERIFY(ui::askToPress(u"NAS"_s, all.join(u',')).has_value());
    }

    void severalMachinesAreOfferedWhatAnyOfThemCanDo()
    {
        // A KVM without a power board, one with, and a server with a light to find it by.
        MenuOf plain;
        plain.machine = kvm();
        plain.status = online();
        MenuOf wired = plain;
        wired.machine.id = u"k2"_s;
        wired.machine.name = u"Build box"_s;
        wired.status->powerIsKnown = true;
        wired.status->ports = {core::Port{}, core::Port{}};
        wired.wake = {core::WakeTarget{u"Backup box"_s, u"00:11:22:aa:bb:cc"_s}};
        MenuOf rack;
        rack.machine = server();
        rack.machine.page = QUrl(u"https://192.168.1.60/"_s);
        rack.status = serverThatIs(true, false);
        core::SavedAction saved;
        saved.id = u"a1"_s;
        saved.name = u"Lock it"_s;
        saved.keys = u"MetaLeft,KeyL"_s;
        plain.saved = {saved};
        const Menu menu = ui::menuForSeveral({plain, wired, rack});

        QCOMPARE(menu.title, u"3 KVMs selected"_s);
        QCOMPARE(menu.about, u"The action you choose runs on every selected KVM. You are asked to confirm first."_s);
        QCOMPARE(menu.warning, QString());
        QCOMPARE(titles(menu), (QStringList{u"Keyboard"_s, u"Your actions"_s, u"Power"_s, u"Find it in the rack"_s,
                                            u"The KVMs themselves"_s}));
        // Nothing that is one machine's own: no way to a console or to one shown large, none of a
        // KVM's computers, and nobody is woken.
        for (const QString& row : {u"enlarge"_s, u"console"_s, u"port:0"_s, u"wake:00:11:22:aa:bb:cc"_s}) {
            QVERIFY2(menu.row(row) == nullptr, qPrintable(row));
        }
        QCOMPARE(rows(menu, u"Your actions"_s), QStringList{u"saved:a1"_s});
        // What a KVM does, what a server does, and what a power board does: each machine answers
        // for itself. A server among KVMs is named where a row is its alone.
        QCOMPARE(rows(menu, u"Power"_s),
                 (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s, u"power:power_on"_s, u"power:power_off"_s,
                              u"power:atx_power"_s, u"power:atx_power_long"_s, u"power:atx_reset"_s}));
        QCOMPARE(menu.row(u"power:power_on"_s)->label, u"Power on (servers)"_s);
        QCOMPARE(menu.row(u"power:power_off"_s)->label, u"Shut down (servers)"_s);
        // Some lights may be on and some off: both switches.
        QCOMPARE(rows(menu, u"Find it in the rack"_s), (QStringList{u"light:on"_s, u"light:off"_s}));
        QCOMPARE(menu.row(u"reboot"_s)->label, u"Reboot KVM"_s);

        // Every row asks about all of them.
        QCOMPARE(ask(menu, u"keys:ShiftLeft"_s).question, u"Wake screen on 3 KVMs?"_s);
        QCOMPARE(ask(menu, u"power:atx_power"_s).question, u"Press power button on 3 KVMs?"_s);
        QCOMPARE(ask(menu, u"saved:a1"_s).question, u"Lock it on 3 KVMs?"_s);
        QCOMPARE(ask(menu, u"reboot"_s).question, u"Reboot 3 KVMs?"_s);
        QCOMPARE(sent(menu, u"power:power_on"_s), QJsonObject({{u"action"_s, u"power_on"_s}}));
    }

    void onlyServersAreOfferedWhatAControllerCanDo()
    {
        MenuOf first;
        first.machine = server();
        first.status = serverThatIs(true, std::nullopt);
        MenuOf second = first;
        second.machine.id = u"s2"_s;
        second.status = serverThatIs(false, std::nullopt);
        const Menu menu = ui::menuForSeveral({first, second});
        QCOMPARE(menu.title, u"2 KVMs selected"_s);
        // No keyboard, no keeping awake, no button to press; and no light where none has one.
        QCOMPARE(titles(menu), (QStringList{u"Power"_s, u"The controllers themselves"_s}));
        QCOMPARE(rows(menu, u"Power"_s), (QStringList{u"power:power_on"_s, u"power:power_off"_s,
                                                      u"power:atx_power_long"_s, u"power:atx_reset"_s}));
        // Among themselves they need no naming.
        QCOMPARE(menu.row(u"power:power_on"_s)->label, u"Power on"_s);
        QCOMPARE(menu.row(u"power:power_off"_s)->label, u"Shut down"_s);
        QCOMPARE(menu.row(u"reboot"_s)->label, u"Restart controller"_s);
        QCOMPARE(ask(menu, u"reboot"_s).question, u"Restart the management controller of 2 KVMs?"_s);
    }

    void severalMachinesWithoutAPowerBoardHaveNoPowerButtons()
    {
        MenuOf first;
        first.machine = kvm();
        first.status = online();
        MenuOf second = first;
        second.machine.id = u"k2"_s;
        const Menu menu = ui::menuForSeveral({first, second});
        QCOMPARE(rows(menu, u"Power"_s), (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s}));
        QCOMPARE(section(menu, u"Power"_s)->note,
                 u"None of the selected KVMs has an ATX power board or a Fingerbot, so there are no power buttons."_s);
        QVERIFY(!titles(menu).contains(u"Find it in the rack"_s));
    }

    void whatCanLoseWorkOnSeveralMachinesTakesATick()
    {
        MenuOf first;
        first.machine = kvm();
        first.status = online();
        first.status->powerIsKnown = true;
        core::SavedAction keys;
        keys.id = u"a1"_s;
        keys.name = u"Lock it"_s;
        keys.keys = u"MetaLeft,KeyL"_s;
        core::SavedAction text;
        text.id = u"b2"_s;
        text.name = u"Unlock the disks"_s;
        text.types = true;
        core::SavedAction harmful;
        harmful.id = u"c3"_s;
        harmful.name = u"Log everybody off"_s;
        harmful.keys = u"MetaLeft,KeyX"_s;
        harmful.danger = true;
        first.saved = {keys, text, harmful};
        MenuOf second = first;
        second.machine.id = u"k2"_s;
        const Menu menu = ui::menuForSeveral({first, second});
        const QList<ui::Target> targets{{u"NAS"_s, u"192.168.1.22"_s, u"online"_s, u"Online"_s},
                                        {u"Build box"_s, u"192.168.1.23"_s, u"online"_s, u"Online"_s}};
        const auto takesATick = [&](const ui::Ask& asked) {
            return !ui::shown(asked, targets).value(u"tick"_s).toString().isEmpty();
        };
        // Whatever is marked as a danger; a power button of whatever kind (a short press shuts a
        // running system down); and a text, saved or typed, which does whatever the window it lands
        // in makes of it.
        for (const QString& row : {u"power:atx_power"_s, u"power:atx_power_long"_s, u"power:atx_reset"_s, u"saved:b2"_s,
                                   u"saved:c3"_s, u"keys:AltLeft,F4"_s, u"reboot"_s}) {
            QVERIFY2(takesATick(ask(menu, row)), qPrintable(row));
        }
        // Not a key that harms nobody, not keeping awake.
        for (const QString& row :
             {u"keys:ShiftLeft"_s, u"keys:ControlLeft,AltLeft,Delete"_s, u"power:jiggler_on"_s, u"saved:a1"_s}) {
            QVERIFY2(!takesATick(ask(menu, row)), qPrintable(row));
        }
        QVERIFY(takesATick(ui::askToType(u"2 KVMs"_s, u"dir"_s, true).value_or(ui::Ask{})));
        QVERIFY(!takesATick(ui::askToPress(u"2 KVMs"_s, u"F5"_s).value_or(ui::Ask{})));
        // About one machine nothing is ticked, whatever it is.
        QCOMPARE(ui::shown(ask(menu, u"power:atx_power_long"_s)).value(u"tick"_s).toString(), QString());
        QCOMPARE(ui::askToType(ui::severalKvms(2), u"dir"_s, true).value_or(ui::Ask{}).question,
                 u"Type this on 2 KVMs?"_s);
        QCOMPARE(ui::askToPress(ui::severalKvms(2), u"F5"_s).value_or(ui::Ask{}).question, u"Send F5 on 2 KVMs?"_s);
    }

    void theQuestionAboutSeveralMachinesNamesThemAndAsksHowToGoAboutIt()
    {
        MenuOf first;
        first.machine = kvm();
        first.status = online();
        first.status->powerIsKnown = true;
        MenuOf second = first;
        second.machine.id = u"k2"_s;
        const Menu menu = ui::menuForSeveral({first, second});
        const QList<ui::Target> targets{{u"NAS"_s, u"192.168.1.22"_s, u"online"_s, u"Online"_s},
                                        {u"Build box"_s, u"192.168.1.23"_s, u"offline"_s, u"Offline"_s}};

        // What can lose work: a tick is asked for, in a sentence that says how many.
        const QVariantMap hold = ui::shown(ask(menu, u"power:atx_power_long"_s), targets);
        QCOMPARE(hold.value(u"title"_s).toString(), u"Hold power button"_s);
        QCOMPARE(hold.value(u"text"_s).toString(), u"Hold power button on 2 KVMs?"_s);
        QCOMPARE(hold.value(u"yes"_s).toString(), u"Run on 2 KVMs"_s);
        QCOMPARE(hold.value(u"tick"_s).toString(), u"Yes, do this on all 2 machines"_s);
        QCOMPARE(hold.value(u"danger"_s).toBool(), true);
        QCOMPARE(hold.value(u"targets"_s).toList(), (QVariantList{QVariantMap{{u"name"_s, u"NAS"_s},
                                                                              {u"host"_s, u"192.168.1.22"_s},
                                                                              {u"state"_s, u"online"_s},
                                                                              {u"label"_s, u"Online"_s}},
                                                                  QVariantMap{{u"name"_s, u"Build box"_s},
                                                                              {u"host"_s, u"192.168.1.23"_s},
                                                                              {u"state"_s, u"offline"_s},
                                                                              {u"label"_s, u"Offline"_s}}}));
        // A few at a time; or one after the other, at once or with a pause.
        QVariantList values;
        QStringList labels;
        for (const QVariant& pace : hold.value(u"paces"_s).toList()) {
            values.append(pace.toMap().value(u"value"_s));
            labels.append(pace.toMap().value(u"label"_s).toString());
        }
        QCOMPARE(values, (QVariantList{-1, 0, 10, 30, 120}));
        QCOMPARE(labels, (QStringList{
                             u"A few at a time"_s, u"One after the other"_s, u"One after the other, 10 seconds apart"_s,
                             u"One after the other, 30 seconds apart"_s, u"One after the other, 2 minutes apart"_s}));
        // A power button that is no danger takes its tick too; a key takes none.
        QCOMPARE(ui::shown(ask(menu, u"power:atx_power"_s), targets).value(u"tick"_s).toString(),
                 u"Yes, do this on all 2 machines"_s);
        QCOMPARE(ui::shown(ask(menu, u"keys:ShiftLeft"_s), targets).value(u"tick"_s).toString(), QString());
        QCOMPARE(ui::shown(ask(menu, u"keys:ShiftLeft"_s), targets).value(u"paces"_s).toList().size(), 5);
    }

    void aMachineThatIsNotSimplyThereSaysSo()
    {
        MenuOf of;
        of.machine = kvm();
        // The server has not said yet how it is.
        QCOMPARE(ui::menuFor(of).warning, u"Connecting…"_s);
        core::Status status;
        status.state = u"offline"_s;
        status.errorMessage = u"Device is unreachable"_s;
        of.status = status;
        QCOMPARE(ui::menuFor(of).warning, u"Device is unreachable"_s);
        status.state = u"no_signal"_s;
        status.errorMessage.clear();
        of.status = status;
        QCOMPARE(ui::menuFor(of).warning, u"The computer is off, asleep or not plugged into the KVM."_s);

        // A server that is switched off is called that.
        MenuOf off;
        off.machine = server();
        core::Status down = serverThatIs(false, std::nullopt);
        down.state = u"no_signal"_s;
        off.status = down;
        QCOMPARE(ui::menuFor(off).warning, u"Powered off"_s);
    }

    // A JetKVM of this PC's own says what it can, as the web's says it (JetKvmDevice::abilities): its menu offers that,
    // and power and reset only where its ATX extension is on.
    void anOwnJetKvmIsOfferedWhatItCan()
    {
        MenuOf of;
        of.machine = kvm();
        of.machine.kind = u"jetkvm"_s;
        of.machine.own = true;
        core::Status status = online();
        status.capabilities = core::JetKvmDevice::abilities(false);
        of.status = status;
        Menu menu = ui::menuFor(of);
        for (const char* offered : {"keys:ControlLeft,AltLeft,Delete", "sendKeys", "typeText", "power:jiggler_on",
                                    "power:jiggler_off", "reboot"}) {
            QVERIFY2(menu.row(QString::fromLatin1(offered)) != nullptr, offered);
        }
        QVERIFY(menu.row(u"power:atx_power"_s) == nullptr);
        // Its extension on: power and reset as well.
        status.powerIsKnown = true;
        status.power = false;
        status.capabilities = core::JetKvmDevice::abilities(true);
        of.status = status;
        menu = ui::menuFor(of);
        for (const char* offered : {"power:atx_power", "power:atx_power_long", "power:atx_reset"}) {
            QVERIFY2(menu.row(QString::fromLatin1(offered)) != nullptr, offered);
        }
        // (Said otherwise, nothing of it: the driver's word is what is offered.)
        status.capabilities = core::JetKvmDevice::abilities(false);
        of.status = status;
        QVERIFY(ui::menuFor(of).row(u"power:atx_reset"_s) == nullptr);
    }

    void whatIsToBeKnownOfAJetKvmIsSaid()
    {
        MenuOf of;
        of.machine = kvm();
        of.machine.kind = u"jetkvm"_s;
        of.status = online();
        // A server's: the server types in the layout the JetKVM says its computer uses (W14d), as the page
        // says it (app.js, actionsDialog).
        QCOMPARE(ui::menuFor(of).kindNote,
                 u"A JetKVM shows its picture to one viewer at a time. Opening its console takes the picture from "
                 "this wall until you choose Show here on the tile. Text is typed in the keyboard layout set on its "
                 "own page."_s);
        // One of this PC's own: this program types in that layout too.
        of.machine.own = true;
        QCOMPARE(ui::menuFor(of).kindNote,
                 u"A JetKVM shows its picture to one viewer at a time. Opening its console takes the picture from "
                 "this wall until you choose Show here on the tile. Text is typed in the keyboard layout set on its "
                 "own page."_s);
    }

    void aMachineWithAConsoleOfItsOwnIsOfferedTheWayThere()
    {
        MenuOf of;
        of.machine = kvm();
        of.machine.page = QUrl(u"http://192.168.1.22/"_s);
        of.status = online();
        Menu menu = ui::menuFor(of);
        // Before the way to the machine shown large: both lead elsewhere, and send nothing.
        QCOMPARE(rows(menu, QString()), (QStringList{u"console"_s, u"enlarge"_s}));
        QCOMPARE(menu.row(u"console"_s)->label, u"Open console"_s);
        QCOMPARE(menu.row(u"console"_s)->icon, u"external"_s);
        QCOMPARE(menu.row(u"console"_s)->hint, QString());
        QVERIFY(!menu.row(u"console"_s)->ask.has_value());

        // Where its controller hands out a pass, the row says that it leads straight in.
        of.status->opensSignedIn = true;
        menu = ui::menuFor(of);
        QCOMPARE(menu.row(u"console"_s)->hint, u"signed in"_s);

        // A browser link remains available without signed-in console passes.
        of.status->capabilities = QStringList{u"screen"_s, u"keys"_s, u"text"_s, u"live"_s, u"control"_s};
        menu = ui::menuFor(of);
        QVERIFY(menu.row(u"console"_s) != nullptr);
        QCOMPARE(menu.row(u"console"_s)->hint, QString());

        // Shown large already, the way to its console is still there.
        of.enlarged = true;
        menu = ui::menuFor(of);
        QCOMPARE(rows(menu, QString()), QStringList{u"console"_s});
        // And a machine without a console of its own, shown large, has no such rows at all.
        of.machine.page = QUrl();
        menu = ui::menuFor(of);
        QVERIFY(menu.row(u"console"_s) == nullptr);
        QCOMPARE(titles(menu).constFirst(), u"Keyboard"_s);
    }

    // b5: a PC with Intel AMT has its three views under "View", in the page's words and order, the
    // one it is in (by its status, the fresher) said "now" and not offered; nothing is sent from
    // them as an action. No other machine has the section.
    void aPcWithIntelAmtOffersItsViews()
    {
        MenuOf of;
        of.machine = server();
        of.machine.kind = u"amt"_s;
        of.machine.name = u"Front desk"_s;
        of.machine.amtView = u"health"_s;
        core::Status said = serverThatIs(true, std::nullopt);
        core::ServerReadings readings = said.server.value_or(core::ServerReadings{});
        readings.amtView = u"live"_s;
        said.server = readings;
        of.status = said;
        const Menu menu = ui::menuFor(of);
        const auto section =
            std::ranges::find_if(menu.sections, [](const MenuSection& one) { return one.title == u"View"_s; });
        QVERIFY(section != menu.sections.end());
        QCOMPARE(section->rows.size(), 3);
        const QStringList ids{u"view:health"_s, u"view:stills"_s, u"view:live"_s};
        const QStringList labels{u"Health only"_s, u"Stills"_s, u"Live KVM"_s};
        const QStringList icons{u"activity"_s, u"camera"_s, u"tv"_s};
        for (int at = 0; at < 3; ++at) {
            const MenuRow& row = section->rows[at];
            QCOMPARE(row.id, ids[at]);
            QCOMPARE(row.label, labels[at]);
            QCOMPARE(row.icon, icons[at]);
            QVERIFY(!row.ask.has_value());
            QCOMPARE(row.enabled, at != 2);
            QCOMPARE(row.hint, at == 2 ? u"now"_s : QString());
        }
        QVERIFY(section->note.startsWith(u"Health only opens no KVM session: Intel's border never shows on the PC"_s));
        // (It comes before what is done to the machine itself, as the page's does.)
        QVERIFY(std::distance(menu.sections.begin(), section) <= 1);
        MenuOf plain;
        plain.machine = server();
        plain.status = serverThatIs(true, std::nullopt);
        for (const MenuSection& one : ui::menuFor(plain).sections) {
            QVERIFY(one.title != u"View"_s);
        }
    }

    // b11: a PC with Intel AMT that its server lists with no view (0.2.2, which has none) has no View section: what a
    // server does not have is not shown. Its keys and power are as they were.
    void aPcListedWithNoViewIsOfferedNone()
    {
        MenuOf of;
        of.machine = server();
        of.machine.kind = u"amt"_s;
        of.machine.name = u"Front desk"_s;
        of.status = serverThatIs(true, std::nullopt);
        const Menu menu = ui::menuFor(of);
        for (const QString& row : {u"view:health"_s, u"view:stills"_s, u"view:live"_s}) {
            QVERIFY2(menu.row(row) == nullptr, qPrintable(row));
        }
        QVERIFY(!titles(menu).contains(u"View"_s));
        QVERIFY(titles(menu).contains(u"Power"_s));
    }

    void aMachineThatIsShownLargeHasNoWayThere()
    {
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        of.enlarged = true;
        const Menu menu = ui::menuFor(of);
        QVERIFY(menu.row(u"enlarge"_s) == nullptr);
        QCOMPARE(titles(menu).constFirst(), u"Keyboard"_s);
        // And the row that leads there sends nothing.
        of.enlarged = false;
        const Menu wall = ui::menuFor(of);
        QVERIFY(wall.row(u"enlarge"_s) != nullptr);
        QCOMPARE(wall.row(u"enlarge"_s)->label, u"Enlarge"_s);
        QVERIFY(!wall.row(u"enlarge"_s)->ask.has_value());
    }

    void theScreenIsGivenWhatItDraws()
    {
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        of.status->powerIsKnown = true;
        const Menu menu = ui::menuFor(of);
        const QVariantMap shown = ui::shown(menu);
        QCOMPARE(shown.value(u"title"_s).toString(), u"NAS"_s);
        QCOMPARE(shown.value(u"about"_s).toString(), u"192.168.1.22"_s);
        const QVariantList sections = shown.value(u"sections"_s).toList();
        QCOMPARE(sections.size(), menu.sections.size());
        const QVariantMap power = sections.value(3).toMap();
        QCOMPARE(power.value(u"title"_s).toString(), u"Power"_s);
        const QVariantMap hold = power.value(u"rows"_s).toList().value(3).toMap();
        QCOMPARE(hold, (QVariantMap{{u"id"_s, u"power:atx_power_long"_s},
                                    {u"icon"_s, u"power"_s},
                                    {u"label"_s, u"Hold power button"_s},
                                    {u"hint"_s, QString()},
                                    {u"danger"_s, true},
                                    {u"enabled"_s, true}}));
        // And of a question: what, and what is to be known before saying yes.
        QCOMPARE(ui::shown(ask(menu, u"power:atx_power_long"_s)),
                 (QVariantMap{{u"title"_s, u"Hold power button"_s},
                              {u"text"_s, u"Hold power button on NAS?"_s},
                              {u"detail"_s, QString()},
                              {u"warning"_s, u"A long press cuts the power. Unsaved work is lost."_s},
                              {u"danger"_s, true},
                              // About one machine: the yes says what is done, and nothing more is asked.
                              {u"yes"_s, u"Hold power button"_s},
                              {u"targets"_s, QVariantList()},
                              {u"tick"_s, QString()},
                              {u"paces"_s, QVariantList()}}));
    }

    void everyRowOfAMenuIsKnownByItsOwnId()
    {
        MenuOf of;
        of.machine = kvm();
        of.status = online();
        of.status->powerIsKnown = true;
        of.status->ports = {core::Port{}, core::Port{}};
        of.status->shownPort = 1;
        core::SavedAction saved;
        saved.id = u"a1"_s;
        saved.name = u"Lock it"_s;
        of.saved = {saved};
        of.wake = {core::WakeTarget{u"Backup box"_s, u"00:11:22:aa:bb:cc"_s}};
        const Menu menu = ui::menuFor(of);
        QStringList ids;
        for (const MenuSection& one : menu.sections) {
            for (const MenuRow& row : one.rows) {
                QVERIFY2(!ids.contains(row.id), qPrintable(row.id));
                ids.append(row.id);
                // And each is found again by it.
                QCOMPARE(menu.row(row.id), &row);
                // Whatever is sent is asked about in a sentence that names the machine.
                if (row.ask) {
                    QVERIFY2(row.ask.value_or(ui::Ask{}).question.contains(u"NAS"_s), qPrintable(row.id));
                }
            }
        }
        QVERIFY(ids.size() > 15);
        QVERIFY(menu.row(u"no such row"_s) == nullptr);
    }
};

QTEST_GUILESS_MAIN(TestMachineMenu)
#include "test_machine_menu.moc"
