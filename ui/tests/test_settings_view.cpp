#include <QFile>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>
#include <QVariantMap>

#include <qkvm/core/settings.h>
#include <qkvm/engine/gpu.h>
#include <qkvm/engine/types.h>

#include "pretend_start.h"
#include "settings_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// What the settings box is handed, for one test: a pretended list of Windows', and what is
// remembered in a folder of the test's own.
struct Desk {
    QTemporaryDir folder;
    QString file = folder.filePath(u"settings.json"_s);
    core::Settings settings{file};
    ui::PretendedStart windows;
};

} // namespace

class TestSettingsView : public QObject {
    Q_OBJECT

private slots:
    void alertsHasAProgramSection()
    {
        const auto groups = qkvm::ui::SettingsView::sectionsFor(u"pc"_s);
        bool found = false;
        for (const QVariant& group : groups) {
            found = found || group.toMap().value(u"sections"_s).toStringList().contains(u"alerts"_s);
        }
        QVERIFY(found);
    }

    void releaseKeyIsChosenAndRemembered()
    {
        Desk desk;
        ui::SettingsView view(desk.windows, desk.settings);
        QCOMPARE(view.property("releaseKey").toString(), u"right-ctrl"_s);
        QVERIFY(view.setProperty("releaseKey", u"f12"_s));
        QCOMPARE(view.property("releaseKey").toString(), u"f12"_s);
        core::Settings reopened(desk.file);
        ui::SettingsView restored(desk.windows, reopened);
        QCOMPARE(restored.property("releaseKey").toString(), u"f12"_s);
        QVERIFY(view.setProperty("releaseKey", u"not-a-key"_s));
        QCOMPARE(view.property("releaseKey").toString(), u"f12"_s);
    }

    // How video is decoded is chosen, remembered and said at once, for every window to take up; a
    // word that is no choice changes nothing.
    void hardwareDecodingIsChosenRememberedAndSaid()
    {
        Desk desk;
        ui::SettingsView view(desk.windows, desk.settings);
        const QSignalSpy chosen(&view, &ui::SettingsView::decodingChosen);
        QCOMPARE(view.property("hardwareDecoding").toString(), u"automatic"_s);
        QCOMPARE(view.decoding(), engine::DecodingChoice::Automatic);
        QVERIFY(view.setProperty("hardwareDecoding", u"off"_s));
        QCOMPARE(view.decoding(), engine::DecodingChoice::Software);
        QCOMPARE(chosen.count(), 1);
        QVERIFY(view.setProperty("hardwareDecoding", u"on"_s));
        QCOMPARE(view.decoding(), engine::DecodingChoice::GraphicsCard);
        QCOMPARE(chosen.count(), 2);
        QVERIFY(view.setProperty("hardwareDecoding", u"on"_s));
        QVERIFY(view.setProperty("hardwareDecoding", u"gpu"_s));
        QCOMPARE(view.property("hardwareDecoding").toString(), u"on"_s);
        QCOMPARE(chosen.count(), 2);
        core::Settings reopened(desk.file);
        const ui::SettingsView restored(desk.windows, reopened);
        QCOMPARE(restored.property("hardwareDecoding").toString(), u"on"_s);
        QCOMPARE(restored.decoding(), engine::DecodingChoice::GraphicsCard);
    }

    // Which graphics adapter is chosen and remembered, and takes effect when the program starts
    // again: the one it was started with stays until then. Each choice is shown with the adapter
    // it comes to on this PC, which is asked and not remembered.
    void theGraphicsCardIsChosenForTheNextStart()
    {
        Desk desk;
        ui::SettingsView view(desk.windows, desk.settings);
        QStringList asked;
        view.nameAdaptersWith([&asked](engine::AdapterChoice choice) {
            const QString name = choice == engine::AdapterChoice::MostPowerful  ? u"Card A"_s
                                 : choice == engine::AdapterChoice::PowerSaving ? u"Card B"_s
                                                                                : u"Card C"_s;
            asked.append(name);
            return name;
        });
        QCOMPARE(view.property("graphicsCard").toString(), u"most-powerful"_s);
        QCOMPARE(view.startedOn(), engine::AdapterChoice::MostPowerful);
        QVERIFY(!view.property("cardChangesAtTheNextStart").toBool());
        const QVariantList cards = view.property("graphicsCards").toList();
        QCOMPARE(cards.size(), 3);
        QCOMPARE(cards.at(0).toMap().value(u"value"_s).toString(), u"most-powerful"_s);
        QCOMPARE(cards.at(0).toMap().value(u"adapter"_s).toString(), u"Card A"_s);
        QCOMPARE(cards.at(1).toMap().value(u"value"_s).toString(), u"power-saving"_s);
        QCOMPARE(cards.at(1).toMap().value(u"adapter"_s).toString(), u"Card B"_s);
        QCOMPARE(cards.at(2).toMap().value(u"value"_s).toString(), u"windows"_s);
        QCOMPARE(cards.at(2).toMap().value(u"adapter"_s).toString(), u"Card C"_s);
        // Whether a monitor is on each: so where it is not known.
        QVERIFY(cards.at(1).toMap().value(u"monitor"_s).toBool());
        view.seeMonitorsWith([](engine::AdapterChoice choice) { return choice != engine::AdapterChoice::PowerSaving; });
        const QVariantList seen = view.property("graphicsCards").toList();
        QVERIFY(seen.at(0).toMap().value(u"monitor"_s).toBool());
        QVERIFY(!seen.at(1).toMap().value(u"monitor"_s).toBool());
        QVERIFY(seen.at(2).toMap().value(u"monitor"_s).toBool());

        QVERIFY(view.setProperty("graphicsCard", u"power-saving"_s));
        QCOMPARE(view.property("graphicsCard").toString(), u"power-saving"_s);
        QVERIFY(view.property("cardChangesAtTheNextStart").toBool());
        QCOMPARE(view.startedOn(), engine::AdapterChoice::MostPowerful);
        QVERIFY(view.setProperty("graphicsCard", u"Card B"_s));
        QCOMPARE(view.property("graphicsCard").toString(), u"power-saving"_s);
        // Back to the one it started with: nothing waits for the next start.
        QVERIFY(view.setProperty("graphicsCard", u"most-powerful"_s));
        QVERIFY(!view.property("cardChangesAtTheNextStart").toBool());
        QVERIFY(view.setProperty("graphicsCard", u"windows"_s));

        // Started again: on the one chosen, and no adapter's name was written down.
        core::Settings reopened(desk.file);
        const ui::SettingsView restored(desk.windows, reopened);
        QCOMPARE(restored.startedOn(), engine::AdapterChoice::AsWindowsChooses);
        QVERIFY(!restored.property("cardChangesAtTheNextStart").toBool());
        QFile file(desk.file);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray written = file.readAll();
        QVERIFY(!written.contains("Card"));
        QVERIFY(!asked.isEmpty());
    }

    void whetherTheProgramStartsWithWindowsIsWhatWindowsSays()
    {
        Desk desk;
        desk.windows.on = true;
        const ui::SettingsView on(desk.windows, desk.settings);
        QVERIFY(on.startsWithWindows());
        QCOMPARE(on.problem(), QString());
        desk.windows.on = false;
        const ui::SettingsView off(desk.windows, desk.settings);
        QVERIFY(!off.startsWithWindows());
        // Looking is not changing: nothing was asked of Windows but how things are.
        QVERIFY(desk.windows.asked.isEmpty());
    }

    void tickingTheBoxPutsTheProgramOnWindowsListAndUntickingTakesItOff()
    {
        Desk desk;
        ui::SettingsView view(desk.windows, desk.settings);
        const QSignalSpy changed(&view, &ui::SettingsView::changed);
        view.setStartsWithWindows(true);
        QCOMPARE(desk.windows.asked, QList<bool>{true});
        QVERIFY(desk.windows.on);
        QVERIFY(view.startsWithWindows());
        QCOMPARE(view.problem(), QString());
        QCOMPARE(changed.count(), 1);

        view.setStartsWithWindows(false);
        QCOMPARE(desk.windows.asked, (QList<bool>{true, false}));
        QVERIFY(!desk.windows.on);
        QVERIFY(!view.startsWithWindows());
        QCOMPARE(changed.count(), 2);
    }

    void whereWindowsDoesNotTakeItTheBoxSaysSoAndShowsWhatIsSo()
    {
        Desk desk;
        desk.windows.refuses = true;
        ui::SettingsView view(desk.windows, desk.settings);
        const QSignalSpy changed(&view, &ui::SettingsView::changed);
        view.setStartsWithWindows(true);
        QVERIFY(!view.startsWithWindows());
        QCOMPARE(view.problem(), u"Windows did not take it: qKVM will not start with Windows."_s);
        // Said also where nothing is other than before: whatever shows a tick takes it back.
        QCOMPARE(changed.count(), 1);

        // And the other way round.
        desk.windows.on = true;
        view.lookAgain();
        QVERIFY(view.startsWithWindows());
        view.setStartsWithWindows(false);
        QVERIFY(view.startsWithWindows());
        QCOMPARE(view.problem(), u"Windows did not take it: qKVM still starts with Windows."_s);

        // What went wrong is said until the next change that goes well.
        desk.windows.refuses = false;
        view.setStartsWithWindows(false);
        QVERIFY(!view.startsWithWindows());
        QCOMPARE(view.problem(), QString());
    }

    void whenTheBoxIsOpenedThingsAreAskedAnew()
    {
        Desk desk;
        desk.windows.refuses = true;
        ui::SettingsView view(desk.windows, desk.settings);
        view.setStartsWithWindows(true);
        QVERIFY(!view.problem().isEmpty());

        // Somebody switched it on in Windows' own settings meanwhile.
        desk.windows.on = true;
        const QSignalSpy changed(&view, &ui::SettingsView::changed);
        view.lookAgain();
        QVERIFY(view.startsWithWindows());
        // And what went wrong the last time the box was open is not said again.
        QCOMPARE(view.problem(), QString());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(desk.windows.asked, QList<bool>{true});
    }

    void whetherTheProgramGoesOnWhenItsWindowIsClosedIsRememberedOnThisPc()
    {
        Desk desk;
        ui::SettingsView view(desk.windows, desk.settings);
        const QSignalSpy changed(&view, &ui::SettingsView::changed);
        // Not until somebody chooses it: closing a window closes it.
        QVERIFY(!view.staysInTray());

        view.setStaysInTray(true);
        QVERIFY(view.staysInTray());
        QCOMPARE(changed.count(), 1);
        QVERIFY(desk.settings.remembered().staysInTray);
        QVERIFY(core::Settings(desk.file).remembered().staysInTray);
        // It has nothing to do with Windows' list.
        QVERIFY(desk.windows.asked.isEmpty());
        // Chosen again, nothing is written and nothing said.
        view.setStaysInTray(true);
        QCOMPARE(changed.count(), 1);

        view.setStaysInTray(false);
        QVERIFY(!view.staysInTray());
        QCOMPARE(changed.count(), 2);
        QVERIFY(!core::Settings(desk.file).remembered().staysInTray);
    }

    void whetherWindowsShowsMessagesOfTheWallIsRememberedOnThisPc()
    {
        Desk desk;
        ui::SettingsView view(desk.windows, desk.settings);
        const QSignalSpy changed(&view, &ui::SettingsView::changed);
        // Until somebody switches it off.
        QVERIFY(view.tellsNews());

        view.setTellsNews(false);
        QVERIFY(!view.tellsNews());
        QCOMPARE(changed.count(), 1);
        QVERIFY(!core::Settings(desk.file).remembered().tellsNews);
        // Chosen again, nothing is written and nothing said.
        view.setTellsNews(false);
        QCOMPARE(changed.count(), 1);
        // Each of the two choices is its own.
        QVERIFY(!view.staysInTray());
        view.setStaysInTray(true);
        QVERIFY(!view.tellsNews());
        QVERIFY(view.staysInTray());

        view.setTellsNews(true);
        QVERIFY(view.tellsNews());
        QVERIFY(view.staysInTray());
        QCOMPARE(changed.count(), 3);
        QVERIFY(core::Settings(desk.file).remembered().tellsNews);
        QVERIFY(core::Settings(desk.file).remembered().staysInTray);
        QVERIFY(desk.windows.asked.isEmpty());
    }

    // --- the box's sections ------------------------------------------------------------------------

    void theBoxListsThisPcsSectionsOnlyWhereItsWindowShowsThisPcsWall()
    {
        // The keys of the sections listed for a wall, group by group: "group: one, two".
        const auto listed = [](const QString& wall) {
            QStringList groups;
            for (const QVariant& one : ui::SettingsView::sectionsFor(wall)) {
                const QVariantMap group = one.toMap();
                groups.append(group.value(u"group"_s).toString() + u": "_s +
                              group.value(u"sections"_s).toStringList().join(u", "_s));
            }
            return groups;
        };
        // This PC's wall: its KVMs and the file of them, then the program's own.
        QCOMPARE(listed(u"pc"_s), (QStringList{u"pc: kvms, actions, files"_s,
                                               u"program: general, video, alerts, security, updates, about"_s}));
        // A server's wall: what is the server's is on its page, and the program's own is here.
        QCOMPARE(listed(u"server"_s), QStringList{u"program: general, video, alerts, security, updates, about"_s});
        // One that whoever is signed in manages: its wall as a file too, as the page lists it for an
        // admin, under a heading of its own, last: it comes in below the rest, which does not move.
        QCOMPARE(listed(u"managed"_s),
                 (QStringList{u"program: general, video, alerts, security, updates, about"_s, u"server: files"_s}));
        // No wall yet (nobody is signed in): the program's own.
        QCOMPARE(listed(u"none"_s), QStringList{u"program: general, video, alerts, security, updates, about"_s});
        QCOMPARE(listed(QString()), QStringList{u"program: general, video, alerts, security, updates, about"_s});
    }

    void theBoxOpensWhereItWasLeftWhereThatIsListed()
    {
        Desk desk;
        ui::SettingsView view(desk.windows, desk.settings);
        const QSignalSpy shown(&view, &ui::SettingsView::sectionChanged);
        // Never shown before: at the first section that is listed.
        QCOMPARE(view.section(), QString());
        QCOMPARE(view.opensAt(u"pc"_s), u"kvms"_s);
        QCOMPARE(view.opensAt(u"server"_s), u"general"_s);
        QCOMPARE(view.opensAt(u"none"_s), u"general"_s);

        // Left at About: there, for any wall.
        view.show(u"about"_s);
        QCOMPARE(view.section(), u"about"_s);
        QCOMPARE(shown.count(), 1);
        QCOMPARE(view.opensAt(u"pc"_s), u"about"_s);
        QCOMPARE(view.opensAt(u"server"_s), u"about"_s);
        // Shown again: nothing is new.
        view.show(u"about"_s);
        QCOMPARE(shown.count(), 1);

        // Left at this PC's export and import: there where the window shows this PC's wall, and at
        // the first that is listed where it shows a server's, which has no such section. What was
        // left is not forgotten by that: back at this PC's wall, the box opens there again.
        view.show(u"files"_s);
        QCOMPARE(shown.count(), 2);
        QCOMPARE(view.opensAt(u"pc"_s), u"files"_s);
        QCOMPARE(view.opensAt(u"server"_s), u"general"_s);
        // (And where it manages a server's wall, at that wall's export and import.)
        QCOMPARE(view.opensAt(u"managed"_s), u"files"_s);
        QCOMPARE(view.section(), u"files"_s);
        QCOMPARE(view.opensAt(u"pc"_s), u"files"_s);

        // A section that is listed nowhere is not shown, and changes nothing.
        view.show(u"accounts"_s);
        QCOMPARE(view.section(), u"files"_s);
        QCOMPARE(shown.count(), 2);
        // (Nothing of it is Windows' business.)
        QVERIFY(desk.windows.asked.isEmpty());
    }

    void theBoxOpensWhereItWasLeftAlsoAfterTheProgramWasStartedAgain()
    {
        Desk desk;
        {
            ui::SettingsView view(desk.windows, desk.settings);
            QCOMPARE(view.section(), QString());
            view.show(u"general"_s);
            view.show(u"files"_s);
            // (Not written at each section shown on the way: when the box is closed.)
            QCOMPARE(core::Settings(desk.file).remembered().settingsSection, QString());
            view.closed();
            QCOMPARE(core::Settings(desk.file).remembered().settingsSection, u"files"_s);
            // Closed once more where it was: nothing to write.
            const quint64 writings = desk.settings.writings();
            view.closed();
            QCOMPARE(desk.settings.writings(), writings);
        }
        // The next start.
        core::Settings settings(desk.file);
        const ui::SettingsView again(desk.windows, settings);
        QCOMPARE(again.section(), u"files"_s);
        QCOMPARE(again.opensAt(u"pc"_s), u"files"_s);
        QCOMPARE(again.opensAt(u"server"_s), u"general"_s);
        // Every section a box lists, for any wall, is one the settings keep.
        for (const QString& wall : {u"pc"_s, u"server"_s, u"managed"_s, u"none"_s}) {
            for (const QVariant& group : ui::SettingsView::sectionsFor(wall)) {
                for (const QString& key : group.toMap().value(u"sections"_s).toStringList()) {
                    QVERIFY2(core::Remembered::namesASettingsSection(key), qPrintable(key));
                }
            }
        }
    }

    void whereTheProgramEndsWithTheBoxOpenTheSectionItShowsIsWrittenDownToo()
    {
        Desk desk;
        {
            ui::SettingsView view(desk.windows, desk.settings);
            view.show(u"about"_s);
            // The box is open on About, and is not closed before the program ends.
            QCOMPARE(core::Settings(desk.file).remembered().settingsSection, QString());
        }
        QCOMPARE(core::Settings(desk.file).remembered().settingsSection, u"about"_s);
        // Where that is what is written down already, nothing is written as the program ends.
        const quint64 writings = desk.settings.writings();
        {
            const ui::SettingsView again(desk.windows, desk.settings);
            QCOMPARE(again.section(), u"about"_s);
        }
        QCOMPARE(desk.settings.writings(), writings);
    }

    void whatElseIsRememberedIsKeptWhenThatIsWrittenDown()
    {
        Desk desk;
        core::Remembered before;
        before.server = u"http://192.168.1.10:8787/"_s;
        before.theme = u"light"_s;
        QVERIFY(desk.settings.remember(before).ok());
        ui::SettingsView view(desk.windows, desk.settings);
        view.setStaysInTray(true);
        const core::Remembered after = core::Settings(desk.file).remembered();
        QCOMPARE(after.server, u"http://192.168.1.10:8787/"_s);
        QCOMPARE(after.theme, u"light"_s);
        QVERIFY(after.staysInTray);
    }
};

QTEST_GUILESS_MAIN(TestSettingsView)
#include "test_settings_view.moc"
