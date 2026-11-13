#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include <QByteArrayView>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QPointF>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSet>
#include <QSignalSpy>
#include <QSize>
#include <QSizeF>
#include <QStringList>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>
#include <QUrl>

#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/settings.h>
#include <qkvm/engine/gpu.h>

#include "about_model.h"
#include "actions_view.h"
#include "add_kvm_view.h"
#include "application.h"
#include "control_view.h"
#include "edit_kvm_view.h"
#include "files_view.h"
#include "look.h"
#include "memory_secrets.h"
#include "own_files_view.h"
#include "own_kvms_view.h"
#include "own_wall.h"
#include "pretend_start.h"
#include "session_view.h"
#include "settings_view.h"
#include "update_by_hand.h"
#include "wall_view.h"
#include "workspaces_view.h"

// Windows' own headers, for how much memory the test takes.
#include <qt_windows.h>

#include <psapi.h>

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::MemorySecrets;

namespace {

// The times of what speaks to a KVM, made short. The KVMs of these tests are at ports of this PC's
// own address where nothing answers: whatever looks at one is refused at once, and nothing else is
// asked anywhere.
core::KvmAdding::Pace quick()
{
    core::KvmAdding::Pace pace;
    pace.probe.connectWithinMs = 1500;
    pace.probe.quietMs = 1500;
    pace.probe.withinMs = 3000;
    pace.driver.connectWithinMs = 1500;
    pace.driver.quietMs = 1500;
    pace.driver.transferMs = 1500;
    pace.driver.holdMs = 400;
    pace.driver.freshMs = 200;
    return pace;
}

// This PC's own wall is shown: nobody is signed in to anything, and whoever sits here manages it.
ui::SessionView::Shown atItsOwnWall()
{
    ui::SessionView::Shown shown;
    shown.stage = u"wall"_s;
    shown.manages = true;
    shown.encrypted = true;
    shown.ownWall = true;
    return shown;
}

// Somebody is signed in to a QKVM server, and manages it: the window shows that server's wall.
ui::SessionView::Shown atAServersWall()
{
    ui::SessionView::Shown shown;
    shown.stage = u"wall"_s;
    shown.address = u"192.168.1.10:8787"_s;
    shown.account = u"anna"_s;
    shown.manages = true;
    return shown;
}

// A KVM that is put on this PC's wall: where, what it is called, the certificate it is known by (a
// letter: its made-up SHA-256 is 32 of it; 0: by none), and the password it is given (none: empty,
// and the wall marks that none is kept for it).
struct Put {
    QString address;
    QString name;
    char known = 0;
    QString password = QStringLiteral("secret");
    QString kind = QStringLiteral("kvmd");
};

// One window of the program, as the program makes it, with everything the program hands it; and
// this PC's own wall behind it, with its boxes served as the program serves them. What is
// remembered and what is kept secret are in a folder of the test's own and in memory.
struct Desk {
    explicit Desk(core::UpdateChannel channel = {{}, {}, QStringLiteral("0.1.0")})
        : settings(folder.filePath(u"settings.json"_s)),
          look(settings, [this](Qt::ColorScheme scheme) { lookTold = scheme; }), ownSettings(start, settings),
          updates(std::move(channel), settings, updateNetwork, folder.filePath(u"updates"_s)), updateView(updates)
    {
        ownSettings.useUpdates(updateView);
        // A verified file is "shown" here by being noted: no test opens a window of Windows'.
        updateView.showWith([this](const QString& file) {
            shownFiles.append(file);
            return true;
        });
        session.show(atItsOwnWall());
        actions.setAllowed(true);
    }

    // Puts KVMs on the wall before the program starts, each as one that was added by hand, with
    // its password (or none); and gives the wall's names for them, in their order.
    QStringList put(const QList<Put>& those)
    {
        core::LocalWall before(settings, secrets, quick().driver);
        QStringList ids;
        for (const Put& one : those) {
            core::OwnKvm kvm;
            kvm.address = one.address;
            kvm.name = one.name;
            kvm.knownByCertificate = one.known != 0;
            kvm.certificate = one.known != 0 ? QByteArray(32, one.known) : QByteArray();
            kvm.kind = one.kind;
            const base::Result<QString> id = before.add(kvm, one.password);
            ids.append(id ? id.value() : QString());
        }
        return ids;
    }
    // Puts a KVM on the wall before the program starts as one that was taken from a file: not
    // asked yet what it is, with the password the file had (none: empty). Its name, or nothing.
    QString putFromAFile(const QString& address, const QString& name, const QString& password = {})
    {
        core::LocalWall before(settings, secrets, quick().driver);
        core::LocalWall::FromAFile one;
        one.kvm.address = address;
        one.kvm.name = name;
        one.password = password;
        const QList<base::Result<QString>> came = before.takeFromAFile({one});
        return came.size() == 1 && came.constFirst() ? came.constFirst().value() : QString();
    }

    // The window, loaded as the program loads it and brought up; null where it did not come up.
    QQuickWindow* load(int width = 0, int height = 0)
    {
        own = std::make_unique<ui::OwnWall>(settings, secrets, quick());
        if (security != nullptr) {
            own->useAppLock(*security);
            savedActions = std::make_unique<ui::OwnActionsView>(own->wall().actions(), *security);
            ownSettings.useSecurity(*security, *savedActions);
        }
        kvms.use(&own->wall());
        own->serve(adding);
        own->serve(editing);
        own->serve(ownFiles);
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError>& warnings) {
            for (const QQmlError& warning : warnings) {
                complaints.append(warning.toString());
            }
        });
        ui::keepOffTheNetwork(engine);
        engine.setInitialProperties({{u"about"_s, QVariant::fromValue(&about)},
                                     {u"session"_s, QVariant::fromValue(&session)},
                                     {u"wall"_s, QVariant::fromValue(&wall)},
                                     {u"actions"_s, QVariant::fromValue(&actions)},
                                     {u"look"_s, QVariant::fromValue(&look)},
                                     {u"settings"_s, QVariant::fromValue(&ownSettings)},
                                     {u"files"_s, QVariant::fromValue(&files)},
                                     {u"ownFiles"_s, QVariant::fromValue(&ownFiles)},
                                     {u"adding"_s, QVariant::fromValue(&adding)},
                                     {u"editing"_s, QVariant::fromValue(&editing)},
                                     {u"control"_s, QVariant::fromValue(&control)},
                                     {u"kvms"_s, QVariant::fromValue(&kvms)}});
        engine.loadFromModule("Qkvm", "Main");
        window =
            engine.rootObjects().size() == 1 ? qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst()) : nullptr;
        if (window == nullptr) {
            return nullptr;
        }
        if (width > 0) {
            window->resize(width, height);
        }
        window->show();
        window->requestActivate();
        return QTest::qWaitForWindowActive(window) ? window : nullptr;
    }

    QObject* box(const QString& name) const { return window->findChild<QObject*>(name); }
    bool isOpen(const QString& name) const { return box(name) != nullptr && box(name)->property("opened").toBool(); }

    QTemporaryDir folder;
    core::Settings settings;
    MemorySecrets secrets;
    std::unique_ptr<ui::AppLock> security;
    std::unique_ptr<core::Incidents> incidents;
    std::unique_ptr<ui::AlertsView> alerts;
    // This PC's own wall, made when the window is.
    std::unique_ptr<ui::OwnWall> own;
    std::unique_ptr<ui::OwnActionsView> savedActions;
    ui::AboutModel about;
    ui::SessionView session;
    ui::WallView wall;
    ui::ActionsView actions;
    // How the program's look tells what it is, here: it is noted, and the screens are told it
    // (lookIs), as Qt tells them where it has a screen. Qt's platform without one takes no telling.
    Qt::ColorScheme lookTold = Qt::ColorScheme::Unknown;
    ui::Look look;
    ui::PretendedStart start;
    ui::SettingsView ownSettings;
    core::testing::UpdateNetwork updateNetwork;
    core::Updates updates;
    ui::UpdatesView updateView;
    QStringList shownFiles;
    ui::FilesView files;
    ui::AddKvmView adding;
    ui::EditKvmView editing;
    ui::ControlView control;
    ui::OwnFilesView ownFiles;
    ui::OwnKvmsView kvms;
    QStringList complaints;
    // (Last, and so the first to go: the window, before anything it was handed.)
    QQmlApplicationEngine engine;
    QQuickWindow* window = nullptr;
};

// Every item of this name in what a window shows.
void named(const QQuickItem* under, const QString& name, QList<const QQuickItem*>& found)
{
    if (under->objectName() == name) {
        found.append(under);
    }
    const QList<QQuickItem*> children = under->childItems();
    for (const QQuickItem* child : children) {
        named(child, name, found);
    }
}

// Whether an item can be seen: it, and everything it is in.
bool isSeen(const QQuickItem* item)
{
    if (item == nullptr) {
        return false;
    }
    for (const QQuickItem* up = item; up != nullptr; up = up->parentItem()) {
        if (!up->isVisible()) {
            return false;
        }
    }
    return true;
}

// The items of this name that can be seen, in the order they are found in.
QList<const QQuickItem*> seenOnes(const QQuickItem* under, const QString& name)
{
    QList<const QQuickItem*> found;
    named(under, name, found);
    found.removeIf([](const QQuickItem* item) { return !isSeen(item); });
    return found;
}
QList<const QQuickItem*> seenOnes(const QQuickWindow* window, const QString& name)
{
    return seenOnes(window->contentItem(), name);
}

// The one item of this name that can be seen; null where none or several can.
const QQuickItem* theOne(const QQuickWindow* window, const QString& name)
{
    const QList<const QQuickItem*> found = seenOnes(window, name);
    return found.size() == 1 ? found.constFirst() : nullptr;
}

// The same, for something a test does to it (puts the keyboard on it, scrolls it).
void toWorkOn(QQuickItem* under, const QString& name, QList<QQuickItem*>& found)
{
    if (under == nullptr || !under->isVisible()) {
        return;
    }
    if (under->objectName() == name) {
        found.append(under);
    }
    const QList<QQuickItem*> children = under->childItems();
    for (QQuickItem* child : children) {
        toWorkOn(child, name, found);
    }
}
QQuickItem* theOneToWorkOn(const QQuickWindow* window, const QString& name)
{
    QList<QQuickItem*> found;
    toWorkOn(window->contentItem(), name, found);
    return found.size() == 1 ? found.constFirst() : nullptr;
}

// The line of text that says exactly this and can be seen, under an item.
const QQuickItem* saying(const QQuickItem* under, const QString& text)
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
bool says(const QQuickWindow* window, const QString& text)
{
    return saying(window->contentItem(), text) != nullptr;
}

// A frame is drawn: what was opened or changed a moment ago is laid out with it, and not before.
void aFrame(QQuickWindow* window)
{
    QSignalSpy drawn(window, &QQuickWindow::frameSwapped);
    window->update();
    static_cast<void>(drawn.wait(2000));
}

QRectF placeOf(const QQuickItem* item)
{
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

// A click on an item, where it is once the window has drawn it: as a person clicks, on what is seen.
void clickOn(QQuickWindow* window, const QQuickItem* item)
{
    aFrame(window);
    QTest::mouseClick(window, Qt::LeftButton, {}, placeOf(item).center().toPoint());
}

// For whoever wants to look at the box without starting the program: with QKVM_SCREENSHOTS set to a
// folder, each state is kept there as a picture of the test's own window, as Qt Quick draws it in
// software.
void keepPictureOf(QQuickWindow* window, const QString& name)
{
    const QString folder = qEnvironmentVariable("QKVM_SCREENSHOTS");
    if (folder.isEmpty()) {
        return;
    }
    aFrame(window);
    QVERIFY(QDir().mkpath(folder));
    QVERIFY(window->grabWindow().save(QDir(folder).filePath(name + u".png"_s)));
}

// The screens are drawn in this look: the program is told it, and the screens are told what the
// program told Qt, as Qt tells them where it has a screen.
bool lookIs(Desk& desk, const QString& theme)
{
    desk.look.choose(theme);
    QObject* colours = desk.engine.singletonInstance<QObject*>("Qkvm", "Theme");
    if (colours == nullptr || desk.look.theme() != theme) {
        return false;
    }
    static_cast<void>(colours->setProperty("scheme", static_cast<int>(desk.lookTold)));
    const bool dark = theme != u"light"_s;
    return QTest::qWaitFor([colours, dark] { return colours->property("dark").toBool() == dark; }, 1000);
}

// --- the settings box ------------------------------------------------------------------------------

// The section the box shows; null where it shows none.
const QQuickItem* shownSection(const Desk& desk)
{
    const QObject* shown = desk.box(u"settingsShown"_s);
    return shown != nullptr ? shown->property("item").value<QQuickItem*>() : nullptr;
}
QString shownName(const Desk& desk)
{
    const QQuickItem* section = shownSection(desk);
    return section != nullptr ? section->objectName() : QString();
}

// The box's list of sections: the keys of its entries in their order, and the entry of one.
QStringList sectionKeys(const Desk& desk)
{
    QStringList keys;
    for (const QQuickItem* entry : seenOnes(desk.window, u"settingsSection"_s)) {
        keys.append(entry->property("key").toString());
    }
    return keys;
}
const QQuickItem* entryOf(const Desk& desk, const QString& key)
{
    for (const QQuickItem* entry : seenOnes(desk.window, u"settingsSection"_s)) {
        if (entry->property("key").toString() == key) {
            return entry;
        }
    }
    return nullptr;
}

// The settings are opened, as with the window's own key for them; true once the box is open.
bool openSettings(Desk& desk)
{
    QTest::keyClick(desk.window, Qt::Key_Comma, Qt::ControlModifier);
    if (!QTest::qWaitFor([&] { return desk.isOpen(u"settingsBox"_s) && shownSection(desk) != nullptr; }, 20000)) {
        return false;
    }
    aFrame(desk.window);
    return true;
}

// Scroll an off-screen tab into view, as with the wheel.
bool revealSection(Desk& desk, const QString& key)
{
    const QQuickItem* entry = entryOf(desk, key);
    if (entry == nullptr) {
        return false;
    }
    QQuickItem* scroll = entry->parentItem();
    while (scroll != nullptr && !scroll->property("contentX").isValid()) {
        scroll = scroll->parentItem();
    }
    if (scroll != nullptr) {
        const QRectF tab = entry->mapRectToItem(scroll, QRectF(0, 0, entry->width(), entry->height()));
        const auto reveal = [scroll](const char* position, const char* content, qreal first, qreal last, qreal room) {
            qreal at = scroll->property(position).toReal();
            at += first < 0 ? first - 10 : last > room ? last - room + 10 : 0;
            const qreal most = std::max(0.0, scroll->property(content).toReal() - room);
            scroll->setProperty(position, std::clamp(at, 0.0, most));
        };
        reveal("contentX", "contentWidth", tab.left(), tab.right(), scroll->width());
        reveal("contentY", "contentHeight", tab.top(), tab.bottom(), scroll->height());
        aFrame(desk.window);
    }
    return true;
}

bool choose(Desk& desk, const QString& key, const QString& section)
{
    if (!revealSection(desk, key)) {
        return false;
    }
    clickOn(desk.window, entryOf(desk, key));
    const bool shown = QTest::qWaitFor([&] { return shownName(desk) == section; }, 20000);
    aFrame(desk.window);
    return shown;
}

// The rows of the KVMs' list that can be seen, in their order, and the one of a KVM.
QList<const QQuickItem*> kvmRows(const Desk& desk)
{
    return seenOnes(desk.window, u"kvmRow"_s);
}
const QQuickItem* rowOfKvm(const Desk& desk, const QString& id)
{
    for (const QQuickItem* row : kvmRows(desk)) {
        if (row->property("key").toString() == id) {
            return row;
        }
    }
    return nullptr;
}
const QQuickItem* inRow(const QQuickItem* row, const QString& name)
{
    if (row == nullptr) {
        return nullptr;
    }
    const QList<const QQuickItem*> found = seenOnes(row, name);
    return found.size() == 1 ? found.constFirst() : nullptr;
}
// The same of a row's control, for something a test does to it (puts the keyboard on it).
QQuickItem* inRowToWorkOn(const Desk& desk, const QString& id, const QString& name)
{
    QList<QQuickItem*> rows;
    toWorkOn(desk.window->contentItem(), u"kvmRow"_s, rows);
    for (QQuickItem* row : std::as_const(rows)) {
        if (row->property("key").toString() == id) {
            QList<QQuickItem*> found;
            toWorkOn(row, name, found);
            return found.size() == 1 ? found.constFirst() : nullptr;
        }
    }
    return nullptr;
}
bool rowSays(const QQuickItem* row, const QString& text)
{
    return saying(row, text) != nullptr;
}
// Whether one of the lines of text of a row that can be seen says this, as a line of its own.
bool rowHasLine(const QQuickItem* row, const QString& line)
{
    if (row == nullptr || !row->isVisible()) {
        return false;
    }
    if (row->inherits("QQuickText") && row->property("text").toString().split(u'\n').contains(line)) {
        return true;
    }
    const QList<QQuickItem*> children = row->childItems();
    return std::ranges::any_of(children, [&line](const QQuickItem* child) { return rowHasLine(child, line); });
}

// What a person who points at something in the box a moment before a change finds there after it.
// Each control that takes a press, by what it is for a person: its name, and in a row the KVM of
// that row, in the list of sections the section it shows. Where it can be seen in the window (what
// cuts off what is scrolled away cuts it off here too); whether it takes a press now; whether it is
// a control of the section shown (and not of the box: its list, its way out).
struct Seen {
    QRectF box;
    QHash<QString, QRectF> controls;
    QStringList live;
    QStringList ofTheSection;
};

QString whatControl(const QQuickItem* control)
{
    QString what = control->objectName().isEmpty() ? QString::fromLatin1(control->metaObject()->className())
                                                   : control->objectName();
    if (control->objectName() == u"settingsSection"_s) {
        return what + u" "_s + control->property("key").toString();
    }
    for (const QQuickItem* up = control->parentItem(); up != nullptr; up = up->parentItem()) {
        if (up->objectName() == u"kvmRow"_s) {
            return what + u" of "_s + up->property("key").toString();
        }
    }
    return what;
}
bool isInTheSection(const QQuickItem* item)
{
    for (const QQuickItem* up = item; up != nullptr; up = up->parentItem()) {
        if (up->objectName() == u"settingsShown"_s) {
            return true;
        }
    }
    return false;
}
QRectF seenPlaceOf(const QQuickItem* item)
{
    QRectF where = placeOf(item);
    for (const QQuickItem* up = item->parentItem(); up != nullptr; up = up->parentItem()) {
        if (up->clip()) {
            where = where.intersected(placeOf(up));
        }
    }
    return where;
}
// (`lines`: also the lines that are typed into; without, the buttons alone.)
void controlsIn(const QQuickItem* under, Seen& seen, bool lines = true)
{
    if (under == nullptr || !under->isVisible()) {
        return;
    }
    if (under->inherits("QQuickAbstractButton") || (lines && under->inherits("QQuickTextInput"))) {
        const QRectF where = seenPlaceOf(under);
        if (where.width() > 1 && where.height() > 1) {
            QString what = whatControl(under);
            for (int more = 2; seen.controls.contains(what); ++more) {
                what = whatControl(under) + u" ("_s + QString::number(more) + u")"_s;
            }
            seen.controls.insert(what, where);
            if (under->isEnabled()) {
                seen.live.append(what);
            }
            if (isInTheSection(under)) {
                seen.ofTheSection.append(what);
            }
        }
    }
    const QList<QQuickItem*> children = under->childItems();
    for (const QQuickItem* child : children) {
        controlsIn(child, seen, lines);
    }
}
// Of the settings box; or of another box, by its name (one that a section opened over it).
Seen seenOf(Desk& desk, const QString& name = u"settingsBox"_s, bool lines = true)
{
    aFrame(desk.window);
    Seen seen;
    const QObject* box = desk.box(name);
    seen.box = QRectF(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                      box->property("height").toReal());
    for (const char* part : {"header", "contentItem"}) {
        controlsIn(box->property(part).value<QQuickItem*>(), seen, lines);
    }
    return seen;
}

// What a change may move besides: nothing; or the section's own controls, where the change is
// another section, which the list of sections was pressed for (they are that section's). (Nothing
// in a section grows: a row is of one height whatever it says, and a line that says more says it
// where nothing stands under it.)
enum class Allowed { Nothing, AnotherSection };

// What is wrong with what stands where after a change: the box is where it was and as large; no
// control that takes a press stands where another stood; and every control that was there and is
// there still stands where it stood, but for what the change may move (above).
QStringList movedBy(const Seen& before, const Seen& after, const QString& when, Allowed allowed = Allowed::Nothing)
{
    const bool anotherSection = allowed == Allowed::AnotherSection;
    QStringList wrong;
    if (before.box != after.box) {
        wrong.append(u"%1: the box was at %2,%3 and %4 by %5, and is at %6,%7 and %8 by %9"_s.arg(when)
                         .arg(before.box.x())
                         .arg(before.box.y())
                         .arg(before.box.width())
                         .arg(before.box.height())
                         .arg(after.box.x())
                         .arg(after.box.y())
                         .arg(after.box.width())
                         .arg(after.box.height()));
    }
    const auto isTheSections = [&](const QString& what, const Seen& seen) {
        return anotherSection && seen.ofTheSection.contains(what);
    };
    for (const QString& now : after.live) {
        if (isTheSections(now, after)) {
            continue;
        }
        const QRectF is = after.controls.value(now);
        for (auto was = before.controls.cbegin(); was != before.controls.cend(); ++was) {
            const QRectF both = is.intersected(was.value());
            if (was.key() != now && both.width() > 1 && both.height() > 1) {
                wrong.append(u"%1: \"%2\", which takes a press, stands where \"%3\" stood"_s.arg(when, now, was.key()));
            }
        }
    }
    for (auto was = before.controls.cbegin(); was != before.controls.cend(); ++was) {
        if (isTheSections(was.key(), before)) {
            continue;
        }
        // (A control that is there only sometimes is there always, and takes no press while it has
        // nothing to do: none goes from where it stood. One that was in sight still is.)
        if (!after.controls.contains(was.key())) {
            wrong.append(u"%1: \"%2\" was there, and is gone"_s.arg(when, was.key()));
            continue;
        }
        const QRectF is = after.controls.value(was.key());
        if (qAbs(is.x() - was.value().x()) > 0.5 || qAbs(is.y() - was.value().y()) > 0.5 ||
            qAbs(is.width() - was.value().width()) > 0.5 || qAbs(is.height() - was.value().height()) > 0.5) {
            wrong.append(u"%1: \"%2\" was at %3,%4 and is at %5,%6"_s.arg(when, was.key())
                             .arg(was.value().x())
                             .arg(was.value().y())
                             .arg(is.x())
                             .arg(is.y()));
        }
    }
    return wrong;
}

// The lines of text each shape is made with, by their names: a text stands in a section only as one
// of these, of the shape it stands in (or is a shape itself, as what explains a section is).
const QHash<QString, QStringList>& textsOfTheShapes()
{
    static const QHash<QString, QStringList> texts{
        {u"line"_s, {u"lineLabel"_s, u"lineHint"_s, u"lineValue"_s}},
        {u"block"_s, {u"blockTitle"_s, u"blockText"_s}},
        {u"listed"_s, {u"listedTitle"_s, u"listedColumn"_s, u"listedNothing"_s}},
        {u"row"_s, {u"rowText"_s, u"rowUnder"_s}},
        {u"footnote"_s, {u"footnoteText"_s}},
    };
    return texts;
}

// Whether an item takes a press: a control, a line that is typed into, a mouse area, what scrolls,
// what a handler of presses is set on, or what the keyboard's Tab comes to. (A line of text takes
// a press only for a link in it, and one that is told it is letters has none. An item with a
// handler is made to take every button for its handlers' sake: a handler that only follows the
// pointer, as a row's does for its tooltip, takes no press.)
bool takesAPress(const QQuickItem* item)
{
    if (item->inherits("QQuickControl") || item->inherits("QQuickAbstractButton") ||
        item->inherits("QQuickTextInput") || item->inherits("QQuickTextEdit") || item->inherits("QQuickMouseArea") ||
        item->inherits("QQuickFlickable") || item->activeFocusOnTab()) {
        return true;
    }
    const QObjectList& children = item->children();
    const bool handled =
        std::ranges::any_of(children, [](const QObject* child) { return child->inherits("QQuickPointerHandler"); });
    if (handled) {
        return std::ranges::any_of(children, [](const QObject* child) {
            return child->inherits("QQuickPointerHandler") && !child->inherits("QQuickHoverHandler");
        });
    }
    return !item->inherits("QQuickText") && item->acceptedMouseButtons() != Qt::NoButton;
}

// The program's own controls, of one shape (Button, Check, Choice), known by their QML type as the
// engine that made a section knows it: by the class each of them is made of there, which QML makes
// for that type and no item comes to be of by saying something of itself. With the files the parts
// of each are written in (its own file, and those of what it is made of): a part of such a control
// that is written anywhere else was put into it by whoever placed it, and is none of the control's.
struct SharedControls {
    QHash<QByteArray, QSet<QString>> filesOf;

    // The type of these an item is of (its class, or one it is made from); empty: none.
    QByteArray typeOf(const QQuickItem* item) const
    {
        for (const QMetaObject* made = item->metaObject(); made != nullptr; made = made->superClass()) {
            if (filesOf.contains(made->className())) {
                return made->className();
            }
        }
        return {};
    }
};

// The file a part is written in, as its engine has it; empty: none (it was made in C++).
QString fileOf(const QQuickItem* part)
{
    const QQmlContext* context = qmlContext(part);
    return context != nullptr ? context->baseUrl().toString() : QString();
}

// Each of them, as one of it is made in that engine, beside the item it is asked for (in a window,
// as the controls stand; not seen, and gone again at once).
SharedControls sharedControlsBeside(const QQuickItem* item)
{
    SharedControls shared;
    QQmlEngine* engine = qmlEngine(item);
    QQuickItem* inWindow = item->window() != nullptr ? item->window()->contentItem() : nullptr;
    if (engine == nullptr || inWindow == nullptr) {
        return shared;
    }
    for (const char* type : {"Button", "Check", "Choice"}) {
        QQmlComponent made(engine);
        made.loadFromModule("Qkvm", type);
        std::unique_ptr<QQuickItem> one(qobject_cast<QQuickItem*>(made.beginCreate(engine->rootContext())));
        if (one == nullptr) {
            continue;
        }
        one->setVisible(false);
        one->setParentItem(inWindow);
        made.completeCreate();
        QSet<QString> files;
        const std::function<void(const QQuickItem*)> partsOf = [&files, &partsOf](const QQuickItem* under) {
            for (const QQuickItem* part : under->childItems()) {
                if (const QString file = fileOf(part); !file.isEmpty()) {
                    files.insert(file);
                }
                partsOf(part);
            }
        };
        partsOf(one.get());
        shared.filesOf.insert(one->metaObject()->className(), files);
    }
    return shared;
}

// What is put into one of the program's own controls from outside it and takes a press, said. (Its
// own parts are written in its own files; whatever else is in it was put there by whoever placed it.)
void putIntoAControl(const QQuickItem* under, const QSet<QString>& itsOwn, const QString& control, const QString& where,
                     QStringList& wrong)
{
    for (const QQuickItem* part : under->childItems()) {
        const QString file = fileOf(part);
        if (!file.isEmpty() && !itsOwn.contains(file) && takesAPress(part)) {
            wrong.append(where + u": a "_s + QString::fromLatin1(part->metaObject()->className()) +
                         u" that takes a press was put into \""_s + control + u"\", and is none of its own"_s);
        }
        putIntoAControl(part, itsOwn, control, where, wrong);
    }
}

// What of a section is laid out otherwise than of the shapes every section is made of. What a
// section shows (all of it but its first line) is a column of those shapes: what explains it before
// its first block (intro), blocks (block), lists (listed), what is said after the last (footnote),
// or a group of those that two places show alike (blocks). Held strictly: a block's frame holds
// lines and nothing else, and a line stands in a frame and nowhere else, the whole width of it; a
// list's frame holds rows (and its own head and its line for nothing), and a row stands in one, the
// whole width of it; what a line or a row does, at its right, holds the program's own controls and
// nothing else (and a line's what is so), and such a control stands there and nowhere else; nothing
// else takes a press, but the part of a line that clicks its tick, and nothing that takes a press is
// put into such a control; and a text is one of the texts of the shape it stands in. Every item is
// walked, whether it is seen just then or not: what is hidden now is shown at another moment. Each
// shape says which it is (its `shape`), and each of its parts that a rule names is named so
// (objectName); that is what is walked by here. The program's own controls are known by their type
// (SharedControls), and by nothing an item says of itself.
void walk(const QQuickItem* item, const SharedControls& shared, const QStringList& within, const QString& where,
          QStringList& wrong)
{
    if (item == nullptr) {
        return;
    }
    const QString shape = item->property("shape").toString();
    const QQuickItem* up = item->parentItem();
    const QString upIs = up != nullptr ? up->objectName() : QString();
    const QString what = QString::fromLatin1(item->metaObject()->className()) +
                         (item->objectName().isEmpty() ? QString() : u" \""_s + item->objectName() + u"\""_s);
    QStringList inside = within;
    if (!shape.isEmpty()) {
        if (shape == u"line"_s && upIs != u"blockFrame"_s) {
            wrong.append(where + u": a line that stands in no block's frame itself"_s);
        }
        // (A list makes the rows that are in sight in a place of its own, inside its frame, as high
        // as all its rows: each row stands there where it stands among all of them.)
        if (shape == u"row"_s && upIs != u"listedRows"_s) {
            wrong.append(where + u": a row that stands in no list's place for its rows"_s);
        }
        if ((shape == u"line"_s || shape == u"row"_s) && up != nullptr &&
            (qAbs(item->x()) > 0.5 || qAbs(item->width() - up->width()) > 0.5)) {
            wrong.append(where + u": a "_s + shape + u" that stands elsewhere than the whole width of its frame"_s);
        }
        inside.append(shape);
    }
    if (upIs == u"blockFrame"_s && shape != u"line"_s) {
        wrong.append(where + u": a "_s + what + u" stands in a block's frame, which holds lines alone"_s);
    }
    // (A list's own repeater makes its rows there, which are walked as all else.)
    if (upIs == u"listedFrame"_s && shape != u"row"_s && item->objectName() != u"listedHead"_s &&
        item->objectName() != u"listedNothing"_s && item->objectName() != u"listedRows"_s) {
        wrong.append(where + u": a "_s + what + u" stands in a list's frame, which holds rows alone"_s);
    }
    const bool inWhatIsDone = upIs == u"lineDoes"_s || upIs == u"rowDoes"_s;
    const QByteArray type = shared.typeOf(item);
    if (inWhatIsDone && type.isEmpty() && item->objectName() != u"lineValue"_s) {
        wrong.append(where + u": a "_s + what +
                     u" stands in what a line or a row does, which holds the program's own controls alone"_s);
    }
    // (What a control is made of is the control's own: what was put into it is not.)
    if (!type.isEmpty()) {
        if (!inWhatIsDone) {
            wrong.append(where + u": \""_s + whatControl(item) +
                         u"\" stands elsewhere than in what a line or a row does"_s);
        }
        putIntoAControl(item, shared.filesOf.value(type), whatControl(item), where, wrong);
        return;
    }
    const bool clicksTheTick =
        item->objectName() == u"lineLeft"_s && up != nullptr && up->property("shape").toString() == u"line"_s;
    if (takesAPress(item) && !clicksTheTick) {
        wrong.append(where + u": a "_s + what + u" takes a press, and is none of the program's own controls"_s);
        return;
    }
    if (item->inherits("QQuickText") && shape.isEmpty() &&
        (inside.isEmpty() || !textsOfTheShapes().value(inside.last()).contains(item->objectName()))) {
        wrong.append(where + u": a text that is none of the shapes' own: \""_s + item->property("text").toString() +
                     u"\""_s);
    }
    const QList<QQuickItem*> children = item->childItems();
    for (const QQuickItem* child : children) {
        walk(child, shared, inside, where, wrong);
    }
}
// How many of one shape can be seen under an item.
int shapesOf(const QQuickItem* under, const QString& shape)
{
    if (under == nullptr || !under->isVisible()) {
        return 0;
    }
    int found = under->property("shape").toString() == shape ? 1 : 0;
    const QList<QQuickItem*> children = under->childItems();
    for (const QQuickItem* child : children) {
        found += shapesOf(child, shape);
    }
    return found;
}

// The keyboard is put on a control of the section shown, as Tab puts it there: the section brings it
// into sight. True once it is whole in what the section shows.
bool bringIntoSight(Desk& desk, const QString& name)
{
    QQuickItem* item = theOneToWorkOn(desk.window, name);
    if (item == nullptr) {
        return false;
    }
    item->forceActiveFocus(Qt::TabFocusReason);
    aFrame(desk.window);
    const QQuickItem* content = theOne(desk.window, u"sectionContent"_s);
    return content != nullptr && seenPlaceOf(content).contains(placeOf(item));
}

QStringList laidOutOtherwise(const QQuickItem* section)
{
    QStringList wrong;
    const QString where = section->objectName();
    if (section->property("shape").toString() != u"section"_s) {
        return {where + u": not a section"_s};
    }
    const auto* flickable = section->property("flickable").value<QQuickItem*>();
    const auto* content = flickable != nullptr ? flickable->property("contentItem").value<QQuickItem*>() : nullptr;
    if (content == nullptr) {
        return {where + u": no content"_s};
    }
    const SharedControls shared = sharedControlsBeside(section);
    if (shared.filesOf.size() != 3) {
        return {where + u": the program's own controls could not be made to be known by"_s};
    }
    for (const QQuickItem* part : content->childItems()) {
        // (What the section says while it asks, after the last of it, is the section's own.)
        if (part->objectName() == u"sectionSaid"_s) {
            continue;
        }
        QList<const QQuickItem*> tops;
        for (const QQuickItem* top : part->childItems()) {
            tops.append(top);
        }
        // Its column: each of what stands in it is one of the shapes, or a group of them. (Seen just
        // then or not.)
        for (qsizetype at = 0; at < tops.size(); ++at) {
            const QQuickItem* top = tops.at(at);
            const QString shape = top->property("shape").toString();
            if (shape == u"blocks"_s) {
                for (const QQuickItem* inGroup : top->childItems()) {
                    tops.append(inGroup);
                }
                continue;
            }
            const QStringList blockShapes{u"intro"_s, u"block"_s, u"listed"_s, u"footnote"_s};
            if (!blockShapes.contains(shape)) {
                wrong.append(where + u": a "_s + QString::fromLatin1(top->metaObject()->className()) +
                             u" stands in the section, laid out by hand"_s);
                continue;
            }
            walk(top, shared, {}, where, wrong);
        }
    }
    return wrong;
}

// Where a row of the KVMs' list that is made stands elsewhere than its place among all of them (the
// list makes only those that are in sight, and a few beyond: each is where it would stand with every
// row made, one row's height under the one before it), said.
QStringList rowsInTheirPlaces(const Desk& desk, const QString& when)
{
    QStringList wrong;
    const QQuickItem* list = theOne(desk.window, u"kvmList"_s);
    const QQuickItem* all = theOne(desk.window, u"listedRows"_s);
    if (list == nullptr || all == nullptr) {
        return {when + u": no list"_s};
    }
    const qreal high = list->property("rowHeight").toReal();
    const int keyRole = desk.kvms.roleNames().key("key");
    QHash<QString, int> numbers;
    for (int at = 0; at < desk.kvms.rowCount(); ++at) {
        numbers.insert(desk.kvms.index(at, 0).data(keyRole).toString(), at);
    }
    for (const QQuickItem* row : kvmRows(desk)) {
        const QString key = row->property("key").toString();
        const qreal should = placeOf(all).top() + numbers.value(key, -1) * high;
        if (!numbers.contains(key) || qAbs(placeOf(row).top() - should) > 0.5) {
            wrong.append(u"%1: the row of %2 is at %3, and its place at %4"_s.arg(when, key)
                             .arg(placeOf(row).top())
                             .arg(should));
        }
    }
    return wrong;
}

// Where the rows of the KVMs' list that can be seen are not all of one height, or not in their
// places, said.
QStringList rowsOfOneHeight(const Desk& desk, const QString& when)
{
    const QList<const QQuickItem*> rows = kvmRows(desk);
    QStringList wrong = rowsInTheirPlaces(desk, when);
    for (const QQuickItem* row : rows) {
        if (qAbs(row->height() - rows.constFirst()->height()) > 0.5) {
            wrong.append(u"%1: the row of %2 is %3 high, and the first %4"_s.arg(when, row->property("key").toString())
                             .arg(row->height())
                             .arg(rows.constFirst()->height()));
        }
    }
    return wrong;
}

// What has the keyboard in the window.
const QQuickItem* keyboardAt(const Desk& desk)
{
    return desk.window->activeFocusItem();
}

// A hundred and twenty KVMs, at ports from this one on where nothing answers.
QList<Put> oneHundredAndTwenty(int firstPort)
{
    QList<Put> many;
    for (int number = 0; number < 120; ++number) {
        many.append({u"http://127.0.0.1:%1"_s.arg(firstPort + number), u"Rack %1"_s.arg(number + 1)});
    }
    return many;
}

// The keyboard at a row's Edit, as Tab brings it there; and the list scrolled 6000 pixels away from it,
// as with the wheel: past the rows the list makes around what is in sight. True once that row is made
// all the same, far out of sight, with the keyboard in it, and the rows around it are not.
bool keptFarFromSight(Desk& desk, const QString& id)
{
    if (!QTest::qWaitFor([&] { return inRowToWorkOn(desk, id, u"kvmEdit"_s) != nullptr; }, 20000)) {
        return false;
    }
    QQuickItem* edit = inRowToWorkOn(desk, id, u"kvmEdit"_s);
    edit->forceActiveFocus(Qt::TabFocusReason);
    QQuickItem* content = theOneToWorkOn(desk.window, u"sectionContent"_s);
    if (content == nullptr || keyboardAt(desk) != edit ||
        content->property("contentHeight").toReal() - content->height() < 6000) {
        return false;
    }
    for (qreal at = 0; at <= 6000; at += 600) {
        content->setProperty("contentY", at);
        aFrame(desk.window);
    }
    QTest::qWait(50);
    aFrame(desk.window);
    return rowOfKvm(desk, id) != nullptr && kvmRows(desk).size() < 60 && keyboardAt(desk) == edit;
}

// The buttons that stand in what scrolls in the box that changes a KVM, with its lines (the eye of
// the password's line), by what each is for a person.
QHash<QString, const QQuickItem*> buttonsInTheEditBoxsLines(const Desk& desk)
{
    QHash<QString, const QQuickItem*> buttons;
    QList<const QQuickItem*> found;
    named(desk.window->contentItem(), u"editLines"_s, found);
    const std::function<void(const QQuickItem*)> under = [&buttons, &under](const QQuickItem* item) {
        for (const QQuickItem* part : item->childItems()) {
            if (part->inherits("QQuickAbstractButton")) {
                buttons.insert(whatControl(part), part);
            }
            under(part);
        }
    };
    if (found.size() == 1) {
        under(found.constFirst());
    }
    return buttons;
}

// Where each line to type into of the box that changes a KVM is placed, whole, by its name, and each
// button among them: wherever what holds them cuts them off.
QHash<QString, QRectF> linesOfTheEditBox(const Desk& desk)
{
    QHash<QString, QRectF> lines;
    for (const QString& name : {u"editName"_s, u"editAddress"_s, u"editUser"_s, u"editPassword"_s, u"editGroup"_s}) {
        QList<const QQuickItem*> found;
        named(desk.window->contentItem(), name, found);
        if (found.size() == 1) {
            lines.insert(name, placeOf(found.constFirst()));
        }
    }
    const QHash<QString, const QQuickItem*> buttons = buttonsInTheEditBoxsLines(desk);
    for (auto button = buttons.cbegin(); button != buttons.cend(); ++button) {
        lines.insert(button.key(), placeOf(button.value()));
    }
    return lines;
}

// What a person who points at something in the box that changes a KVM finds there, but what stands
// in its lines, which scroll (linesOfTheEditBox is where those are): its buttons, each where it can
// be seen.
Seen buttonsOfTheEditBox(Desk& desk)
{
    Seen seen = seenOf(desk, u"editKvm"_s, false);
    const QHash<QString, const QQuickItem*> inTheLines = buttonsInTheEditBoxsLines(desk);
    for (auto button = inTheLines.cbegin(); button != inTheLines.cend(); ++button) {
        seen.controls.remove(button.key());
        seen.live.removeAll(button.key());
        seen.ofTheSection.removeAll(button.key());
    }
    return seen;
}
QStringList linesMoved(const QHash<QString, QRectF>& before, const QHash<QString, QRectF>& after, const QString& when)
{
    QStringList wrong;
    for (auto was = before.cbegin(); was != before.cend(); ++was) {
        const QRectF is = after.value(was.key());
        if (!after.contains(was.key()) || qAbs(is.x() - was.value().x()) > 0.5 ||
            qAbs(is.y() - was.value().y()) > 0.5 || qAbs(is.width() - was.value().width()) > 0.5) {
            wrong.append(u"%1: the line \"%2\" was at %3,%4 and is at %5,%6"_s.arg(when, was.key())
                             .arg(was.value().x())
                             .arg(was.value().y())
                             .arg(is.x())
                             .arg(is.y()));
        }
    }
    return wrong;
}

// Keys typed where the keyboard is, one for each sign of a text (of the twenty-six letters, digits and
// what stands beside them on a keyboard).
void typeKeys(QQuickWindow* window, const QString& text)
{
    for (const QChar sign : text) {
        QTest::keyClick(window, sign.toLatin1());
    }
}

// Something typed into a line of a box, by the line's name, in the place of what stood in it: as a
// person puts the keyboard there, takes all of what stands there, and types. True where it was there.
bool typeInto(Desk& desk, const QString& name, const QString& text)
{
    QQuickItem* line = theOneToWorkOn(desk.window, name);
    if (line == nullptr || !QMetaObject::invokeMethod(line, "takeFocus")) {
        return false;
    }
    QTest::keyClick(desk.window, Qt::Key_A, Qt::ControlModifier);
    typeKeys(desk.window, text);
    return true;
}

// The line of text that says exactly this in what the box that changes a KVM holds (not its title),
// where it can be seen; null: none says it.
const QQuickItem* boxSaying(const Desk& desk, const QString& text)
{
    const QObject* box = desk.box(u"editKvm"_s);
    return box != nullptr ? saying(box->property("contentItem").value<QQuickItem*>(), text) : nullptr;
}

// A click on a button of a KVM's row, once it is in sight: the section is scrolled to it first, as
// with the wheel (in a small window the rows stand far down). True where it was.
bool clickInRow(Desk& desk, const QString& id, const QString& name)
{
    QQuickItem* button = inRowToWorkOn(desk, id, name);
    QQuickItem* content = theOneToWorkOn(desk.window, u"sectionContent"_s);
    const auto* inside = content != nullptr ? content->property("contentItem").value<QQuickItem*>() : nullptr;
    if (button == nullptr || inside == nullptr) {
        return false;
    }
    const qreal most = std::max(0.0, content->property("contentHeight").toReal() - content->height());
    const qreal at = button->mapToItem(inside, QPointF(0, 0)).y();
    content->setProperty("contentY", std::clamp(at - 20, 0.0, most));
    aFrame(desk.window);
    clickOn(desk.window, button);
    return true;
}

// The line (Line) that an item stands in; null where it stands in none.
const QQuickItem* lineOf(const QQuickItem* item)
{
    for (const QQuickItem* up = item; up != nullptr; up = up->parentItem()) {
        if (up->property("shape").toString() == u"line"_s) {
            return up;
        }
    }
    return nullptr;
}

// A section made for a test, of the shared shapes and of whatever else it is given: as the screens
// make one, in the window, at a size of its own. Null where it could not be made.
std::unique_ptr<QQuickItem> sectionOf(Desk& desk, const QByteArray& inside)
{
    QQmlComponent made(&desk.engine);
    made.setData("import QtQuick\nimport Qkvm\n\nSection {\n    objectName: \"planted\"\n    width: 800\n"
                 "    height: 600\n    name: \"Planted\"\n\n" +
                     inside + "\n}\n",
                 QUrl(u"qrc:/qt/qml/Qkvm/planted.qml"_s));
    std::unique_ptr<QQuickItem> section(qobject_cast<QQuickItem*>(made.create()));
    if (section == nullptr) {
        for (const QQmlError& error : made.errors()) {
            desk.complaints.append(error.toString());
        }
        return nullptr;
    }
    section->setParentItem(desk.window->contentItem());
    aFrame(desk.window);
    return section;
}

// Longer words than the program's own, where a test needs them: one of the looks is called by a
// long name. (What the screens say is translated by this while it is installed.)
class LongerWords final : public QTranslator {
public:
    bool isEmpty() const override { return false; }
    QString translate(const char* context, const char* source, const char* disambiguation, int n) const override
    {
        static_cast<void>(context);
        static_cast<void>(disambiguation);
        static_cast<void>(n);
        return QByteArrayView(source) == "As Windows is set" ? u"As Windows is set for whoever is signed in to it"_s
                                                             : QString();
    }
};

} // namespace

// The settings box, on the screen: its sections in their groups, the one shape every section has,
// the KVMs of this PC's wall as a list, and a box that holds still; by the buttons and keys a person
// presses, in a window the program's own screens make. Without a desktop and without a graphics
// card: Qt's off-screen platform, and Qt Quick drawing in software. This PC's wall is the program's
// own, with KVMs at ports of this PC's own address where nothing answers.
class TestSettingsScreens : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void cleanup()
    {
        // (One look for every test that does not say otherwise.)
        ui::Look::tellQt(Qt::ColorScheme::Dark);
    }

    // --- which sections ------------------------------------------------------------------------------

    void incidentHistoryCanBeReviewedAcknowledgedAndPaused_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("theme");
        QTest::newRow("wide-dark") << QSize(1280, 800) << u"dark"_s;
        QTest::newRow("narrow-light") << QSize(640, 480) << u"light"_s;
    }
    void incidentHistoryCanBeReviewedAcknowledgedAndPaused()
    {
        QFETCH(QSize, size);
        QFETCH(QString, theme);
        Desk desk;
        const auto fast = [](QByteArrayView text, QByteArrayView salt,
                             const std::stop_token&) -> base::Result<QByteArray> {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(salt);
            hash.addData(text);
            return hash.result();
        };
        desk.security = std::make_unique<ui::AppLock>(desk.secrets, fast, [] { return 1000; });
        desk.security->submit({}, u"example-password"_s, u"example-password"_s);
        QTRY_VERIFY(!desk.security->restricted());
        desk.incidents = std::make_unique<core::Incidents>(desk.folder.filePath(u"incidents"_s));
        desk.alerts = std::make_unique<ui::AlertsView>(*desk.incidents, *desk.security);
        desk.ownSettings.useAlerts(*desk.alerts);
        desk.alerts->context(u"pc"_s, true);
        core::Machine machine;
        machine.id = u"test"_s;
        machine.name = u"Workshop test machine"_s;
        desk.alerts->inventory({machine});
        core::Status status;
        status.state = u"offline"_s;
        desk.alerts->observe({{machine.id, status}}, true);
        desk.alerts->observe({{machine.id, status}}, true);
        desk.alerts->unavailable(u"server-test"_s, u"Example server"_s, u"Sign in again to resume monitoring"_s);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(lookIs(desk, theme));
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"alerts"_s, u"settingsAlerts"_s));
        QVERIFY(bringIntoSight(desk, u"incidentSource"_s));
        clickOn(desk.window, theOne(desk.window, u"incidentSource"_s));
        QTRY_COMPARE(seenOnes(desk.window, u"choice"_s).size(), 2);
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTRY_COMPARE(desk.alerts->scope(), u"server-test"_s);
        QVERIFY(desk.alerts->entries().isEmpty());
        QSignalSpy opening(desk.alerts.get(), &ui::AlertsView::openAsked);
        QVERIFY(bringIntoSight(desk, u"openIncidentSource"_s));
        clickOn(desk.window, theOne(desk.window, u"openIncidentSource"_s));
        QCOMPARE(opening.size(), 1);
        QCOMPARE(opening.front().front().toString(), u"server-test"_s);
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"alerts"_s, u"settingsAlerts"_s));
        QVERIFY(bringIntoSight(desk, u"incidentSource"_s));
        keepPictureOf(desk.window, u"monitoring-sources-%1-%2"_s.arg(theme).arg(size.width()));
        desk.alerts->choose(u"pc"_s);
        QTRY_VERIFY(theOne(desk.window, u"acknowledgeIncident"_s) != nullptr);
        aFrame(desk.window);
        aFrame(desk.window);
        QTRY_VERIFY(bringIntoSight(desk, u"acknowledgeIncident"_s));
        clickOn(desk.window, theOne(desk.window, u"acknowledgeIncident"_s));
        QTRY_COMPARE(desk.alerts->attention(), 0);
        QVERIFY(desk.incidents->entries(u"pc"_s).front().acknowledged);
        QVERIFY(bringIntoSight(desk, u"pauseDeviceAlerts"_s));
        clickOn(desk.window, theOne(desk.window, u"pauseDeviceAlerts"_s));
        QTRY_VERIFY(desk.alerts->entries().front().toMap()[u"muted"_s].toBool());
        const QStringList wrong = laidOutOtherwise(shownSection(desk));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        keepPictureOf(desk.window, u"alerts-active-%1-%2"_s.arg(theme).arg(size.width()));
        status.state = u"online"_s;
        desk.alerts->observe({{machine.id, status}}, true);
        desk.alerts->observe({{machine.id, status}}, true);
        QTRY_VERIFY(desk.alerts->entries().isEmpty());
        QVERIFY(bringIntoSight(desk, u"showIncidentHistory"_s));
        clickOn(desk.window, theOne(desk.window, u"showIncidentHistory"_s));
        QTRY_COMPARE(desk.alerts->entries().size(), 1);
        QVERIFY(!desk.alerts->entries().front().toMap()[u"active"_s].toBool());
        QVERIFY(bringIntoSight(desk, u"pauseAlerts"_s));
        clickOn(desk.window, theOne(desk.window, u"pauseAlerts"_s));
        QTRY_VERIFY(desk.alerts->paused());
        keepPictureOf(desk.window, u"alerts-history-%1-%2"_s.arg(theme).arg(size.width()));
        desk.security->lock();
        QTRY_VERIFY(desk.alerts->entries().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void releaseKeyCanBeChosenFromGeneralSettings()
    {
        Desk desk;
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        QVERIFY(bringIntoSight(desk, u"releaseKeyChoice"_s));
        clickOn(desk.window, theOne(desk.window, u"releaseKeyChoice"_s));
        QTRY_COMPARE(seenOnes(desk.window, u"choice"_s).size(), 3);
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTRY_COMPARE(desk.ownSettings.releaseKey(), u"scroll-lock"_s);
        QCOMPARE(desk.control.leaveKeyName(), u"Scroll Lock"_s);
        QCOMPARE(core::Settings(desk.folder.filePath(u"settings.json"_s)).remembered().releaseKey, u"scroll-lock"_s);
        keepPictureOf(desk.window, u"settings-release-key"_s);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // Video: hardware acceleration is chosen from its list and taken up at once; the graphics card
    // is chosen for the next start, with the name of the card each choice comes to on this PC, and
    // a line that says when it takes effect, gone again where the choice is the one started with.
    void videoIsChosenInItsOwnSection()
    {
        Desk desk;
        desk.ownSettings.nameAdaptersWith([](engine::AdapterChoice choice) {
            return choice == engine::AdapterChoice::PowerSaving ? u"Graphics in the processor"_s
                                                                : u"Graphics card of its own"_s;
        });
        desk.ownSettings.seeMonitorsWith(
            [](engine::AdapterChoice choice) { return choice != engine::AdapterChoice::PowerSaving; });
        const QSignalSpy chosen(&desk.ownSettings, &ui::SettingsView::decodingChosen);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"video"_s, u"settingsVideo"_s));
        QVERIFY(says(desk.window, u"Hardware acceleration"_s));
        QVERIFY(says(desk.window, u"On this PC: Graphics card of its own"_s));
        QVERIFY(seenOnes(desk.window, u"cardAtTheNextStart"_s).isEmpty());

        QVERIFY(bringIntoSight(desk, u"hardwareDecodingChoice"_s));
        clickOn(desk.window, theOne(desk.window, u"hardwareDecodingChoice"_s));
        QTRY_COMPARE(seenOnes(desk.window, u"choice"_s).size(), 3);
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTRY_COMPARE(desk.ownSettings.hardwareDecoding(), u"off"_s);
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(core::Settings(desk.folder.filePath(u"settings.json"_s)).remembered().hardwareDecoding, u"off"_s);

        QVERIFY(bringIntoSight(desk, u"graphicsCardChoice"_s));
        clickOn(desk.window, theOne(desk.window, u"graphicsCardChoice"_s));
        QTRY_COMPARE(seenOnes(desk.window, u"choice"_s).size(), 3);
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTRY_COMPARE(desk.ownSettings.graphicsCard(), u"power-saving"_s);
        QTRY_VERIFY(says(desk.window, u"On this PC: Graphics in the processor"_s));
        QTRY_VERIFY(theOne(desk.window, u"cardWithoutTheScreen"_s) != nullptr);
        QVERIFY(says(desk.window,
                     u"The screen is on another graphics adapter here: Windows carries every picture to it."_s));
        // Where no adapter has the screen (as over Remote Desktop), nothing is said of it.
        desk.ownSettings.seeMonitorsWith([](engine::AdapterChoice) { return false; });
        QTRY_VERIFY(seenOnes(desk.window, u"cardWithoutTheScreen"_s).isEmpty());
        desk.ownSettings.seeMonitorsWith(
            [](engine::AdapterChoice choice) { return choice != engine::AdapterChoice::PowerSaving; });
        QTRY_VERIFY(theOne(desk.window, u"cardWithoutTheScreen"_s) != nullptr);
        QTRY_VERIFY(theOne(desk.window, u"cardAtTheNextStart"_s) != nullptr);
        QVERIFY(says(desk.window, u"Takes effect when qKVM starts again."_s));
        QStringList wrong = laidOutOtherwise(shownSection(desk));
        keepPictureOf(desk.window, u"settings-video"_s);

        desk.ownSettings.setGraphicsCard(u"most-powerful"_s);
        QTRY_VERIFY(seenOnes(desk.window, u"cardAtTheNextStart"_s).isEmpty());
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void lockingTheAppClearsTheConsolePin()
    {
        Desk desk;
        const auto fast = [](QByteArrayView text, QByteArrayView salt,
                             const std::stop_token&) -> base::Result<QByteArray> {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(salt);
            hash.addData(text);
            return hash.result();
        };
        desk.security = std::make_unique<ui::AppLock>(desk.secrets, fast, [] { return 1000; });
        desk.security->submit({}, u"example-password"_s, u"example-password"_s);
        QTRY_VERIFY(!desk.security->restricted());
        QVERIFY(desk.load() != nullptr);
        ui::WallTile tile;
        tile.key = u"a"_s;
        tile.name = u"Test machine"_s;
        tile.state = u"online"_s;
        desk.wall.machines()->setTiles({tile});
        desk.wall.enlarge(tile.key);
        QTRY_VERIFY(theOne(desk.window, u"alwaysOnTop"_s) != nullptr);
        clickOn(desk.window, theOne(desk.window, u"alwaysOnTop"_s));
        QTRY_VERIFY(desk.window->flags().testFlag(Qt::WindowStaysOnTopHint));
        desk.security->lock();
        QTRY_VERIFY(!desk.window->flags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(!desk.window->property("consoleOnTop").toBool());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // The settings stay open where what they list only grows (whoever is signed in comes to manage
    // the server's wall): they list it in place. Where it shrinks, or the window comes to show
    // another wall, they close, and the server's boxes that stood over them go with them.
    void theSettingsListAgainInPlaceWhereOnlyMoreComes()
    {
        Desk desk;
        ui::SessionView::Shown operating = atAServersWall();
        operating.manages = false;
        desk.session.show(operating);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QCOMPARE(sectionKeys(desk),
                 (QStringList{u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s, u"about"_s}));
        desk.session.show(atAServersWall());
        QTRY_COMPARE(sectionKeys(desk), (QStringList{u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s,
                                                     u"about"_s, u"files"_s}));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QCOMPARE(shownName(desk), u"settingsGeneral"_s);
        QVERIFY(choose(desk, u"files"_s, u"settingsServerFiles"_s));
        clickOn(desk.window, theOne(desk.window, u"serverExport"_s));
        QTRY_VERIFY(desk.isOpen(u"exportBox"_s));
        // No longer manages it: both go.
        desk.session.show(operating);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(!desk.isOpen(u"exportBox"_s));
        // Another wall (this PC's): the same.
        desk.session.show(atAServersWall());
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"files"_s, u"settingsServerFiles"_s));
        clickOn(desk.window, theOne(desk.window, u"serverImport"_s));
        QTRY_VERIFY(desk.isOpen(u"importBox"_s));
        desk.session.show(atItsOwnWall());
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(!desk.isOpen(u"importBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // The app lock is in the account's drop, with its key (Ctrl+L, as before). While it is on, the
    // drop says "Unlock qKVM", and what the lock holds back is not offered: the saved workspaces
    // there, and a server's wall as a file in the settings.
    void theAccountHoldsTheAppLock()
    {
        Desk desk;
        const auto fast = [](QByteArrayView text, QByteArrayView salt,
                             const std::stop_token&) -> base::Result<QByteArray> {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(salt);
            hash.addData(text);
            return hash.result();
        };
        desk.security = std::make_unique<ui::AppLock>(desk.secrets, fast, [] { return 1000; });
        desk.security->submit({}, u"example-password"_s, u"example-password"_s);
        QTRY_VERIFY(!desk.security->restricted());
        ui::WorkspacesView workspaces(desk.settings);
        workspaces.useAppLock(*desk.security);
        desk.session.show(atAServersWall());
        QVERIFY(desk.load() != nullptr);
        QVERIFY(desk.window->setProperty("workspaces", QVariant::fromValue(&workspaces)));
        const auto open = [&] {
            const QQuickItem* account = theOne(desk.window, u"accountButton"_s);
            if (account == nullptr) {
                return false;
            }
            clickOn(desk.window, account);
            return QTest::qWaitFor([&] { return desk.isOpen(u"accountMenu"_s); }, 5000);
        };
        const auto rowSays = [&](const QString& row, const QString& text) {
            const QQuickItem* one = theOne(desk.window, row);
            return one != nullptr && saying(one, text) != nullptr;
        };

        QVERIFY(open());
        QVERIFY(rowSays(u"lockRow"_s, u"Lock qKVM"_s));
        QVERIFY(rowSays(u"lockRow"_s, u"Ctrl+L"_s));
        QVERIFY(theOne(desk.window, u"savedWorkspaces"_s) != nullptr);
        keepPictureOf(desk.window, u"account-lock"_s);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"accountMenu"_s));
        QVERIFY(openSettings(desk));
        QVERIFY(sectionKeys(desk).contains(u"files"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QVERIFY(open());
        clickOn(desk.window, theOne(desk.window, u"lockRow"_s));
        QTRY_VERIFY(desk.security->locked());
        QTRY_VERIFY(!desk.isOpen(u"accountMenu"_s));
        // (The box that asks for the password at once is closed without it: the program stays
        // locked.)
        QCOMPARE(desk.security->mode(), u"unlock"_s);
        desk.security->cancel();
        QVERIFY(desk.security->locked());

        QVERIFY(open());
        QVERIFY(rowSays(u"lockRow"_s, u"Unlock qKVM"_s));
        QVERIFY(theOne(desk.window, u"savedWorkspaces"_s) == nullptr);
        keepPictureOf(desk.window, u"account-locked"_s);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"accountMenu"_s));
        QVERIFY(openSettings(desk));
        QVERIFY(!sectionKeys(desk).contains(u"files"_s));
        QVERIFY(!says(desk.window, u"The server's wall"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QVERIFY(open());
        clickOn(desk.window, theOne(desk.window, u"lockRow"_s));
        QTRY_COMPARE(desk.security->mode(), u"unlock"_s);
        QTRY_VERIFY(!desk.isOpen(u"accountMenu"_s));

        // Ctrl+L locks, as before, wherever the keyboard is.
        desk.security->submit(u"example-password"_s, {}, {});
        QTRY_VERIFY(!desk.security->restricted());
        QTest::keyClick(desk.window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(desk.security->locked());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // M-D1: where Windows does not hand over what qKVM keeps, the box says so and offers Try again and
    // Quit, and no reset; Try again carries on as soon as Windows answers.
    void appSecretsUnavailableOffersTryAgainAndQuit_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("theme");
        QTest::newRow("wide-dark") << QSize(1280, 800) << u"dark"_s;
        QTest::newRow("narrow-light") << QSize(640, 480) << u"light"_s;
    }
    void appSecretsUnavailableOffersTryAgainAndQuit()
    {
        QFETCH(QSize, size);
        QFETCH(QString, theme);
        Desk desk;
        desk.secrets.refusesInspection = true;
        const auto fast = [](QByteArrayView text, QByteArrayView salt,
                             const std::stop_token&) -> base::Result<QByteArray> {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(salt);
            hash.addData(text);
            return hash.result();
        };
        desk.security = std::make_unique<ui::AppLock>(desk.secrets, fast, [] { return 1000; });
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(lookIs(desk, theme));
        QCOMPARE(desk.security->mode(), u"unavailable"_s);
        const auto seen = [&desk](const QString& name) {
            return desk.box(name) != nullptr && desk.box(name)->property("visible").toBool();
        };
        QTRY_VERIFY(seen(u"appSecretsTryAgain"_s));
        QVERIFY(seen(u"appSecretsQuit"_s));
        QVERIFY(!seen(u"appPasswordSubmit"_s));
        QVERIFY(!seen(u"appForgotPassword"_s));
        QVERIFY(!seen(u"appCurrentPassword"_s));
        QTest::qWait(200); // Let the theme transition settle before retaining its image.
        keepPictureOf(desk.window, u"app-secrets-unavailable-%1-%2"_s.arg(theme).arg(size.width()));
        const QSignalSpy quit(desk.security.get(), &ui::AppLock::quitAsked);
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"appSecretsQuit"_s), "clicked"));
        QCOMPARE(quit.size(), 1);
        desk.secrets.refusesInspection = false;
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"appSecretsTryAgain"_s), "clicked"));
        QTRY_COMPARE(desk.security->mode(), u"setup"_s);
        QTRY_VERIFY(seen(u"appNewPassword"_s));
        // (Round 2: what stands where Try again stood takes no click for a moment, so that a second
        // click meant for Try again lands on nothing.)
        QVERIFY(!desk.box(u"appPasswordSubmit"_s)->property("enabled").toBool());
        QVERIFY(!desk.box(u"appLockCancel"_s)->property("enabled").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(desk.box(u"appPasswordSubmit"_s)->property("enabled").toBool(), 3000);
        QVERIFY(desk.box(u"appLockCancel"_s)->property("enabled").toBool());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void appPasswordAndSavedActionEditor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("theme");
        QTest::addColumn<bool>("existing");
        QTest::newRow("wide-dark") << QSize(1280, 800) << u"dark"_s << false;
        QTest::newRow("narrow-light") << QSize(640, 480) << u"light"_s << false;
        QTest::newRow("existing-wide-dark") << QSize(1280, 800) << u"dark"_s << true;
        QTest::newRow("existing-narrow-light") << QSize(640, 480) << u"light"_s << true;
    }

    void appPasswordAndSavedActionEditor()
    {
        QFETCH(QSize, size);
        QFETCH(QString, theme);
        QFETCH(bool, existing);
        Desk desk;
        if (existing) {
            desk.secrets.all.insert(u"existing KVM password"_s, "preserve me");
        }
        const auto fast = [](QByteArrayView text, QByteArrayView salt,
                             const std::stop_token&) -> base::Result<QByteArray> {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(salt);
            hash.addData(text);
            return hash.result();
        };
        desk.security = std::make_unique<ui::AppLock>(desk.secrets, fast, [] { return 1000; });
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(lookIs(desk, theme));
        QTest::qWait(200); // Let the theme transition settle before retaining its image.
        const QString suffix = u"-%1-%2%3"_s.arg(theme).arg(size.width()).arg(existing ? u"-existing"_s : QString());
        if (existing) {
            QCOMPARE(desk.security->mode(), u"recovery"_s);
            QVERIFY(desk.security->restricted());
            keepPictureOf(desk.window, u"app-password-recovery"_s + suffix);
            desk.security->submit({}, u"example-password"_s, u"example-password"_s);
            QVERIFY(desk.security->restricted());
            QCOMPARE(desk.secrets.all.value(u"existing KVM password"_s), QByteArray("preserve me"));
            QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
            return;
        }
        keepPictureOf(desk.window, u"app-password-setup"_s + suffix);
        QVERIFY(desk.box(u"appNewPassword"_s) != nullptr);
        QVERIFY(desk.box(u"appConfirmPassword"_s) != nullptr);
        desk.box(u"appNewPassword"_s)->setProperty("text", u"example-password"_s);
        desk.box(u"appConfirmPassword"_s)->setProperty("text", u"example-password"_s);
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"appPasswordSubmit"_s), "clicked"));
        QTRY_VERIFY(!desk.security->restricted());
        QCOMPARE(desk.box(u"appNewPassword"_s)->property("text").toString(), QString());
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"actions"_s, u"settingsActions"_s));
        const QQuickItem* add = theOne(desk.window, u"addSavedAction"_s);
        QVERIFY(add != nullptr);
        clickOn(desk.window, add);
        QTRY_VERIFY(desk.isOpen(u"savedActionEditor"_s));
        desk.box(u"savedActionName"_s)->setProperty("text", u"Task manager"_s);
        desk.box(u"savedActionKeys"_s)->setProperty("text", u"ControlLeft,ShiftLeft,Escape"_s);
        keepPictureOf(desk.window, u"saved-action-keys"_s + suffix);
        // An editor open with changes: an update that would close the program waits for it, and names it.
        QCOMPARE(desk.window->property("unsavedEdit").toString(), u"Add saved action"_s);
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"saveSavedAction"_s), "clicked"));
        QTRY_COMPARE(desk.savedActions->entries().size(), 1);
        QTRY_VERIFY(!desk.isOpen(u"savedActionEditor"_s));
        QTRY_COMPARE(seenOnes(desk.window, u"savedActionRow"_s).size(), 1);
        QCOMPARE(desk.window->property("unsavedEdit").toString(), QString());
        const QStringList layoutProblems = laidOutOtherwise(shownSection(desk));
        QVERIFY2(layoutProblems.isEmpty(), qPrintable(layoutProblems.join(u'\n')));
        keepPictureOf(desk.window, u"saved-actions-list"_s + suffix);
        clickOn(desk.window, theOne(desk.window, u"addSavedAction"_s));
        QTRY_VERIFY(desk.isOpen(u"savedActionEditor"_s));
        desk.box(u"savedActionKind"_s)->setProperty("value", u"text"_s);
        desk.box(u"savedActionEditor"_s)->setProperty("revealsText", true);
        const QPointer<QObject> text = desk.box(u"savedActionText"_s);
        QVERIFY(text != nullptr);
        text->setProperty("text", u"unsaved private test text"_s);
        desk.security->lock();
        QTRY_VERIFY(text == nullptr || text->property("text").toString().isEmpty());
        QTRY_VERIFY(desk.security->locked());
        keepPictureOf(desk.window, u"app-password-unlock"_s + suffix);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theSectionsListedAreThoseOfWhatTheWindowShows()
    {
        Desk desk;
        QVERIFY(desk.load() != nullptr);
        const QSignalSpy pageAsked(&desk.session, &ui::SessionView::pageAsked);

        // This PC's wall: its own two sections first, under their heading, then the program's.
        QVERIFY(openSettings(desk));
        QCOMPARE(sectionKeys(desk), (QStringList{u"kvms"_s, u"actions"_s, u"files"_s, u"general"_s, u"video"_s,
                                                 u"alerts"_s, u"security"_s, u"updates"_s, u"about"_s}));
        QVERIFY(says(desk.window, u"This PC's wall"_s));
        QVERIFY(says(desk.window, u"This program"_s));
        // (Its list of KVMs has none yet, and says so in one line, with nothing to press in it.)
        QCOMPARE(shownName(desk), u"settingsKvms"_s);
        QTRY_VERIFY(theOne(desk.window, u"listedNothing"_s) != nullptr);
        QVERIFY(says(desk.window, u"No KVMs yet. Add the first one with the button above."_s));
        QVERIFY(kvmRows(desk).isEmpty());
        QVERIFY(!says(desk.window, u"Connection"_s));
        // (Nothing of a server's is said there.)
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        QVERIFY(seenOnes(desk.window, u"settingsServer"_s).isEmpty());
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));

        // A server's wall that whoever is signed in does not manage: the program's own, and no
        // heading over the one group. Its first line says that the server's machines, accounts and
        // own settings are on its page, which it opens.
        ui::SessionView::Shown operating = atAServersWall();
        operating.manages = false;
        desk.session.show(operating);
        QVERIFY(openSettings(desk));
        QCOMPARE(sectionKeys(desk),
                 (QStringList{u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s, u"about"_s}));
        QVERIFY(!says(desk.window, u"This PC's wall"_s));
        QVERIFY(!says(desk.window, u"This program"_s));
        QCOMPARE(shownName(desk), u"settingsGeneral"_s);
        QVERIFY(theOne(desk.window, u"settingsServer"_s) != nullptr);
        QVERIFY(says(desk.window, u"The qKVM server at 192.168.1.10:8787"_s));
        QVERIFY(says(desk.window,
                     u"Its machines, its accounts and its own settings are managed on its page, in your browser."_s));
        clickOn(desk.window, theOne(desk.window, u"settingsOpenPage"_s));
        QTRY_COMPARE(pageAsked.count(), 1);
        keepPictureOf(desk.window, u"settings-general-server-dark-wide"_s);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));

        // One that whoever is signed in manages: its wall as a file first, under a heading of its
        // own, as the page lists it for an admin; then the program's.
        desk.session.show(atAServersWall());
        QVERIFY(openSettings(desk));
        QCOMPARE(sectionKeys(desk), (QStringList{u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s,
                                                 u"about"_s, u"files"_s}));
        QVERIFY(says(desk.window, u"The server's wall"_s));
        QVERIFY(says(desk.window, u"This program"_s));
        QVERIFY(!says(desk.window, u"This PC's wall"_s));
        // (Its General says whose page has the rest, as for anybody at a server's wall.)
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        QVERIFY(theOne(desk.window, u"settingsServer"_s) != nullptr);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));

        // No wall yet (nobody is signed in): the program's own, and nothing of a server's.
        desk.session.show(ui::SessionView::Shown{});
        QVERIFY(openSettings(desk));
        QCOMPARE(sectionKeys(desk),
                 (QStringList{u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s, u"about"_s}));
        QVERIFY(seenOnes(desk.window, u"settingsServer"_s).isEmpty());
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));

        // The window comes to show another wall while the box is open: what it lists is not that
        // wall's, and it closes.
        desk.session.show(atItsOwnWall());
        QVERIFY(openSettings(desk));
        QCOMPARE(sectionKeys(desk).size(), 9);
        desk.session.show(atAServersWall());
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QVERIFY(openSettings(desk));
        QCOMPARE(sectionKeys(desk).size(), 7);
        // (Something else of the session that changes is no other wall.)
        ui::SessionView::Shown away = atAServersWall();
        away.inContact = false;
        desk.session.show(away);
        QTest::qWait(100);
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void updatesShowAvailabilityWithoutDownloading_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("look");
        QTest::addColumn<core::UpdateKind>("kind");
        QTest::newRow("dark-wide") << QSize(1280, 800) << u"dark"_s << core::UpdateKind::Portable;
        QTest::newRow("light-wide") << QSize(1280, 800) << u"light"_s << core::UpdateKind::Portable;
        QTest::newRow("dark-narrow") << QSize(640, 600) << u"dark"_s << core::UpdateKind::Portable;
        QTest::newRow("light-wide-msix") << QSize(1280, 800) << u"light"_s << core::UpdateKind::Msix;
        QTest::newRow("dark-wide-setup") << QSize(1280, 800) << u"dark"_s << core::UpdateKind::Setup;
    }

    void updatesShowAvailabilityWithoutDownloading()
    {
        QFETCH(QSize, size);
        QFETCH(QString, look);
        QFETCH(core::UpdateKind, kind);
        const bool portable = kind == core::UpdateKind::Portable;
        const bool setup = kind == core::UpdateKind::Setup;
        const QString named = portable ? QString() : setup ? u"-setup"_s : u"-msix"_s;
        core::testing::ReleaseSigner signer;
        Desk desk({QUrl(u"https://updates.example.test/stable.json"_s), signer.publicKey(), u"0.1.0"_s, kind});
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(lookIs(desk, look));
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"updates"_s, u"settingsUpdates"_s));
        const QByteArray package("a made-up package, never executed");
        desk.updateNetwork.next = {
            signer.seal(core::testing::releaseFor(package, QDateTime::currentDateTimeUtc(), kind))};
        clickOn(desk.window, theOne(desk.window, u"checkForUpdates"_s));
        QTRY_COMPARE(desk.updates.state(), core::Updates::State::Available);
        aFrame(desk.window);
        QVERIFY(bringIntoSight(desk, u"downloadUpdate"_s));
        QVERIFY(says(desk.window, u"qKVM 0.2.0 is available."_s));
        QCOMPARE(desk.updateNetwork.requests.size(), 1);
        keepPictureOf(desk.window, u"settings-updates-available-%1-%2%3"_s.arg(look).arg(size.width()).arg(named));
        desk.updateNetwork.next = {package};
        clickOn(desk.window, theOne(desk.window, u"downloadUpdate"_s));
        QTRY_COMPARE(desk.updates.state(), core::Updates::State::Ready);
        aFrame(desk.window);
        if (portable) {
            // Shown, verified, in its folder: never an installer.
            QVERIFY(theOne(desk.window, u"installUpdate"_s) == nullptr);
            QVERIFY(bringIntoSight(desk, u"showUpdate"_s));
            QVERIFY(says(desk.window, u"qKVM 0.2.0 is downloaded and verified."_s));
            QVERIFY(says(desk.window, u"Unpack it into a new folder"_s));
            keepPictureOf(desk.window, u"settings-updates-ready-%1-%2"_s.arg(look).arg(size.width()));
            clickOn(desk.window, theOne(desk.window, u"showUpdate"_s));
            QTRY_VERIFY(says(desk.window, u"The verified file is shown in its folder."_s));
            QCOMPARE(desk.shownFiles.size(), 1);
            QCOMPARE(QFileInfo(desk.shownFiles.first()).fileName(), u"qkvm.zip"_s);
            QCOMPARE(desk.updates.state(), core::Updates::State::Ready);
        } else if (setup) {
            // "Install and restart": Setup, verified, once qKVM has closed. (This window's program hands over to
            // nothing: it is never pressed here.)
            QVERIFY(theOne(desk.window, u"showUpdate"_s) == nullptr);
            QVERIFY(bringIntoSight(desk, u"installUpdate"_s));
            QCOMPARE(theOne(desk.window, u"installUpdate"_s)->property("text").toString(), u"Install and restart"_s);
            QVERIFY(says(desk.window, u"qKVM 0.2.0 is downloaded and verified."_s));
            QVERIFY(says(desk.window, u"qKVM closes to install it"_s));
            QVERIFY(!says(desk.window, u"Save your work before installing"_s));
            keepPictureOf(desk.window, u"settings-updates-ready-%1-%2%3"_s.arg(look).arg(size.width()).arg(named));
        } else {
            QVERIFY(theOne(desk.window, u"showUpdate"_s) == nullptr);
            QVERIFY(bringIntoSight(desk, u"installUpdate"_s));
            QVERIFY(says(desk.window, u"The update is verified and ready to install."_s));
            keepPictureOf(desk.window, u"settings-updates-ready-%1-%2%3"_s.arg(look).arg(size.width()).arg(named));
        }
        QCOMPARE(desk.updateNetwork.requests.size(), 2);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void developmentBuildExplainsUnavailableUpdates()
    {
        Desk desk;
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"updates"_s, u"settingsUpdates"_s));
        QVERIFY(says(desk.window, u"Updates are not available for this development build."_s));
        QVERIFY(!theOne(desk.window, u"checkForUpdates"_s)->isEnabled());
        QVERIFY(desk.updateNetwork.requests.isEmpty());
        keepPictureOf(desk.window, u"settings-updates-development"_s);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theBoxOpensWhereItWasLeft()
    {
        Desk desk;
        QVERIFY(desk.load() != nullptr);
        // Never shown before: at the first section.
        QVERIFY(openSettings(desk));
        QCOMPARE(shownName(desk), u"settingsKvms"_s);
        QVERIFY(choose(desk, u"about"_s, u"settingsAbout"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QVERIFY(openSettings(desk));
        QCOMPARE(shownName(desk), u"settingsAbout"_s);
        QVERIFY(choose(desk, u"files"_s, u"settingsFiles"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        // At a server's wall that whoever is signed in does not manage, that section is not listed:
        // the box opens at the first that is.
        ui::SessionView::Shown operating = atAServersWall();
        operating.manages = false;
        desk.session.show(operating);
        QVERIFY(openSettings(desk));
        QCOMPARE(shownName(desk), u"settingsGeneral"_s);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        // Where it manages one, at that wall's own export and import.
        desk.session.show(atAServersWall());
        QVERIFY(openSettings(desk));
        QCOMPARE(shownName(desk), u"settingsServerFiles"_s);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        // Back at this PC's wall, where it was left there.
        desk.session.show(atItsOwnWall());
        QVERIFY(openSettings(desk));
        QCOMPARE(shownName(desk), u"settingsFiles"_s);
        QCOMPARE(desk.ownSettings.section(), u"files"_s);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void upAndDownAndLeftAndRightGoFromOneSectionToTheNext_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("wide") << QSize(1280, 800);
        QTest::newRow("narrow") << QSize(640, 400);
    }
    void upAndDownAndLeftAndRightGoFromOneSectionToTheNext()
    {
        QFETCH(QSize, size);
        QTest::failOnWarning("QQuickItem: Cannot set activeFocusOnTab to false once item is the active focus item.");
        Desk desk;
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openSettings(desk));
        // The keyboard is at the section shown, in the list.
        QTRY_VERIFY(entryOf(desk, u"kvms"_s)->hasActiveFocus());
        const auto press = [&](Qt::Key key, const QString& key2, const QString& section) {
            QTest::keyClick(desk.window, key);
            return QTest::qWaitFor([&] { return shownName(desk) == section; }, 20000) &&
                   QTest::qWaitFor(
                       [&] { return entryOf(desk, key2) != nullptr && entryOf(desk, key2)->hasActiveFocus(); }, 20000);
        };
        QVERIFY(press(Qt::Key_Down, u"actions"_s, u"settingsActions"_s));
        QVERIFY(press(Qt::Key_Down, u"files"_s, u"settingsFiles"_s));
        QVERIFY(press(Qt::Key_Down, u"general"_s, u"settingsGeneral"_s));
        QVERIFY(press(Qt::Key_Down, u"video"_s, u"settingsVideo"_s));
        QVERIFY(press(Qt::Key_Down, u"alerts"_s, u"settingsAlerts"_s));
        QVERIFY(press(Qt::Key_Down, u"security"_s, u"settingsSecurity"_s));
        QVERIFY(press(Qt::Key_Down, u"updates"_s, u"settingsUpdates"_s));
        QVERIFY(press(Qt::Key_Down, u"about"_s, u"settingsAbout"_s));
        // Round the end, and back.
        QVERIFY(press(Qt::Key_Down, u"kvms"_s, u"settingsKvms"_s));
        QVERIFY(press(Qt::Key_Up, u"about"_s, u"settingsAbout"_s));
        QVERIFY(press(Qt::Key_Left, u"updates"_s, u"settingsUpdates"_s));
        QVERIFY(press(Qt::Key_Right, u"about"_s, u"settingsAbout"_s));
        QVERIFY(press(Qt::Key_Right, u"kvms"_s, u"settingsKvms"_s));
        // And what is chosen so is where the box opens the next time.
        QCOMPARE(desk.ownSettings.section(), u"kvms"_s);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- one shape ----------------------------------------------------------------------------------

    void theFirstLineStaysInSightWhileALongSectionScrolls()
    {
        Desk desk;
        QList<Put> many;
        for (int number = 1; number <= 40; ++number) {
            many.append({u"http://127.0.0.1:%1"_s.arg(20400 + number), u"Rack %1"_s.arg(number)});
        }
        const QStringList ids = desk.put(many);
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(theOne(desk.window, u"kvmList"_s)->property("count").toInt(), 40);
        const QQuickItem* add = theOne(desk.window, u"settingsAddKvms"_s);
        const QQuickItem* head = theOne(desk.window, u"sectionHead"_s);
        QVERIFY(add != nullptr && head != nullptr);
        const QRectF addWas = seenPlaceOf(add);
        const QRectF headWas = seenPlaceOf(head);
        QQuickItem* content = theOneToWorkOn(desk.window, u"sectionContent"_s);
        QVERIFY(content != nullptr);
        const qreal most = content->property("contentHeight").toReal() - content->height();
        QVERIFY(most > 1000);
        const QRectF shows = seenPlaceOf(content);
        // The last row's buttons are out of sight, under the section's end (where the list does not
        // even make that row yet); its first line is above.
        const QQuickItem* lastRow = rowOfKvm(desk, ids.last());
        QVERIFY(lastRow == nullptr || !shows.contains(seenPlaceOf(inRow(lastRow, u"kvmEdit"_s)).center()));
        // Scrolled to its end, as with the wheel.
        for (qreal at = 0; at <= most; at += 120) {
            content->setProperty("contentY", at);
            aFrame(desk.window);
        }
        content->setProperty("contentY", most);
        aFrame(desk.window);
        // The first line is where it was, whole, and "Add KVMs" in it.
        QCOMPARE(seenPlaceOf(add), addWas);
        QCOMPARE(seenPlaceOf(head), headWas);
        QVERIFY(isSeen(add));
        // The last row's buttons are in sight, under the first line; the first row's are scrolled
        // away under it, and cannot be seen there.
        QVERIFY(rowOfKvm(desk, ids.last()) != nullptr);
        const QRectF lastEdit = seenPlaceOf(inRow(rowOfKvm(desk, ids.last()), u"kvmEdit"_s));
        QVERIFY(shows.contains(lastEdit));
        QVERIFY(lastEdit.top() >= headWas.bottom());
        const QQuickItem* firstRow = rowOfKvm(desk, ids.first());
        const QRectF firstEdit = firstRow != nullptr ? seenPlaceOf(inRow(firstRow, u"kvmEdit"_s)) : QRectF();
        QVERIFY(firstEdit.isEmpty() || !firstEdit.intersects(headWas));
        // (Not every row is made: those far out of sight are not.)
        QVERIFY(kvmRows(desk).size() < 40);
        keepPictureOf(desk.window, u"settings-kvms-scrolled"_s);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void everySectionIsMadeOfTheSharedShapesAndOfNothingElse_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("wide") << QSize(1280, 800);
        QTest::newRow("narrow") << QSize(640, 400);
    }
    void everySectionIsMadeOfTheSharedShapesAndOfNothingElse()
    {
        QFETCH(QSize, size);
        Desk desk;
        QVERIFY(!desk.put({{u"https://127.0.0.1:20501"_s, u"Rack 1"_s, 'a'}, {u"http://127.0.0.1:20502"_s, u"NAS"_s}})
                     .contains(QString()));
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openSettings(desk));
        QStringList wrong;
        // Each section and its lines and rows; the populated saved-action list is checked above.
        const QList<std::tuple<QString, QString, int, int>> all{
            {u"kvms"_s, u"settingsKvms"_s, 0, 2},         {u"actions"_s, u"settingsActions"_s, 0, 0},
            {u"files"_s, u"settingsFiles"_s, 4, 0},       {u"general"_s, u"settingsGeneral"_s, 5, 0},
            {u"video"_s, u"settingsVideo"_s, 2, 0},       {u"alerts"_s, u"settingsAlerts"_s, 4, 0},
            {u"security"_s, u"settingsSecurity"_s, 2, 0}, {u"updates"_s, u"settingsUpdates"_s, 4, 0},
            {u"about"_s, u"settingsAbout"_s, 2, 2}};
        for (const auto& [key, section, lines, rows] : all) {
            QVERIFY2(choose(desk, key, section), qPrintable(key));
            wrong += laidOutOtherwise(shownSection(desk));
            QVERIFY2(shapesOf(shownSection(desk), u"line"_s) >= lines, qPrintable(key));
            QVERIFY2(shapesOf(shownSection(desk), u"row"_s) >= rows, qPrintable(key));
        }
        // And the line of what went wrong, where Windows did not take a change.
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        desk.start.refuses = true;
        QVERIFY(bringIntoSight(desk, u"startsWithWindows"_s));
        clickOn(desk.window, theOne(desk.window, u"startsWithWindows"_s));
        QTRY_VERIFY(theOne(desk.window, u"settingsProblem"_s) != nullptr);
        wrong += laidOutOtherwise(shownSection(desk));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        // And at a server's wall, its line in General.
        desk.session.show(atAServersWall());
        QVERIFY(openSettings(desk));
        QVERIFY(theOne(desk.window, u"settingsServer"_s) != nullptr);
        wrong += laidOutOtherwise(shownSection(desk));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- this PC's KVMs ---------------------------------------------------------------------------

    // A box with changes that are not saved names itself to an update that would close the program (the window's
    // "unsavedEdit", fix round of the Setup updates): Add a KVM with something typed, Edit with a line changed; none
    // where nothing was typed or nothing differs.
    void aBoxWithChangesThatAreNotSavedIsNamedToAnUpdate()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"http://127.0.0.1:20604"_s, u"NAS"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        const auto unsaved = [&desk] { return desk.window->property("unsavedEdit").toString(); };
        QCOMPARE(unsaved(), QString());
        QVERIFY(openSettings(desk));
        QCOMPARE(shownName(desk), u"settingsKvms"_s);
        clickOn(desk.window, theOne(desk.window, u"settingsAddKvms"_s));
        QTRY_VERIFY(desk.isOpen(u"addKvm"_s));
        QCOMPARE(unsaved(), QString());
        QVERIFY(typeInto(desk, u"kvmAddress"_s, u"192.0.2.10"_s));
        QTRY_COMPARE(unsaved(), u"Add a KVM"_s);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"addKvm"_s));
        QCOMPARE(unsaved(), QString());
        QTRY_VERIFY(rowOfKvm(desk, ids.at(0)) != nullptr);
        clickOn(desk.window, inRow(rowOfKvm(desk, ids.at(0)), u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QCOMPARE(unsaved(), QString());
        QVERIFY(typeInto(desk, u"editName"_s, u"NAS 2"_s));
        QTRY_COMPARE(unsaved(), u"Edit \"NAS\""_s);
        clickOn(desk.window, theOne(desk.window, u"editCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QCOMPARE(unsaved(), QString());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theKvmsListShowsEachCaseAndItsButtonsOpenTheRightBox()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"https://127.0.0.1:20601"_s, u"Rack 1"_s, 'a'},
                                          {u"https://127.0.0.1:20602"_s, u"Rack 1 again"_s, 'a'},
                                          {u"https://127.0.0.1:20603"_s, u"Behind a proxy"_s},
                                          {u"http://127.0.0.1:20604"_s, u"NAS"_s},
                                          {u"https://127.0.0.1:20605"_s, u"No password"_s, 'e'}});
        QVERIFY(!ids.contains(QString()));
        // Its password is gone from Windows' store, before the program starts: the wall's mark says
        // that one was kept, and its driver finds none when it looks.
        desk.secrets.all.remove(core::LocalWall::secretOf(ids.at(4)));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QCOMPARE(shownName(desk), u"settingsKvms"_s);
        QTRY_COMPARE(kvmRows(desk).size(), 5);
        // The wall is asked how its KVMs are, as a window that shows it asks: its drivers look.
        desk.own->wall().askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
        for (qsizetype at = 0; at < ids.size(); ++at) {
            QCOMPARE(kvmRows(desk).at(at)->property("key").toString(), ids.at(at));
        }
        QVERIFY(says(desk.window, u"Name"_s));
        QVERIFY(says(desk.window, u"Address"_s));
        QVERIFY(says(desk.window, u"Connection"_s));
        const QQuickItem* rack = rowOfKvm(desk, ids.at(0));
        const QQuickItem* again = rowOfKvm(desk, ids.at(1));
        const QQuickItem* proxied = rowOfKvm(desk, ids.at(2));
        const QQuickItem* nas = rowOfKvm(desk, ids.at(3));
        const QQuickItem* without = rowOfKvm(desk, ids.at(4));
        // Its name, and under it, in the colour of a problem: the two that are one device each say
        // which other it is; the one whose password is not kept says so, as its driver found it
        // (which wins over the wall's mark).
        QVERIFY(rowSays(rack, u"Rack 1"_s));
        QVERIFY(rowSays(rack, u"Same device as \"Rack 1 again\""_s));
        QVERIFY(rowSays(again, u"Same device as \"Rack 1\""_s));
        QTRY_VERIFY_WITH_TIMEOUT(rowSays(without, u"No password saved"_s), 20000);
        const QColor bad = desk.engine.singletonInstance<QObject*>("Qkvm", "Theme")->property("bad").value<QColor>();
        QCOMPARE(saying(without, u"No password saved"_s)->property("color").value<QColor>(), bad);
        QCOMPARE(saying(rack, u"Same device as \"Rack 1 again\""_s)->property("color").value<QColor>(), bad);
        QVERIFY(!rowSays(nas, u"No password saved"_s));
        QVERIFY(!rowSays(proxied, u"Same device as \"Rack 1\""_s));
        // Where it is, and under it what kind of KVM.
        QVERIFY(rowSays(nas, u"127.0.0.1:20604"_s));
        for (const QQuickItem* row : kvmRows(desk)) {
            QVERIFY(rowSays(row, u"GL.iNet Comet / PiKVM"_s));
        }
        // How it is spoken to.
        QVERIFY(rowSays(rack, u"Known by its certificate"_s));
        QVERIFY(rowSays(proxied, u"Known by Windows' word"_s));
        QVERIFY(rowSays(nas, u"Plain HTTP: its password goes in the clear"_s));
        keepPictureOf(desk.window, u"settings-kvms-cases"_s);

        // "Edit": the box that changes a KVM, for that KVM, with its lines.
        clickOn(desk.window, inRow(nas, u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QCOMPARE(desk.editing.machine(), ids.at(3));
        QVERIFY(!desk.editing.removesOnly());
        QVERIFY(says(desk.window, u"Edit \"NAS\""_s));
        QVERIFY(theOne(desk.window, u"editSave"_s) != nullptr);
        QVERIFY(seenOnes(desk.window, u"removeYes"_s).isEmpty());
        // (Over the settings, which stay.)
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        clickOn(desk.window, theOne(desk.window, u"editCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));

        // "Add KVMs": the box that adds one.
        clickOn(desk.window, theOne(desk.window, u"settingsAddKvms"_s));
        QTRY_VERIFY(desk.isOpen(u"addKvm"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"addKvm"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aJetKvmsRowAndItsBoxSayWhatItIsAndThatNoPasswordIsNoProblem()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"https://127.0.0.1:20611"_s, u"Rack"_s, 'a'},
                                          {u"http://127.0.0.1:20612"_s, u"Desk"_s, 0, QString(), u"jetkvm"_s},
                                          {u"http://127.0.0.1:20613"_s, u"Guarded"_s, 0, u"secret"_s, u"jetkvm"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 3);
        const QQuickItem* rack = rowOfKvm(desk, ids.at(0));
        const QQuickItem* open = rowOfKvm(desk, ids.at(1));
        const QQuickItem* guarded = rowOfKvm(desk, ids.at(2));
        // Under where each is, what kind of KVM it is.
        QVERIFY(rowSays(rack, u"GL.iNet Comet / PiKVM"_s));
        QVERIFY(rowSays(open, u"JetKVM"_s));
        QVERIFY(!rowSays(open, u"GL.iNet Comet / PiKVM"_s));
        QVERIFY(rowSays(guarded, u"JetKVM"_s));
        // A JetKVM with no password is one that runs without one, as it comes out of its box: said so,
        // and not in the colour of a problem.
        QVERIFY(!rowSays(open, u"No password saved"_s));
        const QQuickItem* fine = saying(open, u"No password: fine if it runs without one"_s);
        QVERIFY(fine != nullptr);
        const QColor bad = desk.engine.singletonInstance<QObject*>("Qkvm", "Theme")->property("bad").value<QColor>();
        QVERIFY(fine->property("color").value<QColor>() != bad);
        QVERIFY(!rowSays(guarded, u"No password: fine if it runs without one"_s));

        // "Edit": its type, and no line for a user name, which a JetKVM does not have.
        clickOn(desk.window, inRow(open, u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QCOMPARE(desk.editing.family(), u"jetkvm"_s);
        QVERIFY(says(desk.window, u"Type: JetKVM"_s));
        QVERIFY(!says(desk.window, u"Type: GL.iNet Comet / PiKVM"_s));
        const auto* user = theOne(desk.window, u"editUser"_s);
        QVERIFY(user == nullptr || !user->isVisible());
        clickOn(desk.window, theOne(desk.window, u"editCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        // And a kvmd's box has both.
        clickOn(desk.window, inRow(rack, u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QCOMPARE(desk.editing.family(), u"kvmd"_s);
        QVERIFY(says(desk.window, u"Type: GL.iNet Comet / PiKVM"_s));
        QVERIFY(theOne(desk.window, u"editUser"_s) != nullptr && theOne(desk.window, u"editUser"_s)->isVisible());
        clickOn(desk.window, theOne(desk.window, u"editCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aServersRowAndItsBoxSayWhatItIs()
    {
        // M7 1c: a server's management controller on this PC's wall, in the list of KVMs and in its change box, called
        // what the page calls its kind (words.json: "Server (Redfish)"), with a user name as a kvmd's has.
        Desk desk;
        const QStringList ids = desk.put({{u"https://127.0.0.1:20621"_s, u"Rack"_s, 'a'},
                                          {u"https://127.0.0.1:20622"_s, u"db-01"_s, 'b', u"secret"_s, u"redfish"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 2);
        const QQuickItem* server = rowOfKvm(desk, ids.at(1));
        QVERIFY(rowSays(server, u"Server (Redfish)"_s));
        QVERIFY(!rowSays(server, u"GL.iNet Comet / PiKVM"_s));
        QVERIFY(!rowSays(rowOfKvm(desk, ids.at(0)), u"Server (Redfish)"_s));
        clickOn(desk.window, inRow(server, u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QCOMPARE(desk.editing.family(), u"redfish"_s);
        QVERIFY(says(desk.window, u"Type: Server (Redfish)"_s));
        QVERIFY(!says(desk.window, u"Type: GL.iNet Comet / PiKVM"_s));
        QVERIFY(theOne(desk.window, u"editUser"_s) != nullptr && theOne(desk.window, u"editUser"_s)->isVisible());
        clickOn(desk.window, theOne(desk.window, u"editCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void removeAsksFirstAndTheRowSaysWhatCameOfIt()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"https://127.0.0.1:20701"_s, u"Rack 1"_s, 'a'},
                                          {u"https://127.0.0.1:20702"_s, u"Rack 1 again"_s, 'a'},
                                          {u"http://127.0.0.1:20703"_s, u"NAS"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 3);

        // "Remove": the question first, in the page's words, for that KVM; nothing is taken off.
        clickOn(desk.window, inRow(rowOfKvm(desk, ids.at(1)), u"kvmRemove"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QCOMPARE(desk.editing.machine(), ids.at(1));
        QVERIFY(desk.editing.removesOnly());
        QVERIFY(says(desk.window, u"Remove \"Rack 1 again\"?"_s));
        QVERIFY(says(desk.window, u"The KVM disappears from the wall and its saved password is erased. The KVM itself "
                                  "is not touched."_s));
        QTRY_VERIFY(theOne(desk.window, u"removeYes"_s) != nullptr);
        QVERIFY(theOne(desk.window, u"removeNo"_s) != nullptr);
        QVERIFY(seenOnes(desk.window, u"editSave"_s).isEmpty());
        QCOMPARE(desk.own->wall().kvms().size(), 3);
        keepPictureOf(desk.window, u"settings-kvms-remove-asks"_s);
        // No: out of the box, with nothing taken off.
        clickOn(desk.window, theOne(desk.window, u"removeNo"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QCOMPARE(desk.own->wall().kvms().size(), 3);
        QVERIFY(desk.isOpen(u"settingsBox"_s));

        // Yes: taken off. Its row stays where it was, and says so; its buttons are there, and take
        // no press. The other is the same device as nothing any more.
        const QRectF stood = seenPlaceOf(inRow(rowOfKvm(desk, ids.at(1)), u"kvmRemove"_s));
        clickOn(desk.window, inRow(rowOfKvm(desk, ids.at(1)), u"kvmRemove"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(theOne(desk.window, u"removeYes"_s) != nullptr);
        clickOn(desk.window, theOne(desk.window, u"removeYes"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QCOMPARE(desk.own->wall().kvms().size(), 2);
        const QQuickItem* gone = rowOfKvm(desk, ids.at(1));
        QVERIFY(gone != nullptr);
        QTRY_VERIFY(rowSays(gone, u"Taken off this wall"_s));
        QVERIFY(inRow(gone, u"kvmEdit"_s) != nullptr && inRow(gone, u"kvmRemove"_s) != nullptr);
        QVERIFY(!inRow(gone, u"kvmEdit"_s)->isEnabled());
        QVERIFY(!inRow(gone, u"kvmRemove"_s)->isEnabled());
        aFrame(desk.window);
        QCOMPARE(seenPlaceOf(inRow(gone, u"kvmRemove"_s)), stood);
        QTRY_VERIFY(!rowSays(rowOfKvm(desk, ids.at(0)), u"Same device as \"Rack 1 again\""_s));
        QCOMPARE(kvmRows(desk).size(), 3);
        keepPictureOf(desk.window, u"settings-kvms-removed"_s);
        // Shown anew, the list is the wall's as it is.
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 2);
        QVERIFY(rowOfKvm(desk, ids.at(1)) == nullptr);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- it holds still --------------------------------------------------------------------------

    void theBoxHoldsStillAroundEveryChangeItKnows_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("1280x800") << QSize(1280, 800);
        QTest::newRow("640x400") << QSize(640, 400);
    }
    void theBoxHoldsStillAroundEveryChangeItKnows()
    {
        QFETCH(QSize, size);
        const QString at = u", at %1 by %2"_s.arg(size.width()).arg(size.height());
        Desk desk;
        const QStringList ids = desk.put({{u"http://127.0.0.1:20801"_s, u"One"_s},
                                          {u"https://127.0.0.1:20802"_s, u"Two"_s},
                                          {u"https://127.0.0.1:20803"_s, u"Three"_s, 'c'}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 3);
        QStringList wrong;

        // Another section, and another, and back: nothing that is not the section's moves, nor the
        // box, whichever is shown.
        Seen before = seenOf(desk);
        for (const auto& [key, section] : QList<std::pair<QString, QString>>{{u"files"_s, u"settingsFiles"_s},
                                                                             {u"actions"_s, u"settingsActions"_s},
                                                                             {u"general"_s, u"settingsGeneral"_s},
                                                                             {u"video"_s, u"settingsVideo"_s},
                                                                             {u"security"_s, u"settingsSecurity"_s},
                                                                             {u"about"_s, u"settingsAbout"_s},
                                                                             {u"kvms"_s, u"settingsKvms"_s}}) {
            // Scrolling the tab strip is intentional; selecting the revealed tab must not move it.
            const QRectF boxBeforeScroll = before.box;
            QVERIFY(revealSection(desk, key));
            before = seenOf(desk);
            QCOMPARE(before.box, boxBeforeScroll);
            QVERIFY(choose(desk, key, section));
            const Seen after = seenOf(desk);
            wrong += movedBy(before, after, u"to the section "_s + key + at, Allowed::AnotherSection);
            before = after;
        }
        QTRY_COMPARE(kvmRows(desk).size(), 3);

        // The list grows: a KVM is added elsewhere (another window, a file).
        before = seenOf(desk);
        core::OwnKvm four;
        four.address = u"http://127.0.0.1:20804"_s;
        four.name = u"Four"_s;
        QVERIFY(desk.own->wall().add(four, u"secret"_s).ok());
        QTRY_COMPARE(kvmRows(desk).size(), 4);
        wrong += movedBy(before, seenOf(desk), u"the list grows"_s + at);
        // It shrinks: one is taken off elsewhere. Its row stays.
        before = seenOf(desk);
        QVERIFY(desk.own->wall().remove(ids.at(1)).ok());
        QTRY_VERIFY(rowSays(rowOfKvm(desk, ids.at(1)), u"Taken off this wall"_s));
        wrong += movedBy(before, seenOf(desk), u"the list shrinks"_s + at);
        // One of its KVMs is renamed elsewhere, to a name that is longer: its row says so, and is
        // as high as it was (a row is of one height, whatever it says).
        before = seenOf(desk);
        core::OwnKvm renamed = desk.own->wall().kvms().constFirst();
        renamed.name = u"The first KVM of this wall, by another name"_s;
        QVERIFY(desk.own->wall().change(renamed.id, renamed, std::nullopt).ok());
        QTRY_VERIFY(rowSays(rowOfKvm(desk, ids.at(0)), renamed.name));
        wrong += movedBy(before, seenOf(desk), u"a row changes"_s + at);
        // The wall's order is another.
        before = seenOf(desk);
        bool arranged = false;
        desk.own->wall().arrange({ids.at(2), ids.at(0)},
                                 [&arranged](const base::Result<QStringList>&) { arranged = true; });
        QTRY_VERIFY(arranged);
        wrong += movedBy(before, seenOf(desk), u"the wall's order changes"_s + at);

        // General: Windows does not take a change, and the line that says so comes; and goes again.
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        desk.start.refuses = true;
        QVERIFY(bringIntoSight(desk, u"startsWithWindows"_s));
        before = seenOf(desk);
        clickOn(desk.window, theOne(desk.window, u"startsWithWindows"_s));
        QTRY_VERIFY(theOne(desk.window, u"settingsProblem"_s) != nullptr);
        wrong += movedBy(before, seenOf(desk), u"what went wrong comes"_s + at);
        desk.start.refuses = false;
        before = seenOf(desk);
        clickOn(desk.window, theOne(desk.window, u"startsWithWindows"_s));
        QTRY_VERIFY(theOne(desk.window, u"settingsProblem"_s) == nullptr);
        wrong += movedBy(before, seenOf(desk), u"what went wrong goes"_s + at);
        // A tick, and a choice of the look.
        QVERIFY(bringIntoSight(desk, u"staysInTray"_s));
        before = seenOf(desk);
        clickOn(desk.window, theOne(desk.window, u"staysInTray"_s));
        QTRY_VERIFY(desk.ownSettings.staysInTray());
        wrong += movedBy(before, seenOf(desk), u"a tick"_s + at);
        before = seenOf(desk);
        desk.look.choose(u"system"_s);
        wrong += movedBy(before, seenOf(desk), u"another look is chosen"_s + at);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- asking --------------------------------------------------------------------------------------

    // (No section of today's asks for what it shows: whether a KVM's password is kept is in its row
    // from the start. The shape has it for the sections that will, as the servers' will.)
    void aSectionThatAsksSaysSoAfterTheLastOfItAndWhatWentWrongInItsOwnPlace_data()
    {
        // How wide the section is: its short sentence beside its name, or under it where there is no
        // room beside (in two lines).
        QTest::addColumn<int>("width");
        QTest::newRow("beside") << 800;
        QTest::newRow("under") << 420;
    }
    void aSectionThatAsksSaysSoAfterTheLastOfItAndWhatWentWrongInItsOwnPlace()
    {
        QFETCH(int, width);
        Desk desk;
        QVERIFY(desk.load() != nullptr);
        const std::unique_ptr<QQuickItem> section = sectionOf(desk, R"(
    about: "What this section is and what it holds, said in one short sentence of its own."
    asking: "Asking for what this shows…"

    Block {
        title: "A block"

        Line {
            label: "A line"

            Button {
                objectName: "plantedButton"
                text: "A button"
            }
        }
    }
)");
        QVERIFY(section != nullptr);
        section->setWidth(width);
        aFrame(desk.window);
        const auto said = [&section] {
            QList<const QQuickItem*> found;
            named(section.get(), u"sectionSaid"_s, found);
            return found.size() == 1 && isSeen(found.constFirst()) ? found.constFirst() : nullptr;
        };
        QVERIFY(said() != nullptr);
        QCOMPARE(said()->property("text").toString(), u"Asking for what this shows…"_s);
        QCOMPARE(said()->property("tone").toString(), QString());
        // After the last of what it shows, and under no button.
        QList<const QQuickItem*> buttons;
        named(section.get(), u"plantedButton"_s, buttons);
        QCOMPARE(buttons.size(), 1);
        QVERIFY(placeOf(said()).top() > placeOf(buttons.constFirst()).bottom());
        const QRectF button = placeOf(buttons.constFirst());
        // What went wrong where it failed takes the place of what was asked, in the colour of a
        // problem; and nothing above it moves.
        QVERIFY(section->setProperty("problem", u"It could not be asked."_s));
        aFrame(desk.window);
        QCOMPARE(said()->property("text").toString(), u"It could not be asked."_s);
        QCOMPARE(said()->property("tone").toString(), u"bad"_s);
        QCOMPARE(placeOf(buttons.constFirst()), button);
        // And it goes, once nothing is asked; nothing above it moves for that either.
        QVERIFY(section->setProperty("problem", QString()));
        QVERIFY(section->setProperty("asking", QString()));
        aFrame(desk.window);
        QVERIFY(said() == nullptr);
        QCOMPARE(placeOf(buttons.constFirst()), button);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- rows of one height ---------------------------------------------------------------------------

    void theRowsAreOfOneHeightWhateverComesLateAndHoweverLongANameIs_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("1280x800") << QSize(1280, 800);
        QTest::newRow("640x400") << QSize(640, 400);
    }
    void theRowsAreOfOneHeightWhateverComesLateAndHoweverLongANameIs()
    {
        QFETCH(QSize, size);
        const QString at = u", at %1 by %2"_s.arg(size.width()).arg(size.height());
        Desk desk;
        // Two that are one device, each put on the wall without a password, above two others.
        const QStringList ids = desk.put({{u"https://127.0.0.1:21301"_s, u"Rack 1"_s, 'a', QString()},
                                          {u"https://127.0.0.1:21302"_s, u"Rack 1 again"_s, 'a', QString()},
                                          {u"http://127.0.0.1:21303"_s, u"NAS"_s},
                                          {u"http://127.0.0.1:21304"_s, u"Lab"_s},
                                          {u"https://127.0.0.1:21305"_s, u"Five"_s, 'f'}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 5);
        // That no password is kept for the two is said from the start: nothing of it comes late.
        QVERIFY(rowHasLine(rowOfKvm(desk, ids.at(0)), u"No password saved"_s));
        QVERIFY(rowHasLine(rowOfKvm(desk, ids.at(1)), u"No password saved"_s));
        QVERIFY(rowHasLine(rowOfKvm(desk, ids.at(1)), u"Same device as \"Rack 1\""_s));
        QStringList wrong = rowsOfOneHeight(desk, u"at the start"_s + at);
        const auto kvmOf = [&desk](const QString& id) {
            const QList<core::OwnKvm>& all = desk.own->wall().kvms();
            const auto found = std::ranges::find_if(all, [&id](const core::OwnKvm& one) { return one.id == id; });
            return found == all.end() ? core::OwnKvm{} : *found;
        };

        // Another window gives the second its password: the line that said none is kept goes. No
        // row is lower for it, and none moves.
        Seen before = seenOf(desk);
        QVERIFY(desk.own->wall().change(ids.at(1), kvmOf(ids.at(1)), u"secret"_s).ok());
        QTRY_VERIFY(!rowHasLine(rowOfKvm(desk, ids.at(1)), u"No password saved"_s));
        wrong += movedBy(before, seenOf(desk), u"a password is given"_s + at);
        wrong += rowsOfOneHeight(desk, u"after a password is given"_s + at);

        // Another window names one of them by a name of five lines: its row is as high as it was,
        // and so is every row under it.
        before = seenOf(desk);
        core::OwnKvm nas = kvmOf(ids.at(2));
        nas.name = u"The rack in the second server room, top shelf, left of the door, the one with the blue "
                   "cable, which was moved there"_s;
        QVERIFY(desk.own->wall().change(nas.id, nas, std::nullopt).ok());
        QTRY_VERIFY(rowSays(rowOfKvm(desk, ids.at(2)), nas.name));
        wrong += movedBy(before, seenOf(desk), u"a name of five lines"_s + at);
        wrong += rowsOfOneHeight(desk, u"after a name of five lines"_s + at);

        // Another window adds the same device as the last: that one says so under its name, and
        // the new one comes last.
        before = seenOf(desk);
        core::OwnKvm again;
        again.address = u"https://127.0.0.1:21306"_s;
        again.name = u"Five again"_s;
        again.knownByCertificate = true;
        again.certificate = QByteArray(32, 'f');
        QVERIFY(desk.own->wall().add(again, u"secret"_s).ok());
        QTRY_COMPARE(kvmRows(desk).size(), 6);
        QTRY_VERIFY(rowHasLine(rowOfKvm(desk, ids.at(4)), u"Same device as \"Five again\""_s));
        wrong += movedBy(before, seenOf(desk), u"one is made a twin"_s + at);
        wrong += rowsOfOneHeight(desk, u"after one is made a twin"_s + at);

        // And one of the first two is taken off elsewhere: its row says so in the place of its two
        // lines, and the other is the same device as nothing any more.
        before = seenOf(desk);
        QVERIFY(desk.own->wall().remove(ids.at(1)).ok());
        QTRY_VERIFY(rowHasLine(rowOfKvm(desk, ids.at(1)), u"Taken off this wall"_s));
        QVERIFY(rowHasLine(rowOfKvm(desk, ids.at(0)), u"No password saved"_s));
        QTRY_VERIFY(!rowHasLine(rowOfKvm(desk, ids.at(0)), u"Same device as \"Rack 1 again\""_s));
        wrong += movedBy(before, seenOf(desk), u"one is taken off"_s + at);
        wrong += rowsOfOneHeight(desk, u"after one is taken off"_s + at);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));

        // A name that is longer than its place is cut there, and is whole in what the row says to
        // whoever asks it (its tooltip, and its name for a reader of the screen).
        const QQuickItem* named = rowOfKvm(desk, ids.at(2));
        QVERIFY(named != nullptr);
        QVERIFY(named->property("whole").toString().contains(nas.name));
        QVERIFY(named->property("whole").toString().contains(u"127.0.0.1:21303"_s));
        const QQuickItem* nameText = saying(named, nas.name);
        QVERIFY(nameText != nullptr);
        QVERIFY(nameText->property("truncated").toBool());
        // Whoever rests the pointer on it is shown it whole, under it; and no longer once the
        // pointer has gone. (In the wide window, where the row is in sight as the list opens.)
        if (size.width() >= 800) {
            const QQuickItem* list = theOne(desk.window, u"kvmList"_s);
            QVERIFY(list != nullptr);
            const QObject* tip = list->findChild<QObject*>(u"listedTip"_s);
            QVERIFY(tip != nullptr);
            QVERIFY(!tip->property("opened").toBool());
            QTest::mouseMove(desk.window, placeOf(nameText).center().toPoint());
            QTRY_VERIFY(tip->property("opened").toBool());
            QCOMPARE(tip->property("text").toString(), named->property("whole").toString());
            keepPictureOf(desk.window, u"settings-kvms-whole"_s);
            QTest::mouseMove(desk.window, QPoint(4, 4));
            QTRY_VERIFY(!tip->property("opened").toBool());
        }
        keepPictureOf(desk.window, u"settings-kvms-late-long-twins-%1"_s.arg(size.width()));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- what every section is made of, held strictly ----------------------------------------------

    void theWalkFindsWhatIsLaidOutByHand()
    {
        Desk desk;
        QVERIFY(desk.load() != nullptr);
        // What a section is made of where it is made right: nothing is found in it.
        const std::unique_ptr<QQuickItem> right = sectionOf(desk, R"(
    Block {
        title: "A block"

        Line {
            label: "A line"
            hint: "A sentence under it"
            value: "What is so"
        }
        Line {
            label: "A line with a tick"

            Check {
            }
        }
    }
    Footnote {
        text: "Said after the last"
    }
)");
        QVERIFY(right != nullptr);
        QCOMPARE(laidOutOtherwise(right.get()), QStringList());
        // And each of these is found.
        const QList<std::pair<QString, QByteArray>> planted{
            {u"a text placed by hand in a block's frame"_s, R"(
    Block {
        title: "A block"

        Line {
            label: "A line"
        }
        Text {
            textFormat: Text.PlainText
            x: 40
            text: "Laid out by hand"
        }
    }
)"},
            {u"a line with its own place, in an item of the section's own"_s, R"(
    Block {
        title: "A block"

        Item {
            width: parent.width
            height: 70

            Line {
                x: 12
                y: 6
                width: 300
                label: "A line in an item"
            }
        }
    }
)"},
            {u"a text of its own in a line"_s, R"(
    Block {
        Line {
            label: "A line"

            Text {
                textFormat: Text.PlainText
                text: "Laid out by hand"
            }
        }
    }
)"},
            {u"a button in an item of its own in a line"_s, R"(
    Block {
        Line {
            label: "A line"

            Item {
                width: 80
                height: 30

                Button {
                    x: 4
                    text: "By hand"
                }
            }
        }
    }
)"},
            {u"a line moved in its frame"_s, R"(
    Block {
        Line {
            x: 30
            label: "A line"
        }
    }
)"},
            // What takes a press and is none of the shared controls, and what is drawn by hand where
            // a line or a row has what one does: found whether it is seen while it is walked or not.
            {u"a mouse area working as a button in a line"_s, R"(
    Block {
        Line {
            label: "A line"

            MouseArea {
                width: 80
                height: 30
                onClicked: parent.visible = true
            }
        }
    }
)"},
            {u"a drawn rectangle in a line's right side"_s, R"(
    Block {
        Line {
            label: "A line"

            Rectangle {
                width: 80
                height: 30
                color: "red"
            }
        }
    }
)"},
            {u"a button in a block that is not seen while it is walked"_s, R"(
    Block {
        Line {
            label: "A line"
        }
        Button {
            visible: false
            text: "Hidden"
        }
    }
)"},
            {u"a rectangle with a mouse area in a row's right side"_s, R"(
    Listed {
        id: planted

        columns: ["What"]
        model: 1

        delegate: ListRow {
            of: planted
            cells: [
                {
                    "text": "A row"
                }
            ]

            Rectangle {
                width: 60
                height: 30

                MouseArea {
                    anchors.fill: parent
                }
            }
        }
    }
)"},
            // What says of itself that it is one of the program's own controls and is not one, and
            // what takes a press and was put into one of them: found by what each is, and by where
            // its parts are written.
            {u"a rectangle in a line's right side that says it is a shared control, with a mouse area in it"_s, R"(
    Block {
        Line {
            label: "A line"

            Rectangle {
                readonly property bool sharedControl: true

                width: 80
                height: 30

                MouseArea {
                    anchors.fill: parent
                }
            }
        }
    }
)"},
            {u"a mouse area put into a button in a line"_s, R"(
    Block {
        Line {
            label: "A line"

            Button {
                text: "A button"

                MouseArea {
                    anchors.fill: parent
                }
            }
        }
    }
)"},
            {u"a mouse area in an item put into a button, in a row's right side"_s, R"(
    Listed {
        id: planted

        columns: ["What"]
        model: 1

        delegate: ListRow {
            of: planted
            cells: [
                {
                    "text": "A row"
                }
            ]

            Button {
                text: "Edit"

                Item {
                    width: 40
                    height: 20

                    MouseArea {
                        anchors.fill: parent
                    }
                }
            }
        }
    }
)"},
            {u"a tick put into a button in a line"_s, R"(
    Block {
        Line {
            label: "A line"

            Button {
                text: "A button"

                Check {
                }
            }
        }
    }
)"}};
        QStringList unfound;
        for (const auto& [what, qml] : planted) {
            const std::unique_ptr<QQuickItem> section = sectionOf(desk, qml);
            QVERIFY2(section != nullptr, qPrintable(desk.complaints.join(u'\n')));
            // (A list makes its rows a turn of the loop after it is: there is one to walk.)
            if (qml.contains("ListRow")) {
                QTRY_VERIFY(shapesOf(section.get(), u"row"_s) > 0);
            }
            if (laidOutOtherwise(section.get()).isEmpty()) {
                unfound.append(what);
            }
        }
        QVERIFY2(unfound.isEmpty(), qPrintable(u"not found: "_s + unfound.join(u"; "_s)));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- the keyboard after a removal, and the question that follows the wall ---------------------

    void afterARemovalTheKeyboardIsAtTheNextRowOrAtTheSectionsEntry()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"http://127.0.0.1:21401"_s, u"One"_s},
                                          {u"http://127.0.0.1:21402"_s, u"Two"_s},
                                          {u"http://127.0.0.1:21403"_s, u"Three"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 3);
        // The second is taken off with its question: the keyboard is at the next row's button.
        const auto removeWithItsQuestion = [&desk](const QString& id) {
            clickOn(desk.window, inRow(rowOfKvm(desk, id), u"kvmRemove"_s));
            if (!QTest::qWaitFor([&] { return theOne(desk.window, u"removeYes"_s) != nullptr; }, 20000)) {
                return false;
            }
            clickOn(desk.window, theOne(desk.window, u"removeYes"_s));
            return QTest::qWaitFor(
                [&] { return !desk.isOpen(u"editKvm"_s) && rowSays(rowOfKvm(desk, id), u"Taken off this wall"_s); },
                20000);
        };
        QVERIFY(removeWithItsQuestion(ids.at(1)));
        QTRY_COMPARE(keyboardAt(desk), inRow(rowOfKvm(desk, ids.at(2)), u"kvmEdit"_s));
        // The last: there is no row after it with a button that takes a press. The keyboard is at
        // the section's entry in the list, where the arrows go on from.
        QVERIFY(removeWithItsQuestion(ids.at(2)));
        QTRY_COMPARE(keyboardAt(desk), entryOf(desk, u"kvms"_s));
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTRY_COMPARE(shownName(desk), u"settingsActions"_s);
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTRY_COMPARE(shownName(desk), u"settingsFiles"_s);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theQuestionBeforeARemovalFollowsTheWall()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"http://127.0.0.1:21501"_s, u"One"_s},
                                          {u"http://127.0.0.1:21502"_s, u"Two"_s},
                                          {u"http://127.0.0.1:21503"_s, u"Three"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 3);
        // Renamed elsewhere while its question stands: the question says what it is called now.
        clickOn(desk.window, inRow(rowOfKvm(desk, ids.at(0)), u"kvmRemove"_s));
        QTRY_VERIFY(theOne(desk.window, u"removeYes"_s) != nullptr);
        QCOMPARE(desk.box(u"editKvm"_s)->property("title").toString(), u"Remove \"One\"?"_s);
        core::OwnKvm one = desk.own->wall().kvms().constFirst();
        one.name = u"The first"_s;
        QVERIFY(desk.own->wall().change(one.id, one, std::nullopt).ok());
        QTRY_COMPARE(desk.box(u"editKvm"_s)->property("title").toString(), u"Remove \"The first\"?"_s);
        clickOn(desk.window, theOne(desk.window, u"removeNo"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));

        // Taken off elsewhere while its question stands: the question stays, says so in its own
        // place, and nothing is asked of the wall; its way out closes it. (Held still around that
        // moment: theQuestionStaysWhereItIsWhenItsKvmIsTakenOffElsewhere.)
        clickOn(desk.window, inRow(rowOfKvm(desk, ids.at(1)), u"kvmRemove"_s));
        QTRY_VERIFY(theOne(desk.window, u"removeYes"_s) != nullptr);
        QVERIFY(desk.own->wall().remove(ids.at(1)).ok());
        QTRY_VERIFY(boxSaying(desk, u"\"Two\" was taken off this wall meanwhile."_s) != nullptr);
        QVERIFY(desk.isOpen(u"editKvm"_s));
        QVERIFY(!says(desk.window, u"Unknown device"_s));
        QCOMPARE(desk.own->wall().kvms().size(), 2);
        clickOn(desk.window, theOne(desk.window, u"removeNo"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(rowSays(rowOfKvm(desk, ids.at(1)), u"Taken off this wall"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theQuestionStaysWhereItIsWhenItsKvmIsTakenOffElsewhere_data()
    {
        QTest::addColumn<QString>("look");
        QTest::addColumn<QSize>("size");
        for (const QString& look : {u"dark"_s, u"light"_s}) {
            for (const QSize& size : {QSize(640, 400), QSize(800, 500), QSize(860, 540), QSize(1280, 800)}) {
                QTest::addRow("%s, %dx%d", qPrintable(look), size.width(), size.height()) << look << size;
            }
        }
    }
    void theQuestionStaysWhereItIsWhenItsKvmIsTakenOffElsewhere()
    {
        // Thirty KVMs: rows of the list stand under the question, wherever the window is. The first is
        // called by a name of 72 signs.
        QFETCH(QString, look);
        QFETCH(QSize, size);
        const QString at = u", %1, at %2 by %3"_s.arg(look).arg(size.width()).arg(size.height());
        const QString name = u"The rack in the second server room on the top shelf, left of the door 01"_s;
        QCOMPARE(name.size(), 72);
        Desk desk;
        QList<Put> many;
        for (int number = 1; number <= 30; ++number) {
            many.append({u"http://127.0.0.1:%1"_s.arg(22000 + number), number == 1 ? name : u"Rack %1"_s.arg(number)});
        }
        const QStringList ids = desk.put(many);
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY2(lookIs(desk, look), qPrintable(look));
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(theOne(desk.window, u"kvmList"_s)->property("count").toInt(), 30);
        const QColor warn = desk.engine.singletonInstance<QObject*>("Qkvm", "Theme")->property("warn").value<QColor>();
        QStringList wrong;
        // The question of the first row, and of one further down (under the question's buttons where
        // the window is about 800 wide); each taken off elsewhere while it stands.
        for (const int row : {0, 4}) {
            const QString of = u" (row %1)"_s.arg(row + 1) + at;
            QVERIFY(clickInRow(desk, ids.at(row), u"kvmRemove"_s));
            QTRY_VERIFY(theOne(desk.window, u"removeYes"_s) != nullptr);
            aFrame(desk.window);
            const Seen question = buttonsOfTheEditBox(desk);
            const Seen settings = seenOf(desk);
            const QQuickItem* warning = theOne(desk.window, u"removeWarning"_s);
            QVERIFY(warning != nullptr);
            const QRectF warned = placeOf(warning);
            QVERIFY(desk.own->wall().remove(ids.at(row)).ok());
            const QString gone = (row == 0 ? u"\""_s + name + u"\""_s : u"\"Rack %1\""_s.arg(row + 1)) +
                                 u" was taken off this wall meanwhile."_s;
            QTRY_VERIFY2(boxSaying(desk, gone) != nullptr, qPrintable(u"not said in the question"_s + of));
            aFrame(desk.window);
            // The question stays, as large as it was and where it was: nothing that takes a press
            // moved in it, nor came to stand where another stood; the settings under it are as they
            // were.
            QVERIFY(desk.isOpen(u"editKvm"_s));
            wrong += movedBy(question, buttonsOfTheEditBox(desk), u"its KVM is taken off"_s + of);
            wrong += movedBy(settings, seenOf(desk), u"under the question"_s + of);
            // That it was taken off this wall stands where its words stood, in the colour of a
            // warning, whole (a long name too: the sentence is never cut).
            const QQuickItem* said = boxSaying(desk, gone);
            QCOMPARE(placeOf(said).topLeft(), warned.topLeft());
            QCOMPARE(said->property("color").value<QColor>(), warn);
            QVERIFY2(!said->property("truncated").toBool(), qPrintable(u"cut"_s + of));
            QVERIFY2(seenPlaceOf(said) == placeOf(said), qPrintable(u"not whole in sight"_s + of));
            QVERIFY(theOne(desk.window, u"removeWarning"_s) == nullptr);
            QVERIFY(!says(desk.window, u"Unknown device"_s));
            // Its Remove takes nothing; its Cancel has the keyboard, and closes it.
            const Seen after = buttonsOfTheEditBox(desk);
            QVERIFY(after.controls.contains(u"removeYes"_s) && !after.live.contains(u"removeYes"_s));
            QVERIFY(after.live.contains(u"removeNo"_s));
            QTRY_COMPARE(keyboardAt(desk), theOne(desk.window, u"removeNo"_s));
            if (row == 0) {
                keepPictureOf(desk.window, u"settings-question-gone-%1-%2"_s.arg(look).arg(size.width()));
            }
            clickOn(desk.window, theOne(desk.window, u"removeYes"_s));
            QTest::qWait(100);
            QVERIFY(desk.isOpen(u"editKvm"_s));
            QCOMPARE(desk.own->wall().kvms().size(), row == 0 ? 29 : 28);
            clickOn(desk.window, theOne(desk.window, u"removeNo"_s));
            QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
            QVERIFY(desk.isOpen(u"settingsBox"_s));
            QVERIFY(rowSays(rowOfKvm(desk, ids.at(row)), u"Taken off this wall"_s));
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- the box that changes a KVM, where that KVM is taken off elsewhere ---------------------------

    void theEditBoxStaysWhereItIsWhenItsKvmIsTakenOffElsewhere_data()
    {
        QTest::addColumn<QString>("look");
        QTest::addColumn<QSize>("size");
        QTest::newRow("dark, 1280x800") << u"dark"_s << QSize(1280, 800);
        QTest::newRow("light, 1280x800") << u"light"_s << QSize(1280, 800);
        QTest::newRow("dark, 640x400") << u"dark"_s << QSize(640, 400);
        QTest::newRow("light, 640x400") << u"light"_s << QSize(640, 400);
    }
    void theEditBoxStaysWhereItIsWhenItsKvmIsTakenOffElsewhere()
    {
        QFETCH(QString, look);
        QFETCH(QSize, size);
        const QString at = u", %1, at %2 by %3"_s.arg(look).arg(size.width()).arg(size.height());
        const QString of = u"-%1-%2"_s.arg(look).arg(size.width());
        Desk desk;
        // At an address of this PC's own, something that takes the line and never answers: a saving
        // that asks there is under way until it gives up.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        const QStringList ids = desk.put({{u"http://127.0.0.1:22101"_s, u"One"_s},
                                          {u"http://127.0.0.1:22102"_s, u"Two"_s},
                                          {u"http://127.0.0.1:22103"_s, u"Three"_s},
                                          {u"http://127.0.0.1:22104"_s, u"Four"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY2(lookIs(desk, look), qPrintable(look));
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 4);
        const QColor warn = desk.engine.singletonInstance<QObject*>("Qkvm", "Theme")->property("warn").value<QColor>();
        // What the box says, in what it holds (not in its title).
        const auto boxSays = [&desk](const QString& text) {
            const QObject* box = desk.box(u"editKvm"_s);
            return box != nullptr ? saying(box->property("contentItem").value<QQuickItem*>(), text) : nullptr;
        };
        // Around a change: the box where it was and as large, its buttons where they were and none
        // where another stood, and each of its lines to type into where it stood (less of them may be
        // in sight: what holds them may be cut shorter).
        Seen before;
        QHash<QString, QRectF> linesWere;
        const auto lookBefore = [&] {
            before = buttonsOfTheEditBox(desk);
            linesWere = linesOfTheEditBox(desk);
        };
        const auto heldStill = [&](const QString& when) {
            return movedBy(before, buttonsOfTheEditBox(desk), when + at) +
                   linesMoved(linesWere, linesOfTheEditBox(desk), when + at);
        };

        // The box that changes the second, from the list, at its lines; something typed into them.
        QVERIFY(clickInRow(desk, ids.at(1), u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(theOne(desk.window, u"editSave"_s) != nullptr);
        typeKeys(desk.window, u"xy"_s);
        lookBefore();
        // Taken off elsewhere: there is nothing to change, to take off or to say yes to any more. The
        // box stays where it is, and says so in its own place, in the colour of a warning; nothing
        // says the core's "Unknown device".
        QVERIFY(desk.own->wall().remove(ids.at(1)).ok());
        const QString twoGone = u"\"Two\" was taken off this wall meanwhile."_s;
        QTRY_VERIFY(boxSays(twoGone) != nullptr);
        QVERIFY(desk.isOpen(u"editKvm"_s));
        QVERIFY(isSeen(boxSays(twoGone)));
        QCOMPARE(boxSays(twoGone)->property("color").value<QColor>(), warn);
        QVERIFY(!says(desk.window, u"Unknown device"_s));
        QStringList wrong = heldStill(u"its KVM is taken off"_s);
        // Save takes nothing, nor do Remove and the lines; the way out does, and has the keyboard.
        QTRY_COMPARE(keyboardAt(desk), theOne(desk.window, u"editCancel"_s));
        const Seen gone = buttonsOfTheEditBox(desk);
        QVERIFY(gone.controls.contains(u"editSave"_s) && !gone.live.contains(u"editSave"_s));
        QVERIFY(gone.controls.contains(u"editRemove"_s) && !gone.live.contains(u"editRemove"_s));
        QVERIFY(gone.live.contains(u"editCancel"_s));
        QVERIFY(!theOne(desk.window, u"editName"_s)->isEnabled());
        clickOn(desk.window, theOne(desk.window, u"editSave"_s));
        QTest::qWait(100);
        QVERIFY(desk.isOpen(u"editKvm"_s));
        QVERIFY(!desk.editing.busy());
        QVERIFY(!says(desk.window, u"Unknown device"_s));
        QCOMPARE(desk.own->wall().kvms().size(), 3);
        keepPictureOf(desk.window, u"settings-edit-gone"_s + of);
        clickOn(desk.window, theOne(desk.window, u"editCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(rowSays(rowOfKvm(desk, ids.at(1)), u"Taken off this wall"_s));

        // A saving that is under way when its KVM is taken off: it ends with that same word, in that
        // place, and not with what came of the saving.
        QVERIFY(clickInRow(desk, ids.at(2), u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(theOne(desk.window, u"editSave"_s) != nullptr);
        // (Another address, where what answers says nothing, and a password for it there.)
        QVERIFY(typeInto(desk, u"editAddress"_s, u"http://127.0.0.1:%1"_s.arg(silent.serverPort())));
        QVERIFY(typeInto(desk, u"editPassword"_s, u"secret"_s));
        clickOn(desk.window, theOne(desk.window, u"editSave"_s));
        QTRY_VERIFY(desk.editing.busy());
        lookBefore();
        QVERIFY(desk.own->wall().remove(ids.at(2)).ok());
        QTest::qWait(100);
        aFrame(desk.window);
        QVERIFY(desk.editing.busy());
        wrong += heldStill(u"its KVM is taken off while it saves"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!desk.editing.busy(), 20000);
        const QString threeGone = u"\"Three\" was taken off this wall meanwhile."_s;
        QTRY_VERIFY(boxSays(threeGone) != nullptr);
        QVERIFY(isSeen(boxSays(threeGone)));
        QVERIFY(theOne(desk.window, u"editProblem"_s) == nullptr);
        QVERIFY(!says(desk.window, u"Unknown device"_s));
        wrong += heldStill(u"the saving ends"_s);
        QVERIFY(!buttonsOfTheEditBox(desk).live.contains(u"editSave"_s));
        keepPictureOf(desk.window, u"settings-edit-gone-while-saving"_s + of);
        clickOn(desk.window, theOne(desk.window, u"editCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));

        // The box opened elsewhere (a card's own way to it), with the settings closed: the same.
        desk.editing.openFor(ids.at(3), false);
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(theOne(desk.window, u"editSave"_s) != nullptr);
        lookBefore();
        QVERIFY(desk.own->wall().remove(ids.at(3)).ok());
        QTRY_VERIFY(boxSays(u"\"Four\" was taken off this wall meanwhile."_s) != nullptr);
        QVERIFY(desk.isOpen(u"editKvm"_s));
        QVERIFY(!says(desk.window, u"Unknown device"_s));
        wrong += heldStill(u"its KVM is taken off, from a card"_s);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theEditBoxFitsASmallerWindowAfterItsKvmWent()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"http://127.0.0.1:22701"_s, u"One"_s},
                                          {u"http://127.0.0.1:22702"_s, u"Two"_s},
                                          {u"http://127.0.0.1:22703"_s, u"Three"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load(1280, 800) != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 3);
        QVERIFY(clickInRow(desk, ids.at(1), u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(theOne(desk.window, u"editSave"_s) != nullptr);
        QVERIFY(desk.own->wall().remove(ids.at(1)).ok());
        const QString gone = u"\"Two\" was taken off this wall meanwhile."_s;
        QTRY_VERIFY(boxSaying(desk, gone) != nullptr);
        // Where each button of the box stands in it: from its left edge, and from its top or its
        // bottom, whichever it is nearer.
        const auto inTheBox = [](const Seen& seen) {
            QHash<QString, QPointF> at;
            for (auto one = seen.controls.cbegin(); one != seen.controls.cend(); ++one) {
                const QRectF is = one.value();
                const bool high = is.center().y() < seen.box.center().y();
                at.insert(one.key(), QPointF(is.x() - seen.box.x(),
                                             high ? is.y() - seen.box.y() : is.bottom() - seen.box.bottom()));
            }
            return at;
        };
        const Seen wide = buttonsOfTheEditBox(desk);
        const QHash<QString, QPointF> wasInTheBox = inTheBox(wide);
        QStringList wrong;
        // The window is made smaller, and smaller again: the box fits it, and every one of its buttons
        // is in sight, whole, where it stood in the box; and so is what it says of its KVM.
        for (const QSize size : {QSize(1024, 700), QSize(800, 500), QSize(640, 400)}) {
            const QString at = u", at %1 by %2"_s.arg(size.width()).arg(size.height());
            desk.window->resize(size);
            QTRY_COMPARE(desk.window->size(), size);
            aFrame(desk.window);
            const Seen now = buttonsOfTheEditBox(desk);
            const QRectF window(0, 0, size.width(), size.height());
            if (!window.contains(now.box)) {
                wrong.append(u"the box stands beyond the window"_s + at);
            }
            const QHash<QString, QPointF> isInTheBox = inTheBox(now);
            for (auto was = wide.controls.cbegin(); was != wide.controls.cend(); ++was) {
                const QRectF is = now.controls.value(was.key());
                if (!now.controls.contains(was.key()) || !window.contains(is) ||
                    qAbs(is.width() - was.value().width()) > 0.5 || qAbs(is.height() - was.value().height()) > 0.5) {
                    wrong.append(u"\"%1\" is not whole in sight"_s.arg(was.key()) + at);
                } else if ((isInTheBox.value(was.key()) - wasInTheBox.value(was.key())).manhattanLength() > 1) {
                    wrong.append(u"\"%1\" stands elsewhere in the box"_s.arg(was.key()) + at);
                }
            }
            const QQuickItem* said = boxSaying(desk, gone);
            if (said == nullptr || !window.contains(seenPlaceOf(said)) || seenPlaceOf(said) != placeOf(said)) {
                wrong.append(u"what it says of its KVM is not whole in sight"_s + at);
            }
            keepPictureOf(desk.window, u"settings-edit-gone-then-%1"_s.arg(size.width()));
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aBoxWhoseKvmGoesAtOnceIsAsLargeAsItIsWhereItStays_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x400") << QSize(640, 400);
        QTest::newRow("1280x800") << QSize(1280, 800);
    }
    void aBoxWhoseKvmGoesAtOnceIsAsLargeAsItIsWhereItStays()
    {
        // The box holds the height it has for what it shows, also where the KVM goes in the very
        // moment the box is opened, before it was ever drawn so: not the height of what it showed
        // the time before.
        QFETCH(QSize, size);
        const QString at = u", at %1 by %2"_s.arg(size.width()).arg(size.height());
        Desk desk;
        const QStringList ids = desk.put({{u"http://127.0.0.1:23201"_s, u"One"_s},
                                          {u"http://127.0.0.1:23202"_s, u"Two"_s},
                                          {u"http://127.0.0.1:23203"_s, u"Three"_s},
                                          {u"http://127.0.0.1:23204"_s, u"Four"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 4);
        const auto boxHigh = [&desk] {
            aFrame(desk.window);
            return desk.box(u"editKvm"_s)->property("height").toReal();
        };
        // How high each is, with its KVM there: the question, and the lines.
        QVERIFY(clickInRow(desk, ids.at(3), u"kvmRemove"_s));
        QTRY_VERIFY(theOne(desk.window, u"removeYes"_s) != nullptr);
        const qreal question = boxHigh();
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY(clickInRow(desk, ids.at(3), u"kvmEdit"_s));
        QTRY_VERIFY(theOne(desk.window, u"editSave"_s) != nullptr);
        const qreal lines = boxHigh();
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY(question < lines);
        // The question, its KVM taken off the moment it is there (the lines were shown last).
        QVERIFY(clickInRow(desk, ids.at(0), u"kvmRemove"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QVERIFY(desk.own->wall().remove(ids.at(0)).ok());
        QTRY_VERIFY(boxSaying(desk, u"\"One\" was taken off this wall meanwhile."_s) != nullptr);
        QCOMPARE(boxHigh(), question);
        keepPictureOf(desk.window, u"settings-question-gone-at-once-%1"_s.arg(size.width()));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        // The lines, their KVM taken off the moment they are there (the question was shown last).
        QVERIFY(clickInRow(desk, ids.at(1), u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QVERIFY(desk.own->wall().remove(ids.at(1)).ok());
        QTRY_VERIFY(boxSaying(desk, u"\"Two\" was taken off this wall meanwhile."_s) != nullptr);
        QVERIFY2(qAbs(boxHigh() - lines) < 0.5,
                 qPrintable(u"the lines' box is %1 high, and %2 where its KVM stays"_s.arg(boxHigh()).arg(lines) + at));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theWayOutHasTheKeyboardWhereTheKvmOfABoxWent()
    {
        Desk desk;
        const QStringList ids =
            desk.put({{u"https://127.0.0.1:22801"_s, u"One"_s, 'a'}, {u"https://127.0.0.1:22802"_s, u"Two"_s, 'b'}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 2);
        // At its two certificates, as the program shows them once they were read (it is told so as
        // the program tells it): taken off elsewhere, the way out has the keyboard, and closes it.
        QVERIFY(clickInRow(desk, ids.at(0), u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(theOne(desk.window, u"editSave"_s) != nullptr);
        desk.editing.began(u"looking"_s);
        desk.editing.reviewed(QByteArray(32, 'a'), QByteArray(32, 'c'));
        QTRY_VERIFY(theOne(desk.window, u"reviewClose"_s) != nullptr);
        QVERIFY(desk.own->wall().remove(ids.at(0)).ok());
        QTRY_VERIFY(boxSaying(desk, u"\"One\" was taken off this wall meanwhile."_s) != nullptr);
        QTRY_COMPARE(keyboardAt(desk), theOne(desk.window, u"reviewClose"_s));
        QVERIFY(!theOne(desk.window, u"reviewTrust"_s)->isEnabled());
        keepPictureOf(desk.window, u"settings-review-gone"_s);
        QTest::keyClick(desk.window, Qt::Key_Space);
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        // At its lines: Cancel has it, and closes it.
        QVERIFY(clickInRow(desk, ids.at(1), u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(theOne(desk.window, u"editSave"_s) != nullptr);
        QVERIFY(desk.own->wall().remove(ids.at(1)).ok());
        QTRY_VERIFY(boxSaying(desk, u"\"Two\" was taken off this wall meanwhile."_s) != nullptr);
        QTRY_COMPARE(keyboardAt(desk), theOne(desk.window, u"editCancel"_s));
        QTest::keyClick(desk.window, Qt::Key_Space);
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aNameWithNoBreakStaysInsideTheBoxWhenItsKvmGoes_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x400") << QSize(640, 400);
        QTest::newRow("1280x800") << QSize(1280, 800);
    }
    void aNameWithNoBreakStaysInsideTheBoxWhenItsKvmGoes()
    {
        // Names of 78 signs with no blank in them, in capitals: one for the question, one for the box
        // that changes a KVM.
        QFETCH(QSize, size);
        const QString at = u", at %1 by %2"_s.arg(size.width()).arg(size.height());
        const QString first = u"THE_RACK_IN_THE_SECOND_SERVER_ROOM_ON_THE_TOP_SHELF_LEFT_OF_THE_DOOR_NUMBER_01"_s;
        const QString second = u"THE_RACK_IN_THE_SECOND_SERVER_ROOM_ON_THE_TOP_SHELF_LEFT_OF_THE_DOOR_NUMBER_02"_s;
        QCOMPARE(first.size(), 78);
        Desk desk;
        const QStringList ids = desk.put({{u"http://127.0.0.1:22901"_s, first}, {u"http://127.0.0.1:22902"_s, second}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 2);
        const auto inside = [&desk](const QString& gone, const QString& when) {
            QStringList wrong;
            const QQuickItem* said = boxSaying(desk, gone);
            const QObject* box = desk.box(u"editKvm"_s);
            const QRectF boxIs(box->property("x").toReal(), box->property("y").toReal(),
                               box->property("width").toReal(), box->property("height").toReal());
            if (said == nullptr) {
                return QStringList{when + u": not said"_s};
            }
            if (said->property("contentWidth").toReal() > said->width() + 0.5 ||
                said->property("contentHeight").toReal() > said->height() + 0.5) {
                wrong.append(u"%1: drawn %2 by %3 in a place %4 by %5"_s.arg(when)
                                 .arg(said->property("contentWidth").toReal())
                                 .arg(said->property("contentHeight").toReal())
                                 .arg(said->width())
                                 .arg(said->height()));
            }
            if (seenPlaceOf(said) != placeOf(said) || !boxIs.contains(placeOf(said))) {
                wrong.append(when + u": not whole in sight in the box"_s);
            }
            return wrong;
        };
        // The question.
        QVERIFY(clickInRow(desk, ids.at(0), u"kvmRemove"_s));
        QTRY_VERIFY(theOne(desk.window, u"removeYes"_s) != nullptr);
        QVERIFY(desk.own->wall().remove(ids.at(0)).ok());
        const QString firstGone = u"\""_s + first + u"\" was taken off this wall meanwhile."_s;
        QTRY_VERIFY(boxSaying(desk, firstGone) != nullptr);
        aFrame(desk.window);
        QStringList wrong = inside(firstGone, u"the question"_s + at);
        keepPictureOf(desk.window, u"settings-question-gone-joined-%1"_s.arg(size.width()));
        clickOn(desk.window, theOne(desk.window, u"removeNo"_s));
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        // The box that changes the other.
        QVERIFY(clickInRow(desk, ids.at(1), u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(theOne(desk.window, u"editSave"_s) != nullptr);
        QVERIFY(desk.own->wall().remove(ids.at(1)).ok());
        const QString secondGone = u"\""_s + second + u"\" was taken off this wall meanwhile."_s;
        QTRY_VERIFY(boxSaying(desk, secondGone) != nullptr);
        aFrame(desk.window);
        wrong += inside(secondGone, u"the box that changes it"_s + at);
        keepPictureOf(desk.window, u"settings-edit-gone-joined-%1"_s.arg(size.width()));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void cancelAndReviewStandWhereTheyStoodWhenDoneTurnsBackIntoCancel()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"https://127.0.0.1:23101"_s, u"One"_s, 'a'}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 1);
        QVERIFY(clickInRow(desk, ids.at(0), u"kvmEdit"_s));
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(theOne(desk.window, u"editReview"_s) != nullptr);
        // Saved, and the box stays to show what it is known by now (as the program tells it): its
        // way out says "Done".
        ui::EditKvmView::Shown now;
        now.id = desk.editing.machine();
        now.called = desk.editing.called();
        now.inASentence = desk.editing.calledInASentence();
        now.name = desk.editing.name();
        now.address = desk.editing.address();
        now.user = desk.editing.user();
        now.group = desk.editing.group();
        now.certificate = desk.editing.certificate();
        now.https = desk.editing.isHttps();
        desk.editing.began(u"saving"_s);
        desk.editing.savedAndStays(now);
        QTRY_COMPARE(theOne(desk.window, u"editCancel"_s)->property("text").toString(), u"Done"_s);
        aFrame(desk.window);
        const QRectF cancelWas = placeOf(theOne(desk.window, u"editCancel"_s));
        const QRectF reviewWas = placeOf(theOne(desk.window, u"editReview"_s));
        // The next thing asked, and given up: "Cancel" again, where "Done" stood, and Review where
        // it stood. (Side by side: what is said under the lines goes with the saving, and the box
        // is lower by that.)
        desk.editing.began(u"looking"_s);
        desk.editing.gaveUp();
        QTRY_COMPARE(theOne(desk.window, u"editCancel"_s)->property("text").toString(), u"Cancel"_s);
        aFrame(desk.window);
        const QRectF cancelIs = placeOf(theOne(desk.window, u"editCancel"_s));
        const QRectF reviewIs = placeOf(theOne(desk.window, u"editReview"_s));
        QCOMPARE(cancelIs.x(), cancelWas.x());
        QCOMPARE(cancelIs.width(), cancelWas.width());
        QCOMPARE(reviewIs.x(), reviewWas.x());
        QCOMPARE(reviewIs.width(), reviewWas.width());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- the keyboard where its row is made anew, or is far out of sight ----------------------------

    void theKeyboardOnARowsButtonIsThereStillWhenTheWindowComesBack()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"http://127.0.0.1:22201"_s, u"One"_s},
                                          {u"http://127.0.0.1:22202"_s, u"Two"_s},
                                          {u"http://127.0.0.1:22203"_s, u"Three"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 3);
        // The keyboard at the second row's Edit, as Tab brings it there.
        QQuickItem* edit = inRowToWorkOn(desk, ids.at(1), u"kvmEdit"_s);
        QVERIFY(edit != nullptr);
        edit->forceActiveFocus(Qt::TabFocusReason);
        QTRY_COMPARE(keyboardAt(desk), edit);
        const auto outOfSightAndBack = [&desk] {
            desk.window->hide();
            if (!QTest::qWaitFor([&] { return !desk.kvms.isShown(); }, 5000)) {
                return false;
            }
            desk.window->show();
            desk.window->requestActivate();
            return QTest::qWaitForWindowActive(desk.window) &&
                   QTest::qWaitFor([&] { return desk.kvms.isShown(); }, 5000);
        };
        // The window goes out of sight (to the notification area) and comes back: the list is shown
        // anew, as the wall is, which is as it was. The keyboard is where it was.
        QVERIFY(outOfSightAndBack());
        aFrame(desk.window);
        QTRY_COMPARE(keyboardAt(desk), inRow(rowOfKvm(desk, ids.at(1)), u"kvmEdit"_s));
        // (And Tab goes on from there, to its Remove.)
        QTest::keyClick(desk.window, Qt::Key_Tab);
        QTRY_COMPARE(keyboardAt(desk), inRow(rowOfKvm(desk, ids.at(1)), u"kvmRemove"_s));

        // Out of sight once more, and the KVM of its row is taken off elsewhere meanwhile: shown anew,
        // the list is the wall's as it is, and that row is not there. The keyboard is at the section's
        // entry in the list of sections, where the arrows go on from, and not at nothing.
        desk.window->hide();
        QTRY_VERIFY(!desk.kvms.isShown());
        QVERIFY(desk.own->wall().remove(ids.at(1)).ok());
        desk.window->show();
        desk.window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(desk.window));
        QTRY_COMPARE(kvmRows(desk).size(), 2);
        QTRY_COMPARE(keyboardAt(desk), entryOf(desk, u"kvms"_s));
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTRY_COMPARE(shownName(desk), u"settingsActions"_s);
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTRY_COMPARE(shownName(desk), u"settingsFiles"_s);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aRowWithTheKeyboardIsKeptWhenItIsScrolledFarOutOfSight()
    {
        Desk desk;
        QList<Put> many;
        for (int number = 1; number <= 120; ++number) {
            many.append({u"http://127.0.0.1:%1"_s.arg(22300 + number), u"Rack %1"_s.arg(number)});
        }
        const QStringList ids = desk.put(many);
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(theOne(desk.window, u"kvmList"_s)->property("count").toInt(), 120);
        QQuickItem* edit = inRowToWorkOn(desk, ids.at(0), u"kvmEdit"_s);
        QVERIFY(edit != nullptr);
        edit->forceActiveFocus(Qt::TabFocusReason);
        QTRY_COMPARE(keyboardAt(desk), edit);
        // Scrolled 6000 pixels away, as with the wheel: past the rows the list makes around what is
        // in sight. The row with the keyboard is kept, and the keyboard with it.
        QQuickItem* content = theOneToWorkOn(desk.window, u"sectionContent"_s);
        QVERIFY(content != nullptr);
        QVERIFY(content->property("contentHeight").toReal() - content->height() > 6000);
        for (qreal at = 0; at <= 6000; at += 600) {
            content->setProperty("contentY", at);
            aFrame(desk.window);
        }
        QTest::qWait(50);
        aFrame(desk.window);
        QVERIFY(rowOfKvm(desk, ids.at(1)) == nullptr);
        QVERIFY(rowOfKvm(desk, ids.at(0)) != nullptr);
        QCOMPARE(keyboardAt(desk), inRow(rowOfKvm(desk, ids.at(0)), u"kvmEdit"_s));
        const QStringList wrong = rowsInTheirPlaces(desk, u"scrolled away from the keyboard"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        // Tab goes on from it, to its Remove, which is brought into sight.
        QTest::keyClick(desk.window, Qt::Key_Tab);
        QTRY_COMPARE(keyboardAt(desk), inRow(rowOfKvm(desk, ids.at(0)), u"kvmRemove"_s));
        QTRY_VERIFY(content->property("contentY").toReal() < 1000);
        // Once the keyboard has left it and it is far out of sight, it is let go as any other is.
        content->setProperty("contentY", content->property("contentHeight").toReal() - content->height());
        aFrame(desk.window);
        QTRY_VERIFY(inRowToWorkOn(desk, ids.last(), u"kvmEdit"_s) != nullptr);
        QQuickItem* last = inRowToWorkOn(desk, ids.last(), u"kvmEdit"_s);
        last->forceActiveFocus(Qt::TabFocusReason);
        QTRY_COMPARE(keyboardAt(desk), last);
        content->setProperty("contentY", content->property("contentY").toReal() - 100);
        aFrame(desk.window);
        QTRY_VERIFY(rowOfKvm(desk, ids.at(0)) == nullptr);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // The row kept for the keyboard is let go only where the keyboard comes to rest on another item of
    // its section: not while the window has no keyboard (another program is in front), and not while
    // a box that stands over the section has it (the one its Edit opened).
    void aRowKeptForTheKeyboardStaysWhileTheWindowHasNoKeyboard()
    {
        Desk desk;
        const QStringList ids = desk.put(oneHundredAndTwenty(22600));
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QVERIFY(keptFarFromSight(desk, ids.at(0)));
        // Another window is the one in front: this one has the keyboard no longer.
        QQuickWindow other;
        other.resize(320, 200);
        other.show();
        other.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QTRY_VERIFY(desk.window->activeFocusItem() == nullptr);
        QTest::qWait(50);
        aFrame(desk.window);
        // The row is kept meanwhile, and the keyboard is back at its Edit when the window is in front
        // again.
        QVERIFY(rowOfKvm(desk, ids.at(0)) != nullptr);
        desk.window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(desk.window));
        QTRY_VERIFY(rowOfKvm(desk, ids.at(0)) != nullptr &&
                    keyboardAt(desk) == inRow(rowOfKvm(desk, ids.at(0)), u"kvmEdit"_s));
        // And Tab goes on from there, to its Remove.
        QTest::keyClick(desk.window, Qt::Key_Tab);
        QTRY_COMPARE(keyboardAt(desk), inRow(rowOfKvm(desk, ids.at(0)), u"kvmRemove"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aRowKeptForTheKeyboardStaysWhileTheBoxItsEditOpenedIsOpen()
    {
        Desk desk;
        const QStringList ids = desk.put(oneHundredAndTwenty(22800));
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QVERIFY(keptFarFromSight(desk, ids.at(0)));
        // Space on that Edit: its box, for that KVM, over the settings, with the keyboard in it.
        QTest::keyClick(desk.window, Qt::Key_Space);
        QTRY_VERIFY(desk.isOpen(u"editKvm"_s));
        QCOMPARE(desk.editing.machine(), ids.at(0));
        QTRY_VERIFY(keyboardAt(desk) != nullptr && !isInTheSection(keyboardAt(desk)));
        QTest::qWait(50);
        aFrame(desk.window);
        QVERIFY(rowOfKvm(desk, ids.at(0)) != nullptr);
        // Closed: the keyboard is back at the Edit that opened it, and Tab goes on from there.
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"editKvm"_s));
        QTRY_VERIFY(rowOfKvm(desk, ids.at(0)) != nullptr &&
                    keyboardAt(desk) == inRow(rowOfKvm(desk, ids.at(0)), u"kvmEdit"_s));
        QTest::keyClick(desk.window, Qt::Key_Tab);
        QTRY_COMPARE(keyboardAt(desk), inRow(rowOfKvm(desk, ids.at(0)), u"kvmRemove"_s));
        // Far out of sight once more, with the keyboard at that Remove; and then the keyboard comes
        // to rest on another item of the section (the button in its first line): the row is let go,
        // as any other far out of sight is.
        QQuickItem* content = theOneToWorkOn(desk.window, u"sectionContent"_s);
        QVERIFY(content != nullptr);
        content->setProperty("contentY", 6000);
        aFrame(desk.window);
        QTest::qWait(50);
        aFrame(desk.window);
        QVERIFY(rowOfKvm(desk, ids.at(0)) != nullptr);
        QVERIFY(rowOfKvm(desk, ids.at(1)) == nullptr);
        QQuickItem* add = theOneToWorkOn(desk.window, u"settingsAddKvms"_s);
        QVERIFY(add != nullptr);
        add->forceActiveFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(keyboardAt(desk), add);
        QTRY_VERIFY(rowOfKvm(desk, ids.at(0)) == nullptr);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void removeIsRedInTheRowAndInItsQuestion()
    {
        Desk desk;
        const QStringList ids = desk.put({{u"http://127.0.0.1:21601"_s, u"One"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 1);
        // In the row: its words and its frame in the colour of a problem, as the page's "danger" is
        // (what it leads to asks first); the yes of its question filled with that colour.
        const QColor bad = desk.engine.singletonInstance<QObject*>("Qkvm", "Theme")->property("bad").value<QColor>();
        const QQuickItem* remove = inRow(rowOfKvm(desk, ids.at(0)), u"kvmRemove"_s);
        QVERIFY(remove->property("risky").toBool());
        QCOMPARE(remove->property("ink").value<QColor>(), bad);
        QVERIFY(!inRow(rowOfKvm(desk, ids.at(0)), u"kvmEdit"_s)->property("risky").toBool());
        QVERIFY(inRow(rowOfKvm(desk, ids.at(0)), u"kvmEdit"_s)->property("ink").value<QColor>() != bad);
        clickOn(desk.window, inRow(rowOfKvm(desk, ids.at(0)), u"kvmRemove"_s));
        QTRY_VERIFY(theOne(desk.window, u"removeYes"_s) != nullptr);
        QVERIFY(theOne(desk.window, u"removeYes"_s)->property("danger").toBool());
        QVERIFY(!theOne(desk.window, u"removeYes"_s)->property("risky").toBool());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- a box out of sight, and what is said of a password ------------------------------------------

    void aBoxIsShowingOnlyWhileItsWindowIsInSight()
    {
        Desk desk;
        const QStringList ids =
            desk.put({{u"http://127.0.0.1:21701"_s, u"One"_s}, {u"http://127.0.0.1:21702"_s, u"Two"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 2);
        QVERIFY(desk.kvms.isShown());
        // The window goes out of sight (to the notification area) with its box open on the list:
        // the list is shown by nobody, and holds nothing for anybody.
        desk.window->hide();
        QTRY_VERIFY(!desk.kvms.isShown());
        QVERIFY(desk.own->wall().remove(ids.at(0)).ok());
        // Back in sight: shown anew, as the wall is.
        desk.window->show();
        QTRY_VERIFY(desk.kvms.isShown());
        QTRY_COMPARE(kvmRows(desk).size(), 1);
        QVERIFY(rowOfKvm(desk, ids.at(0)) == nullptr);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aKvmThatWasNotAskedYetSaysWhetherAPasswordIsKept()
    {
        Desk desk;
        const QStringList byHand = desk.put({{u"http://127.0.0.1:21801"_s, u"By hand"_s}});
        QVERIFY(!byHand.contains(QString()));
        // Two from a file, off and not asked yet what they are: one came without a password.
        const QString without = desk.putFromAFile(u"http://127.0.0.1:21802"_s, u"From a file"_s);
        const QString with = desk.putFromAFile(u"http://127.0.0.1:21803"_s, u"With one"_s, u"secret"_s);
        QVERIFY(!without.isEmpty() && !with.isEmpty());
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(kvmRows(desk).size(), 3);
        QTRY_VERIFY_WITH_TIMEOUT(rowSays(rowOfKvm(desk, without), u"No password saved"_s), 5000);
        QVERIFY(!rowSays(rowOfKvm(desk, with), u"No password saved"_s));
        QVERIFY(!rowSays(rowOfKvm(desk, byHand.at(0)), u"No password saved"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- a line with a tick, and the theme's choice ---------------------------------------------------

    void aLineWithATickTakesAClickOnItsWholeLeftSide_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("1280x800") << QSize(1280, 800);
        QTest::newRow("640x400") << QSize(640, 400);
    }
    void aLineWithATickTakesAClickOnItsWholeLeftSide()
    {
        QFETCH(QSize, size);
        Desk desk;
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        QVERIFY(bringIntoSight(desk, u"staysInTray"_s));
        QVERIFY(!desk.ownSettings.staysInTray());
        // On what it says, as on the page's label.
        const QQuickItem* label =
            saying(shownSection(desk), u"Keep running in the notification area when the window is closed"_s);
        QVERIFY(label != nullptr);
        clickOn(desk.window, label);
        QTRY_VERIFY(desk.ownSettings.staysInTray());
        // At its left edge, beside nothing it says.
        const QQuickItem* line = lineOf(label);
        QVERIFY(line != nullptr);
        aFrame(desk.window);
        const QRectF stands = placeOf(line);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPointF(stands.left() + 6, stands.top() + 8).toPoint());
        QTRY_VERIFY(!desk.ownSettings.staysInTray());
        // And on the tick itself, as before.
        clickOn(desk.window, theOne(desk.window, u"staysInTray"_s));
        QTRY_VERIFY(desk.ownSettings.staysInTray());
        QVERIFY(theOne(desk.window, u"staysInTray"_s)->property("checked").toBool());
        // A line without a tick takes no click: the look's line does nothing.
        const QQuickItem* theme = saying(shownSection(desk), u"Theme"_s);
        QVERIFY(theme != nullptr);
        const QString look = desk.look.theme();
        clickOn(desk.window, theme);
        QTest::qWait(100);
        QCOMPARE(desk.look.theme(), look);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theThemesChoiceKeepsTheWidthOfItsWidestWord()
    {
        // One of the looks is called by a long name: as wide as that, whichever is chosen. (With the
        // program's own words, every one of them is narrower than the least the choice is wide.)
        LongerWords longer;
        QVERIFY(QCoreApplication::installTranslator(&longer));
        Desk desk;
        QVERIFY(desk.load() != nullptr);
        desk.engine.retranslate();
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        QStringList wrong;
        for (const QString& theme : {u"dark"_s, u"system"_s, u"light"_s, u"dark"_s}) {
            const Seen before = seenOf(desk);
            desk.look.choose(theme);
            wrong += movedBy(before, seenOf(desk), u"the look "_s + theme + u" is chosen"_s);
        }
        desk.look.choose(u"system"_s);
        aFrame(desk.window);
        QVERIFY(says(desk.window, u"As Windows is set for whoever is signed in to it"_s));
        QCoreApplication::removeTranslator(&longer);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- a wall as full as it can be -----------------------------------------------------------------

    void aWallOfAsManyKvmsAsItHoldsOpensSoonAndScrolls()
    {
        // As many KVMs as a wall holds, written as the program writes them: half over plain HTTP,
        // half known by a certificate, two by two the same device; every other without a password.
        Desk desk;
        core::Remembered what = desk.settings.remembered();
        for (int number = 0; number < core::Remembered::mostKvms; ++number) {
            core::OwnKvm kvm;
            kvm.id = u"%1"_s.arg(number + 1, 8, 16, QLatin1Char('0'));
            kvm.name = u"Rack %1, shelf %2"_s.arg(number / 20 + 1).arg(number % 20 + 1);
            const bool encrypted = number % 2 == 1;
            kvm.address = u"%1://127.0.0.1:%2"_s.arg(encrypted ? u"https"_s : u"http"_s).arg(30000 + number);
            kvm.knownByCertificate = encrypted;
            kvm.certificate = encrypted
                                  ? QCryptographicHash::hash(QByteArray::number(number / 4), QCryptographicHash::Sha256)
                                  : QByteArray();
            kvm.askedWhatItIs = encrypted;
            what.kvms.append(kvm);
            if (number % 4 < 2) {
                const QByteArray knownBy = encrypted ? "sha256:" + kvm.certificate.toHex() : QByteArray("none");
                desk.secrets.all.insert(core::LocalWall::secretOf(kvm.id),
                                        "QKVM-KVM-2\n" + kvm.address.toUtf8() + '\n' + knownBy + "\nsecret");
                // (And the wall's mark beside it, as the wall writes it with the password.)
                what.kvmPasswordsKept.insert(kvm.id);
            }
        }
        QVERIFY(desk.settings.remember(what).ok());
        QVERIFY(desk.load() != nullptr);
        QCOMPARE(desk.own->wall().kvms().size(), core::Remembered::mostKvms);
        const auto privateBytes = [] {
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            static_cast<void>(K32GetProcessMemoryInfo(
                GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)));
            return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
        };
        aFrame(desk.window);
        const double before = privateBytes();

        // Opened at the list: until it is shown, with every row there is, and drawn.
        QElapsedTimer clock;
        clock.start();
        QTest::keyClick(desk.window, Qt::Key_Comma, Qt::ControlModifier);
        QVERIFY(QTest::qWaitFor(
            [&] {
                const QQuickItem* list = theOne(desk.window, u"kvmList"_s);
                return desk.isOpen(u"settingsBox"_s) && list != nullptr &&
                       list->property("count").toInt() == core::Remembered::mostKvms;
            },
            120000));
        aFrame(desk.window);
        const qint64 opened = clock.elapsed();
        const double shown = privateBytes();
        QCOMPARE(desk.kvms.count(), core::Remembered::mostKvms);
        // Only the rows in sight are made, and a few beyond: not two thousand.
        const qsizetype madeAtFirst = kvmRows(desk).size();
        QVERIFY2(madeAtFirst > 4 && madeAtFirst < 60, qPrintable(QString::number(madeAtFirst)));
        QStringList wrong = rowsOfOneHeight(desk, u"opened"_s);

        // Scrolled through, as with the wheel: twenty steps from its top to its end, each drawn.
        QQuickItem* content = theOneToWorkOn(desk.window, u"sectionContent"_s);
        QVERIFY(content != nullptr);
        const qreal most = content->property("contentHeight").toReal() - content->height();
        QVERIFY(most > 100'000);
        clock.restart();
        qint64 slowest = 0;
        for (int step = 1; step <= 20; ++step) {
            QElapsedTimer one;
            one.start();
            content->setProperty("contentY", most * step / 20);
            aFrame(desk.window);
            slowest = std::max(slowest, one.elapsed());
        }
        const qint64 scrolled = clock.elapsed();
        // The last row is in sight at the end, with its buttons.
        const QQuickItem* last = rowOfKvm(desk, what.kvms.constLast().id);
        QVERIFY(last != nullptr);
        QVERIFY(seenPlaceOf(content).intersects(seenPlaceOf(inRow(last, u"kvmEdit"_s))));
        QVERIFY(rowOfKvm(desk, what.kvms.constFirst().id) == nullptr);
        wrong += rowsOfOneHeight(desk, u"at the end"_s);
        const qsizetype madeAtTheEnd = kvmRows(desk).size();
        QVERIFY2(madeAtTheEnd < 60, qPrintable(QString::number(madeAtTheEnd)));
        // Closed and opened anew.
        // (How many rows the list has made since it was, in all: each step of the way made a few.)
        const int madeBefore = theOne(desk.window, u"kvmList"_s)->property("made").toInt();
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        clock.restart();
        QVERIFY(openSettings(desk));
        QTRY_COMPARE(theOne(desk.window, u"kvmList"_s)->property("count").toInt(), core::Remembered::mostKvms);
        aFrame(desk.window);
        const qint64 again = clock.elapsed();
        // At its top again, with as few rows made for it: not every row between the end and there.
        const int madeAgain = theOne(desk.window, u"kvmList"_s)->property("made").toInt() - madeBefore;
        QVERIFY2(madeAgain < 120, qPrintable(QString::number(madeAgain)));
        QVERIFY(rowOfKvm(desk, what.kvms.constFirst().id) != nullptr);
        wrong += rowsOfOneHeight(desk, u"opened again"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        qInfo().noquote() << u"MEASURED %1 KVMs: opened in %2 ms, opened again in %3 ms; twenty steps to the end of "
                             "the list in %4 ms, the slowest %5 ms; rows there: %6 at first, %7 at the end, %11 made "
                             "for the opening again; private memory %8 MB before, %9 MB with the list shown, %10 MB "
                             "now"_s.arg(core::Remembered::mostKvms)
                                 .arg(opened)
                                 .arg(again)
                                 .arg(scrolled)
                                 .arg(slowest)
                                 .arg(madeAtFirst)
                                 .arg(madeAtTheEnd)
                                 .arg(before, 0, 'f', 1)
                                 .arg(shown, 0, 'f', 1)
                                 .arg(privateBytes(), 0, 'f', 1)
                                 .arg(madeAgain);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- the boxes for the wall as a file ------------------------------------------------------------

    void exportAndImportOpenOverTheSettingsAndCloseBackWhereTheKeyboardWas()
    {
        Desk desk;
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"files"_s, u"settingsFiles"_s));
        clickOn(desk.window, theOne(desk.window, u"settingsExport"_s));
        QTRY_VERIFY(desk.isOpen(u"ownExportBox"_s));
        // Over the settings, which stay as they were.
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QCOMPARE(shownName(desk), u"settingsFiles"_s);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QCOMPARE(shownName(desk), u"settingsFiles"_s);
        QTRY_COMPARE(keyboardAt(desk), theOne(desk.window, u"settingsExport"_s));
        // And the other.
        clickOn(desk.window, theOne(desk.window, u"settingsImport"_s));
        QTRY_VERIFY(desk.isOpen(u"ownImportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QCOMPARE(shownName(desk), u"settingsFiles"_s);
        QTRY_COMPARE(keyboardAt(desk), theOne(desk.window, u"settingsImport"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- a narrow window, and the pictures -----------------------------------------------------------

    void aNarrowWindowHasTheSectionsInARowAndEachLinesRightUnderItsLeft()
    {
        Desk desk;
        QVERIFY(!desk.put({{u"http://127.0.0.1:21001"_s, u"NAS"_s}}).contains(QString()));
        QVERIFY(desk.load(640, 400) != nullptr);
        QVERIFY(openSettings(desk));
        const QRectF box = seenOf(desk).box;
        QCOMPARE(box.width(), 616.0);
        QCOMPARE(box.height(), 376.0);
        // The sections in one row above the section, and no heading over them.
        const QList<const QQuickItem*> entries = seenOnes(desk.window, u"settingsSection"_s);
        QCOMPARE(entries.size(), 9);
        for (qsizetype at = 1; at < entries.size(); ++at) {
            QCOMPARE(placeOf(entries.at(at)).top(), placeOf(entries.at(0)).top());
            QVERIFY(placeOf(entries.at(at)).left() > placeOf(entries.at(at - 1)).right());
        }
        QVERIFY(!says(desk.window, u"This PC's wall"_s));
        QVERIFY(placeOf(theOne(desk.window, u"sectionHead"_s)).top() > placeOf(entries.at(0)).bottom());
        QVERIFY(box.contains(placeOf(entries.at(0))));
        // (Where they do not all fit, the row scrolls sideways: the one the keys come to is in
        // sight, the last as well.)
        QTRY_VERIFY(entryOf(desk, u"kvms"_s)->hasActiveFocus());
        QTest::keyClick(desk.window, Qt::Key_Left);
        QTRY_COMPARE(shownName(desk), u"settingsAbout"_s);
        aFrame(desk.window);
        QVERIFY(box.contains(seenPlaceOf(entryOf(desk, u"about"_s))));
        QCOMPARE(seenPlaceOf(entryOf(desk, u"about"_s)).width(), placeOf(entryOf(desk, u"about"_s)).width());
        QTest::keyClick(desk.window, Qt::Key_Right);
        QTRY_COMPARE(shownName(desk), u"settingsKvms"_s);
        aFrame(desk.window);
        QCOMPARE(seenPlaceOf(entryOf(desk, u"kvms"_s)).width(), placeOf(entryOf(desk, u"kvms"_s)).width());
        // A row's buttons stand under what it says.
        const QQuickItem* row = kvmRows(desk).value(0);
        QVERIFY(row != nullptr);
        QVERIFY(placeOf(inRow(row, u"kvmEdit"_s)).top() > placeOf(saying(row, u"GL.iNet Comet / PiKVM"_s)).bottom());
        // A line's control stands under what the line is, at its left.
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        const QQuickItem* theme = saying(shownSection(desk), u"Theme"_s);
        const QQuickItem* choice = theOne(desk.window, u"themeChoice"_s);
        QVERIFY(theme != nullptr && choice != nullptr);
        QVERIFY(placeOf(choice).top() >= placeOf(theme).bottom());
        QVERIFY(qAbs(placeOf(choice).left() - placeOf(theme).left()) < 1);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));

        // Wide, beside each other: the sections down the side, a line's control at its right.
        desk.window->resize(1280, 800);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QVERIFY(openSettings(desk));
        const QList<const QQuickItem*> down = seenOnes(desk.window, u"settingsSection"_s);
        QCOMPARE(down.size(), 9);
        for (qsizetype at = 1; at < down.size(); ++at) {
            QCOMPARE(placeOf(down.at(at)).left(), placeOf(down.at(0)).left());
            QVERIFY(placeOf(down.at(at)).top() > placeOf(down.at(at - 1)).bottom());
        }
        QVERIFY(says(desk.window, u"This PC's wall"_s));
        const QQuickItem* wideTheme = saying(shownSection(desk), u"Theme"_s);
        const QQuickItem* wideChoice = theOne(desk.window, u"themeChoice"_s);
        QVERIFY(placeOf(wideChoice).left() > placeOf(wideTheme).right());
        QVERIFY(placeOf(wideChoice).top() < placeOf(wideTheme).bottom());
        QCOMPARE(seenOf(desk).box.width(), 1080.0);
        QCOMPARE(seenOf(desk).box.height(), 776.0);
        // And in a large window, as large as the page's and no larger, in the middle.
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        desk.window->resize(1600, 1000);
        QVERIFY(openSettings(desk));
        const QRectF large = seenOf(desk).box;
        QCOMPARE(large.size(), QSizeF(1080, 780));
        QCOMPARE(large.center(), QPointF(800, 500));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void picturesOfEverySectionInBothLooksWideAndNarrow_data()
    {
        QTest::addColumn<QString>("look");
        QTest::addColumn<QSize>("size");
        QTest::newRow("dark, wide") << u"dark"_s << QSize(1280, 800);
        QTest::newRow("light, wide") << u"light"_s << QSize(1280, 800);
        QTest::newRow("dark, narrow") << u"dark"_s << QSize(640, 400);
        QTest::newRow("light, narrow") << u"light"_s << QSize(640, 400);
    }
    void picturesOfEverySectionInBothLooksWideAndNarrow()
    {
        QFETCH(QString, look);
        QFETCH(QSize, size);
        Desk desk;
        // Two that are one device and have no password kept, one whose name is longer than its place,
        // and one of each other case.
        const QStringList ids = desk.put(
            {{u"https://127.0.0.1:21101"_s, u"Rack 1"_s, 'a', QString()},
             {u"https://127.0.0.1:21102"_s, u"Rack 1 again"_s, 'a', QString()},
             {u"https://127.0.0.1:21103"_s, u"Behind a proxy"_s},
             {u"http://127.0.0.1:21104"_s, u"NAS"_s},
             {u"https://127.0.0.1:21105"_s, u"No password"_s, 'e', QString()},
             {u"http://127.0.0.1:21106"_s, u"The rack in the second server room, top shelf, left of the door"_s}});
        QVERIFY(!ids.contains(QString()));
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY2(lookIs(desk, look), qPrintable(look));
        QVERIFY(openSettings(desk));
        QTRY_VERIFY_WITH_TIMEOUT(rowSays(rowOfKvm(desk, ids.at(4)), u"No password saved"_s), 20000);
        const QString of = u"-%1-%2"_s.arg(look, size.width() < 800 ? u"narrow"_s : u"wide"_s);
        const QStringList atFirst = rowsOfOneHeight(desk, u"at first"_s);
        QVERIFY2(atFirst.isEmpty(), qPrintable(atFirst.join(u'\n')));
        keepPictureOf(desk.window, u"settings-kvms"_s + of);
        // What comes late, from another window: one of the two is given its password, and another KVM
        // a name of five lines. Nothing moves for either.
        const Seen before = seenOf(desk);
        QVERIFY(desk.own->wall().change(ids.at(1), desk.own->wall().kvms().at(1), u"secret"_s).ok());
        core::OwnKvm nas = desk.own->wall().kvms().at(3);
        nas.name = u"The NAS in the cupboard under the stairs, the one that is switched off at night and on again "
                   "in the morning by whoever comes first"_s;
        QVERIFY(desk.own->wall().change(nas.id, nas, std::nullopt).ok());
        QTRY_VERIFY(!rowHasLine(rowOfKvm(desk, ids.at(1)), u"No password saved"_s));
        QTRY_VERIFY(rowSays(rowOfKvm(desk, ids.at(3)), nas.name));
        QStringList moved = rowsOfOneHeight(desk, u"before"_s);
        moved += movedBy(before, seenOf(desk), u"what comes late"_s);
        moved += rowsOfOneHeight(desk, u"after"_s);
        keepPictureOf(desk.window, u"settings-kvms-late"_s + of);
        QVERIFY2(moved.isEmpty(), qPrintable(moved.join(u'\n')));
        QVERIFY(choose(desk, u"files"_s, u"settingsFiles"_s));
        keepPictureOf(desk.window, u"settings-files"_s + of);
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        keepPictureOf(desk.window, u"settings-general"_s + of);
        QVERIFY(choose(desk, u"about"_s, u"settingsAbout"_s));
        keepPictureOf(desk.window, u"settings-about"_s + of);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        // And the About box, which shows what that section shows.
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"aboutBox"_s), "open"));
        QTRY_VERIFY(desk.isOpen(u"aboutBox"_s));
        QVERIFY(seenOnes(desk.window, u"aboutProgram"_s).size() == 1);
        QVERIFY(seenOnes(desk.window, u"aboutLibraries"_s).size() == 1);
        // The program's own source, at the tag of the version that runs.
        const QList<const QQuickItem*> source = seenOnes(desk.window, u"aboutSource"_s);
        QCOMPARE(source.size(), 1);
        QVERIFY(
            source.first()->property("hint").toString().startsWith(u"https://github.com/qkvm/qkvm-desktop/tree/v"_s));
        keepPictureOf(desk.window, u"about-box"_s + of);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"aboutBox"_s));
        // At a server's wall, General with its first line.
        desk.session.show(atAServersWall());
        QVERIFY(openSettings(desk));
        QVERIFY(choose(desk, u"general"_s, u"settingsGeneral"_s));
        keepPictureOf(desk.window, u"settings-general-server"_s + of);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    // The off-screen platform knows no fonts but those in a folder it is shown: without Windows' own,
    // every letter is an empty box, and no line of text is as long as it will be.
    qputenv("QT_QPA_FONTDIR", qgetenv("SystemRoot") + "/Fonts");
    QGuiApplication app(argc, argv);
    qkvm::ui::prepareApplication();
    TestSettingsScreens test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_settings_screens.moc"
