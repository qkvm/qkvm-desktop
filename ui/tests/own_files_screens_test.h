#pragma once

#include <algorithm>
#include <functional>
#include <memory>

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wall_exchange.h>

#include "about_model.h"
#include "actions_view.h"
#include "add_kvm_view.h"
#include "application.h"
#include "control_view.h"
#include "edit_kvm_view.h"
#include "files_view.h"
#include "look.h"
#include "own_files_view.h"
#include "own_kvms_view.h"
#include "pretend_start.h"
#include "pretend_wall.h"
#include "session_view.h"
#include "settings_view.h"
#include "wall_files_by_hand.h"
#include "wall_view.h"

// What the programs that try this PC's two boxes for its wall as a file on the screen share (test_own_files_screens_*:
// they were one program, which took 598 of its 600 s on the slower cores, until PERF of 2026-11-15): the desk the
// boxes are tried at, the helpers that find, press and measure what is on it, and the files it is given.
namespace qkvm::ui::testing::own_files_screens {

using namespace Qt::StringLiterals;
using core::WallExchange;
using core::exchange::Device;
using core::testing::thePassphrase;
using ui::OwnFilesView;
using ui::testing::fileOf;
using ui::testing::fileWithSoManyLeftOut;
using ui::testing::HeldWork;
using ui::testing::knownBy;
using ui::testing::kvmAt;
using ui::testing::notKeptHere;
using ui::testing::quickly;
using ui::testing::saying;
using ui::testing::WallOfOneTest;

// This PC's own wall is shown: nobody is signed in to anything, and whoever sits here manages it.
inline ui::SessionView::Shown atItsOwnWall()
{
    ui::SessionView::Shown shown;
    shown.stage = u"wall"_s;
    shown.manages = true;
    shown.encrypted = true;
    shown.ownWall = true;
    return shown;
}

// Somebody is signed in to a QKVM server, and manages it: the window shows that server's wall.
inline ui::SessionView::Shown atAServersWall()
{
    ui::SessionView::Shown shown;
    shown.stage = u"wall"_s;
    shown.address = u"192.168.1.10:8787"_s;
    shown.account = u"anna"_s;
    shown.manages = true;
    return shown;
}

// One window of the program at this PC's own wall, as the program makes it, with everything the
// program hands it; and of that, the boxes for the wall as a file over a WallExchange of their
// own and a wall that is held to this PC's own address. Where a file is saved and which is opened
// is said here, and how often either was asked is counted.
//
// The two moments in which nothing is taken and nothing is saved are short here (`quickRest`),
// so that what is waited for comes soon. A test that looks at what is NOT to be had yet makes
// them long first (`longRest`): it then has seconds to look in, on a PC that is busy too.
struct Desk {
    static constexpr int quickRest = 250;
    static constexpr int longRest = 4000;
    // How long a wait is that is surely longer than a quick moment.
    static constexpr int pastAQuickRest = 4 * quickRest;
    // A moment for a test that looks, after each of many changes, at what the change held back:
    // the look comes within a frame or two, and there are seconds to spare for it on a busy PC.
    static constexpr int heldRest = 2000;

    explicit Desk(WallOfOneTest& own, int keptForMs = WallExchange::keptForMsAsItIs,
                  core::exchange::Sealing sealing = quickly())
        : settings(folder.filePath(u"settings.json"_s)),
          look(settings, [this](Qt::ColorScheme scheme) { lookTold = scheme; }), ownSettings(start, settings),
          exchange(own.wall, std::move(sealing), keptForMs)
    {
        session.show(atItsOwnWall());
        actions.setAllowed(true);
        // The settings list the wall's KVMs. (Nothing goes to a KVM for that: whether a password is
        // kept for each is the wall's answer from its store.)
        kvms.use(&own.wall);
        ownFiles.restFor(quickRest, quickRest);
        ownFiles.use(&exchange);
        ownFiles.setDoor({[this](const QString& suggested) -> base::Result<QString> {
                              suggestions.append(suggested);
                              return savesTo;
                          },
                          [this]() -> base::Result<QString> {
                              ++opened;
                              if (doorFails) {
                                  return base::Error{"box", "Windows did not open its box for choosing a file."};
                              }
                              return opens;
                          }});
    }

    // The window, loaded as the program loads it and brought up; null where it did not come up.
    QQuickWindow* load(int width = 0, int height = 0)
    {
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

    QString write(const QString& name, const QByteArray& text) const
    {
        const QString path = folder.filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(text);
        }
        return path;
    }
    static QByteArray read(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("(no such file)");
    }
    QObject* box(const QString& name) const { return window->findChild<QObject*>(name); }
    bool isOpen(const QString& name) const { return box(name) != nullptr && box(name)->property("opened").toBool(); }
    // One of the colours the screens are drawn in, by its name (Theme.qml).
    QColor colour(const char* name)
    {
        const QObject* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        return colours != nullptr ? colours->property(name).value<QColor>() : QColor();
    }

    QTemporaryDir folder;
    core::Settings settings;
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
    ui::FilesView files;
    ui::AddKvmView adding;
    ui::EditKvmView editing;
    ui::ControlView control;
    ui::OwnKvmsView kvms;
    WallExchange exchange;
    OwnFilesView ownFiles;
    QString savesTo;
    QString opens;
    QStringList suggestions;
    int opened = 0;
    // Windows' box for choosing a file does not open: it says why instead.
    bool doorFails = false;
    QStringList complaints;
    // (Last, and so the first to go: the window, before anything it was handed.)
    QQmlApplicationEngine engine;
    QQuickWindow* window = nullptr;
};

// Every item of this name in what a window shows.
inline void named(const QQuickItem* under, const QString& name, QList<const QQuickItem*>& found)
{
    if (under->objectName() == name) {
        found.append(under);
    }
    const QList<QQuickItem*> children = under->childItems();
    for (const QQuickItem* child : children) {
        named(child, name, found);
    }
}

// Whether an item can be seen: it, and everything it is in. (What is not there is not seen.)
inline bool isSeen(const QQuickItem* item)
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
inline QList<const QQuickItem*> seenOnes(const QQuickWindow* window, const QString& name)
{
    QList<const QQuickItem*> found;
    named(window->contentItem(), name, found);
    found.removeIf([](const QQuickItem* item) { return !isSeen(item); });
    return found;
}

// The one item of this name that can be seen; null where none or several can.
inline const QQuickItem* theOne(const QQuickWindow* window, const QString& name)
{
    const QList<const QQuickItem*> found = seenOnes(window, name);
    return found.size() == 1 ? found.constFirst() : nullptr;
}

// The line of text that says exactly this and can be seen, wherever in the window it is.
inline const QQuickItem* saying(const QQuickItem* under, const QString& text)
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

inline bool says(const QQuickWindow* window, const QString& text)
{
    return saying(window->contentItem(), text) != nullptr;
}

// A frame is drawn: what was opened or changed a moment ago is laid out with it, and not before.
inline void aFrame(QQuickWindow* window)
{
    QSignalSpy drawn(window, &QQuickWindow::frameSwapped);
    window->update();
    static_cast<void>(drawn.wait(2000));
}

inline QPoint middleOf(const QQuickItem* item)
{
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

// A click on an item, where it is once the window has drawn it: as a person clicks, on what is seen.
inline void clickOn(QQuickWindow* window, const QQuickItem* item)
{
    aFrame(window);
    QTest::mouseClick(window, Qt::LeftButton, {}, middleOf(item));
}

inline void type(QQuickWindow* window, const QString& text)
{
    for (const QChar letter : text) {
        QTest::keyClick(window, letter.toLatin1());
    }
}

// Whether an item is whole inside its window.
inline bool isInside(const QQuickWindow* window, const QQuickItem* item)
{
    const QRectF where = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    return where.left() >= 0 && where.top() >= 0 && where.right() <= window->width() &&
           where.bottom() <= window->height();
}

// How much of an item's height lies within what a list shows, which cuts off the rest; nothing:
// none of it is in sight.
inline qreal heightInSight(const QQuickItem* list, const QQuickItem* item)
{
    const QRectF where = item->mapRectToItem(list, QRectF(0, 0, item->width(), item->height()));
    return std::max(0.0, std::min(where.bottom(), list->height()) - std::max(where.top(), 0.0));
}

// Whether an item is whole within what a list shows.
inline bool isWholeIn(const QQuickItem* list, const QQuickItem* item)
{
    return item->height() > 0 && heightInSight(list, item) >= item->height() - 0.5;
}

// A certificate's SHA-256 as a row says it in short: its first two pairs and its last two, with
// the sign for what is left out between them (three dots in one, made here from its number).
inline QString inShortOf(const QByteArray& sha256)
{
    const QStringList pairs = core::kvm::fingerprint(sha256).split(u':');
    return pairs.first(2).join(u':') + u' ' + QChar(0x2026) + u' ' + pairs.last(2).join(u':');
}

// For whoever wants to look at the boxes without starting the program: with QKVM_SCREENSHOTS set to
// a folder, each state is kept there as a picture of the test's own window, as Qt Quick draws it in
// software.
inline void keepPictureOf(QQuickWindow* window, const QString& name)
{
    const QString folder = qEnvironmentVariable("QKVM_SCREENSHOTS");
    if (folder.isEmpty()) {
        return;
    }
    aFrame(window);
    QVERIFY(QDir().mkpath(folder));
    QVERIFY(window->grabWindow().save(QDir(folder).filePath(name + u".png"_s)));
}

// What a window's notices say, in the order they came.
inline QStringList toldBy(ui::ActionsView& actions)
{
    QStringList told;
    for (int row = 0; row < actions.notices()->count(); ++row) {
        const QModelIndex notice = actions.notices()->index(row, 0);
        told.append(notice.data(ui::Notices::KindRole).toString() + u": "_s +
                    notice.data(ui::Notices::TextRole).toString());
    }
    return told;
}

// Addresses that reach nothing by the books, each of its own.
inline QString elsewhere(int number)
{
    return u"https://192.0.2.%1"_s.arg(number);
}

// A device that is ticked to begin with: over HTTPS, known by Windows' word, somewhere else.
inline Device ticked(int number, const QString& name)
{
    return saying(kvmAt(elsewhere(number), name), u"system"_s);
}

// A file of so many devices, each at a place of its own that reaches nothing: plain, as a wall of
// that size is read in a moment.
inline QByteArray fileOfSoMany(int count)
{
    QList<Device> devices;
    devices.reserve(count);
    for (int number = 0; number < count; ++number) {
        devices.append(
            saying(kvmAt(u"https://192.0.2.1:%1"_s.arg(1024 + number), u"KVM %1"_s.arg(number + 1)), u"system"_s));
    }
    return fileOf(devices, false);
}

// A file with a device of every kind a box has something to say of, a device that the reader
// leaves out, and what is not this PC's yet: as a server writes one, by hand.
inline QByteArray fileOfEveryKind()
{
    // What the program's own writer writes: a KVM known by the certificate its file names; one
    // that Windows vouches for; one whose certificate is not checked; one over plain HTTP; one
    // without a name; one at a name in its xn-- form; one at this PC; one at an address no router
    // passes on; two that this PC's wall does not keep as the file has them; one that a test may
    // have on its wall already; the twin of the first, by its certificate; and one whose name this
    // PC's wall does not keep (fifty signs that take two places each).
    const QList<Device> writes{knownBy(kvmAt(elsewhere(1), u"Rack 1, top"_s), "first"),
                               ticked(2, u"By Windows"_s),
                               saying(kvmAt(elsewhere(3), u"Not checked"_s), u"insecure"_s),
                               kvmAt(u"http://192.0.2.4:8080"_s, u"In the clear"_s),
                               ticked(5, QString()),
                               saying(kvmAt(u"https://xn--80ak6aa92e.invalid"_s, u"Looks like another"_s), u"system"_s),
                               saying(kvmAt(u"https://127.0.0.1:20443"_s, u"Here"_s), u"system"_s),
                               saying(kvmAt(u"https://169.254.7.7"_s, u"Link-local"_s), u"system"_s),
                               notKeptHere(ticked(9, u"A small one"_s)),
                               notKeptHere(ticked(10, u"A server"_s)),
                               ticked(11, u"There already"_s),
                               knownBy(kvmAt(elsewhere(12), u"Rack 1, top, moved"_s), "first"),
                               ticked(13, core::testing::wideSigns(50))};
    QJsonArray devices = QJsonDocument::fromJson(fileOf(writes, false)).object()[u"devices"_s].toArray();
    if (devices.size() != writes.size()) {
        return {};
    }
    // And by hand, what no writer of QKVM's writes: a device with no address, which the reader
    // leaves out with a sentence; a second device at the address of another; and a server's
    // management controller, which this wall reads too (from M7 1b-ii on).
    QJsonObject again = devices.at(1).toObject();
    again[u"name"_s] = u"By Windows, again"_s;
    devices.append(again);
    QJsonObject controller = devices.at(1).toObject();
    controller[u"name"_s] = u"A server's controller"_s;
    controller[u"kind"_s] = u"redfish"_s;
    controller[u"url"_s] = u"https://192.0.2.30"_s;
    devices.append(controller);
    devices.insert(1, QJsonObject{{u"name"_s, u"Nowhere"_s},
                                  {u"url"_s, u""_s},
                                  {u"username"_s, u"admin"_s},
                                  {u"kind"_s, u"kvmd"_s},
                                  {u"group"_s, u""_s},
                                  {u"tls"_s, u"system"_s},
                                  {u"cert_pem"_s, QJsonValue()},
                                  {u"open_url"_s, u""_s},
                                  {u"alerts"_s, true},
                                  {u"ident"_s, u""_s}});
    return core::testing::plainFile({{u"devices"_s, devices},
                                     {u"actions"_s, QJsonArray{QJsonObject{{u"name"_s, u"Lock"_s},
                                                                           {u"kind"_s, u"keys"_s},
                                                                           {u"keys"_s, u"MetaLeft,KeyL"_s},
                                                                           {u"enter"_s, false},
                                                                           {u"danger"_s, false}}}}});
}

// The two boxes for this PC's own wall as a file, on the screen: each through, by the buttons a
// person presses, in a window the program's own screens make. Without a desktop and without a
// graphics card: Qt's off-screen platform, and Qt Quick drawing in software. The files are made
// and read by the program's own reader and writer, and the wall is one that is held to this PC's
// own address.
class OwnFilesScreensTest : public QObject {
protected:
    // Opens one of this PC's two boxes the way a person does: the settings, at their section of this
    // PC's wall as a file (where they were left last), and its button there, which opens the box
    // over the settings. (Once whatever was held for a moment after a taking is not: a press in that
    // moment is taken by nothing.)
    static bool openFromTheSettings(Desk& desk, const QString& button, const QString& box)
    {
        desk.ownSettings.show(u"files"_s);
        if (!QMetaObject::invokeMethod(desk.box(u"settingsBox"_s), "open") ||
            !QTest::qWaitFor([&] { return desk.isOpen(u"settingsBox"_s) && theOne(desk.window, button) != nullptr; },
                             20000) ||
            !QTest::qWaitFor([&] { return !desk.ownFiles.isHeldAfterTaking(); }, 20000)) {
            return false;
        }
        clickOn(desk.window, theOne(desk.window, button));
        return QTest::qWaitFor([&] { return desk.isOpen(box) && desk.isOpen(u"settingsBox"_s); }, 20000);
    }

    // A file is chosen and read in the box that is open, by its buttons; true once it is laid out,
    // and its list has stood for its moment (until then nothing of it takes a click).
    static bool chooseAndRead(Desk& desk, const QByteArray& file, const QString& passphrase = {})
    {
        desk.opens = desk.write(u"wall-%1.qkvm"_s.arg(desk.opened), file);
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        if (desk.ownFiles.fileName().isEmpty()) {
            return false;
        }
        if (!passphrase.isEmpty()) {
            if (theOne(desk.window, u"ownImportPassphrase"_s) == nullptr) {
                return false;
            }
            clickOn(desk.window, theOne(desk.window, u"ownImportPassphrase"_s));
            type(desk.window, passphrase);
        }
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        return QTest::qWaitFor([&] { return desk.ownFiles.isLaidOut(); }, 60000) &&
               QTest::qWaitFor([&] { return desk.ownFiles.isSettled(); }, 20000);
    }

    // The rows the list has made for devices at this moment.
    static QList<const QQuickItem*> deviceRows(const QQuickWindow* window)
    {
        QList<const QQuickItem*> rows;
        named(window->contentItem(), u"ownImportRow"_s, rows);
        rows.removeIf([](const QQuickItem* row) { return row->property("kind").toString() != u"device"_s; });
        return rows;
    }
    // The row of the device at a place of the file, where the list has one made for it just now.
    static const QQuickItem* rowOf(const QQuickWindow* window, int place)
    {
        for (const QQuickItem* row : deviceRows(window)) {
            if (row->property("place").toInt() == place) {
                return row;
            }
        }
        return nullptr;
    }
    // The one thing of this name in a row.
    static const QQuickItem* inRow(const QQuickItem* row, const QString& name)
    {
        QList<const QQuickItem*> found;
        if (row != nullptr) {
            named(row, name, found);
        }
        return found.size() == 1 ? found.constFirst() : nullptr;
    }
    // And what it says; "(none)" where the row has no one such thing.
    static QString saidIn(const QQuickItem* row, const QString& name)
    {
        const QQuickItem* item = inRow(row, name);
        return item != nullptr ? item->property("text").toString() : u"(none)"_s;
    }
    // Every line of text that can be seen in something, in the order they are found in.
    static void everyTextIn(const QQuickItem* under, QStringList& texts)
    {
        if (under == nullptr || !under->isVisible()) {
            return;
        }
        if (under->inherits("QQuickText")) {
            texts.append(under->property("text").toString());
        }
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            everyTextIn(child, texts);
        }
    }
    // The list of the box that takes a file in: to be looked at, and to be scrolled.
    static QQuickItem* listOf(const Desk& desk) { return desk.window->findChild<QQuickItem*>(u"ownImportOffered"_s); }
    // Where a list stands: how far it is scrolled from its top.
    static qreal scrolledBy(const QQuickItem* list)
    {
        return list->property("contentY").toReal() - list->property("originY").toReal();
    }
    // And how far it can be scrolled at the most.
    static qreal mostScrolled(const QQuickItem* list)
    {
        return std::max(0.0, list->property("contentHeight").toReal() - list->height());
    }
    static void scrollTo(QQuickItem* list, qreal fromItsTop)
    {
        list->setProperty("contentY", list->property("originY").toReal() + fromItsTop);
        QCoreApplication::processEvents();
    }

    // The look the screens are drawn in, by the program's own choice of it (ui/look.h, which
    // remembers it): what the look tells is told to what the screens take their colours from
    // (Theme.qml), as Qt tells it where it has a screen, and is asked of it there. False where they
    // are not drawn so.
    static bool lookIs(Desk& desk, const QString& theme)
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

    // What a box shows at one moment, for what somebody who points at something in it finds
    // under the pointer a moment later: where the box is, and how large; every control in it that
    // a person can press (a button, a tick, a row's "Details", the row that opens the notes, a
    // line to type into and its eye, the way out in the box's title), where it can be seen in the
    // window, by what it is; which of them take a click at this moment; which stand in a row of
    // the list, and of those in a device's row, which device's; and where the first line of each
    // device's row stands.
    struct Seen {
        bool open = false;
        QRectF box;
        QHash<QString, QRectF> controls;
        QStringList live;
        QStringList inTheList;
        QHash<QString, int> ofDevice;
        QHash<int, QPointF> firstLines;
    };

    // What a control is, for a person: by its name; one in a row of the list, of that row's
    // device; one in a line to type into, of that line. The box's own button is one, whichever
    // of the two it says ("Read the file" and "Take", "Make the file" and "Save"): the one comes
    // after the other in its place, with a moment in which it takes no click.
    static QString whatControl(const QQuickItem* control)
    {
        const QString name = control->objectName();
        if (name == u"ownImportRead"_s || name == u"ownImportTake"_s) {
            return u"the box's own button (import)"_s;
        }
        if (name == u"ownExportMake"_s || name == u"ownExportSave"_s) {
            return u"the box's own button (export)"_s;
        }
        QString what = name.isEmpty() ? QString::fromLatin1(control->metaObject()->className()) : name;
        for (const QQuickItem* up = control->parentItem(); up != nullptr; up = up->parentItem()) {
            if (up->objectName() == u"ownImportRow"_s) {
                if (up->property("kind").toString() == u"device"_s) {
                    what += u" of device %1"_s.arg(up->property("place").toInt());
                }
                break;
            }
            if (up->objectName().endsWith(u"Passphrase"_s)) {
                what += u" of "_s + up->objectName();
                break;
            }
        }
        return what;
    }
    static bool isInARow(const QQuickItem* item)
    {
        for (const QQuickItem* up = item->parentItem(); up != nullptr; up = up->parentItem()) {
            if (up->objectName() == u"ownImportRow"_s) {
                return true;
            }
        }
        return false;
    }
    // The device whose row an item stands in, by its place in the file; 0: none.
    static int deviceOf(const QQuickItem* item)
    {
        for (const QQuickItem* up = item->parentItem(); up != nullptr; up = up->parentItem()) {
            if (up->objectName() == u"ownImportRow"_s) {
                return up->property("kind").toString() == u"device"_s ? up->property("place").toInt() : 0;
            }
        }
        return 0;
    }
    // Where an item can be seen in the window: its place, cut off by whatever it is in that cuts
    // off what lies beyond its edges (a list). A click is taken there, and not beyond.
    static QRectF seenPlaceOf(const QQuickItem* item)
    {
        QRectF where = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        for (const QQuickItem* up = item->parentItem(); up != nullptr; up = up->parentItem()) {
            if (up->clip()) {
                where = where.intersected(up->mapRectToScene(QRectF(0, 0, up->width(), up->height())));
            }
        }
        return where;
    }
    static void controlsIn(const QQuickItem* under, Seen& seen)
    {
        if (under == nullptr || !under->isVisible()) {
            return;
        }
        if (under->inherits("QQuickAbstractButton") || under->inherits("QQuickTextInput")) {
            const QRectF where = seenPlaceOf(under);
            if (where.width() > 1 && where.height() > 1) {
                const QString what = whatControl(under);
                seen.controls.insert(what, where);
                if (under->isEnabled()) {
                    seen.live.append(what);
                }
                if (isInARow(under)) {
                    seen.inTheList.append(what);
                }
                if (const int place = deviceOf(under); place > 0) {
                    seen.ofDevice.insert(what, place);
                }
            }
        }
        const QList<QQuickItem*> children = under->childItems();
        for (const QQuickItem* child : children) {
            controlsIn(child, seen);
        }
    }
    // What a box shows once the window has drawn it.
    static Seen seenOf(const Desk& desk, const QString& name)
    {
        aFrame(desk.window);
        Seen seen;
        const QObject* box = desk.box(name);
        seen.open = desk.isOpen(name);
        seen.box = QRectF(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                          box->property("height").toReal());
        if (!seen.open) {
            return seen;
        }
        for (const char* part : {"header", "contentItem"}) {
            controlsIn(box->property(part).value<QQuickItem*>(), seen);
        }
        if (name == u"ownImportBox"_s) {
            for (const QQuickItem* row : deviceRows(desk.window)) {
                const QQuickItem* line = inRow(row, u"ownImportNameAndAddress"_s);
                if (line != nullptr && isSeen(line) && seenPlaceOf(line).height() > 1) {
                    seen.firstLines.insert(row->property("place").toInt(), line->mapToScene(QPointF(0, 0)));
                }
            }
        }
        return seen;
    }

    // Which rows of the list a change is to leave where they stand on the screen: none (the list
    // comes, goes, or is another), every one (a tick), or a row and those above it (that row is
    // opened or closed: the rows under it move by what it grew or shrank).
    static constexpr int noRow = 0;
    static constexpr int everyRow = 1 << 30;

    // What somebody who pointed at something in a box a moment before a change finds under the
    // pointer after it, where that is not what was pointed at; empty: nothing. The box is where it
    // was and as large (while it is open); no control that takes a click stands where another
    // stood; none outside the list has moved; and the rows that were to stay have stayed, their
    // first lines and every control in them (a tick, "Details"), whether or not that takes a press
    // yet: where a change begins the moment anew, nothing of the list takes one just after it, and
    // what stays has to be where it was all the same, for when it does.
    static QStringList movedBy(const Seen& before, const Seen& after, const QString& when, int rowsUpTo = noRow)
    {
        QStringList wrong;
        const auto apart = [](const QPointF& one, const QPointF& other) {
            return std::max(qAbs(one.x() - other.x()), qAbs(one.y() - other.y()));
        };
        if (before.open && after.open &&
            (apart(before.box.topLeft(), after.box.topLeft()) > 0.5 ||
             apart(before.box.bottomRight(), after.box.bottomRight()) > 0.5)) {
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
        for (const QString& now : after.live) {
            const QRectF is = after.controls.value(now);
            for (auto was = before.controls.cbegin(); was != before.controls.cend(); ++was) {
                const QRectF both = is.intersected(was.value());
                if (was.key() != now && both.width() > 1 && both.height() > 1) {
                    wrong.append(
                        u"%1: \"%2\", which takes a click, stands where \"%3\" stood"_s.arg(when, now, was.key()));
                }
            }
        }
        for (auto was = before.controls.cbegin(); was != before.controls.cend(); ++was) {
            if (before.inTheList.contains(was.key()) || !after.controls.contains(was.key())) {
                continue;
            }
            const QRectF is = after.controls.value(was.key());
            if (apart(was.value().topLeft(), is.topLeft()) > 0.5 ||
                apart(was.value().bottomRight(), is.bottomRight()) > 0.5) {
                wrong.append(u"%1: \"%2\", outside the list, was at %3,%4 and is at %5,%6"_s.arg(when, was.key())
                                 .arg(was.value().x())
                                 .arg(was.value().y())
                                 .arg(is.x())
                                 .arg(is.y()));
            }
        }
        for (auto was = before.firstLines.cbegin(); was != before.firstLines.cend(); ++was) {
            if (was.key() > rowsUpTo || !after.firstLines.contains(was.key())) {
                continue;
            }
            const QPointF is = after.firstLines.value(was.key());
            if (apart(was.value(), is) > 0.5) {
                wrong.append(u"%1: the first line of device %2's row was at %3,%4 and is at %5,%6"_s.arg(when)
                                 .arg(was.key())
                                 .arg(was.value().x())
                                 .arg(was.value().y())
                                 .arg(is.x())
                                 .arg(is.y()));
            }
        }
        for (auto was = before.ofDevice.cbegin(); was != before.ofDevice.cend(); ++was) {
            if (was.value() > rowsUpTo || !after.controls.contains(was.key())) {
                continue;
            }
            const QRectF stood = before.controls.value(was.key());
            const QRectF is = after.controls.value(was.key());
            if (apart(stood.topLeft(), is.topLeft()) > 0.5 || apart(stood.bottomRight(), is.bottomRight()) > 0.5) {
                wrong.append(u"%1: \"%2\", which was to stay, was at %3,%4 and is at %5,%6"_s.arg(when, was.key())
                                 .arg(stood.x())
                                 .arg(stood.y())
                                 .arg(is.x())
                                 .arg(is.y()));
            }
        }
        return wrong;
    }

    // What takes a press at a point of a window: every button, every MouseArea that takes presses,
    // and every item with a TapHandler that is there and can be seen there, by its name, or by its
    // kind and the name of what it is in. (What lies over them and holds them is not looked at:
    // this is what a press would find without it.)
    static QStringList pressableAt(const QQuickWindow* window, const QPoint& point)
    {
        QStringList found;
        const std::function<void(const QQuickItem*)> look = [&](const QQuickItem* item) {
            if (item == nullptr || !item->isVisible()) {
                return;
            }
            bool takes =
                item->isEnabled() && (item->inherits("QQuickAbstractButton") ||
                                      (item->inherits("QQuickMouseArea") && item->property("enabled").toBool()));
            const QObjectList& children = item->children();
            for (const QObject* child : children) {
                takes = takes || (item->isEnabled() && child->inherits("QQuickTapHandler") &&
                                  child->property("enabled").toBool());
            }
            if (takes && seenPlaceOf(item).contains(QPointF(point))) {
                QString in;
                for (const QQuickItem* up = item->parentItem(); up != nullptr && in.isEmpty(); up = up->parentItem()) {
                    in = up->objectName();
                }
                // (A kind of QML's own is called by its file's name: "Tile", and not "Tile_QMLTYPE_12".)
                const QString kind = QString::fromLatin1(item->metaObject()->className()).section(u"_QML"_s, 0, 0);
                found.append(item->objectName().isEmpty()
                                 ? u"a %1 in %2"_s.arg(kind, in.isEmpty() ? u"the window"_s : in)
                                 : item->objectName());
            }
            const QList<QQuickItem*> under = item->childItems();
            for (const QQuickItem* child : under) {
                look(child);
            }
        };
        look(window->contentItem());
        return found;
    }

    // Whether nothing moved: each thing that did is said by itself first, whole.
    static bool nothingMoved(const QStringList& wrong)
    {
        for (const QString& one : wrong) {
            qInfo("%s", qPrintable(one));
        }
        return wrong.isEmpty();
    }

    // The "Details" of a device's row pressed as a person presses it, once to open the row and once
    // to close it, each when the list has stood for its moment; what each moved is added to `wrong`.
    // With `readDown`, the opened row is read down to before it is closed: the list is scrolled
    // until its first line stands at the list's upper edge, or as near to it as the list goes.
    // Said: why it could not be done.
    static QString openAndClose(Desk& desk, int place, const QString& when, QStringList& wrong, bool readDown,
                                const std::function<void(const QString&)>& keep = {})
    {
        for (const bool opening : {true, false}) {
            if (!QTest::qWaitFor([&desk] { return desk.ownFiles.isSettled(); }, 20000)) {
                return u"%1: the list never stood for its moment"_s.arg(when);
            }
            const QQuickItem* row = rowOf(desk.window, place);
            const QQuickItem* details = inRow(row, u"ownImportDetails"_s);
            if (details == nullptr || !isSeen(details) || seenPlaceOf(details).height() < details->height() - 0.5) {
                return u"%1: the row's \"Details\" is not in sight"_s.arg(when);
            }
            if (row->property("open").toBool() == opening) {
                return u"%1: the row is %2 already"_s.arg(when, opening ? u"open"_s : u"closed"_s);
            }
            const Seen before = seenOf(desk, u"ownImportBox"_s);
            if (keep && opening) {
                keep(u"before"_s);
            }
            clickOn(desk.window, details);
            const bool done = QTest::qWaitFor(
                [&] {
                    const QQuickItem* now = rowOf(desk.window, place);
                    return now != nullptr && now->property("open").toBool() == opening;
                },
                20000);
            if (!done) {
                return u"%1: the row was not %2"_s.arg(when, opening ? u"opened"_s : u"closed"_s);
            }
            wrong += movedBy(before, seenOf(desk, u"ownImportBox"_s),
                             u"%1, %2"_s.arg(when, opening ? u"opened"_s : u"closed"_s), place);
            if (keep) {
                keep(opening ? u"opened"_s : u"closed"_s);
            }
            if (opening && readDown) {
                QQuickItem* list = listOf(desk);
                const QQuickItem* opened = rowOf(desk.window, place);
                scrollTo(list, std::min(mostScrolled(list), opened->y() - list->property("originY").toReal()));
                aFrame(desk.window);
            }
        }
        return {};
    }

    // The device whose row stands nearest the middle of the list, with its first line in sight;
    // 0: none.
    static int theRowInTheMiddle(const Desk& desk)
    {
        const QQuickItem* list = listOf(desk);
        const qreal middle = list->height() / 2;
        int nearest = 0;
        qreal by = 1e9;
        for (const QQuickItem* row : deviceRows(desk.window)) {
            const QQuickItem* details = inRow(row, u"ownImportDetails"_s);
            const QRectF where = row->mapRectToItem(list, QRectF(0, 0, row->width(), row->height()));
            if (details == nullptr || !isWholeIn(list, details) || qAbs(where.center().y() - middle) >= by) {
                continue;
            }
            by = qAbs(where.center().y() - middle);
            nearest = row->property("place").toInt();
        }
        return nearest;
    }

    // Scrolls the list, a little at a time from its top as a person does, until the tick of the
    // device at a place (or another thing of its row) is wholly in sight; false where it never is.
    static bool bringIntoSight(const Desk& desk, int place, const QString& what = u"ownImportTick"_s)
    {
        QQuickItem* list = listOf(desk);
        for (qreal at = 0;; at += 10) {
            scrollTo(list, std::min(at, mostScrolled(list)));
            const QQuickItem* tick = inRow(rowOf(desk.window, place), what);
            if (tick != nullptr && isWholeIn(list, tick)) {
                aFrame(desk.window);
                return true;
            }
            if (at >= mostScrolled(list) || at > 20000) {
                return false;
            }
        }
    }

    // A file of nine devices as a wall of somebody's has them: most of them known by their
    // certificate or by Windows' word, one over plain HTTP, one at this PC, one that this PC's
    // wall does not keep as the file has it. Sealed, with their passwords.
    static QByteArray anOrdinaryFile()
    {
        return fileOf({knownBy(kvmAt(elsewhere(1), u"Rack 1, top"_s), "first"),
                       knownBy(kvmAt(elsewhere(2), u"Rack 1, middle"_s), "second"),
                       knownBy(kvmAt(elsewhere(3), u"Rack 1, bottom"_s), "comet"), ticked(4, u"Build server"_s),
                       ticked(5, u"Mail"_s), kvmAt(elsewhere(6), u"Backup"_s),
                       kvmAt(u"http://192.0.2.7:8080"_s, u"Workshop"_s),
                       saying(kvmAt(u"https://127.0.0.1:20443"_s, u"Under the desk"_s), u"system"_s),
                       notKeptHere(ticked(9, u"A small one"_s))});
    }
};

// The start of each of the programs.
template <typename Tests>
int runOwnFilesScreensTests(int argc, char* argv[])
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

} // namespace qkvm::ui::testing::own_files_screens
