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

using namespace Qt::StringLiterals;
using namespace qkvm;
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

namespace {

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

// Whether an item can be seen: it, and everything it is in. (What is not there is not seen.)
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
QList<const QQuickItem*> seenOnes(const QQuickWindow* window, const QString& name)
{
    QList<const QQuickItem*> found;
    named(window->contentItem(), name, found);
    found.removeIf([](const QQuickItem* item) { return !isSeen(item); });
    return found;
}

// The one item of this name that can be seen; null where none or several can.
const QQuickItem* theOne(const QQuickWindow* window, const QString& name)
{
    const QList<const QQuickItem*> found = seenOnes(window, name);
    return found.size() == 1 ? found.constFirst() : nullptr;
}

// The line of text that says exactly this and can be seen, wherever in the window it is.
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

QPoint middleOf(const QQuickItem* item)
{
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

// A click on an item, where it is once the window has drawn it: as a person clicks, on what is seen.
void clickOn(QQuickWindow* window, const QQuickItem* item)
{
    aFrame(window);
    QTest::mouseClick(window, Qt::LeftButton, {}, middleOf(item));
}

void type(QQuickWindow* window, const QString& text)
{
    for (const QChar letter : text) {
        QTest::keyClick(window, letter.toLatin1());
    }
}

// Whether an item is whole inside its window.
bool isInside(const QQuickWindow* window, const QQuickItem* item)
{
    const QRectF where = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    return where.left() >= 0 && where.top() >= 0 && where.right() <= window->width() &&
           where.bottom() <= window->height();
}

// How much of an item's height lies within what a list shows, which cuts off the rest; nothing:
// none of it is in sight.
qreal heightInSight(const QQuickItem* list, const QQuickItem* item)
{
    const QRectF where = item->mapRectToItem(list, QRectF(0, 0, item->width(), item->height()));
    return std::max(0.0, std::min(where.bottom(), list->height()) - std::max(where.top(), 0.0));
}

// Whether an item is whole within what a list shows.
bool isWholeIn(const QQuickItem* list, const QQuickItem* item)
{
    return item->height() > 0 && heightInSight(list, item) >= item->height() - 0.5;
}

// A certificate's SHA-256 as a row says it in short: its first two pairs and its last two, with
// the sign for what is left out between them (three dots in one, made here from its number).
QString inShortOf(const QByteArray& sha256)
{
    const QStringList pairs = core::kvm::fingerprint(sha256).split(u':');
    return pairs.first(2).join(u':') + u' ' + QChar(0x2026) + u' ' + pairs.last(2).join(u':');
}

// For whoever wants to look at the boxes without starting the program: with QKVM_SCREENSHOTS set to
// a folder, each state is kept there as a picture of the test's own window, as Qt Quick draws it in
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

// What a window's notices say, in the order they came.
QStringList toldBy(ui::ActionsView& actions)
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
QString elsewhere(int number)
{
    return u"https://192.0.2.%1"_s.arg(number);
}

// A device that is ticked to begin with: over HTTPS, known by Windows' word, somewhere else.
Device ticked(int number, const QString& name)
{
    return saying(kvmAt(elsewhere(number), name), u"system"_s);
}

// A file of so many devices, each at a place of its own that reaches nothing: plain, as a wall of
// that size is read in a moment.
QByteArray fileOfSoMany(int count)
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
QByteArray fileOfEveryKind()
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

} // namespace

// The two boxes for this PC's own wall as a file, on the screen: each through, by the buttons a
// person presses, in a window the program's own screens make. Without a desktop and without a
// graphics card: Qt's off-screen platform, and Qt Quick drawing in software. The files are made
// and read by the program's own reader and writer, and the wall is one that is held to this PC's
// own address.
class TestOwnFilesScreens : public QObject {
    Q_OBJECT

private:
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

private slots:
    void init()
    {
        // Whatever Qt has to say of these screens while they are worked fails the test that
        // made it say so: a box that is opened, filled, scrolled and emptied does all of it
        // without a word of complaint.
        QTest::failOnWarning();
    }

    void cleanup()
    {
        // (One look for every test that does not say otherwise.)
        ui::Look::tellQt(Qt::ColorScheme::Dark);
    }

    // --- where the boxes hang --------------------------------------------------------------------

    void thisPcOffersItsWallAsAFileInTheSettingsWhereAnAccountOffersAServers()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        // "This PC" has no file of the wall any more: whose the wall is, and the way to a server.
        clickOn(desk.window, theOne(desk.window, u"accountButton"_s));
        QTRY_VERIFY(desk.isOpen(u"accountMenu"_s));
        QVERIFY(says(desk.window, u"This PC"_s));
        QVERIFY(theOne(desk.window, u"connectToAServer"_s) != nullptr);
        QVERIFY(!says(desk.window, u"Export…"_s));
        QVERIFY(!says(desk.window, u"Import…"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"accountMenu"_s));

        // The settings have it: their section of this PC's wall as a file, chosen in their list,
        // with its two buttons in its first line, and what the page says of the two kinds of file.
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"settingsBox"_s), "open"));
        QTRY_VERIFY(desk.isOpen(u"settingsBox"_s));
        const QList<const QQuickItem*> sections = seenOnes(desk.window, u"settingsSection"_s);
        const auto files = std::ranges::find_if(
            sections, [](const QQuickItem* one) { return one->property("key").toString() == u"files"_s; });
        QVERIFY(files != sections.end());
        clickOn(desk.window, *files);
        QTRY_VERIFY(theOne(desk.window, u"settingsExport"_s) != nullptr);
        QVERIFY(theOne(desk.window, u"settingsImport"_s) != nullptr);
        QVERIFY(says(desk.window, u"Export and import"_s));
        QVERIFY(says(desk.window,
                     u"Choose KVMs, saved actions and wall settings to keep in a file or bring to this PC. "
                     "A qKVM server writes and reads the same file."_s));
        QVERIFY(says(desk.window, u"Encrypted (.qkvm)"_s));
        QVERIFY(says(desk.window, u"Plain (.json)"_s));
        QVERIFY(says(desk.window, u"Adds what is not here yet"_s));
        // Each opens its box for this PC's wall, and not a server's, over the settings, as the page
        // stacks them; and closes back to them.
        clickOn(desk.window, theOne(desk.window, u"settingsExport"_s));
        QTRY_VERIFY(desk.isOpen(u"ownExportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(!desk.isOpen(u"exportBox"_s));
        QVERIFY(says(desk.window, u"Export to a file"_s));
        // (A wall with a box over it is told so.)
        QVERIFY(desk.window->property("boxOpen").toBool());
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(!desk.isOpen(u"importBox"_s));
        QVERIFY(says(desk.window, u"Import from a file"_s));
        QVERIFY(desk.window->property("boxOpen").toBool());
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"settingsImport"_s) != nullptr);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QVERIFY(!desk.window->property("boxOpen").toBool());
        // (The settings open again where they were left: at that section.)
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"settingsBox"_s), "open"));
        QTRY_VERIFY(theOne(desk.window, u"settingsImport"_s) != nullptr);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));

        // At a server's wall that whoever is signed in manages, the settings list no section of this
        // PC's wall: they list that server's wall as a file, under a heading of its own, as the page
        // lists it for an admin, in the page's words. Its two buttons open the server's boxes, and
        // not these, over the settings; and each closes back to them. The account has neither.
        desk.session.show(atAServersWall());
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"settingsBox"_s), "open"));
        QTRY_VERIFY(desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(!seenOnes(desk.window, u"settingsSection"_s).isEmpty());
        QStringList keys;
        for (const QQuickItem* one : seenOnes(desk.window, u"settingsSection"_s)) {
            keys.append(one->property("key").toString());
        }
        QCOMPARE(keys, (QStringList{u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s, u"about"_s,
                                    u"files"_s}));
        QVERIFY(says(desk.window, u"The server's wall"_s));
        QVERIFY(!says(desk.window, u"This PC's wall"_s));
        // (It opens where it was left, at "Export and import": the server's now.)
        QTRY_VERIFY(theOne(desk.window, u"serverExport"_s) != nullptr);
        QVERIFY(seenOnes(desk.window, u"settingsExport"_s).isEmpty());
        for (const QString& line :
             {u"The wall as a file: to keep, or to take to another qKVM."_s,
              u"The KVMs and servers on this wall, the saved actions and the alert settings can be written to a "
              u"file, and a file can be taken in here or on another qKVM."_s,
              u"Made with a passphrase you choose. It holds the passwords of the devices, the saved texts and the "
              u"address alerts go to, and opens with that passphrase only."_s,
              u"Made without a passphrase. The same, with every password and saved text left out: its devices ask "
              u"for their password where they are taken in."_s,
              u"Nothing on this wall is changed or removed by it. The alert and wall settings are replaced only "
              u"where you tick them."_s,
              u"Accounts, tokens, two-factor keys and the recovery key are in no such file: each belongs to the qKVM "
              u"it was made on. To keep a whole qKVM, accounts and all, keep a copy of its data folder."_s}) {
            QVERIFY2(says(desk.window, line), qPrintable(line));
        }
        keepPictureOf(desk.window, u"settings-server-files"_s);
        clickOn(desk.window, theOne(desk.window, u"serverExport"_s));
        QTRY_VERIFY(desk.isOpen(u"exportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(!desk.isOpen(u"ownExportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"exportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"exportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        clickOn(desk.window, theOne(desk.window, u"serverImport"_s));
        QTRY_VERIFY(desk.isOpen(u"importBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(!desk.isOpen(u"ownImportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"importCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"importBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"serverImport"_s) != nullptr);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        clickOn(desk.window, theOne(desk.window, u"accountButton"_s));
        QTRY_VERIFY(desk.isOpen(u"accountMenu"_s));
        QVERIFY(!says(desk.window, u"Export…"_s));
        QVERIFY(!says(desk.window, u"Import…"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"accountMenu"_s));

        // Whoever only acts on that server's machines, or only looks, is offered no such file: the
        // server would say no. And the settings open at the first section they list.
        ui::SessionView::Shown operating = atAServersWall();
        operating.manages = false;
        desk.session.show(operating);
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"settingsBox"_s), "open"));
        QTRY_VERIFY(desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(!seenOnes(desk.window, u"settingsSection"_s).isEmpty());
        for (const QQuickItem* one : seenOnes(desk.window, u"settingsSection"_s)) {
            QVERIFY(one->property("key").toString() != u"files"_s);
        }
        QVERIFY(!says(desk.window, u"The server's wall"_s));
        QVERIFY(seenOnes(desk.window, u"serverExport"_s).isEmpty());
        // (And where the same wall comes to let them manage it, the box stays and lists in place
        // what comes with that: the server's wall as a file. Only more came.)
        desk.session.show(atAServersWall());
        QTRY_VERIFY(std::ranges::any_of(seenOnes(desk.window, u"settingsSection"_s), [](const QQuickItem* one) {
            return one->property("key").toString() == u"files"_s;
        }));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- export, through -------------------------------------------------------------------------

    void theWallIsWrittenToAFileByTheButtonsAPersonPresses()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        const QString rack = own.put(u"http://127.0.0.1:20002"_s, u"Rack"_s);
        own.secrets.all.remove(core::LocalWall::secretOf(rack));
        HeldWork work;
        Desk desk(own, WallExchange::keptForMsAsItIs, work.sealing());
        // (A long moment before "Save" takes a click: this test looks at it not taking one.)
        desk.ownFiles.restFor(Desk::longRest, Desk::longRest);
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));

        // A passphrase for the file, or none, and what either does. No password of an account:
        // there is none on this PC.
        const QQuickItem* passphrase = theOne(desk.window, u"ownExportPassphrase"_s);
        QVERIFY(passphrase != nullptr);
        QVERIFY(says(desk.window, u"Passphrase for the file"_s));
        QVERIFY(says(desk.window, u"A passphrase encrypts the file, including device passwords and saved text actions. "
                                  "Left empty, the file omits passwords and all text actions."_s));
        QVERIFY(!says(desk.window, u"Your password"_s));
        QTRY_VERIFY(passphrase->property("hasKeyboard").toBool());
        QVERIFY(theOne(desk.window, u"ownExportSave"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownExportWorking"_s) == nullptr);

        // What is typed is hidden, and shown while its eye is open; sent, it is on show no longer.
        type(desk.window, thePassphrase());
        QCOMPARE(passphrase->property("text").toString(), thePassphrase());
        QVERIFY(!passphrase->property("shown").toBool());
        QList<const QQuickItem*> eyes;
        named(passphrase, u"eye"_s, eyes);
        QCOMPARE(eyes.size(), 1);
        clickOn(desk.window, eyes.constFirst());
        QTRY_VERIFY(passphrase->property("shown").toBool());

        // "Make the file": the box says that it works, there is nothing to press twice, and what
        // the file is made with is not written over. The window does not stand still meanwhile.
        work.hold();
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_COMPARE(work.begun(), 1);
        QVERIFY(!passphrase->property("shown").toBool());
        QTRY_VERIFY(theOne(desk.window, u"ownExportWorking"_s) != nullptr);
        // (Its last sign is the three dots in one, made here from its number.)
        QVERIFY(says(desk.window, u"Making the file"_s + QChar(0x2026)));
        QVERIFY(!theOne(desk.window, u"ownExportMake"_s)->isEnabled());
        QVERIFY(!passphrase->isEnabled());
        QVERIFY(theOne(desk.window, u"ownExportCancel"_s)->isEnabled());
        aFrame(desk.window);
        // (Not by a slip of the hand: a click beside the box does not end it.)
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTest::qWait(100);
        QVERIFY(desk.isOpen(u"ownExportBox"_s));
        QVERIFY(desk.ownFiles.isMaking());

        // Made: every line of what is not in the file stands in the box, each whole, and only
        // then "Save". The passphrase is out of its line. What the lines are is said so that it
        // can be read.
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isMade(), 20000);
        QTRY_VERIFY(theOne(desk.window, u"ownExportSave"_s) != nullptr);
        QVERIFY(theOne(desk.window, u"ownExportMake"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownExportPassphrase"_s) == nullptr);
        QCOMPARE(passphrase->property("text").toString(), QString());
        QVERIFY(says(desk.window, u"The file is made, encrypted with its passphrase, and is not saved yet. Before you "
                                  "save it: what is not in it, and what a qKVM server will read differently."_s));
        const QStringList lines = desk.ownFiles.notInIt();
        QCOMPARE(lines.size(), 2);
        QTRY_COMPARE(seenOnes(desk.window, u"ownExportLine"_s).size(), 2);
        for (const QString& line : lines) {
            const QQuickItem* said = saying(desk.window->contentItem(), line);
            QVERIFY2(said != nullptr, qPrintable(line));
            QVERIFY(!said->property("truncated").toBool());
            QVERIFY(isInside(desk.window, said));
        }
        // Not by the click that made the file: "Save" stands where "Make the file" stood, and
        // takes no click for the first moment the lines are on show.
        const QQuickItem* save = theOne(desk.window, u"ownExportSave"_s);
        QVERIFY(!save->isEnabled());
        clickOn(desk.window, save);
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY(!QFile::exists(desk.savesTo));

        // "Save": Windows' own box, and the file is written whole. The box closes, and its
        // window's corner says what was written.
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
        clickOn(desk.window, save);
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QCOMPARE(desk.suggestions.size(), 1);
        QVERIFY(desk.suggestions.constFirst().endsWith(u".qkvm"_s));
        QCOMPARE(
            toldBy(desk.actions),
            QStringList{u"ok: Exported to wall.qkvm. The file opens with its passphrase only: keep the two apart."_s});
        const base::Result<core::exchange::Content> kept =
            core::exchange::read(Desk::read(desk.savesTo), thePassphrase());
        QVERIFY2(kept.ok(), kept.ok() ? "" : kept.error().message.c_str());
        QCOMPARE(kept.value().devices.value_or(QList<Device>()).size(), 2);
        // Closed, the box keeps nothing; opened again, it begins anew.
        QVERIFY(!desk.ownFiles.isMade());
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        QVERIFY(theOne(desk.window, u"ownExportMake"_s) != nullptr);
        QCOMPARE(theOne(desk.window, u"ownExportPassphrase"_s)->property("text").toString(), QString());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void cancelClosesTheBoxOverAFileThatIsBeingMadeAndThatFileIsNobodys()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        HeldWork work;
        Desk desk(own, WallExchange::keptForMsAsItIs, work.sealing());
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownExportPassphrase"_s)->property("hasKeyboard").toBool());
        type(desk.window, thePassphrase());
        work.hold();
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_COMPARE(work.begun(), 1);
        QTRY_VERIFY(theOne(desk.window, u"ownExportWorking"_s) != nullptr);
        // "Cancel" while the file is made: the box is gone at once, and waits for nothing. The
        // making itself is not ended: it goes on, with nobody waiting for it.
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QVERIFY(!desk.ownFiles.isBusy());
        QVERIFY(!desk.ownFiles.holdsItsWindow());
        QVERIFY(desk.exchange.isBusy());
        // Whoever asks for a file meanwhile is told that one is being worked on, which passes.
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownExportNote"_s) != nullptr);
        QCOMPARE(theOne(desk.window, u"ownExportNote"_s)->property("text").toString(),
                 u"Another file is being worked on just now, and one is worked on at a time. That passes: try again "
                 "in a moment."_s);
        QVERIFY(theOne(desk.window, u"ownExportProblem"_s) == nullptr);
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        // What the work comes to afterwards is nobody's.
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.exchange.isBusy(), 20000);
        QTest::qWait(100);
        QVERIFY(!desk.ownFiles.isMade());
        QVERIFY(toldBy(desk.actions).isEmpty());
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- import, through -------------------------------------------------------------------------

    void kvmsOfAFileAreTakenOntoTheWallByTheButtonsAPersonPresses()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(elsewhere(11), u"There already"_s).isEmpty());
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // Nothing chosen yet: nothing to read, no passphrase asked for, no list.
        QVERIFY(!theOne(desk.window, u"ownImportRead"_s)->isEnabled());
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportTake"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportAbove"_s) == nullptr);

        // A file is chosen in Windows' own box. It says of itself that it is sealed, and only
        // then is its passphrase asked for.
        const Device pinned = knownBy(kvmAt(elsewhere(1), u"Rack 1, top"_s), "first");
        const QByteArray sealed =
            fileOf({pinned, ticked(2, u"By Windows"_s), kvmAt(u"http://192.0.2.4:8080"_s, u"In the clear"_s),
                    notKeptHere(ticked(9, u"A small one"_s)), ticked(11, u"Twice"_s)});
        desk.opens = desk.write(u"wall.qkvm"_s, sealed);
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QCOMPARE(desk.opened, 1);
        QTRY_VERIFY(says(desk.window, u"wall.qkvm"_s));
        const QQuickItem* passphrase = theOne(desk.window, u"ownImportPassphrase"_s);
        QVERIFY(passphrase != nullptr);
        QVERIFY(says(desk.window, u"Passphrase of the file"_s));
        QVERIFY(theOne(desk.window, u"ownImportRead"_s)->isEnabled());
        // A wrong passphrase: said in the server's words, and nothing is laid out. What was
        // typed stays in its line, to be put right.
        clickOn(desk.window, passphrase);
        type(desk.window, u"not the passphrase"_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY(says(desk.window, u"That is not the passphrase this file was made with"_s));
        QVERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportTake"_s) == nullptr);
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QCOMPARE(passphrase->property("text").toString(), u"not the passphrase"_s);
        // The right one, with Enter in its line.
        clickOn(desk.window, passphrase);
        QTest::keyClick(desk.window, Qt::Key_A, Qt::ControlModifier);
        type(desk.window, thePassphrase());
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        // (Nothing of the list takes a click until it has stood for its moment.)
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);

        // Laid out, and nothing on the wall has changed. The passphrase is asked for no more,
        // and is out of its line.
        QCOMPARE(own.names(), QStringList{u"There already"_s});
        QTRY_VERIFY(theOne(desk.window, u"ownImportOffered"_s) != nullptr);
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        QCOMPARE(passphrase->property("text").toString(), QString());
        QVERIFY(theOne(desk.window, u"ownImportRead"_s) == nullptr);
        aFrame(desk.window);
        // Above the list, once: how many devices the file has, how many are ticked, how many
        // are not and want a look, how many cannot be taken; that nothing has changed yet, nor
        // was anything asked anywhere; and that each KVM that is taken is asked what it is
        // before a password goes to it.
        const QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        const QQuickItem* counts = theOne(desk.window, u"ownImportCounts"_s);
        QVERIFY(counts != nullptr);
        QCOMPARE(
            counts->property("text").toString(),
            u"5 devices of this file are in this list. 2 are ticked. 1 is not ticked and wants a look. 2 cannot be "
            "taken."_s);
        const QString once = u"Nothing on the wall has changed yet, and nothing was asked at any address. Each KVM "
                             "that is taken is asked what it is before a password goes to it."_s;
        QVERIFY(says(desk.window, once));
        QVERIFY(isInside(desk.window, counts));
        // (It is no row of the list: it is not scrolled away with it.)
        for (const QQuickItem* up = counts; up != nullptr; up = up->parentItem()) {
            QVERIFY2(up != list, "what stands above the list is in the list");
        }

        // Every device of the file in the file's order, all five in sight at once, each a
        // short row: its name and address in one piece, and one line of a few words. What is
        // said of every device alike is said of none.
        for (int place = 1; place <= 5; ++place) {
            const QQuickItem* row = rowOf(desk.window, place);
            QVERIFY2(row != nullptr, qPrintable(QString::number(place)));
            QVERIFY2(isWholeIn(list, row), qPrintable(QString::number(place)));
            QVERIFY2(row->height() < 60, qPrintable(u"%1: %2"_s.arg(place).arg(row->height())));
            QVERIFY(!isSeen(inRow(row, u"ownImportBehind"_s)));
        }
        QStringList inTheList;
        everyTextIn(list, inTheList);
        for (const QString& said : std::as_const(inTheList)) {
            QVERIFY2(!said.contains(u"asked"_s), qPrintable(said));
            QVERIFY2(!said.contains(u"SHA-256"_s), qPrintable(said));
        }
        const QQuickItem* first = rowOf(desk.window, 1);
        QCOMPARE(saidIn(first, u"ownImportNameAndAddress"_s), u"\"Rack 1, top\" at https://192.0.2.1"_s);
        QCOMPARE(saidIn(first, u"ownImportInShort"_s), u"Known by its certificate "_s + inShortOf(pinned.certificate));
        QCOMPARE(inRow(first, u"ownImportInShort"_s)->property("color").value<QColor>(), desk.colour("muted"));
        QVERIFY(isSeen(inRow(first, u"ownImportTick"_s)));
        QVERIFY(inRow(first, u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(saidIn(rowOf(desk.window, 2), u"ownImportInShort"_s), u"Known by Windows' word"_s);
        // Which are ticked to begin with: not the one whose password goes in the clear. Why
        // not stands on its row, in sight, in the colour of a warning; its tick is there to be
        // set.
        QCOMPARE(desk.ownFiles.tickedCount(), 2);
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QCOMPARE(take->property("text").toString(), u"Take 2 KVMs"_s);
        const QQuickItem* plain = rowOf(desk.window, 3);
        QVERIFY(isSeen(inRow(plain, u"ownImportTick"_s)));
        QVERIFY(inRow(plain, u"ownImportTick"_s)->isEnabled());
        QVERIFY(!inRow(plain, u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(saidIn(plain, u"ownImportInShort"_s), u"Plain HTTP: its password goes in the clear"_s);
        QVERIFY(isSeen(inRow(plain, u"ownImportInShort"_s)));
        QCOMPARE(inRow(plain, u"ownImportInShort"_s)->property("color").value<QColor>(), desk.colour("warn"));
        QCOMPARE(inRow(plain, u"ownImportNameAndAddress"_s)->property("color").value<QColor>(), desk.colour("text"));
        // One that cannot be taken is shown all the same, with why, and does not look like one
        // that waits to be ticked: it has no tick at all, and is grey. A click where its tick
        // would be does nothing.
        const QQuickItem* small = rowOf(desk.window, 4);
        QCOMPARE(saidIn(small, u"ownImportInShort"_s),
                 u"Cannot be taken: this PC's wall does not keep it as the file has it"_s);
        QVERIFY(!isSeen(inRow(small, u"ownImportTick"_s)));
        QVERIFY(isSeen(inRow(small, u"ownImportNoTick"_s)));
        QVERIFY(!isSeen(inRow(plain, u"ownImportNoTick"_s)));
        QCOMPARE(inRow(small, u"ownImportInShort"_s)->property("color").value<QColor>(), desk.colour("muted"));
        QCOMPARE(inRow(small, u"ownImportNameAndAddress"_s)->property("color").value<QColor>(), desk.colour("muted"));
        aFrame(desk.window);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, middleOf(inRow(small, u"ownImportNoTick"_s)));
        const QQuickItem* twice = rowOf(desk.window, 5);
        QCOMPARE(saidIn(twice, u"ownImportInShort"_s),
                 u"Cannot be taken: on the wall already, or earlier in this file"_s);
        QTest::mouseClick(desk.window, Qt::LeftButton, {},
                          inRow(twice, u"ownImportNoTick"_s)->mapToScene(QPointF(5, 1)).toPoint());
        QTest::qWait(50);
        QCOMPARE(desk.ownFiles.tickedCount(), 2);
        // (Which KVM of the wall is in its way is behind its row, as a sentence.)
        clickOn(desk.window, inRow(twice, u"ownImportDetails"_s));
        QTRY_VERIFY(isSeen(inRow(twice, u"ownImportBehind"_s)));
        QCOMPARE(saidIn(twice, u"ownImportVerdict"_s),
                 u"It cannot be taken: it is on the wall already (\"There already\" at https://192.0.2.11)."_s);
        // Ticked by whoever wants it; and the tick of one that is not wanted taken away. What
        // stands above the list follows, and so does the button. (Once the list has stood for
        // its moment again: a row that is opened moves those under it.)
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, inRow(plain, u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 3);
        QVERIFY(inRow(plain, u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(take->property("text").toString(), u"Take 3 KVMs"_s);
        QCOMPARE(counts->property("text").toString(),
                 u"5 devices of this file are in this list. 3 are ticked. 2 cannot be taken."_s);
        clickOn(desk.window, inRow(rowOf(desk.window, 2), u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 2);
        QVERIFY(!inRow(rowOf(desk.window, 2), u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(take->property("text").toString(), u"Take 2 KVMs"_s);
        QCOMPARE(counts->property("text").toString(),
                 u"5 devices of this file are in this list. 2 are ticked. 2 cannot be taken."_s);

        // "Take 2 KVMs": those two are on the wall, the box closes, and its window's corner says
        // how many, and nothing a second time.
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(own.names(), (QStringList{u"There already"_s, u"Rack 1, top"_s, u"In the clear"_s}));
        QCOMPARE(toldBy(desk.actions), QStringList{u"ok: 2 KVMs were taken onto this wall."_s});
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QVERIFY(!desk.ownFiles.isLaidOut());
        // Opened again, the box begins anew.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        QVERIFY(!says(desk.window, u"wall.qkvm"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void nineDevicesOfAnOrdinaryFileAreInSightAtOnceWithTheirTicks()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(1280, 800) != nullptr);
        QCOMPARE(desk.window->size(), QSize(1280, 800));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, anOrdinaryFile(), thePassphrase()));
        QTRY_VERIFY(rowOf(desk.window, 9) != nullptr);
        aFrame(desk.window);
        const QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        // All of the list is in sight: there is nothing to scroll, and nothing says there is.
        QVERIFY2(mostScrolled(list) < 0.5, qPrintable(QString::number(mostScrolled(list))));
        QVERIFY(theOne(desk.window, u"ownImportScrollMark"_s) == nullptr);
        QVERIFY(isInside(desk.window, list));
        QCOMPARE(deviceRows(desk.window).size(), 9);
        // Each of the nine, whole: its name and address, its one line, and its tick, or what
        // stands where a tick would be.
        for (int place = 1; place <= 9; ++place) {
            const QQuickItem* row = rowOf(desk.window, place);
            QVERIFY2(row != nullptr, qPrintable(QString::number(place)));
            QVERIFY2(isWholeIn(list, row), qPrintable(QString::number(place)));
            const QQuickItem* name = inRow(row, u"ownImportNameAndAddress"_s);
            QVERIFY(name != nullptr && isSeen(name) && !name->property("truncated").toBool());
            QVERIFY(isSeen(inRow(row, u"ownImportInShort"_s)));
            const QQuickItem* tick =
                inRow(row, row->property("canBeTaken").toBool() ? u"ownImportTick"_s : u"ownImportNoTick"_s);
            QVERIFY2(tick != nullptr && isSeen(tick) && isWholeIn(list, tick), qPrintable(QString::number(place)));
        }
        // What each says in short, in the file's order.
        QStringList inShort;
        for (int place = 1; place <= 9; ++place) {
            inShort.append(saidIn(rowOf(desk.window, place), u"ownImportInShort"_s));
        }
        const QString byCertificate = u"Known by its certificate "_s;
        QCOMPARE(inShort,
                 (QStringList{byCertificate + inShortOf(knownBy(kvmAt(elsewhere(1)), "first").certificate),
                              byCertificate + inShortOf(knownBy(kvmAt(elsewhere(2)), "second").certificate),
                              byCertificate + inShortOf(knownBy(kvmAt(elsewhere(3)), "comet").certificate),
                              u"Known by Windows' word"_s, u"Known by Windows' word"_s,
                              u"Known by Windows' word, not by what it shows first"_s,
                              u"Plain HTTP: its password goes in the clear"_s, u"Its address is this PC itself"_s,
                              u"Cannot be taken: this PC's wall does not keep it as the file has it"_s}));
        QCOMPARE(
            theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
            u"9 devices of this file are in this list. 6 are ticked. 2 are not ticked and want a look. 1 cannot be "
            "taken."_s);
        QCOMPARE(theOne(desk.window, u"ownImportTake"_s)->property("text").toString(), u"Take 6 KVMs"_s);
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportTake"_s)));
        keepPictureOf(desk.window, u"own-import-nine-devices"_s);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatStandsBehindARowIsOpenedByItsDetailsAndNothingOfADeviceIsLost()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // The first of forty is known by the certificate its file names; the second would be
        // known by what it shows first, to a server.
        const Device pinned = knownBy(kvmAt(elsewhere(1), u"Rack 1, top"_s), "first");
        QList<Device> devices{pinned, saying(kvmAt(elsewhere(2), u"Not checked"_s), u"insecure"_s)};
        for (int number = 3; number <= 40; ++number) {
            devices.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        QVERIFY(chooseAndRead(desk, fileOf(devices), thePassphrase()));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        const QQuickItem* first = rowOf(desk.window, 1);
        const qreal closed = first->height();
        // Closed to begin with: its name and address, and its one line; and "Details".
        QVERIFY(!isSeen(inRow(first, u"ownImportBehind"_s)));
        QVERIFY(!isSeen(inRow(first, u"ownImportFingerprint"_s)));
        const QQuickItem* details = inRow(first, u"ownImportDetails"_s);
        QVERIFY(details != nullptr && isSeen(details));
        QCOMPARE(details->property("text").toString(), u"Details"_s);

        // Opened: everything else that is to be seen of it before it is taken. Whether it can
        // be taken and as what, as a sentence; every sentence that is said of it, each by
        // itself, in the core's order; and the SHA-256 of the certificate its file names,
        // whole, in the letters the box that changes a KVM shows one in: four lines of eight
        // pairs, to be compared by eye.
        clickOn(desk.window, details);
        QTRY_VERIFY(isSeen(inRow(first, u"ownImportBehind"_s)));
        QVERIFY(first->property("open").toBool());
        QVERIFY(details->property("checked").toBool());
        // (How high a row is with what stands behind it is known when it was laid out anew.)
        QTRY_VERIFY2(first->height() > closed + 100, qPrintable(QString::number(first->height())));
        QCOMPARE(saidIn(first, u"ownImportInShort"_s), u"Known by its certificate "_s + inShortOf(pinned.certificate));
        QCOMPARE(saidIn(first, u"ownImportVerdict"_s),
                 u"It can be taken, as a KVM that runs kvmd (PiKVM, GL.iNet Comet)."_s);
        QStringList saidOfIt;
        QList<const QQuickItem*> sentences;
        named(first, u"ownImportSays"_s, sentences);
        for (const QQuickItem* sentence : std::as_const(sentences)) {
            QVERIFY(isSeen(sentence));
            QVERIFY(!sentence->property("truncated").toBool());
            saidOfIt.append(sentence->property("text").toString());
        }
        QCOMPARE(saidOfIt,
                 (QStringList{u"The file names its certificate. It will be known by that certificate and by no "
                              "other."_s,
                              u"The file has its password. It is kept with Windows, for this address and for what "
                              "the KVM is known by."_s}));
        QVERIFY(says(desk.window, u"SHA-256 of the certificate the file names, to compare with what the KVM's own "
                                  "page shows:"_s));
        const QQuickItem* fingerprint = inRow(first, u"ownImportFingerprint"_s);
        QVERIFY(fingerprint != nullptr && isSeen(fingerprint));
        QCOMPARE(fingerprint->property("text").toString().replace(u'\n', u':'),
                 core::kvm::fingerprint(pinned.certificate));
        QCOMPARE(fingerprint->property("text").toString().count(u'\n'), 3);
        QCOMPARE(fingerprint->property("font").value<QFont>().family(), u"Consolas"_s);
        QVERIFY(!fingerprint->property("truncated").toBool());
        QVERIFY(isWholeIn(list, fingerprint));
        keepPictureOf(desk.window, u"own-import-row-opened"_s);
        // Its tick is as it was, and is set as before.
        QVERIFY(inRow(first, u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(desk.ownFiles.tickedCount(), 40);

        // The second, opened: what a server would know it by, and what this program does
        // instead, as the whole sentence. On its row, that in a few words.
        const QQuickItem* second = rowOf(desk.window, 2);
        QVERIFY(second != nullptr);
        QCOMPARE(saidIn(second, u"ownImportInShort"_s), u"Known by Windows' word, not by what it shows first"_s);
        // (Once the list has stood for its moment again: the first one, opened, moved it.)
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, inRow(second, u"ownImportDetails"_s));
        QTRY_VERIFY(isSeen(inRow(second, u"ownImportBehind"_s)));
        sentences.clear();
        named(second, u"ownImportSays"_s, sentences);
        QCOMPARE(sentences.size(), 2);
        QVERIFY2(sentences.constFirst()->property("text").toString().startsWith(
                     u"The file says that its certificate is not checked. This program does not take the first "
                     "certificate a device from a file shows."_s),
                 qPrintable(sentences.constFirst()->property("text").toString()));
        QVERIFY(!isSeen(inRow(second, u"ownImportFingerprint"_s)));

        // What is opened is the program's to keep, as a tick is: scrolled out and in, the row
        // is as it was left.
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(rowOf(desk.window, 40) != nullptr);
        aFrame(desk.window);
        QTRY_VERIFY(rowOf(desk.window, 1) == nullptr);
        QVERIFY(!isSeen(inRow(rowOf(desk.window, 40), u"ownImportBehind"_s)));
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        first = rowOf(desk.window, 1);
        QVERIFY(isSeen(inRow(first, u"ownImportBehind"_s)));
        QVERIFY(isSeen(inRow(first, u"ownImportFingerprint"_s)));
        // Closed again by the same button.
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, inRow(first, u"ownImportDetails"_s));
        QTRY_VERIFY(!isSeen(inRow(first, u"ownImportBehind"_s)));
        QTRY_VERIFY2(qAbs(first->height() - closed) < 0.5, qPrintable(QString::number(first->height())));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aTickIsInSightWheneverAnyOfItsRowIs()
    {
        // The smallest window: the list has room for three short rows, and a row that is
        // opened is higher than the list.
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(640, 400) != nullptr);
        QCOMPARE(desk.window->size(), QSize(640, 400));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, anOrdinaryFile(), thePassphrase()));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        QVERIFY(isInside(desk.window, list));
        // What is looked at wherever the list stands: of every device that can be taken and has
        // any of its row in sight, as much of its tick is in sight as of its row can be.
        int looks = 0;
        const auto everyTickIsInSight = [&](const QString& when) -> QString {
            for (const QQuickItem* row : deviceRows(desk.window)) {
                const qreal ofTheRow = heightInSight(list, row);
                if (ofTheRow < 0.5 || !row->property("canBeTaken").toBool()) {
                    continue;
                }
                ++looks;
                const QQuickItem* tick = inRow(row, u"ownImportTick"_s);
                if (tick == nullptr || !isSeen(tick)) {
                    return u"%1: device %2 has %3 px of its row in sight, and no tick at all"_s.arg(when)
                        .arg(row->property("place").toInt())
                        .arg(ofTheRow);
                }
                const qreal ofTheTick = heightInSight(list, tick);
                if (ofTheTick < std::min(tick->height(), ofTheRow) - 1.0) {
                    return u"%1: device %2 has %3 px of its row in sight, and %4 of the %5 px of its tick"_s.arg(when)
                        .arg(row->property("place").toInt())
                        .arg(ofTheRow)
                        .arg(ofTheTick)
                        .arg(tick->height());
                }
            }
            return {};
        };
        // Through the whole list, a few pixels at a time, with every row closed.
        const auto throughTheList = [&](const QString& when) -> QString {
            for (qreal at = 0;; at += 3) {
                const qreal to = std::min(at, mostScrolled(list));
                scrollTo(list, to);
                if (const QString wrong = everyTickIsInSight(u"%1, scrolled by %2"_s.arg(when).arg(to));
                    !wrong.isEmpty()) {
                    return wrong;
                }
                if (to >= mostScrolled(list) || at > 20000) {
                    return {};
                }
            }
        };
        QVERIFY2(mostScrolled(list) > 100, qPrintable(QString::number(mostScrolled(list))));
        QString wrong = throughTheList(u"rows closed"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        QVERIFY2(looks > 100, qPrintable(QString::number(looks)));
        // (A picture of a row that is in sight in part, at each edge of the list.)
        scrollTo(list, 20);
        keepPictureOf(desk.window, u"own-import-small-row-in-part"_s);

        // The first row opened: it is higher than the list now. While its top is out of sight
        // and more of it is left than a tick is high, its tick stands whole at the list's upper
        // edge, to be seen and to be pressed.
        scrollTo(list, 0);
        aFrame(desk.window);
        clickOn(desk.window, inRow(rowOf(desk.window, 1), u"ownImportDetails"_s));
        QTRY_VERIFY(isSeen(inRow(rowOf(desk.window, 1), u"ownImportBehind"_s)));
        aFrame(desk.window);
        const QQuickItem* first = rowOf(desk.window, 1);
        QVERIFY2(first->height() > list->height() + 40,
                 qPrintable(u"%1 in %2"_s.arg(first->height()).arg(list->height())));
        looks = 0;
        wrong = throughTheList(u"the first row opened"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        // (Back to its top, and down again a little at a time, as a person scrolls: a list that is
        // sent to a far place at once reckons where its rows are from how high they are on
        // average, and one of them is six times as high as the others.)
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        aFrame(desk.window);
        for (int scrolled = 5; scrolled <= 70; scrolled += 5) {
            scrollTo(list, scrolled);
        }
        aFrame(desk.window);
        first = rowOf(desk.window, 1);
        QVERIFY(first != nullptr);
        const QQuickItem* tick = inRow(first, u"ownImportTick"_s);
        const QRectF where = tick->mapRectToItem(list, QRectF(0, 0, tick->width(), tick->height()));
        QVERIFY2(qAbs(where.top()) < 1.0, qPrintable(QString::number(where.top())));
        QVERIFY(isWholeIn(list, tick));
        keepPictureOf(desk.window, u"own-import-small-row-opened"_s);
        // Pressed there, it is the first one's tick that is taken away.
        QVERIFY(tick->property("checked").toBool());
        QCOMPARE(desk.ownFiles.tickedCount(), 6);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, tick);
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 5);
        QVERIFY(!tick->property("checked").toBool());
        QVERIFY(!desk.ownFiles.offered()->tickedPlaces().contains(1));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void anOpenedRowTallerThanTheListKeepsTheDevicesNameWithItsTick()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(640, 400) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, anOrdinaryFile(), thePassphrase()));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        // The first row opened: it is higher than the list. Scrolled down into it, as a person
        // reads down it, a little at a time.
        clickOn(desk.window, inRow(rowOf(desk.window, 1), u"ownImportDetails"_s));
        QTRY_VERIFY(isSeen(inRow(rowOf(desk.window, 1), u"ownImportBehind"_s)));
        aFrame(desk.window);
        QVERIFY(rowOf(desk.window, 1)->height() > list->height() + 40);
        const QString called = u"\"Rack 1, top\" at https://192.0.2.1"_s;
        // (Not before its top has gone: then its name stands where it stands, once.)
        QVERIFY(!isSeen(inRow(rowOf(desk.window, 1), u"ownImportStuckName"_s)));
        for (int scrolled = 5; scrolled <= 70; scrolled += 5) {
            scrollTo(list, scrolled);
        }
        aFrame(desk.window);
        const QQuickItem* first = rowOf(desk.window, 1);
        QVERIFY(first != nullptr);
        // Its own line with the name is scrolled out over the list's upper edge; the tick stands
        // at that edge, and the device's name and address with it, whole: what is ticked and
        // what is read below it are never of two devices that the eye cannot tell apart.
        QVERIFY(heightInSight(list, inRow(first, u"ownImportNameAndAddress"_s)) < 0.5);
        const QQuickItem* tick = inRow(first, u"ownImportTick"_s);
        QVERIFY(tick != nullptr && isWholeIn(list, tick));
        const QQuickItem* stuck = inRow(first, u"ownImportStuckName"_s);
        QVERIFY2(stuck != nullptr && isSeen(stuck), "the tick stands at the list's edge without the device's name");
        keepPictureOf(desk.window, u"own-import-small-row-opened-name-kept"_s);
        const QQuickItem* name = saying(stuck, called);
        QVERIFY2(name != nullptr, qPrintable(saidIn(stuck, u"ownImportStuckNameText"_s)));
        QVERIFY(isWholeIn(list, name));
        QVERIFY(!name->property("truncated").toBool());
        const QRectF ofTheTick = tick->mapRectToItem(list, QRectF(0, 0, tick->width(), tick->height()));
        const QRectF ofTheName = name->mapRectToItem(list, QRectF(0, 0, name->width(), name->height()));
        QVERIFY2(qAbs(ofTheTick.top()) < 1.0 && ofTheName.top() < ofTheTick.bottom(),
                 qPrintable(u"the tick at %1, the name at %2"_s.arg(ofTheTick.top()).arg(ofTheName.top())));
        QVERIFY(ofTheName.left() >= ofTheTick.right());
        // (And what scrolls under it is not seen through it.)
        QCOMPARE(stuck->property("color").value<QColor>(), desk.colour("panel"));
        // Scrolled back to the row's top, it is gone: its name stands once.
        scrollTo(list, 0);
        aFrame(desk.window);
        QVERIFY(!isSeen(inRow(rowOf(desk.window, 1), u"ownImportStuckName"_s)));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theMarkOfATickIsDrawnWithinTheListAndNotOverWhatLiesBeyondItsEdge()
    {
        // The tests' pictures are drawn without a graphics card, by Qt Quick's own drawing in
        // software. There, a drawing of lines (the mark of a tick is one) was painted whole over
        // what lies beyond the edge of the list it is in, where its box was half scrolled out.
        // Looked at here in the picture itself: the strip of the box just above the list, and
        // the one just under it, are the same whatever the list is scrolled to.
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QList<Device> devices;
        for (int number = 1; number <= 30; ++number) {
            devices.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        QVERIFY(chooseAndRead(desk, fileOf(devices, false)));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(mostScrolled(list) > 200);
        const QPoint corner = list->mapToScene(QPointF(0, 0)).toPoint();
        const int width = 40;
        const int height = 12;
        const QRect above(corner.x(), corner.y() - height - 1, width, height);
        const QRect under(corner.x(), corner.y() + qRound(list->height()) + 1, width, height);
        QVERIFY(above.top() > 0 && under.bottom() < desk.window->height());
        const auto picture = [&] {
            aFrame(desk.window);
            return desk.window->grabWindow().convertToFormat(QImage::Format_RGB32);
        };
        const QImage atItsTop = picture();
        // (The mark is drawn at all: lighter than anything else in the tick of the first row, where
        // all of it is in sight. A mark that is not drawn would not be drawn beyond any edge either.)
        const QQuickItem* tick = inRow(rowOf(desk.window, 1), u"ownImportTick"_s);
        const QRect ofTheTick = tick->mapRectToScene(QRectF(0, 0, tick->width(), tick->height())).toRect();
        int white = 0;
        for (int y = ofTheTick.top(); y <= ofTheTick.bottom(); ++y) {
            for (int x = ofTheTick.left(); x <= ofTheTick.right(); ++x) {
                const QColor one = atItsTop.pixelColor(x, y);
                white += one.red() > 120 && one.green() > 140 ? 1 : 0;
            }
        }
        QVERIFY2(white >= 6, qPrintable(u"%1 pixels of the mark"_s.arg(white)));
        // Scrolled a pixel at a time through more than one row and the gap after it: every way
        // in which a tick is cut off by the list's upper edge and by its lower one.
        const auto differing = [&](const QImage& now, const QRect& strip) {
            int pixels = 0;
            for (int y = strip.top(); y <= strip.bottom(); ++y) {
                for (int x = strip.left(); x <= strip.right(); ++x) {
                    pixels += now.pixel(x, y) != atItsTop.pixel(x, y) ? 1 : 0;
                }
            }
            return pixels;
        };
        for (int scrolled = 1; scrolled <= 60; ++scrolled) {
            scrollTo(list, scrolled);
            const QImage now = picture();
            const int over = differing(now, above);
            const int below = differing(now, under);
            if (over > 0 || below > 0) {
                keepPictureOf(desk.window, u"own-import-mark-beyond-the-edge"_s);
            }
            QVERIFY2(over == 0, qPrintable(u"scrolled by %1: %2 pixels just above the list's edge are not the "
                                           "box's own any more"_s.arg(scrolled)
                                               .arg(over)));
            QVERIFY2(below == 0, qPrintable(u"scrolled by %1: %2 pixels just under the list's edge are not the "
                                            "box's own any more"_s.arg(scrolled)
                                                .arg(below)));
        }
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theAddressOfADeviceWithTheLongestNameIsOnShowWhole_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("a window of the usual size") << QSize(1280, 800);
        QTest::newRow("the smallest window") << QSize(640, 400);
    }

    void theAddressOfADeviceWithTheLongestNameIsOnShowWhole()
    {
        QFETCH(QSize, size);
        // The longest name a file holds (eighty signs: a device with a longer one is left out by
        // the reader), of the widest letters there are, and an address of two hundred signs.
        const QString name = QString(80, u'W');
        QStringList parts;
        for (int part = 0; part < 4; ++part) {
            parts.append(QString(46, QChar(u'a' + part)));
        }
        const QString address = u"https://"_s + parts.join(u'.') + u".invalid:8443"_s;
        QVERIFY(address.size() >= 200);
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({saying(kvmAt(address, name), u"system"_s)}, false)));
        aFrame(desk.window);
        const QQuickItem* row = rowOf(desk.window, 1);
        QVERIFY(row != nullptr);
        const QQuickItem* said = inRow(row, u"ownImportNameAndAddress"_s);
        QVERIFY(said != nullptr && isSeen(said));
        // In one piece, as it is handed over: the name set off, then the address.
        const QString whole = u'"' + name + u"\" at "_s + address;
        QCOMPARE(said->property("text").toString(), whole);
        // Whole: broken into lines, each within the row, and cut short nowhere.
        QVERIFY(!said->property("truncated").toBool());
        QCOMPARE(said->property("elide").toInt(), 3);
        QVERIFY2(said->property("lineCount").toInt() >= 3, qPrintable(said->property("lineCount").toString()));
        QVERIFY2(said->property("contentWidth").toReal() <= said->width() + 0.5,
                 qPrintable(u"%1 in %2"_s.arg(said->property("contentWidth").toReal()).arg(said->width())));
        QVERIFY(said->height() >= said->property("contentHeight").toReal() - 0.5);
        // (It does not run under the row's button.)
        const QQuickItem* details = inRow(row, u"ownImportDetails"_s);
        QVERIFY(details != nullptr);
        QVERIFY(said->mapRectToItem(row, QRectF(0, 0, said->width(), said->height())).right() <=
                details->mapRectToItem(row, QRectF(0, 0, details->width(), details->height())).left());
        // And in sight to its last sign, where the list is scrolled to its top: the row's own
        // letters lie within the list, which lies within the box, which lies within the window.
        const QQuickItem* list = theOne(desk.window, u"ownImportOffered"_s);
        QVERIFY(list != nullptr);
        const QRectF letters = said->mapRectToItem(list, QRectF(0, 0, said->width(), said->height()));
        QVERIFY2(letters.left() >= 0 && letters.right() <= list->width() && letters.top() >= 0 &&
                     letters.bottom() <= list->height(),
                 qPrintable(u"%1,%2 %3x%4 in %5x%6"_s.arg(letters.x())
                                .arg(letters.y())
                                .arg(letters.width())
                                .arg(letters.height())
                                .arg(list->width())
                                .arg(list->height())));
        QVERIFY(isInside(desk.window, list));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportCancel"_s)));
        keepPictureOf(desk.window, u"own-import-long-name-%1x%2"_s.arg(size.width()).arg(size.height()));
        // One is ticked, and the button says so; with none ticked it takes no click, and says
        // that it would take none.
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QCOMPARE(take->property("text").toString(), u"Take 1 KVM"_s);
        QVERIFY(isInside(desk.window, take));
        clickOn(desk.window, inRow(row, u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 0);
        QTRY_COMPARE(take->property("text").toString(), u"Take 0 KVMs"_s);
        QVERIFY(isSeen(take));
        QVERIFY(!take->isEnabled());
        QCOMPARE(theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
                 u"1 device of this file is in this list. None is ticked. The file has no password for any of them."_s);
        QTest::qWait(Desk::pastAQuickRest);
        desk.ownFiles.take();
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theClickThatReadsTheFileDoesNotTake()
    {
        WallOfOneTest own;
        Desk desk(own);
        // (A long moment before "Take" takes a click: this test looks at it not taking one, and
        // has seconds to do that in.)
        const int rest = Desk::longRest;
        desk.ownFiles.restFor(rest, rest);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        desk.opens = desk.write(u"wall.json"_s, fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)}, false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        // A plain file asks for no passphrase.
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        const QQuickItem* read = theOne(desk.window, u"ownImportRead"_s);
        QVERIFY(read != nullptr && read->isEnabled());
        aFrame(desk.window);
        const QPoint where = middleOf(read);

        // A double click on "Read the file". The first click reads; the list is there a moment
        // later, with "Take 2 KVMs" in the place "Read the file" had in the box's row of
        // buttons. (Where that is in the window depends on how high the box has become, which
        // stands in the middle of its window: in a low window it is the very same place.)
        QElapsedTimer since;
        since.start();
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, where);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(theOne(desk.window, u"ownImportTake"_s) != nullptr);
        aFrame(desk.window);
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        const QRectF takes = take->mapRectToScene(QRectF(0, 0, take->width(), take->height()));
        const QRectF readStood = QRectF(where, QSizeF(1, 1));
        QVERIFY2(takes.left() <= readStood.left() && takes.right() >= readStood.right(),
                 "\"Take\" does not stand in the place \"Read the file\" had in its row");
        // It takes no click in that moment, and looks so; and the program takes nothing: not by
        // the second click where the first one was, and not by one on the button where it is now.
        QVERIFY(!take->isEnabled());
        QVERIFY(!desk.ownFiles.mayTake());
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, where);
        QTest::mouseDClick(desk.window, Qt::LeftButton, {}, where);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, middleOf(take));
        QTest::mouseDClick(desk.window, Qt::LeftButton, {}, middleOf(take));
        QTest::qWait(100);
        // (All of it in the moment: of four seconds, this is to have taken a fraction.)
        QVERIFY2(since.elapsed() < rest, qPrintable(QString::number(since.elapsed())));
        // (Neither took a tick away, either: the list is as it was.)
        QCOMPARE(desk.ownFiles.tickedCount(), 2);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QVERIFY(desk.ownFiles.isLaidOut());
        QVERIFY(toldBy(desk.actions).isEmpty());
        // Nor does a key that was meant for the line or the button before: the keyboard is at
        // the list, and at no button.
        QVERIFY(!take->hasActiveFocus());
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTest::keyClick(desk.window, Qt::Key_Space);
        QVERIFY(own.wall.kvms().isEmpty());

        // Looked at for its moment, it takes. (All of the moment, but for what a timer of
        // Windows' may be early by: a twentieth, and here a tenth is let pass.)
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 30000);
        QVERIFY2(since.elapsed() >= rest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(own.names(), (QStringList{u"One"_s, u"Two"_s}));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void noButtonOfEitherBoxComesToStandWhereAnotherStood_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("the smallest window") << QSize(640, 400);
        QTest::newRow("a window of 1280 by 800") << QSize(1280, 800);
    }

    void noButtonOfEitherBoxComesToStandWhereAnotherStood()
    {
        // Whatever comes, goes, opens or closes in a box, what a person was pointing at a moment
        // before is where it was, or nothing that takes a click stands there now: the box keeps
        // its size and its place, its buttons stand at its foot, and what changes in the list
        // changes nothing outside the list. Looked at after each change the box knows, here and
        // in the next test: in the smallest window, and in one of the usual size.
        QFETCH(QSize, size);
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        desk.ownFiles.restFor(Desk::heldRest, Desk::heldRest);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QCOMPARE(desk.window->size(), size);
        const QString at = u", at %1 by %2"_s.arg(size.width()).arg(size.height());
        const auto keep = [&](const QString& name) {
            keepPictureOf(desk.window, u"own-still-%1x%2-%3"_s.arg(size.width()).arg(size.height()).arg(name));
        };
        const QString box = u"ownImportBox"_s;
        QStringList wrong;
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));

        // A short list, of as many devices as the list has room for: a list comes; and a row of
        // it is opened and closed at its top, in its middle and at its end (where the last one is
        // read down to before it is closed).
        const int few = size.height() < 500 ? 2 : 3;
        QList<Device> shortOne;
        for (int number = 1; number <= few; ++number) {
            shortOne.append(ticked(number, u"Short %1"_s.arg(number)));
        }
        desk.opens = desk.write(u"short.json"_s, fileOf(shortOne, false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QVERIFY(!desk.ownFiles.fileName().isEmpty());
        Seen before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(rowOf(desk.window, few) != nullptr);
        wrong += movedBy(before, seenOf(desk, box), u"a short list comes"_s + at);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        QVERIFY2(mostScrolled(listOf(desk)) < 0.5, qPrintable(QString::number(mostScrolled(listOf(desk)))));
        const std::function<void(const QString&)> keepShort = [&](const QString& step) {
            keep(u"short-row-"_s + step);
        };
        for (int place = 1; place <= few; ++place) {
            const QString where = place == 1 ? u"top"_s : place == few ? u"end"_s : u"middle"_s;
            const QString failed =
                openAndClose(desk, place, u"a row at the %1 of a short list"_s.arg(where) + at, wrong, place == few,
                             place == 1 ? keepShort : std::function<void(const QString&)>());
            QVERIFY2(failed.isEmpty(), qPrintable(failed));
        }
        // The row at the end was read down to and closed again: in the smallest window the list
        // stands where it stood, with room left after its end and its first row in part above its
        // upper edge. Wherever the list is not at its beginning, its mark says that there is more.
        QQuickItem* shortList = listOf(desk);
        keep(u"short-row-end-closed"_s);
        if (size.height() < 500) {
            QVERIFY2(scrolledBy(shortList) > 0.5, qPrintable(QString::number(scrolledBy(shortList))));
        }
        if (scrolledBy(shortList) > 0.5) {
            QVERIFY2(
                theOne(desk.window, u"ownImportScrollMark"_s) != nullptr,
                qPrintable(u"the list stands %1 from its beginning, and no mark says so"_s.arg(scrolledBy(shortList))));
        }
        // The keys while that room is left: Down and PageDown go no further than what is there,
        // and move nothing; Up moves the rows a step down, and no further than to the list's top;
        // and where the keys moved the rows, the list's controls take no press for their moment.
        if (scrolledBy(shortList) > 0.5) {
            QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
            shortList->forceActiveFocus();
            const qreal stood = scrolledBy(shortList);
            for (const Qt::Key key : {Qt::Key_Down, Qt::Key_PageDown}) {
                const QString pressed = key == Qt::Key_Down ? u"Down"_s : u"PageDown"_s;
                before = seenOf(desk, box);
                QTest::keyClick(desk.window, key);
                wrong += movedBy(before, seenOf(desk, box),
                                 u"%1 while room is left after a short list's end"_s.arg(pressed) + at, everyRow);
                QVERIFY2(qAbs(scrolledBy(shortList) - stood) < 0.5,
                         qPrintable(u"%1: the list stood %2 from its beginning, and stands %3 from it"_s.arg(pressed)
                                        .arg(stood)
                                        .arg(scrolledBy(shortList))));
                QVERIFY2(desk.ownFiles.isSettled(), qPrintable(pressed + u": nothing moved, and the list was held"_s));
            }
            before = seenOf(desk, box);
            QTest::keyClick(desk.window, Qt::Key_Up);
            const Seen up = seenOf(desk, box);
            QVERIFY2(qAbs(scrolledBy(shortList) - std::max(0.0, stood - 40)) < 0.5,
                     qPrintable(u"Up: the list stood %1 from its beginning, and stands %2 from it"_s.arg(stood).arg(
                         scrolledBy(shortList))));
            QVERIFY2(!desk.ownFiles.isSettled(),
                     "Up moved the rows under the pointer, and the list took presses at once");
            for (const QString& control : up.live) {
                QVERIFY2(!up.inTheList.contains(control), qPrintable(u"after Up, \"%1\" takes a click"_s.arg(control)));
            }
            const int tickedWere = desk.ownFiles.tickedCount();
            QTest::mouseClick(desk.window, Qt::LeftButton, {},
                              before.controls.value(u"ownImportTick of device 1"_s).center().toPoint());
            QTest::qWait(50);
            QCOMPARE(desk.ownFiles.tickedCount(), tickedWere);
            QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        }

        // Another file chosen while that list is shown: a long one, and sealed, so that the line
        // for its passphrase comes where the list stood. And its list comes; a row of it is
        // opened and closed at its top, in its middle and at its end.
        QList<Device> longOne;
        for (int number = 1; number <= 30; ++number) {
            longOne.append(ticked(number, u"Long %1"_s.arg(number)));
        }
        desk.opens = desk.write(u"long.qkvm"_s, fileOf(longOne));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportPassphrase"_s) != nullptr);
        QVERIFY(!desk.ownFiles.isLaidOut());
        wrong += movedBy(before, seenOf(desk, box), u"another file is chosen"_s + at);
        keep(u"another-file-chosen"_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportPassphrase"_s));
        type(desk.window, thePassphrase());
        before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        wrong += movedBy(before, seenOf(desk, box), u"a long list comes"_s + at);
        QQuickItem* list = listOf(desk);
        QVERIFY2(mostScrolled(list) > 200, qPrintable(QString::number(mostScrolled(list))));
        const std::function<void(const QString&)> keepLong = [&](const QString& step) { keep(u"long-row-"_s + step); };
        QString failed = openAndClose(desk, 1, u"the row at the top of a long list"_s + at, wrong, false, keepLong);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        scrollTo(list, mostScrolled(list) / 2);
        aFrame(desk.window);
        const int middle = theRowInTheMiddle(desk);
        QVERIFY2(middle > 1 && middle < 30, qPrintable(QString::number(middle)));
        failed = openAndClose(desk, middle, u"a row in the middle of a long list"_s + at, wrong, false);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(rowOf(desk.window, 30) != nullptr);
        aFrame(desk.window);
        failed = openAndClose(desk, 30, u"the row at the end of a long list"_s + at, wrong, true);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));

        // A taking that takes some: one of the devices was put on the wall by hand meanwhile.
        QVERIFY(!own.put(elsewhere(2), u"By hand"_s).isEmpty());
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        before = seenOf(desk, box);
        clickOn(desk.window, take);
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        wrong += movedBy(before, seenOf(desk, box), u"a taking that took some"_s + at);
        keep(u"took-some"_s);
        QCOMPARE(desk.ownFiles.tookSummary(), u"29 of 30 KVMs were taken onto this wall."_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(box));

        // Export: the file is made, and the name it is to be kept under is refused (a plain file
        // given a sealed one's name, where a plain one of its own name is there); each is said
        // in the box, and nothing moves for it.
        const QString exportBox = u"ownExportBox"_s;
        static_cast<void>(desk.write(u"wall.json"_s, "a plain file that was there"));
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, exportBox));
        before = seenOf(desk, exportBox);
        keep(u"export-asked"_s);
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownExportSave"_s) != nullptr, 20000);
        wrong += movedBy(before, seenOf(desk, exportBox), u"the file is made"_s + at);
        keep(u"export-made"_s);
        const QQuickItem* save = theOne(desk.window, u"ownExportSave"_s);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
        before = seenOf(desk, exportBox);
        clickOn(desk.window, save);
        QTRY_VERIFY(theOne(desk.window, u"ownExportProblem"_s) != nullptr);
        wrong += movedBy(before, seenOf(desk, exportBox), u"a name is refused"_s + at);
        keep(u"export-name-refused"_s);
        QCOMPARE(desk.suggestions.size(), 1);
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(exportBox));
        QVERIFY2(nothingMoved(wrong), qPrintable(u"%1 times; the first: %2"_s.arg(wrong.size()).arg(wrong.value(0))));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void noTickNoteTakingOrEndOfItsTimeComesToStandWhereAnotherStood_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("the smallest window") << QSize(640, 400);
        QTest::newRow("a window of 1280 by 800") << QSize(1280, 800);
    }

    void noTickNoteTakingOrEndOfItsTimeComesToStandWhereAnotherStood()
    {
        // The rest of what the box knows: its notes opened; a tick, the first one and the last
        // one, and one after which what stands over the list is a line shorter; a taking that
        // takes none, and one that takes all; and the end of the time for which what was read is
        // kept.
        QFETCH(QSize, size);
        WallOfOneTest own;
        Desk desk(own);
        desk.ownFiles.restFor(Desk::heldRest, Desk::heldRest);
        // (A wall of cards under the box, as a wall that is used has them: what a click finds there
        // once the box has gone is looked at below.)
        desk.wall.machines()->setTiles(ui::pretendWall());
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        const QString at = u", at %1 by %2"_s.arg(size.width()).arg(size.height());
        const QString box = u"ownImportBox"_s;
        QStringList wrong;
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));

        // A list with a note beside its two devices; and the note opened.
        desk.opens = desk.write(u"noted.json"_s, fileWithSoManyLeftOut(1, {ticked(1, u"One"_s), ticked(2, u"Two"_s)}));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        Seen before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(rowOf(desk.window, 2) != nullptr);
        wrong += movedBy(before, seenOf(desk, box), u"a list with a note comes"_s + at);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportMoreNotes"_s));
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        wrong += movedBy(before, seenOf(desk, box), u"the notes are opened"_s + at);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);

        // Another file: three devices whose passwords go in the clear, none of them ticked to
        // begin with. (What stands over the list is two lines in the smallest window while one
        // of them wants a look, and one line once none does.)
        desk.opens = desk.write(u"clear.json"_s, fileOf({kvmAt(u"http://192.0.2.3:8080"_s, u"One"_s),
                                                         kvmAt(u"http://192.0.2.4:8080"_s, u"Two"_s),
                                                         kvmAt(u"http://192.0.2.5:8080"_s, u"Three"_s)},
                                                        false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        // An error over the list that is shown: Windows' box for choosing a file does not open.
        // What it says stands over the box's buttons, and the list is shown as it was: its end
        // comes up to it, and nothing in it moves.
        desk.doorFails = true;
        before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportProblem"_s) != nullptr);
        QVERIFY(desk.ownFiles.isLaidOut());
        wrong += movedBy(before, seenOf(desk, box), u"an error comes over the list"_s + at, everyRow);
        desk.doorFails = false;
        // (Nothing is ticked: the button that takes stands in its place all the same, says that
        // it would take none, and takes no click.)
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QCOMPARE(take->property("text").toString(), u"Take 0 KVMs"_s);
        QVERIFY(!take->isEnabled());
        const QQuickItem* counts = theOne(desk.window, u"ownImportCounts"_s);
        QVERIFY(counts != nullptr);
        // A tick, by its click: and what moved for it.
        const auto tick = [&](int place, int ticks, const QString& when) -> QString {
            if (!bringIntoSight(desk, place)) {
                return u"%1: the tick of device %2 is never in sight"_s.arg(when).arg(place);
            }
            const Seen was = seenOf(desk, box);
            clickOn(desk.window, inRow(rowOf(desk.window, place), u"ownImportTick"_s));
            if (!QTest::qWaitFor([&] { return desk.ownFiles.tickedCount() == ticks; }, 20000)) {
                return u"%1: %2 are ticked"_s.arg(when).arg(desk.ownFiles.tickedCount());
            }
            wrong += movedBy(was, seenOf(desk, box), when + at, everyRow);
            return {};
        };
        QElapsedTimer sinceTheFirstTick;
        sinceTheFirstTick.start();
        QString failed = tick(1, 1, u"the first tick"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        // (From nothing ticked to something: it takes no click for its moment. A click on it in
        // that moment takes nothing.)
        QCOMPARE(take->property("text").toString(), u"Take 1 KVM"_s);
        QVERIFY(!take->isEnabled());
        clickOn(desk.window, take);
        QTest::qWait(50);
        QVERIFY2(sinceTheFirstTick.elapsed() < Desk::heldRest,
                 qPrintable(QString::number(sinceTheFirstTick.elapsed())));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.ownFiles.isLaidOut());
        QCOMPARE(desk.ownFiles.tickedCount(), 1);
        failed = tick(2, 2, u"a second tick"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        const int linesWere = counts->property("lineCount").toInt();
        failed = tick(3, 3, u"a tick that leaves none to look at"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        // (Where the box is narrowest, that is a line less of it: what this is about. Wider, it
        // may be one line before and after.)
        QVERIFY2(size.height() >= 500 || counts->property("lineCount").toInt() < linesWere,
                 qPrintable(u"what stands over the list has %1 lines, and had %2: %3"_s
                                .arg(counts->property("lineCount").toInt())
                                .arg(linesWere)
                                .arg(counts->property("text").toString())));
        failed = tick(3, 2, u"a tick taken away"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        failed = tick(2, 1, u"another tick taken away"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        failed = tick(1, 0, u"the last tick taken away"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        QCOMPARE(take->property("text").toString(), u"Take 0 KVMs"_s);
        QVERIFY(isSeen(take));
        QVERIFY(!take->isEnabled());

        // A taking that takes none: both that are ticked were put on the wall by hand meanwhile.
        failed = tick(1, 1, u"a first tick again"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        failed = tick(2, 2, u"a second tick again"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        QVERIFY(!own.put(u"http://192.0.2.3:8080"_s, u"By hand"_s).isEmpty());
        QVERIFY(!own.put(u"http://192.0.2.4:8080"_s, u"By hand too"_s).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        before = seenOf(desk, box);
        clickOn(desk.window, take);
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        wrong += movedBy(before, seenOf(desk, box), u"a taking that took none"_s + at);
        QCOMPARE(desk.ownFiles.tookSummary(), u"None of the 2 KVMs was taken onto this wall."_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(box));

        // A taking that takes all: the box closes, and nothing of it is left to be pressed.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(11, u"Eleven"_s), ticked(12, u"Twelve"_s)}, false)));
        take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        before = seenOf(desk, box);
        const QPoint takeStood = middleOf(take);
        const QSignalSpy menus(&desk.actions, &ui::ActionsView::openAsked);
        const QSignalSpy consoles(&desk.actions, &ui::ActionsView::consoleAsked);
        const QSignalSpy cardActions(&desk.actions, &ui::ActionsView::cardActionAsked);
        const QSignalSpy ports(&desk.actions, &ui::ActionsView::portAsked);
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(box));
        const Seen gone = seenOf(desk, box);
        wrong += movedBy(before, gone, u"a taking that took all"_s + at);
        QVERIFY(gone.controls.isEmpty());
        QVERIFY2(toldBy(desk.actions).contains(u"ok: 2 KVMs were taken onto this wall."_s),
                 qPrintable(toldBy(desk.actions).join(u'\n')));
        // (It stood over the settings, which are there again.)
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        // The box is gone, and the click that follows lands on what is under it: what is there, for
        // a pointer that rests where "Take" stood, takes no press for a moment, by one click or two.
        const QStringList there = pressableAt(desk.window, takeStood);
        qInfo("where \"Take\" stood%s, once the box is gone: %s", qPrintable(at),
              there.isEmpty() ? "nothing that takes a press" : qPrintable(there.join(u", "_s)));
        const QQuickItem* focused = desk.window->activeFocusItem();
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, takeStood);
        QTest::mouseDClick(desk.window, Qt::LeftButton, {}, takeStood);
        QTest::qWait(50);
        QVERIFY2(desk.window->activeFocusItem() == focused && desk.wall.enlarged().isEmpty() && menus.isEmpty() &&
                     consoles.isEmpty() && cardActions.isEmpty() && ports.isEmpty(),
                 qPrintable(u"a click where \"Take\" stood, just after the box went, was taken there (%1)"_s.arg(
                     there.join(u", "_s))));
        // After that moment, a click there is taken as any: with the settings closed, by the wall.
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        aFrame(desk.window);
        const QQuickItem* focusedThen = desk.window->activeFocusItem();
        if (!there.isEmpty()) {
            QTRY_VERIFY_WITH_TIMEOUT(
                [&] {
                    QTest::mouseClick(desk.window, Qt::LeftButton, {}, takeStood);
                    return desk.window->activeFocusItem() != focusedThen || !menus.isEmpty() || !consoles.isEmpty() ||
                           !cardActions.isEmpty() || !ports.isEmpty();
                }(),
                20000);
        }

        // The end of the time for which what was read is kept (six seconds here, in a window of
        // its own), with the list scrolled as by somebody who reads down it.
        WallOfOneTest ownToo;
        Desk brief(ownToo, 6000);
        QVERIFY(brief.load(size.width(), size.height()) != nullptr);
        QVERIFY(openFromTheSettings(brief, u"settingsImport"_s, box));
        QList<Device> thirty;
        for (int number = 1; number <= 30; ++number) {
            thirty.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        QVERIFY(chooseAndRead(brief, fileOf(thirty), thePassphrase()));
        scrollTo(listOf(brief), 100);
        before = seenOf(brief, box);
        // (The look before is of the list, with its rows and their ticks in sight: what is compared
        // is what was pointed at in the list, and not a box that had lost its list already.)
        QVERIFY2(
            brief.ownFiles.isLaidOut() && !before.firstLines.isEmpty() &&
                before.ofDevice.size() >= before.firstLines.size(),
            qPrintable(u"%1 rows, %2 of their controls"_s.arg(before.firstLines.size()).arg(before.ofDevice.size())));
        QTRY_VERIFY_WITH_TIMEOUT(!brief.ownFiles.isLaidOut(), 30000);
        QTRY_VERIFY(theOne(brief.window, u"ownImportNote"_s) != nullptr);
        wrong += movedBy(before, seenOf(brief, box), u"the list's time is over"_s + at);
        QVERIFY2(nothingMoved(wrong), qPrintable(u"%1 times; the first: %2"_s.arg(wrong.size()).arg(wrong.value(0))));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
        QVERIFY2(brief.complaints.isEmpty(), qPrintable(brief.complaints.join(u'\n')));
    }

    void whatCameOfATakingIsNotLostOnAnyWayTheBoxIsClosed_data()
    {
        QTest::addColumn<QString>("way");
        QTest::newRow("by its own button") << u"button"_s;
        QTest::newRow("by the way out in its title") << u"title"_s;
        QTest::newRow("by the program, from outside") << u"outside"_s;
        QTest::newRow("as its window leaves this PC's wall") << u"wall"_s;
    }

    void whatCameOfATakingIsNotLostOnAnyWayTheBoxIsClosed()
    {
        QFETCH(QString, way);
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        const QString box = u"ownImportBox"_s;
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)}), thePassphrase()));
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        const QPoint takeStood = middleOf(take);
        // Somebody put the second on the wall by hand meanwhile: one of the two is taken.
        QVERIFY(!own.put(elsewhere(2), u"By hand"_s).isEmpty());
        clickOn(desk.window, take);
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        aFrame(desk.window);
        // While what came of it is shown, nothing beside the box closes it, nor Esc: a second
        // click where "Take" stood lands on the box, and one beside it on nothing.
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, takeStood);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(desk.window->width() / 2, 6));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTest::qWait(100);
        QVERIFY2(desk.isOpen(box), "what came of the taking was closed by a click beside it, unread");
        QVERIFY(desk.ownFiles.hasTaken());
        QVERIFY(toldBy(desk.actions).isEmpty());
        // Closed, by the way of this row: the window's corner says what was taken, as something
        // to be looked at.
        if (way == u"button"_s) {
            clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        } else if (way == u"title"_s) {
            // (Its own: the settings it stands over have one too.)
            QList<const QQuickItem*> ways;
            named(desk.box(box)->property("header").value<QQuickItem*>(), u"closeBox"_s, ways);
            QCOMPARE(ways.size(), 1);
            clickOn(desk.window, ways.constFirst());
        } else if (way == u"outside"_s) {
            desk.ownFiles.close();
        } else {
            desk.session.show(atAServersWall());
        }
        QTRY_VERIFY(!desk.isOpen(box));
        QCOMPARE(toldBy(desk.actions), QStringList{u"error: 1 of 2 KVMs were taken onto this wall."_s});
        QVERIFY(!desk.ownFiles.hasTaken());
        QCOMPARE(own.names(), (QStringList{u"By hand"_s, u"One"_s}));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aWindowThatChangesSizeHoldsWhatTakesAndWhatSavesForTheirMoment_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QSize>("then");
        QTest::newRow("from 1280 by 800 to 900 by 600") << QSize(1280, 800) << QSize(900, 600);
        QTest::newRow("from the smallest window to 800 by 500") << QSize(640, 400) << QSize(800, 500);
    }

    void aWindowThatChangesSizeHoldsWhatTakesAndWhatSavesForTheirMoment()
    {
        // The box's size and place follow its window's: the window is snapped, made as large as
        // its screen, or comes to another screen, and the box stands somewhere else, larger or
        // smaller, with something else under a pointer that rests where it rested. Nothing of the
        // list, and nothing that takes or saves, takes a press in the moment after: by a click,
        // and by whatever else speaks for a person (the program's own calls).
        QFETCH(QSize, size);
        QFETCH(QSize, then);
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        desk.ownFiles.restFor(Desk::heldRest, Desk::heldRest);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        const QString of =
            u"%1x%2-to-%3x%4"_s.arg(size.width()).arg(size.height()).arg(then.width()).arg(then.height());
        const auto keep = [&](const QString& name) { keepPictureOf(desk.window, u"own-resize-%1-%2"_s.arg(of, name)); };
        const QString box = u"ownImportBox"_s;
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));
        // A list that has stood for its moment, with something ticked: its ticks, its rows'
        // "Details" and "Take" take a press.
        QVERIFY(chooseAndRead(
            desk,
            fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s), kvmAt(u"http://192.0.2.3:8080"_s, u"Three"_s)}, false)));
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        const QString takes = u"the box's own button (import)"_s;
        const QStringList held{u"ownImportTick of device 1"_s, u"ownImportTick of device 3"_s,
                               u"ownImportDetails of device 2"_s, takes};
        Seen before = seenOf(desk, box);
        for (const QString& control : held) {
            QVERIFY2(before.live.contains(control), qPrintable(control));
        }
        keep(u"import-before"_s);

        // The window changes size: the box with it. (Looked at once the window has drawn it and
        // what the program said of it meanwhile has come to the screens, a turn of the loop later:
        // the program says it so, and holds every press itself from the moment it was told.)
        QElapsedTimer since;
        since.start();
        desk.window->resize(then);
        QTRY_COMPARE(desk.window->size(), then);
        aFrame(desk.window);
        QCoreApplication::processEvents();
        Seen after = seenOf(desk, box);
        keep(u"import-after"_s);
        QVERIFY2(after.box != before.box, "the box did not change with its window");
        QStringList pressable;
        for (const QString& control : std::as_const(after.live)) {
            if (after.inTheList.contains(control) || control == takes) {
                pressable.append(control);
            }
        }
        QVERIFY2(pressable.isEmpty(),
                 qPrintable(u"the window changed size %1, and at once these took a press: %2"_s.arg(
                     of, pressable.join(u", "_s))));
        QVERIFY(!desk.ownFiles.isSettled());
        QVERIFY(!desk.ownFiles.mayTake());
        // (What stands where something else stood a moment before, and is not held: say so.)
        for (const QString& moved : movedBy(before, after, u"the window changed size "_s + of)) {
            qInfo("%s", qPrintable(moved));
        }
        // Pressed in that moment, each where it stands now, and by the program's own calls:
        // nothing of it is taken.
        for (const QString& control : held) {
            QTest::mouseClick(desk.window, Qt::LeftButton, {}, after.controls.value(control).center().toPoint());
        }
        desk.ownFiles.setTicked(1, false);
        desk.ownFiles.setOpen(2, true);
        desk.ownFiles.take();
        QTest::qWait(50);
        QVERIFY2(since.elapsed() < Desk::heldRest, qPrintable(QString::number(since.elapsed())));
        QCOMPARE(own.names(), QStringList{u"Build server"_s});
        QCOMPARE(desk.ownFiles.tickedCount(), 2);
        QVERIFY(!rowOf(desk.window, 2)->property("open").toBool());
        QVERIFY(desk.isOpen(box));
        QVERIFY(desk.ownFiles.isLaidOut());
        // After its moment, each takes a press again. (All of the moment, but for what a timer of
        // Windows' may be early by: a twentieth, and here a tenth is let pass.)
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        QVERIFY2(since.elapsed() >= Desk::heldRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        QVERIFY(desk.ownFiles.isSettled());
        // The window moves, as large as it is (to another place on its screen, or to another
        // screen): the box is in its middle as before, and stands elsewhere under the pointer all
        // the same. The same moment.
        const QPoint was = desk.window->position();
        desk.window->setPosition(was + QPoint(37, 23));
        QTRY_COMPARE(desk.window->position(), was + QPoint(37, 23));
        aFrame(desk.window);
        QCoreApplication::processEvents();
        after = seenOf(desk, box);
        QVERIFY2(!after.live.contains(takes), "the window moved, and \"Take\" took a press at once");
        QVERIFY2(!after.live.contains(u"ownImportTick of device 1"_s),
                 "the window moved, and a tick took a press at once");
        QVERIFY(!desk.ownFiles.isSettled());
        QVERIFY(!desk.ownFiles.mayTake());
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(box));
        QCOMPARE(own.names(), (QStringList{u"Build server"_s, u"One"_s, u"Two"_s}));

        // The box that writes the wall: a file that was made, with "Save" taking a press; and the
        // window goes back to its size.
        const QString exportBox = u"ownExportBox"_s;
        desk.savesTo = desk.folder.filePath(u"wall.json"_s);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, exportBox));
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownExportSave"_s) != nullptr, 20000);
        const QQuickItem* save = theOne(desk.window, u"ownExportSave"_s);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
        before = seenOf(desk, exportBox);
        QVERIFY(before.live.contains(u"the box's own button (export)"_s));
        keep(u"export-before"_s);
        since.restart();
        desk.window->resize(size);
        QTRY_COMPARE(desk.window->size(), size);
        aFrame(desk.window);
        QCoreApplication::processEvents();
        after = seenOf(desk, exportBox);
        keep(u"export-after"_s);
        QVERIFY2(after.box != before.box, "the box did not change with its window");
        QVERIFY2(!after.live.contains(u"the box's own button (export)"_s),
                 qPrintable(u"the window changed size %1, and \"Save\" took a press at once"_s.arg(of)));
        QVERIFY(!desk.ownFiles.maySave());
        clickOn(desk.window, save);
        desk.ownFiles.saveFile();
        QTest::qWait(50);
        QVERIFY2(since.elapsed() < Desk::heldRest, qPrintable(QString::number(since.elapsed())));
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY(!QFile::exists(desk.savesTo));
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
        QVERIFY2(since.elapsed() >= Desk::heldRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        clickOn(desk.window, save);
        QTRY_VERIFY(!desk.isOpen(exportBox));
        QCOMPARE(desk.suggestions.size(), 1);
        QVERIFY(QFile::exists(desk.savesTo));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatStandsOverTheListHasRoomForEveryCountItCanSayAtEveryWidthOfTheBox()
    {
        // What stands over the list keeps the room that the highest of what it can say takes as
        // it is drawn, at the width the box has: so no count needs more lines than it has room
        // for, and the sentence under it never runs under the list, whatever is ticked. Looked at
        // for every count each list can come to, at every width the box can have (from the
        // smallest window to one in which the box is at its widest), with lists where two forms
        // of a count that are as long by their letters are not as wide as they are drawn ("None
        // is ticked." and "100 are ticked."; "1 is not ticked and wants a look." and "3 are not
        // ticked and want a look.").
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(1280, 600) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // A line of text as the counts are drawn, to measure every count in, one after the other.
        QQmlComponent probeOf(&desk.engine);
        probeOf.setData("import QtQuick\nText { textFormat: Text.PlainText; wrapMode: Text.WordWrap; visible: false }",
                        QUrl());
        const std::unique_ptr<QQuickItem> probe(qobject_cast<QQuickItem*>(probeOf.create()));
        QVERIFY2(probe != nullptr, qPrintable(probeOf.errorString()));
        probe->setParentItem(desk.window->contentItem());
        struct OfAList {
            int ticked = 0;
            int toLookAt = 0;
            int cannot = 0;
            bool sealed = true;
        };
        QStringList wrong;
        for (const OfAList& kind : {OfAList{100, 3, 0, true}, OfAList{100, 3, 0, false}, OfAList{100, 3, 1, true},
                                    OfAList{100, 3, 1, false}}) {
            const QString called = u"%1 ticked, %2 to look at, %3 that cannot be taken, %4"_s.arg(kind.ticked)
                                       .arg(kind.toLookAt)
                                       .arg(kind.cannot)
                                       .arg(kind.sealed ? u"sealed"_s : u"plain"_s);
            QList<Device> devices;
            for (int number = 1; number <= kind.ticked; ++number) {
                devices.append(ticked(number, u"KVM %1"_s.arg(number)));
            }
            for (int number = 1; number <= kind.toLookAt; ++number) {
                devices.append(kvmAt(u"http://192.0.2.%1:8080"_s.arg(200 + number), u"Plain %1"_s.arg(number)));
            }
            for (int number = 1; number <= kind.cannot; ++number) {
                devices.append(notKeptHere(ticked(150 + number, u"Small %1"_s.arg(number))));
            }
            desk.window->resize(1280, 600);
            QVERIFY(chooseAndRead(desk, fileOf(devices, kind.sealed), kind.sealed ? thePassphrase() : QString()));
            // Every count this list can come to: of the devices ticked to begin with, any number
            // ticked, and of those that want a look, any number.
            ui::LaidOutList* listed = desk.ownFiles.offered();
            QCOMPARE(listed->deviceCount(), static_cast<int>(devices.size()));
            QStringList said;
            for (int looked = 0; looked <= kind.toLookAt; ++looked) {
                for (int number = 1; number <= kind.toLookAt; ++number) {
                    static_cast<void>(listed->setTicked(kind.ticked + number, number <= looked));
                }
                for (int number = 1; number <= kind.ticked; ++number) {
                    static_cast<void>(listed->setTicked(number, false));
                }
                said.append(listed->saidOfItsCounts());
                for (int number = 1; number <= kind.ticked; ++number) {
                    static_cast<void>(listed->setTicked(number, true));
                    said.append(listed->saidOfItsCounts());
                }
            }
            for (int number = 1; number <= kind.toLookAt; ++number) {
                static_cast<void>(listed->setTicked(kind.ticked + number, false));
            }
            QCOMPARE(said.size(), (kind.ticked + 1) * (kind.toLookAt + 1));
            for (int width = 640; width <= 724; ++width) {
                desk.window->resize(width, 600);
                QTRY_COMPARE(desk.window->width(), width);
                aFrame(desk.window);
                const QQuickItem* counts = theOne(desk.window, u"ownImportCounts"_s);
                const QQuickItem* nothingYet = theOne(desk.window, u"ownImportNothingYet"_s);
                const QQuickItem* above = theOne(desk.window, u"ownImportAbove"_s);
                QVERIFY(counts != nullptr && nothingYet != nullptr && above != nullptr);
                const QQuickItem* band = above->parentItem();
                QCOMPARE(desk.box(u"ownImportBox"_s)->property("width").toReal(), std::min(700.0, width - 24.0));
                probe->setProperty("font", counts->property("font"));
                probe->setWidth(counts->width());
                qreal highest = 0;
                QString highestSaid;
                for (const QString& one : std::as_const(said)) {
                    probe->setProperty("text", one);
                    const qreal high = probe->implicitHeight();
                    if (high > highest) {
                        highest = high;
                        highestSaid = one;
                    }
                }
                if (4 + highest + nothingYet->height() > band->height() + 0.5) {
                    wrong.append(u"%1, a box %2 wide: \"%3\" is %4 high, and over the list there is room for %5 with "
                                 "the sentence under it"_s.arg(called)
                                     .arg(std::min(700, width - 24))
                                     .arg(highestSaid)
                                     .arg(highest)
                                     .arg(band->height() - 4 - nothingYet->height()));
                }
                if (width == 640) {
                    keepPictureOf(desk.window, u"own-import-counts-narrowest-%1-%2-%3-%4"_s.arg(kind.ticked)
                                                   .arg(kind.toLookAt)
                                                   .arg(kind.cannot)
                                                   .arg(kind.sealed ? u"sealed"_s : u"plain"_s));
                }
            }
        }
        for (const QString& one : std::as_const(wrong)) {
            qInfo("%s", qPrintable(one));
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(u"%1 times; the first: %2"_s.arg(wrong.size()).arg(wrong.value(0))));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatComesWithTheListTakesNoClickInItsFirstMoment()
    {
        // A list comes where something else stood a moment before (the line for the passphrase,
        // the button that reads), and the notes, opened, move every row. So nothing of the list
        // takes a click in the moment it has come, nor in the moment after its notes were
        // opened: as "Take" does not.
        WallOfOneTest own;
        Desk desk(own);
        desk.ownFiles.restFor(Desk::longRest, Desk::longRest);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        desk.opens =
            desk.write(u"plain.json"_s,
                       fileWithSoManyLeftOut(1, {kvmAt(u"http://192.0.2.3:8080"_s, u"One"_s), ticked(4, u"Two"_s)}));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        // (From the press that reads the file: the list's moment begins after it, when the list
        // has come, and what is looked at here is to have been within that moment.)
        QElapsedTimer since;
        since.start();
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(rowOf(desk.window, 3) != nullptr);
        aFrame(desk.window);
        const QQuickItem* tick = inRow(rowOf(desk.window, 2), u"ownImportTick"_s);
        const QQuickItem* details = inRow(rowOf(desk.window, 2), u"ownImportDetails"_s);
        const QQuickItem* more = theOne(desk.window, u"ownImportMoreNotes"_s);
        QVERIFY(tick != nullptr && details != nullptr && more != nullptr);
        QVERIFY2(!tick->isEnabled(), "a tick takes a click in the moment the list has come");
        QVERIFY(!details->isEnabled());
        QVERIFY(!more->isEnabled());
        clickOn(desk.window, tick);
        clickOn(desk.window, details);
        clickOn(desk.window, more);
        QTest::qWait(50);
        QVERIFY2(since.elapsed() < Desk::longRest, qPrintable(QString::number(since.elapsed())));
        QCOMPARE(desk.ownFiles.tickedCount(), 1);
        QVERIFY(!rowOf(desk.window, 2)->property("open").toBool());
        QCOMPARE(desk.ownFiles.foldedNotes(), 1);
        // After its moment, they do.
        QTRY_VERIFY_WITH_TIMEOUT(tick->isEnabled(), 20000);
        QVERIFY(details->isEnabled() && more->isEnabled());
        // The notes opened: every row moves, and its moment begins anew.
        clickOn(desk.window, more);
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        aFrame(desk.window);
        tick = inRow(rowOf(desk.window, 2), u"ownImportTick"_s);
        QVERIFY(tick != nullptr && !tick->isEnabled());
        clickOn(desk.window, tick);
        QTest::qWait(50);
        QCOMPARE(desk.ownFiles.tickedCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(tick->isEnabled(), 20000);
        // And a tick does not hold the others back: the first tick where none was begins the
        // moment of "Take" anew, and nothing else.
        clickOn(desk.window, tick);
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 2);
        QVERIFY(inRow(rowOf(desk.window, 3), u"ownImportTick"_s)->isEnabled());
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aListOfTwoThousandDevicesIsShownSoonAndOnlyWhereItIsSeenAndATickSurvivesItsRowGoing()
    {
        const int many = 2000;
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        desk.opens = desk.write(u"many.json"_s, fileOfSoMany(many));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QVERIFY(!desk.ownFiles.fileName().isEmpty());
        const QQuickItem* read = theOne(desk.window, u"ownImportRead"_s);
        aFrame(desk.window);

        // From the press on "Read the file" until the list is on the screen, drawn.
        QElapsedTimer since;
        since.start();
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, middleOf(read));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 60000);
        const qint64 laidOut = since.elapsed();
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        const qint64 shown = since.elapsed();
        qInfo("a file of %d devices: laid out %lld ms after the press, and on the screen after %lld ms", many,
              static_cast<long long>(laidOut), static_cast<long long>(shown));
        QVERIFY2(shown < 20000, qPrintable(QString::number(shown)));
        // Every device is in the list, and ticked: but a row was made only where the list is seen.
        QObject* list = desk.window->findChild<QObject*>(u"ownImportOffered"_s);
        QVERIFY(list != nullptr);
        QCOMPARE(list->property("count").toInt(), many);
        QCOMPARE(desk.ownFiles.tickedCount(), many);
        QCOMPARE(theOne(desk.window, u"ownImportTake"_s)->property("text").toString(), u"Take 2000 KVMs"_s);
        QCOMPARE(
            theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
            u"2000 devices of this file are in this list. 2000 are ticked. The file has no password for any of them."_s);
        const int made = static_cast<int>(deviceRows(desk.window).size());
        qInfo("rows made for it: %d", made);
        QVERIFY2(made >= 1 && made < 60, qPrintable(QString::number(made)));
        QVERIFY(theOne(desk.window, u"ownImportScrollMark"_s) != nullptr);
        keepPictureOf(desk.window, u"own-import-two-thousand-top"_s);

        // The tick of the second device is taken away (once the list has stood for its moment),
        // and the list scrolled to its end: the row of the second is gone, and that of the last
        // is there.
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, inRow(rowOf(desk.window, 2), u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), many - 1);
        QVERIFY(!inRow(rowOf(desk.window, 2), u"ownImportTick"_s)->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(rowOf(desk.window, many) != nullptr);
        aFrame(desk.window);
        QTRY_VERIFY(rowOf(desk.window, 2) == nullptr);
        QVERIFY(static_cast<int>(deviceRows(desk.window).size()) < 60);
        // (And the last one's is taken away there.)
        clickOn(desk.window, inRow(rowOf(desk.window, many), u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), many - 2);
        keepPictureOf(desk.window, u"own-import-two-thousand-end"_s);
        // Scrolled back: the second comes back as it was left, not ticked, and its neighbours
        // ticked as they were.
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        QTRY_VERIFY(rowOf(desk.window, 2) != nullptr);
        aFrame(desk.window);
        QVERIFY(!inRow(rowOf(desk.window, 2), u"ownImportTick"_s)->property("checked").toBool());
        QVERIFY(inRow(rowOf(desk.window, 1), u"ownImportTick"_s)->property("checked").toBool());
        QVERIFY(inRow(rowOf(desk.window, 3), u"ownImportTick"_s)->property("checked").toBool());
        QTRY_VERIFY(rowOf(desk.window, many) == nullptr);
        QCOMPARE(desk.ownFiles.tickedCount(), many - 2);
        QCOMPARE(theOne(desk.window, u"ownImportTake"_s)->property("text").toString(), u"Take 1998 KVMs"_s);
        QCOMPARE(
            theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
            u"2000 devices of this file are in this list. 1998 are ticked. The file has no password for any of them."_s);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theLinesBesideTheDevicesAreFoldedAndNothingIsTakenBeforeTheyWereOpened()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // A file made by anybody: twenty devices that the reader leaves out, each with a
        // sentence, and six that it takes.
        QList<Device> devices;
        for (int number = 1; number <= 6; ++number) {
            devices.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        QVERIFY(chooseAndRead(desk, fileWithSoManyLeftOut(20, devices)));
        QTRY_VERIFY(rowOf(desk.window, 21) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        // The first screen: one row that says how many lines there are beside the devices, and
        // the devices, every one of them in sight with its tick. Not twenty lines, and the
        // devices somewhere under them.
        QCOMPARE(seenOnes(desk.window, u"ownImportBeside"_s).size(), 0);
        QVERIFY(!says(desk.window, u"Device 1: it has no address"_s));
        const QQuickItem* more = theOne(desk.window, u"ownImportMoreNotes"_s);
        QVERIFY(more != nullptr);
        QCOMPARE(more->property("text").toString(), u"20 notes about this file"_s);
        QVERIFY(isWholeIn(list, more));
        QVERIFY(says(desk.window, u"To be opened before anything is taken."_s));
        for (int place = 21; place <= 26; ++place) {
            const QQuickItem* row = rowOf(desk.window, place);
            QVERIFY2(row != nullptr && isWholeIn(list, row), qPrintable(QString::number(place)));
            QVERIFY(isWholeIn(list, inRow(row, u"ownImportTick"_s)));
        }
        QVERIFY(scrolledBy(list) < 0.5);
        // (A plain file has no passwords: that is said once, above the list, and on no row.)
        QCOMPARE(saidIn(rowOf(desk.window, 21), u"ownImportInShort"_s), u"Known by Windows' word"_s);
        // What is ticked stands above the list in words, beside the number on the button.
        QCOMPARE(theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
                 u"6 devices of this file are in this list. 6 are ticked. The file has no password for any of them."_s);
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QCOMPARE(take->property("text").toString(), u"Take 6 KVMs"_s);
        keepPictureOf(desk.window, u"own-import-twenty-notes-folded"_s);
        // "Take" takes no click while lines are folded away that nobody opened, however long
        // the list is looked at (several of its moments, which are short here).
        QTest::qWait(Desk::pastAQuickRest);
        QVERIFY(!take->isEnabled());
        QVERIFY(!desk.ownFiles.mayTake());
        clickOn(desk.window, take);
        QTest::qWait(50);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.isOpen(u"ownImportBox"_s));

        // Opened by a click: every line is in the list, before the first device, and the list
        // stands where it stood. Then "Take" takes a click, after its moment.
        clickOn(desk.window, more);
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        aFrame(desk.window);
        QVERIFY(theOne(desk.window, u"ownImportMoreNotes"_s) == nullptr);
        QVERIFY(seenOnes(desk.window, u"ownImportBeside"_s).size() > 3);
        QVERIFY(says(desk.window, u"Device 1: it has no address"_s));
        QVERIFY(says(desk.window, u"Device 4: it has no address"_s));
        QCOMPARE(list->property("count").toInt(), 26);
        QVERIFY(scrolledBy(list) < 0.5);
        QVERIFY(theOne(desk.window, u"ownImportScrollMark"_s) != nullptr);
        keepPictureOf(desk.window, u"own-import-twenty-notes-opened"_s);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        // (The last of the lines and the devices, at the list's end.)
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(rowOf(desk.window, 26) != nullptr);
        aFrame(desk.window);
        QVERIFY(isWholeIn(list, rowOf(desk.window, 26)));
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(own.wall.kvms().size(), 6);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void atTheSmallestSizeTheFirstDeviceIsInSightWhateverNotesThereAre_data()
    {
        QTest::addColumn<int>("notes");
        QTest::newRow("one note") << 1;
        QTest::newRow("three notes") << 3;
        QTest::newRow("twenty notes") << 20;
    }

    void atTheSmallestSizeTheFirstDeviceIsInSightWhateverNotesThereAre()
    {
        QFETCH(int, notes);
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(640, 400) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // Notes as long as a file can make them: devices that the reader leaves out, as they are
        // at the address of one that stands before them, each called by the longest name of the
        // widest letters there is; and four devices that it takes.
        QJsonArray devices = QJsonDocument::fromJson(fileOf({ticked(1, u"First"_s), ticked(2, u"Second"_s),
                                                             ticked(3, u"Third"_s), ticked(4, u"Fourth"_s)},
                                                            false))
                                 .object()[u"devices"_s]
                                 .toArray();
        QCOMPARE(devices.size(), 4);
        for (int note = 0; note < notes; ++note) {
            QJsonObject again = devices.at(0).toObject();
            again[u"name"_s] = QString(80, u'W');
            devices.insert(1, again);
        }
        QVERIFY(chooseAndRead(desk, core::testing::plainFile({{u"devices"_s, devices}})));
        QCOMPARE(desk.ownFiles.offered()->besideCount(), notes);
        QCOMPARE(desk.ownFiles.offered()->deviceCount(), 4);
        aFrame(desk.window);
        const QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        QVERIFY(isInside(desk.window, list));
        QVERIFY(scrolledBy(list) < 0.5);
        // In the first screen: the first device, whole, with its tick, and whatever stands for
        // the notes before it.
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        const QQuickItem* first = rowOf(desk.window, 1);
        keepPictureOf(desk.window, u"own-import-smallest-%1-notes"_s.arg(notes));
        QVERIFY2(isWholeIn(list, first), qPrintable(u"%1 of the first device's %2 px are in sight in a list of %3 px"_s
                                                        .arg(heightInSight(list, first))
                                                        .arg(first->height())
                                                        .arg(list->height())));
        QVERIFY(isWholeIn(list, inRow(first, u"ownImportTick"_s)));
        // And what stands for the notes says how many there are, and that they are to be opened
        // before anything is taken: nothing is, until they are.
        const QQuickItem* more = theOne(desk.window, u"ownImportMoreNotes"_s);
        QVERIFY(more != nullptr);
        QVERIFY(isWholeIn(list, more));
        QCOMPARE(more->property("text").toString(),
                 notes == 1 ? u"1 note about this file"_s : u"%1 notes about this file"_s.arg(notes));
        QCOMPARE(desk.ownFiles.foldedNotes(), notes);
        QTest::qWait(Desk::pastAQuickRest);
        QVERIFY(!theOne(desk.window, u"ownImportTake"_s)->isEnabled());
        clickOn(desk.window, more);
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        QTRY_COMPARE(seenOnes(desk.window, u"ownImportBeside"_s).size() > 0, true);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aNewListBeginsAtItsTopWhereverTheOneBeforeWasScrolledTo()
    {
        // (The smallest window: the list has room for three rows, and the tenth is far down.)
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(640, 400) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        const auto fileOfForty = [](const QString& called) {
            QList<Device> devices;
            for (int number = 1; number <= 40; ++number) {
                devices.append(ticked(number, u"%1 %2"_s.arg(called).arg(number)));
            }
            return fileWithSoManyLeftOut(1, devices);
        };
        QVERIFY(chooseAndRead(desk, fileOfForty(u"First"_s)));
        QTRY_VERIFY(rowOf(desk.window, 2) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        // The keyboard is at the list, and its keys scroll it: Down, ten times.
        QTRY_VERIFY(list->hasActiveFocus());
        QVERIFY(scrolledBy(list) < 0.5);
        for (int press = 0; press < 10; ++press) {
            QTest::keyClick(desk.window, Qt::Key_Down);
        }
        // (Looked at once the list stands still: wherever its keys have taken it by then.)
        QTRY_VERIFY2_WITH_TIMEOUT(scrolledBy(list) > 150, qPrintable(QString::number(scrolledBy(list))), 20000);
        aFrame(desk.window);
        QVERIFY(!says(desk.window, u"1 note about this file"_s));
        QVERIFY(rowOf(desk.window, 2) == nullptr);

        // Another file is laid out: its list begins at its top, with the row that stands for
        // the line beside its devices and its first devices in sight. Nothing is kept from the
        // list before: not where that one was scrolled to, and no row of the keyboard's.
        const auto standsAtItsTop = [&](const QString& called) -> QString {
            aFrame(desk.window);
            if (scrolledBy(list) > 0.5) {
                return u"the list is scrolled by %1"_s.arg(scrolledBy(list));
            }
            if (!says(desk.window, u"1 note about this file"_s)) {
                return u"the row for the line beside the devices is not in sight"_s;
            }
            const QQuickItem* first = rowOf(desk.window, 2);
            if (first == nullptr || heightInSight(list, first) < 1) {
                return u"the first device is not in sight"_s;
            }
            if (saidIn(first, u"ownImportNameAndAddress"_s) != u"\"%1 1\" at https://192.0.2.1"_s.arg(called)) {
                return u"the first device is %1"_s.arg(saidIn(first, u"ownImportNameAndAddress"_s));
            }
            return list->property("currentIndex").toInt() > 0
                       ? u"the list's own row is %1"_s.arg(list->property("currentIndex").toInt())
                       : QString();
        };
        QVERIFY(chooseAndRead(desk, fileOfForty(u"Second"_s)));
        QString wrong = standsAtItsTop(u"Second"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        // So where the box was closed over a list that was scrolled, and is opened again.
        QTRY_VERIFY(list->hasActiveFocus());
        for (int press = 0; press < 10; ++press) {
            QTest::keyClick(desk.window, Qt::Key_Down);
        }
        QTRY_VERIFY2_WITH_TIMEOUT(scrolledBy(list) > 150, qPrintable(QString::number(scrolledBy(list))), 20000);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOfForty(u"Third"_s)));
        wrong = standsAtItsTop(u"Third"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        // (And what else the keys of a list do: back up, a page at a time, and to either end.)
        QTRY_VERIFY(list->hasActiveFocus());
        QTest::keyClick(desk.window, Qt::Key_PageDown);
        const qreal aPageDown = scrolledBy(list);
        QVERIFY2(aPageDown > 40, qPrintable(QString::number(aPageDown)));
        QTest::keyClick(desk.window, Qt::Key_Up);
        QVERIFY(scrolledBy(list) < aPageDown);
        QTest::keyClick(desk.window, Qt::Key_End);
        QTRY_VERIFY(rowOf(desk.window, 41) != nullptr);
        QVERIFY(qAbs(scrolledBy(list) - mostScrolled(list)) < 1.0);
        QTest::keyClick(desk.window, Qt::Key_PageUp);
        QVERIFY(scrolledBy(list) < mostScrolled(list) - 40);
        QTest::keyClick(desk.window, Qt::Key_Home);
        QTRY_VERIFY(scrolledBy(list) < 0.5);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatWasTypedAsAPassphraseIsInItsLineForNoLongerThanItsFileIsTheChosenOne()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        const QByteArray sealed = fileOf({ticked(1, u"One"_s)});
        const QByteArray plain = fileOf({ticked(2, u"Two"_s)}, false);
        // (Sealed, and what is sealed in it is another by one sign: it does not open with its
        // own key, and no other try will read it.)
        QJsonObject parts = QJsonDocument::fromJson(sealed).object();
        QJsonObject inside = parts[u"sealed"_s].toObject();
        QString data = inside[u"data"_s].toString();
        data[10] = data.at(10) == u'A' ? u'B' : u'A';
        inside[u"data"_s] = data;
        parts[u"sealed"_s] = inside;
        const QByteArray damaged = QJsonDocument(parts).toJson(QJsonDocument::Compact);
        parts = QJsonDocument::fromJson(sealed).object();
        parts[u"version"_s] = 2;
        const QByteArray newer = QJsonDocument(parts).toJson(QJsonDocument::Compact);

        const auto choose = [&desk](const QString& name, const QByteArray& file) {
            desk.opens = desk.write(name, file);
            clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        };
        // The line, once it is there: looked at from then on whether or not it is in sight.
        choose(u"first.qkvm"_s, sealed);
        const QQuickItem* line = theOne(desk.window, u"ownImportPassphrase"_s);
        QVERIFY(line != nullptr);
        const auto typed = [&] { return line->property("text").toString(); };
        const auto typeIntoIt = [&](const QString& text) {
            clickOn(desk.window, line);
            type(desk.window, text);
            return typed() == text;
        };
        // What this is about, at every step: a line that nobody sees holds nothing.
        const auto holdsNothingUnseen = [&] { return isSeen(line) || typed().isEmpty(); };

        // A file that is damaged: its passphrase is typed (the right one: its lock opens, and
        // what is in it does not), it is read, and refused. It is not the chosen one any more,
        // its line is gone, and what was typed is gone with it: a passphrase that opens files
        // does not stay in a line nobody sees. The next sealed file has an empty line.
        choose(u"damaged.qkvm"_s, damaged);
        QVERIFY(isSeen(line));
        QVERIFY(typeIntoIt(thePassphrase()));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportProblem"_s) != nullptr);
        QCOMPARE(desk.ownFiles.fileName(), QString());
        QVERIFY(!isSeen(line));
        QCOMPARE(typed(), QString());
        choose(u"first.qkvm"_s, sealed);
        QVERIFY(isSeen(line));
        QCOMPARE(typed(), QString());
        // A plain file chosen after a passphrase was typed for a sealed one.
        QVERIFY(typeIntoIt(u"typed for the first"_s));
        choose(u"plain.json"_s, plain);
        QVERIFY(!isSeen(line));
        QCOMPARE(typed(), QString());
        choose(u"first.qkvm"_s, sealed);
        QCOMPARE(typed(), QString());
        // A file of a newer kind chosen after it: refused at the choosing.
        QVERIFY(typeIntoIt(u"typed for the first"_s));
        choose(u"newer.qkvm"_s, newer);
        QVERIFY(theOne(desk.window, u"ownImportProblem"_s) != nullptr);
        QVERIFY(holdsNothingUnseen());
        QCOMPARE(typed(), QString());
        // Another sealed file chosen, with the line in sight throughout: what was typed was
        // the first one's, and is not the second one's.
        choose(u"first.qkvm"_s, sealed);
        QVERIFY(typeIntoIt(u"typed for the first"_s));
        choose(u"second.qkvm"_s, sealed);
        QVERIFY(isSeen(line));
        QCOMPARE(typed(), QString());
        // Windows' box given up: the same file is chosen still, and what was typed for it stays.
        QVERIFY(typeIntoIt(u"typed for the second"_s));
        desk.opens.clear();
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QCOMPARE(typed(), u"typed for the second"_s);
        // So after a passphrase that was not the one: the file stays chosen, to be tried again.
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY(says(desk.window, u"That is not the passphrase this file was made with"_s));
        QVERIFY(isSeen(line));
        QCOMPARE(typed(), u"typed for the second"_s);
        // The box closed over it, and opened again.
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(typed(), QString());
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        choose(u"first.qkvm"_s, sealed);
        QVERIFY(isSeen(line));
        QCOMPARE(typed(), QString());
        // And read with the right one: laid out, and the line is empty and gone.
        QVERIFY(typeIntoIt(thePassphrase()));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QVERIFY(holdsNothingUnseen());
        QCOMPARE(typed(), QString());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatCameOfATakingWhereOneWasNotTakenIsShownAndOnlyTheWayOutIsLeft()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s), ticked(3, u"Three"_s)}),
                              thePassphrase()));
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        // Since the file was laid out, somebody added the second by hand in another window.
        QVERIFY(!own.put(elsewhere(2), u"By hand"_s).isEmpty());
        clickOn(desk.window, take);
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        // The box stays: how many were taken, and each that was not, with what it is and why.
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(own.names(), (QStringList{u"By hand"_s, u"One"_s, u"Three"_s}));
        QVERIFY(says(desk.window, u"2 of 3 KVMs were taken onto this wall."_s));
        QVERIFY(says(desk.window, u"Not taken:"_s));
        QTRY_COMPARE(seenOnes(desk.window, u"ownImportLeft"_s).size(), 1);
        QVERIFY(says(desk.window, u"\"Two\" at https://192.0.2.2"_s));
        QVERIFY(
            says(desk.window, u"It cannot be taken: it is on the wall already (\"By hand\" at https://192.0.2.2)."_s));
        // The laying-out is over: no list, no file to choose, nothing to read or take. Only
        // "Close" is left, and it closes.
        QVERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportNothingYet"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportChoose"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportRead"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportTake"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        const QQuickItem* close = theOne(desk.window, u"ownImportCancel"_s);
        QVERIFY(close != nullptr);
        QCOMPARE(close->property("text").toString(), u"Close"_s);
        QVERIFY(toldBy(desk.actions).isEmpty());
        clickOn(desk.window, close);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(!desk.ownFiles.hasTaken());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void afterItsTimeTheListGoesAndTheBoxStaysWhereItIsAndSaysThatTheFileIsToBeChosenAgain()
    {
        WallOfOneTest own;
        // (What is read is kept for five seconds here: what is looked at while the list is
        // there has seconds to be looked at in.)
        Desk desk(own, 5000);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QList<Device> devices;
        for (int number = 1; number <= 30; ++number) {
            devices.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        const QByteArray file = fileOf(devices);
        QVERIFY(chooseAndRead(desk, file, thePassphrase()));
        QTRY_VERIFY(theOne(desk.window, u"ownImportOffered"_s) != nullptr);
        QCOMPARE(desk.exchange.passwordsHeld(), 30);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        // Where the box is, how large, and where its buttons stand, with the list in it. The
        // list is scrolled, as by somebody who reads down it.
        const QObject* box = desk.box(u"ownImportBox"_s);
        const QRectF boxWas(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                            box->property("height").toReal());
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        const QPoint takeStood = middleOf(take);
        const QPoint cancelStood = middleOf(theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(list->hasActiveFocus());
        for (int press = 0; press < 10; ++press) {
            QTest::keyClick(desk.window, Qt::Key_Down);
        }
        QVERIFY(scrolledBy(list) > 150);

        // The time for which what was read is kept is over: the list is gone, nothing of the
        // file is there, and a sentence says what is so and what there is to do: choose the
        // file again. As something that is nothing wrong.
        QTRY_VERIFY_WITH_TIMEOUT(!desk.ownFiles.isLaidOut(), 30000);
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        aFrame(desk.window);
        QVERIFY(theOne(desk.window, u"ownImportTake"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportAbove"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        QVERIFY(!says(desk.window, u"wall-0.qkvm"_s));
        const QQuickItem* note = theOne(desk.window, u"ownImportNote"_s);
        QVERIFY(note != nullptr);
        QCOMPARE(note->property("text").toString(),
                 u"What is read from a file is kept for 5 seconds, and that time is over. Nothing of the file is "
                 "kept: choose it again."_s);
        QVERIFY(isInside(desk.window, note));
        QVERIFY(theOne(desk.window, u"ownImportProblem"_s) == nullptr);
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        // The box has not moved, and is as large as it was: a click that was on its way to
        // "Take" lands where "Take" stood, on a button that takes none ("Read the file", with
        // no file to read), and one that was on its way to "Cancel" lands on "Cancel".
        const QRectF boxIs(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                           box->property("height").toReal());
        QVERIFY2(qAbs(boxIs.y() - boxWas.y()) < 0.5 && qAbs(boxIs.height() - boxWas.height()) < 0.5 &&
                     qAbs(boxIs.x() - boxWas.x()) < 0.5 && qAbs(boxIs.width() - boxWas.width()) < 0.5,
                 qPrintable(u"the box was at %1,%2 and %3 by %4, and is at %5,%6 and %7 by %8"_s.arg(boxWas.x())
                                .arg(boxWas.y())
                                .arg(boxWas.width())
                                .arg(boxWas.height())
                                .arg(boxIs.x())
                                .arg(boxIs.y())
                                .arg(boxIs.width())
                                .arg(boxIs.height())));
        const QQuickItem* read = theOne(desk.window, u"ownImportRead"_s);
        QVERIFY(read != nullptr && !read->isEnabled());
        QVERIFY(read->mapRectToScene(QRectF(0, 0, read->width(), read->height())).contains(takeStood));
        const QQuickItem* cancel = theOne(desk.window, u"ownImportCancel"_s);
        QVERIFY(cancel->mapRectToScene(QRectF(0, 0, cancel->width(), cancel->height())).contains(cancelStood));
        keepPictureOf(desk.window, u"own-import-after-its-time"_s);
        const int openedBefore = desk.opened;
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, takeStood);
        // Nor does a click beside the box close it at that moment, nor Esc: the sentence is
        // there to be read until the box is left by one of its own buttons.
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(desk.window->width() / 2, 6));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTest::qWait(100);
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(desk.opened, openedBefore);
        QVERIFY(theOne(desk.window, u"ownImportNote"_s) != nullptr);
        QVERIFY(own.wall.kvms().isEmpty());

        // What there is to do: choose the file again, and read it. Its list begins at its top,
        // whatever the one before was scrolled to; the sentence is gone, and the box is as any.
        QVERIFY(theOne(desk.window, u"ownImportChoose"_s)->isEnabled());
        QVERIFY(chooseAndRead(desk, file, thePassphrase()));
        aFrame(desk.window);
        QVERIFY(theOne(desk.window, u"ownImportNote"_s) == nullptr);
        QVERIFY2(scrolledBy(list) < 0.5, qPrintable(QString::number(scrolledBy(list))));
        QVERIFY(rowOf(desk.window, 1) != nullptr);
        QCOMPARE(desk.exchange.passwordsHeld(), 30);
        // (And with a file chosen, a box with no list in it closes for a click beside it, as
        // it did before any of this.)
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        aFrame(desk.window);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aFileThatIsChosenAndNotReadGoesAfterTheSameTimeWithWhatWasTypedForIt()
    {
        WallOfOneTest own;
        // (What is read is kept for five seconds here.)
        Desk desk(own, 5000);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // A sealed file is chosen, and a passphrase typed for it; and the box is left so.
        desk.opens = desk.write(u"left.qkvm"_s, fileOf({ticked(1, u"One"_s)}));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        const QQuickItem* line = theOne(desk.window, u"ownImportPassphrase"_s);
        QVERIFY(line != nullptr);
        clickOn(desk.window, line);
        type(desk.window, u"typed for it"_s);
        QCOMPARE(line->property("text").toString(), u"typed for it"_s);
        aFrame(desk.window);
        const QObject* box = desk.box(u"ownImportBox"_s);
        const QRectF boxWas(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                            box->property("height").toReal());
        const QPoint readStood = middleOf(theOne(desk.window, u"ownImportRead"_s));
        // Its time is over: nothing of the file is held, what was typed for it is out of its
        // line, and the box says so as it does after that time. It stays where it is, as large
        // as it was, and is not closed by a click beside it.
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.bytesHeld() == 0, 30000);
        aFrame(desk.window);
        QCOMPARE(line->property("text").toString(), QString());
        QVERIFY(!isSeen(line));
        QVERIFY(!says(desk.window, u"left.qkvm"_s));
        const QQuickItem* note = theOne(desk.window, u"ownImportNote"_s);
        QVERIFY(note != nullptr);
        QCOMPARE(note->property("text").toString(),
                 u"What is read from a file is kept for 5 seconds, and that time is over. Nothing of the file is "
                 "kept: choose it again."_s);
        keepPictureOf(desk.window, u"own-import-chosen-kept-no-longer"_s);
        const QRectF boxIs(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                           box->property("height").toReal());
        QVERIFY2(qAbs(boxIs.y() - boxWas.y()) < 0.5 && qAbs(boxIs.height() - boxWas.height()) < 0.5,
                 qPrintable(u"the box was at %1 and %2 high, and is at %3 and %4 high"_s.arg(boxWas.y())
                                .arg(boxWas.height())
                                .arg(boxIs.y())
                                .arg(boxIs.height())));
        const QQuickItem* read = theOne(desk.window, u"ownImportRead"_s);
        QVERIFY(read != nullptr && !read->isEnabled());
        QVERIFY(read->mapRectToScene(QRectF(0, 0, read->width(), read->height())).contains(readStood));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTest::qWait(100);
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- with the wall, and with this PC's wall ----------------------------------------------------

    void theBoxesGoWhenTheirWindowIsNotAtThisPcsWallAnyMore()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        // A list is shown, and another window went on to a server: this one shows that server's
        // wall now. The box is not left open over it, and what was laid out is forgotten.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s)}), thePassphrase()));
        QCOMPARE(desk.exchange.passwordsHeld(), 1);
        desk.session.show(atAServersWall());
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(!desk.ownFiles.isLaidOut());
        QCOMPARE(desk.ownFiles.fileName(), QString());
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        // Nor over the screen that asks for a server: with the wall, the boxes go.
        desk.session.show(atItsOwnWall());
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s)}), thePassphrase()));
        desk.session.show({});
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(!desk.ownFiles.isLaidOut());
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        // (And with the wall alone, whatever is said of whose wall it was: a window that shows
        // no wall has no box of a file open.)
        desk.session.show(atItsOwnWall());
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s)}), thePassphrase()));
        ui::SessionView::Shown noWall = atItsOwnWall();
        noWall.stage = u"address"_s;
        desk.session.show(noWall);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(!desk.ownFiles.isLaidOut());
        QCOMPARE(desk.exchange.passwordsHeld(), 0);

        // So the box that writes the wall, with a file made and not kept yet.
        desk.session.show(atItsOwnWall());
        desk.savesTo = desk.folder.filePath(u"never.json"_s);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY(desk.ownFiles.isMade());
        desk.session.show(atAServersWall());
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QVERIFY(!desk.ownFiles.isMade());
        desk.session.show(atItsOwnWall());
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY(desk.ownFiles.isMade());
        desk.session.show({});
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QVERIFY(!desk.ownFiles.isMade());
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY(!QFile::exists(desk.savesTo));
        // And from outside, where the window goes out of sight: the program closes them.
        desk.session.show(atItsOwnWall());
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s)}), thePassphrase()));
        desk.ownFiles.close();
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QVERIFY(own.names() == QStringList{u"Build server"_s});
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theBoxesFitTheSmallestWindowAndScrollWhatDoesNotFit()
    {
        WallOfOneTest own;
        // A wall of fourteen, of which Windows hands out no password: fourteen lines beside the
        // file, and the one that every file of this PC's has.
        for (int number = 1; number <= 14; ++number) {
            const QString id = own.put(u"http://127.0.0.1:%1"_s.arg(20000 + number), u"Rack %1"_s.arg(number));
            QVERIFY(!id.isEmpty());
            own.secrets.all.remove(core::LocalWall::secretOf(id));
        }
        // And one more, called by the longest name a KVM can have, with no blank in it.
        const QString longNamed = own.put(u"http://127.0.0.1:20099"_s, QString(80, u'W'));
        QVERIFY(!longNamed.isEmpty());
        own.secrets.all.remove(core::LocalWall::secretOf(longNamed));
        Desk desk(own);
        desk.savesTo = desk.folder.filePath(u"small.qkvm"_s);
        QVERIFY(desk.load(640, 400) != nullptr);
        QCOMPARE(desk.window->height(), 400);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownExportPassphrase"_s)->property("hasKeyboard").toBool());
        type(desk.window, thePassphrase());
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isMade(), 20000);
        QCOMPARE(desk.ownFiles.notInIt().size(), 16);
        aFrame(desk.window);
        // The box is whole inside the window, with its buttons; the lines are scrolled, and say so.
        const auto fits = [&desk](const QString& name) {
            const QObject* box = desk.box(name);
            const qreal top = box->property("y").toReal();
            return top >= 0 && top + box->property("height").toReal() <= desk.window->height();
        };
        QTRY_VERIFY(fits(u"ownExportBox"_s));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownExportSave"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownExportCancel"_s)));
        QVERIFY(theOne(desk.window, u"ownExportScrollMark"_s) != nullptr);
        QObject* lines = desk.window->findChild<QObject*>(u"ownExportNotInIt"_s);
        QVERIFY(lines != nullptr);
        QCOMPARE(lines->property("count").toInt(), 16);
        keepPictureOf(desk.window, u"own-export-small"_s);
        // Every line can be come to: the last one, at the list's end. It names a KVM whose name is
        // longer than a line of the box and has no blank to be broken at: it is whole all the same,
        // broken inside the name, and not cut off at the edge of its note.
        QVERIFY(QMetaObject::invokeMethod(lines, "positionViewAtEnd"));
        aFrame(desk.window);
        const QString last = desk.ownFiles.notInIt().constLast();
        QVERIFY2(last.contains(QString(80, u'W')), qPrintable(last));
        const QQuickItem* said = saying(desk.window->contentItem(), last);
        QVERIFY2(said != nullptr, qPrintable(last));
        QVERIFY(isInside(desk.window, said));
        QVERIFY(!said->property("truncated").toBool());
        QVERIFY2(said->property("contentWidth").toReal() <= said->width() + 0.5,
                 qPrintable(u"%1 in %2"_s.arg(said->property("contentWidth").toReal()).arg(said->width())));
        const QQuickItem* note = said->parentItem();
        QVERIFY(note != nullptr && note->objectName() == u"ownExportLine"_s);
        const QRectF letters = said->mapRectToItem(
            note, QRectF(0, 0, said->property("contentWidth").toReal(), said->property("contentHeight").toReal()));
        QVERIFY2(letters.right() <= note->width() && letters.bottom() <= note->height(),
                 "the line's letters reach beyond its note");
        keepPictureOf(desk.window, u"own-export-small-end"_s);
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));

        // The box that takes a file in, with a list that is longer than the window is high.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOfEveryKind()));
        aFrame(desk.window);
        QTRY_VERIFY(fits(u"ownImportBox"_s));
        // (The server's controller, at the list's end, is one that can be taken, as the wall reads one from M7 1b-ii
        // on: said in short as what it will be known by.)
        bool controllerSaid = false;
        for (int place = 1; place <= 20 && !controllerSaid; ++place) {
            const QQuickItem* row =
                bringIntoSight(desk, place, u"ownImportInShort"_s) ? rowOf(desk.window, place) : nullptr;
            if (row != nullptr && saidIn(row, u"ownImportNameAndAddress"_s).contains(u"A server's controller"_s)) {
                QCOMPARE(saidIn(row, u"ownImportInShort"_s),
                         u"Known by Windows' word "_s + QChar(0xB7) + u" No password in the file"_s);
                controllerSaid = true;
            }
        }
        QVERIFY(controllerSaid);
        QQuickItem* offered = listOf(desk);
        offered->setProperty("contentY", offered->property("originY"));
        aFrame(desk.window);
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportTake"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportCancel"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportChoose"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportCounts"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportNothingYet"_s)));
        QVERIFY(theOne(desk.window, u"ownImportScrollMark"_s) != nullptr);
        keepPictureOf(desk.window, u"own-import-small"_s);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- pictures, for whoever wants to look -------------------------------------------------------

    void everyStateOfBothBoxesInBothLooks_data()
    {
        QTest::addColumn<QString>("look");
        QTest::addColumn<QSize>("size");
        QTest::newRow("dark") << u"dark"_s << QSize(1280, 800);
        QTest::newRow("light") << u"light"_s << QSize(1280, 800);
        QTest::newRow("dark, the smallest window") << u"dark"_s << QSize(640, 400);
        QTest::newRow("light, the smallest window") << u"light"_s << QSize(640, 400);
    }

    void actionAndSettingsPreviewFitsAndShowsTextOnlyOnRequest_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("wide") << QSize(1280, 800);
        QTest::newRow("small") << QSize(640, 400);
    }

    void actionAndSettingsPreviewFitsAndShowsTextOnlyOnRequest()
    {
        QFETCH(QSize, size);
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        const QByteArray file =
            core::testing::plainFile({{u"actions"_s, QJsonArray{QJsonObject{{u"name"_s, u"Example text"_s},
                                                                            {u"kind"_s, u"text"_s},
                                                                            {u"text"_s, u"fixture text\nsecond line"_s},
                                                                            {u"danger"_s, true}}}},
                                      {u"wall"_s, QJsonObject{{u"interval"_s, 2.75}, {u"update_check"_s, false}}}});
        QVERIFY(chooseAndRead(desk, file));
        QTRY_VERIFY(desk.ownFiles.isSettled());
        QVERIFY(!says(desk.window, u"fixture text"_s));
        QCOMPARE(desk.ownFiles.tickedCount(), 1);
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportTake"_s)));
        keepPictureOf(desk.window, u"action-import-%1"_s.arg(size.width()));
        QCOMPARE(theOne(desk.window, u"ownImportTake"_s)->property("text").toString(), u"Import 1"_s);
        const auto actionRow = [&desk]() -> const QQuickItem* {
            QList<const QQuickItem*> rows;
            named(desk.window->contentItem(), u"ownImportRow"_s, rows);
            for (const auto* row : rows) {
                if (row->property("kind").toString() == u"action"_s) {
                    return row;
                }
            }
            return nullptr;
        };
        QVERIFY(actionRow() != nullptr);
        clickOn(desk.window, inRow(actionRow(), u"ownImportDetails"_s));
        QTRY_VERIFY(actionRow() != nullptr && actionRow()->property("open").toBool());
        aFrame(desk.window);
        const QQuickItem* reveal = inRow(actionRow(), u"importShowText"_s);
        QVERIFY(reveal != nullptr);
        QQuickItem* listItem = listOf(desk);
        const qreal buttonY = reveal->mapToItem(listItem, QPointF()).y();
        scrollTo(listItem,
                 std::clamp(scrolledBy(listItem) + buttonY - listItem->height() / 2, 0.0, mostScrolled(listItem)));
        aFrame(desk.window);
        clickOn(desk.window, inRow(actionRow(), u"importShowText"_s));
        QTRY_VERIFY(actionRow() != nullptr && !actionRow()->property("revealedText").toString().isEmpty());
        keepPictureOf(desk.window, u"action-text-review-%1"_s.arg(size.width()));
        QTRY_VERIFY(desk.ownFiles.isSettled());
        desk.ownFiles.setOpen(static_cast<int>(WallExchange::firstActionPlace), false);
        QTRY_VERIFY(actionRow() == nullptr || actionRow()->property("revealedText").toString().isEmpty());
        const auto* list = desk.ownFiles.offered();
        bool foundSettings = false;
        for (int at = 0; at < list->rowCount(); ++at) {
            const auto index = list->index(at);
            if (index.data(ui::LaidOutList::KindRole).toString() == u"settings"_s) {
                foundSettings = true;
                QVERIFY(!index.data(ui::LaidOutList::TickedRole).toBool());
            }
        }
        QVERIFY(foundSettings);
        QCOMPARE(desk.ownFiles.actionText(static_cast<int>(WallExchange::firstActionPlace)),
                 u"fixture text"_s + QChar(0x21B5) + u"\nsecond line"_s);
        desk.ownFiles.reset();
        QVERIFY(desk.ownFiles.actionText(static_cast<int>(WallExchange::firstActionPlace)).isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void everyStateOfBothBoxesInBothLooks()
    {
        QFETCH(QString, look);
        QFETCH(QSize, size);
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        const QString rack = own.put(u"http://127.0.0.1:20002"_s, u"Rack"_s);
        own.secrets.all.remove(core::LocalWall::secretOf(rack));
        QVERIFY(!own.put(elsewhere(11), u"There already"_s).isEmpty());
        HeldWork work;
        Desk desk(own, WallExchange::keptForMsAsItIs, work.sealing());
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QCOMPARE(desk.window->size(), size);
        // (Every look is drawn and kept: the program's own, which a test sets without Windows. The
        // pictures of the smallest window are called so.)
        QVERIFY2(lookIs(desk, look), qPrintable(u"the screens are not drawn in the %1 look"_s.arg(look)));
        const QString ofTheSize = size.height() < 500 ? u"-%1x%2"_s.arg(size.width()).arg(size.height()) : QString();
        const auto keep = [&](const QString& name) { keepPictureOf(desk.window, name + u'-' + look + ofTheSize); };
        // (The pictures of a button that takes no click yet are taken in a long moment; and
        // what is then waited for, in a short one.)
        const auto restLong = [&desk] { desk.ownFiles.restFor(Desk::longRest, Desk::longRest); };
        const auto restShort = [&desk] { desk.ownFiles.restFor(Desk::quickRest, Desk::quickRest); };

        // Export: empty; working; the lines, with "Save".
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownExportPassphrase"_s)->property("hasKeyboard").toBool());
        keep(u"own-export-1-empty"_s);
        type(desk.window, thePassphrase());
        work.hold();
        restLong();
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_COMPARE(work.begun(), 1);
        QTRY_VERIFY(theOne(desk.window, u"ownExportWorking"_s) != nullptr);
        keep(u"own-export-2-working"_s);
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isMade(), 20000);
        QTRY_VERIFY(theOne(desk.window, u"ownExportSave"_s) != nullptr);
        keep(u"own-export-3-lines-save-resting"_s);
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownExportSave"_s)->isEnabled(), 20000);
        keep(u"own-export-3-lines-save"_s);
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));

        // Import: a file chosen, and sealed; laid out, with devices of every kind (from its
        // top to its end); the top of a long list; what came of a taking where one was not taken.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        keep(u"own-import-0-empty"_s);
        desk.opens = desk.write(u"sealed.qkvm"_s, fileOf({ticked(1, u"One"_s)}));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportPassphrase"_s) != nullptr);
        clickOn(desk.window, theOne(desk.window, u"ownImportPassphrase"_s));
        type(desk.window, u"a passphrase"_s);
        keep(u"own-import-1-chosen-sealed"_s);
        desk.opens = desk.write(u"every-kind.json"_s, fileOfEveryKind());
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(theOne(desk.window, u"ownImportTake"_s) != nullptr);
        keep(u"own-import-2-laid-out-resting"_s);
        // (Its notes, folded, opened once the list has stood for its moment.)
        QVERIFY(theOne(desk.window, u"ownImportMoreNotes"_s) != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, theOne(desk.window, u"ownImportMoreNotes"_s));
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownImportTake"_s)->isEnabled(), 20000);
        keep(u"own-import-2-laid-out-a"_s);
        restShort();
        QObject* list = desk.window->findChild<QObject*>(u"ownImportOffered"_s);
        QVERIFY(list != nullptr);
        const qreal page = list->property("height").toReal();
        const char* const parts = "bcdefghij";
        for (int part = 0; parts[part] != '\0'; ++part) {
            const qreal most = list->property("contentHeight").toReal() - page;
            const qreal next = std::min(most, list->property("contentY").toReal() + page - 24);
            if (next <= list->property("contentY").toReal() + 1) {
                break;
            }
            list->setProperty("contentY", next);
            aFrame(desk.window);
            keep(u"own-import-2-laid-out-"_s + QLatin1Char(parts[part]));
        }
        // (And the same list with a row of each kind opened, from its top: before, opened, and
        // closed again.)
        list->setProperty("contentY", list->property("originY"));
        aFrame(desk.window);
        keep(u"own-import-2-laid-out-rows-before"_s);
        for (const bool opening : {true, false}) {
            for (const int place : {1, 5}) {
                QVERIFY(bringIntoSight(desk, place, u"ownImportDetails"_s));
                QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
                clickOn(desk.window, inRow(rowOf(desk.window, place), u"ownImportDetails"_s));
                QTRY_COMPARE(isSeen(inRow(rowOf(desk.window, place), u"ownImportBehind"_s)), opening);
            }
            QVERIFY(bringIntoSight(desk, 1, u"ownImportDetails"_s));
            keep(opening ? u"own-import-2-laid-out-rows-opened"_s : u"own-import-2-laid-out-rows-closed-again"_s);
        }
        desk.opens = desk.write(u"two-thousand.json"_s, fileOfSoMany(2000));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 60000);
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        keep(u"own-import-3-long-list-top"_s);
        desk.opens = desk.write(u"three.json"_s,
                                fileOf({ticked(21, u"One"_s), ticked(22, u"Two"_s), ticked(23, u"Three"_s)}, false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownImportTake"_s) != nullptr &&
                                     theOne(desk.window, u"ownImportTake"_s)->isEnabled(),
                                 20000);
        QVERIFY(!own.put(elsewhere(22), u"By hand"_s).isEmpty());
        clickOn(desk.window, theOne(desk.window, u"ownImportTake"_s));
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        keep(u"own-import-4-took-one-not-taken"_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        // And another file is being worked on just now, which is said as what passes.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        WallExchange other(own.wall, work.sealing());
        QObject asker;
        work.hold();
        other.layOut(fileOf({ticked(1, u"One"_s)}), thePassphrase(), &asker, [](const auto&) {});
        QTRY_COMPARE(work.begun(), 2);
        desk.opens = desk.write(u"waits.json"_s, fileOf({ticked(31, u"One"_s)}, false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportNote"_s) != nullptr);
        keep(u"own-import-6-another-file-is-worked-on"_s);
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(!other.isBusy(), 20000);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));

        // After the time for which what was read is kept (a few seconds, here): the box as it
        // was with its list, and then with its sentence.
        WallOfOneTest ownToo;
        Desk brief(ownToo, 4000);
        QVERIFY(brief.load(size.width(), size.height()) != nullptr);
        QVERIFY(lookIs(brief, look));
        QVERIFY(openFromTheSettings(brief, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(brief, anOrdinaryFile(), thePassphrase()));
        QTRY_VERIFY_WITH_TIMEOUT(!brief.ownFiles.isLaidOut(), 30000);
        QTRY_VERIFY(theOne(brief.window, u"ownImportNote"_s) != nullptr);
        keepPictureOf(brief.window, u"own-import-5-kept-no-longer-"_s + look + ofTheSize);
        QVERIFY2(brief.complaints.isEmpty(), qPrintable(brief.complaints.join(u'\n')));
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
    TestOwnFilesScreens test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_own_files_screens.moc"
