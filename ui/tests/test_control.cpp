#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QTimer>

#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/live.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/input/keys.h>

#include "control.h"
#include "control_view.h"
#include "jetkvm_by_hand.h"
#include "jetkvm_viewer_by_hand.h"
#include "keys_by_hand.h"
#include "win/keyboard_hook.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using ui::ControlView;
using ui::KeysByHand;
using ui::MachineControl;

namespace {

// Some keys, as Windows tells of them: the keyboard's number, the mark, Windows' name.
constexpr input::Pressed keyA{0x1E, false, 0x41};
constexpr input::Pressed keyC{0x2E, false, 0x43};
constexpr input::Pressed del{0x53, true, 0x2E};
constexpr input::Pressed leftCtrl{0x1D, false, 0xA2};
constexpr input::Pressed rightCtrl{0x1D, true, 0xA3};
constexpr input::Pressed leftShift{0x2A, false, 0xA0};
constexpr input::Pressed leftWindows{0x5B, true, 0x5B};
constexpr input::Pressed rightWindows{0x5C, true, 0x5C};
constexpr input::Pressed keyL{0x26, false, 0x4C};
// A key for sound: it has no USB number on the keyboard's page.
constexpr input::Pressed louder{0x30, true, 0xAF};
constexpr input::Pressed enter{0x1C, false, 0x0D};

// What a machine says by itself of keyboard and mouse, when it is asked for them and when they
// are given back: the two ways there are.
enum class Says {
    // A word for every asking and every giving back, in the order they were said, a turn of
    // the loop after: as a KVM of this PC's own does (core/kvmd_live). Its yes or no to an
    // asking is the test's to say (`give`, `refuse`), for as long as the asking stands. A
    // giving back is answered by a no without a sentence; and one that calls an asking off
    // before it was answered by two words: a no for the asking, with the code "given_up" and
    // a sentence, and then that word.
    EveryWord,
    // No word by itself: as a line that has none for a giving back (a QKVM server's may not).
    // And for a test that says each word of the machine by hand, at the moment it is to come.
    NoWord,
};

// The sentence a machine gives with its "no" to an asking that was called off (made up here:
// it is shown nowhere).
const QString calledOff = u"Keyboard and mouse of this machine were asked for no longer."_s;

// A machine that is had live, and notes everything it is told, in order: "ask" and "give back"
// (keyboard and mouse), "+4" and "-4" (a key by its USB number, down and up), "at 1,2" (the
// pointer), "b1+" and "b1-" (a button), "wheel 0,1", "release all". A test says what the
// machine says to an asking; what it says by itself is `says`.
class NotedLive : public core::Live {
public:
    explicit NotedLive(Says how = Says::EveryWord) : Live(nullptr), says(how) {}

    bool isOpen() const override { return open; }
    ControlOwner controlOwner() const override { return owner; }
    void reportOwner(ControlOwner value)
    {
        owner = std::move(value);
        emit controlOwnerChanged();
    }
    void takeOver(const QString& expected) override
    {
        m_isAsked = true;
        said.append(u"take from "_s + expected);
    }
    Pointer pointerMode() const override { return mode; }
    void setNeed(Need /*need*/) override {}
    void askForAKeyFrame() override {}
    void chooseRelativeMouse(bool relative) override { chosenRelative = relative; }
    void askControl(bool on) override
    {
        if (on) {
            askedRelative = chosenRelative;
        }
        said.append(on ? u"ask"_s : u"give back"_s);
        if (says != Says::EveryWord) {
            return;
        }
        if (on) {
            m_isAsked = true;
            return;
        }
        // (An asking that still stood is called off by it: answered first, as it was asked first.)
        if (std::exchange(m_isAsked, false)) {
            sayLater(u"given_up"_s, calledOff);
        }
        holds = false;
        sayLater({}, {});
    }
    bool hasControl() const override { return holds; }
    void key(int usage, bool down) override { said.append((down ? u"+"_s : u"-"_s) + QString::number(usage, 16)); }
    void pointerAt(int x, int y) override
    {
        said.append(u"at %1,%2"_s.arg(x).arg(y));
        if (endsWhilePointing) {
            endsWhilePointing = false;
            end(u"unreachable"_s, u"Lost contact with the KVM."_s);
        }
    }
    void pointerBy(int dx, int dy) override { said.append(u"by %1,%2"_s.arg(dx).arg(dy)); }
    void button(int number, bool down) override { said.append(u"b%1%2"_s.arg(number).arg(down ? u'+' : u'-')); }
    void wheel(int dx, int dy) override { said.append(u"wheel %1,%2"_s.arg(dx).arg(dy)); }
    void releaseAll() override
    {
        said.append(u"release all"_s);
        if (whileLettingGo) {
            whileLettingGo();
        }
    }
    void close() override { open = false; }

    // What the machine says: yes to keyboard and mouse; no, or that they are taken away; how
    // large its picture is; that it is open now; and that it is over.
    void give()
    {
        m_isAsked = false;
        holds = true;
        reportOwner({});
        emit controlChanged(true, {}, {});
    }
    void refuse(const QString& code, const QString& message)
    {
        m_isAsked = false;
        holds = false;
        emit controlChanged(false, code, message);
    }
    void sayPicture(int width, int height) { emit videoChanged({false, width, height}); }
    void comeOpen()
    {
        open = true;
        emit opened();
    }
    void end(const QString& code, const QString& message)
    {
        open = false;
        holds = false;
        emit closed(code, message);
    }
    // What it was told since this was last asked.
    QStringList take() { return std::exchange(said, {}); }

    // What it says by itself. (Said before it is told anything.)
    Says says;
    bool open = true;
    bool holds = false;
    ControlOwner owner;
    Pointer mode = Pointer::Both;
    std::optional<bool> chosenRelative;
    std::optional<bool> askedRelative;
    QStringList said;
    // Run in the middle of letting go: for looking at what else is so at that moment.
    std::function<void()> whileLettingGo;
    // Its line ends in the middle of being told where the pointer is, once.
    bool endsWhilePointing = false;

private:
    // A no, a turn of the loop after it was told what this answers: as a machine says it, from
    // its own work and not from inside the call. (One whose line has ended says nothing more.)
    void sayLater(const QString& code, const QString& message)
    {
        QTimer::singleShot(0, this, [this, code, message] {
            if (open) {
                emit controlChanged(false, code, message);
            }
        });
    }

    // It was asked for keyboard and mouse, and has not answered yet.
    bool m_isAsked = false;
};

// Longer than the pause after a place of the pointer was said, in the tests' own pace: whatever
// waited for that pause is said by the end of this, also on a PC that is busy with other work.
void rest()
{
    constexpr int longerThanThePauseMs = 60;
    QTest::qWait(longerThanThePauseMs);
    QCoreApplication::processEvents();
}

// One window's control with what it stands between: a machine that is had live, the view its
// screen is handed, and keys that a test presses. The times are short ones: the key source is
// told every 20 ms that the program is alive, two places of the pointer are 40 ms apart, and the
// machine is given 150 ms to answer. `shared`: the one memory of what is held that a program's
// windows have together; none: this control has its own, as where there is one window. The
// machine says a word for every asking and every giving back, unless a test says otherwise
// (`Says`): a test of askings says which of the two it is.
struct Desk {
    explicit Desk(MachineControl::Pace pace = {20, 40, 150}, ui::KeysKept* shared = nullptr)
    {
        control = shared != nullptr ? std::make_unique<MachineControl>(view, keys, *shared, pace)
                                    : std::make_unique<MachineControl>(view, keys, pace);
        control->setAllowed(true);
        control->setInFront(true);
        control->have(&live, u"a"_s);
    }
    explicit Desk(Says how, MachineControl::Pace pace = {20, 40, 150}) : Desk(pace) { live.says = how; }

    // The machine says what it has to say of what it was told: a turn of the loop, as a machine
    // that says its words takes. (One that says none says none here either: whoever tries that
    // waits for the time a word is owed, or says the words by hand.)
    void letTheMachineSpeak()
    {
        if (view.isBusy()) {
            QCoreApplication::processEvents();
        }
    }
    // Keyboard and mouse are taken, and the machine has said yes. False where that did not come
    // about. What the machine was told for it is forgotten. (Where they were held or asked for
    // before, the machine is in the state a machine is in after that: it has said its words
    // for it, where it says them, and nothing was said for it in its place.)
    bool take()
    {
        letTheMachineSpeak();
        view.take();
        live.give();
        live.said.clear();
        return control->isOn() && view.isOn();
    }
    // The same, with the pointer over the middle of the picture, said and waited out.
    bool takeWithThePointerOver()
    {
        if (!take()) {
            return false;
        }
        control->pointerAt(0.5, 0.5);
        rest();
        return live.take() == QStringList{u"at 16384,16384"_s};
    }

    NotedLive live;
    ControlView view;
    KeysByHand keys;
    std::unique_ptr<MachineControl> control;
};

const QStringList letGo = {u"release all"_s, u"give back"_s};

const QString answeredLate = u"Keyboard and mouse were not taken: the machine answered late."_s;
const QString didNotAnswer = u"The machine did not answer when its keyboard and mouse were asked for."_s;
// What is said with why an asking was given up, of asking again: while the machine still owes a
// word, when that can be done; and once it can, to do it.
const QString askAgainLater =
    u" Control can be clicked again once the machine has answered, or after the time it is given for that."_s;
const QString askAgainNow = u" Click Control again."_s;
// The times of a control that owes a word for no longer than a test can wait out: the machine is
// given 100 ms to answer, and a word is owed for 400 ms. (In the program: five seconds each.)
constexpr MachineControl::Pace owedForAMoment{20, 40, 100, 60'000, 3000, 600'000, 1000, 400};

// Which window Windows has in front, as a test says it to the hook's procedure
// (`hook::Procedure::isFor`): the window the keys are for, or another program's. Its own, unless a
// test says otherwise; each desk of the hook begins with it so.
constexpr std::uintptr_t ownWindow = 0x1010;
constexpr std::uintptr_t anotherWindow = 0x2020;
std::uintptr_t inFront = ownWindow;
std::uintptr_t windowInFront() noexcept
{
    return inFront;
}

// The key source as the program has it, but for Windows itself: the hook's own procedure on
// plain values (`hook::Procedure`), and what the interface thread makes of what that has for it
// (`hook::whatToTell`), told as `WindowsKeyboard` tells it. A test hands it each key as Windows
// hands a hook its keys (KBDLLHOOKSTRUCT) and tells it of each as Raw Input tells of it
// (RAWKEYBOARD). No hook is put in, nothing is asked of Windows, and no key of this PC is heard.
class KeysAsWindowsTellsOfThem final : public ui::KeySource {
public:
    using KeySource::KeySource;

    bool begin(std::uint16_t leaveKey = input::usageRightCtrl) override
    {
        if (m_listens && m_procedure.leaveKey() != leaveKey) {
            end();
        }
        if (!m_listens) {
            m_procedure.begin(m_now, ui::hook::windowsWaitsMs, leaveKey);
            m_procedure.isFor(ownWindow, &windowInFront);
            m_telling.forget();
            for (const input::Pressed& key : heldAsItBegins) {
                m_procedure.windowsHas(key.virtualKey);
                m_procedure.isDownAlready(key);
            }
            if (secondAccount) {
                // (Asked for with Windows' word for a keyboard that goes, as the hook's thread
                // asks for it; and Windows took the asking.)
                m_procedure.rawInputTells();
                m_procedure.keyboardsGoingIsTold();
            }
            m_listens = true;
        }
        return true;
    }
    void end() override
    {
        m_procedure.forget();
        m_telling.forget();
        m_listens = false;
    }
    std::vector<input::Pressed> whatIsDown() const override
    {
        return ui::hook::together(m_procedure.whatIsDown(), heldAsItBegins);
    }
    std::vector<input::Pressed> whatIsKept() const override { return m_procedure.whatIsKept(); }
    bool isWatched() const override { return m_procedure.isWatched(); }
    bool isToldOfKeyboardsGoing() const override { return m_procedure.isToldOfKeyboardsGoing(); }
    bool swallows() const override { return m_procedure.swallows(); }
    void setSwallowing(bool swallowing) override { m_procedure.setSwallowing(swallowing, m_now); }
    void stillThere() override { m_procedure.stillThere(m_now); }
    OnTheirWay onTheirWay() const override
    {
        const ui::hook::Procedure::OnTheirWay way = m_procedure.onTheirWay();
        return {way.last, static_cast<int>(way.count)};
    }
    void tellNow() override { tellAll(); }

    // The hook is handed a key: Windows' name for it, the keyboard's number, the flags. What
    // the procedure made of it: whether it was kept from Windows.
    ui::hook::Verdict hears(std::uint32_t name, std::uint32_t scanCode, std::uint32_t flags, bool down)
    {
        ++m_now;
        const ui::hook::Verdict verdict = m_procedure.key(name, scanCode, flags, down, m_now);
        tell();
        return verdict;
    }
    // Raw Input tells of a key: the keyboard's number, its marks, Windows' name. Of whichever
    // keyboard: which, is not said to the procedure. (Of what is no key of its own there,
    // nothing. And nothing at all on a PC that gives no second account: a test says the same
    // of both kinds of PC, and this leaves out what is not told there.)
    void isToldOf(std::uint32_t makeCode, std::uint32_t flags, std::uint32_t name, bool down)
    {
        if (!secondAccount) {
            return;
        }
        ++m_now;
        const std::optional<input::Pressed> key =
            ui::hook::fromRawInput(makeCode, flags | (down ? 0U : ui::hook::rawUpFlag), name);
        if (key.has_value()) {
            static_cast<void>(m_procedure.rawKey(*key, down, m_now));
        }
        tell();
    }
    // The hook's thread looks, as it does every so often: so much later.
    void looks(std::uint64_t laterMs = 1)
    {
        m_now += laterMs;
        m_procedure.stillThere(m_now);
        static_cast<void>(m_procedure.look(m_now));
        tell();
    }
    // Windows says that a keyboard was taken away from this PC, as it says so to the hook's
    // thread: a made-up notice, of no keyboard in particular. No "up" comes for what was held.
    void aKeyboardGoes()
    {
        ++m_now;
        static_cast<void>(m_procedure.aKeyboardWent(m_now));
        tell();
    }

    // The interface thread is held up from here on: what the hook's thread hands it waits to be
    // taken, as it does while that thread is at other work. And it is back, so much later, and
    // takes what waits: all of it in one pass.
    void isHeldUp() { m_isHeldUp = true; }
    void isBack(std::uint64_t laterMs = 1)
    {
        m_isHeldUp = false;
        m_now += laterMs;
        tellAll();
    }
    // The hook is handed a key, or Raw Input tells of one, so much later than the last.
    void later(std::uint64_t laterMs) { m_now += laterMs; }

    // Windows gives a second account of the keyboard (Raw Input); and the keys that Windows has
    // as down as the hook goes in. Each said before it begins.
    bool secondAccount = false;
    std::vector<input::Pressed> heldAsItBegins;

private:
    // As `WindowsKeyboard::handOn` tells what the procedure has for whoever listens: by the
    // same telling (`hook::Telling`).
    void tellAll()
    {
        m_telling.all(m_procedure, m_now, *this, [] { return true; });
    }
    void tell()
    {
        if (!m_isHeldUp) {
            tellAll();
        }
    }

    ui::hook::Procedure m_procedure;
    ui::hook::Telling m_telling;
    // (A clock of its own, which goes on by a millisecond with every key.)
    std::uint64_t m_now = 1000;
    bool m_listens = false;
    bool m_isHeldUp = false;
};

// A window's control between a machine that is had live and that key source.
struct DeskOfTheHook {
    explicit DeskOfTheHook(bool secondAccount = false, std::vector<input::Pressed> heldAsItBegins = {})
    {
        inFront = ownWindow;
        keys.secondAccount = secondAccount;
        keys.heldAsItBegins = std::move(heldAsItBegins);
        control = std::make_unique<MachineControl>(view, keys, MachineControl::Pace{20, 40, 150});
        control->setAllowed(true);
        control->setInFront(true);
        control->have(&live, u"a"_s);
    }

    bool take()
    {
        // (The machine has said by now what it had to say of before, as in `Desk::take`.)
        if (view.isBusy()) {
            QCoreApplication::processEvents();
        }
        view.take();
        live.give();
        live.said.clear();
        return control->isOn() && view.isOn();
    }

    NotedLive live;
    ControlView view;
    KeysAsWindowsTellsOfThem keys;
    std::unique_ptr<MachineControl> control;
};

// What the hook and Raw Input say of some keys. For the hook: the mark of a key that has one.
constexpr std::uint32_t marked = ui::hook::markedFlag;
constexpr std::uint32_t rawMarked = ui::hook::rawMarkedFlag;
// The name Windows gives what it cannot name.
constexpr std::uint32_t noName = 0xFF;

// What the bar says, while they are taken, of the keys that were held when they were last given
// back (%1: which).
QString heldFromBefore()
{
    return u"Held when keyboard and mouse were last given back, and not heard to be let go since: %1. Until each "
           "is let go or pressed anew, no other key and no click goes to the machine."_s;
}

// The ways a holding of keyboard and mouse, or an asking for them, ends, each with what the bar
// says of it: at once (`said`); while the machine's word for it is owed and has not come for a
// moment (`saidMeanwhile`); and once Control is offered again (`saidOnceOffered`).
void eachEndingOfHoldingOrAsking()
{
    QTest::addColumn<int>("ending");
    QTest::addColumn<QString>("said");
    QTest::addColumn<QString>("saidMeanwhile");
    QTest::addColumn<QString>("saidOnceOffered");
    const QString aKey = u"A key was pressed before the machine answered, so keyboard and mouse were not taken."_s;
    const QString notInFront = u"This window was no longer in front before the machine answered, so keyboard and "
                               "mouse were not taken."_s;
    const QString inUse = u"Keyboard and mouse of this machine are in use in another window. Release them there "
                          "first."_s;
    const QString takenAway = u"An admin took keyboard and mouse of this machine."_s;
    // Why Control is not offered while the machine owes a word, where nothing else says so.
    const QString notOffered = u"The machine has still to confirm that its keyboard and mouse are no longer held or "
                               "asked for here. Control can be clicked again once it has, or after a few seconds."_s;
    QTest::newRow("1: an asking is given up, a key was pressed too soon")
        << 1 << aKey + askAgainLater << aKey + askAgainLater << aKey + askAgainNow;
    QTest::newRow("2: an asking is given up, the window was no longer in front")
        << 2 << notInFront + askAgainLater << notInFront + askAgainLater << notInFront + askAgainNow;
    QTest::newRow("3: an asking is given up by Stop asking") << 3 << QString() << notOffered << QString();
    QTest::newRow("4: an asking is refused, in use in another window")
        << 4 << inUse << inUse + u' ' + notOffered << inUse;
    QTest::newRow("5: keyboard and mouse are taken away")
        << 5 << takenAway << takenAway + u' ' + notOffered << takenAway;
    QTest::newRow("6: a plain giving back") << 6 << QString() << notOffered << QString();
    QTest::newRow("7: an asking gets no answer in its time")
        << 7 << didNotAnswer << didNotAnswer + u' ' + notOffered << didNotAnswer;
}

// One of those endings but the last (which is waited for), brought about at a desk.
void bringAbout(Desk& desk, int ending)
{
    desk.view.take();
    switch (ending) {
    case 1:
        desk.view.wentOn(true);
        break;
    case 2:
        desk.control->setInFront(false);
        desk.control->setInFront(true);
        break;
    case 3:
        desk.view.giveBack();
        break;
    case 4:
        desk.live.refuse(u"in_use"_s, u"Keyboard and mouse of this machine are in use in another window. Release "
                                      "them there first."_s);
        break;
    case 5:
        desk.live.give();
        desk.live.refuse(u"taken"_s, u"An admin took keyboard and mouse of this machine."_s);
        break;
    default:
        desk.live.give();
        desk.view.giveBack();
        break;
    }
}

} // namespace

// Keyboard and mouse of a machine that is shown large: what goes to the machine, what never does,
// and that everything is let go there whichever way it ends. No key of this PC is listened for:
// the keys are pressed by hand.
class TestControl : public QObject {
    Q_OBJECT

private slots:
    // (The review of the fitted view, 2026-11-06: an own JetKVM's mouse in the enlarged view.) The JetKVM's live says
    // the size its key frames show, so a click in the fitted view, the picture letterboxed in a tile of another shape,
    // lands where it was made on the JetKVM, its button with it; and where the JetKVM's screen changes its size, the
    // view follows. (Before, its live said 0 by 0, the view placed the pointer nowhere, and no pointer went.) Against a
    // JetKVM by hand on this PC's own address.
    void anOwnJetKvmsPointerLandsWhereTheFittedViewIsClicked()
    {
        using core::testing::JetKvmByHand;
        using core::testing::JetKvmPeerByHand;
        using core::testing::JetKvmViewerByHand;
        if (!JetKvmPeerByHand::filmIsThere() || !JetKvmPeerByHand::filmIsThere("desktop-720p15.h264")) {
            QSKIP(JetKvmPeerByHand::noFilm);
        }
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        core::JetKvmDevice::Pace pace;
        pace.connectWithinMs = 3000;
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.viewer.connection.watchEveryMs = 100;
        core::JetKvmDevice::Where where;
        where.address = jet.address();
        core::JetKvmDevice device(where, u"secret"_s, pace);
        const std::unique_ptr<core::OwnLive> own = device.makeLive();
        const std::unique_ptr<core::Live> live = own->open({});
        ControlView view;
        KeysByHand keys;
        MachineControl control(view, keys);
        control.setAllowed(true);
        control.setInFront(true);
        control.have(live.get(), u"jet"_s);
        view.take();
        QTRY_VERIFY_WITH_TIMEOUT(control.isOn(), 20'000);
        // A tile of 1000 by 1000: a picture of 16 by 9 fills its width, 562.5 high, with 218.75 above and below.
        const auto clickAt = [&view](double across, double down) {
            view.pointerAt(1000 * across, 218.75 + 562.5 * down, 1000, 1000);
        };
        QTRY_VERIFY_WITH_TIMEOUT((clickAt(0.25, 0.25), viewer.place() == QPoint(8192, 8192)), 20'000);
        QCOMPARE(view.pictureSize(), QSize(1920, 1080));
        view.button(Qt::LeftButton, true);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.absButtons(), 1, 5000);
        QCOMPARE(viewer.place(), QPoint(8192, 8192));
        view.button(Qt::LeftButton, false);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.absButtons(), 0, 5000);
        // Its screen changes its size: the view follows, and a click lands where it is made.
        QVERIFY(viewer.filmAnew("desktop-720p15.h264"));
        QTRY_COMPARE_WITH_TIMEOUT(view.pictureSize(), QSize(1280, 720), 20'000);
        QTRY_VERIFY_WITH_TIMEOUT((clickAt(0.75, 0.75), viewer.place() == QPoint(24575, 24575)), 5000);
        control.giveBack();
        QTRY_VERIFY_WITH_TIMEOUT(!control.isOn(), 5000);
    }

    void localConsoleControlsKeepOwnershipAndReleasePhysicalInput()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        desk.live.take();
        desk.keys.press(leftCtrl);
        desk.control->button(1, true);
        desk.live.take();
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "setLocalInput", Q_ARG(bool, true)));
        QVERIFY(desk.view.property("localInput").toBool());
        QVERIFY(desk.view.isOn() && desk.live.hasControl());
        QVERIFY(!desk.keys.swallows());
        QCOMPARE(desk.live.take(), QStringList{u"release all"_s});
        desk.keys.press(keyA);
        desk.control->pointerAt(0.8, 0.8);
        desk.control->button(1, true);
        desk.control->wheel(0, 120);
        rest();
        QVERIFY(desk.view.isOn());
        QVERIFY(desk.live.take().isEmpty());
        desk.keys.release(leftCtrl);
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "setLocalInput", Q_ARG(bool, false)));
        QVERIFY(desk.keys.swallows());
        desk.keys.repeat(keyA);
        QVERIFY(desk.live.take().isEmpty());
        desk.keys.release(keyA);
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), QStringList({u"+4"_s, u"-4"_s}));
    }

    void screenKeysAreBalancedAndNeverAcquireControl()
    {
        Desk desk;
        const QList<int> chord{0xe1, 4};
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "tapKeys", Q_ARG(QList<int>, chord)));
        QVERIFY(desk.live.take().isEmpty());
        QVERIFY(desk.take());
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "tapKeys", Q_ARG(QList<int>, chord)));
        QCOMPARE(desk.live.take(), QStringList({u"release all"_s, u"+e1"_s, u"+4"_s, u"-4"_s, u"-e1"_s}));
        desk.control->setAllowed(false);
        desk.live.take();
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "tapKeys", Q_ARG(QList<int>, chord)));
        QVERIFY(desk.live.take().isEmpty());
    }

    void invalidScreenChordsSendNothing()
    {
        Desk desk;
        QVERIFY(desk.take());
        for (const QList<int>& chord :
             {QList<int>{}, QList<int>{0}, QList<int>{0x66}, QList<int>{0xe8}, QList<int>{-1}, QList<int>{4, 4},
              QList<int>{4, 5}, QList<int>{224, 225, 226, 227, 228, 229, 230, 231, 4, 5}}) {
            desk.view.tapKeys(chord);
            QVERIFY(desk.live.take().isEmpty());
            QVERIFY(desk.view.isOn());
        }
    }

    void aScreenChordIsDiscardedWhenReleasingChangesItsTargetOrPermission_data()
    {
        QTest::addColumn<int>("change");
        for (int change = 0; change < 4; ++change)
            QTest::addRow("change-%d", change) << change;
    }

    void aScreenChordIsDiscardedWhenReleasingChangesItsTargetOrPermission()
    {
        QFETCH(int, change);
        Desk desk;
        NotedLive other;
        QVERIFY(desk.take());
        auto revoke = [&] {
            switch (change) {
            case 0:
                desk.control->setAllowed(false);
                break;
            case 1:
                desk.control->setInFront(false);
                break;
            case 2:
                desk.control->have(&other, u"other"_s);
                break;
            case 3:
                desk.live.refuse(u"taken"_s, u"Another holder"_s);
                break;
            default:
                QFAIL("Unexpected target change");
            }
        };
        bool changed = false;
        desk.live.whileLettingGo = [&] {
            if (std::exchange(changed, true))
                return;
            revoke();
        };
        desk.view.tapKeys({225, 4});
        QVERIFY(!desk.live.said.contains(u"+4"_s));
        QVERIFY(!desk.live.said.contains(u"+e1"_s));
        QVERIFY(other.said.isEmpty());
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.keys.swallows());
    }

    void aLocalPanelCannotSurviveFocusLossOrAnUnsuccessfulResume()
    {
        Desk desk;
        QVERIFY(desk.take());
        desk.view.setLocalInput(true);
        desk.control->setInFront(false);
        QVERIFY(!desk.view.isOn() && !desk.view.localInput() && !desk.keys.swallows());
        desk.view.setLocalInput(false);
        desk.view.tapKeys({4});
        QVERIFY(!desk.live.said.contains(u"+4"_s));
        desk.control->setInFront(true);
        QVERIFY(desk.take());
        desk.view.setLocalInput(true);
        desk.keys.lost = true;
        desk.view.setLocalInput(false);
        QVERIFY(!desk.view.isOn() && !desk.keys.swallows());
        QVERIFY(!desk.view.problem().isEmpty());
    }

    void takeoverIsExplicitAndNormalRequestsNeverEscalate()
    {
        Desk desk;
        desk.live.reportOwner({u"First operator"_s, u"owner-reference"_s, false});
        QCOMPARE(desk.view.ownerName(), u"First operator"_s);
        QVERIFY(!desk.view.takeoverPossible());
        desk.control->askTakeover();
        QVERIFY(!desk.view.takeoverPending());
        desk.control->setTakeoverAllowed(true);
        QVERIFY(desk.view.takeoverPossible());
        desk.view.take();
        QVERIFY(desk.view.takeoverPending());
        QCOMPARE(desk.view.takeoverOwner(), u"First operator"_s);
        QVERIFY(desk.live.said.isEmpty());
        desk.view.answerTakeover(false);
        QVERIFY(!desk.view.takeoverPending());
        QVERIFY(desk.live.said.isEmpty());
        desk.control->take(); // Also used after typing: it must remain an ordinary request.
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        desk.live.refuse(u"taken"_s, u"Already controlled"_s);
        QTRY_VERIFY(!desk.view.isBusy());
        desk.live.take(); // Release of this refused request has been acknowledged.
        desk.view.take();
        QVERIFY(desk.view.takeoverPending());
        desk.view.answerTakeover(true);
        QCOMPARE(desk.live.take(), QStringList{u"take from owner-reference"_s});
        QVERIFY(desk.view.isAsking());
        desk.live.give();
        QVERIFY(desk.view.isOn());
    }

    void takeoverConfirmationIsInvalidated_data()
    {
        QTest::addColumn<int>("change");
        for (int change = 0; change != 8; ++change)
            QTest::newRow(qPrintable(QString::number(change))) << change;
    }

    void takeoverConfirmationIsInvalidated()
    {
        QFETCH(int, change);
        Desk desk;
        desk.control->setTakeoverAllowed(true);
        desk.live.reportOwner({u"First operator"_s, u"first"_s, false});
        desk.view.take();
        QVERIFY(desk.view.takeoverPending());
        switch (change) {
        case 0:
            desk.live.reportOwner({u"Second operator"_s, u"second"_s, false});
            break;
        case 1:
            desk.live.reportOwner({u"First operator"_s, u"new-reference"_s, false});
            break;
        case 2:
            desk.control->setTakeoverAllowed(false);
            break;
        case 3:
            desk.control->setAllowed(false);
            break;
        case 4:
            desk.control->setInFront(false);
            break;
        case 5:
            desk.control->have(nullptr, {});
            break;
        case 6:
            desk.control->have(&desk.live, u"other-machine"_s);
            break;
        case 7:
            desk.live.end(u"gone"_s, u"Disconnected"_s);
            break;
        default:
            QFAIL("Unexpected takeover invalidation case");
        }
        QVERIFY(!desk.view.takeoverPending());
        desk.view.answerTakeover(true);
        QVERIFY(desk.live.said.isEmpty());
        QVERIFY(!desk.view.isOn());
    }

    void localWindowOwnershipNeverOffersServerTakeover()
    {
        Desk desk;
        desk.control->setTakeoverAllowed(true);
        desk.live.reportOwner({{}, {}, true});
        QVERIFY(desk.view.ownerInAnotherWindow());
        QVERIFY(!desk.view.takeoverPossible());
        desk.control->askTakeover();
        QVERIFY(!desk.view.takeoverPending());
        QVERIFY(desk.live.said.isEmpty());
    }

    void takeoverCannotFollowAReentrantTargetOrPermissionChange_data()
    {
        QTest::addColumn<int>("change");
        QTest::newRow("different-machine-same-owner") << 0;
        QTest::newRow("administrator-revoked") << 1;
        QTest::newRow("owner-replaced") << 2;
    }

    void losingAdminWhileTakeoverWaitsRejectsALateGrant()
    {
        Desk desk;
        desk.control->setTakeoverAllowed(true);
        desk.live.reportOwner({u"Operator"_s, u"operator"_s, false});
        desk.view.take();
        desk.view.answerTakeover(true);
        QVERIFY(desk.view.isAsking());
        desk.control->setTakeoverAllowed(false);
        QVERIFY(!desk.view.isAsking());
        desk.live.give();
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.keys.swallows());
        QVERIFY(desk.live.said.contains(u"give back"_s));
        QVERIFY(desk.view.problem().contains(u"no longer take over"_s));
    }

    void takeoverCannotFollowAReentrantTargetOrPermissionChange()
    {
        QFETCH(int, change);
        Desk desk;
        desk.control->setTakeoverAllowed(true);
        desk.live.reportOwner({u"First operator"_s, u"first"_s, false});
        desk.view.take();
        QVERIFY(desk.view.takeoverPending());
        bool changed = false;
        connect(&desk.view, &ControlView::changed, &desk.view, [&] {
            if (changed || desk.view.takeoverPending())
                return;
            changed = true;
            switch (change) {
            case 0:
                desk.control->have(&desk.live, u"different-machine"_s);
                break;
            case 1:
                desk.control->setTakeoverAllowed(false);
                break;
            case 2:
                desk.live.reportOwner({u"Replacement"_s, u"second"_s, false});
                break;
            default:
                QFAIL("Unexpected reentrant takeover case");
            }
        });
        desk.view.answerTakeover(true);
        QVERIFY(changed);
        QVERIFY(desk.live.said.isEmpty());
        QVERIFY(!desk.view.isAsking());
    }

    void controlAdmissionAllowsTheServersSixtySecondDeadline()
    {
        Desk desk(MachineControl::Pace{});
        desk.view.take();
        QTest::qWait(5500);
        QVERIFY(desk.view.isAsking());
        desk.live.give();
        QVERIFY(desk.view.isOn());
        QVERIFY(MachineControl::Pace{}.answerWithinMs >= 65'000);
    }

    void cancellingTextKeepsOwnershipAndRestoresLocalCapture()
    {
        Desk desk;
        QVERIFY(desk.take());
        desk.control->pauseForText(u"a"_s);
        QVERIFY(!desk.control->isOn());
        QVERIFY(desk.live.hasControl());
        QVERIFY(!desk.keys.swallows());
        QCOMPARE(desk.live.take(), QStringList{u"release all"_s});
        desk.control->cancelText();
        QVERIFY(desk.control->isOn());
        QVERIFY(desk.keys.swallows());
        QVERIFY(desk.live.take().isEmpty());
    }

    void advertisedMouseModeChoosesTheOnlySupportedModeAndReleasesOnChange()
    {
        Desk desk;
        desk.live.mode = core::Live::Pointer::Relative;
        emit desk.live.pointerModeChanged();
        QVERIFY(desk.view.isRelative());
        QVERIFY(!desk.view.mouseSelectable());
        desk.view.setRelative(false);
        QVERIFY(desk.view.isRelative());
        QVERIFY(desk.take());
        desk.live.mode = core::Live::Pointer::Absolute;
        emit desk.live.pointerModeChanged();
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.view.isRelative());
        QCOMPARE(desk.live.take(), letGo);
        desk.live.mode = core::Live::Pointer::Both;
        emit desk.live.pointerModeChanged();
        QVERIFY(desk.view.mouseSelectable());
    }

    void textReleaseTimeoutAndLateAcknowledgementNeverSendText()
    {
        MachineControl::Pace pace;
        pace.textReleaseWithinMs = 60;
        Desk desk(Says::NoWord, pace);
        QVERIFY(desk.take());
        int sent = 0;
        int refused = 0;
        desk.control->textAfterRelease(u"a"_s, [&](const auto&) { ++sent; }, [&] { ++refused; });
        QTRY_COMPARE(refused, 1);
        QCOMPARE(sent, 0);
        desk.live.refuse({}, {});
        QCoreApplication::processEvents();
        QCOMPARE(sent, 0);
        QVERIFY(!desk.control->isAsking());
    }

    void failedTextDoesNotResumeAndFocusLossCancelsWaitingText()
    {
        Desk desk(Says::NoWord);
        QVERIFY(desk.take());
        std::function<void(bool)> finished;
        int refused = 0;
        desk.control->textAfterRelease(u"a"_s, [&](auto done) { finished = std::move(done); }, [&] { ++refused; });
        QVERIFY(!finished);
        desk.live.refuse({}, {});
        QVERIFY(finished);
        desk.live.take();
        finished(false);
        QVERIFY(desk.live.take().isEmpty());
        QVERIFY(desk.take());
        finished = {};
        desk.control->textAfterRelease(u"a"_s, [&](auto done) { finished = std::move(done); }, [&] { ++refused; });
        desk.control->setInFront(false);
        desk.live.refuse({}, {});
        QCOMPARE(refused, 1);
        QVERIFY(!finished);
    }

    void destructionFinishesInputReleaseEvenWhenTextObserverThrows()
    {
        Desk desk(Says::NoWord);
        QVERIFY(desk.take());
        int refused = 0;
        desk.control->textAfterRelease(
            u"a"_s, [](const auto&) { QFAIL("Text must not start before release acknowledgement"); },
            [&] {
                ++refused;
                throw std::bad_function_call();
            });
        QCOMPARE(desk.live.take(), letGo);
        desk.control.reset();
        QCOMPARE(refused, 1);
        QVERIFY(!desk.keys.swallows());
        QVERIFY(!desk.view.isOn());
        desk.live.refuse({}, {});
        QCoreApplication::processEvents();
        QCOMPARE(refused, 1);
    }

    void otherMachineTextNeverReleasesThisMachine()
    {
        Desk desk;
        QVERIFY(desk.take());
        int sent = 0;
        desk.control->textAfterRelease(
            u"other"_s,
            [&](const auto& done) {
                ++sent;
                done(true);
            },
            [] {});
        QCOMPARE(sent, 1);
        QVERIFY(desk.live.take().isEmpty());
        QVERIFY(desk.control->isOn());
    }

    void relativeMovesAreSummedAndFlushedBeforeClicks()
    {
        Desk desk;
        desk.view.setRelative(true);
        QVERIFY(desk.take());
        desk.view.pointerBy(4, 5);
        desk.view.button(1, true);
        QVERIFY(desk.live.take().isEmpty());
        desk.view.captureReady();
        QVERIFY(desk.view.pointerCaptured());
        desk.control->pointerAt(0.5, 0.5);
        desk.view.pointerBy(4, -5);
        QCOMPARE(desk.live.take(), QStringList{u"by 4,-5"_s});
        desk.view.pointerBy(3, 8);
        desk.view.pointerBy(-1, 2);
        QVERIFY(desk.live.take().isEmpty());
        desk.view.button(1, true);
        QCOMPARE(desk.live.take(), (QStringList{u"by 2,10"_s, u"b1+"_s}));
        desk.view.pointerBy(2, 3);
        desk.view.button(1, false);
        QCOMPARE(desk.live.take(), (QStringList{u"by 2,3"_s, u"b1-"_s}));
        desk.view.pointerBy(5, 6);
        desk.view.wheel(0, 120);
        QCOMPARE(desk.live.take(), (QStringList{u"by 5,6"_s, u"wheel 0,1"_s}));
        desk.view.pointerBy(7, 8);
        rest();
        QCOMPARE(desk.live.take(), QStringList{u"by 7,8"_s});
    }

    void relativeClicksKeepTheirPlaceBehindDelayedKeys()
    {
        Desk desk;
        desk.view.setRelative(true);
        QVERIFY(desk.take());
        desk.view.captureReady();
        desk.keys.holdsBackPresses = true;
        desk.keys.press(leftShift);
        desk.view.pointerBy(3, 4);
        desk.view.button(1, true);
        desk.view.pointerBy(7, 8);
        QVERIFY(desk.live.take().isEmpty());
        desk.keys.tellWhatWaits();
        QCOMPARE(desk.live.take(), (QStringList{u"+e1"_s, u"by 3,4"_s, u"b1+"_s, u"by 7,8"_s}));
    }

    void largeRelativeMovesReachTheTransportWithoutTruncation()
    {
        Desk desk;
        desk.view.setRelative(true);
        QVERIFY(desk.take());
        desk.view.captureReady();
        desk.view.pointerBy(10000, -5000);
        int x = 0;
        int y = 0;
        const QStringList moves = desk.live.take();
        QVERIFY(!moves.isEmpty());
        for (const QString& move : moves) {
            QVERIFY(move.startsWith(u"by "));
            const auto pair = move.sliced(3).split(u',');
            QCOMPARE(pair.size(), 2);
            const int dx = pair[0].toInt();
            const int dy = pair[1].toInt();
            QVERIFY(std::abs(dx) <= 4064 && std::abs(dy) <= 4064);
            x += dx;
            y += dy;
        }
        QCOMPARE(x, 10000);
        QCOMPARE(y, -5000);
        desk.view.pointerBy(std::numeric_limits<int>::max(), 1);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
    }

    void relativeCaptureEndsWithoutReplayingWaitingInput_data()
    {
        QTest::addColumn<int>("ending");
        for (int which = 0; which < 5; ++which) {
            QTest::newRow(qPrintable(QString::number(which))) << which;
        }
    }

    void relativeCaptureEndsWithoutReplayingWaitingInput()
    {
        QFETCH(int, ending);
        Desk desk;
        desk.view.setRelative(true);
        QVERIFY(desk.take());
        desk.view.captureReady();
        desk.view.button(1, true);
        QCOMPARE(desk.live.take(), QStringList{u"b1+"_s});
        desk.keys.holdsBackPresses = true;
        desk.keys.press(leftShift);
        desk.view.pointerBy(5, 6);
        desk.view.button(2, true);
        if (ending == 0)
            desk.view.captureLost();
        else if (ending == 1)
            desk.control->setInFront(false);
        else if (ending == 2)
            desk.control->setAllowed(false);
        else if (ending == 3)
            desk.control->have(nullptr, u"b"_s);
        else
            desk.live.end(u"lost"_s, u"Test line ended."_s);
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.view.pointerCaptured());
        desk.live.take();
        desk.keys.tellWhatWaits();
        desk.view.pointerBy(90, 90);
        rest();
        QVERIFY(desk.live.take().isEmpty());
        if (ending == 3)
            QVERIFY(!desk.view.isRelative());
    }

    void relativeMouseIsAnExplicitChoiceBeforeControl()
    {
        Desk desk;
        QVERIFY(desk.view.metaObject()->indexOfMethod("setRelative(bool)") >= 0);
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "setRelative", Q_ARG(bool, true)));
        QVERIFY(desk.view.property("relative").toBool());
        QVERIFY(desk.take());
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "setRelative", Q_ARG(bool, false)));
        QVERIFY(desk.view.property("relative").toBool());
        QCOMPARE(desk.live.askedRelative, std::optional<bool>(true));
        desk.view.giveBack();
        QVERIFY(!desk.view.isOn());
    }

    void selectedReleaseKeyControlsTheHookAndRemoteInput_data()
    {
        QTest::addColumn<QString>("choice");
        QTest::addColumn<int>("scan");
        QTest::addColumn<int>("name");
        QTest::newRow("F12") << u"f12"_s << 0x58 << 0x7B;
        QTest::newRow("Scroll Lock") << u"scroll-lock"_s << 0x46 << 0x91;
    }

    void selectedReleaseKeyControlsTheHookAndRemoteInput()
    {
        QFETCH(QString, choice);
        QFETCH(int, scan);
        QFETCH(int, name);
        DeskOfTheHook desk;
        desk.view.setReleaseKey(choice);
        QVERIFY(desk.take());
        // Right Ctrl is now a usable remote modifier, not the way out.
        desk.keys.hears(0xA3, 0x1D, marked, true);
        desk.keys.hears(0xA3, 0x1D, marked, false);
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.live.take(), (QStringList{u"+e4"_s, u"-e4"_s}));
        desk.keys.hears(static_cast<std::uint32_t>(name), static_cast<std::uint32_t>(scan), 0, true);
        QVERIFY(desk.live.take().isEmpty());
        desk.keys.hears(static_cast<std::uint32_t>(name), static_cast<std::uint32_t>(scan), 0, false);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
    }

    void anotherProgramsWindowGetsItsKeysWhileThisWindowsInterfaceIsBusy()
    {
        // (The outside review of 2026-11-05, F2.) A machine is controlled, and this window's
        // interface thread is busy: Qt's word that the window is no longer in front waits behind
        // that work, and so do the keys. Somebody clicks another program's window meanwhile and
        // types into it. Those keys are that program's as they are heard, and none reaches the
        // machine; A, held for the machine, goes up there. Once the interface thread is back,
        // keyboard and mouse are given back, with everything let go on the machine, before Qt's
        // word has come; which then changes nothing. (The keys were kept from the other program
        // until that word came, or until the interface thread was silent for a second.)
        constexpr ui::hook::Verdict passedOn{false, false};
        DeskOfTheHook desk;
        QVERIFY(desk.take());
        QCOMPARE(desk.keys.hears(0x41, 0x1E, 0, true), (ui::hook::Verdict{true, true}));
        QCOMPARE(desk.live.take(), QStringList{u"+4"_s});
        desk.keys.isHeldUp();
        inFront = anotherWindow;
        QCOMPARE(desk.keys.hears(0x43, 0x2E, 0, true), passedOn);
        QCOMPARE(desk.keys.hears(0x43, 0x2E, 0, false), passedOn);
        QCOMPARE(desk.keys.hears(0x42, 0x30, 0, true), passedOn);
        QCOMPARE(desk.keys.hears(0x42, 0x30, 0, false), passedOn);
        desk.keys.hears(0x41, 0x1E, 0, false);
        QVERIFY(desk.view.isOn());
        QVERIFY(desk.live.take().isEmpty());
        desk.keys.isBack();
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), (QStringList{u"-4"_s, u"release all"_s, u"give back"_s}));
        QVERIFY(desk.view.problem().isEmpty());
        desk.control->setInFront(false);
        QVERIFY(desk.live.take().isEmpty());
        QVERIFY(!desk.keys.swallows());
    }

    void changingReleaseKeyEndsCaptureOrItsPendingRequest_data()
    {
        QTest::addColumn<bool>("pending");
        QTest::newRow("captured") << false;
        QTest::newRow("pending") << true;
    }

    void changingReleaseKeyEndsCaptureOrItsPendingRequest()
    {
        QFETCH(bool, pending);
        Desk desk;
        if (pending) {
            desk.view.take();
        } else {
            QVERIFY(desk.take());
            desk.keys.press(leftShift);
        }
        desk.live.take();
        desk.view.setReleaseKey(u"f12"_s);
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.view.isAsking());
        QCOMPARE(desk.view.leaveKeyName(), u"F12"_s);
        QCOMPARE(desk.view.leaveKey(), std::uint16_t{0x45});
        QVERIFY(!desk.keys.swallows());
        desk.view.setReleaseKey(u"invalid"_s);
        QCOMPARE(desk.view.releaseKey(), u"f12"_s);
        desk.live.give();
        QVERIFY(!desk.view.isOn());
    }

    void takingAsksTheMachineAndWaitsForItsYes()
    {
        // (A machine that says a word for every asking and every giving back: its yes is said
        // here, and nothing is given back.)
        Desk desk(Says::EveryWord);
        QVERIFY(desk.view.isPossible());
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.leaveKeyName(), u"Right Ctrl"_s);
        const QSignalSpy changed(&desk.view, &ControlView::changed);
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        QCOMPARE(desk.live.askedRelative, std::optional<bool>(false));
        QVERIFY(desk.view.isAsking());
        QVERIFY(!desk.view.isOn());
        // Nothing is listened for, and nothing swallowed, before the machine has said yes.
        QCOMPARE(desk.keys.begun, 0);
        QVERIFY(!desk.keys.swallows());
        // Asked a second time meanwhile, it is asked once.
        desk.view.take();
        QVERIFY(desk.live.take().isEmpty());

        desk.live.give();
        QVERIFY(desk.view.isOn());
        QVERIFY(!desk.view.isAsking());
        QVERIFY(desk.control->isOn());
        QVERIFY(desk.keys.listens);
        QVERIFY(desk.keys.swallows());
        QCOMPARE(desk.keys.begun, 1);
        QVERIFY(desk.live.take().isEmpty());
        QVERIFY(changed.count() >= 2);
        // Taken already: asking again is nothing, and so is a second yes.
        desk.view.take();
        desk.live.give();
        QVERIFY(desk.live.take().isEmpty());
        QCOMPARE(desk.keys.begun, 1);
    }

    void theyCanBeTakenOnlyOfAMachineThatIsHadLiveByWhoeverMayAct()
    {
        // (A machine that says a word for every asking and every giving back: it is asked
        // once, at the end, and says nothing here.)
        NotedLive live(Says::EveryWord);
        ControlView view;
        KeysByHand keys;
        MachineControl control(view, keys);
        control.setInFront(true);
        // No machine is had live.
        control.setAllowed(true);
        QVERIFY(!view.isPossible());
        view.take();
        QVERIFY(!view.isAsking());
        // One is, and whoever sits here may not act (or the wall is only shown).
        control.setAllowed(false);
        control.have(&live, u"a"_s);
        QVERIFY(!view.isPossible());
        view.take();
        QVERIFY(live.take().isEmpty());
        control.setAllowed(true);
        QVERIFY(view.isPossible());
        // Its line is not open yet: possible from when it is.
        live.open = false;
        control.have(nullptr, u"a"_s);
        control.have(&live, u"a"_s);
        QVERIFY(!view.isPossible());
        view.take();
        QVERIFY(live.take().isEmpty());
        live.comeOpen();
        QVERIFY(view.isPossible());
        // And not from a window that does not have the keyboard.
        control.setInFront(false);
        view.take();
        QVERIFY(live.take().isEmpty());
        QVERIFY(!view.isAsking());
        control.setInFront(true);
        view.take();
        QCOMPARE(live.take(), QStringList{u"ask"_s});
    }

    void aRefusalIsSaidAndGoesWithTheNextTaking()
    {
        // (Each word of the machine is said by hand here, the one for the giving back among
        // them. What comes of a refusal with a machine that says its words by itself, and with
        // one that says none: `afterEveryEndingControlIsOfferedAgain...`.)
        Desk desk(Says::NoWord);
        desk.view.take();
        desk.live.take();
        const QString inUse = u"Keyboard and mouse of this machine are in use in another window."_s;
        desk.live.refuse(u"in_use"_s, inUse);
        QVERIFY(!desk.view.isAsking());
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(), inUse);
        QCOMPARE(desk.keys.begun, 0);
        // (The machine is told that nothing is asked of it any more.)
        QCOMPARE(desk.live.take(), letGo);
        // What it says to that is no news.
        desk.live.refuse({}, {});
        QCOMPARE(desk.view.problem(), inUse);
        QVERIFY(desk.live.take().isEmpty());
        // The next taking begins without it.
        desk.view.take();
        QCOMPARE(desk.view.problem(), QString());
        QVERIFY(desk.view.isAsking());
        // A refusal without a sentence still says something.
        desk.live.refuse(u"refused"_s, {});
        QCOMPARE(desk.view.problem(), u"Keyboard and mouse of this machine were not given."_s);
    }

    void aMachineThatDoesNotAnswerIsNotWaitedForForEver()
    {
        // (A machine that says no word by itself: its yes comes late, by hand.)
        Desk desk(Says::NoWord);
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        QTRY_VERIFY(!desk.view.isAsking());
        QCOMPARE(desk.view.problem(), u"The machine did not answer when its keyboard and mouse were asked for."_s);
        QCOMPARE(desk.live.take(), letGo);
        // Its yes comes after all: nobody waits for it, and they are given back at once. Why is
        // said: the machine answered late.
        desk.live.give();
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.keys.begun, 0);
        QCOMPARE(desk.view.problem(), answeredLate);
    }

    void givingUpWhileTheMachineIsAsked()
    {
        // (A machine that says no word by itself: its yes comes after the asking was given up,
        // by hand. One that says a word for every call says no to such an asking, and no yes.)
        Desk desk(Says::NoWord);
        desk.view.take();
        desk.view.giveBack();
        QVERIFY(!desk.view.isAsking());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.live.take(), (QStringList{u"ask"_s, u"release all"_s, u"give back"_s}));
        desk.live.give();
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        // (Whoever stopped asking themselves is told nothing of a yes that comes after that.)
        QCOMPARE(desk.view.problem(), QString());
        // Given back with nothing held or asked for: nothing.
        desk.view.giveBack();
        QVERIFY(desk.live.take().isEmpty());
    }

    void keysGoToTheMachineInTheOrderTheyCame()
    {
        Desk desk;
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        desk.keys.press(leftShift);
        desk.keys.press(keyC);
        // Held: Windows says it again and again, the machine repeats by itself.
        desk.keys.repeat(keyC);
        desk.keys.repeat(keyC);
        desk.keys.release(keyC);
        desk.keys.release(leftShift);
        // The left Ctrl is a key like any other.
        desk.keys.press(leftCtrl);
        desk.keys.release(leftCtrl);
        // A key that has no USB number is nothing.
        desk.keys.press(louder);
        desk.keys.release(louder);
        QCOMPARE(desk.live.take(),
                 (QStringList{u"+4"_s, u"-4"_s, u"+e1"_s, u"+6"_s, u"-6"_s, u"-e1"_s, u"+e0"_s, u"-e0"_s}));
        QVERIFY(desk.view.isOn());
    }

    void aKeyThatWasDownWhenTheyWereTakenIsNotTheMachines()
    {
        Desk desk;
        // A is down as they are taken (Enter on the button that takes them would be).
        desk.keys.press(keyA);
        QVERIFY(desk.take());
        // Not by its repeat, and not when it goes up.
        desk.keys.repeat(keyA);
        desk.keys.repeat(keyA);
        desk.keys.release(keyA);
        QVERIFY(desk.live.take().isEmpty());
        // Pressed anew, it is a key.
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
        // A key that was held so, and is no modifier, holds no other key back.
        Desk other;
        other.keys.press(keyA);
        QVERIFY(other.take());
        other.keys.press(keyC);
        other.keys.release(keyC);
        QCOMPARE(other.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
    }

    void underAModifierThatIsKeptBackNoOtherKeyIsPressed()
    {
        Desk desk;
        // Ctrl is down as they are taken. On the machine it is up: C sent now would be a bare
        // "c" typed into a shell.
        desk.keys.press(leftCtrl);
        QVERIFY(desk.takeWithThePointerOver());
        desk.keys.press(keyC);
        desk.keys.repeat(keyC);
        desk.keys.release(keyC);
        // Nor a button of the mouse: a click with Ctrl is another thing than a click.
        desk.control->button(1, true);
        desk.control->button(1, false);
        // Shift goes down under it, and is kept back as well.
        desk.keys.press(leftShift);
        desk.keys.press(keyC);
        QVERIFY(desk.live.take().isEmpty());
        // Ctrl is let go. Shift and C are still held from under it: neither is pressed late, by
        // its repeat, and under that Shift no key is pressed either.
        desk.keys.release(leftCtrl);
        desk.keys.repeat(keyC);
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        desk.control->button(1, true);
        desk.control->button(1, false);
        desk.keys.release(keyC);
        desk.keys.release(leftShift);
        QVERIFY(desk.live.take().isEmpty());
        // Each was let go: from here on every key is the machine's, and so are the buttons.
        desk.keys.press(leftCtrl);
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        desk.keys.release(leftCtrl);
        desk.control->button(1, true);
        desk.control->button(1, false);
        QCOMPARE(desk.live.take(), (QStringList{u"+e0"_s, u"+6"_s, u"-6"_s, u"-e0"_s, u"b1+"_s, u"b1-"_s}));
    }

    void everyUpOfAKeyThatIsDownOnTheMachineIsPassedOn()
    {
        Desk desk;
        QVERIFY(desk.take());
        // A is down on the machine. The leave-key goes down, and A is said again under it: that
        // is nothing. A goes up: it goes up on the machine, whatever the leave-key makes of it.
        desk.keys.press(keyA);
        desk.keys.press(rightCtrl);
        desk.keys.repeat(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
        // (A key that is only said again is no key going down: the leave-key was pressed and
        // let go with none between, and that gives them back. Turned round: this recorded that
        // they were still held.)
        desk.keys.release(rightCtrl);
        QVERIFY(!desk.view.isOn());
        QVERIFY(desk.take());
        // Down under Ctrl+Alt+Del's own keys, too: Del is down on the machine. It is let go
        // there first, so that the three go down in the order a hand presses them (Del, Ctrl,
        // Alt is no Ctrl+Alt+Del); the button presses and lets go of all three; and Del going
        // up here is nothing more. (Turned round: this recorded Del, Ctrl, Alt as right.)
        desk.keys.press(del);
        desk.view.ctrlAltDel();
        desk.keys.release(del);
        QCOMPARE(desk.live.take(),
                 (QStringList{u"+4c"_s, u"-4c"_s, u"+e0"_s, u"+e2"_s, u"+4c"_s, u"-4c"_s, u"-e2"_s, u"-e0"_s}));
    }

    void theLeaveKeyByItselfGivesThemBack()
    {
        Desk desk;
        QVERIFY(desk.take());
        desk.keys.press(rightCtrl);
        desk.keys.repeat(rightCtrl);
        QVERIFY(desk.view.isOn());
        QVERIFY(desk.live.take().isEmpty());
        desk.keys.release(rightCtrl);
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.control->isOn());
        QVERIFY(!desk.keys.listens);
        QVERIFY(!desk.keys.swallows());
        // The key itself never reaches the machine.
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.view.problem(), QString());
        // Held as they are taken, it gives nothing back when it is let go: pressed for it, it does.
        desk.keys.press(rightCtrl);
        QVERIFY(desk.take());
        desk.keys.repeat(rightCtrl);
        desk.keys.release(rightCtrl);
        QVERIFY(desk.view.isOn());
        desk.keys.press(rightCtrl);
        desk.keys.release(rightCtrl);
        QVERIFY(!desk.view.isOn());
    }

    void theLeaveKeyWithAnotherKeyDoesNothingAndNeitherDoesThatKey()
    {
        Desk desk;
        QVERIFY(desk.take());
        desk.keys.press(rightCtrl);
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        desk.keys.release(rightCtrl);
        QVERIFY(desk.view.isOn());
        QVERIFY(desk.live.take().isEmpty());
        // The other key let go last: the same.
        desk.keys.press(rightCtrl);
        desk.keys.press(keyC);
        desk.keys.release(rightCtrl);
        desk.keys.release(keyC);
        QVERIFY(desk.view.isOn());
        QVERIFY(desk.live.take().isEmpty());
        // After that the keys are keys again.
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
    }

    void theLeaveKeyWithDelIsCtrlAltDelInTheRightOrder()
    {
        Desk desk;
        const QStringList pressed = {u"+e0"_s, u"+e2"_s, u"+4c"_s, u"-4c"_s, u"-e2"_s, u"-e0"_s};
        // Not while they are not taken.
        desk.view.ctrlAltDel();
        QVERIFY(desk.live.take().isEmpty());
        QVERIFY(desk.take());
        desk.keys.press(rightCtrl);
        desk.keys.press(del);
        // Down: Ctrl, Alt, Del. Up: Del, Alt, Ctrl. At once, and once however long Del is held.
        QCOMPARE(desk.live.take(), pressed);
        desk.keys.repeat(del);
        desk.keys.release(del);
        desk.keys.release(rightCtrl);
        QVERIFY(desk.live.take().isEmpty());
        QVERIFY(desk.view.isOn());
        // The button does the same.
        desk.view.ctrlAltDel();
        QCOMPARE(desk.live.take(), pressed);
        // Del by itself is Del.
        desk.keys.press(del);
        desk.keys.release(del);
        QCOMPARE(desk.live.take(), (QStringList{u"+4c"_s, u"-4c"_s}));
    }

    void aKeyPressedUnderTheLeaveKeyIsNeverTypedBareAlsoNotByItsRepeat()
    {
        Desk desk;
        QVERIFY(desk.take());
        // Whoever uses the right Ctrl as a Ctrl: held with C for longer than a keyboard waits
        // before it says a key again, and the right Ctrl let go first. C is said again with the
        // leave-key up: it is the leave-key's all the same, until it was let go itself.
        desk.keys.press(rightCtrl);
        desk.keys.press(keyC);
        desk.keys.repeat(keyC);
        desk.keys.release(rightCtrl);
        desk.keys.repeat(keyC);
        desk.keys.repeat(keyC);
        QCOMPARE(desk.live.take(), QStringList());
        QVERIFY(desk.view.isOn());
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), QStringList());
        // Pressed anew, it is a key.
        desk.keys.press(keyC);
        desk.keys.repeat(keyC);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
        // And so with the Del that made Ctrl+Alt+Del: said again after the right Ctrl was let go,
        // it is no Del held on the machine.
        desk.keys.press(rightCtrl);
        desk.keys.press(del);
        QCOMPARE(desk.live.take(), (QStringList{u"+e0"_s, u"+e2"_s, u"+4c"_s, u"-4c"_s, u"-e2"_s, u"-e0"_s}));
        desk.keys.release(rightCtrl);
        desk.keys.repeat(del);
        desk.keys.repeat(del);
        desk.keys.release(del);
        QCOMPARE(desk.live.take(), QStringList());
        QVERIFY(desk.view.isOn());
        desk.keys.press(del);
        desk.keys.release(del);
        QCOMPARE(desk.live.take(), (QStringList{u"+4c"_s, u"-4c"_s}));
    }

    void aModifierThatWentDownUnderTheLeaveKeyHoldsBackWhatIsPressedUnderIt()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        // Shift goes down under the right Ctrl, which is let go first. On the machine Shift is
        // up: A under it would arrive there bare, and a click would be a click without Shift.
        desk.keys.press(rightCtrl);
        desk.keys.press(leftShift);
        desk.keys.release(rightCtrl);
        desk.keys.press(keyA);
        desk.keys.repeat(keyA);
        desk.control->button(1, true);
        desk.control->button(1, false);
        desk.keys.release(leftShift);
        // (A is still held from under it: not pressed late by its repeat either.)
        desk.keys.repeat(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), QStringList());
        QVERIFY(desk.view.isOn());
        // Each was let go: Shift and A are the machine's again.
        desk.keys.press(leftShift);
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        desk.keys.release(leftShift);
        desk.control->button(1, true);
        desk.control->button(1, false);
        QCOMPARE(desk.live.take(), (QStringList{u"+e1"_s, u"+4"_s, u"-4"_s, u"-e1"_s, u"b1+"_s, u"b1-"_s}));
    }

    void everythingIsLetGoOnTheMachineBeforeTheyAreGivenBack_data()
    {
        QTest::addColumn<int>("way");
        // The seven ways keyboard and mouse of a machine end, as its window brings them about.
        QTest::newRow("1: giving back is asked for") << 1;
        QTest::newRow("2: the leave-key") << 2;
        QTest::newRow("3: the window loses the keyboard") << 3;
        QTest::newRow("4: the window goes out of sight, and its machine is had live no more") << 4;
        QTest::newRow("5: another machine is shown large") << 5;
        QTest::newRow("6: the live ends") << 6;
        QTest::newRow("7: the live says that it holds them no longer") << 7;
        // And the ways that are this program's own.
        QTest::newRow("8: whoever sits here may act no longer, or the wall is only shown") << 8;
        QTest::newRow("9: the leave-key, heard only where the keys are heard") << 9;
        QTest::newRow("10: the key source fell behind") << 10;
        QTest::newRow("11: the keys could not be listened for at all") << 11;
        QTest::newRow("12: the window ends") << 12;
    }

    void everythingIsLetGoOnTheMachineBeforeTheyAreGivenBack()
    {
        QFETCH(int, way);
        Desk desk;
        NotedLive another;
        // The key source is still listening while the machine is told to let go: the machine
        // first, then the keyboard.
        bool listenedWhileLettingGo = false;
        bool wasToldToLetGo = false;
        desk.live.whileLettingGo = [&] {
            wasToldToLetGo = true;
            listenedWhileLettingGo = desk.keys.listens;
        };
        desk.keys.canBegin = way != 11;
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        desk.live.give();
        if (way != 11) {
            QVERIFY(desk.view.isOn());
            // A key and a button are down on the machine.
            desk.control->pointerAt(0.5, 0.5);
            desk.keys.press(keyA);
            desk.control->button(1, true);
            QCOMPARE(desk.live.take(), (QStringList{u"at 16384,16384"_s, u"+4"_s, u"b1+"_s}));
            QVERIFY(!wasToldToLetGo);
        }
        QString problem;
        switch (way) {
        case 1:
            desk.view.giveBack();
            break;
        case 2:
            desk.keys.press(rightCtrl);
            desk.keys.release(rightCtrl);
            break;
        case 3:
            desk.control->setInFront(false);
            break;
        case 4:
            desk.control->have(nullptr, u"a"_s);
            break;
        case 5:
            desk.control->have(&another, u"b"_s);
            break;
        case 6:
            problem = u"Lost contact with the KVM."_s;
            desk.live.end(u"unreachable"_s, problem);
            break;
        case 7:
            problem = u"Somebody else took keyboard and mouse."_s;
            desk.live.refuse(u"taken"_s, problem);
            break;
        case 8:
            desk.control->setAllowed(false);
            break;
        case 9:
            desk.keys.leaveByItself();
            QTRY_VERIFY(!desk.view.isOn());
            break;
        case 10:
            problem = u"Keys could not be passed on in time. Keyboard and mouse were given back, and every key on "
                      "the machine was let go."_s;
            desk.keys.fallBehind();
            break;
        case 11:
            problem = u"Windows did not let qKVM listen for keys. Keyboard and mouse were given back."_s;
            break;
        default:
            desk.control.reset();
            break;
        }
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.view.isAsking());
        QVERIFY(!desk.keys.swallows());
        // The finger is still on A, which Windows never saw go down: the key source goes on
        // listening for it to be let go, swallowing nothing. Where it fell behind, too: what
        // was held then is still held. (Turned round: this recorded the listening as ended at
        // once there, and A as forgotten.) Not where it never began, or went with the window.
        QCOMPARE(desk.keys.listens, way <= 10);
        // The machine was told to let go of everything, and then that they are given back; and
        // nothing after that.
        QCOMPARE(desk.live.take(), letGo);
        QVERIFY(wasToldToLetGo);
        QCOMPARE(listenedWhileLettingGo, way != 11);
        QCOMPARE(desk.view.problem(), problem);
        // Whatever comes now goes nowhere: a key that goes down, the key that is still held and
        // is let go (which ends the listening), the pointer, a button.
        desk.live.whileLettingGo = nullptr;
        emit desk.keys.keyChanged(keyC, true, true);
        desk.keys.release(keyA);
        QVERIFY(!desk.keys.listens);
        emit desk.keys.keyChanged(keyA, false, false);
        emit desk.keys.keyChanged(keyC, true, true);
        if (desk.control != nullptr) {
            desk.control->pointerAt(0.2, 0.2);
            desk.control->button(1, false);
            desk.control->wheel(0, 120);
            desk.control->ctrlAltDel();
        }
        rest();
        QVERIFY(desk.live.take().isEmpty());
        QVERIFY(another.take().isEmpty());
    }

    void aLiveThatIsThrownAwayTakesThemWithIt()
    {
        ControlView view;
        KeysByHand keys;
        MachineControl control(view, keys);
        control.setAllowed(true);
        control.setInFront(true);
        auto live = std::make_unique<NotedLive>();
        control.have(live.get(), u"a"_s);
        view.take();
        live->give();
        QVERIFY(view.isOn());
        live.reset();
        QVERIFY(!view.isOn());
        QVERIFY(!view.isPossible());
        QVERIFY(!keys.listens);
        QVERIFY(!keys.swallows());
    }

    void whyTheyWereTakenAwayIsKeptForItsMachine()
    {
        Desk desk;
        QVERIFY(desk.take());
        const QString why = u"The line to the KVM ended."_s;
        desk.live.refuse(u"dropped"_s, why);
        QCOMPARE(desk.view.problem(), why);
        // Taken away without a sentence, it is still said.
        QVERIFY(desk.take());
        QCOMPARE(desk.view.problem(), QString());
        desk.live.refuse(u"dropped"_s, {});
        QCOMPARE(desk.view.problem(), u"Keyboard and mouse of this machine were taken away."_s);
        // The same machine through another line: the sentence stays, until they are asked for.
        NotedLive again;
        desk.control->have(nullptr, u"a"_s);
        desk.control->have(&again, u"a"_s);
        QCOMPARE(desk.view.problem(), u"Keyboard and mouse of this machine were taken away."_s);
        // Another machine: it is nothing to that one.
        desk.control->have(&again, u"b"_s);
        QCOMPARE(desk.view.problem(), QString());
    }

    void aPlaceInTheTileIsAPlaceInThePictureOrOverABar_data()
    {
        QTest::addColumn<QPointF>("at");
        QTest::addColumn<QSizeF>("tile");
        QTest::addColumn<QSize>("picture");
        QTest::addColumn<QPointF>("in");
        const QSize wide(1920, 1080);
        // A tile of the picture's shape: the picture fills it.
        QTest::newRow("its corner") << QPointF(0, 0) << QSizeF(960, 540) << wide << QPointF(0, 0);
        QTest::newRow("its middle") << QPointF(480, 270) << QSizeF(960, 540) << wide << QPointF(0.5, 0.5);
        QTest::newRow("its far corner") << QPointF(960, 540) << QSizeF(960, 540) << wide << QPointF(1, 1);
        // A wider tile: the picture is 960 wide in the middle, with a bar of 120 on each side.
        QTest::newRow("left edge, bars beside") << QPointF(120, 0) << QSizeF(1200, 540) << wide << QPointF(0, 0);
        QTest::newRow("middle, bars beside") << QPointF(600, 135) << QSizeF(1200, 540) << wide << QPointF(0.5, 0.25);
        QTest::newRow("over the left bar") << QPointF(60, 270) << QSizeF(1200, 540) << wide << QPointF(-0.0625, 0.5);
        QTest::newRow("over the right bar") << QPointF(1140, 270) << QSizeF(1200, 540) << wide << QPointF(1.0625, 0.5);
        // A higher tile: bars of 100 above and below.
        QTest::newRow("top edge, bars above") << QPointF(0, 100) << QSizeF(960, 740) << wide << QPointF(0, 0);
        QTest::newRow("over the bar above") << QPointF(480, 46) << QSizeF(960, 740) << wide << QPointF(0.5, -0.1);
        QTest::newRow("over the bar below") << QPointF(480, 694) << QSizeF(960, 740) << wide << QPointF(0.5, 1.1);
        // A picture that is higher than wide, as a screen turned on its side is.
        QTest::newRow("a high picture") << QPointF(500, 250) << QSizeF(1000, 1000) << QSize(500, 1000)
                                        << QPointF(0.5, 0.25);
        // A picture whose size is not known is nowhere, and so is one in no tile.
        QTest::newRow("no size known") << QPointF(480, 270) << QSizeF(960, 540) << QSize() << QPointF(-1, -1);
        QTest::newRow("no tile") << QPointF(0, 0) << QSizeF(0, 0) << wide << QPointF(-1, -1);
    }

    void aPlaceInTheTileIsAPlaceInThePictureOrOverABar()
    {
        QFETCH(QPointF, at);
        QFETCH(QSizeF, tile);
        QFETCH(QSize, picture);
        QFETCH(QPointF, in);
        const QPointF place = ui::inPicture(at, tile, picture);
        QVERIFY2(qAbs(place.x() - in.x()) < 1e-9 && qAbs(place.y() - in.y()) < 1e-9,
                 qPrintable(u"%1, %2"_s.arg(place.x()).arg(place.y())));
    }

    void thePointerGoesToTheMachineByWhereItIsInThePicture()
    {
        Desk desk;
        QVERIFY(desk.take());
        // Each way 0 to 32767 over the picture.
        desk.control->pointerAt(0.0, 0.0);
        QCOMPARE(desk.live.take(), QStringList{u"at 0,0"_s});
        rest();
        desk.control->pointerAt(1.0, 1.0);
        QCOMPARE(desk.live.take(), QStringList{u"at 32767,32767"_s});
        rest();
        desk.control->pointerAt(0.25, 0.75);
        QCOMPARE(desk.live.take(), QStringList{u"at 8192,24575"_s});
        rest();
        // Beside the picture, nothing is said: not the place, and no place that waited either.
        for (const QPointF beside : {QPointF(-0.01, 0.5), QPointF(1.01, 0.5), QPointF(0.5, -0.2), QPointF(0.5, 1.2)}) {
            desk.control->pointerAt(beside.x(), beside.y());
        }
        desk.control->pointerAt(0.5, 0.5);
        desk.control->pointerAt(0.6, 0.6);
        desk.control->pointerAt(2.0, 0.6);
        rest();
        QCOMPARE(desk.live.take(), QStringList{u"at 16384,16384"_s});

        // From the screen: the place in the tile, which the view makes a place in the picture
        // of, by the size the machine said its picture has.
        desk.live.sayPicture(1920, 1080);
        desk.view.pointerAt(600, 135, 1200, 540);
        QCOMPARE(desk.live.take(), QStringList{u"at 16384,8192"_s});
        rest();
        desk.view.pointerAt(60, 270, 1200, 540);
        desk.view.pointerAt(1140, 270, 1200, 540);
        rest();
        QVERIFY(desk.live.take().isEmpty());
        // The picture is another size: the same place in the tile is another in the picture.
        desk.live.sayPicture(1200, 540);
        desk.view.pointerAt(60, 270, 1200, 540);
        QCOMPARE(desk.live.take(), QStringList{u"at 1638,16384"_s});
        // Through another live, its size is not known until that one says it.
        NotedLive again;
        desk.control->have(&again, u"a"_s);
        desk.view.take();
        again.give();
        again.take();
        desk.view.pointerAt(600, 270, 1200, 540);
        rest();
        QVERIFY(again.take().isEmpty());
    }

    void thePointerIsSaidNoMoreOftenThanSixtyTimesASecondAndTheLastPlaceWins()
    {
        // (The pace the program has: 17 ms between two places.)
        Desk desk(MachineControl::Pace{20, 17, 150});
        QVERIFY(desk.take());
        // The first place at once. Of those that follow in the same moment, the last, a moment
        // later.
        desk.control->pointerAt(0.1, 0.1);
        for (int more = 2; more <= 9; ++more) {
            desk.control->pointerAt(more / 10.0, more / 10.0);
        }
        QCOMPARE(desk.live.take(), QStringList{u"at 3277,3277"_s});
        QTRY_COMPARE(desk.live.said, QStringList{u"at 29490,29490"_s});
        rest();
        QCOMPARE(desk.live.take(), QStringList{u"at 29490,29490"_s});

        // A hand that moves all the time, for a third of a second: no more places than sixty a
        // second, and the last of them where the hand came to rest.
        QElapsedTimer clock;
        clock.start();
        int moves = 0;
        while (clock.elapsed() < 330) {
            ++moves;
            desk.control->pointerAt((moves % 100) / 100.0, 0.5);
            QTest::qWait(1);
        }
        desk.control->pointerAt(0.75, 0.25);
        const qint64 moved = clock.elapsed();
        rest();
        const QStringList said = desk.live.take();
        QVERIFY2(said.size() <= moved / 17 + 2, qPrintable(u"%1 places in %2 ms"_s.arg(said.size()).arg(moved)));
        QVERIFY2(said.size() >= 4, qPrintable(u"%1 places of %2 moves"_s.arg(said.size()).arg(moves)));
        QCOMPARE(said.constLast(), u"at 24575,8192"_s);
    }

    void theButtonsGoToTheMachineWhereThePointerIs()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        // The five, each down once however often it is said, and up once.
        for (int number = 1; number <= 5; ++number) {
            desk.control->button(number, true);
            desk.control->button(number, true);
            desk.control->button(number, false);
            desk.control->button(number, false);
        }
        QCOMPARE(desk.live.take(), (QStringList{u"b1+"_s, u"b1-"_s, u"b2+"_s, u"b2-"_s, u"b3+"_s, u"b3-"_s, u"b4+"_s,
                                                u"b4-"_s, u"b5+"_s, u"b5-"_s}));
        // No other button is one.
        for (const int number : {0, 6, -1}) {
            desk.control->button(number, true);
            desk.control->button(number, false);
        }
        QVERIFY(desk.live.take().isEmpty());
        // A click lands where the pointer is, and not where it was said to be last: the place
        // that waited is said first.
        desk.control->pointerAt(0.25, 0.25);
        desk.control->pointerAt(0.75, 0.75);
        desk.control->button(1, true);
        QCOMPARE(desk.live.take(), (QStringList{u"at 8192,8192"_s, u"at 24575,24575"_s, u"b1+"_s}));
        // Dragged to another place, and dropped there.
        desk.control->pointerAt(0.5, 0.5);
        desk.control->button(1, false);
        QCOMPARE(desk.live.take(), (QStringList{u"at 16384,16384"_s, u"b1-"_s}));
        rest();
        // Beside the picture nothing is pressed. What went down over the picture goes up
        // wherever the pointer is by then.
        desk.control->button(2, true);
        desk.control->pointerAt(1.5, 0.5);
        desk.control->button(1, true);
        desk.control->button(1, false);
        desk.control->button(2, false);
        QCOMPARE(desk.live.take(), (QStringList{u"b2+"_s, u"b2-"_s}));

        // From the screen, by Qt's names for the buttons.
        desk.live.sayPicture(1920, 1080);
        desk.view.pointerAt(480, 270, 960, 540);
        rest();
        desk.live.take();
        for (const Qt::MouseButton qtButton :
             {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton, Qt::BackButton, Qt::ForwardButton, Qt::TaskButton}) {
            desk.view.button(qtButton, true);
            desk.view.button(qtButton, false);
        }
        QCOMPARE(desk.live.take(), (QStringList{u"b1+"_s, u"b1-"_s, u"b2+"_s, u"b2-"_s, u"b3+"_s, u"b3-"_s, u"b4+"_s,
                                                u"b4-"_s, u"b5+"_s, u"b5-"_s}));
    }

    void theWheelGoesToTheMachineANotchAtATime()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        // Away from the hand is positive, here as in Windows; towards it, negative.
        desk.control->wheel(0, 120);
        QCOMPARE(desk.live.take(), QStringList{u"wheel 0,1"_s});
        desk.control->wheel(0, -120);
        QCOMPARE(desk.live.take(), QStringList{u"wheel 0,-1"_s});
        // Three notches in one turn: three messages.
        desk.control->wheel(0, 360);
        QCOMPARE(desk.live.take(), (QStringList{u"wheel 0,1"_s, u"wheel 0,1"_s, u"wheel 0,1"_s}));
        // A wheel that says less than a notch at a time: a message when a notch is full.
        desk.control->wheel(0, -50);
        desk.control->wheel(0, -50);
        QVERIFY(desk.live.take().isEmpty());
        desk.control->wheel(0, -50);
        QCOMPARE(desk.live.take(), QStringList{u"wheel 0,-1"_s});
        desk.control->wheel(0, -90);
        QCOMPARE(desk.live.take(), QStringList{u"wheel 0,-1"_s});
        // Sideways: what Qt says as a turn to the left is, as Windows has it, to the right's other
        // side. To the right is positive for the machine.
        desk.control->wheel(-120, 0);
        QCOMPARE(desk.live.take(), QStringList{u"wheel 1,0"_s});
        desk.control->wheel(240, 0);
        QCOMPARE(desk.live.take(), (QStringList{u"wheel -1,0"_s, u"wheel -1,0"_s}));
        // A number from nowhere is not a thousand messages.
        desk.control->wheel(0, 120 * 100'000);
        QCOMPARE(desk.live.take().size(), 16);
        // Where the pointer is said first, where that waited.
        desk.control->pointerAt(0.25, 0.25);
        desk.control->pointerAt(0.75, 0.75);
        desk.control->wheel(0, 120);
        QCOMPARE(desk.live.take(), (QStringList{u"at 8192,8192"_s, u"at 24575,24575"_s, u"wheel 0,1"_s}));
        // Beside the picture, nothing: and what was turned there is not kept for later.
        desk.control->pointerAt(-0.5, 0.5);
        desk.control->wheel(0, 100);
        desk.control->wheel(0, 240);
        desk.control->pointerAt(0.5, 0.5);
        desk.control->wheel(0, 30);
        rest();
        QCOMPARE(desk.live.take(), QStringList{u"at 16384,16384"_s});
        // From the screen, as Qt says a turn.
        desk.view.wheel(0, 120);
        QCOMPARE(desk.live.take(), QStringList{u"wheel 0,1"_s});
    }

    void theKeySourceIsToldThatTheProgramIsAliveWhileTheyAreTaken()
    {
        Desk desk;
        QTest::qWait(80);
        QCOMPARE(desk.keys.saidAlive, 0);
        QVERIFY(desk.take());
        // At once, before a key is swallowed; and then every so often.
        QVERIFY(desk.keys.saidAlive >= 1);
        const int atFirst = desk.keys.saidAlive;
        QTRY_VERIFY(desk.keys.saidAlive >= atFirst + 5);
        desk.view.giveBack();
        const int atTheEnd = desk.keys.saidAlive;
        QTest::qWait(100);
        QCOMPARE(desk.keys.saidAlive, atTheEnd);
    }

    void whereTheKeysAreHeardTheLeaveKeyIsDecidedAndThisFollows()
    {
        // The leave-key gave the keyboard back where the keys are heard. With the next key that
        // is heard of, the machine's is given back here.
        // (The stand-in tells, as the real one does, only of a key it kept from Windows once it
        // swallows no more: the next key heard of here is A going up, and not, as this had it,
        // the going up of a key that never went down.)
        Desk desk;
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        QCOMPARE(desk.live.take(), QStringList{u"+4"_s});
        desk.keys.leaveByItself();
        desk.keys.release(keyA);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), (QStringList{u"-4"_s, u"release all"_s, u"give back"_s}));
        QVERIFY(!desk.keys.listens);
    }

    void nothingGoesToAMachineWhileItsKeyboardAndMouseAreNotTaken()
    {
        // (A machine that is given long to answer: it is still being asked at the end.)
        Desk desk(MachineControl::Pace{20, 40, 30'000});
        // Not taken, and only asked for: neither is taken.
        for (const bool asked : {false, true}) {
            if (asked) {
                desk.view.take();
                QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
            }
            emit desk.keys.keyChanged(keyA, true, true);
            emit desk.keys.keyChanged(keyA, false, true);
            emit desk.keys.keyChanged(rightCtrl, true, true);
            emit desk.keys.keyChanged(rightCtrl, false, true);
            desk.view.pointerAt(10, 10, 100, 100);
            desk.control->pointerAt(0.5, 0.5);
            desk.view.button(Qt::LeftButton, true);
            desk.view.button(Qt::LeftButton, false);
            desk.view.wheel(0, 120);
            desk.view.ctrlAltDel();
            rest();
            QVERIFY(desk.live.take().isEmpty());
            QCOMPARE(desk.view.isAsking(), asked);
        }
    }

    // --- what a reviewer found ---

    void aKeyThatWentDownWhileNothingCouldBeSwallowedIsNotTheMachines()
    {
        Desk desk;
        QVERIFY(desk.take());
        // A press that could not be kept from Windows, and is told as Windows' own: Ctrl goes
        // down, passes to Windows, and is this PC's. C under it would be a bare "c" on the
        // machine.
        desk.keys.stalled = true;
        desk.keys.press(leftCtrl);
        desk.keys.stalled = false;
        desk.keys.press(keyC);
        desk.keys.repeat(keyC);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), QStringList());
        // Let go, and pressed for the machine: Ctrl+C.
        desk.keys.release(leftCtrl);
        desk.keys.press(leftCtrl);
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        desk.keys.release(leftCtrl);
        QCOMPARE(desk.live.take(), (QStringList{u"+e0"_s, u"+6"_s, u"-6"_s, u"-e0"_s}));
        // A key that is down on the machine, and is said again while nothing can be swallowed,
        // stays the machine's: under it a key is pressed, and it goes up there when let go.
        desk.keys.press(leftShift);
        desk.keys.stalled = true;
        desk.keys.repeat(leftShift);
        desk.keys.stalled = false;
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        desk.keys.release(leftShift);
        QCOMPARE(desk.live.take(), (QStringList{u"+e1"_s, u"+4"_s, u"-4"_s, u"-e1"_s}));
        QVERIFY(desk.view.isOn());
    }

    void aKeyThatWasKeptFromWindowsIsStillKnownWhenTheyAreTakenAgain()
    {
        Desk desk;
        QVERIFY(desk.take());
        desk.keys.press(leftCtrl);
        QCOMPARE(desk.live.take(), QStringList{u"+e0"_s});
        // Given back with Ctrl held. Windows never saw it go down, and so does not know.
        desk.view.giveBack();
        QCOMPARE(desk.live.take(), letGo);
        QVERIFY(!desk.keys.swallows());
        QVERIFY(desk.keys.windowsHas.empty());
        // Taken again with the finger still on it: on the machine it is up, and C is no "c".
        QVERIFY(desk.take());
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        QVERIFY(desk.live.take().isEmpty());
        desk.keys.release(leftCtrl);
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
    }

    void theKeySourceListensUntilWhatItKeptFromWindowsWasLetGo()
    {
        // (It gives a key 200 ms to be let go.)
        Desk desk(MachineControl::Pace{20, 40, 150, 200});
        QVERIFY(desk.take());
        desk.keys.press(leftCtrl);
        desk.keys.press(keyA);
        desk.view.giveBack();
        QVERIFY(desk.keys.listens);
        QVERIFY(!desk.keys.swallows());
        desk.live.take();
        // What is pressed meanwhile is this PC's, and is nothing to the machine.
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        desk.keys.release(keyA);
        QVERIFY(desk.keys.listens);
        desk.keys.release(leftCtrl);
        QVERIFY(!desk.keys.listens);
        QCOMPARE(desk.keys.ended, 1);
        QVERIFY(desk.live.take().isEmpty());
        // Where nothing was kept from Windows that is still held, it ends at once.
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        desk.view.giveBack();
        QVERIFY(!desk.keys.listens);
        QCOMPARE(desk.keys.ended, 2);
        // And a key whose "up" never comes is not waited for for ever.
        QVERIFY(desk.take());
        desk.keys.press(leftShift);
        desk.view.giveBack();
        QVERIFY(desk.keys.listens);
        QTRY_VERIFY(!desk.keys.listens);
        QCOMPARE(desk.keys.ended, 3);
    }

    void noKeyGoesDownUnknownInTheMomentTheyAreTaken()
    {
        Desk desk;
        // Ctrl goes down in the very moment in which it is asked what is down.
        bool pressed = false;
        desk.keys.whileAskedWhatIsDown = [&] {
            if (!pressed) {
                pressed = true;
                desk.keys.press(leftCtrl);
            }
        };
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        desk.live.give();
        QVERIFY(desk.view.isOn());
        QVERIFY(pressed);
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        desk.keys.release(leftCtrl);
        // They were taken by then: Ctrl is the machine's, and went down there before C. Never
        // a bare C.
        QCOMPARE(desk.live.take(), (QStringList{u"+e0"_s, u"+6"_s, u"-6"_s, u"-e0"_s}));
    }

    void whenNothingIsHeardAnyMoreItIsSaidWhy_data()
    {
        QTest::addColumn<ui::KeySource::Lost>("why");
        QTest::addColumn<QString>("sentence");
        // (Taken out of here, with what it was the word for: "more keyboards held keys than
        // can be kept apart". Keyboards are not kept apart any more, and nothing ends by how
        // many there are.)
        QTest::newRow("Windows shows another desktop, or the session is locked")
            << ui::KeySource::Lost::AnotherDesktop
            << u"Windows showed a screen of its own (the lock screen, Ctrl+Alt+Del, or a question about "
               "administrator rights). Keyboard and mouse were given back, and every key on the machine was let go."_s;
        // Windows' two accounts of the keyboard differed. Said as what was seen, which of the
        // two had a key that the other did not, with what may be why and what can be done: a
        // key remapper or one of Windows' own key helpers makes them differ where nothing is
        // wrong with Windows. (This had one sentence for it, "Windows no longer hands QKVM
        // Desktop its keys", which was not what had happened on such a PC.)
        QTest::newRow("Windows saw a key that was not heard here")
            << ui::KeySource::Lost::NotHandedKeys
            << u"Windows saw a key on the keyboard that qKVM was not handed. Windows may have stopped "
               "handing it keys, or a key remapper, a launcher or a Windows key helper (Sticky, Filter or Mouse "
               "Keys) held the key back. Keyboard and mouse were given back, and every key on the machine was let "
               "go. If it happens again, close that program or switch the helper off."_s;
        // (Turned round: this had "a key that Windows did not see on the keyboard", and a
        // program that changes keys as the one thing that may be why. What is known is that
        // Windows' other account of the keyboard did not tell of the key: that account may also
        // be told to this program no more, which looks the same from here. Both are said.)
        QTest::newRow("a key was heard here that Windows' other account did not tell of")
            << ui::KeySource::Lost::NotToldOfKeys
            << u"qKVM was handed a key that Windows did not report from the keyboard. Windows may no "
               "longer be reporting the keyboard to qKVM, or a key remapper, a macro tool or a Windows key "
               "helper (Sticky, Filter or Mouse Keys) made or changed the key. Keyboard and mouse were given back, "
               "and every key on the machine was let go. If it happens again, close such a program or switch the "
               "helper off."_s;
        QTest::newRow("Windows saw one key and another was heard here")
            << ui::KeySource::Lost::ToldDifferently
            << u"Windows saw one key on the keyboard, and qKVM was handed another. A remapped key, a "
               "program that changes keys or a Windows key helper (Sticky, Filter or Mouse Keys) may be why. "
               "Keyboard and mouse were given back, and every key on the machine was let go. If it happens again, "
               "close that program or switch the helper off."_s;
        // Said as what happened: the keys were pressed for the machine, and Windows acted on
        // them. (This had the sentence of a screen that Windows showed by itself.)
        QTest::newRow("the keys were pressed that Windows itself acts on")
            << ui::KeySource::Lost::KeysOfWindows
            << u"Windows itself acts on the keys that were pressed, whoever they were meant for: the Windows key "
               "with L locks this PC, and Ctrl+Alt+Del brings Windows' own screen. Keyboard and mouse were given "
               "back, and every key on the machine was let go."_s;
        QTest::newRow("whatever listens for the keys no longer looks")
            << ui::KeySource::Lost::NotLooking
            << u"qKVM stopped looking at the keys of this PC. Keyboard and mouse were given back, and every "
               "key on the machine was let go."_s;
        // Said as what Windows said, and what that is to the machine: which keyboard it was,
        // and whether a key was held on it, is known to nobody here.
        QTest::newRow("a keyboard was taken away from this PC")
            << ui::KeySource::Lost::AKeyboardWent
            << u"Windows reported that a keyboard was taken away from this PC: unplugged, or switched to another "
               "computer (it counts some mice, headsets and docks as keyboards). A key that was held on it would "
               "have stayed down on the machine, so keyboard and mouse were given back, and every key on the "
               "machine was let go."_s;
        // True whichever it was: a key that was heard before any hearing was noted, or one that
        // was not handed over at all. And with what to do about it, said as it can be done at
        // this moment (the machine has still to say its word for the giving back).
        QTest::newRow("Windows' other account began to tell, and a key of its first moment found no hearing")
            << ui::KeySource::Lost::BeganToTell
            << u"Windows began to report this PC's keyboard to qKVM only after keyboard and mouse were "
               "taken (a keyboard may have been plugged in), and a key from that first moment could not be "
               "checked. Keyboard and mouse were given back, and every key on the machine was let go."_s +
                   askAgainLater;
    }

    void whenNothingIsHeardAnyMoreItIsSaidWhy()
    {
        QFETCH(ui::KeySource::Lost, why);
        QFETCH(QString, sentence);
        // (What is held is remembered for 200 ms once no machine is controlled. In the program
        // it is a minute.)
        Desk desk(MachineControl::Pace{20, 40, 150, 200});
        // (While nothing is taken it is nothing.)
        emit desk.keys.lostHearing(why);
        QCOMPARE(desk.view.problem(), QString());
        QVERIFY(desk.take());
        desk.keys.press(leftCtrl);
        desk.keys.loseHearing(why);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), (QStringList{u"+e0"_s, u"release all"_s, u"give back"_s}));
        QCOMPARE(desk.view.problem(), sentence);
        QVERIFY(!desk.keys.swallows());
        // Ctrl is still held, and that is not forgotten: its going up is listened for, by
        // whatever of Windows still tells of it. (Turned round: this recorded the listening as
        // ended at once, with nothing waited for.) Not for ever: for as long as a finger is
        // given once no machine is controlled. (Turned round a second time: this had a time of
        // its own, a few seconds, after which a key that might have been let go unheard was
        // taken to be up, also while a machine was controlled again. That time is gone.)
        QVERIFY(desk.keys.listens);
        QCOMPARE(desk.keys.ended, 0);
        QTRY_VERIFY(!desk.keys.listens);
        QCOMPARE(desk.keys.ended, 1);
    }

    void aKeyThatMayHaveBeenLetGoUnheardIsKeptBackUntilItIsSeenToGoDownAnew()
    {
        // (Turned round: "a key that may have been let go unheard holds the keyboard back only
        // for a while". After that while, a few seconds, a Ctrl that was still held was taken to
        // be up, and C went to the machine bare, with control on.)
        // (A held key that is said again is silent for no longer than 150 ms here. In the
        // program it is three seconds.)
        Desk desk(MachineControl::Pace{20, 40, 150, 60'000, 150});
        // On Windows' own screen: what is let go there is heard by nobody.
        desk.keys.hearsWhatGoesUp = false;
        QVERIFY(desk.take());
        desk.keys.press(leftCtrl);
        desk.keys.press(keyA);
        desk.keys.loseHearing(ui::KeySource::Lost::AnotherDesktop);
        desk.live.take();
        desk.keys.release(keyA);
        desk.keys.release(leftCtrl);
        QVERIFY(desk.keys.listens);
        // Taken again. For all that is known both are down: under that Ctrl, C is kept back,
        // and so is a click.
        QVERIFY(desk.takeWithThePointerOver());
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        desk.control->button(1, true);
        desk.control->button(1, false);
        QCOMPARE(desk.live.take(), QStringList());
        // However long that is: a key is not taken to be up for having been silent. A finger
        // may be on it, and C would arrive on the machine bare.
        QTest::qWait(400);
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), QStringList());
        // Nothing is eaten without a word: the bar says which key, and what ends it.
        QCOMPARE(desk.view.note(), heldFromBefore().arg(u"Left Ctrl"_s));
        // Ctrl goes down. A key that is held and said again is never silent for this long: the
        // finger had left it, and this is a press, for the machine. Nothing is held back any more.
        desk.keys.press(leftCtrl);
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        desk.keys.release(leftCtrl);
        QCOMPARE(desk.live.take(), (QStringList{u"+e0"_s, u"+6"_s, u"-6"_s, u"-e0"_s}));
        QCOMPARE(desk.view.note(), QString());
        // A, which is no modifier, held nothing back, and is a key when it is pressed anew.
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
        QVERIFY(desk.view.isOn());

        // Pressed so soon after they were taken that it cannot be told from a held key that is
        // said again: kept back as a key that is down, this once, until it is let go.
        Desk soon(MachineControl::Pace{20, 40, 150, 60'000, 30'000});
        soon.keys.hearsWhatGoesUp = false;
        QVERIFY(soon.take());
        soon.keys.press(leftCtrl);
        soon.keys.loseHearing(ui::KeySource::Lost::AnotherDesktop);
        soon.keys.release(leftCtrl);
        soon.live.take();
        QVERIFY(soon.take());
        soon.keys.press(leftCtrl);
        soon.keys.press(keyC);
        soon.keys.release(keyC);
        QCOMPARE(soon.live.take(), QStringList());
        QVERIFY(!soon.view.note().isEmpty());
        soon.keys.release(leftCtrl);
        QCOMPARE(soon.view.note(), QString());
        soon.keys.press(leftCtrl);
        soon.keys.press(keyC);
        soon.keys.release(keyC);
        soon.keys.release(leftCtrl);
        QCOMPARE(soon.live.take(), (QStringList{u"+e0"_s, u"+6"_s, u"-6"_s, u"-e0"_s}));

        // A Ctrl that was held all the time, and is said again and again for longer than a held
        // key is ever silent: never taken for one that was pressed anew, and never up.
        Desk held(MachineControl::Pace{20, 40, 150, 60'000, 400});
        QVERIFY(held.take());
        held.keys.press(leftCtrl);
        held.keys.loseHearing(ui::KeySource::Lost::NotHandedKeys);
        held.live.take();
        QVERIFY(held.take());
        for (int said = 0; said < 40; ++said) {
            QTest::qWait(25);
            held.keys.repeat(leftCtrl);
        }
        held.keys.press(keyC);
        held.keys.release(keyC);
        QCOMPARE(held.live.take(), QStringList());
        QVERIFY(!held.view.note().isEmpty());
        held.keys.release(leftCtrl);
        held.keys.press(keyC);
        held.keys.release(keyC);
        QCOMPARE(held.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
        QCOMPARE(held.view.note(), QString());

        // (Taken out: "where they are not taken again, the listening ends with that time". There
        // is no such time any more; the listening ends when the key was let go, or with the
        // time a finger is given once no machine is controlled, which has its test above.)
        // And where the key's going up is still heard (by Windows' other account of the keyboard,
        // say), it ends with that.
        Desk fourth(MachineControl::Pace{20, 40, 150, 60'000, 60'000});
        QVERIFY(fourth.take());
        fourth.keys.press(leftCtrl);
        fourth.keys.loseHearing(ui::KeySource::Lost::NotHandedKeys);
        QVERIFY(fourth.keys.listens);
        fourth.keys.release(leftCtrl);
        QVERIFY(!fourth.keys.listens);
        fourth.live.take();
        QVERIFY(fourth.take());
        QCOMPARE(fourth.view.note(), QString());
        fourth.keys.press(keyC);
        fourth.keys.release(keyC);
        QCOMPARE(fourth.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
    }

    void aKeyThatWasKeptFromWindowsAndToldToNobodyIsKnownAtTheNextTaking()
    {
        Desk desk;
        QVERIFY(desk.take());
        // Shift went down and was kept from Windows in the very moment in which the keys were
        // heard no more: it was told to nobody as a press. It is down all the same, and
        // Windows does not know.
        desk.keys.held.push_back(leftShift);
        desk.keys.kept.push_back(leftShift);
        desk.keys.loseHearing(ui::KeySource::Lost::NotHandedKeys);
        QCOMPARE(desk.live.take(), letGo);
        // Taken again with the finger still on it: A is no bare "a".
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.release(leftShift);
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
        // And so where giving back was asked for in that moment.
        desk.keys.held.push_back(leftShift);
        desk.keys.kept.push_back(leftShift);
        desk.view.giveBack();
        QVERIFY(desk.keys.listens);
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.release(leftShift);
        desk.view.giveBack();
        QVERIFY(!desk.keys.listens);
        // And a press that was kept from Windows in that moment and is told only after it (it
        // waited to be handed on): known as well, by the key source's own word that it kept it.
        QVERIFY(desk.take());
        desk.keys.press(leftCtrl);
        desk.view.giveBack();
        QVERIFY(desk.keys.listens);
        desk.keys.held.push_back(leftShift);
        desk.keys.kept.push_back(leftShift);
        emit desk.keys.keyChanged(leftShift, true, true);
        desk.keys.release(leftCtrl);
        QVERIFY(desk.keys.listens);
        desk.live.take();
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.release(leftShift);
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
    }

    void whatIsHeldIsOneMemoryForAllTheWindows()
    {
        // Two windows of one program: each has its control and its keys, and the two have one
        // memory of what is held. (It forgets after 300 ms once no machine is controlled.)
        ui::KeysKept kept(300);
        const MachineControl::Pace pace{20, 40, 150};
        Desk first(pace, &kept);
        Desk second(pace, &kept);
        QVERIFY(first.take());
        first.keys.press(leftCtrl);
        QCOMPARE(first.live.take(), QStringList{u"+e0"_s});
        // A click into the second window: the first has the keyboard no longer. Ctrl is still
        // held, and Windows never saw it go down.
        first.control->setInFront(false);
        QCOMPARE(first.live.take(), letGo);
        QVERIFY(first.keys.listens);
        // "Control" there. That window's keys never heard Ctrl go down: C is no "c" all the same.
        QVERIFY(second.take());
        second.keys.press(keyC);
        second.keys.release(keyC);
        QCOMPARE(second.live.take(), QStringList());
        QCOMPARE(second.view.note(), heldFromBefore().arg(u"Left Ctrl"_s));
        // While a machine is controlled, in whichever window, nothing is forgotten for the time
        // it took.
        QTest::qWait(450);
        second.keys.press(keyC);
        second.keys.release(keyC);
        QCOMPARE(second.live.take(), QStringList());
        // Ctrl is let go. The second window's keys hear that, and keep it from the first's: it
        // is known to both. The first does not go on waiting for it.
        emit second.keys.keyChanged(leftCtrl, false, true);
        first.keys.release(leftCtrl);
        QVERIFY(!first.keys.listens);
        QCOMPARE(second.view.note(), QString());
        second.keys.press(keyC);
        second.keys.release(keyC);
        QCOMPARE(second.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
        // And back in the first window Ctrl is not kept back for ever.
        second.control->setInFront(false);
        QVERIFY(first.control != nullptr);
        first.control->setInFront(true);
        QVERIFY(first.take());
        QCOMPARE(first.view.note(), QString());
        first.keys.press(keyC);
        first.keys.release(keyC);
        QCOMPARE(first.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
        // A window that goes while a key of its keeping is held: the key is still known to the
        // other one.
        first.keys.press(leftShift);
        first.live.take();
        first.control.reset();
        second.control->setInFront(true);
        QVERIFY(second.take());
        second.keys.press(keyA);
        second.keys.release(keyA);
        QCOMPARE(second.live.take(), QStringList());
        emit second.keys.keyChanged(leftShift, false, true);
        second.keys.press(keyA);
        second.keys.release(keyA);
        QCOMPARE(second.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
    }

    void whatIsHeldIsForgottenOnlyOnceNoWindowControlsAMachine()
    {
        // (Forgotten after 150 ms here. In the program it is a minute.)
        ui::KeysKept kept(150);
        const QSignalSpy changed(&kept, &ui::KeysKept::changed);
        QVERIFY(kept.isEmpty());
        // While a window controls a machine a key is kept, however long that is.
        kept.controlBegan();
        kept.keep(0xE0);
        kept.keep(0xE0);
        kept.keep(0x04);
        kept.keep(0);
        QCOMPARE(changed.count(), 2);
        QTest::qWait(400);
        QVERIFY(kept.isIn(0xE0) && kept.isIn(0x04) && !kept.isIn(0));
        // Heard to go up: gone, and said once.
        kept.letGo(0x04);
        kept.letGo(0x04);
        QCOMPARE(changed.count(), 3);
        QVERIFY(!kept.isIn(0x04));
        // A second window controls a machine as well, and the first gives back: one still does.
        kept.controlBegan();
        kept.controlEnded();
        QTest::qWait(400);
        QVERIFY(kept.isIn(0xE0));
        // The last one gives back: forgotten after the time a finger is given, and said.
        kept.controlEnded();
        QVERIFY(kept.isIn(0xE0));
        QTRY_VERIFY(kept.isEmpty());
        QCOMPARE(changed.count(), 4);
        // And so a key that is kept while nothing is controlled (it was kept from Windows in
        // the very moment of the giving back).
        kept.keep(0xE1);
        QVERIFY(kept.isIn(0xE1));
        QTRY_VERIFY(kept.isEmpty());
    }

    void whereThisPcGivesNoSecondAccountOfItsKeyboardTheBarSaysSo()
    {
        const QString notNoticed = u"If Windows stops passing keys to qKVM, that is not noticed on this PC: "
                                   "click Control to give keyboard and mouse back."_s;
        Desk desk;
        // Raw Input was refused, or none comes (a remote desktop): the keys are heard, and a
        // listening that Windows took away would be noticed by nobody.
        desk.keys.watched = false;
        QVERIFY(desk.view.isWatched());
        QVERIFY(desk.take());
        QVERIFY(!desk.view.isWatched());
        QCOMPARE(desk.view.note(), notNoticed);
        // Said while they are taken, and not after.
        desk.view.giveBack();
        QCOMPARE(desk.view.note(), QString());
        QVERIFY(desk.view.isWatched());
        // Found only once keys come (the account was asked for, and tells of none): said then.
        desk.keys.watched = true;
        QVERIFY(desk.take());
        QCOMPARE(desk.view.note(), QString());
        desk.keys.watched = false;
        QTRY_COMPARE(desk.view.note(), notNoticed);
        desk.keys.watched = true;
        QTRY_COMPARE(desk.view.note(), QString());
        QVERIFY(desk.view.isOn());
    }

    void theWindowsKeyIsSaidToTheMachineOnlyWhenItIsNoWinL()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        // The Windows key goes down: nothing yet. With L, Windows locks this PC whoever the keys
        // were meant for, and the machine would have been tapped on its Windows key.
        desk.keys.press(leftWindows);
        desk.keys.repeat(leftWindows);
        QCOMPARE(desk.live.take(), QStringList());
        // Another key that is not L goes down: it is no Win+L. Both, in their order.
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        desk.keys.release(leftWindows);
        QCOMPARE(desk.live.take(), (QStringList{u"+e3"_s, u"+4"_s, u"-4"_s, u"-e3"_s}));
        // The other order: the Windows key goes up by itself. Down and up together.
        desk.keys.press(rightWindows);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.release(rightWindows);
        QCOMPARE(desk.live.take(), (QStringList{u"+e7"_s, u"-e7"_s}));
        // A click under it is a click with the Windows key.
        desk.keys.press(leftWindows);
        desk.control->button(1, true);
        desk.control->button(1, false);
        desk.keys.release(leftWindows);
        QCOMPARE(desk.live.take(), (QStringList{u"+e3"_s, u"b1+"_s, u"b1-"_s, u"-e3"_s}));
        // A key that goes up meanwhile says nothing of it: the Windows key still waits.
        desk.keys.press(keyC);
        desk.keys.press(leftWindows);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
        desk.keys.release(leftWindows);
        QCOMPARE(desk.live.take(), (QStringList{u"+e3"_s, u"-e3"_s}));

        // Win+L. Where the keys are heard it is known before L goes anywhere, and is said as
        // what it is. Nothing of the two reached the machine: no Windows key was tapped there.
        desk.keys.press(leftWindows);
        desk.keys.loseHearing(ui::KeySource::Lost::KeysOfWindows);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        desk.keys.release(leftWindows);

        // And were L to come all the same: neither of the two goes to the machine.
        QVERIFY(desk.take());
        desk.keys.press(rightWindows);
        desk.keys.press(keyL);
        desk.keys.release(keyL);
        desk.keys.release(rightWindows);
        QCOMPARE(desk.live.take(), QStringList());
        // L by itself is a key.
        desk.keys.press(keyL);
        desk.keys.release(keyL);
        QCOMPARE(desk.live.take(), (QStringList{u"+f"_s, u"-f"_s}));
    }

    void oneAskingAtATime()
    {
        // What it shows, since no asking is made while the machine owes a word: that the
        // machine is never asked a second time while it has still to answer the first, or to say
        // its word for a giving back; that the screen is told so for exactly as long; and that
        // the first asking after that is one asking with one answer. (It recorded that Control
        // was busy only until the first word, or until the asking's time was over, and offered
        // again while words were owed: a yes that came then could not be told from the owed one.)
        // (Each word of the machine is said by hand, at the moment it is to come: the yes to an
        // asking that was given up among them, which a machine that calls such an asking off
        // does not say.)
        Desk desk(Says::NoWord);
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        // A key was pressed in the window before the machine answered: the asking is given up,
        // and the machine owes its answer to it, and its word for the giving back.
        desk.view.wentOn(true);
        QCOMPARE(desk.live.take(), letGo);
        // "Control" is clicked again before those have come. The machine is not asked a second
        // time: the yes to the first would be taken for the yes to the second, its word for the
        // giving back for "taken away", and the second yes thrown away without a word.
        desk.view.take();
        QVERIFY(!desk.view.isAsking());
        QCOMPARE(desk.live.take(), QStringList());
        // (The button that asks does nothing meanwhile, and the screen is told so.)
        QVERIFY(desk.view.isBusy());
        // The yes to the first comes: given back at once, as a yes that nobody waits for is.
        // The machine's words for the two givings back are owed still: not asked meanwhile.
        desk.live.give();
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        QVERIFY(desk.view.isBusy());
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList());
        desk.live.refuse({}, {});
        QVERIFY(desk.view.isBusy());
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isBusy());
        QVERIFY(!desk.view.isOn());
        // Asked anew from here: one asking, one yes, and they are taken.
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        QVERIFY(desk.view.isAsking());
        desk.live.give();
        QVERIFY(desk.view.isOn());
        // Given back: busy until the machine has said its word for that, and no longer.
        desk.view.giveBack();
        QVERIFY(desk.view.isBusy());
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isBusy());
        desk.live.take();

        // An asking that the machine has answered, with a no, owes no answer: only its word for
        // the giving back that followed. (Turned round: this recorded that Control was offered
        // at once after the no, with that word still to come.)
        desk.view.take();
        desk.live.refuse(u"in_use"_s, u"In use."_s);
        QVERIFY(desk.view.isBusy());
        desk.live.take();
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList());
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isBusy());
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        // One that was stopped by its button owes both: busy until the machine has said two
        // words, of which the first is taken for its answer to the asking.
        desk.view.giveBack();
        QVERIFY(desk.view.isBusy());
        desk.live.take();
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList());
        desk.live.refuse({}, {});
        QVERIFY(desk.view.isBusy());
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isBusy());
        // And with another line to the machine nothing of the one before is owed.
        desk.view.take();
        desk.view.wentOn(false);
        QVERIFY(desk.view.isBusy());
        NotedLive another;
        desk.control->have(&another, u"a"_s);
        QVERIFY(!desk.view.isBusy());
        desk.view.take();
        QCOMPARE(another.take(), QStringList{u"ask"_s});
    }

    void keyboardAndMouseThatAreLeftAreGivenBack()
    {
        // (Left for 400 ms, they are given back. In the program it is fifteen minutes, as the web's
        // IDLE_GIVE_BACK: one constant, here.)
        Desk desk(MachineControl::Pace{20, 40, 150, 60'000, 5000, 400});
        const QString left = u"Keyboard and mouse were given back: no key was pressed and the mouse was not used for "
                             "1 min. Every key on the machine was let go."_s;
        QCOMPARE(MachineControl::Pace().idleAfterMs, 900'000);
        QVERIFY(desk.takeWithThePointerOver());
        // While somebody is there, they are held: a key, the pointer, a button, the wheel.
        for (int more = 0; more < 20; ++more) {
            QTest::qWait(50);
            switch (more % 4) {
            case 0:
                desk.keys.press(keyA);
                desk.keys.release(keyA);
                break;
            case 1:
                desk.control->pointerAt(0.5 + more / 100.0, 0.5);
                break;
            case 2:
                desk.control->button(1, true);
                desk.control->button(1, false);
                break;
            default:
                desk.control->wheel(0, 120);
                break;
            }
            QVERIFY2(desk.view.isOn(), qPrintable(QString::number(more)));
        }
        rest();
        desk.live.take();
        // Nobody is: given back by itself, everything let go on the machine first, and the bar
        // says why.
        QTRY_VERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.view.problem(), left);
        QVERIFY(!desk.keys.swallows());
        QVERIFY(!desk.keys.listens);
        // A key that is held and said again and again is nobody at the keyboard: something lies
        // on it. It is let go on the machine with the rest.
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        QCOMPARE(desk.live.take(), QStringList{u"+4"_s});
        for (int said = 0; said < 40 && desk.view.isOn(); ++said) {
            QTest::qWait(25);
            desk.keys.repeat(keyA);
        }
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.view.problem(), left);
        // And the pointer over the bars beside the picture is somebody.
        desk.keys.release(keyA);
        QVERIFY(desk.take());
        for (int more = 0; more < 12; ++more) {
            QTest::qWait(50);
            desk.control->pointerAt(1.5, 0.5);
        }
        QVERIFY(desk.view.isOn());
    }

    void aKeyThatIsHeldAndSaidNoMoreStaysDownOnTheMachine()
    {
        // (Turned round: this recorded that a key which went down last and is said no more for
        // a while is let go on the machine, as one whose "up" was lost. A modifier that is held
        // while a key without a USB number is tapped is said no more either, and it is held: it
        // went up on the machine under the finger, and the next key arrived there bare. A lost
        // "up" is found where the keys are heard, by the key it was lost of.)
        Desk desk;
        QVERIFY(desk.take());
        // Shift is held, and a key for sound is tapped under it: that one is nothing to the
        // machine, and the keyboard says nothing again from here on.
        desk.keys.press(leftShift);
        desk.keys.press(louder);
        desk.keys.release(louder);
        QCOMPARE(desk.live.take(), QStringList{u"+e1"_s});
        QTest::qWait(700);
        // Shift is still down on the machine, as it is here: A arrives under it.
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        desk.keys.release(leftShift);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s, u"-e1"_s}));
        // A key that is held by itself and said by nobody again (a keyboard that was told not to
        // repeat, a key held over a remote desktop) is held as well.
        desk.keys.press(keyA);
        QTest::qWait(700);
        QCOMPARE(desk.live.take(), QStringList{u"+4"_s});
        QVERIFY(desk.view.isOn());
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), QStringList{u"-4"_s});
    }

    void whatWasKeptFromWindowsIsKnownAtTheNextTakingWhateverTheEnding_data()
    {
        QTest::addColumn<int>("ending");
        QTest::newRow("1: keys could not be passed on in time") << 1;
        QTest::newRow("2: Windows hands the program its keys no more") << 2;
        QTest::newRow("3: Windows showed another desktop") << 3;
        QTest::newRow("4: giving back was asked for") << 4;
    }

    void whatWasKeptFromWindowsIsKnownAtTheNextTakingWhateverTheEnding()
    {
        QFETCH(int, ending);
        // (A held key that is said again is silent for no longer than 100 ms here.)
        Desk desk(MachineControl::Pace{20, 40, 150, 60'000, 100});
        QVERIFY(desk.take());
        desk.keys.press(leftCtrl);
        QCOMPARE(desk.live.take(), QStringList{u"+e0"_s});
        switch (ending) {
        case 1:
            desk.keys.fallBehind();
            break;
        case 2:
            desk.keys.loseHearing(ui::KeySource::Lost::NotHandedKeys);
            break;
        case 3:
            desk.keys.loseHearing(ui::KeySource::Lost::AnotherDesktop);
            break;
        default:
            desk.view.giveBack();
            break;
        }
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        // Taken again with the finger still on Ctrl. Windows never saw it go down, and a hook
        // that goes in now did not hear it either: C is no "c" all the same.
        QVERIFY(desk.take());
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), QStringList());
        // And for as long as it is held, whatever the ending was: a key is known to be down
        // until it was heard to go up, and is not given up for having been silent. (This took
        // them again inside the few seconds after which such a key was taken to be up, and so
        // never came to what followed them: C, bare, with control on.)
        QTest::qWait(300);
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), QStringList());
        QCOMPARE(desk.view.note(), heldFromBefore().arg(u"Left Ctrl"_s));
        desk.keys.release(leftCtrl);
        QCOMPARE(desk.view.note(), QString());
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
    }

    void theKeysAreListenedForAnewAtEveryTaking()
    {
        Desk desk;
        QVERIFY(desk.take());
        desk.keys.press(leftShift);
        desk.view.giveBack();
        // (Still listening, for Shift to be let go.)
        QVERIFY(desk.keys.listens);
        QCOMPARE(desk.keys.begun, 1);
        // Its hook went out meanwhile, by itself, and Windows lets no new one in: they are not
        // taken, and it is said. (Not: taken, with a keyboard that hears nothing.)
        desk.keys.lost = true;
        desk.keys.canBegin = false;
        desk.live.take();
        // (The machine says its word for the giving back, as one that says them does: they can
        // be asked for again.)
        desk.letTheMachineSpeak();
        desk.view.take();
        desk.live.give();
        QCOMPARE(desk.keys.begun, 2);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(),
                 u"Windows did not let qKVM listen for keys. Keyboard and mouse were given back."_s);
        QVERIFY(!desk.keys.swallows());
        // Where it lets one in, they are taken with a hook that is new.
        desk.keys.canBegin = true;
        QVERIFY(desk.take());
        QCOMPARE(desk.keys.begun, 3);
        QVERIFY(desk.keys.swallows());
    }

    void aYesIsTakenOnlyWhileItIsWaitedFor_data()
    {
        QTest::addColumn<int>("way");
        QTest::addColumn<QString>("why");
        QTest::addColumn<QString>("whyAfterTheYes");
        QTest::addColumn<QString>("whyOnceTheyCanBeAskedFor");
        // (Each of these said "Click Control again" from the first moment, while the machine
        // still owed its answer and clicking could not work. What can be done is said as it is:
        // `theBarAsksForNoClickThatCannotWork`.)
        const QString notInFront = u"This window was no longer in front before the machine answered, so keyboard and "
                                   "mouse were not taken."_s;
        const QString aKey = u"A key was pressed before the machine answered, so keyboard and mouse were not taken."_s;
        const QString aBox = u"Something else was opened in this window before the machine answered, so keyboard "
                             "and mouse were not taken."_s;
        QTest::newRow("1: the window was not in front for a moment")
            << 1 << notInFront + askAgainLater << notInFront + askAgainLater << notInFront + askAgainNow;
        QTest::newRow("2: a key was pressed in the window")
            << 2 << aKey + askAgainLater << aKey + askAgainLater << aKey + askAgainNow;
        QTest::newRow("3: the time the machine is given is over") << 3 << didNotAnswer << answeredLate << answeredLate;
        QTest::newRow("4: a box was opened in the window")
            << 4 << aBox + askAgainLater << aBox + askAgainLater << aBox + askAgainNow;
    }

    void aYesIsTakenOnlyWhileItIsWaitedFor()
    {
        QFETCH(int, way);
        QFETCH(QString, why);
        QFETCH(QString, whyAfterTheYes);
        QFETCH(QString, whyOnceTheyCanBeAskedFor);
        // (A machine that says no word by itself, and is slow: its yes comes after the asking
        // was given up, twice, and then its words for the givings back, each by hand.)
        Desk desk(Says::NoWord);
        // (Nothing while nothing is asked for.)
        desk.view.wentOn(true);
        desk.view.wentOn(false);
        QCOMPARE(desk.view.problem(), QString());
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        QVERIFY(desk.view.isAsking());
        switch (way) {
        case 1:
            desk.control->setInFront(false);
            desk.control->setInFront(true);
            break;
        case 2:
            desk.view.wentOn(true);
            break;
        case 4:
            desk.view.wentOn(false);
            break;
        default:
            QTRY_VERIFY(!desk.view.isAsking());
            break;
        }
        // The machine is told at once that nothing is asked of it any more, and the bar says
        // what happened: not that the machine was late, where it was not. (Turned round: a key
        // pressed before the machine had answered was said as "the machine answered late".)
        QVERIFY(!desk.view.isAsking());
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.view.problem(), why);
        // Its yes comes after all, into whatever is being done by then: given back at once, no
        // key is listened for, and why is said.
        desk.live.give();
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.keys.begun, 0);
        QCOMPARE(desk.view.problem(), whyAfterTheYes);
        // A second such yes is nothing more: given back as well.
        desk.live.give();
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.view.problem(), whyAfterTheYes);
        // The machine says its words for the givings back: nothing is owed, and what can be
        // done now is said. (This asked anew at once, with those words still owed.) One word
        // is owed by now: the second yes answered no asking that was owed, so it came after
        // everything that had been asked, and only its own giving back is behind it.
        QVERIFY(desk.view.isBusy());
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.problem(), whyOnceTheyCanBeAskedFor);
        // Asked for anew, the sentence goes and a yes is a yes.
        desk.view.take();
        QCOMPARE(desk.view.problem(), QString());
        desk.live.give();
        QVERIFY(desk.view.isOn());
    }

    void ctrlAltDelLeavesWhatFingersHoldAsItWas()
    {
        Desk desk;
        QVERIFY(desk.take());
        // Left Ctrl is down on the machine. It is not said down a second time, and after the
        // three were let go it is down there again, as it is held here: C is no bare "c".
        desk.keys.press(leftCtrl);
        QCOMPARE(desk.live.take(), QStringList{u"+e0"_s});
        desk.view.ctrlAltDel();
        QCOMPARE(desk.live.take(), (QStringList{u"+e2"_s, u"+4c"_s, u"-4c"_s, u"-e2"_s, u"-e0"_s, u"+e0"_s}));
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        desk.keys.release(leftCtrl);
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-6"_s, u"-e0"_s}));
        // With Del held: it goes down last all the same, and is not pressed once more by its
        // repeat afterwards.
        desk.keys.press(del);
        desk.view.ctrlAltDel();
        desk.keys.repeat(del);
        desk.keys.release(del);
        QCOMPARE(desk.live.take(),
                 (QStringList{u"+4c"_s, u"-4c"_s, u"+e0"_s, u"+e2"_s, u"+4c"_s, u"-4c"_s, u"-e2"_s, u"-e0"_s}));
        desk.keys.press(del);
        desk.keys.release(del);
        QCOMPARE(desk.live.take(), (QStringList{u"+4c"_s, u"-4c"_s}));
    }

    void aButtonIsNotNotedAsDownAfterTheLiveEndedUnderIt()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        // A place of the pointer waits to be said, and the line ends in the middle of saying it
        // for a click: the click goes nowhere.
        desk.control->pointerAt(0.25, 0.25);
        desk.control->pointerAt(0.75, 0.75);
        desk.live.endsWhilePointing = true;
        desk.control->button(1, true);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(),
                 (QStringList{u"at 8192,8192"_s, u"at 24575,24575"_s, u"release all"_s, u"give back"_s}));
        desk.control->button(1, false);
        // And with the wheel, and a button going up.
        for (const int way : {1, 2}) {
            desk.live.open = true;
            QVERIFY(desk.takeWithThePointerOver());
            if (way == 2) {
                desk.control->button(2, true);
            }
            desk.control->pointerAt(0.25, 0.25);
            desk.control->pointerAt(0.75, 0.75);
            desk.live.take();
            desk.live.endsWhilePointing = true;
            if (way == 1) {
                desk.control->wheel(0, 120);
            } else {
                desk.control->button(2, false);
            }
            QCOMPARE(desk.live.take(), (QStringList{u"at 24575,24575"_s, u"release all"_s, u"give back"_s}));
        }
        // The next time they are taken, the first press of that button is a press.
        desk.live.open = true;
        QVERIFY(desk.takeWithThePointerOver());
        desk.control->button(1, true);
        desk.control->button(1, false);
        QCOMPARE(desk.live.take(), (QStringList{u"b1+"_s, u"b1-"_s}));
    }

    // (Taken out: "the view is told whether the keyboard has the leave-key". Nothing can say
    // whether the keyboard that is plugged in has it, and nothing pretends to any more: the bar
    // names the button beside the key, always, which the tests of the screens hold it to.)

    void whyTheLineEndedIsSaidWhereTheyWereOnlyAskedFor()
    {
        // (Machines that say a word for every asking and every giving back, as far as their
        // line stands: none of those words is waited for here.)
        Desk desk(Says::EveryWord);
        const QString why = u"Lost contact with the KVM."_s;
        // Whoever holds the live says that its line ended, and then that there is none.
        desk.view.take();
        desk.control->lineEnded(why);
        desk.control->have(nullptr, u"a"_s);
        QVERIFY(!desk.view.isAsking());
        QCOMPARE(desk.view.problem(), why);
        // Where they were held, the live said why itself, and that sentence stays.
        NotedLive again;
        desk.control->have(&again, u"a"_s);
        desk.view.take();
        again.give();
        QVERIFY(desk.view.isOn());
        again.refuse(u"dropped"_s, u"The line to the KVM ended."_s);
        desk.control->lineEnded(why);
        desk.control->have(nullptr, u"a"_s);
        QCOMPARE(desk.view.problem(), u"The line to the KVM ended."_s);
        // And where nothing was asked of it, its end is nothing to say.
        NotedLive third;
        desk.control->have(&third, u"b"_s);
        desk.control->lineEnded(why);
        QCOMPARE(desk.view.problem(), QString());
    }

    // --- what a fourth reviewer found ---

    void aClickDoesNotPassAKeyThatWasPressedBeforeItAndIsStillToBeTold()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        // Shift goes down, and is not told yet: where the keys are heard it waits a moment,
        // for Windows' two accounts of the keyboard to be at one. A click follows. It does not
        // go to the machine before Shift does: it would be a bare click there.
        desk.keys.holdsBackPresses = true;
        desk.keys.press(leftShift);
        desk.control->button(1, true);
        desk.control->button(1, false);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.tellWhatWaits();
        QCOMPARE(desk.live.take(), (QStringList{u"+e1"_s, u"b1+"_s, u"b1-"_s}));
        desk.keys.release(leftShift);
        QCOMPARE(desk.live.take(), QStringList{u"-e1"_s});
        // Clicked twice in the moment it waits: two clicks, each whole. (A button that is said
        // to go down twice with nothing between is still pressed once.)
        desk.keys.holdsBackPresses = true;
        desk.keys.press(leftShift);
        for (int click = 0; click < 2; ++click) {
            desk.control->button(1, true);
            desk.control->button(1, true);
            desk.control->button(1, false);
            desk.control->button(1, false);
        }
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.tellWhatWaits();
        QCOMPARE(desk.live.take(), (QStringList{u"+e1"_s, u"b1+"_s, u"b1-"_s, u"b1+"_s, u"b1-"_s}));
        desk.keys.release(leftShift);
        QCOMPARE(desk.live.take(), QStringList{u"-e1"_s});

        // The wheel as well, and the pointer with them: each lands where the pointer was for
        // it, in the order it came in, and where the pointer is by now is said after them.
        desk.control->pointerAt(0.25, 0.25);
        rest();
        QCOMPARE(desk.live.take(), QStringList{u"at 8192,8192"_s});
        desk.keys.holdsBackPresses = true;
        desk.keys.press(keyA);
        desk.control->button(1, true);
        desk.control->pointerAt(0.75, 0.75);
        desk.control->button(1, false);
        desk.control->wheel(0, 240);
        desk.control->pointerAt(0.5, 0.5);
        rest();
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.tellWhatWaits();
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"b1+"_s, u"at 24575,24575"_s, u"b1-"_s, u"wheel 0,1"_s,
                                                u"wheel 0,1"_s, u"at 16384,16384"_s}));
        desk.keys.release(keyA);
        rest();
        QCOMPARE(desk.live.take(), QStringList{u"-4"_s});

        // A key that is pressed after the click goes to the machine after it; and a click that
        // comes while nothing waits goes at once, as ever.
        desk.keys.holdsBackPresses = true;
        desk.keys.press(leftShift);
        desk.control->button(1, true);
        desk.control->button(1, false);
        desk.keys.press(keyC);
        desk.control->button(2, true);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.tellWhatWaits();
        QCOMPARE(desk.live.take(), (QStringList{u"+e1"_s, u"b1+"_s, u"b1-"_s, u"+6"_s, u"b2+"_s}));
        desk.control->button(2, false);
        desk.control->button(3, true);
        QCOMPARE(desk.live.take(), (QStringList{u"b2-"_s, u"b3+"_s}));
        // A button whose press has gone out goes up at once, whatever waits: letting go needs
        // nobody's word.
        desk.keys.holdsBackPresses = true;
        desk.keys.repeat(keyC);
        desk.control->button(3, false);
        QCOMPARE(desk.live.take(), QStringList{u"b3-"_s});
        desk.keys.tellWhatWaits();
        desk.keys.release(keyC);
        desk.keys.release(leftShift);
        QCOMPARE(desk.live.take(), (QStringList{u"-6"_s, u"-e1"_s}));

        // Under a modifier that turns out to be kept back, the click is kept back with it:
        // Ctrl went to Windows (it could not be kept from it), and is this PC's.
        desk.keys.holdsBackPresses = true;
        desk.keys.stalled = true;
        desk.keys.press(leftCtrl);
        desk.keys.stalled = false;
        desk.control->button(1, true);
        desk.control->button(1, false);
        desk.keys.tellWhatWaits();
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.release(leftCtrl);

        // Given back while a click waits: it is dropped, and never reaches the machine.
        desk.keys.holdsBackPresses = true;
        desk.keys.press(leftShift);
        desk.control->button(1, true);
        desk.view.giveBack();
        QCOMPARE(desk.live.take(), letGo);
        desk.keys.tellWhatWaits();
        desk.control->button(1, false);
        desk.keys.release(leftShift);
        rest();
        QCOMPARE(desk.live.take(), QStringList());
    }

    void whatTheMouseDidBehindAKeyIsNotSentLateAndNotWithoutEnd()
    {
        const QString notInTime = u"Keys could not be passed on in time. Keyboard and mouse were given back, and "
                                  "every key on the machine was let go."_s;
        // (A click waits 100 ms at the most here. In the program it is a second, as for a key.)
        Desk late(MachineControl::Pace{20, 40, 150, 60'000, 3000, 600'000, 100});
        QVERIFY(late.takeWithThePointerOver());
        late.keys.holdsBackPresses = true;
        late.keys.press(leftShift);
        late.control->button(1, true);
        QVERIFY(late.view.isOn());
        // The key is not told in time: the click is not sent late. Given back, as with keys
        // that fell behind, and nothing of the click reached the machine.
        QTRY_VERIFY(!late.view.isOn());
        QCOMPARE(late.view.problem(), notInTime);
        QCOMPARE(late.live.take(), letGo);

        // No more than a few wait at a time.
        Desk many;
        QVERIFY(many.takeWithThePointerOver());
        many.keys.holdsBackPresses = true;
        many.keys.press(leftShift);
        for (int turn = 0; turn < 32; ++turn) {
            many.control->wheel(0, 120);
        }
        QVERIFY(many.view.isOn());
        QCOMPARE(many.live.take(), QStringList());
        many.control->wheel(0, 120);
        QVERIFY(!many.view.isOn());
        QCOMPARE(many.view.problem(), notInTime);
        QCOMPARE(many.live.take(), letGo);
    }

    void aKeyThatNoFingerHoldsAnyMoreDoesNotStayDownOnTheMachine()
    {
        // (In the place of "where no key is down any more nothing stays down on the machine",
        // which said the key source's words by hand, in an order the real one never says them
        // in, and so showed a letting go that the real one never set off: where a key's "up"
        // came as another key's, its table went on holding the key and never said that nothing
        // is down. Here the keys come through the real one's own procedure, as Windows hands a
        // hook its keys and as Raw Input tells of them. Taken out with it, since they said that
        // word by hand: that a Windows key which still waits goes down and up on that word (it
        // does so by its own going up, `theWindowsKeyIsSaidToTheMachineOnlyWhenItIsNoWinL`);
        // and what `MachineControl` does on that word while a key is still down on the machine.
        // The real key source tells of every key that it lets go, so by that word none is: the
        // letting go there is a net beneath, and no sequence of keys comes to it.)
        //
        // On a PC that gives no second account. Ctrl is held, and Num Lock is pressed under
        // it: were Windows to tell a hook of that without the key's mark, as "Pause" (not known
        // whether it does), the key goes down on the machine as that. Ctrl is let go first.
        // Num Lock is let go, and is told with its mark and its own name: what went down on
        // the machine goes up there, and where the keys are heard nothing is down any more.
        // What this asserts with that, and it is no good thing: in that case the machine is
        // sent Ctrl+Pause ("+48" below), which is Break there, where Ctrl+Num Lock was pressed.
        // Nothing here can tell the two apart without the mark (`input::usageOf`). Whether
        // Windows drops the mark is for a person at a keyboard to see, and is on the list.
        DeskOfTheHook desk;
        QVERIFY(desk.take());
        const QSignalSpy nothingIsDown(&desk.keys, &ui::KeySource::nothingIsDown);
        desk.keys.hears(0xA2, 0x1D, 0, true);
        desk.keys.hears(0x13, 0x45, 0, true);
        desk.keys.hears(0xA2, 0x1D, 0, false);
        QCOMPARE(desk.live.take(), (QStringList{u"+e0"_s, u"+48"_s, u"-e0"_s}));
        QCOMPARE(nothingIsDown.count(), 0);
        desk.keys.hears(0x90, 0x45, marked, false);
        QCOMPARE(desk.live.take(), QStringList{u"-48"_s});
        QCOMPARE(nothingIsDown.count(), 1);
        QVERIFY(desk.view.isOn());
        // The key is a key again, under whichever of its two names.
        desk.keys.hears(0x90, 0x45, marked, true);
        desk.keys.hears(0x90, 0x45, marked, false);
        QCOMPARE(desk.live.take(), (QStringList{u"+53"_s, u"-53"_s}));
    }

    void anUpNamedAsNoKeyOfTheKeyboardsEndsTheControlAndLeavesNoKeyDownOnTheMachine()
    {
        // (Turned round, and named anew. It was "an up named otherwise than its down which
        // nobody has seen does not leave the key down on the machine": the right Shift let go
        // and told to the hook as "Shift" with the mark, which is no key's number; the
        // comparing read that with the mark turned, took it for the right Shift, found the two
        // accounts at one, and the key was let go by Raw Input's own word with the control
        // going on. The reading with the mark turned is gone: by it a key for sound was the
        // letter that shares its number. So such an "up" is no hearing of the right Shift's
        // any more. The key does not stay down on the machine all the same: the two accounts
        // differ, and the control ends within the moment, with everything let go.)
        // On a PC that gives a second account. The right Shift is held and A typed under it.
        // The Shift is let go, and the hook is told so as "Shift" with the mark its right Shift
        // comes with: no key of the keyboard's, and nothing to the comparing. (Not known to
        // happen: it stands for every "up" that Windows may name in a way not foreseen here.)
        // Raw Input tells of the right Shift going up, and that waits in vain to be heard. A
        // moment later: the control ends, said as a key that Windows saw and that was not
        // handed over; the Shift is let go on the machine with everything else, and is not
        // remembered as a key that is still held.
        const QString notHanded =
            u"Windows saw a key on the keyboard that qKVM was not handed. Windows may have stopped "
            "handing it keys, or a key remapper, a launcher or a Windows key helper (Sticky, Filter or Mouse "
            "Keys) held the key back. Keyboard and mouse were given back, and every key on the machine was let "
            "go. If it happens again, close that program or switch the helper off."_s;
        DeskOfTheHook watched(true);
        QVERIFY(watched.take());
        watched.keys.hears(0xA1, 0x36, marked, true);
        watched.keys.isToldOf(0x36, 0, 0x10, true);
        watched.keys.hears(0x41, 0x1E, 0, true);
        watched.keys.isToldOf(0x1E, 0, 0x41, true);
        watched.keys.hears(0x41, 0x1E, 0, false);
        watched.keys.isToldOf(0x1E, 0, 0x41, false);
        QCOMPARE(watched.live.take(), (QStringList{u"+e5"_s, u"+4"_s, u"-4"_s}));
        watched.keys.hears(0x10, 0x36, marked, false);
        watched.keys.isToldOf(0x36, 0, 0x10, false);
        QCOMPARE(watched.live.take(), QStringList());
        QVERIFY(watched.view.isOn());
        watched.keys.looks(ui::hook::heardWithinMs + 1);
        QVERIFY(!watched.view.isOn());
        QCOMPARE(watched.live.take(), letGo);
        QCOMPARE(watched.view.problem(), notHanded);
        QVERIFY(watched.keys.whatIsKept().empty());
        QVERIFY(!watched.keys.swallows());
    }

    void thatNoKeyIsDownAnyMoreSaysNothingOfAKeyThatIsHeldFromBefore()
    {
        // What the key source says of what is down says nothing of a key that is kept back as
        // held from before the keys were heard: Ctrl was down when they were taken. A is typed
        // under it, and with A's going up no key that was heard to go down is down any more.
        // Ctrl stays kept back, and C under it, until Ctrl was let go.
        DeskOfTheHook held(false, {leftCtrl});
        QVERIFY(held.take());
        const QSignalSpy nothingIsDownHere(&held.keys, &ui::KeySource::nothingIsDown);
        held.keys.hears(0x41, 0x1E, 0, true);
        held.keys.hears(0x41, 0x1E, 0, false);
        QCOMPARE(nothingIsDownHere.count(), 1);
        held.keys.hears(0x43, 0x2E, 0, true);
        held.keys.hears(0x43, 0x2E, 0, false);
        QCOMPARE(held.live.take(), QStringList());
        held.keys.hears(0xA2, 0x1D, 0, false);
        held.keys.hears(0x43, 0x2E, 0, true);
        held.keys.hears(0x43, 0x2E, 0, false);
        QCOMPARE(held.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
    }

    void theShiftThatAKeyboardMakesUpDoesNotLetGoTheShiftThatIsHeld()
    {
        // Shift is held, and Home is pressed under it. A keyboard says a Shift going up before
        // Home and one going down after it, which no finger made. Raw Input tells of those
        // without a name, and they are no key there. Were Windows to hand a hook the same
        // without a name at Shift's own place (not known whether it hands them over at all),
        // the Shift that is held on the machine stays down there: what arrives is Shift+Home,
        // and not Shift up, then Home. (An "up" without a name at the place of a key that was
        // down let that key go: the machine got Home without Shift, and what was to be marked
        // on a server was not.)
        for (const bool secondAccount : {false, true}) {
            DeskOfTheHook desk(secondAccount);
            QVERIFY(desk.take());
            desk.keys.hears(0xA0, 0x2A, 0, true);
            desk.keys.isToldOf(0x2A, 0, 0x10, true);
            desk.keys.isToldOf(0x2A, rawMarked, noName, false);
            desk.keys.hears(noName, 0x2A, 0, false);
            desk.keys.hears(0x24, 0x47, marked, true);
            desk.keys.isToldOf(0x47, rawMarked, 0x24, true);
            desk.keys.hears(0x24, 0x47, marked, false);
            desk.keys.isToldOf(0x47, rawMarked, 0x24, false);
            desk.keys.isToldOf(0x2A, rawMarked, noName, true);
            desk.keys.hears(noName, 0x2A, 0, true);
            QCOMPARE(desk.live.take(), (QStringList{u"+e1"_s, u"+4a"_s, u"-4a"_s}));
            QVERIFY(desk.view.isOn());
            desk.keys.hears(0xA0, 0x2A, 0, false);
            desk.keys.isToldOf(0x2A, 0, 0x10, false);
            QCOMPARE(desk.live.take(), QStringList{u"-e1"_s});
            QVERIFY(desk.view.isOn());
        }
    }

    void aClickGoesBehindEverythingOfTheKeyboardThatWasHeardBeforeIt()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        // Ctrl is down on the machine. C goes down under it and is not told yet; Ctrl is let
        // go, and its going up stays behind C, which was pressed under it. Then a click. It
        // goes to the machine behind both: a plain click, as it was made. (It waited behind
        // the keys that went down only, and went out right after C: a click with Ctrl.)
        desk.keys.press(leftCtrl);
        QCOMPARE(desk.live.take(), QStringList{u"+e0"_s});
        desk.keys.holdsBackPresses = true;
        desk.keys.press(keyC);
        desk.keys.release(leftCtrl);
        desk.control->button(1, true);
        desk.control->button(1, false);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.tellWhatWaits();
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-e0"_s, u"b1+"_s, u"b1-"_s}));
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), QStringList{u"-6"_s});
    }

    void theWheelGoesBehindEverythingOfTheKeyboardThatWasHeardBeforeIt()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        // The wheel as a click: Ctrl let go, and then the wheel turned, is no turn with Ctrl.
        desk.keys.press(leftCtrl);
        desk.live.take();
        desk.keys.holdsBackPresses = true;
        desk.keys.press(keyC);
        desk.keys.release(leftCtrl);
        desk.control->wheel(0, 120);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.tellWhatWaits();
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-e0"_s, u"wheel 0,1"_s}));
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), QStringList{u"-6"_s});
    }

    void aClickGoesBehindNothingThatWasHeardAfterIt()
    {
        Desk desk;
        QVERIFY(desk.takeWithThePointerOver());
        // Behind what was heard before it, and behind nothing that was heard after it: A, which
        // is down on the machine, is let go after the click, and its going up is told at once,
        // past what waits. That is none of what the click waits for: it still goes behind C
        // and Ctrl's going up, and not one key sooner. (Nor, as it was before any of this, one
        // key too soon: behind C, and before Ctrl's going up.)
        desk.keys.press(keyA);
        desk.keys.press(leftCtrl);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"+e0"_s}));
        desk.keys.holdsBackPresses = true;
        desk.keys.press(keyC);
        desk.keys.release(leftCtrl);
        desk.control->button(1, true);
        desk.keys.release(keyA);
        QCOMPARE(desk.live.take(), QStringList{u"-4"_s});
        desk.keys.tellWhatWaits();
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-e0"_s, u"b1+"_s}));
        desk.control->button(1, false);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), (QStringList{u"b1-"_s, u"-6"_s}));
        QVERIFY(desk.view.isOn());
    }

    void aClickGoesBehindTheKeysAsTheHooksOwnProcedureNumbersThem()
    {
        // Through the real key source's own procedure, which numbers the keys: Ctrl is down on
        // the machine; C is heard, and waits for Raw Input's word; Ctrl's going up is heard,
        // and stays behind C; a click; then Raw Input's words come.
        DeskOfTheHook real(true);
        QVERIFY(real.take());
        real.control->pointerAt(0.5, 0.5);
        rest();
        real.live.take();
        real.keys.hears(0xA2, 0x1D, 0, true);
        real.keys.isToldOf(0x1D, 0, 0x11, true);
        QCOMPARE(real.live.take(), QStringList{u"+e0"_s});
        real.keys.hears(0x43, 0x2E, 0, true);
        real.keys.hears(0xA2, 0x1D, 0, false);
        real.control->button(1, true);
        real.control->button(1, false);
        QCOMPARE(real.live.take(), QStringList());
        real.keys.isToldOf(0x2E, 0, 0x43, true);
        real.keys.isToldOf(0x1D, 0, 0x11, false);
        QCOMPARE(real.live.take(), (QStringList{u"+6"_s, u"-e0"_s, u"b1+"_s, u"b1-"_s}));
        QVERIFY(real.view.isOn());
    }

    void thatAnotherProgramTypesKeysWhichReachNoMachineIsSaid()
    {
        const QString typed = u"Keys that another program types (a remote-control program, a keyboard shared over "
                              "the network, the on-screen keyboard) are not sent to the machine: only what is "
                              "pressed on this PC's own keyboard is."_s;
        const QString notNoticed = u"If Windows stops passing keys to qKVM, that is not noticed on this PC: "
                                   "click Control to give keyboard and mouse back."_s;
        Desk desk;
        // (While nothing is taken, it is nothing.)
        emit desk.keys.typedByAProgram();
        QCOMPARE(desk.view.note(), QString());
        QVERIFY(desk.take());
        QCOMPARE(desk.view.note(), QString());
        // Somebody types from afar: the keys go to Windows, and to no machine. The first time
        // that is seen, the bar says so, for as long as they are held.
        desk.keys.typeAsAProgram();
        QCOMPARE(desk.view.note(), typed);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.typeAsAProgram();
        desk.keys.press(keyA);
        desk.keys.release(keyA);
        QCOMPARE(desk.view.note(), typed);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
        // Given back, it is gone; taken again, it is said again when it happens again.
        desk.view.giveBack();
        QCOMPARE(desk.view.note(), QString());
        QVERIFY(desk.take());
        QCOMPARE(desk.view.note(), QString());
        desk.keys.typeAsAProgram();
        QCOMPARE(desk.view.note(), typed);
        // Beside what else is to be known: each in its turn, none in the place of another.
        desk.keys.watched = false;
        QTRY_COMPARE(desk.view.note(), typed + u' ' + notNoticed);
    }

    void anAskingThatNothingAnswersIsOverAndTheyCanBeAskedForAgain()
    {
        // The machine says nothing to the asking, and nothing to the giving back that follows
        // it (one kind of live says neither, where it cannot ask its KVM at all). The asking is
        // over when its time is, said as that. Control is busy for as long as the machine's
        // words are owed, and offered again after that: not inert for as long as the machine is
        // shown large. (Turned round: this recorded that Control was offered again the moment
        // the asking's time was over, with both words still owed. An asking made then could not
        // tell its answer from them: `noAskingIsMadeWhileTheMachineOwesAWord`.)
        // (A machine that says no word by itself, here and for the second one.)
        Desk desk(Says::NoWord, owedForAMoment);
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        QTRY_VERIFY(!desk.view.isAsking());
        QCOMPARE(desk.view.problem(), didNotAnswer);
        QCOMPARE(desk.live.take(), letGo);
        QVERIFY(desk.view.isBusy());
        QTRY_VERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.problem(), didNotAnswer);
        desk.view.take();
        QVERIFY(desk.view.isAsking());
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        desk.view.giveBack();
        desk.live.take();

        // And so an asking that whoever sits here gave up: busy for the rest of the time the
        // machine is given and for as long as a word is owed, and no longer.
        Desk gaveUp(Says::NoWord, owedForAMoment);
        const QString aKey = u"A key was pressed before the machine answered, so keyboard and mouse were not taken."_s;
        gaveUp.view.take();
        gaveUp.view.wentOn(true);
        QVERIFY(gaveUp.view.isBusy());
        QCOMPARE(gaveUp.view.problem(), aKey + askAgainLater);
        gaveUp.live.take();
        gaveUp.view.take();
        QCOMPARE(gaveUp.live.take(), QStringList());
        QTRY_VERIFY(!gaveUp.view.isBusy());
        // (What was said of why it was given up still stands, with what can be done now.)
        QCOMPARE(gaveUp.view.problem(), aKey + askAgainNow);
        gaveUp.view.take();
        QCOMPARE(gaveUp.live.take(), QStringList{u"ask"_s});
        QVERIFY(gaveUp.view.isAsking());
    }

    void whatTheMachineSaysOfAnAskingThatIsOverIsNobodys()
    {
        // (Taken out of here, with the rule: "asked anew while the first asking's answer is
        // owed, a yes is not taken for the yes to this one, and the asking that waits is taken
        // back with it", and its sentence "may be its answer to an earlier asking... Click
        // Control again"; and "a late no with its sentence, while they are asked for anew, is
        // shown, and the asking that waits is taken back". Neither can come about any more: no
        // asking is made while a word is owed. Whoever followed that sentence and clicked at
        // once was refused the same way, every time.)
        // The machine's yes to an asking that is over comes late: nobody's, and given back at
        // once. No key is listened for. (Each of the machines here says no word by itself:
        // what it says is said by hand.)
        Desk desk(Says::NoWord, owedForAMoment);
        desk.view.take();
        QTRY_VERIFY(!desk.view.isAsking());
        desk.live.take();
        desk.live.give();
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.view.problem(), answeredLate);
        QCOMPARE(desk.keys.begun, 0);
        // Its words for the two givings back are nobody's either.
        QVERIFY(desk.view.isBusy());
        desk.live.refuse({}, {});
        QVERIFY(desk.view.isBusy());
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.problem(), answeredLate);
        // From here a yes is a yes.
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        desk.live.give();
        QVERIFY(desk.view.isOn());
        // While they are held a no is theirs, whatever came before: taken away.
        desk.live.refuse(u"taken"_s, u"Taken."_s);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(), u"Taken."_s);

        // A machine that was only slow (its line for keyboard and mouse took longer to open than
        // it is given, and was given up): its word for the giving back comes at once, and none
        // for the asking ever does. That cannot be told from a no to the asking with the word
        // for the giving back still to come: one word is taken to be owed still, and runs out
        // its time. Asked anew after that, a yes is a yes. (Turned round: this recorded that
        // they could be asked for the moment that one word had come.)
        Desk slow(Says::NoWord, owedForAMoment);
        slow.view.take();
        QTRY_VERIFY(!slow.view.isAsking());
        slow.live.refuse({}, {});
        QVERIFY(slow.view.isBusy());
        slow.live.take();
        slow.view.take();
        QCOMPARE(slow.live.take(), QStringList());
        QTRY_VERIFY(!slow.view.isBusy());
        slow.view.take();
        slow.live.give();
        QVERIFY(slow.view.isOn());

        // Where the late words never come, nothing waits for them longer than the machine is
        // shown: with another machine shown large, nothing is owed.
        Desk other(Says::NoWord, MachineControl::Pace{20, 40, 100});
        other.view.take();
        QTRY_VERIFY(!other.view.isAsking());
        QVERIFY(other.view.isBusy());
        NotedLive another;
        other.control->have(&another, u"b"_s);
        QVERIFY(!other.view.isBusy());
        other.view.take();
        another.give();
        QVERIFY(other.view.isOn());
    }

    void theBarNamesEveryModifierThatIsKeptBackWhyever()
    {
        const QString keptBackUntilLetGo = u"Kept back from the machine until it is let go: %1. Until then, no other "
                                           "key and no click goes to the machine."_s;
        const auto keptBack = [&](const QString& which) { return keptBackUntilLetGo.arg(which); };
        // Ctrl is down as keyboard and mouse are taken: it is kept back, and every key with it.
        // Not without a word. (The bar named only a key that was held from an earlier time of
        // holding them.)
        Desk desk;
        desk.keys.press(leftCtrl);
        QVERIFY(desk.take());
        QCOMPARE(desk.view.note(), keptBack(u"Left Ctrl"_s));
        desk.keys.press(keyC);
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), QStringList());
        // Shift goes down under it, and is kept back as well.
        desk.keys.press(leftShift);
        QCOMPARE(desk.view.note(), keptBack(u"Left Ctrl, Left Shift"_s));
        desk.keys.release(leftCtrl);
        QCOMPARE(desk.view.note(), keptBack(u"Left Shift"_s));
        desk.keys.release(leftShift);
        QCOMPARE(desk.view.note(), QString());
        // One that went to Windows, because it could not be kept from it.
        desk.keys.stalled = true;
        desk.keys.press(leftShift);
        desk.keys.stalled = false;
        QCOMPARE(desk.view.note(), keptBack(u"Left Shift"_s));
        desk.keys.release(leftShift);
        QCOMPARE(desk.view.note(), QString());
        // One that went down under the leave-key, which was let go first.
        desk.keys.press(rightCtrl);
        desk.keys.press(leftShift);
        desk.keys.release(rightCtrl);
        QCOMPARE(desk.view.note(), keptBack(u"Left Shift"_s));
        desk.keys.release(leftShift);
        QCOMPARE(desk.view.note(), QString());
        QVERIFY(desk.view.isOn());
        // A key that is no modifier holds nothing back, and is not named.
        desk.keys.stalled = true;
        desk.keys.press(keyA);
        desk.keys.stalled = false;
        QCOMPARE(desk.view.note(), QString());
        desk.keys.release(keyA);
        // Beside one that is held from before: each said as what it is.
        desk.keys.press(leftCtrl);
        desk.view.giveBack();
        desk.keys.press(leftShift);
        QVERIFY(desk.take());
        QCOMPARE(desk.view.note(), heldFromBefore().arg(u"Left Ctrl"_s) + u' ' + keptBack(u"Left Shift"_s));
    }

    void thatSomethingMayStillBeDownOnTheMachineIsSaidInTheBar()
    {
        const QString mayBeDown = u"Something that was pressed may still be down on this machine: the word that lets "
                                  "go of it may not have arrived. Take keyboard and mouse again, and press and let go "
                                  "of it."_s;
        const QString fromBefore = u"Something that was pressed before may still be down on this machine: the word "
                                   "that lets go of it may not have arrived. Press and let go of it."_s;
        const QString takenAway = u"Somebody else took keyboard and mouse of this machine."_s;
        // Keyboard and mouse are taken away, and whoever gave the live says that something may
        // still be down on the machine: just before it says that they went. Said in the bar,
        // behind why they went. (Nothing heard it: whoever sat there was told nothing.)
        Desk desk;
        QVERIFY(desk.take());
        emit desk.live.leftDown();
        desk.live.refuse(u"taken"_s, takenAway);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(), takenAway + u' ' + mayBeDown);
        QCOMPARE(desk.view.note(), QString());
        // They are taken anew: whoever sits there has the machine's keyboard again, and it is
        // said no longer.
        QVERIFY(desk.take());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.note(), QString());
        // Said late, seconds after they went, in a time of holding them that it is not of (a
        // line that was let go and then went without the KVM's goodbye): it is said all the
        // same, as a note, and takes nothing away.
        emit desk.live.leftDown();
        QVERIFY(desk.view.isOn());
        QVERIFY(desk.control->isOn());
        QCOMPARE(desk.view.note(), fromBefore);
        QCOMPARE(desk.live.take(), QStringList());
        // It stays said when they are given back, with nothing else to say.
        desk.view.giveBack();
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(), mayBeDown);
        // It is of that machine: another machine in this window, and it is not said of it.
        desk.control->have(&desk.live, u"b"_s);
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.note(), QString());
        // And said where they were only given back: alone, there being no other why.
        desk.control->have(&desk.live, u"a"_s);
        QVERIFY(desk.take());
        desk.view.giveBack();
        emit desk.live.leftDown();
        desk.live.refuse({}, {});
        QCOMPARE(desk.view.problem(), mayBeDown);
    }

    void whyNothingIsSwallowedAnyMoreIsAskedForBeforeItIsTakenForTheLeaveKey()
    {
        Desk desk;
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        QCOMPARE(desk.live.take(), QStringList{u"+4"_s});
        // Where the keys are heard, they fell behind (this thread was held up), and nothing is
        // swallowed any more. The word for it is still on its way when this one next looks:
        // it asks for it, and says that. (It took a keyboard that swallows no more for the
        // leave-key's doing, and gave them back with no reason.)
        desk.keys.fallBehindUnsaid();
        QTRY_VERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(), u"Keys could not be passed on in time. Keyboard and mouse were given back, and "
                                      "every key on the machine was let go."_s);
        QCOMPARE(desk.live.take(), letGo);
        // The leave-key's own doing has no reason, and none is said.
        desk.keys.release(keyA);
        QVERIFY(desk.take());
        desk.keys.leaveByItself();
        QTRY_VERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(), QString());
    }

    void aKeyThatIsOnlySaidAgainIsNoKeyGoingDownForTheLeaveKey()
    {
        Desk desk;
        QVERIFY(desk.take());
        // Enter is held, on a second keyboard say, which goes on saying it. The leave-key is
        // pressed and let go: no other key went down between, and they are given back.
        desk.keys.press(keyC);
        desk.keys.press(rightCtrl);
        desk.keys.repeat(keyC);
        desk.keys.repeat(keyC);
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.live.take(), QStringList{u"+6"_s});
        desk.keys.release(rightCtrl);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        desk.keys.release(keyC);
        // A key that goes down between still makes it no leaving.
        QVERIFY(desk.take());
        desk.keys.press(rightCtrl);
        desk.keys.press(keyC);
        desk.keys.repeat(keyC);
        desk.keys.release(rightCtrl);
        QVERIFY(desk.view.isOn());
        desk.keys.release(keyC);
        QCOMPARE(desk.live.take(), QStringList());
    }

    // --- what a fifth reviewer found ---

    void theMachinesWordForAGivingBackIsNotTheNextAskingsAnswer()
    {
        // Held, and given back. The machine's word for the giving back, a "no" without a
        // sentence, has not come: until it has they are not asked for, so it is never an
        // asking's "no". (Turned round: this asked again at once, and recorded that the word
        // which came then was taken for what was owed while the asking went on waiting. That
        // rule is gone with what it was for: an asking is made only while nothing is owed, and
        // the first word that comes is its answer.)
        // (Each word of the machine is said by hand, at the moment it is to come.)
        Desk desk(Says::NoWord);
        QVERIFY(desk.take());
        desk.view.giveBack();
        QCOMPARE(desk.live.take(), letGo);
        QVERIFY(desk.view.isBusy());
        desk.view.take();
        QVERIFY(!desk.view.isAsking());
        QCOMPARE(desk.live.take(), QStringList());
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.problem(), QString());
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        desk.live.give();
        QVERIFY(desk.view.isOn());
        // Nothing is owed while they are held: a "no" then is theirs.
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(), u"Keyboard and mouse of this machine were taken away."_s);
        // And asked for with nothing owed, a "no" without a sentence is the asking's own.
        desk.live.refuse({}, {});
        desk.view.take();
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isAsking());
        QCOMPARE(desk.view.problem(), u"Keyboard and mouse of this machine were not given."_s);
    }

    void aYesThatCameLateOwesAWordForItsOwnGivingBack()
    {
        // A yes that came late is given back at once, which is a second giving back: three
        // words where an asking and its giving back make two. The machine's word for each of
        // the two givings back is nobody's, and until both have come they are not asked for.
        // (Turned round: this asked again after the yes, and recorded that the two words were
        // taken for what was owed while that asking went on waiting.)
        // (A machine that says no word by itself: the late yes and the two words by hand.)
        Desk late(Says::NoWord, MachineControl::Pace{20, 40, 100});
        late.view.take();
        QTRY_VERIFY(!late.view.isAsking());
        QCOMPARE(late.live.take(), (QStringList{u"ask"_s, u"release all"_s, u"give back"_s}));
        late.live.give();
        QVERIFY(!late.view.isOn());
        QCOMPARE(late.live.take(), letGo);
        QCOMPARE(late.view.problem(), answeredLate);
        late.view.take();
        QCOMPARE(late.live.take(), QStringList());
        late.live.refuse({}, {});
        QVERIFY(late.view.isBusy());
        late.view.take();
        QCOMPARE(late.live.take(), QStringList());
        late.live.refuse({}, {});
        QVERIFY(!late.view.isBusy());
        late.view.take();
        QCOMPARE(late.live.take(), QStringList{u"ask"_s});
        QCOMPARE(late.view.problem(), QString());
        late.live.give();
        QVERIFY(late.view.isOn());
        QCOMPARE(late.keys.begun, 1);
    }

    void afterTheOneWordOfAMachineThatCallsAnAskingOffWithoutAWordTheOtherRunsOutItsTime()
    {
        // (In the place of "with its word for a giving back the machine owes no answer to the
        // asking before it", which asked anew the moment that word had come and recorded the
        // yes as taken. By the order a machine answers in, no answer is owed after that word:
        // that still holds. But the word cannot be told from a no to the asking, with the word
        // for the giving back still to come, and while a word may be owed they are not asked
        // for.)
        // (Named anew: it was "the one word of a KVM of this PC's own". Such a KVM says two
        // by now, a no for the asking and the word for the giving back, and Control is offered
        // a turn of the loop later: `afterEveryEndingControlIsOfferedAgainWithinATurnOfTheLoop`.
        // What is tried here is a machine that says the one word only, by hand.)
        // A machine that answers an asking which it was too slow for with its word for the
        // giving back, and with no word for the asking ever (the asking was called off there).
        // One word is taken to be owed still, for its time and no longer. Then: asked,
        // answered, taken.
        Desk own(Says::NoWord, owedForAMoment);
        own.view.take();
        QTRY_VERIFY(!own.view.isAsking());
        own.live.refuse({}, {});
        own.live.take();
        QVERIFY(own.view.isBusy());
        own.view.take();
        QCOMPARE(own.live.take(), QStringList());
        QTRY_VERIFY(!own.view.isBusy());
        own.view.take();
        QCOMPARE(own.live.take(), QStringList{u"ask"_s});
        own.live.give();
        QVERIFY(own.view.isOn());
        QCOMPARE(own.view.problem(), QString());
    }

    void aNoWithItsSentenceIsShownAsWhatTheMachineSaid()
    {
        const QString inUse = u"Keyboard and mouse of this machine are in use in another window."_s;
        // A machine that says nothing at all to an asking and its giving back, and is asked
        // again when what it owed has run out. It refuses, with its sentence: that is shown as
        // what the machine said. (Turned round in how it comes there: this asked again while
        // both words were owed, which is not done any more. It was written because such a no
        // was once taken for a word that was owed, the asking waited out its time and was said
        // as "did not answer", and so every asking after it.)
        Desk silent(Says::NoWord, owedForAMoment);
        silent.view.take();
        QTRY_VERIFY(!silent.view.isAsking());
        QCOMPARE(silent.view.problem(), didNotAnswer);
        silent.live.take();
        QTRY_VERIFY(!silent.view.isBusy());
        silent.view.take();
        QCOMPARE(silent.live.take(), QStringList{u"ask"_s});
        silent.live.refuse(u"in_use"_s, inUse);
        QVERIFY(!silent.view.isAsking());
        QCOMPARE(silent.view.problem(), inUse);
    }

    void aMachineThatWasLateWithItsNoIsSaidAsWhatItSaid()
    {
        const QString inUse = u"Keyboard and mouse of this machine are in use in another window."_s;
        // A machine that was late with its "no": nobody waits for it any more, and the bar
        // says "did not answer". What it said is said in that place when it comes. (It was
        // nobody's, and the bar went on saying that the machine did not answer.)
        // (A machine that says no word by itself: its late no and its one word by hand.)
        Desk refusedLate(Says::NoWord, MachineControl::Pace{20, 40, 100});
        refusedLate.view.take();
        QTRY_VERIFY(!refusedLate.view.isAsking());
        QCOMPARE(refusedLate.view.problem(), didNotAnswer);
        refusedLate.live.refuse(u"in_use"_s, inUse);
        QCOMPARE(refusedLate.view.problem(), inUse);
        QVERIFY(!refusedLate.view.isOn());
        // Its word for the giving back is owed still, and they are not asked for until it has
        // come. (Turned round: this recorded Control as offered at once.)
        QVERIFY(refusedLate.view.isBusy());
        refusedLate.live.refuse({}, {});
        QVERIFY(!refusedLate.view.isBusy());
        QCOMPARE(refusedLate.view.problem(), inUse);
    }

    void whatTheMachineOwesRunsOutByItself()
    {
        // What is owed runs out by itself, after a time that is said (80 ms here; in the
        // program as long again as the machine is given to answer). Asked after that, a "no"
        // without a sentence is this asking's own, and is said as that. (Where the machine said
        // nothing at all, what it owed was owed without end: every later "no" was taken for
        // that, and the asking was said as not answered.)
        // (A machine that says no word by itself, here and for the second one.)
        Desk longAgo(Says::NoWord, MachineControl::Pace{20, 40, 100, 60'000, 3000, 600'000, 1000, 80});
        longAgo.view.take();
        QTRY_VERIFY(!longAgo.view.isAsking());
        longAgo.live.take();
        QTest::qWait(250);
        // (And the screen was told so when it ran out: nobody had to ask.)
        QVERIFY(!longAgo.view.isBusy());
        longAgo.view.take();
        QCOMPARE(longAgo.live.take(), QStringList{u"ask"_s});
        longAgo.live.refuse(u"refused"_s, {});
        QVERIFY(!longAgo.view.isAsking());
        QCOMPARE(longAgo.view.problem(), u"Keyboard and mouse of this machine were not given."_s);
        // And a yes is this asking's own.
        longAgo.live.take();
        longAgo.live.refuse({}, {});
        QTest::qWait(250);
        longAgo.view.take();
        longAgo.live.give();
        QVERIFY(longAgo.view.isOn());
        // (The program's own time for it.)
        QCOMPARE(MachineControl::Pace().owedForMs, 5000);

        // While it is owed, they are not asked for; and a "no" without a sentence is taken for
        // the first of what is owed. Both words have come: asked, and the next "no" is the
        // asking's own. (Turned round: this asked while both were owed, and recorded that the
        // asking went on waiting through two such words.)
        Desk within(Says::NoWord, MachineControl::Pace{20, 40, 100});
        within.view.take();
        QTRY_VERIFY(!within.view.isAsking());
        within.live.take();
        within.view.take();
        QVERIFY(!within.view.isAsking());
        QCOMPARE(within.live.take(), QStringList());
        within.live.refuse({}, {});
        QVERIFY(within.view.isBusy());
        within.live.refuse({}, {});
        QVERIFY(!within.view.isBusy());
        within.view.take();
        QVERIFY(within.view.isAsking());
        within.live.refuse({}, {});
        QVERIFY(!within.view.isAsking());
        QCOMPARE(within.view.problem(), u"Keyboard and mouse of this machine were not given."_s);
    }

    // --- what a sixth reviewer found ---

    void noAskingIsMadeWhileTheMachineOwesAWord()
    {
        // An asking gets no word, and none for its giving back. After its time: "did not
        // answer", and two words are owed for a while. Control is clicked within that while:
        // the machine is not asked. (It was: and its yes could then not be told from the owed
        // one, was given back as "may be an earlier asking's", and armed the same refusal for
        // the asking after it, without end.)
        // (A machine that says no word by itself: its one yes is said by hand.)
        Desk desk(Says::NoWord, owedForAMoment);
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        QTRY_VERIFY(!desk.view.isAsking());
        QCOMPARE(desk.view.problem(), didNotAnswer);
        QCOMPARE(desk.live.take(), letGo);
        QVERIFY(desk.view.isBusy());
        desk.view.take();
        QVERIFY(!desk.view.isAsking());
        QCOMPARE(desk.live.take(), QStringList());
        // The machine says yes within that while. No asking waits, since none could be made:
        // it is nobody's, given back at once, and never taken. Its word for that is owed too.
        desk.live.give();
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.control->isOn());
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.keys.begun, 0);
        QCOMPARE(desk.view.problem(), answeredLate);
        QVERIFY(desk.view.isBusy());
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList());
        // After the time that is owed for, with nothing more said: one click takes them.
        QTRY_VERIFY(!desk.view.isBusy());
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        QVERIFY(desk.view.isAsking());
        desk.live.give();
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.keys.begun, 1);
        QCOMPARE(desk.view.problem(), QString());
    }

    void afterASilentAskingAYesAndAWaitTheNextYesTakesThem()
    {
        // The reviewer's own sequence, as whoever sits there does it: Control; the machine is
        // silent; Control again at once; the machine says yes; wait until Control is offered;
        // Control; yes. That last yes takes keyboard and mouse. (It was refused as "may be an
        // earlier asking's", and so was every yes after it for whoever clicked when the
        // sentence said to.)
        // (A machine that says no word by itself: each yes is said by hand.)
        Desk desk(Says::NoWord, owedForAMoment);
        desk.view.take();
        QTRY_VERIFY(!desk.view.isAsking());
        desk.view.take();
        desk.live.give();
        // (That yes is nobody's: given back, and nothing is held with the bar saying otherwise.)
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.control->isOn());
        QVERIFY(desk.live.take().endsWith(u"give back"_s));
        QTRY_VERIFY(!desk.view.isBusy());
        desk.view.take();
        desk.live.give();
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.keys.begun, 1);
        QCOMPARE(desk.view.problem(), QString());
    }

    void theBarAsksForNoClickThatCannotWork()
    {
        const QString aKey = u"A key was pressed before the machine answered, so keyboard and mouse were not taken."_s;
        // A key was pressed before the machine answered. The machine owes its answer, and
        // Control does nothing until that has come: the bar does not say to click it. It says
        // when it can be clicked. (It said "Click Control again" from the first moment.)
        // (Each word of the machine is said by hand, at the moment it is to come.)
        Desk desk(Says::NoWord);
        desk.view.take();
        desk.view.wentOn(true);
        QVERIFY(desk.view.isBusy());
        QVERIFY2(!desk.view.problem().contains(u"Click Control again"_s), qPrintable(desk.view.problem()));
        QCOMPARE(desk.view.problem(), aKey + askAgainLater);
        // The machine has answered (a no, and its word for the giving back): now it says so.
        desk.live.refuse({}, {});
        QVERIFY(desk.view.isBusy());
        QCOMPARE(desk.view.problem(), aKey + askAgainLater);
        desk.live.refuse({}, {});
        QVERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.problem(), aKey + askAgainNow);
        // And clicking works.
        desk.view.take();
        QVERIFY(desk.view.isAsking());
        QCOMPARE(desk.view.problem(), QString());
        // A reason that says nothing of clicking says the same while a word is owed and after.
        desk.live.refuse(u"in_use"_s, u"In use."_s);
        QVERIFY(desk.view.isBusy());
        QCOMPARE(desk.view.problem(), u"In use."_s);
        desk.live.refuse({}, {});
        QCOMPARE(desk.view.problem(), u"In use."_s);
    }

    void theButtonIsBusyForNoLongerThanIsSaid()
    {
        // The times that are said at the code, with a machine that says no word by itself. (The
        // machine is given 200 ms to answer here, and a word is owed for 300 ms.)
        const MachineControl::Pace pace{20, 40, 200, 60'000, 3000, 600'000, 1000, 300};
        // (How late a timer may be on a PC that is busy with other work: the times above it are
        // held to this, and what tells one rule from another is further apart than this.)
        constexpr int late = 1000;
        QElapsedTimer clock;
        // From a giving back: for as long as a word is owed.
        Desk held(Says::NoWord, pace);
        QVERIFY(held.take());
        clock.start();
        held.view.giveBack();
        QVERIFY(held.view.isBusy());
        QTRY_VERIFY(!held.view.isBusy());
        QVERIFY2(clock.elapsed() >= 290 && clock.elapsed() < 300 + late, qPrintable(QString::number(clock.elapsed())));
        // From an asking that is given up at once: for the time it had to answer and that.
        Desk asked(Says::NoWord, pace);
        clock.restart();
        asked.view.take();
        asked.view.giveBack();
        QVERIFY(asked.view.isBusy());
        QTRY_VERIFY(!asked.view.isBusy());
        QVERIFY2(clock.elapsed() >= 490 && clock.elapsed() < 500 + late, qPrintable(QString::number(clock.elapsed())));
        // And where the machine says something meanwhile, what it still owes is owed from then:
        // it has come as far as that. The asking is given up at once, the machine says its one
        // word a moment later, and the other is owed for 300 ms from there, not for the 2.3 s
        // that an asking with two seconds to answer in would otherwise be owed for. (With 200 ms
        // to answer in, the two were 150 ms apart, and a busy PC was later than that.)
        const MachineControl::Pace patient{20, 40, 2000, 60'000, 3000, 600'000, 1000, 300};
        Desk spoke(Says::NoWord, patient);
        spoke.view.take();
        spoke.view.giveBack();
        clock.restart();
        spoke.live.refuse({}, {});
        QVERIFY(spoke.view.isBusy());
        QTRY_VERIFY(!spoke.view.isBusy());
        QVERIFY2(clock.elapsed() >= 290 && clock.elapsed() < 300 + late, qPrintable(QString::number(clock.elapsed())));
        // Production admission allows the server's 60 seconds plus transport margin.
        QCOMPARE(MachineControl::Pace().answerWithinMs + MachineControl::Pace().owedForMs, 70'000);
    }

    void whyKeysAreHeardNoMoreIsAskedForBeforeTheyAreGivenBackOverAKey()
    {
        const QString notInTime = u"Keys could not be passed on in time. Keyboard and mouse were given back, and "
                                  "every key on the machine was let go."_s;
        // Where the keys are heard they are heard no more, and the word for why is still on its
        // way as a key is told: A, which was down on the machine, going up. This one finds that
        // nothing is swallowed any more, asks why before it makes anything of that, and says
        // the reason. (It took it for the leave-key's doing and gave them back with nothing
        // said; the reason, when it came, found them given back, and was dropped.)
        Desk desk;
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        QCOMPARE(desk.live.take(), QStringList{u"+4"_s});
        desk.keys.fallBehindUnsaid();
        desk.keys.release(keyA);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(), notInTime);
        QCOMPARE(desk.live.take(), (QStringList{u"-4"_s, u"release all"_s, u"give back"_s}));
        // The leave-key's own doing has no reason, and none is said; nor is a reason that comes
        // some time after they were given back, for whatever it is.
        QVERIFY(desk.take());
        desk.keys.press(keyA);
        desk.keys.leaveByItself();
        desk.keys.release(keyA);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.view.problem(), QString());
        emit desk.keys.lostHearing(ui::KeySource::Lost::AnotherDesktop);
        emit desk.keys.fellBehind();
        QCOMPARE(desk.view.problem(), QString());
    }

    void aKeyHeldOnOneKeyboardIsLetGoOnTheMachineByTheSameKeyOnAnother_data()
    {
        QTest::addColumn<bool>("hookFirst");
        QTest::newRow("the hook hears each key first") << true;
        QTest::newRow("Raw Input tells of each key first") << false;
    }

    void aKeyHeldOnOneKeyboardIsLetGoOnTheMachineByTheSameKeyOnAnother()
    {
        QFETCH(bool, hookFirst);
        // (Turned round, and named anew: it was "... is not let go on the machine ...". The
        // telling apart of keyboards, which that came of, is gone by the owner's decision: a
        // key is let go on the machine when any keyboard lets go of it. This is what a person
        // at two keyboards meets, and it is on the list for them.)
        DeskOfTheHook desk(true);
        QVERIFY(desk.take());
        // A key as both accounts tell of it, in the one order or the other. (Which keyboard it
        // is pressed on is said in the comments only: neither account is asked.)
        const auto key = [&](std::uint32_t name, std::uint32_t scanCode, std::uint32_t rawName, bool down) {
            if (hookFirst) {
                desk.keys.hears(name, scanCode, 0, down);
            }
            desk.keys.isToldOf(scanCode, 0, rawName, down);
            if (!hookFirst) {
                desk.keys.hears(name, scanCode, 0, down);
            }
        };
        // Ctrl is held on the first keyboard, and C typed and let go there: Ctrl says itself
        // again no longer.
        key(0xA2, 0x1D, 0x11, true);
        key(0x43, 0x2E, 0x43, true);
        key(0x43, 0x2E, 0x43, false);
        QCOMPARE(desk.live.take(), (QStringList{u"+e0"_s, u"+6"_s, u"-6"_s}));
        // Ctrl is tapped on the second (a macro pad sends modifiers too): its "up" lets Ctrl go
        // on the machine. Nothing stays down there.
        const QSignalSpy nothingIsDown(&desk.keys, &ui::KeySource::nothingIsDown);
        key(0xA2, 0x1D, 0x11, true);
        key(0xA2, 0x1D, 0x11, false);
        QCOMPARE(desk.live.take(), QStringList{u"-e0"_s});
        QCOMPARE(nothingIsDown.count(), 1);
        // The cost, which is known: the finger is still on the first keyboard's Ctrl, and the
        // next key from there arrives bare. Whoever sees it presses the key again.
        key(0x43, 0x2E, 0x43, true);
        key(0x43, 0x2E, 0x43, false);
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
        // That finger leaves the key: nothing more for the machine, and nothing ends.
        key(0xA2, 0x1D, 0x11, false);
        QCOMPARE(desk.live.take(), QStringList());
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.note(), QString());
        // Pressed again, it is down on the machine again, and up by its own "up".
        key(0xA2, 0x1D, 0x11, true);
        key(0x43, 0x2E, 0x43, true);
        key(0x43, 0x2E, 0x43, false);
        key(0xA2, 0x1D, 0x11, false);
        QCOMPARE(desk.live.take(), (QStringList{u"+e0"_s, u"+6"_s, u"-6"_s, u"-e0"_s}));
    }

    void aKeyThatIsKeptBackIsKeptBackNoLongerWhenAnyKeyboardLetsGoOfIt()
    {
        // (Turned round, and named anew. It was "a key that is kept back is kept back no longer
        // when the other account's word lets it go": the key was held on two keyboards, the
        // first one's "up" let nothing go, and the second was unplugged, of which Windows was
        // asked to say so. No keyboard holds a key for itself any more, and the first "up" lets
        // the key go. A keyboard that goes ends the control, whatever was held on it:
        // `aKeyboardThatGoesGivesKeyboardAndMouseBackAndTheBarSaysWhy`.)
        // Shift went down under the leave-key and is the leave-key's until it was let go: kept
        // back from the machine, and so is what is typed under it. It is held on two keyboards,
        // and the first lets go: by that "up" it is kept back no longer, and A is a key.
        DeskOfTheHook desk(true);
        QVERIFY(desk.take());
        const auto typeA = [&desk] {
            desk.keys.hears(0x41, 0x1E, 0, true);
            desk.keys.isToldOf(0x1E, 0, 0x41, true);
            desk.keys.hears(0x41, 0x1E, 0, false);
            desk.keys.isToldOf(0x1E, 0, 0x41, false);
        };
        desk.keys.hears(0xA3, 0x1D, marked, true);
        desk.keys.isToldOf(0x1D, rawMarked, 0x11, true);
        desk.keys.hears(0xA0, 0x2A, 0, true);
        desk.keys.isToldOf(0x2A, 0, 0x10, true);
        desk.keys.hears(0xA0, 0x2A, 0, true);
        desk.keys.isToldOf(0x2A, 0, 0x10, true);
        desk.keys.hears(0xA3, 0x1D, marked, false);
        desk.keys.isToldOf(0x1D, rawMarked, 0x11, false);
        QVERIFY(desk.view.isOn());
        QVERIFY(desk.view.note().contains(u"Left Shift"_s));
        typeA();
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.hears(0xA0, 0x2A, 0, false);
        desk.keys.isToldOf(0x2A, 0, 0x10, false);
        QCOMPARE(desk.view.note(), QString());
        typeA();
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
        // The second keyboard, where a finger is still on the key, says it again: a press like
        // any other by now, and down on the machine.
        desk.keys.hears(0xA0, 0x2A, 0, true);
        desk.keys.isToldOf(0x2A, 0, 0x10, true);
        QCOMPARE(desk.live.take(), QStringList{u"+e1"_s});
        QVERIFY(desk.view.isOn());
    }

    // --- what a seventh reviewer found ---

    void afterEveryEndingControlIsOfferedAgainWithinATurnOfTheLoop_data() { eachEndingOfHoldingOrAsking(); }

    void afterEveryEndingControlIsOfferedAgainWithinATurnOfTheLoop()
    {
        QFETCH(int, ending);
        QFETCH(QString, said);
        QFETCH(QString, saidOnceOffered);
        // A machine that says a word for every asking and every giving back, in the order they
        // were said: as a KVM of this PC's own does. Whatever the holding or the asking ended
        // by, it has said what it owed a turn of the loop later, and Control is offered again.
        // (It is given 100 ms to answer here. A word would be owed for five seconds, as in the
        // program: Control was dead for those after every ending but a plain giving back,
        // since such a KVM said no word for the others.)
        Desk desk(Says::EveryWord, MachineControl::Pace{20, 40, 100});
        if (ending == 7) {
            desk.view.take();
            QTRY_VERIFY(!desk.view.isAsking());
        } else {
            bringAbout(desk, ending);
            // Until it has said so, they are not asked for.
            QVERIFY(desk.view.isBusy());
            QCOMPARE(desk.view.problem(), said);
        }
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.view.isAsking());
        QCoreApplication::processEvents();
        QVERIFY(!desk.view.isBusy());
        // What is said of why it ended is what was said then: the machine's "no" to an asking
        // that was called off from here says nothing new. (Where the asking's time had run
        // out, that "no" came with its sentence into the place of "did not answer".)
        QCOMPARE(desk.view.problem(), saidOnceOffered);
        // One click, one asking, one yes.
        desk.live.take();
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        desk.live.give();
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.view.problem(), QString());
    }

    void whereTheMachineSaysNoWordTheBarSaysWhyControlIsNotOffered_data() { eachEndingOfHoldingOrAsking(); }

    void whereTheMachineSaysNoWordTheBarSaysWhyControlIsNotOffered()
    {
        QFETCH(int, ending);
        QFETCH(QString, said);
        QFETCH(QString, saidMeanwhile);
        QFETCH(QString, saidOnceOffered);
        // A machine that says no word by itself: as a line that has none for a giving back, or
        // for an asking that was called off (a server's may not; a KVM of this PC's own had
        // none for a giving back by a window that held nothing). The time a word is owed for
        // runs out, and Control is not offered meanwhile. Not without a word: once that has
        // lasted a moment, the bar says why, whatever it ended by. (It said nothing, and the
        // button was dead for five seconds.) The times here: 100 ms to answer, a word owed for
        // 1200 ms, said after 60.
        Desk desk(Says::NoWord, MachineControl::Pace{20, 40, 100, 60'000, 3000, 600'000, 1000, 1200, 60});
        if (ending == 7) {
            desk.view.take();
            QTRY_VERIFY(!desk.view.isAsking());
        } else {
            bringAbout(desk, ending);
            // At first only why it ended: a machine that says its words has a moment to say
            // them, and the bar does not change twice at every giving back.
            QCOMPARE(desk.view.problem(), said);
        }
        QVERIFY(desk.view.isBusy());
        QTRY_COMPARE_WITH_TIMEOUT(desk.view.problem(), saidMeanwhile, 2000);
        QVERIFY(desk.view.isBusy());
        // (And the button does nothing meanwhile.)
        desk.live.take();
        desk.view.take();
        QVERIFY(!desk.view.isAsking());
        QCOMPARE(desk.live.take(), QStringList());
        // The time is over: offered again, and nothing more is said of it.
        QTRY_VERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.problem(), saidOnceOffered);
        desk.view.take();
        QCOMPARE(desk.live.take(), QStringList{u"ask"_s});
        desk.live.give();
        QVERIFY(desk.view.isOn());
    }

    void whyControlIsNotOfferedIsSaidOnlyWhereItCanBeOffered()
    {
        const QString lostContact = u"Lost contact with the KVM."_s;
        // The machine's line ends while its keyboard and mouse are held: they are given back,
        // why is said, and a word is owed that will never come. Control is not there at all
        // then (the machine is not had live), and the bar says nothing of when it can be
        // clicked again. (A machine that says no word by itself; a word is owed for 1200 ms
        // here, and said after 60.)
        Desk desk(Says::NoWord, MachineControl::Pace{20, 40, 100, 60'000, 3000, 600'000, 1000, 1200, 60});
        QVERIFY(desk.take());
        desk.live.end(u"unreachable"_s, lostContact);
        QVERIFY(!desk.view.isOn());
        QVERIFY(!desk.view.isPossible());
        QCOMPARE(desk.view.problem(), lostContact);
        QTest::qWait(300);
        QVERIFY(desk.view.isBusy());
        QCOMPARE(desk.view.problem(), lostContact);
        // The line is back within that time: Control is there, and does nothing yet, which is
        // said from here on.
        desk.live.comeOpen();
        QVERIFY(desk.view.isPossible());
        QVERIFY(desk.view.isBusy());
        QVERIFY2(desk.view.problem().startsWith(lostContact + u" The machine has still to confirm"_s),
                 qPrintable(desk.view.problem()));
        QTRY_VERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.problem(), lostContact);
    }

    void whyControlEndedIsSaidWhereItComesBehindAKeyInTheSamePass()
    {
        const QString notInTime = u"Keys could not be passed on in time. Keyboard and mouse were given back, and "
                                  "every key on the machine was let go."_s;
        // Through the real key source's own procedure and telling. A is down on the machine.
        // Raw Input tells of Ctrl going down, which the hook has not heard yet; C is heard, and
        // waits for the two to be at one. The interface thread is held up from here. Almost a
        // second later A is let go, which is handed on past what waits; then Ctrl is heard, the
        // two are at one, and C is handed on behind A's going up. The interface thread is back:
        // in one pass it takes A's going up, which is in time, and C, which has waited too long
        // to be typed. So what it has to say is the key, and behind it why keys are heard no
        // more. Whoever is told of the key finds nothing swallowed any more and asks why: it is
        // told there and then, and gives keyboard and mouse back with that reason. (The reason
        // stood in the list of the pass that was being said, where the asking did not find it:
        // they were given back with nothing said, and the reason, said after the key's telling
        // had returned, found them given back and was dropped.)
        DeskOfTheHook desk(true);
        QVERIFY(desk.take());
        desk.keys.hears(0x41, 0x1E, 0, true);
        desk.keys.isToldOf(0x1E, 0, 0x41, true);
        QCOMPARE(desk.live.take(), QStringList{u"+4"_s});
        desk.keys.isToldOf(0x1D, 0, 0x11, true);
        desk.keys.hears(0x43, 0x2E, 0, true);
        desk.keys.isToldOf(0x2E, 0, 0x43, true);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.isHeldUp();
        desk.keys.later(980);
        desk.keys.hears(0x41, 0x1E, 0, false);
        desk.keys.hears(0xA2, 0x1D, 0, true);
        QVERIFY(desk.view.isOn());
        desk.keys.isBack(20);
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), (QStringList{u"-4"_s, u"release all"_s, u"give back"_s}));
        QCOMPARE(desk.view.problem(), notInTime);
        // (C and Ctrl are down, kept from Windows, and typed into nothing: remembered.)
        QVERIFY(!desk.keys.swallows());
        QCOMPARE(desk.keys.whatIsKept().size(), std::size_t{2});
    }

    void aModifierOfAKeyboardThatDropsOutUnsaidStaysDownOnTheMachineUntilItIsTappedElsewhereOrTheyAreGivenBack()
    {
        // (In the place of two tests, with what they held: "a key that stays down for another
        // keyboard's sake is named in the bar" and "a hold that nothing bears out gives way to
        // a tap of the key on another keyboard". Raw Input's account was kept for each
        // keyboard: a key that one of them held was not let go by a tap on another, the bar
        // said so, and a keyboard that had been silent for three seconds gave its hold up to
        // such a tap. None of that is kept any more.)
        // (Named anew: it was "a modifier of a keyboard that is gone stays down ...". Where
        // Windows says that a keyboard went, that ends the control now:
        // `aKeyboardThatGoesGivesKeyboardAndMouseBackAndTheBarSaysWhy`. This is what is left,
        // and it is on the list for a person at a keyboard: a keyboard that drops out and
        // Windows has no word for it. One that sleeps behind a receiver which stays plugged
        // in; a desk switch that keeps a made-up keyboard at each computer.)
        // Shift is held on such a keyboard as it drops out: no "up" comes for the key, and no
        // word of Windows'. Nothing here knows of it: Shift stays down on the machine, the bar
        // says nothing of it, and what is typed arrives under it.
        const auto typeA = [](DeskOfTheHook& desk) {
            desk.keys.hears(0x41, 0x1E, 0, true);
            desk.keys.isToldOf(0x1E, 0, 0x41, true);
            desk.keys.hears(0x41, 0x1E, 0, false);
            desk.keys.isToldOf(0x1E, 0, 0x41, false);
        };
        DeskOfTheHook desk(true);
        QVERIFY(desk.take());
        desk.keys.hears(0xA0, 0x2A, 0, true);
        desk.keys.isToldOf(0x2A, 0, 0x10, true);
        QCOMPARE(desk.live.take(), QStringList{u"+e1"_s});
        desk.keys.looks(5000);
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.view.note(), QString());
        typeA(desk);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
        // The one way out: Shift is pressed and let go on another keyboard. Its "up" lets it go
        // on the machine, at once and however soon after, and what is typed then is typed bare.
        desk.keys.hears(0xA0, 0x2A, 0, true);
        desk.keys.isToldOf(0x2A, 0, 0x10, true);
        desk.keys.hears(0xA0, 0x2A, 0, false);
        desk.keys.isToldOf(0x2A, 0, 0x10, false);
        QCOMPARE(desk.live.take(), QStringList{u"-e1"_s});
        typeA(desk);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s}));
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.view.problem(), QString());

        // The other: keyboard and mouse are given back, which lets go of everything on the
        // machine. Taken again, the key is remembered as one that was kept from Windows and
        // not heard to go up: kept back, with the bar saying which key it is and what ends
        // that, until it is pressed anew.
        DeskOfTheHook back(true);
        QVERIFY(back.take());
        back.keys.hears(0xA0, 0x2A, 0, true);
        back.keys.isToldOf(0x2A, 0, 0x10, true);
        QCOMPARE(back.live.take(), QStringList{u"+e1"_s});
        back.view.giveBack();
        QCOMPARE(back.live.take(), letGo);
        QVERIFY(back.take());
        QCOMPARE(back.view.note(), heldFromBefore().arg(u"Left Shift"_s));
    }

    void aModifierThatSaidItselfAgainCanBeLetGoAndTappedAgainWithTheHooksThreadBehind()
    {
        // The hook's thread is a little behind, and hears three things of Shift before Raw
        // Input's word for the first of them: a saying of the key, an "up", and the key going
        // down once more. On one keyboard that is Shift held until it says itself again, let
        // go, and tapped again; on two it is Shift held on the first, which goes on saying it,
        // and tapped on the second. Which of the two it is, the hook is not told, and nothing
        // here asks: the "up" lets Shift go on the machine as it is heard, and the key going
        // down once more is a press, which waits for its own word as every press. Nothing
        // ends. (Turned round in the middle: while keyboards were told apart, the "up" let
        // nothing go until Raw Input had said whose it was. Before that, the third of them
        // ended the control, as keys that could not be passed on in time, and went to
        // Windows.)
        DeskOfTheHook desk(true);
        QVERIFY(desk.take());
        desk.keys.hears(0xA0, 0x2A, 0, true);
        desk.keys.isToldOf(0x2A, 0, 0x10, true);
        QCOMPARE(desk.live.take(), QStringList{u"+e1"_s});
        desk.keys.hears(0xA0, 0x2A, 0, true);
        desk.keys.hears(0xA0, 0x2A, 0, false);
        desk.keys.hears(0xA0, 0x2A, 0, true);
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.live.take(), QStringList{u"-e1"_s});
        // Raw Input's words, in the order the keys came in. Its word for the saying is that
        // saying's, and its word for the "up" is of the key as it went up then: neither lets
        // the press go, or lets go of it. Its own word does.
        desk.keys.isToldOf(0x2A, 0, 0x10, true);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.isToldOf(0x2A, 0, 0x10, false);
        QCOMPARE(desk.live.take(), QStringList());
        desk.keys.isToldOf(0x2A, 0, 0x10, true);
        QCOMPARE(desk.live.take(), QStringList{u"+e1"_s});
        QCOMPARE(desk.view.note(), QString());
        // What is typed now is typed under Shift, and the key goes up when it is let go.
        desk.keys.hears(0x41, 0x1E, 0, true);
        desk.keys.isToldOf(0x1E, 0, 0x41, true);
        desk.keys.hears(0x41, 0x1E, 0, false);
        desk.keys.isToldOf(0x1E, 0, 0x41, false);
        desk.keys.hears(0xA0, 0x2A, 0, false);
        desk.keys.isToldOf(0x2A, 0, 0x10, false);
        QCOMPARE(desk.live.take(), (QStringList{u"+4"_s, u"-4"_s, u"-e1"_s}));
        QCOMPARE(desk.view.note(), QString());
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.view.problem(), QString());
    }

    void aKeyHeldFromBeforeTheKeysWereHeardIsLetGoByAnotherKeyboardsTap()
    {
        const QString keptBack = u"Kept back from the machine until it is let go: Left Ctrl. Until then, no other "
                                 "key and no click goes to the machine."_s;
        // What is so, and no mending of it: the rule, of a key that is kept back. Ctrl is down
        // on the first keyboard as keyboard and mouse are taken: Windows' own, kept back until
        // it is let go, and the bar says so. Ctrl is tapped on the second keyboard: a key is
        // let go when any keyboard lets go of it. Told as gone up, it is kept back no longer;
        // the bar's line goes; and the next key goes to the machine bare, with the finger still
        // on the first keyboard's Ctrl. What whoever sits there sees: the line "Kept back from
        // the machine until it is let go: Left Ctrl" goes away at the tap on the other keyboard.
        DeskOfTheHook desk(true, {leftCtrl});
        QVERIFY(desk.take());
        QCOMPARE(desk.view.note(), keptBack);
        const auto typeC = [&desk] {
            desk.keys.hears(0x43, 0x2E, 0, true);
            desk.keys.isToldOf(0x2E, 0, 0x43, true);
            desk.keys.hears(0x43, 0x2E, 0, false);
            desk.keys.isToldOf(0x2E, 0, 0x43, false);
        };
        typeC();
        QCOMPARE(desk.live.take(), QStringList());
        QCOMPARE(desk.view.note(), keptBack);
        desk.keys.hears(0xA2, 0x1D, 0, true);
        desk.keys.isToldOf(0x1D, 0, 0x11, true);
        desk.keys.hears(0xA2, 0x1D, 0, false);
        desk.keys.isToldOf(0x1D, 0, 0x11, false);
        QCOMPARE(desk.view.note(), QString());
        typeC();
        QCOMPARE(desk.live.take(), (QStringList{u"+6"_s, u"-6"_s}));
        QVERIFY(desk.view.isOn());
    }

    // --- what a ninth reviewer found ---

    void aKeyboardThatGoesGivesKeyboardAndMouseBackAndTheBarSaysWhy()
    {
        const QString went = u"Windows reported that a keyboard was taken away from this PC: unplugged, or switched "
                             "to another computer (it counts some mice, headsets and docks as keyboards). A key "
                             "that was held on it would have stayed down on the machine, so keyboard and mouse were "
                             "given back, and every key on the machine was let go."_s;
        // Through the real key source's own procedure. Enter is held, and its keyboard is
        // unplugged, or a desk switch takes it to another computer, or its link is lost: no
        // "up" comes for the key, from either account, and Windows says that a keyboard went.
        // Keyboard and mouse are given back, which lets go of everything on the machine, and
        // the bar says why. (Enter stayed down on the machine, said again there by the machine
        // itself, with the bar saying that keyboard and mouse go to it: until they were given
        // back, the window left the front, or nobody had touched anything for ten minutes.)
        DeskOfTheHook desk(true);
        QVERIFY(desk.take());
        desk.keys.hears(0x0D, 0x1C, 0, true);
        desk.keys.isToldOf(0x1C, 0, 0x0D, true);
        QCOMPARE(desk.live.take(), QStringList{u"+28"_s});
        desk.keys.looks(700);
        QVERIFY(desk.view.isOn());
        desk.keys.aKeyboardGoes();
        QVERIFY(!desk.view.isOn());
        QCOMPARE(desk.live.take(), letGo);
        QCOMPARE(desk.view.problem(), went);
        QVERIFY(!desk.keys.swallows());
        // Which keyboard held Enter is not known, so Enter is not taken to be up: it is known
        // as a key that was kept from Windows and not heard to go up.
        QCOMPARE(desk.keys.whatIsKept().size(), std::size_t{1});
        // Another keyboard goes right after (a dock takes every device with it): nothing more.
        desk.keys.aKeyboardGoes();
        QCOMPARE(desk.view.problem(), went);
        QCOMPARE(desk.live.take(), QStringList());

        // With nothing held at all, the same: no keyboard is told apart, and nothing of one is
        // kept. Any keyboard that goes is enough.
        Desk plain;
        QVERIFY(plain.take());
        plain.keys.aKeyboardGoes();
        QVERIFY(!plain.view.isOn());
        QCOMPARE(plain.live.take(), letGo);
        QCOMPARE(plain.view.problem(), went);

        // Shift was held as the keyboard went. Taken again, with the keyboard plugged in again:
        // the key is kept back as one that was held when they were last given back, and the bar
        // says which key that is and what ends it, until it is let go there.
        Desk held;
        QVERIFY(held.take());
        held.keys.press(leftShift);
        QCOMPARE(held.live.take(), QStringList{u"+e1"_s});
        held.keys.aKeyboardGoes();
        QCOMPARE(held.live.take(), letGo);
        QCOMPARE(held.view.problem(), went);
        QVERIFY(held.take());
        QCOMPARE(held.view.problem(), QString());
        QCOMPARE(held.view.note(), heldFromBefore().arg(u"Left Shift"_s));
        held.keys.press(keyA);
        held.keys.release(keyA);
        QCOMPARE(held.live.take(), QStringList());
        held.keys.release(leftShift);
        QCOMPARE(held.view.note(), QString());
        held.keys.press(keyA);
        held.keys.release(keyA);
        QCOMPARE(held.live.take(), (QStringList{u"+4"_s, u"-4"_s}));

        // While nothing is taken, a keyboard that goes is nothing, and so is the word for it.
        Desk idle;
        idle.keys.aKeyboardGoes();
        emit idle.keys.lostHearing(ui::KeySource::Lost::AKeyboardWent);
        QCOMPARE(idle.view.problem(), QString());
        QVERIFY(!idle.view.isOn());
    }

    void whereThisPcDoesNotSayThatAKeyboardWentTheBarSaysSo()
    {
        const QString notNoticed =
            u"If a keyboard is unplugged while a key is held, that is not noticed on this "
            "PC, and the key stays down on the machine: click Control to give keyboard and mouse back."_s;
        const QString notWatched = u"If Windows stops passing keys to qKVM, that is not noticed on this PC: "
                                   "click Control to give keyboard and mouse back."_s;
        // Windows would not say when a keyboard goes (the asking for that word was refused): a
        // key that is held on a keyboard as it is unplugged stays down on the machine, and
        // nothing here would know. Said while they are taken, as it is said where the keyboard
        // is not watched.
        Desk desk;
        desk.keys.toldOfKeyboardsGoing = false;
        QCOMPARE(desk.view.note(), QString());
        QVERIFY(desk.take());
        QCOMPARE(desk.view.note(), notNoticed);
        // (And so it is there: the keyboard goes with Enter held, and nothing ends.)
        desk.keys.press(enter);
        desk.keys.aKeyboardGoes();
        QVERIFY(desk.view.isOn());
        QCOMPARE(desk.live.take(), QStringList{u"+28"_s});
        // Not said once they are given back.
        desk.view.giveBack();
        QCOMPARE(desk.view.note(), QString());
        desk.keys.release(enter);
        // Where it is known only later (that word goes to the hook of the window that took
        // keyboard and mouse last): said from then on, and no longer once it is had again.
        desk.keys.toldOfKeyboardsGoing = true;
        QVERIFY(desk.take());
        QCOMPARE(desk.view.note(), QString());
        desk.keys.toldOfKeyboardsGoing = false;
        QTRY_COMPARE(desk.view.note(), notNoticed);
        desk.keys.toldOfKeyboardsGoing = true;
        QTRY_COMPARE(desk.view.note(), QString());
        QVERIFY(desk.view.isOn());

        // Through the real key source's own procedure. Where this PC gives no second account
        // of its keyboard at all, there is nothing to ask that word with either: both are said.
        DeskOfTheHook alone(false);
        QVERIFY(alone.take());
        QCOMPARE(alone.view.note(), notWatched + u' ' + notNoticed);
        // And where it gives one, and took the asking: neither.
        DeskOfTheHook both(true);
        QVERIFY(both.take());
        QCOMPARE(both.view.note(), QString());
    }
};

QTEST_GUILESS_MAIN(TestControl)

#include "test_control.moc"
