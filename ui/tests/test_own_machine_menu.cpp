#include <optional>

#include <QTest>

#include <qkvm/core/server_types.h>

#include "machine_menu.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using ui::Menu;
using ui::MenuOf;
using ui::MenuRow;
using ui::MenuSection;

// The machine menu of this PC's own wall, where no server says what a machine can be asked (`can`): what its own
// driver says instead. A Fingerbot paired with a Comet (rule 29), by the driver's word that one is paired.
class TestOwnMachineMenu : public QObject {
    Q_OBJECT

private:
    // A Comet of this PC's own wall: no `can`, as its driver says none.
    static core::Machine comet()
    {
        core::Machine machine;
        machine.id = u"own-1"_s;
        machine.name = u"NAS"_s;
        machine.host = u"192.168.1.22"_s;
        machine.kind = u"kvmd"_s;
        return machine;
    }
    static core::Status online()
    {
        core::Status status;
        status.state = u"online"_s;
        return status;
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

private slots:
    void aCometsFingerbotHasItsRowsWhereItsDriverSaysOneIsPaired()
    {
        // The remote level's rows and words (b3), for a Fingerbot this PC's own driver heard of.
        MenuOf of;
        of.machine = comet();
        of.status = online();
        Menu menu = ui::menuFor(of);
        QCOMPARE(rows(menu, u"Power"_s), (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s}));
        QCOMPARE(section(menu, u"Power"_s)->note,
                 u"Power and reset buttons appear here when the KVM has an ATX power board, or a Fingerbot paired with "
                 "it."_s);
        of.status->fingerbot = core::Fingerbot{};
        menu = ui::menuFor(of);
        QCOMPARE(rows(menu, u"Power"_s), (QStringList{u"power:jiggler_on"_s, u"power:jiggler_off"_s,
                                                      u"power:fingerbot_power"_s, u"power:fingerbot_power_long"_s}));
        QCOMPARE(section(menu, u"Power"_s)->note, QString());
        QCOMPARE(menu.row(u"power:fingerbot_power"_s)->label, u"Press with Fingerbot"_s);
        QVERIFY(menu.row(u"power:fingerbot_power_long"_s)->danger);
        QCOMPARE(menu.battery, QString());
        // Its battery low: said at the top.
        of.status->fingerbot = core::Fingerbot{12, true};
        QCOMPARE(ui::menuFor(of).battery,
                 u"The battery of its Fingerbot is low (12 %). A press may fail: charge it."_s);
        // Several machines: where any has one.
        MenuOf other;
        other.machine = comet();
        other.machine.id = u"own-2"_s;
        other.status = online();
        const Menu several = ui::menuForSeveral({other, of});
        QVERIFY(several.row(u"power:fingerbot_power"_s) != nullptr);
        QVERIFY(ui::menuForSeveral({other, other}).row(u"power:fingerbot_power"_s) == nullptr);
        // Where a server says what the machine can be asked, its word stands: none listed, none offered.
        of.status->capabilities = QStringList{u"screen"_s, u"keys"_s, u"text"_s, u"jiggler"_s};
        menu = ui::menuFor(of);
        QVERIFY(menu.row(u"power:fingerbot_power"_s) == nullptr);
        QCOMPARE(menu.battery, QString());
    }

    void anOwnServerStartsIntoItsSetupForOneMachineAskedFirst()
    {
        // This PC's own server, whose controller offers it (its driver lists `bios_setup`, c5): the remote level's row
        // and question (b4), for one machine, naming it; never in a menu for several.
        MenuOf of;
        of.machine.id = u"own-s"_s;
        of.machine.name = u"Rack server"_s;
        of.machine.kind = u"redfish"_s;
        of.machine.hasPicture = false;
        of.machine.own = true;
        of.status = online();
        of.status->hasPicture = false;
        of.status->powerIsKnown = true;
        of.status->power = true;
        of.status->capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"reboot"_s, u"bios_setup"_s};
        const Menu menu = ui::menuFor(of);
        const MenuRow* setup = menu.row(u"power:bios_setup"_s);
        QVERIFY(setup != nullptr);
        const ui::Ask asked = setup->ask.value_or(ui::Ask{});
        QVERIFY(asked.confirm);
        QVERIFY(asked.question.startsWith(u"Rack server will restart now"_s));
        QCOMPARE(asked.yes, u"Restart into setup"_s);
        QVERIFY(ui::menuForSeveral({of, of}).row(u"power:bios_setup"_s) == nullptr);
        // Not offered by its controller: no row.
        of.status->capabilities->removeAll(u"bios_setup"_s);
        QVERIFY(ui::menuFor(of).row(u"power:bios_setup"_s) == nullptr);
    }
};

QTEST_MAIN(TestOwnMachineMenu)
#include "test_own_machine_menu.moc"
