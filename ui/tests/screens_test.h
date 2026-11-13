#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/settings.h>

#include "about_model.h"
#include "actions_view.h"
#include "add_kvm_view.h"
#include "application.h"
#include "control_view.h"
#include "edit_kvm_view.h"
#include "files_view.h"
#include "look.h"
#include "machine_menu.h"
#include "own_files_view.h"
#include "own_kvms_view.h"
#include "pretend_start.h"
#include "pretend_wall.h"
#include "relative_pointer.h"
#include "screen_capture.h"
#include "session_view.h"
#include "settings_view.h"
#include "stand_in.h"
#include "video_tile.h"
#include "wall_model.h"
#include "wall_view.h"
#include "workspaces_view.h"

using namespace Qt::StringLiterals;

// What the tests of the screens share, in the three programs they are in (the wall and its cards,
// a machine shown large, the boxes): every screen is loaded the way the program loads it, and must
// come up without a word of complaint from QML. It runs without a desktop and without a graphics
// card: Qt's off-screen platform, and Qt Quick drawing in software.
class ScreensTest : public QObject {
protected:
    QStringList m_complaints;

    void collectComplaintsOf(QQmlApplicationEngine& engine)
    {
        m_complaints.clear();
        connect(&engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError>& warnings) {
            for (const QQmlError& warning : warnings) {
                m_complaints.append(warning.toString());
            }
        });
    }

    // For whoever wants to look at the screens without starting the program: with QKVM_SCREENSHOTS set
    // to a folder, each screen is kept there as a picture, as Qt Quick draws it in software.
    static void keepPictureOf(QQuickWindow* window, const QString& name)
    {
        const QString folder = qEnvironmentVariable("QKVM_SCREENSHOTS");
        if (folder.isEmpty()) {
            return;
        }
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(window->grabWindow().save(QDir(folder).filePath(name + u".png"_s)));
    }

    // What a window is given for doing things to machines, where a test does none of that: nobody
    // may. And for the wall as a file, a server's and this PC's own. One of each for each window a
    // test makes, kept until the test is over: a window is not left with nothing where it was
    // handed something (it says so, line by line, of every property it reads there).
    std::vector<std::unique_ptr<qkvm::ui::ActionsView>> m_noActions;
    std::vector<std::unique_ptr<qkvm::ui::FilesView>> m_noFiles;
    std::vector<std::unique_ptr<qkvm::ui::OwnFilesView>> m_noOwnFiles;
    // And how it looks: as on a PC where nothing was chosen, remembered in a folder of the test's
    // own. One for each window a test makes, kept until the test is over.
    QTemporaryDir m_folder;
    std::vector<std::unique_ptr<qkvm::core::Settings>> m_settings;
    std::vector<std::unique_ptr<qkvm::ui::Look>> m_looks;
    // And whether it starts with Windows, which is only pretended here.
    std::vector<std::unique_ptr<qkvm::ui::PretendedStart>> m_starts;
    std::vector<std::unique_ptr<qkvm::ui::SettingsView>> m_ownSettings;
    // What the box that adds a KVM shows and asks: one for all the windows of a test.
    qkvm::ui::AddKvmView m_adding;
    // And what the box that changes one shows and asks.
    qkvm::ui::EditKvmView m_editing;
    // What the screen of a machine that is shown large is told of its keyboard and mouse, and
    // asks: nothing can be taken until a test says so.
    qkvm::ui::ControlView m_control;
    // This PC's KVMs as the settings list them: none, of no wall.
    qkvm::ui::OwnKvmsView m_kvms;

    // The window as the program makes it, with what the program hands it. Null when it did not come up.
    QQuickWindow* load(QQmlApplicationEngine& engine, qkvm::ui::AboutModel& about, qkvm::ui::SessionView& session,
                       qkvm::ui::WallView& wall, qkvm::ui::ActionsView* actions = nullptr,
                       qkvm::ui::FilesView* files = nullptr, qkvm::ui::OwnFilesView* ownFiles = nullptr)
    {
        collectComplaintsOf(engine);
        qkvm::ui::keepOffTheNetwork(engine);
        if (actions == nullptr) {
            m_noActions.push_back(std::make_unique<qkvm::ui::ActionsView>());
            actions = m_noActions.back().get();
        }
        if (files == nullptr) {
            m_noFiles.push_back(std::make_unique<qkvm::ui::FilesView>());
            files = m_noFiles.back().get();
        }
        if (ownFiles == nullptr) {
            m_noOwnFiles.push_back(std::make_unique<qkvm::ui::OwnFilesView>());
            ownFiles = m_noOwnFiles.back().get();
        }
        const QString file = m_folder.filePath(u"settings-%1.json"_s.arg(m_looks.size()));
        QFile::remove(file);
        m_settings.push_back(std::make_unique<qkvm::core::Settings>(file));
        m_looks.push_back(std::make_unique<qkvm::ui::Look>(*m_settings.back()));
        m_starts.push_back(std::make_unique<qkvm::ui::PretendedStart>());
        m_ownSettings.push_back(std::make_unique<qkvm::ui::SettingsView>(*m_starts.back(), *m_settings.back()));
        engine.setInitialProperties({{u"about"_s, QVariant::fromValue(&about)},
                                     {u"session"_s, QVariant::fromValue(&session)},
                                     {u"wall"_s, QVariant::fromValue(&wall)},
                                     {u"actions"_s, QVariant::fromValue(actions)},
                                     {u"look"_s, QVariant::fromValue(m_looks.back().get())},
                                     {u"settings"_s, QVariant::fromValue(m_ownSettings.back().get())},
                                     {u"files"_s, QVariant::fromValue(files)},
                                     {u"ownFiles"_s, QVariant::fromValue(ownFiles)},
                                     {u"adding"_s, QVariant::fromValue(&m_adding)},
                                     {u"editing"_s, QVariant::fromValue(&m_editing)},
                                     {u"control"_s, QVariant::fromValue(&m_control)},
                                     {u"kvms"_s, QVariant::fromValue(&m_kvms)}});
        engine.loadFromModule("Qkvm", "Main");
        return engine.rootObjects().size() == 1 ? qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst())
                                                : nullptr;
    }

    // Somebody is signed in: the window shows its wall.
    static qkvm::ui::SessionView::Shown signedIn()
    {
        qkvm::ui::SessionView::Shown shown;
        shown.stage = u"wall"_s;
        shown.address = u"192.168.1.10:8787"_s;
        shown.account = u"anna"_s;
        // An admin: the bar is as full as it gets.
        shown.manages = true;
        return shown;
    }

    // This PC's own wall is shown: nobody is signed in to anything, and whoever sits here manages it.
    static qkvm::ui::SessionView::Shown atItsOwnWall()
    {
        qkvm::ui::SessionView::Shown shown;
        shown.stage = u"wall"_s;
        shown.manages = true;
        shown.encrypted = true;
        shown.ownWall = true;
        return shown;
    }

    // Shown, and the one the keyboard goes to.
    static bool bringUp(QQuickWindow* window)
    {
        window->show();
        window->requestActivate();
        return QTest::qWaitForWindowActive(window);
    }

    static void type(QQuickWindow* window, const QString& text)
    {
        for (const QChar letter : text) {
            QTest::keyClick(window, letter.toLatin1());
        }
    }

    // Whether a visible line of text under an item says exactly this.
    static bool says(const QQuickItem* under, const QString& text)
    {
        if (under == nullptr || !under->isVisible()) {
            return false;
        }
        if (under->inherits("QQuickText") && under->property("text").toString() == text) {
            return true;
        }
        const QList<QQuickItem*> children = under->childItems();
        return std::ranges::any_of(children, [&](const QQuickItem* child) { return says(child, text); });
    }

    // The line of text that says exactly this and can be seen, wherever in the window it is; null
    // when there is none.
    static const QQuickItem* saying(const QQuickItem* under, const QString& text)
    {
        if (under == nullptr || !under->isVisible()) {
            return nullptr;
        }
        if (under->inherits("QQuickText") && under->property("text").toString() == text) {
            return under;
        }
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            if (const QQuickItem* found = saying(child, text)) {
                return found;
            }
        }
        return nullptr;
    }

    // Whether the row of an open menu that says this has the tick of the one that is chosen: it is
    // then written heavier, too.
    static bool isTicked(const QObject* menu, const QString& text)
    {
        const QQuickItem* row = rowOf(menu, text);
        return row != nullptr && row->property("font").value<QFont>().weight() == QFont::DemiBold;
    }

    // The row of an open menu that says this: looked for in the menu, since a wall has many a "1".
    static const QQuickItem* rowOf(const QObject* menu, const QString& text)
    {
        return saying(menu->property("contentItem").value<QQuickItem*>(), text);
    }

    // Chooses, in the one Choice of this name that can be seen, the entry that says this, as a
    // person does: a click opens its list, and the entry is clicked where it is. Whether that was
    // so, and the list closed again.
    static bool chooseIn(QQuickWindow* window, const QString& choice, const QString& label)
    {
        const QQuickItem* button = theOneSeen(window, choice);
        if (button == nullptr || seenNamed(window, u"choice"_s) != 0) {
            return false;
        }
        clickOn(window, button);
        if (!QTest::qWaitFor([&] { return seenNamed(window, u"choice"_s) > 0; })) {
            return false;
        }
        QList<const QQuickItem*> entries;
        named(window->contentItem(), u"choice"_s, entries);
        const auto entry = std::ranges::find_if(
            entries, [&](const QQuickItem* one) { return isSeen(one) && saying(one, label) != nullptr; });
        if (entry == entries.end()) {
            return false;
        }
        clickOn(window, *entry);
        return QTest::qWaitFor([&] { return seenNamed(window, u"choice"_s) == 0; });
    }

    // The strip of the window above what this frame shows, as drawn now: what a list scrolls out
    // over its top edge is not drawn there. (Qt Quick drawing in software, as the tests do, did not
    // hold a drawing of lines to that edge: Check.qml says how a drawing is held to it.)
    // It is taken once what changes there has been at rest for longer than a change of colour takes
    // (Theme.medium), as that of a button whose menu opened.
    static QImage stripAbove(QQuickWindow* window, const QQuickItem* frame)
    {
        const QRectF where = frame->mapRectToScene(QRectF(0, 0, frame->width(), frame->height()));
        const auto grab = [&] {
            const QImage drawn = window->grabWindow();
            const qreal ratio = drawn.devicePixelRatio();
            return drawn.copy(QRectF(where.left() * ratio, 0, where.width() * ratio, where.top() * ratio).toRect());
        };
        QImage strip = grab();
        QElapsedTimer still;
        still.start();
        for (int tries = 0; tries < 300 && still.elapsed() < 300; ++tries) {
            QTest::qWait(16);
            QImage again = grab();
            if (again != strip) {
                strip = std::move(again);
                still.restart();
            }
        }
        return strip;
    }

    // Whether one of the page's icons of this name is drawn, and seen, in what lies under an item.
    static bool drawsIcon(const QQuickItem* under, const QString& icon)
    {
        if (under->property("name").toString() == icon && under->property("paths").isValid()) {
            return isSeen(under);
        }
        const QList<QQuickItem*> children = under->childItems();
        return std::ranges::any_of(children, [&](const QQuickItem* child) { return drawsIcon(child, icon); });
    }

    // Shows the settings' section of this key, as a person does: by its line in their list. Whether
    // there was such a line to click.
    static bool chooseSection(QQuickWindow* window, const QString& key)
    {
        QList<const QQuickItem*> found;
        named(window->contentItem(), u"settingsSection"_s, found);
        const auto line = std::ranges::find_if(
            found, [&](const QQuickItem* one) { return isSeen(one) && one->property("key").toString() == key; });
        if (line == found.end()) {
            return false;
        }
        // (Where the sections lie in a row wider than the window, the row is scrolled to it first,
        // as a person scrolls it.)
        if (!isInside(window, *line)) {
            for (QQuickItem* up = (*line)->parentItem(); up != nullptr; up = up->parentItem()) {
                if (up->inherits("QQuickFlickable")) {
                    const auto* content = up->property("contentItem").value<QQuickItem*>();
                    up->setProperty("contentX", std::max(0.0, (*line)->mapToItem(content, QPointF(0, 0)).x() - 10));
                    break;
                }
            }
        }
        clickOn(window, *line);
        return true;
    }

    // What the one Choice of this name that can be seen says is chosen; empty where there is none.
    static QString chosenIn(const QQuickWindow* window, const QString& choice)
    {
        const QQuickItem* button = theOneSeen(window, choice);
        return button == nullptr ? QString() : button->property("text").toString();
    }

    // A click on an item, where it is once the window has drawn it: as a person clicks, on what is
    // seen. What was opened a moment ago is laid out with its first frame, and not before.
    // Whether a card of the wall is on its way to another place.
    static bool aCardGlides(const QQuickItem* under)
    {
        if (under->property("places").isValid() && under->property("key").isValid()) {
            const QObjectList& own = under->children();
            return std::ranges::any_of(own, [](const QObject* one) {
                const QObject* glide =
                    one->inherits("QQuickBehavior") ? one->property("animation").value<QObject*>() : nullptr;
                return glide != nullptr && glide->property("running").toBool();
            });
        }
        const QList<QQuickItem*> children = under->childItems();
        return std::ranges::any_of(children, [](const QQuickItem* child) { return aCardGlides(child); });
    }

    // Waits for a frame, and for as many more as the wall's cards take to come to rest: a card
    // that glides to another place may pass over what is about to be clicked, and take the click.
    static void letTheWallComeToRest(QQuickWindow* window)
    {
        for (int frames = 0; frames < 400; ++frames) {
            QSignalSpy drawn(window, &QQuickWindow::frameSwapped);
            window->update();
            static_cast<void>(drawn.wait(2000));
            if (!aCardGlides(window->contentItem())) {
                return;
            }
        }
    }

    static void clickOn(QQuickWindow* window, const QQuickItem* item)
    {
        letTheWallComeToRest(window);
        QTest::mouseClick(window, Qt::LeftButton, {},
                          item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }

    // Every item of this name in what a window shows. (Asked of the items as they are drawn: a card
    // is made by the wall for each machine, and is nobody's child in the other sense.)
    static void named(const QQuickItem* under, const QString& name, QList<const QQuickItem*>& found)
    {
        if (under->objectName() == name) {
            found.append(under);
        }
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            named(child, name, found);
        }
    }

    // Puts the keyboard on the item of this name, as Tab would when it came to it. False where the
    // window shows no such item.
    static bool tabTo(QQuickItem* under, const QString& name)
    {
        if (under->objectName() == name) {
            under->forceActiveFocus(Qt::TabFocusReason);
            return true;
        }
        const QList<QQuickItem*> children = under->childItems();
        return std::ranges::any_of(children, [&](QQuickItem* child) { return tabTo(child, name); });
    }

    // A click at a place in the window, once the window has drawn.
    static void clickOn(QQuickWindow* window, const QPoint& place)
    {
        letTheWallComeToRest(window);
        QTest::mouseClick(window, Qt::LeftButton, {}, place);
    }

    // The card of the machine with this key on the wall a window shows; null where it shows none.
    static const QQuickItem* cardOf(const QQuickItem* under, const QString& key)
    {
        if (under->property("places").isValid() && under->property("key").toString() == key) {
            return under;
        }
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            if (const QQuickItem* found = cardOf(child, key); found != nullptr) {
                return found;
            }
        }
        return nullptr;
    }

    // The card that lies at a place of the window, under whatever lies over it there; null where
    // none does.
    static const QQuickItem* cardAt(const QQuickItem* under, const QPointF& at)
    {
        if (under->property("places").isValid() && under->property("key").isValid()) {
            return isSeenAt(under, at) ? under : nullptr;
        }
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            if (const QQuickItem* found = cardAt(child, at); found != nullptr) {
                return found;
            }
        }
        return nullptr;
    }
    static bool isSeenAt(const QQuickItem* item, const QPointF& at)
    {
        return item->isVisible() && item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).contains(at);
    }

    // The one thing of this name in a card.
    static const QQuickItem* inCard(const QQuickItem* card, const QString& name)
    {
        QList<const QQuickItem*> found;
        if (card != nullptr) {
            named(card, name, found);
        }
        return found.size() == 1 ? found.constFirst() : nullptr;
    }

    // The wall's machines in the order they are drawn in: the order that is being made while one
    // is, and otherwise what the bar's choices leave.
    static QStringList made(qkvm::ui::WallView& wall)
    {
        return wall.arranging() ? wall.arrangement() : wall.shown()->keys();
    }

    // A small wall: five machines, "a" to "e", none with a picture.
    static QList<qkvm::ui::WallTile> fiveMachines()
    {
        QList<qkvm::ui::WallTile> tiles;
        for (const QString& key : {u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}) {
            qkvm::ui::WallTile tile;
            tile.key = key;
            tile.name = u"Machine "_s + key.toUpper();
            tile.host = u"192.168.1.2"_s + QString::number(tiles.size());
            tile.state = u"online"_s;
            tile.label = u"Online"_s;
            tiles.append(tile);
        }
        return tiles;
    }

    // The one item of this name in what a window shows; null where there is none, or more than one.
    static const QQuickItem* theOne(const QQuickWindow* window, const QString& name)
    {
        QList<const QQuickItem*> found;
        named(window->contentItem(), name, found);
        return found.size() == 1 ? found.constFirst() : nullptr;
    }

    // Whether an item can be seen: it, and everything it is in.
    static bool isSeen(const QQuickItem* item)
    {
        for (const QQuickItem* up = item; up != nullptr; up = up->parentItem()) {
            if (!up->isVisible()) {
                return false;
            }
        }
        return true;
    }

    // How many items of this name can be seen in what a window shows.
    static int seenNamed(const QQuickWindow* window, const QString& name)
    {
        QList<const QQuickItem*> found;
        named(window->contentItem(), name, found);
        return static_cast<int>(std::ranges::count_if(found, isSeen));
    }

    // The one item of this name that can be seen, where a list has one in every row and shows it in
    // some; null where none or several can be seen.
    static const QQuickItem* theOneSeen(const QQuickWindow* window, const QString& name)
    {
        QList<const QQuickItem*> found;
        named(window->contentItem(), name, found);
        const QQuickItem* seen = nullptr;
        for (const QQuickItem* item : std::as_const(found)) {
            if (!isSeen(item)) {
                continue;
            }
            if (seen != nullptr) {
                return nullptr;
            }
            seen = item;
        }
        return seen;
    }

    // The menu of a made-up KVM with two computers behind it and a power board, as the program's own
    // rules make it and as the screens are handed it.
    static QVariantMap menuOfAKvm()
    {
        qkvm::ui::MenuOf of;
        of.machine.id = u"k1"_s;
        of.machine.name = u"NAS"_s;
        of.machine.host = u"192.168.1.22"_s;
        of.machine.kind = u"kvmd"_s;
        qkvm::core::Status status;
        status.state = u"no_signal"_s;
        status.model = u"KVM-A"_s;
        status.version = u"4.82"_s;
        status.powerIsKnown = true;
        status.ports = {qkvm::core::Port{QString(), false}, qkvm::core::Port{u"Build box"_s, false}};
        status.shownPort = 0;
        of.status = status;
        qkvm::core::SavedAction saved;
        saved.id = u"a1"_s;
        saved.name = u"Lock it"_s;
        saved.keys = u"MetaLeft,KeyL"_s;
        of.saved = {saved};
        return qkvm::ui::shown(qkvm::ui::menuFor(of));
    }

    // The menu of several made-up machines, two KVMs with a power board each and a server, as the
    // program's own rules make it; and the machines themselves, as a question about them names them.
    static qkvm::ui::Menu menuOfSeveral()
    {
        qkvm::ui::MenuOf first;
        first.machine.id = u"k1"_s;
        first.machine.name = u"NAS"_s;
        first.machine.kind = u"kvmd"_s;
        qkvm::core::Status status;
        status.state = u"online"_s;
        status.powerIsKnown = true;
        first.status = status;
        qkvm::core::SavedAction saved;
        saved.id = u"a1"_s;
        saved.name = u"Lock it"_s;
        saved.keys = u"MetaLeft,KeyL"_s;
        first.saved = {saved};
        qkvm::ui::MenuOf second = first;
        second.machine.id = u"k2"_s;
        second.machine.name = u"Build box"_s;
        qkvm::ui::MenuOf rack;
        rack.machine.id = u"s1"_s;
        rack.machine.name = u"Rack server"_s;
        rack.machine.kind = u"redfish"_s;
        rack.machine.hasPicture = false;
        status.hasPicture = false;
        status.state = u"no_signal"_s;
        status.power = false;
        qkvm::core::ServerReadings readings;
        readings.locator = false;
        status.server = readings;
        rack.status = status;
        return qkvm::ui::menuForSeveral({first, second, rack});
    }

    static QList<qkvm::ui::Target> severalMachines()
    {
        return {{u"NAS"_s, u"192.168.1.22"_s, u"online"_s, u"Online"_s},
                {u"Build box"_s, u"192.168.1.23"_s, u"offline"_s, u"Offline"_s},
                {u"Rack server"_s, u"192.168.1.60"_s, u"no_signal"_s, u"Powered off"_s}};
    }

    // A run on several machines, as the screens are handed it.
    static QVariantMap aRun(const QString& summary, bool running, const QString& again, const QVariantList& rows)
    {
        return {{u"title"_s, u"Hold power button · %1 KVMs"_s.arg(rows.size())},
                {u"summary"_s, summary},
                {u"running"_s, running},
                {u"stopping"_s, false},
                {u"again"_s, again},
                {u"rows"_s, rows}};
    }

    static QVariantMap runRow(const QString& name, const QString& state, const QString& text)
    {
        return {{u"name"_s, name}, {u"state"_s, state}, {u"text"_s, text}};
    }

    // Whether an item is whole inside its window.
    // Whether all of `item` lies within `area`'s own bounds (a scroll clips what lies outside them).
    static bool isWithin(const QQuickItem* area, const QQuickItem* item)
    {
        const QRectF where = item->mapRectToItem(area, QRectF(0, 0, item->width(), item->height()));
        return where.left() >= 0 && where.top() >= 0 && where.right() <= area->width() &&
               where.bottom() <= area->height();
    }

    static bool isInside(const QQuickWindow* window, const QQuickItem* item)
    {
        const QRectF where = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        return where.left() >= 0 && where.top() >= 0 && where.right() <= window->width() &&
               where.bottom() <= window->height();
    }

    // Whether each entry of the open list of a Choice is whole inside the window, and has the room
    // to say what it says.
    static bool choicesCanBeRead(const QQuickWindow* window)
    {
        QList<const QQuickItem*> choices;
        named(window->contentItem(), u"choice"_s, choices);
        return !choices.isEmpty() && std::ranges::all_of(choices, [&](const QQuickItem* choice) {
            QList<const QQuickItem*> said;
            lines(choice, said);
            return isInside(window, choice) && said.size() == 1 &&
                   said.constFirst()->x() + said.constFirst()->implicitWidth() <= choice->width();
        });
    }

    // A KVM of this PC's own wall as the box that changes it is handed it: over HTTPS, and known by
    // a certificate. And two certificates as a review reads them, each by its SHA-256 and as the
    // page writes it.
    static qkvm::ui::EditKvmView::Shown aKvmOfTheWall()
    {
        qkvm::ui::EditKvmView::Shown kvm;
        kvm.id = u"0a1b2c3d"_s;
        kvm.called = u"NAS"_s;
        // (As a sentence calls it: set off, as whoever knows the KVM hands it over.)
        kvm.inASentence = u"\"NAS\""_s;
        kvm.name = u"NAS"_s;
        kvm.address = u"192.168.1.22"_s;
        kvm.user = u"admin"_s;
        kvm.group = u"Rack A"_s;
        kvm.certificate = writtenOne();
        kvm.https = true;
        return kvm;
    }
    static QByteArray oneCertificate()
    {
        return QByteArray::fromHex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    }
    static QByteArray anotherCertificate()
    {
        return QByteArray::fromHex("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100");
    }
    static QString writtenOne()
    {
        return u"00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF"_s;
    }
    static QString writtenAnother()
    {
        return u"FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00:FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00"_s;
    }
    // The KVM of aKvmOfTheWall() as its box shows it once it was saved at another address: known
    // by the other certificate there; or spoken to in the clear, and known by none.
    static qkvm::ui::EditKvmView::Shown movedKvm(bool inTheClear)
    {
        qkvm::ui::EditKvmView::Shown kvm = aKvmOfTheWall();
        kvm.address = inTheClear ? u"http://192.168.1.30:8080"_s : u"192.168.1.30:8443"_s;
        kvm.certificate = inTheClear ? QString() : writtenAnother();
        kvm.https = !inTheClear;
        kvm.overPlainHttp = inTheClear;
        return kvm;
    }
    // In lines of eight, as the box shows one to be compared.
    static QString oneInLines()
    {
        return u"00:11:22:33:44:55:66:77\n88:99:AA:BB:CC:DD:EE:FF\n00:11:22:33:44:55:66:77\n88:99:AA:BB:CC:DD:EE:FF"_s;
    }
    static QString anotherInLines()
    {
        return u"FF:EE:DD:CC:BB:AA:99:88\n77:66:55:44:33:22:11:00\nFF:EE:DD:CC:BB:AA:99:88\n77:66:55:44:33:22:11:00"_s;
    }
    // A button of a box as it stood in one of the box's states: which, and where in the window.
    struct Stood {
        QString state;
        QString button;
        QRectF where;
    };
    // Every button under an item that can be seen, each by its name and where it is in the window.
    static void buttonsUnder(const QQuickItem* under, const QString& state, QList<Stood>& found)
    {
        if (!under->isVisible()) {
            return;
        }
        if (under->inherits("QQuickAbstractButton")) {
            found.append(
                {state, under->objectName(), under->mapRectToScene(QRectF(0, 0, under->width(), under->height()))});
        }
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            buttonsUnder(child, state, found);
        }
    }
    static QString whereStood(const Stood& one)
    {
        return u"%1 (%2): %3,%4 %5x%6"_s.arg(one.button, one.state)
            .arg(one.where.x())
            .arg(one.where.y())
            .arg(one.where.width())
            .arg(one.where.height());
    }

    // What a window's notices say, in the order they came.
    static QStringList toldBy(qkvm::ui::ActionsView& actions)
    {
        QStringList told;
        for (int row = 0; row < actions.notices()->count(); ++row) {
            const QModelIndex notice = actions.notices()->index(row, 0);
            told.append(notice.data(qkvm::ui::Notices::KindRole).toString() + u": "_s +
                        notice.data(qkvm::ui::Notices::TextRole).toString());
        }
        return told;
    }

    static QVariantMap aQuestion()
    {
        return {{u"title"_s, u"Hold power button"_s},
                {u"text"_s, u"Hold power button on NAS?"_s},
                {u"warning"_s, u"A long press cuts the power. Unsaved work is lost."_s},
                {u"danger"_s, true}};
    }

    // Every line of text under an item, seen or not.
    static void lines(const QQuickItem* under, QList<const QQuickItem*>& found)
    {
        if (under->inherits("QQuickText")) {
            found.append(under);
        }
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            lines(child, found);
        }
    }

    // The lines of text in what a window shows that would take what they are given for a page where
    // it is written like one, and not for the letters it is: each as its text.
    static QStringList linesThatCouldBeAPage(const QQuickWindow* window)
    {
        QList<const QQuickItem*> all;
        lines(window->contentItem(), all);
        QStringList found;
        for (const QQuickItem* line : std::as_const(all)) {
            // (0 is Text.PlainText.)
            if (line->property("textFormat").toInt() != 0) {
                found.append(line->property("text").toString().left(60));
            }
        }
        return found;
    }

    // How many of a kind of item can be seen in what a window shows: a wall that is covered by one
    // machine shown large still has its cards, and shows none of them.
    template <typename Kind>
    static int seen(const QQuickItem* under)
    {
        if (!under->isVisible()) {
            return 0;
        }
        int found = qobject_cast<const Kind*>(under) != nullptr ? 1 : 0;
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            found += seen<Kind>(child);
        }
        return found;
    }

    // How many of a kind of item are in what a window shows.
    template <typename Kind>
    static int count(const QQuickItem* under)
    {
        int found = qobject_cast<const Kind*>(under) != nullptr ? 1 : 0;
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            found += count<Kind>(child);
        }
        return found;
    }

    // The widths the bar of a machine that is shown large is looked at in, each with a short
    // name, one of thirty letters, and one that is longer than a bar is wide.
    static void aBarOfEveryWidthWithEveryKindOfName()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("kind");
        for (const int width : {640, 800, 1280}) {
            QTest::addRow("%d wide, a short name", width) << width << u"NAS"_s << u"short"_s;
            QTest::addRow("%d wide, a name of thirty letters", width)
                << width << u"Build server 12, rack B4 upper"_s << u"thirty"_s;
            QTest::addRow("%d wide, a name longer than a bar is wide", width)
                << width << u"The build server in the second rack of the back room, upper half, left of the door"_s
                << u"long"_s;
        }
    }

    // Whether the buttons of that bar are all in the window, each as wide as it is.
    static bool theBarsButtonsKeepTheirPlace(const QQuickWindow* window)
    {
        const QString control = seenNamed(window, u"controlGiveBack"_s) ? u"controlGiveBack"_s : u"enlargedControl"_s;
        for (const QString& button : {control, u"enlargedActions"_s, u"enlargedClose"_s}) {
            const auto* item = theOne(window, button);
            if (item == nullptr || !isSeen(item) || !isInside(window, item) ||
                item->width() < item->implicitWidth() - 0.5)
                return false;
        }
        return theToolsStandAtTheRight(window);
    }

    // Whether the tools of the console's header stand packed at its right, as the page's do
    // (app.css, ".focus-tools"): the last of those in its row 6 px before More, where More is
    // there, and otherwise before Close. Says what is not so.
    static bool theToolsStandAtTheRight(const QQuickWindow* window)
    {
        const auto* row = theOne(window, u"consoleHeaderRow"_s);
        const auto* close = theOne(window, u"enlargedClose"_s);
        const auto* more = theOne(window, u"consoleMoreButton"_s);
        if (row == nullptr || close == nullptr || more == nullptr) {
            qWarning("no header, no Close or no More");
            return false;
        }
        const qreal end = (isSeen(more) ? more : close)->mapToScene(QPointF(0, 0)).x();
        qreal right = -1;
        for (const QString& name :
             {u"enlargedPrevious"_s, u"enlargedNext"_s, u"enlargedConsole"_s, u"enlargedControl"_s,
              u"controlGiveBack"_s, u"enlargedActions"_s, u"enlargedFullscreen"_s}) {
            const auto* item = theOne(window, name);
            // (Those in More are not in the row.)
            if (item != nullptr && isSeen(item) && row->isAncestorOf(item)) {
                right = std::max(right, item->mapToScene(QPointF(item->width(), 0)).x());
            }
        }
        if (right >= 0 && std::abs(end - 6 - right) > 1) {
            qWarning("the tools end at %.1f, and %.1f stands after them", right, end);
            return false;
        }
        return true;
    }

    // The console header's tools in the page's order (index.html, "focus-bar"), by their names: Control
    // is either of its two buttons, the one that asks and the pressed one.
    static QStringList theHeadersTools()
    {
        return {u"enlargedPrevious"_s, u"enlargedNext"_s,       u"control"_s,           u"enlargedActions"_s,
                u"enlargedConsole"_s,  u"enlargedFullscreen"_s, u"consoleMoreButton"_s, u"enlargedClose"_s};
    }

    static const QQuickItem* theHeadersTool(const QQuickWindow* window, const QString& name)
    {
        if (name != u"control"_s)
            return theOne(window, name);
        return seenNamed(window, u"controlGiveBack"_s) ? theOne(window, u"controlGiveBack"_s)
                                                       : theOne(window, u"enlargedControl"_s);
    }

    // Where each of the header's tools stands, as a line a tool: its left edge and its width in the
    // window where it is in the row, "in More" where it went into More, "not there" otherwise.
    static QStringList whereTheToolsStand(const QQuickWindow* window)
    {
        const auto* row = theOne(window, u"consoleHeaderRow"_s);
        QStringList said;
        for (const QString& name : theHeadersTools()) {
            const QQuickItem* item = theHeadersTool(window, name);
            if (item != nullptr && row != nullptr && !row->isAncestorOf(item)) {
                said.append(name + u": in More"_s);
            } else if (item == nullptr || row == nullptr || !isSeen(item)) {
                said.append(name + u": not there"_s);
            } else {
                said.append(u"%1: %2 wide at %3"_s.arg(name)
                                .arg(item->width(), 0, 'f', 1)
                                .arg(item->mapToScene(QPointF(0, 0)).x(), 0, 'f', 1));
            }
        }
        return said;
    }

    // Whether those of them in the row stand in the page's order from the left. Says what is not so.
    static bool theToolsAreInThePagesOrder(const QQuickWindow* window)
    {
        const auto* row = theOne(window, u"consoleHeaderRow"_s);
        qreal before = -1;
        QString was;
        for (const QString& name : theHeadersTools()) {
            const QQuickItem* item = theHeadersTool(window, name);
            if (item == nullptr || row == nullptr || !row->isAncestorOf(item) || !isSeen(item))
                continue;
            const qreal x = item->mapToScene(QPointF(0, 0)).x();
            if (x <= before) {
                qWarning("%s stands at %.1f, before %s at %.1f", qPrintable(name), x, qPrintable(was), before);
                return false;
            }
            before = x;
            was = name;
        }
        return true;
    }

    static bool revealConsoleItem(QQuickWindow* window, const QString& name)
    {
        if (const auto* item = theOne(window, name); item != nullptr && isSeen(item))
            return true;
        for (const QString& menu : {u"consoleMoreButton"_s, u"inputMoreButton"_s}) {
            const auto* button = theOne(window, menu);
            if (button == nullptr || !isSeen(button))
                continue;
            clickOn(window, button);
            const bool found = QTest::qWaitFor(
                [&] {
                    const auto* item = theOne(window, name);
                    return item != nullptr && isSeen(item) && isInside(window, item);
                },
                500);
            if (found)
                return true;
            QTest::keyClick(window, Qt::Key_Escape);
        }
        return false;
    }

    // Whether what gave way from the bar as the window got narrower is in the bar's More, and only
    // that, in the order it gave way: the groups (of a wall that has groups), Fullscreen and the
    // theme, Settings. And More at the end of the bar, before the account, and only where something
    // gave way. Says what is not so.
    static bool whatGaveWayIsInMore(QQuickWindow* window)
    {
        const auto* more = theOne(window, u"moreButton"_s);
        const auto* account = theOne(window, u"accountButton"_s);
        const auto* theme = theOne(window, u"themeButton"_s);
        if (more == nullptr || account == nullptr || theme == nullptr) {
            qWarning("no More, no account or no theme button");
            return false;
        }
        // Each, by its button and by what its row in More says (the theme as its button's hint says
        // it, with what a click does, as the page's row). The groups only where the wall has groups: a
        // wall without them has no button for them, which gave way to nothing.
        const QObject* groupsMenu = window->findChild<QObject*>(u"groupsMenu"_s);
        const bool hasGroups = groupsMenu != nullptr && groupsMenu->property("rows").toList().size() > 1;
        QList<std::pair<QString, QString>> mayGo = {{u"fullScreenButton"_s, u"Fullscreen"_s},
                                                    {u"themeButton"_s, theme->property("hint").toString()},
                                                    {u"settingsButton"_s, u"Settings"_s}};
        if (hasGroups) {
            mayGo.prepend({u"groupsButton"_s, u"Group"_s});
        }
        QStringList gone;
        for (const auto& [button, row] : mayGo) {
            const auto* seen = theOne(window, button);
            if (seen == nullptr) {
                qWarning("no %s", qPrintable(button));
                return false;
            }
            if (!isSeen(seen)) {
                gone.append(button);
            }
        }
        if (gone.isEmpty()) {
            if (isSeen(more)) {
                qWarning("More is in the bar, and nothing gave way");
                return false;
            }
            return true;
        }
        if (!isSeen(more)) {
            qWarning("%s gave way, and there is no More", qPrintable(gone.join(u", "_s)));
            return false;
        }
        if (more->mapToScene(QPointF(more->width(), 0)).x() > account->mapToScene(QPointF(0, 0)).x()) {
            qWarning("More is not before the account");
            return false;
        }
        clickOn(window, more);
        const QObject* menu = window->findChild<QObject*>(u"moreMenu"_s);
        if (menu == nullptr || !QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000)) {
            qWarning("More does not open");
            return false;
        }
        bool asItShouldBe = true;
        qreal below = -1;
        for (const auto& [button, row] : mayGo) {
            const QQuickItem* inMore = rowOf(menu, row);
            const bool gaveWay = gone.contains(button);
            if ((inMore != nullptr) != gaveWay) {
                qWarning("%s %s", qPrintable(button),
                         gaveWay ? "gave way and is not in More" : "is in the bar and in More");
                asItShouldBe = false;
            }
            if (inMore != nullptr) {
                const qreal y = inMore->mapToScene(QPointF(0, 0)).y();
                if (y < below) {
                    qWarning("%s stands in More before what gave way before it", qPrintable(button));
                    asItShouldBe = false;
                }
                below = y;
            }
        }
        if (!hasGroups && rowOf(menu, u"Group"_s) != nullptr) {
            qWarning("More has groups, and the wall has none");
            asItShouldBe = false;
        }
        QTest::keyClick(window, Qt::Key_Escape);
        static_cast<void>(QTest::qWaitFor([&] { return !menu->property("opened").toBool(); }, 2000));
        return asItShouldBe;
    }

    // After each test, when its windows are gone: what they were handed goes too (each program's
    // cleanup()).
    void forgetWhatTheWindowsWereHanded()
    {
        m_looks.clear();
        // (What the settings box was handed writes down where it was left as it goes: before the
        // settings it writes to, and before what it was given beside them.)
        m_ownSettings.clear();
        m_starts.clear();
        m_settings.clear();
        m_noActions.clear();
        m_noFiles.clear();
        m_noOwnFiles.clear();
        // (One for all the tests: the next begins with nothing taken and nothing to be taken.)
        m_control.show({});
        m_control.setReleaseKey(u"right-ctrl"_s);
    }
};

// The start of each of the three programs.
template <typename Tests>
int runScreensTests(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    // The off-screen platform knows no fonts but those in a folder it is shown: without Windows' own,
    // every letter is an empty box, and no line of text is as long as it will be.
    qputenv("QT_QPA_FONTDIR", qgetenv("SystemRoot") + "/Fonts");
    QGuiApplication app(argc, argv);
    qkvm::ui::prepareApplication();
    Tests test;
    return QTest::qExec(&test, argc, argv);
}
