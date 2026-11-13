#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/input/keys.h>

#include "keys_by_hand.h"
#include "win/keyboard_hook.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using ui::hook::Heard;
using ui::hook::KeyRing;
using ui::hook::LeaveWatch;
using ui::hook::Procedure;
using ui::hook::Verdict;
using ui::hook::WindowsKeys;

namespace {

// Windows' names and numbers for some keys, as it tells a listener of them: its name (virtual
// key), the keyboard's number (scan code) and the mark.
struct Key {
    std::uint32_t name = 0;
    std::uint32_t scanCode = 0;
    std::uint32_t flags = 0;
};
constexpr Key keyA{0x41, 0x1E, 0};
constexpr Key keyC{0x43, 0x2E, 0};
constexpr Key leftCtrl{0xA2, 0x1D, 0};
constexpr Key rightCtrl{0xA3, 0x1D, ui::hook::markedFlag};
constexpr Key del{0x2E, 0x53, ui::hook::markedFlag};
// A key for sound: it has no USB number on the keyboard's page.
constexpr Key louder{0xAF, 0x30, ui::hook::markedFlag};
constexpr Key keyL{0x4C, 0x26, 0};
constexpr Key leftWindows{0x5B, 0x5B, ui::hook::markedFlag};
constexpr Key rightWindows{0x5C, 0x5C, ui::hook::markedFlag};
constexpr Key leftAlt{0xA4, 0x38, 0};
constexpr Key rightAlt{0xA5, 0x38, ui::hook::markedFlag};
// Del on the number block, with Num Lock on.
constexpr Key blockDel{0x6E, 0x53, 0};
// The left Ctrl that Windows makes up before the right Alt of a layout with AltGr.
constexpr Key madeUpCtrl{0xA2, 0x21D, 0};
// 7 and 9 on the number block, as they are named with Num Lock on and with it off.
constexpr Key block7{0x67, 0x47, 0};
constexpr Key block7AsHome{0x24, 0x47, 0};
constexpr Key block9{0x69, 0x49, 0};
constexpr Key block9AsPageUp{0x21, 0x49, 0};

// What Windows adds to the flags of a key that a program typed (LLKHF_INJECTED), and of one that
// a program of lower standing typed (LLKHF_LOWER_IL_INJECTED).
constexpr std::uint32_t injected = 0x10;
constexpr std::uint32_t injectedFromBelow = 0x02;

constexpr Verdict passedOn{false, false};
constexpr Verdict passedOnAndTold{false, true};
constexpr Verdict swallowedAndTold{true, true};
constexpr Verdict swallowedUntold{true, false};

// A procedure that swallows, with the interface thread alive at time 0.
void swallowing(Procedure& procedure)
{
    procedure.begin(0);
    procedure.setSwallowing(true, 0);
}

Verdict press(Procedure& procedure, const Key& key, std::uint64_t now = 0)
{
    return procedure.key(key.name, key.scanCode, key.flags, true, now);
}

Verdict release(Procedure& procedure, const Key& key, std::uint64_t now = 0)
{
    return procedure.key(key.name, key.scanCode, key.flags, false, now);
}

// Everything that waits to be handed on, as USB numbers: positive for down, negative for up.
std::vector<int> heard(Procedure& procedure)
{
    std::vector<int> all;
    while (const std::optional<Heard> next = procedure.next()) {
        const int usage = input::usageOf(next->key);
        all.push_back(next->down ? usage : -usage);
    }
    return all;
}

// The same with whose each press is: "+4" a press that was kept from Windows as the machine's,
// "w+4" one that is Windows', "-4" a key going up.
QStringList told(Procedure& procedure)
{
    QStringList all;
    while (const std::optional<Heard> next = procedure.next()) {
        const QString usage = QString::number(input::usageOf(next->key), 16);
        const QString how = !next->down ? QStringLiteral("-") : QStringLiteral("+");
        all.append((next->down && !next->swallowed ? QStringLiteral("w") : QString()) + how + usage);
    }
    return all;
}

// The keys the procedure has as down, as USB numbers (0: a key that has none).
std::multiset<int> down(const Procedure& procedure)
{
    std::multiset<int> all;
    for (const input::Pressed& key : procedure.whatIsDown()) {
        all.insert(input::usageOf(key));
    }
    return all;
}

// And those of them that it kept from Windows.
std::multiset<int> kept(const Procedure& procedure)
{
    std::multiset<int> all;
    for (const input::Pressed& key : procedure.whatIsKept()) {
        all.insert(input::usageOf(key));
    }
    return all;
}

// Some keys as Windows' other account of the keyboard tells of them (Raw Input): it says "Ctrl"
// where the hook says which one.
constexpr input::Pressed rawA{0x1E, false, 0x41};
constexpr input::Pressed rawC{0x2E, false, 0x43};
constexpr input::Pressed rawCtrl{0x1D, false, 0x11};
constexpr input::Pressed rawEnter{0x1C, false, 0x0D};
constexpr input::Pressed rawShift{0x2A, false, 0x10};
constexpr Key keyEnter{0x0D, 0x1C, 0};
constexpr Key keyB{0x42, 0x30, 0};
constexpr Key leftShift{0xA0, 0x2A, 0};
// Num Lock as it is named by itself and, as far as is known, under Ctrl; and Pause as it is
// named by itself and under Ctrl (Break, with another number and a mark).
constexpr Key keyNumLock{0x90, 0x45, ui::hook::markedFlag};
constexpr Key numLockAsPause{0x13, 0x45, ui::hook::markedFlag};
constexpr Key keyPause{0x13, 0x45, 0};
constexpr Key pauseAsBreak{0x03, 0x46, ui::hook::markedFlag};
constexpr Key keyHome{0x24, 0x47, ui::hook::markedFlag};
// The right Shift as the hook is told of it (with a mark that key never had); and as it would
// be told of were Windows to name it "Shift" with that mark, which is no key's place.
constexpr Key keyRightShift{0xA1, 0x36, ui::hook::markedFlag};
constexpr Key keyRightShiftAsShift{0x10, 0x36, ui::hook::markedFlag};
// The name Windows gives what it cannot name.
constexpr std::uint32_t noName = 0xFF;

// A key as Raw Input tells of it (RAWKEYBOARD): the keyboard's number, its marks, Windows' name.
struct RawKey {
    std::uint32_t makeCode = 0;
    std::uint32_t flags = 0;
    std::uint32_t name = 0;
};

// The key the hook's side makes of that, going down or up; nothing: it is no key of its own.
std::optional<input::Pressed> asRawInputTells(const RawKey& key, bool down)
{
    return ui::hook::fromRawInput(key.makeCode, key.flags | (down ? 0U : ui::hook::rawUpFlag), key.name);
}

// Raw Input tells the procedure of a key, in the form `fromRawInput` makes of what it says.
// True: the interface thread is to be woken. (What is no key of its own is told to nobody.)
bool tellAsRawInput(Procedure& procedure, const RawKey& key, bool down, std::uint64_t now)
{
    const std::optional<input::Pressed> made = asRawInputTells(key, down);
    return made.has_value() && procedure.rawKey(*made, down, now);
}

constexpr RawKey rawRightShift{0x36, 0, 0x10};
constexpr RawKey rawLeftShift{0x2A, 0, 0x10};
constexpr RawKey rawLetterA{0x1E, 0, 0x41};
constexpr RawKey rawLetterB{0x30, 0, 0x42};
constexpr RawKey rawLouder{0x30, ui::hook::rawMarkedFlag, 0xAF};
constexpr RawKey rawHome{0x47, ui::hook::rawMarkedFlag, 0x24};
// The Shift a keyboard makes up around Home under Shift: Shift's number, the mark, no name.
constexpr RawKey rawMadeUpShift{0x2A, ui::hook::rawMarkedFlag, noName};

// The right Ctrl as Raw Input tells of it: "Ctrl", with the mark.
constexpr input::Pressed rawRightCtrl{0x1D, true, 0x11};

// By which of Windows' accounts of the keyboard a test's keys are told. The hook's alone: a PC
// that gives no second account, where nothing waits. Or both, as on every PC with Raw Input: the
// hook hears each key first, or Raw Input tells of it first.
enum class Accounts { HookOnly, HookFirst, RawFirst };

// A keyboard as Windows tells of it: each key to the hook's procedure and, where there is a
// second account, to Raw Input as well, in the one order or the other. What comes back is what
// became of the key: whether it was kept from Windows, and whether the interface thread was
// woken for it, by either of the two.
//
// What it does not show: Raw Input's telling is given here in the hook's own form of the key,
// with the hook's name and mark, and not as `fromRawInput` makes it of what Raw Input says. For
// the keys of which the two tell differently, that form is tried through the gate in
// `theKeysOfWhichTheTwoAccountsTellDifferentlyPassTheGateAsRawInputTellsOfThem`.
struct Keyboard {
    Verdict press(const Key& key, std::uint64_t now = 0) const { return say(key, true, now); }
    Verdict release(const Key& key, std::uint64_t now = 0) const { return say(key, false, now); }
    Verdict say(const Key& key, bool down, std::uint64_t now) const
    {
        const input::Pressed raw = ui::hook::pressedOf(key.name, key.scanCode, key.flags);
        bool woke = false;
        if (accounts == Accounts::RawFirst) {
            woke = procedure->rawKey(raw, down, now);
        }
        Verdict verdict = procedure->key(key.name, key.scanCode, key.flags, down, now);
        if (accounts == Accounts::HookFirst) {
            woke = procedure->rawKey(raw, down, now);
        }
        verdict.tell = verdict.tell || woke;
        return verdict;
    }

    Procedure* procedure = nullptr;
    Accounts accounts = Accounts::HookOnly;
};

// A procedure that swallows, with the interface thread alive at time 0, and with the gate armed
// where there is a second account.
void swallowing(Procedure& procedure, Accounts accounts)
{
    procedure.begin(0);
    if (accounts != Accounts::HookOnly) {
        procedure.rawInputTells();
    }
    procedure.setSwallowing(true, 0);
}

// A procedure that swallows on a PC which is taken not to tell: Raw Input was asked for and
// tells of nothing. A was pressed, waited its moment in vain for that account's word, went on,
// and was let go a second after the start. From there no hearing is noted, until that account
// tells of a key after all.
void takenNotToTell(Procedure& procedure)
{
    swallowing(procedure);
    procedure.rawInputTells();
    static_cast<void>(press(procedure, keyA, 100));
    static_cast<void>(procedure.look(101 + ui::hook::heardWithinMs));
    procedure.stillThere(1000);
    static_cast<void>(release(procedure, keyA, 1000));
    static_cast<void>(told(procedure));
}

// Which window Windows has in front, as a test says it (`Procedure::isFor`): the window a hook is
// for, or another program's.
constexpr std::uintptr_t ownWindow = 0x1010;
constexpr std::uintptr_t anotherWindow = 0x2020;
std::atomic<std::uintptr_t> inFront{ownWindow};
std::uintptr_t windowInFront() noexcept
{
    return inFront.load();
}

// A row for each way: for what has to hold on a PC without a second account and on one with it.
void eachWayOfTheAccounts()
{
    QTest::addColumn<int>("way");
    QTest::newRow("no second account: nothing waits") << static_cast<int>(Accounts::HookOnly);
    QTest::newRow("two accounts, the hook hears first") << static_cast<int>(Accounts::HookFirst);
    QTest::newRow("two accounts, Raw Input tells first") << static_cast<int>(Accounts::RawFirst);
}

// What `whatToTell` made, written short: "lost 3" (hearing, with which reason), "behind", "+4"
// and "w+4" and "-4" (a key, as `told` writes it), "none down", "typed".
QStringList written(const std::vector<ui::hook::Told>& all)
{
    using What = ui::hook::Told::What;
    QStringList said;
    for (const ui::hook::Told& one : all) {
        switch (one.what) {
        case What::Key: {
            const QString usage = QString::number(input::usageOf(one.heard.key), 16);
            const QString how = one.heard.down ? QStringLiteral("+") : QStringLiteral("-");
            said.append((one.heard.down && !one.heard.swallowed ? QStringLiteral("w") : QString()) + how + usage);
            break;
        }
        case What::FellBehind:
            said.append(QStringLiteral("behind"));
            break;
        case What::LostHearing:
            said.append(QStringLiteral("lost %1").arg(static_cast<int>(one.why)));
            break;
        case What::NothingIsDown:
            said.append(QStringLiteral("none down"));
            break;
        case What::TypedByAProgram:
            said.append(QStringLiteral("typed"));
            break;
        }
    }
    return said;
}

QString lost(ui::KeySource::Lost why)
{
    return QStringLiteral("lost %1").arg(static_cast<int>(why));
}

// The ways a program has of typing into Windows, or clicking at it: sending it input, and
// posting or sending a window the message of a key. (The names are put together here, so that
// this file does not hold them.) What is found in the sources under a folder, each as "file:
// what"; and how many files were read.
struct LookedFor {
    QStringList found;
    int read = 0;
};

LookedFor typingIntoWindowsIn(const QString& folder)
{
    const QList<QByteArray> inputs = {QByteArray("Send") + "Input", QByteArray("keybd") + "_event",
                                      QByteArray("mouse") + "_event"};
    const QList<QByteArray> posters = {QByteArray("Post") + "Message", QByteArray("Send") + "Message",
                                       QByteArray("PostThread") + "Message", QByteArray("SendNotify") + "Message",
                                       QByteArray("SendDlgItem") + "Message"};
    const QList<QByteArray> keyMessages = {QByteArray("WM_") + "KEYDOWN",    QByteArray("WM_") + "KEYUP",
                                           QByteArray("WM_") + "SYSKEYDOWN", QByteArray("WM_") + "SYSKEYUP",
                                           QByteArray("WM_") + "CHAR",       QByteArray("WM_") + "SYSCHAR",
                                           QByteArray("WM_") + "UNICHAR"};
    LookedFor looked;
    QDirIterator files(folder, {u"*.h"_s, u"*.cpp"_s, u"*.qml"_s, u"*.py"_s, u"*.ps1"_s}, QDir::Files,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (!file.open(QIODevice::ReadOnly)) {
            looked.found.append(file.fileName() + u": not read"_s);
            continue;
        }
        const QByteArray text = file.readAll();
        ++looked.read;
        for (const QByteArray& way : inputs) {
            if (text.contains(way)) {
                looked.found.append(file.fileName() + u": "_s + QString::fromLatin1(way));
            }
        }
        // A key's message in what a posting is given: from the posting's name to the end of
        // its statement. (Reading such a message, as the hook does, is no posting.)
        for (const QByteArray& poster : posters) {
            for (qsizetype at = text.indexOf(poster); at >= 0; at = text.indexOf(poster, at + 1)) {
                const qsizetype end = text.indexOf(';', at);
                const QByteArray statement = text.mid(at, end < 0 ? -1 : end - at);
                for (const QByteArray& message : keyMessages) {
                    if (statement.contains(message)) {
                        looked.found.append(file.fileName() + u": "_s + QString::fromLatin1(poster + " " + message));
                    }
                }
            }
        }
    }
    return looked;
}

// The events the leave-key's machine is tried with: the leave-key, two other keys and a key that
// has no USB number, each going down and up.
struct Event {
    std::uint16_t usage = 0;
    bool down = false;
};
constexpr std::uint16_t leave = input::usageRightCtrl;
constexpr std::array<Event, 8> events = {
    {{leave, true}, {leave, false}, {0x04, true}, {0x04, false}, {0x05, true}, {0x05, false}, {0, true}, {0, false}}};

// Whether the event at `at` is a key that is only said again: it goes down, and was down already
// (it went down before in the sequence, and not up since).
bool isSaidAgain(const std::vector<Event>& sequence, std::size_t at)
{
    if (!sequence[at].down) {
        return false;
    }
    for (std::size_t before = at; before > 0; --before) {
        if (sequence[before - 1].usage == sequence[at].usage) {
            return sequence[before - 1].down;
        }
    }
    return false;
}

// Whether the leave-key's going up at `at` gives the keyboard back, said the long way round: it
// went down since it last went up (or since the start, where it was not held then), and no other
// key went down since it first did. A key that is only said again is no key going down.
bool givesBack(const std::vector<Event>& sequence, std::size_t at, bool heldAtTheStart)
{
    std::size_t from = 0;
    bool wasUpBefore = false;
    for (std::size_t before = at; before > 0; --before) {
        if (sequence[before - 1].usage == leave && !sequence[before - 1].down) {
            from = before;
            wasUpBefore = true;
            break;
        }
    }
    if (!wasUpBefore && heldAtTheStart) {
        return false;
    }
    bool isDown = false;
    for (std::size_t between = from; between < at; ++between) {
        const Event& event = sequence[between];
        if (event.usage == leave && event.down) {
            isDown = true;
        } else if (isDown && event.usage != 0 && event.usage != leave && event.down &&
                   !isSaidAgain(sequence, between)) {
            return false;
        }
    }
    return isDown;
}

} // namespace

// What Windows' own way of hearing every key does with a key, tried without it: no hook is put in
// by any of this, and no key of this PC is listened for.
class TestKeyboardHook : public QObject {
    Q_OBJECT

private slots:
    void aKeyAsWindowsTellsOfItIsThePhysicalKey()
    {
        const auto usage = [](std::uint32_t name, std::uint32_t scanCode, std::uint32_t flags) {
            return int{input::usageOf(ui::hook::pressedOf(name, scanCode, flags))};
        };
        QCOMPARE(usage(0x41, 0x1E, 0), 0x04);
        // The mark tells right Ctrl from left, and the Enter of the number block from Enter.
        QCOMPARE(usage(0xA2, 0x1D, 0), 0xE0);
        QCOMPARE(usage(0xA3, 0x1D, ui::hook::markedFlag), 0xE4);
        QCOMPARE(usage(0x0D, 0x1C, 0), 0x28);
        QCOMPARE(usage(0x0D, 0x1C, ui::hook::markedFlag), 0x58);
        // What else Windows says in the flags (Alt is down, the key goes up) is nothing to the key.
        QCOMPARE(usage(0x2E, 0x53, ui::hook::markedFlag | 0x20 | 0x80), 0x4C);
        // The left Ctrl that Windows makes up before the right Alt of a layout with AltGr is no key.
        QCOMPARE(usage(0xA2, 0x21D, 0), 0);
        // The right Shift never had the mark, and a listener for every key is told of it with
        // one: it is the right Shift all the same, and not a key that has no USB number.
        QCOMPARE(usage(0xA1, 0x36, 0), 0xE5);
        QCOMPARE(usage(0xA1, 0x36, ui::hook::markedFlag), 0xE5);
        const input::Pressed pressed = ui::hook::pressedOf(0xA3, 0x1D, ui::hook::markedFlag);
        QCOMPARE(pressed.virtualKey, 0xA3U);
        QCOMPARE(pressed.scanCode, 0x1DU);
        QVERIFY(pressed.extended);
    }

    void whatAProgramTypedIsToldApart()
    {
        QVERIFY(ui::hook::isTypedByAProgram(injected));
        QVERIFY(ui::hook::isTypedByAProgram(injectedFromBelow));
        QVERIFY(ui::hook::isTypedByAProgram(injected | injectedFromBelow | ui::hook::markedFlag));
        QVERIFY(!ui::hook::isTypedByAProgram(0));
        QVERIFY(!ui::hook::isTypedByAProgram(ui::hook::markedFlag | 0x20 | 0x80));
    }

    void theProgramIsAliveForASecondAfterItSaidSo()
    {
        QVERIFY(ui::hook::isAlive(5000, 5000));
        QVERIFY(ui::hook::isAlive(5999, 5000));
        QVERIFY(!ui::hook::isAlive(6000, 5000));
        QVERIFY(!ui::hook::isAlive(1'000'000, 5000));
        // Said by the other thread after this one read the time: alive, and not a number that
        // went round.
        QVERIFY(ui::hook::isAlive(4990, 5000));
        // And a key that waited a second to be handed on is late.
        QVERIFY(!ui::hook::isLate(5000, 5000));
        QVERIFY(!ui::hook::isLate(5999, 5000));
        QVERIFY(ui::hook::isLate(6000, 5000));
        QVERIFY(!ui::hook::isLate(4990, 5000));
    }

    void theLeaveKeyByItselfGivesTheKeyboardBack()
    {
        LeaveWatch watch;
        QVERIFY(!watch.take(leave, true));
        QVERIFY(!watch.take(leave, true)); // held: Windows says it again
        QVERIFY(watch.take(leave, false));
        // Let go without having gone down: nothing.
        QVERIFY(!watch.take(leave, false));
        // With another key between, it gives nothing back; and after that it does again.
        QVERIFY(!watch.take(leave, true));
        QVERIFY(!watch.take(0x4C, true));
        QVERIFY(!watch.take(0x4C, false));
        QVERIFY(!watch.take(leave, false));
        QVERIFY(!watch.take(leave, true));
        QVERIFY(watch.take(leave, false));
        // A key that was down before it, and goes up while it is down, is not a key between.
        QVERIFY(!watch.take(0x04, true));
        QVERIFY(!watch.take(leave, true));
        QVERIFY(!watch.take(0x04, false));
        QVERIFY(watch.take(leave, false));
        // Nor is a key without a USB number.
        QVERIFY(!watch.take(leave, true));
        QVERIFY(!watch.take(0, true));
        QVERIFY(watch.take(leave, false));
        // No other key gives anything back.
        QVERIFY(!watch.take(0xE0, true));
        QVERIFY(!watch.take(0xE0, false));
    }

    void aLeaveKeyThatWasHeldFromBeforeGivesNothingBackWhenLetGo()
    {
        LeaveWatch watch(leave, true);
        QVERIFY(!watch.take(leave, true)); // its repeat
        QVERIFY(!watch.take(leave, false));
        // Pressed for it, it does.
        QVERIFY(!watch.take(leave, true));
        QVERIFY(watch.take(leave, false));
    }

    void theLeaveKeysMachineIsRightForEveryShortSequence()
    {
        // Every sequence of up to seven of the eight events, with the leave-key held at the start
        // and not: the machine against the rule said the long way round, and, where the key was
        // not held at the start, against what whoever listens runs (input::ControlKeys). Some
        // 2.4 million sequences each way. (A key that goes down twice with no going up between
        // is said again the second time, and the rule the long way round does not count it as
        // going down. This had every "down" count, said again or not.)
        // Each of the two is told "said again" as it knows it in the program, which is not the
        // same thing: the machine where the keys are heard, of a key that is down in its table
        // (it went down before in the sequence, and not up since); what whoever listens runs,
        // of a key that is down on the machine (it was sent there, and not let go since: a key
        // that went down under the leave-key never was). This had both told the first.
        // What it does not show: `MachineControl` keeps some keys back before it asks
        // `ControlKeys` at all (one that was down when they were taken, one that went to
        // Windows, one under a modifier that is kept back). Such a key going down under the
        // leave-key is a key between for the machine where the keys are heard, and none for
        // `ControlKeys`: there the two differ, and `MachineControl` gives back where the hook
        // would not have. That is its tests' to show (ui/tests/test_control.cpp), not this one's.
        constexpr std::size_t longest = 7;
        qsizetype tried = 0;
        for (const bool heldAtTheStart : {false, true}) {
            std::vector<std::size_t> picked;
            std::vector<Event> sequence;
            // Counted through like a number with eight digits: the next sequence of the same
            // length, or the first of the next length.
            for (std::size_t length = 1; length <= longest; ++length) {
                picked.assign(length, 0);
                for (bool more = true; more;) {
                    sequence.clear();
                    for (const std::size_t pick : picked) {
                        sequence.push_back(events.at(pick));
                    }
                    LeaveWatch watch(leave, heldAtTheStart);
                    input::ControlKeys keys;
                    // The two keys that can be down on the machine: A and B.
                    std::array<bool, 2> onTheMachine = {false, false};
                    for (std::size_t at = 0; at < length; ++at) {
                        const Event& event = sequence[at];
                        const bool again = isSaidAgain(sequence, at);
                        const bool gaveBack = watch.take(event.usage, event.down, again);
                        const bool should =
                            event.usage == leave && !event.down && givesBack(sequence, at, heldAtTheStart);
                        const bool isAOrB = event.usage == 0x04 || event.usage == 0x05;
                        const std::size_t which = event.usage == 0x05 ? 1 : 0;
                        const bool againThere = event.down && isAOrB && onTheMachine.at(which);
                        const input::ControlKeys::Step step = keys.take(event.usage, event.down, againThere);
                        if (isAOrB && step.does == input::ControlKeys::Does::Send) {
                            onTheMachine.at(which) = step.down;
                        }
                        const bool listenerLeaves = step.does == input::ControlKeys::Does::Leave;
                        if (gaveBack != should || (!heldAtTheStart && gaveBack != listenerLeaves)) {
                            QString written;
                            for (const Event& one : sequence) {
                                written += QStringLiteral("%1%2 ").arg(one.usage, 0, 16).arg(one.down ? u'v' : u'^');
                            }
                            QFAIL(qPrintable(QStringLiteral("at %1 of: %2").arg(at).arg(written)));
                        }
                    }
                    ++tried;
                    more = false;
                    for (std::size_t digit = 0; digit < length && !more; ++digit) {
                        if (++picked[digit] < events.size()) {
                            more = true;
                        } else {
                            picked[digit] = 0;
                        }
                    }
                }
            }
        }
        QCOMPARE(tried, qsizetype{2} * (8 + 64 + 512 + 4096 + 32768 + 262144 + 2097152));
    }

    void aPressIsSwallowedAndToldWhereItCanBe()
    {
        WindowsKeys keys;
        QCOMPARE(keys.take(0x41, true, true), swallowedAndTold);
        // Held: said again, and whoever listens knows that it is down already.
        QCOMPARE(keys.take(0x41, true, true), swallowedAndTold);
        QCOMPARE(keys.take(0x41, false, true), swallowedAndTold);
    }

    void aPressThatCannotBeSwallowedIsWindowsAndNobodyElses()
    {
        WindowsKeys keys;
        QCOMPARE(keys.take(0x41, true, false), passedOn);
        // It can be swallowed again while the key is still held: its repeats are kept from
        // Windows, and are no presses for whoever listens.
        QCOMPARE(keys.take(0x41, true, true), swallowedUntold);
        QCOMPARE(keys.take(0x41, true, true), swallowedUntold);
        // What Windows saw go down, it sees go up; and whoever listens hears of every up.
        QCOMPARE(keys.take(0x41, false, true), passedOnAndTold);
        // Pressed anew, it is a press.
        QCOMPARE(keys.take(0x41, true, true), swallowedAndTold);
        // And let go while nothing can be swallowed, it goes up for Windows too: which is nothing
        // to Windows, and told all the same.
        QCOMPARE(keys.take(0x41, false, false), passedOnAndTold);
    }

    void aKeyThatWasDownBeforeIsNotAPressByItsRepeat()
    {
        WindowsKeys keys;
        keys.has(0xA0); // left Shift, held as the listening begins
        QCOMPARE(keys.take(0xA0, true, true), swallowedUntold);
        QCOMPARE(keys.take(0xA0, false, true), passedOnAndTold);
        QCOMPARE(keys.take(0xA0, true, true), swallowedAndTold);
        QCOMPARE(keys.take(0xA0, false, true), swallowedAndTold);
        // Forgotten, nothing is held.
        keys.has(0xA0);
        keys.forget();
        QCOMPARE(keys.take(0xA0, true, true), swallowedAndTold);
        // A name Windows does not have is the name of no key, and breaks nothing.
        keys.has(70'000);
        QCOMPARE(keys.take(70'000, true, true), swallowedUntold);
        QCOMPARE(keys.take(0, false, true), passedOnAndTold);
    }

    void windowsNeverKeepsAKeyThatWasLetGo()
    {
        // Two keys, every sequence of up to six events that fingers can make of them (a key
        // goes down, is said again, goes up), with swallowing possible or not at each, and each
        // key held or not as the listening begins. Windows is shown what is not swallowed, and
        // after every event it has no key as down that no finger is on. A press is told only
        // where it was swallowed and Windows did not have the key, and every up is told.
        constexpr std::array<std::uint32_t, 2> names = {0x41, 0xA0};
        constexpr int longest = 6;
        qsizetype tried = 0;
        for (int heldAtTheStart = 0; heldAtTheStart < 4; ++heldAtTheStart) {
            // Each event is one of eight: which key, down or up, swallowed or not.
            for (int length = 1; length <= longest; ++length) {
                for (int code = 0; code < (1 << (3 * length)); ++code) {
                    WindowsKeys keys;
                    std::array<bool, 2> finger = {(heldAtTheStart & 1) != 0, (heldAtTheStart & 2) != 0};
                    std::array<bool, 2> windows = finger;
                    for (std::size_t key = 0; key < names.size(); ++key) {
                        if (finger.at(key)) {
                            keys.has(names.at(key));
                        }
                    }
                    bool possible = true;
                    for (int at = 0; at < length && possible; ++at) {
                        const int event = (code >> (3 * at)) & 7;
                        const auto key = static_cast<std::size_t>(event & 1);
                        const bool down = (event & 2) != 0;
                        const bool canSwallow = (event & 4) != 0;
                        // (A key that is up does not go up.)
                        if (!down && !finger.at(key)) {
                            possible = false;
                            break;
                        }
                        const bool windowsHadIt = windows.at(key);
                        const Verdict verdict = keys.take(names.at(key), down, canSwallow);
                        finger.at(key) = down;
                        if (!verdict.swallow) {
                            windows.at(key) = down;
                        }
                        QVERIFY2(!verdict.swallow || canSwallow, "swallowed where it could not be");
                        QVERIFY2(down || verdict.tell, "an up that was not told");
                        QVERIFY2(!down || verdict.tell == (verdict.swallow && !windowsHadIt),
                                 "a press told that Windows had, or one that was not swallowed");
                        QVERIFY2(!down || verdict.swallow == canSwallow, "a press passed on that could be swallowed");
                        for (std::size_t one = 0; one < names.size(); ++one) {
                            QVERIFY2(!windows.at(one) || finger.at(one), "Windows keeps a key that was let go");
                        }
                    }
                    tried += possible ? 1 : 0;
                }
            }
        }
        QVERIFY2(tried > 10'000, qPrintable(QString::number(tried)));
    }

    void theRingHandsOnInOrderAndHoldsNoMoreThanItHas()
    {
        KeyRing ring;
        QVERIFY(ring.hasRoom());
        QVERIFY(!ring.take().has_value());
        // Three times round, so that its end is passed.
        std::uint32_t put = 0;
        std::uint32_t taken = 0;
        for (int round = 0; round < 3; ++round) {
            for (std::size_t more = 0; more < KeyRing::places - 1; ++more) {
                QVERIFY(ring.hasRoom());
                QVERIFY(ring.put({{put, (put & 1) != 0, put + 7}, (put & 2) != 0, std::uint64_t{put} * 3}));
                ++put;
            }
            // Full: nothing more is taken in, and nothing that was is lost.
            QVERIFY(!ring.hasRoom());
            QVERIFY(!ring.put({{9999, false, 0}, true, 0}));
            for (std::size_t more = 0; more < KeyRing::places - 1; ++more) {
                const std::optional<Heard> next = ring.take();
                if (!next) {
                    QFAIL("The ring handed nothing on where it held something.");
                }
                QCOMPARE(next->key.scanCode, taken);
                QCOMPARE(next->key.extended, (taken & 1) != 0);
                QCOMPARE(next->key.virtualKey, taken + 7);
                QCOMPARE(next->down, (taken & 2) != 0);
                QCOMPARE(next->atMs, std::uint64_t{taken} * 3);
                ++taken;
            }
            QVERIFY(!ring.take().has_value());
            QVERIFY(ring.hasRoom());
        }
        // Emptied, it holds nothing.
        QVERIFY(ring.put({{1, false, 1}, true, 1}));
        ring.clear();
        QVERIFY(!ring.take().has_value());
        QVERIFY(ring.hasRoom());
    }

    void nothingIsSwallowedUntilItIsAskedFor()
    {
        Procedure procedure;
        procedure.begin(0);
        QVERIFY(!procedure.swallows());
        QCOMPARE(press(procedure, keyA), passedOn);
        // (Nothing is told of it, going up either: while nothing is swallowed a key of this
        // PC's is nobody's to hear of. Turned round: this recorded its going up as told.)
        QCOMPARE(release(procedure, keyA), passedOn);
        QVERIFY(heard(procedure).empty());
        procedure.setSwallowing(true, 0);
        QVERIFY(procedure.swallows());
        QCOMPARE(press(procedure, keyA, 10), swallowedAndTold);
        QCOMPARE(release(procedure, keyA, 20), swallowedAndTold);
        QCOMPARE(heard(procedure), (std::vector<int>{0x04, -0x04}));
        procedure.setSwallowing(false, 0);
        QCOMPARE(press(procedure, keyA, 30), passedOn);
    }

    void aKeyIsHandedOnWithWhenItWasHeard()
    {
        Procedure procedure;
        swallowing(procedure);
        QCOMPARE(press(procedure, rightCtrl, 123), swallowedAndTold);
        const std::optional<Heard> next = procedure.next();
        if (!next) {
            QFAIL("The key that was swallowed was not handed on.");
        }
        QCOMPARE(next->key.scanCode, 0x1DU);
        QVERIFY(next->key.extended);
        QCOMPARE(next->key.virtualKey, 0xA3U);
        QVERIFY(next->down);
        QCOMPARE(next->atMs, std::uint64_t{123});
        QVERIFY(!procedure.next().has_value());
    }

    // (What follows is tried each way: with the gate that waits for Windows' other account not
    // armed, as on a PC that gives none, and armed, with the one account first and the other.
    // The keys are pressed on a `Keyboard`, which tells both where there are two.)

    void aProgramThatHangsStopsSwallowingWithinASecond_data() { eachWayOfTheAccounts(); }

    void aProgramThatHangsStopsSwallowingWithinASecond()
    {
        QFETCH(int, way);
        Procedure procedure;
        swallowing(procedure, static_cast<Accounts>(way));
        const Keyboard keys{&procedure, static_cast<Accounts>(way)};
        QCOMPARE(keys.press(keyA, 999), swallowedAndTold);
        // A second without a word from the interface thread: the keys are this PC's again, and
        // nothing of them is told to anybody. The key that went down for the machine still goes
        // up for it, which is told. (Turned round twice. First this recorded a press that
        // passed to Windows so as told to nobody, and a modifier that went down then was
        // unknown. Then it recorded every key of the PC as told, as Windows' own, with its
        // time, for as long as the interface thread hung: every key into the ring until the
        // program ended, with the swallowing still asked for. Now the swallowing ends with the
        // first such key, for good, and that it did is what the interface thread finds when
        // it is back: it lets go of everything. A modifier that went down meanwhile is
        // Windows', which knows of it at the next taking.)
        QCOMPARE(keys.press(keyC, 1000), passedOn);
        QCOMPARE(keys.release(keyC, 1001), passedOn);
        QCOMPARE(keys.release(keyA, 1002), passedOnAndTold);
        QVERIFY(!procedure.swallows());
        QVERIFY(procedure.isClosed());
        QVERIFY(procedure.takeFellBehind());
        QCOMPARE(told(procedure), (QStringList{u"+4"_s, u"-4"_s}));
        // It says that it is alive again: this hook swallows nothing all the same. The next
        // taking puts a new one in.
        procedure.stillThere(5000);
        QVERIFY(!procedure.setSwallowing(true, 5000));
        QCOMPARE(keys.press(keyA, 5999), passedOn);
        QVERIFY(told(procedure).isEmpty());
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
    }

    void whileTheInterfaceThreadIsSilentNothingIsNotedAndTheHookGoesAfterSomeSeconds_data() { eachWayOfTheAccounts(); }

    void whileTheInterfaceThreadIsSilentNothingIsNotedAndTheHookGoesAfterSomeSeconds()
    {
        QFETCH(int, way);
        const auto accounts = static_cast<Accounts>(way);
        using Next = Procedure::Next;
        constexpr std::uint64_t hung = ui::hook::hungAfterMs;
        constexpr std::uint64_t stays = ui::hook::staysForMs;
        Procedure procedure;
        swallowing(procedure, accounts);
        const Keyboard keys{&procedure, accounts};
        QCOMPARE(keys.press(leftCtrl, 500), swallowedAndTold);
        static_cast<void>(told(procedure));
        // The interface thread hangs, and whoever sits at the PC types on, for seconds: each
        // key is passed on, and none is noted, with its time or without.
        for (std::uint64_t at = 1000; at < 9000; at += 100) {
            QCOMPARE(keys.press(keyA, at), passedOn);
            QCOMPARE(keys.release(keyA, at + 50), passedOn);
        }
        QVERIFY(told(procedure).isEmpty());
        // The swallowing is not "asked for" all that time: it is over, and marked as having
        // fallen behind, for the interface thread to find when it is back.
        QVERIFY(!procedure.swallows());
        QVERIFY(procedure.isClosed());
        QVERIFY(procedure.takeFellBehind());
        // What it had kept from Windows still goes up, and that is told: it is waited for.
        QCOMPARE(keys.release(leftCtrl, 9000), passedOnAndTold);
        QCOMPARE(told(procedure), QStringList{u"-e0"_s});
        // And the hook's own thread takes the hook out once the interface thread has been
        // silent for longer than a hung program is worth waiting for. (Not as a hook that
        // hears no more, with a second account either: what it told of was heard.)
        QCOMPARE(procedure.look(hung - 1).next, Next::GoOn);
        QCOMPARE(procedure.look(hung).next, Next::End);
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotTold());

        // No key comes while it hangs: the same, by the thread's own look, and marked.
        Procedure silent;
        swallowing(silent, accounts);
        QCOMPARE(silent.look(hung - 1).next, Next::GoOn);
        QVERIFY(silent.swallows());
        const Procedure::Looked ended = silent.look(hung);
        QCOMPARE(ended.next, Next::End);
        QVERIFY(ended.wake);
        QVERIFY(!silent.swallows());
        QVERIFY(silent.takeFellBehind());

        // It is back before that, and takes what was marked: the hook stays its time for what
        // it had kept from Windows, as after any ending, and is not taken for a hung one's.
        Procedure back;
        swallowing(back, accounts);
        const Keyboard backKeys{&back, accounts};
        QCOMPARE(backKeys.press(leftCtrl, 500), swallowedAndTold);
        QCOMPARE(backKeys.press(keyA, 1500), passedOn);
        back.stillThere(3000);
        QVERIFY(back.takeFellBehind());
        QCOMPARE(back.look(hung).next, Next::GoOn);
        QCOMPARE(back.look(3000 + hung).next, Next::GoOn);
        QCOMPARE(back.look(1499 + stays).next, Next::GoOn);
        QCOMPARE(back.look(1500 + stays).next, Next::End);

        // While nothing is swallowed the interface thread says nothing, and is not waited for.
        Procedure idle;
        swallowing(idle, accounts);
        QVERIFY(!idle.setSwallowing(false, 100));
        QCOMPARE(idle.look(hung).next, Next::GoOn);
        QCOMPARE(idle.look(5 * hung).next, Next::GoOn);
        QVERIFY(!idle.takeFellBehind());
    }

    void nothingIsSwallowedThatCannotBeTold_data() { eachWayOfTheAccounts(); }

    void nothingIsSwallowedThatCannotBeTold()
    {
        QFETCH(int, way);
        const auto accounts = static_cast<Accounts>(way);
        Procedure procedure;
        swallowing(procedure, accounts);
        const Keyboard keys{&procedure, accounts};
        // Nobody takes anything out: the ring fills with a held key that Windows says again and
        // again.
        for (std::size_t said = 0; said < KeyRing::places - 1; ++said) {
            QCOMPARE(keys.press(keyA), swallowedAndTold);
        }
        QVERIFY(!procedure.takeFellBehind());
        // Full: the next key is this PC's, in each way: nothing is kept from Windows that
        // there is no room to tell of.
        if (accounts == Accounts::HookFirst) {
            // Where it waits for the other account's word, it has a place to wait in: it is
            // told when there is room, as Windows' own, and nothing is lost.
            QCOMPARE(keys.press(keyC), passedOn);
            QVERIFY(!procedure.takeFellBehind());
            QCOMPARE(procedure.onTheirWay().count, std::uint32_t{KeyRing::places});
        } else {
            // It cannot be told, and whoever listens would not know that it is down: they are
            // to let go of everything. (Turned round: this recorded that nothing was said of it.)
            QCOMPARE(keys.press(keyC), passedOnAndTold);
            QVERIFY(procedure.takeFellBehind());
        }
        // A key that goes up now cannot be told, and it may be down on a machine: whoever
        // listens is to let go of everything.
        QCOMPARE(keys.release(keyA), passedOnAndTold);
        QVERIFY(procedure.takeFellBehind());
        QVERIFY(!procedure.takeFellBehind());
        // And the leave-key gives the keyboard back all the same.
        QVERIFY(procedure.swallows());
        keys.press(rightCtrl);
        keys.release(rightCtrl);
        QVERIFY(!procedure.swallows());
        // What had a place to wait in goes out when there is room, in its order, as Windows'
        // own: the key, and the leave-key after it.
        const QStringList all = told(procedure);
        QCOMPARE(all.size(), qsizetype{KeyRing::places - 1});
        QCOMPARE(all.count(u"+4"_s), qsizetype{KeyRing::places - 1});
        static_cast<void>(procedure.look(1));
        QCOMPARE(told(procedure),
                 accounts == Accounts::HookFirst ? (QStringList{u"w+6"_s, u"w+e4"_s, u"-e4"_s}) : QStringList());
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
    }

    void whatAProgramTypesIsPassedOnAndToldToNobody()
    {
        Procedure procedure;
        swallowing(procedure);
        // (The first of them wakes the interface thread, for it to say that such keys reach no
        // machine: `thatAProgramTypedIsSaidOnceAndNothingOfTheKey`. Of the key, nothing.)
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, true, 0), passedOnAndTold);
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, false, 0), passedOn);
        for (const std::uint32_t how : {injected, injectedFromBelow, injected | injectedFromBelow}) {
            QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | how, true, 0), passedOn);
            QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | how, false, 0), passedOn);
        }
        QVERIFY(heard(procedure).empty());
        // A program cannot give the keyboard back by typing the leave-key...
        QCOMPARE(procedure.key(rightCtrl.name, rightCtrl.scanCode, rightCtrl.flags | injected, true, 0), passedOn);
        QCOMPARE(procedure.key(rightCtrl.name, rightCtrl.scanCode, rightCtrl.flags | injected, false, 0), passedOn);
        QVERIFY(procedure.swallows());
        // ...nor keep whoever sits here from doing so, by typing between its going down and up.
        QCOMPARE(press(procedure, rightCtrl), swallowedAndTold);
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, true, 0), passedOn);
        release(procedure, rightCtrl);
        QVERIFY(!procedure.swallows());
    }

    void theLeaveKeyGivesTheKeyboardBackInTheProcedureItself_data() { eachWayOfTheAccounts(); }

    void theLeaveKeyGivesTheKeyboardBackInTheProcedureItself()
    {
        QFETCH(int, way);
        Procedure procedure;
        swallowing(procedure, static_cast<Accounts>(way));
        const Keyboard keys{&procedure, static_cast<Accounts>(way)};
        QCOMPARE(keys.press(keyA), swallowedAndTold);
        QCOMPARE(keys.release(keyA), swallowedAndTold);
        QCOMPARE(keys.press(rightCtrl), swallowedAndTold);
        QVERIFY(procedure.swallows());
        // Its going up: swallowing is off before anybody else hears of the key, and the key is
        // told as any other.
        QCOMPARE(keys.release(rightCtrl), passedOnAndTold);
        QVERIFY(!procedure.swallows());
        QCOMPARE(keys.press(keyA), passedOn);
        QCOMPARE(heard(procedure), (std::vector<int>{0x04, -0x04, 0xE4, -0xE4}));
    }

    void theLeaveKeyGivesBackWhateverElseIsSo_data() { eachWayOfTheAccounts(); }

    void theLeaveKeyGivesBackWhateverElseIsSo()
    {
        QFETCH(int, way);
        const auto accounts = static_cast<Accounts>(way);
        // Where the program hangs.
        Procedure hung;
        swallowing(hung, accounts);
        const Keyboard hungKeys{&hung, accounts};
        hungKeys.press(rightCtrl, 60'000);
        hungKeys.release(rightCtrl, 60'001);
        QVERIFY(!hung.swallows());
        // Where a key is held from before, and one that has no USB number is pressed between.
        Procedure busy;
        busy.begin(0);
        busy.windowsHas(keyA.name);
        if (accounts != Accounts::HookOnly) {
            busy.rawInputTells();
        }
        busy.setSwallowing(true, 0);
        const Keyboard busyKeys{&busy, accounts};
        busyKeys.press(keyA);
        busyKeys.press(rightCtrl);
        busyKeys.press(louder);
        busyKeys.release(louder);
        busyKeys.release(keyA);
        busyKeys.release(rightCtrl);
        QVERIFY(!busy.swallows());
        // Where the other account tells of nothing though it was asked for (the keys wait for
        // its word, and it does not come): the leave-key is decided as it is heard, before it
        // waits for anything.
        Procedure waiting;
        swallowing(waiting, Accounts::HookFirst);
        press(waiting, keyA, 100);
        press(waiting, rightCtrl, 110);
        QVERIFY(waiting.swallows());
        release(waiting, rightCtrl, 120);
        QVERIFY(!waiting.swallows());
        QVERIFY(told(waiting).isEmpty());
    }

    void anotherWindowInFrontEndsTheSwallowingInTheProcedureItself_data() { eachWayOfTheAccounts(); }

    void anotherWindowInFrontEndsTheSwallowingInTheProcedureItself()
    {
        // (The outside review of 2026-11-05, F2.) A machine is controlled, and the interface thread
        // is at other work: Qt's word that its window is no longer in front waits for it. Somebody
        // clicks another program's window meanwhile and types. Windows says on the hook's own
        // thread which window is in front, as each key goes down: from the first key that goes
        // down there, nothing is swallowed, though the interface thread says that it is alive
        // and has not taken the swallowing back. (It was kept from that program until Qt's word
        // came, or until the interface thread was silent for a second.)
        QFETCH(int, way);
        const auto accounts = static_cast<Accounts>(way);
        Procedure procedure;
        swallowing(procedure, accounts);
        inFront = ownWindow;
        procedure.isFor(ownWindow, &windowInFront);
        const Keyboard keys{&procedure, accounts};
        QCOMPARE(keys.press(keyA, 10), swallowedAndTold);
        QCOMPARE(keys.press(leftShift, 20), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+4"_s, u"+e1"_s}));
        // Another window comes in front, and the interface thread has not heard of it.
        inFront = anotherWindow;
        procedure.stillThere(30);
        QVERIFY(procedure.swallows());
        // The next key that goes down is that window's, passed on and told to nobody: here A,
        // which is held for the machine and only said again; then a key pressed there.
        QCOMPARE(keys.press(keyA, 40), passedOn);
        QVERIFY(!procedure.swallows());
        QCOMPARE(keys.press(keyC, 50), passedOn);
        QCOMPARE(keys.release(keyC, 60), passedOn);
        // What was kept from Windows for the machine goes up there all the same.
        QCOMPARE(keys.release(leftShift, 70), passedOnAndTold);
        QCOMPARE(keys.release(keyA, 80), passedOnAndTold);
        QCOMPARE(told(procedure), (QStringList{u"-e1"_s, u"-4"_s}));
        QVERIFY(kept(procedure).empty());
        // Its window is in front again before the interface thread heard of any of it: nothing is
        // swallowed until that takes the keys anew.
        inFront = ownWindow;
        QCOMPARE(keys.press(keyC, 90), passedOn);
        QCOMPARE(keys.release(keyC, 100), passedOn);
        // Taken anew while another window is in front (on a word of Qt's that came late): the
        // first key that goes down ends it again.
        inFront = anotherWindow;
        QVERIFY(procedure.setSwallowing(true, 110));
        QCOMPARE(keys.press(keyC, 120), passedOn);
        QVERIFY(!procedure.swallows());
        QCOMPARE(keys.release(keyC, 130), passedOn);
        QVERIFY(told(procedure).isEmpty());
        // And taken anew with its window in front: its keys are the machine's again.
        inFront = ownWindow;
        QVERIFY(procedure.setSwallowing(true, 140));
        QCOMPARE(keys.press(keyC, 150), swallowedAndTold);
        QCOMPARE(keys.release(keyC, 160), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+6"_s, u"-6"_s}));
        QVERIFY(procedure.swallows());
    }

    void theLeaveKeyWithAnotherKeyGivesNothingBack_data() { eachWayOfTheAccounts(); }

    void theLeaveKeyWithAnotherKeyGivesNothingBack()
    {
        QFETCH(int, way);
        Procedure procedure;
        swallowing(procedure, static_cast<Accounts>(way));
        const Keyboard keys{&procedure, static_cast<Accounts>(way)};
        keys.press(rightCtrl);
        keys.press(del);
        keys.release(del);
        QCOMPARE(keys.release(rightCtrl), swallowedAndTold);
        QVERIFY(procedure.swallows());
        // Whoever listens is told all four, and makes Ctrl+Alt+Del of them.
        QCOMPARE(heard(procedure), (std::vector<int>{0xE4, 0x4C, -0x4C, -0xE4}));
        // The left Ctrl is a key like any other.
        keys.press(leftCtrl);
        keys.release(leftCtrl);
        QVERIFY(procedure.swallows());
    }

    void aLeaveKeyHeldAsTheListeningBeginsIsNotTheWayOutUntilPressedAnew_data() { eachWayOfTheAccounts(); }

    void aChosenReleaseKeyEscapesWithoutTheInterface_data() { eachWayOfTheAccounts(); }

    void aChosenReleaseKeyEscapesWithoutTheInterface()
    {
        QFETCH(int, way);
        const auto accounts = static_cast<Accounts>(way);
        for (const Key key : {Key{0x7B, 0x58, 0}, Key{0x91, 0x46, 0}}) {
            const auto usage = input::usageOf({key.scanCode, false, key.name});
            Procedure procedure;
            procedure.begin(0, ui::hook::windowsWaitsMs, usage);
            procedure.windowsHas(key.name);
            procedure.leaveKeyIsHeld();
            if (accounts != Accounts::HookOnly) {
                procedure.rawInputTells();
            }
            procedure.setSwallowing(true, 0);
            const Keyboard keys{&procedure, accounts};
            keys.press(key);
            keys.release(key);
            QVERIFY(procedure.swallows());
            keys.press(rightCtrl);
            keys.release(rightCtrl);
            QVERIFY(procedure.swallows());
            // Used with Del, the release key remains a remote Ctrl+Alt+Del chord.
            keys.press(key);
            keys.press(del);
            keys.release(del);
            keys.release(key);
            QVERIFY(procedure.swallows());
            keys.press(key);
            keys.release(key);
            QVERIFY(!procedure.swallows());
        }
    }

    void aLeaveKeyHeldAsTheListeningBeginsIsNotTheWayOutUntilPressedAnew()
    {
        QFETCH(int, way);
        const auto accounts = static_cast<Accounts>(way);
        Procedure procedure;
        procedure.begin(0);
        procedure.windowsHas(rightCtrl.name);
        procedure.leaveKeyIsHeld();
        if (accounts != Accounts::HookOnly) {
            procedure.rawInputTells();
        }
        procedure.setSwallowing(true, 0);
        const Keyboard keys{&procedure, accounts};
        // Its repeat is no press of the machine's (it is told as Windows' own), and letting it
        // go gives nothing back: Windows sees it go up.
        QCOMPARE(keys.press(rightCtrl), swallowedAndTold);
        QCOMPARE(keys.release(rightCtrl), passedOnAndTold);
        QVERIFY(procedure.swallows());
        QCOMPARE(keys.press(rightCtrl), swallowedAndTold);
        keys.release(rightCtrl);
        QVERIFY(!procedure.swallows());
        QCOMPARE(told(procedure), (QStringList{u"w+e4"_s, u"-e4"_s, u"+e4"_s, u"-e4"_s}));
    }

    // --- what a reviewer found ---

    void whileKeysAreSwallowedEveryPressIsToldWithWhoseItIs()
    {
        Procedure procedure;
        procedure.begin(0);
        // Ctrl is held as the listening begins: Windows has it as down. (This had Ctrl pass to
        // Windows while the interface thread said nothing for over a second, and the swallowing
        // go on after that. Such a silence ends the swallowing now: a press that is Windows'
        // while keys are swallowed is one of a key that Windows had before.)
        procedure.windowsHas(leftCtrl.name);
        procedure.setSwallowing(true, 0);
        // Said again: kept from Windows, which goes on having it, and told as Windows' own, so
        // that whoever listens knows that it is down.
        QCOMPARE(press(procedure, leftCtrl, 10), swallowedAndTold);
        QCOMPARE(press(procedure, leftCtrl, 15), swallowedAndTold);
        QCOMPARE(press(procedure, keyC, 20), swallowedAndTold);
        QCOMPARE(release(procedure, keyC, 30), swallowedAndTold);
        QCOMPARE(release(procedure, leftCtrl, 40), passedOnAndTold);
        QCOMPARE(told(procedure), (QStringList{u"w+e0"_s, u"w+e0"_s, u"+6"_s, u"-6"_s, u"-e0"_s}));
        // While nothing is asked to be swallowed (the hook is in only until what was kept from
        // Windows was let go), nothing is told of the keys of this PC: not a press, and not the
        // going up of a key that went to Windows. Only the going up of a key that was kept
        // from Windows, which whoever listened waits for.
        QCOMPARE(press(procedure, keyC, 45), swallowedAndTold);
        procedure.setSwallowing(false, 0);
        QCOMPARE(press(procedure, keyA, 50), passedOn);
        QCOMPARE(release(procedure, keyA, 60), passedOn);
        QCOMPARE(release(procedure, keyC, 70), passedOnAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+6"_s, u"-6"_s}));
    }

    void theKeysThatAreDownAreKnownWhateverBecameOfThem()
    {
        Procedure procedure;
        procedure.begin(0);
        QVERIFY(down(procedure).empty());
        // One that passed to Windows, one that was kept from it, one that has no USB number.
        press(procedure, keyA);
        procedure.setSwallowing(true, 0);
        QCOMPARE(press(procedure, leftCtrl), swallowedAndTold);
        press(procedure, louder);
        press(procedure, leftCtrl); // said again: once in the table
        QCOMPARE(down(procedure), (std::multiset<int>{0, 0x04, 0xE0}));
        release(procedure, keyA);
        release(procedure, louder);
        QCOMPARE(down(procedure), (std::multiset<int>{0xE0}));
        // Each as the key it is.
        const std::vector<input::Pressed> held = procedure.whatIsDown();
        QCOMPARE(held.size(), std::size_t{1});
        QCOMPARE(held.front().scanCode, 0x1DU);
        QVERIFY(!held.front().extended);
        QCOMPARE(held.front().virtualKey, 0xA2U);
        // What a program types is no finger on a key, and neither is the Ctrl Windows makes up.
        procedure.key(keyC.name, keyC.scanCode, keyC.flags | injected, true, 0);
        press(procedure, madeUpCtrl);
        QCOMPARE(down(procedure), (std::multiset<int>{0xE0}));
        // Known while nothing is swallowed, too: the key that is let go then.
        procedure.setSwallowing(false, 0);
        press(procedure, keyC);
        QCOMPARE(down(procedure), (std::multiset<int>{0x06, 0xE0}));
        release(procedure, leftCtrl);
        QCOMPARE(down(procedure), (std::multiset<int>{0x06}));
        // Known still after keys were not told in time. (Turned round: this recorded the table
        // as emptied then, and a key that was held was unknown at the next taking.) Nothing is
        // known when it ends.
        procedure.fallBehind(0);
        QCOMPARE(down(procedure), (std::multiset<int>{0x06}));
        press(procedure, keyA);
        procedure.forget();
        QVERIFY(down(procedure).empty());
        press(procedure, keyA);
        procedure.begin(10);
        QVERIFY(down(procedure).empty());
    }

    void whatWindowsTakesTheKeyboardWithEndsTheSwallowingFirst_data()
    {
        QTest::addColumn<QList<int>>("held");
        QTest::addColumn<int>("last");
        QTest::addColumn<bool>("taken");
        // (The keys by their place in `keys` below.)
        QTest::newRow("L under the left Windows key") << QList<int>{0} << 2 << true;
        QTest::newRow("L under the right Windows key") << QList<int>{1} << 2 << true;
        QTest::newRow("L by itself") << QList<int>{} << 2 << false;
        QTest::newRow("L under Ctrl") << QList<int>{3} << 2 << false;
        QTest::newRow("Del under Ctrl and Alt") << QList<int>{3, 5} << 7 << true;
        QTest::newRow("Del under Alt and the right Ctrl") << QList<int>{5, 4} << 7 << true;
        QTest::newRow("Del of the number block under Ctrl and the right Alt") << QList<int>{3, 6} << 8 << true;
        QTest::newRow("Del under AltGr, which Windows makes Ctrl and Alt of") << QList<int>{9, 6} << 7 << true;
        QTest::newRow("Del under Ctrl") << QList<int>{3} << 7 << false;
        QTest::newRow("Del under Alt") << QList<int>{5} << 7 << false;
        QTest::newRow("Del under the leave-key, which is Ctrl+Alt+Del for the machine") << QList<int>{4} << 7 << false;
        QTest::newRow("A under Ctrl and Alt") << QList<int>{3, 5} << 10 << false;
    }

    void whatWindowsTakesTheKeyboardWithEndsTheSwallowingFirst()
    {
        QFETCH(QList<int>, held);
        QFETCH(int, last);
        QFETCH(bool, taken);
        constexpr std::array<Key, 11> keys = {leftWindows, rightWindows, keyL,     leftCtrl,   rightCtrl, leftAlt,
                                              rightAlt,    del,          blockDel, madeUpCtrl, keyA};
        Procedure procedure;
        swallowing(procedure);
        for (const int key : held) {
            press(procedure, keys.at(static_cast<std::size_t>(key)));
        }
        static_cast<void>(told(procedure));
        QVERIFY(!procedure.takeKeysOfWindows());
        const Verdict verdict = press(procedure, keys.at(static_cast<std::size_t>(last)));
        if (!taken) {
            QCOMPARE(verdict, swallowedAndTold);
            QVERIFY(procedure.swallows());
            QVERIFY(!procedure.takeKeysOfWindows());
            return;
        }
        // Windows acts on it whatever is done here, and the keys that are let go on its own
        // screen are heard by nobody: the key is Windows', nothing is swallowed any more, and
        // whoever listens is told to let go of everything before the key could go to a machine.
        // (Told: the interface thread is woken, and finds nothing but the word for why.)
        QCOMPARE(verdict, passedOnAndTold);
        QVERIFY(!procedure.swallows());
        // Said as what it is: these keys were pressed, for whomever, and Windows acts on them.
        // (Turned round: this was said as a screen that Windows showed by itself, which is
        // another thing, and has its own word when it comes.)
        QVERIFY(procedure.takeKeysOfWindows());
        QVERIFY(!procedure.takeKeysOfWindows());
        QVERIFY(!procedure.takeAnotherDesktop());
        QVERIFY(told(procedure).isEmpty());
        // What was held is still known: where it is let go in the hearing of either account,
        // that is told. (Turned round: this recorded the table as emptied.) And this hook
        // swallows nothing ever again.
        QVERIFY(!down(procedure).empty());
        QVERIFY(procedure.isClosed());
        QVERIFY(!procedure.setSwallowing(true, 0));
        QVERIFY(!procedure.swallows());
    }

    void whenWindowsShowsAnotherDesktopThatIsSaidAsWhatItIs()
    {
        Procedure procedure;
        swallowing(procedure);
        press(procedure, leftCtrl);
        procedure.anotherDesktop(0);
        QVERIFY(!procedure.swallows());
        QVERIFY(procedure.takeAnotherDesktop());
        QVERIFY(!procedure.takeAnotherDesktop());
        // (Not as keys that could not be passed on in time.)
        QVERIFY(!procedure.takeFellBehind());
        // Said once, however often Windows says it: on the way there, and on the way back.
        procedure.anotherDesktop(5);
        QVERIFY(!procedure.takeAnotherDesktop());
        // Ctrl is still known to be down (turned round: the table was emptied), and where its
        // going up is heard after all, that is told.
        QCOMPARE(down(procedure), (std::multiset<int>{0xE0}));
        static_cast<void>(told(procedure));
        QCOMPARE(release(procedure, leftCtrl, 10), passedOnAndTold);
        QCOMPARE(told(procedure), QStringList{u"-e0"_s});
        // What Windows says of the session: locked and unlocked, taken to another screen and
        // back, here or from afar. Not signing in and out, which a program does not outlive.
        for (const std::uint32_t change : {1U, 2U, 3U, 4U, 7U, 8U}) {
            QVERIFY2(ui::hook::meansNothingIsHeard(change), qPrintable(QString::number(change)));
        }
        for (const std::uint32_t change : {0U, 5U, 6U, 9U, 100U}) {
            QVERIFY2(!ui::hook::meansNothingIsHeard(change), qPrintable(QString::number(change)));
        }
    }

    void theProgramAsksItselfNothingByTyping()
    {
        // (Turned round: this recorded a key that the program sent itself four times a second,
        // the "up" of a name Windows has left unassigned with a mark of the program's own, as
        // kept from every other program always, and noted. The program sends this PC nothing any
        // more. Such a key, typed by whichever program, is what everything a program types is:
        // passed on, and told to nobody.)
        Procedure procedure;
        procedure.begin(100);
        QCOMPARE(procedure.key(0xE8, 0, injected | 0x80, false, 500), passedOn);
        procedure.setSwallowing(true, 0);
        // (While keys are swallowed, the first key that a program types wakes the interface
        // thread, to say that such keys reach no machine. It is passed on all the same, and
        // nothing of it is told or noted. This had "passed on" and no waking.)
        QCOMPARE(procedure.key(0xE8, 0, injected | 0x80, false, 750), passedOnAndTold);
        QCOMPARE(procedure.key(0xE8, 0, injected | 0x80, false, 1000), passedOn);
        QVERIFY(told(procedure).isEmpty());
        QVERIFY(down(procedure).empty());
    }

    void nothingInTheProgramTypesIntoWindows()
    {
        // The ways Windows has of being typed into, or clicked at, by a program: being sent
        // input, and a window being posted or sent the message of a key. None of them is
        // anywhere in the sources of the tree, in whichever of its folders, the tools and the
        // rigs among them: a program that sends this PC input types into whatever is in
        // front, and keeps the PC from locking. (This looked for three names in six folders.)
        // Nothing of control keeps this PC awake either. One thing in the program does, on
        // purpose, and by no input: `ui/win/display_awake` keeps the display on while a window
        // fills a screen, as a wall on a wall's screen is looked at without anybody touching
        // the PC. Control adds nothing to that: it asks Windows for nothing of the kind.
        QStringList found;
        int read = 0;
        const QDir tree(QString::fromUtf8(QKVM_SOURCES));
        QStringList parts;
        // (Every folder of sources: not what was built, and not what was fetched, which is
        // others' and lies in folders whose names begin with a dot.)
        for (const QString& part : tree.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (part != u"build"_s && !part.startsWith(u'.')) {
                parts.append(part);
            }
        }
        for (const QString& part : {u"base"_s, u"bench"_s, u"core"_s, u"engine"_s, u"input"_s, u"tools"_s, u"ui"_s}) {
            QVERIFY2(parts.contains(part), qPrintable(part));
        }
        for (const QString& part : std::as_const(parts)) {
            const LookedFor looked = typingIntoWindowsIn(tree.filePath(part));
            found += looked.found;
            read += looked.read;
        }
        QVERIFY2(read > 200, qPrintable(QString::number(read)));
        QVERIFY2(found.isEmpty(), qPrintable(found.join(u", "_s)));
    }

    void theLookForTypingIntoWindowsFindsWhatItLooksFor()
    {
        // Sources made up for this, each a way of typing into Windows or none, written into a
        // folder of the test's own (and put together here, so that this file holds no such way).
        const QTemporaryDir folder;
        QVERIFY(folder.isValid());
        QVERIFY(QDir(folder.path()).mkpath(u"deep/er"_s));
        const auto write = [&](const QString& name, const QByteArray& text) {
            QFile file(folder.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
        };
        const QByteArray post = QByteArray("Post") + "MessageW";
        const QByteArray keyDown = QByteArray("WM_") + "KEYDOWN";
        // The message of a key, posted to a window: over several lines, as a formatter sets it.
        QVERIFY(
            write(u"posts.cpp"_s, "void f(HWND w)\n{\n    " + post + "(w,\n        " + keyDown + ", 0x41, 0);\n}\n"));
        QVERIFY(write(u"deep/er/sends.h"_s, QByteArray("Send") + "Message(w, WM_" + "CHAR, 'a', 0);\n"));
        QVERIFY(write(u"deep/thread.cpp"_s, QByteArray("PostThread") + "Message(id, WM_" + "SYSKEYUP, 0, 0);\n"));
        QVERIFY(write(u"input.cpp"_s, QByteArray("Send") + "Input(1, &one, sizeof(INPUT));\n"));
        QVERIFY(write(u"script.py"_s, QByteArray("user32.keybd") + "_event(0x41, 0, 0, 0)\n"));
        // None: a key's message that is read, as the hook reads it; and a posting of another
        // message in a source that also reads a key's, as a window's own procedure may.
        QVERIFY(write(u"reads.cpp"_s, "const bool down = what == " + keyDown + ";\n"));
        QVERIFY(
            write(u"closes.cpp"_s, "case " + keyDown + ":\n    " + post + "(window, WM_CLOSE, 0, 0);\n    break;\n"));
        const LookedFor looked = typingIntoWindowsIn(folder.path());
        QCOMPARE(looked.read, 7);
        QStringList files;
        for (const QString& one : looked.found) {
            files.append(QFileInfo(one.section(u": "_s, 0, 0)).fileName());
        }
        files.sort();
        QCOMPARE(files, (QStringList{u"input.cpp"_s, u"posts.cpp"_s, u"script.py"_s, u"sends.h"_s, u"thread.cpp"_s}));
    }

    void whileNothingIsSwallowedOnlyTheGoingUpOfAKeyThisHookKeptIsTold()
    {
        Procedure procedure;
        procedure.begin(0);
        // A key goes up whose going down this hook never saw: it is in since a moment ago, and
        // Windows did not have the key as down. It is a key of this PC's, and nobody is told.
        QCOMPARE(release(procedure, keyA), passedOn);
        // 7 on the number block goes down while nothing is swallowed, and goes up as Home (Num
        // Lock was pressed between): Windows' table has no key of that name as down, and nobody
        // is told all the same.
        QCOMPARE(press(procedure, block7), passedOn);
        QCOMPARE(release(procedure, block7AsHome), passedOn);
        QVERIFY(told(procedure).isEmpty());
        // Kept from Windows, and let go after the swallowing ended, under whichever name: told,
        // once. And a key that Windows had is not, though it goes up under a name Windows lacks.
        QCOMPARE(press(procedure, block9), passedOn);
        procedure.setSwallowing(true, 0);
        QCOMPARE(press(procedure, block7, 10), swallowedAndTold);
        QCOMPARE(press(procedure, keyC, 11), swallowedAndTold);
        procedure.setSwallowing(false, 0);
        QCOMPARE(release(procedure, block7AsHome, 20), passedOnAndTold);
        QCOMPARE(release(procedure, block7AsHome, 21), passedOn);
        QCOMPARE(release(procedure, block9AsPageUp, 22), passedOn);
        QCOMPARE(release(procedure, keyC, 23), passedOnAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+5f"_s, u"+6"_s, u"-5f"_s, u"-6"_s}));
    }

    void whatWindowsSawGoDownItSeesGoUpUnderWhicheverName_data() { eachWayOfTheAccounts(); }

    void whatWindowsSawGoDownItSeesGoUpUnderWhicheverName()
    {
        QFETCH(int, way);
        const auto accounts = static_cast<Accounts>(way);
        Procedure procedure;
        swallowing(procedure, accounts);
        const Keyboard keys{&procedure, accounts};
        // 7 on the number block goes down while the ring has no room for it: it passes to
        // Windows, as the 7. Shift is pressed, and under it the key is Home: let go, it goes
        // up as Home. Windows saw that key go down, and sees it go up: it is not kept from
        // Windows because its name is another one by now. Nor does Windows go on having a 7.
        // (This had the 7 pass to Windows while the interface thread said nothing for over a
        // second, which ends the swallowing now.)
        for (std::size_t said = 0; said < KeyRing::places - 1; ++said) {
            QCOMPARE(keys.press(keyA), swallowedAndTold);
        }
        // (Where it waits for the other account's word, it has a place to wait in: it is told
        // when there is room, as Windows' own, and nothing is marked as fallen behind.)
        const bool hadAPlaceToWait = accounts == Accounts::HookFirst;
        QCOMPARE(keys.press(block7, 10), hadAPlaceToWait ? passedOn : passedOnAndTold);
        static_cast<void>(told(procedure));
        QCOMPARE(procedure.takeFellBehind(), !hadAPlaceToWait);
        QCOMPARE(keys.release(block7AsHome, 20), passedOnAndTold);
        QCOMPARE(keys.press(block7, 30), swallowedAndTold);
        QCOMPARE(keys.release(block7, 40), swallowedAndTold);
        QCOMPARE(told(procedure), hadAPlaceToWait ? (QStringList{u"w+5f"_s, u"-5f"_s, u"+5f"_s, u"-5f"_s})
                                                  : (QStringList{u"-5f"_s, u"+5f"_s, u"-5f"_s}));
    }

    // (Taken out: "the hook's thread knows when it was kept from its work", which recorded that a
    // beat that came more than 300 ms late ends the swallowing. A thread that only waits has
    // such gaps on a PC that is merely busy, and a gap in which no key came harms nobody. What
    // follows is in its place: a key that came late, by its own time, and a key that never came.)

    void howLongAKeyHasWaitedIsKnownByWhenWindowsStampedIt()
    {
        QCOMPARE(ui::hook::waitedMs(5000, 5000), std::uint64_t{0});
        QCOMPARE(ui::hook::waitedMs(5000, 4700), std::uint64_t{300});
        QCOMPARE(ui::hook::waitedMs(5000, 1), std::uint64_t{4999});
        // The clock goes round every seven weeks: a stamp from just before it did.
        QCOMPARE(ui::hook::waitedMs(40, 0xFFFF'FFF0U), std::uint64_t{56});
        // Not known: a key that has no stamp, and a stamp from after now (the two were read a
        // moment apart). Such a key has not waited.
        QCOMPARE(ui::hook::waitedMs(5000, 0), std::uint64_t{0});
        QCOMPARE(ui::hook::waitedMs(5000, 5003), std::uint64_t{0});
        QCOMPARE(ui::hook::waitedMs(3, 0xFFFF'FFF0U + 20), std::uint64_t{0});
    }

    void aKeyThatComesAfterWindowsStoppedWaitingForItIsWindowsAndEndsTheSwallowing()
    {
        Procedure procedure;
        swallowing(procedure);
        // The hook's thread was kept from its work, and no key came meanwhile: nothing of it.
        // A key that waited, and not for nearly as long as Windows waits: the machine's.
        procedure.stillThere(5000);
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags, true, 5000, 249), swallowedAndTold);
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeFellBehind());
        // One that waited longer may have gone to Windows by now, whatever is said of it here:
        // it is not kept, it is not the machine's, and nothing more is swallowed. Whoever
        // listens is told that keys could not be passed on in time, and lets go of everything.
        // (Not only from the 300 ms on that Windows waits: a margin is kept under them. This
        // had 299 as in time; a "down" that Windows took at 300 and an "up" that was kept
        // from it a moment later are a key stuck on this PC.)
        QCOMPARE(procedure.key(keyC.name, keyC.scanCode, keyC.flags, true, 5400, 250), passedOnAndTold);
        QVERIFY(!procedure.swallows());
        QVERIFY(procedure.takeFellBehind());
        QVERIFY(procedure.isClosed());
        QCOMPARE(told(procedure), (QStringList{u"+4"_s, u"w+6"_s}));
        // What goes up after that: A, which was kept from Windows, is told; C, Windows' own, not.
        QCOMPARE(procedure.key(keyC.name, keyC.scanCode, keyC.flags, false, 5450, 0), passedOn);
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags, false, 5460, 0), passedOnAndTold);
        QCOMPARE(told(procedure), QStringList{u"-4"_s});
        // While nothing is swallowed a key that waited is a key of this PC's as any other.
        Procedure idle;
        idle.begin(0);
        QCOMPARE(idle.key(keyA.name, keyA.scanCode, keyA.flags, true, 9000, 5000), passedOn);
        QVERIFY(!idle.takeFellBehind());
        QVERIFY(!idle.isClosed());
    }

    void aKeyAsRawInputTellsOfItIsTheKeyTheHookTellsOf()
    {
        using ui::hook::fromRawInput;
        using ui::hook::rawMarkedFlag;
        using ui::hook::rawPauseFlag;
        using ui::hook::rawUpFlag;
        const auto usage = [](std::uint32_t makeCode, std::uint32_t flags, std::uint32_t name) {
            const std::optional<input::Pressed> key = fromRawInput(makeCode, flags, name);
            return key.has_value() ? int{input::usageOf(*key)} : -1;
        };
        // By the keyboard's number and its mark, whatever Raw Input calls the key: it says
        // "Shift" and "Ctrl" where the hook says which one.
        QCOMPARE(usage(0x1E, 0, 0x41), 0x04);
        QCOMPARE(usage(0x1E, rawUpFlag, 0x41), 0x04);
        QCOMPARE(usage(0x2A, 0, 0x10), 0xE1);
        QCOMPARE(usage(0x36, 0, 0x10), 0xE5);
        QCOMPARE(usage(0x1D, 0, 0x11), 0xE0);
        QCOMPARE(usage(0x1D, rawMarkedFlag, 0x11), 0xE4);
        QCOMPARE(usage(0x1C, rawMarkedFlag | rawUpFlag, 0x0D), 0x58);
        QCOMPARE(usage(0x53, rawMarkedFlag, 0x2E), 0x4C);
        // Pause comes in two halves, the first with a mark of its own and the number of Ctrl:
        // that one is Pause, and the second half is nothing.
        QCOMPARE(usage(0x1D, rawPauseFlag, 0x13), 0x48);
        QCOMPARE(usage(0x45, 0, 0xFF), -1);
        QCOMPARE(usage(0x1D, rawPauseFlag, 0x11), -1);
        // Num Lock, which the hook is told of with the mark and Raw Input without: one key,
        // down in one place of the table.
        const std::optional<input::Pressed> numLock = fromRawInput(0x45, 0, 0x90);
        QVERIFY(numLock.has_value());
        const input::Pressed asTheHookHasIt = ui::hook::pressedOf(0x90, 0x45, ui::hook::markedFlag);
        QCOMPARE(numLock.value_or(input::Pressed{}).scanCode, asTheHookHasIt.scanCode);
        QCOMPARE(numLock.value_or(input::Pressed{}).extended, asTheHookHasIt.extended);
        QCOMPARE(usage(0x45, 0, 0x90), 0x53);
        // The right Shift, the other way round: the hook's comes with a mark the key never had.
        const input::Pressed rightShift = ui::hook::pressedOf(0xA1, 0x36, ui::hook::markedFlag);
        QCOMPARE(fromRawInput(0x36, 0, 0x10).value_or(input::Pressed{}).extended, rightShift.extended);
        // No key of its own: the Shift a keyboard makes up around some keys, a keyboard that
        // says it lost count, and a number no key has.
        QCOMPARE(usage(0x2A, rawMarkedFlag, 0xFF), -1);
        QCOMPARE(usage(0xFF, 0, 0xFF), -1);
        QCOMPARE(usage(0, 0, 0x41), -1);
        QCOMPARE(usage(0x21D, 0, 0xA2), -1);
    }

    void theTwoAccountsOfTheKeyboardAreHeldAgainstEachOther()
    {
        using ui::hook::TwoAccounts;
        const input::Pressed a{0x1E, false, 0x41};
        const input::Pressed c{0x2E, false, 0x43};
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // Heard, and then told of: they agree, however long one looks.
        TwoAccounts both;
        both.heard(a, true, 100);
        both.told(a, true, 101);
        QVERIFY(!both.isBehind(101));
        QVERIFY(!both.isBehind(1'000'000));
        // Told of first, and heard a little later: the same.
        both.told(a, false, 200);
        QVERIFY(!both.isBehind(200 + moment));
        both.heard(a, false, 200 + moment);
        QVERIFY(!both.isBehind(1'000'000));
        // Told of, and never heard: for a moment that may yet come, and after it the hook no
        // longer hears.
        both.told(c, true, 5000);
        QVERIFY(!both.isBehind(5000));
        QVERIFY(!both.isBehind(5000 + moment));
        QVERIFY(both.isBehind(5001 + moment));
        // Forgotten, nothing waits.
        both.forget();
        QVERIFY(!both.isBehind(1'000'000));

        // An "up" that Windows left out. A was pressed and let go long ago, heard both times;
        // it is pressed again, and of its going up only Raw Input tells.
        TwoAccounts lostUp;
        for (const bool down : {true, false, true}) {
            lostUp.heard(a, down, down ? 9000 : 100);
            lostUp.told(a, down, down ? 9000 : 100);
        }
        lostUp.told(a, false, 9100);
        QVERIFY(!lostUp.isBehind(9100 + moment));
        QVERIFY(lostUp.isBehind(9101 + moment));
        // The same of a key that is tapped twice in one moment: each telling needs a hearing of
        // its own, and the first tap's is not taken for the second's.
        TwoAccounts twice;
        twice.heard(a, false, 100);
        twice.told(a, false, 110);
        twice.told(a, false, 300);
        QVERIFY(!twice.isBehind(300 + moment));
        QVERIFY(twice.isBehind(301 + moment));
        // Another key's hearing is not this key's, and neither is the same key going the other way.
        TwoAccounts other;
        other.heard(c, true, 100);
        other.heard(a, false, 100);
        other.told(a, true, 100);
        QVERIFY(other.isBehind(101 + moment));

        // A held key is said again and again. The thread was kept from its work for a moment,
        // and hears the hook twice before it reads what Raw Input told: they agree.
        TwoAccounts held;
        held.heard(a, true, 100);
        held.heard(a, true, 133);
        held.heard(a, true, 166);
        held.told(a, true, 167);
        held.told(a, true, 167);
        held.told(a, true, 168);
        QVERIFY(!held.isBehind(1'000'000));
        // And the other way round.
        held.told(a, true, 2000);
        held.told(a, true, 2033);
        held.heard(a, true, 2034);
        held.heard(a, true, 2034);
        QVERIFY(!held.isBehind(1'000'000));
        // Held while Windows takes the hook out: Raw Input goes on telling of it, and a moment
        // after the first telling that was not heard it is known.
        held.heard(a, true, 3000);
        held.told(a, true, 3000);
        for (std::uint64_t at = 3033; at < 3033 + 2 * moment; at += 33) {
            held.told(a, true, at);
        }
        QVERIFY(!held.isBehind(3033 + moment));
        QVERIFY(held.isBehind(3034 + moment));

        // A key that is heard and never told of says nothing: a keyboard Raw Input does not tell
        // of (over a remote desktop there may be none). And such a hearing does not wait for
        // ever to be taken for another's.
        TwoAccounts alone;
        for (std::uint64_t at = 0; at < 10'000; at += 50) {
            alone.heard(a, (at / 50) % 2 == 0, at);
            QVERIFY(!alone.isBehind(at));
        }
        alone.told(a, false, 20'000);
        QVERIFY(alone.isBehind(20'001 + moment));
    }

    void theTwoAccountsKnowAKeyUnderWhicheverNameAndMark()
    {
        using ui::hook::fromRawInput;
        using ui::hook::pressedOf;
        using ui::hook::TwoAccounts;
        // The hook's telling of a key, and Raw Input's of the same one.
        struct Pair {
            const char* key;
            input::Pressed hooked;
            std::optional<input::Pressed> raw;
        };
        const std::array<Pair, 9> pairs = {{
            {"left Shift", pressedOf(0xA0, 0x2A, 0), fromRawInput(0x2A, 0, 0x10)},
            {"right Shift", pressedOf(0xA1, 0x36, ui::hook::markedFlag), fromRawInput(0x36, 0, 0x10)},
            {"right Ctrl", pressedOf(0xA3, 0x1D, ui::hook::markedFlag),
             fromRawInput(0x1D, ui::hook::rawMarkedFlag, 0x11)},
            {"Num Lock", pressedOf(0x90, 0x45, ui::hook::markedFlag), fromRawInput(0x45, 0, 0x90)},
            {"Num Lock, were the hook told of it without the mark", pressedOf(0x90, 0x45, 0),
             fromRawInput(0x45, 0, 0x90)},
            {"Pause", pressedOf(0x13, 0x45, 0), fromRawInput(0x1D, ui::hook::rawPauseFlag, 0x13)},
            {"Print Screen", pressedOf(0x2C, 0x37, ui::hook::markedFlag),
             fromRawInput(0x37, ui::hook::rawMarkedFlag, 0x2C)},
            // (What Windows calls a key by what else is held: the two change the name in step.)
            {"Num Lock under Ctrl, which both call Pause", pressedOf(0x13, 0x45, ui::hook::markedFlag),
             fromRawInput(0x45, 0, 0x13)},
            {"Pause under Ctrl, which is Break with another number", pressedOf(0x03, 0x46, ui::hook::markedFlag),
             fromRawInput(0x46, ui::hook::rawMarkedFlag, 0x03)},
        }};
        for (const Pair& pair : pairs) {
            QVERIFY2(pair.raw.has_value(), pair.key);
            TwoAccounts both;
            both.told(pair.raw.value_or(input::Pressed{}), true, 100);
            both.heard(pair.hooked, true, 105);
            both.heard(pair.hooked, false, 200);
            both.told(pair.raw.value_or(input::Pressed{}), false, 201);
            QVERIFY2(!both.isBehind(1'000'000), pair.key);
            // (And it is compared at all: told of alone, it is missed.)
            both.told(pair.raw.value_or(input::Pressed{}), true, 5000);
            QVERIFY2(both.isBehind(5001 + ui::hook::heardWithinMs), pair.key);
        }
        // A key that has no USB number of the keyboard's page is not compared at all: louder,
        // which has the keyboard's number of B with the mark. Heard, nothing of it is noted;
        // told of alone, it is not missed; and it is not B. (Turned round: this had it among
        // the pairs above, compared and missed like any key. It was compared as B, read with
        // the mark turned, which is what is gone.)
        const input::Pressed hookedLouder = pressedOf(0xAF, 0x30, ui::hook::markedFlag);
        const input::Pressed toldLouder = fromRawInput(0x30, ui::hook::rawMarkedFlag, 0xAF).value_or(input::Pressed{});
        TwoAccounts sound;
        QCOMPARE(sound.heard(hookedLouder, true, 100), TwoAccounts::Compared::No);
        QVERIFY(sound.notesNoHearing());
        QVERIFY(!sound.told(toldLouder, true, 5000));
        QVERIFY(!sound.isBehind(5001 + ui::hook::heardWithinMs));
        QVERIFY(!TwoAccounts::isSameKey(hookedLouder, pressedOf(0x42, 0x30, 0)));
        QVERIFY(!TwoAccounts::isSameKey(hookedLouder, toldLouder));
        // The left Ctrl Windows makes up, where Raw Input says nothing of it: nothing waits.
        TwoAccounts madeUp;
        madeUp.heard(pressedOf(0xA2, 0x21D, 0), true, 100);
        madeUp.heard(pressedOf(0xA2, 0x21D, 0), false, 300);
        QVERIFY(!madeUp.isBehind(1'000'000));
        // And it is no key to this comparing: a left Ctrl that Raw Input tells of about then is
        // a real key's, and the made-up one's hearing does not take its telling. (Turned round:
        // this had a pair of the two, the made-up Ctrl noted as the left Ctrl "were Raw Input
        // to tell of it". It does not, and that hearing took the telling of a real left Ctrl
        // which the hook was not called for.)
        QCOMPARE(madeUp.heard(pressedOf(0xA2, 0x21D, 0), true, 400), TwoAccounts::Compared::No);
        madeUp.told(fromRawInput(0x1D, 0, 0x11).value_or(input::Pressed{}), true, 410);
        QVERIFY(madeUp.isBehind(411 + ui::hook::heardWithinMs));
    }

    void aKeyThatRawInputToldOfAndTheHookDidNotHearEndsTheSwallowing()
    {
        using Next = Procedure::Next;
        const input::Pressed a{0x1E, false, 0x41};
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        Procedure procedure;
        swallowing(procedure);
        // (Raw Input is told to this hook, as it is wherever it tells of a key at all: said, as
        // the hook's thread says it when it has asked for that account. This left it unsaid,
        // and leaned on a hearing being noted where no second account was known of, which it
        // is no longer. So A waits for the other account's word, and goes with it.)
        procedure.rawInputTells();
        // Both accounts tell of A: nothing of it, however long.
        QCOMPARE(press(procedure, keyA, 100), swallowedUntold);
        QVERIFY(procedure.rawKey(a, true, 100));
        QCOMPARE(procedure.look(100 + 5 * moment).next, Next::GoOn);
        // (Taken out of here, and turned round in `whatRawInputWillNotTellOfTakesNoTelling`: that
        // the hearing of a key which a program typed counts for a telling of that key. It could
        // take the telling that belongs to a real press which the hook was not called for.)
        // Raw Input tells that A went up, and the hook's procedure is not called for it: Windows
        // has taken the hook out, or left this key out. For a moment it may yet come. After it:
        // nothing more is swallowed, and whoever listens is told that Windows hands the program
        // its keys no more. (The interface thread is alive all the while, and says so.)
        procedure.stillThere(19'900);
        QVERIFY(!procedure.rawKey(a, false, 20'000));
        const Procedure::Looked soon = procedure.look(20'000 + moment);
        QCOMPARE(soon.next, Next::GoOn);
        QVERIFY(!soon.wake);
        QVERIFY(procedure.swallows());
        const Procedure::Looked later = procedure.look(20'001 + moment);
        QCOMPARE(later.next, Next::GoOn);
        QVERIFY(later.wake);
        QVERIFY(!procedure.swallows());
        QVERIFY(procedure.isClosed());
        QVERIFY(procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeFellBehind());
        QVERIFY(!procedure.takeAnotherDesktop());
        // Said once; and this hook swallows nothing ever again, whoever asks.
        QVERIFY(!procedure.look(20'100 + moment).wake);
        QVERIFY(!procedure.setSwallowing(true, 20'200 + moment));
        QCOMPARE(press(procedure, keyC, 20'300 + moment), passedOn);

        // Where there is no Raw Input (over a remote desktop there may be none), this says
        // nothing, and ends nothing.
        Procedure remote;
        swallowing(remote);
        for (std::uint64_t at = 0; at < 30'000; at += 100) {
            remote.stillThere(at);
            QCOMPARE(remote.key(keyA.name, keyA.scanCode, keyA.flags, (at / 100) % 2 == 0, at), swallowedAndTold);
            QCOMPARE(remote.look(at).next, Next::GoOn);
            static_cast<void>(told(remote));
        }
        QVERIFY(remote.swallows());

        // Found while nothing is swallowed (the hook is still in for a key to be let go): nobody
        // is told, and the next taking puts a new hook in.
        // (Raw Input is told to this hook from the start, as above: left unsaid, the comparing
        // began at the telling below, which says nothing of the hook any more where it is the
        // first of all.)
        Procedure idle;
        swallowing(idle);
        idle.rawInputTells();
        QVERIFY(idle.setSwallowing(false, 50) == false);
        QVERIFY(!idle.rawKey(a, true, 100));
        const Procedure::Looked found = idle.look(101 + moment);
        QCOMPARE(found.next, Next::GoOn);
        QVERIFY(!found.wake);
        QVERIFY(!idle.takeNotHanded());
        QVERIFY(idle.isClosed());
    }

    void whileNoHookHearsRawInputTellsOfWhatWasKeptFromWindowsGoingUp()
    {
        const input::Pressed ctrl{0x1D, false, 0x11};
        const input::Pressed a{0x1E, false, 0x41};
        const input::Pressed c{0x2E, false, 0x43};
        Procedure procedure;
        procedure.begin(0);
        procedure.windowsHas(keyC.name);
        procedure.setSwallowing(true, 0);
        // Ctrl and A were kept from Windows; C is Windows' own, held since before. (This had C
        // pass to Windows while the program was held up, which ends the swallowing now.)
        QCOMPARE(press(procedure, leftCtrl, 100), swallowedAndTold);
        QCOMPARE(press(procedure, keyA, 110), swallowedAndTold);
        QCOMPARE(press(procedure, keyC, 120), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+e0"_s, u"+4"_s, u"w+6"_s}));
        QCOMPARE(kept(procedure), (std::multiset<int>{0x04, 0xE0}));
        // Keys are not told any more, and this hook is closed (Windows may have taken it out by
        // now). What was down is still known.
        procedure.fallBehind(2100);
        QVERIFY(procedure.takeFellBehind());
        QCOMPARE(procedure.look(2200).next, Procedure::Next::GoOn);
        // Raw Input tells of each of them going up: of those that were kept from Windows whoever
        // listens is told, once; of the one that was Windows' own, and of a key going down, not.
        QVERIFY(!procedure.rawKey(c, false, 2300));
        QVERIFY(!procedure.rawKey(c, true, 2310));
        QVERIFY(procedure.rawKey(ctrl, false, 2320));
        QVERIFY(!procedure.rawKey(ctrl, false, 2330));
        QVERIFY(procedure.rawKey(a, false, 2340));
        QCOMPARE(told(procedure), (QStringList{u"-e0"_s, u"-4"_s}));
        QVERIFY(!procedure.takeFellBehind());
        QVERIFY(kept(procedure).empty());
        // (And where the hook is still in and hears it first, Raw Input's word for it is nothing more.)
        Procedure both;
        swallowing(both);
        QCOMPARE(press(both, leftCtrl, 100), swallowedAndTold);
        QVERIFY(!both.setSwallowing(false, 200));
        QCOMPARE(release(both, leftCtrl, 300), passedOnAndTold);
        QVERIFY(!both.rawKey(ctrl, false, 300));
        QCOMPARE(told(both), (QStringList{u"+e0"_s, u"-e0"_s}));
        // (And where Raw Input's word comes first while the hook is still in: told at once by
        // that word, and the hook's hearing of it is nothing more.)
        Procedure rawFirst;
        swallowing(rawFirst);
        QCOMPARE(press(rawFirst, leftCtrl, 100), swallowedAndTold);
        QVERIFY(!rawFirst.setSwallowing(false, 200));
        QVERIFY(rawFirst.rawKey(ctrl, false, 300));
        QCOMPARE(release(rawFirst, leftCtrl, 300), passedOn);
        QCOMPARE(told(rawFirst), (QStringList{u"+e0"_s, u"-e0"_s}));
        QVERIFY(kept(rawFirst).empty());
    }

    void whatWindowsTakesTheKeyboardWithIsKnownByRawInputsWordToo()
    {
        const input::Pressed rawDel{0x53, true, 0x2E};
        Procedure procedure;
        swallowing(procedure);
        QCOMPARE(press(procedure, leftCtrl, 100), swallowedAndTold);
        QCOMPARE(press(procedure, leftAlt, 110), swallowedAndTold);
        // Del goes down, and Windows shows its own screen before the hook is called for it:
        // Raw Input's word is enough.
        QVERIFY(procedure.rawKey(rawDel, true, 120));
        QVERIFY(!procedure.swallows());
        QVERIFY(procedure.takeKeysOfWindows());
        QVERIFY(!procedure.takeAnotherDesktop());
        QVERIFY(!procedure.takeNotHanded());
        // (Not a moment later as a hook that hears no more: it is closed, and nothing waits.)
        QVERIFY(!procedure.look(120 + 5 * ui::hook::heardWithinMs).wake);
        QVERIFY(!procedure.takeNotHanded());
    }

    void onceTheSwallowingEndedTheHookStaysForAWhileAndNoLonger()
    {
        using Next = Procedure::Next;
        constexpr std::uint64_t stays = ui::hook::staysForMs;
        const input::Pressed ctrl{0x1D, false, 0x11};
        // Given back with Ctrl held, at 1000.
        Procedure procedure;
        swallowing(procedure);
        QCOMPARE(press(procedure, leftCtrl, 10), swallowedAndTold);
        QCOMPARE(press(procedure, keyA, 20), swallowedAndTold);
        static_cast<void>(told(procedure));
        QVERIFY(!procedure.setSwallowing(false, 1000));
        // Until its time is up the hook stays: it swallows nothing, and tells of what it had
        // kept from Windows going up.
        QCOMPARE(procedure.look(999 + stays).next, Next::GoOn);
        QCOMPARE(press(procedure, keyC, 999 + stays), passedOn);
        QCOMPARE(release(procedure, keyA, 999 + stays), passedOnAndTold);
        QCOMPARE(told(procedure), QStringList{u"-4"_s});
        // Its time is up. The procedure itself does nothing with a key from then on, whatever
        // the hook's thread and the interface thread do or fail to do: not even the going up of
        // what it kept from Windows is told, by either account.
        QCOMPARE(release(procedure, leftCtrl, 1000 + stays), passedOn);
        QVERIFY(!procedure.rawKey(ctrl, false, 1000 + stays));
        QCOMPARE(press(procedure, keyC, 1001 + stays), passedOn);
        QVERIFY(told(procedure).isEmpty());
        // And the hook's own thread ends it: no timer of the interface thread is needed for that.
        QCOMPARE(procedure.look(1000 + stays).next, Next::End);
        QVERIFY(procedure.isClosed());
        QVERIFY(!procedure.setSwallowing(true, 1001 + stays));
        QVERIFY(!procedure.swallows());

        // Taken again before that: it swallows again, and its time begins anew when that ends.
        Procedure again;
        swallowing(again);
        QVERIFY(!again.setSwallowing(false, 1000));
        QVERIFY(again.setSwallowing(true, 50'000));
        // (The interface thread is alive while keys are swallowed, and says so.)
        again.stillThere(999 + stays);
        QCOMPARE(again.look(1000 + stays).next, Next::GoOn);
        again.stillThere(1000 + stays);
        QCOMPARE(press(again, keyA, 1000 + stays), swallowedAndTold);
        // The leave-key ends it in the procedure itself, at 70'000: from then.
        again.stillThere(69'980);
        press(again, rightCtrl, 69'990);
        release(again, rightCtrl, 70'000);
        QVERIFY(!again.swallows());
        QCOMPARE(again.look(69'999 + stays).next, Next::GoOn);
        QCOMPARE(again.look(70'000 + stays).next, Next::End);

        // A hook that was never asked to swallow does not stay for ever either.
        Procedure unused;
        unused.begin(500);
        QCOMPARE(unused.look(499 + stays).next, Next::GoOn);
        QCOMPARE(unused.look(500 + stays).next, Next::End);

        // Closed because keys were not heard: it swallows nothing ever again, and stays for what
        // goes up, by either account's word, no longer than the same time.
        Procedure closed;
        swallowing(closed);
        closed.fallBehind(3000);
        QCOMPARE(closed.look(3001).next, Next::GoOn);
        QVERIFY(!closed.setSwallowing(true, 3002));
        QCOMPARE(closed.look(2999 + stays).next, Next::GoOn);
        QCOMPARE(closed.look(3000 + stays).next, Next::End);
    }

    void takenAnewALeaveKeyThatIsHeldIsNotTheWayOut_data() { eachWayOfTheAccounts(); }

    void takenAnewALeaveKeyThatIsHeldIsNotTheWayOut()
    {
        QFETCH(int, way);
        Procedure procedure;
        swallowing(procedure, static_cast<Accounts>(way));
        const Keyboard keys{&procedure, static_cast<Accounts>(way)};
        keys.press(keyA);
        // Given back, with the hook still in for A; the leave-key goes down, as any key of this
        // PC's; and they are taken again with it held. Let go, it gives nothing back.
        procedure.setSwallowing(false, 0);
        keys.press(rightCtrl);
        procedure.setSwallowing(true, 0);
        QCOMPARE(keys.release(rightCtrl), passedOnAndTold);
        QVERIFY(procedure.swallows());
        // Pressed for it, it does.
        keys.press(rightCtrl);
        keys.release(rightCtrl);
        QVERIFY(!procedure.swallows());
    }

    void underAltGrTheLeftCtrlWindowsMakesUpIsNotSaidToBeDown()
    {
        const std::map<std::pair<std::uint32_t, bool>, std::uint32_t> names = {
            {{0x1D, false}, 0xA2}, // left Ctrl
            {{0x38, false}, 0xA4}, // left Alt
            {{0x38, true}, 0xA5},  // right Alt
            {{0x1E, false}, 0x41}, // A
        };
        const auto nameOf = [&](std::uint32_t scanCode, bool extended) -> std::uint32_t {
            const auto found = names.find({scanCode, extended});
            return found == names.end() ? 0 : found->second;
        };
        const auto held = [&](const std::set<std::uint32_t>& isDown) {
            std::set<int> usages;
            for (const input::Pressed& key :
                 ui::hook::heldKeys(nameOf, [&](std::uint32_t name) { return isDown.contains(name); })) {
                usages.insert(input::usageOf(key));
            }
            return usages;
        };
        // AltGr is held: Windows has the right Alt as down and a left Ctrl of its own making,
        // which is no key and is never let go as one. Only the right Alt is said.
        QCOMPARE(held({0xA2, 0xA5, 0x41}), (std::set<int>{0xE6, 0x04}));
        // By itself, and under the left Alt, the left Ctrl is a key.
        QCOMPARE(held({0xA2}), (std::set<int>{0xE0}));
        QCOMPARE(held({0xA2, 0xA4}), (std::set<int>{0xE0, 0xE2}));
    }

    void whatIsDownIsSaidByTheProcedureAndByWindowsTogether()
    {
        const input::Pressed a{0x1E, false, 0x41};
        const input::Pressed c{0x2E, false, 0x43};
        const input::Pressed ctrl{0x1D, false, 0xA2};
        const input::Pressed otherCtrl{0x1D, true, 0xA3};
        // What the procedure heard (Ctrl was kept from Windows), and what Windows has (C from
        // before the listening began): each once.
        const std::vector<input::Pressed> all = ui::hook::together({a, ctrl}, {a, c, otherCtrl});
        std::multiset<int> usages;
        for (const input::Pressed& key : all) {
            usages.insert(input::usageOf(key));
        }
        QCOMPARE(usages, (std::multiset<int>{0x04, 0x06, 0xE0, 0xE4}));
        QVERIFY(ui::hook::together({}, {}).empty());
    }

    void whenKeysWillNotBeToldAnyMoreTheSwallowingEndsAndThatIsSaidAsFallenBehind()
    {
        // (Named for what it shows. It was called "when nothing is heard any more the keyboard
        // is given back and everything let go", and said of itself that Windows shows another
        // desktop, which has a word and a test of its own.)
        Procedure procedure;
        swallowing(procedure);
        press(procedure, leftCtrl);
        QVERIFY(!procedure.takeFellBehind());
        // Windows would not wait for the hook's thread: what comes from here on is not told.
        procedure.fallBehind(100);
        QVERIFY(!procedure.swallows());
        QVERIFY(procedure.isClosed());
        QVERIFY(procedure.takeFellBehind());
        QVERIFY(!procedure.takeFellBehind());
        QVERIFY(!procedure.takeAnotherDesktop());
        QVERIFY(!procedure.takeNotHanded());
    }

    void beginningAnewAndEndingLeaveNothingBehind()
    {
        Procedure procedure;
        swallowing(procedure);
        press(procedure, keyA);
        press(procedure, rightCtrl);
        procedure.fallBehind(0);
        // Ended: nothing of what was typed waits.
        procedure.forget();
        QVERIFY(!procedure.swallows());
        QVERIFY(heard(procedure).empty());
        // Begun anew: nothing is held, the leave-key is not half pressed, nothing fell behind,
        // and the program is alive.
        procedure.begin(100'000);
        QVERIFY(!procedure.swallows());
        QVERIFY(!procedure.takeFellBehind());
        procedure.setSwallowing(true, 0);
        // (The leave-key goes up, and has not gone down since: that gives nothing back.)
        QCOMPARE(release(procedure, rightCtrl, 100'001), swallowedAndTold);
        QVERIFY(procedure.swallows());
        QCOMPARE(press(procedure, keyA, 100'002), swallowedAndTold);
        QCOMPARE(heard(procedure), (std::vector<int>{-0xE4, 0x04}));
    }

    void theKeysThatAreDownAreSaidOnlyWhereWindowsNamesOnePhysicalKey()
    {
        // Windows' names for some physical keys, as its own table has them: Enter and the Enter
        // of the number block share one, and so do Home and the number block's 7.
        const std::map<std::pair<std::uint32_t, bool>, std::uint32_t> names = {
            {{0x1E, false}, 0x41},                       // A
            {{0x2E, false}, 0x43},                       // C
            {{0x1D, false}, 0xA2},                       // left Ctrl
            {{0x1D, true}, 0xA3},                        // right Ctrl
            {{0x2A, false}, 0xA0},                       // left Shift
            {{0x1C, false}, 0x0D},                       // Enter
            {{0x1C, true}, 0x0D},                        // Enter on the number block
            {{0x47, false}, 0x24},                       // 7 on the number block
            {{0x47, true}, 0x24},                        // Home
            {{0x45, false}, 0x90},                       // Num Lock, told with the mark or without: one key
            {{0x45, true}, 0x90},  {{0x30, true}, 0xAF}, // louder: no key of the keyboard's page
            {{0x39, false}, 300},                        // a name Windows does not have
        };
        const auto nameOf = [&](std::uint32_t scanCode, bool extended) -> std::uint32_t {
            const auto found = names.find({scanCode, extended});
            return found == names.end() ? 0 : found->second;
        };
        std::set<std::uint32_t> down = {0x41, 0xA3, 0x0D, 0x24, 0x90, 0xAF, 300};
        const auto held = [&] {
            std::set<int> usages;
            for (const input::Pressed& key :
                 ui::hook::heldKeys(nameOf, [&](std::uint32_t name) { return down.contains(name); })) {
                usages.insert(input::usageOf(key));
            }
            return usages;
        };
        // A, right Ctrl and Num Lock; neither Enter, neither Home, and nothing that is no key.
        QCOMPARE(held(), (std::set<int>{0x04, 0xE4, 0x53}));
        down = {0xA2, 0xA0, 0x43};
        QCOMPARE(held(), (std::set<int>{0xE0, 0xE1, 0x06}));
        down.clear();
        QVERIFY(held().empty());
        // Each as the key it is: with its numbers, its mark and Windows' name.
        down = {0xA3};
        const std::vector<input::Pressed> one =
            ui::hook::heldKeys(nameOf, [&](std::uint32_t name) { return down.contains(name); });
        QCOMPARE(one.size(), std::size_t{1});
        QCOMPARE(one.front().scanCode, 0x1DU);
        QVERIFY(one.front().extended);
        QCOMPARE(one.front().virtualKey, 0xA3U);
    }

    // --- what a third reviewer found ---

    void whileTheTwoAccountsAreNotAtOneNothingNewIsHandedOn()
    {
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        // Windows leaves the hook out for Ctrl's "down" (its thread was slow): only the other
        // account tells of it. C, a tenth of a second later, is heard, and kept from Windows.
        QVERIFY(!procedure.rawKey(rawCtrl, true, 100));
        QCOMPARE(press(procedure, keyC, 200), swallowedUntold);
        // It is not handed on: on the machine Ctrl is up, and C would arrive there bare.
        QVERIFY(told(procedure).isEmpty());
        QVERIFY(!procedure.rawKey(rawC, true, 200));
        QVERIFY(told(procedure).isEmpty());
        // Kept so for no longer than the moment: then the hook hears no more, whoever listens
        // lets go of everything, and C never went anywhere.
        const Procedure::Looked soon = procedure.look(100 + moment);
        QVERIFY(!soon.wake);
        QVERIFY(procedure.swallows());
        QVERIFY(told(procedure).isEmpty());
        const Procedure::Looked later = procedure.look(101 + moment);
        QVERIFY(later.wake);
        QVERIFY(procedure.takeNotHanded());
        QVERIFY(procedure.isClosed());
        QVERIFY(told(procedure).isEmpty());
        // That it is down, and that Windows does not know, is known all the same: asked of
        // the table, by whoever lets go of everything.
        QCOMPARE(kept(procedure), (std::multiset<int>{0x06}));
        QCOMPARE(release(procedure, keyC, 900), passedOnAndTold);
        QCOMPARE(told(procedure), QStringList{u"-6"_s});
        QVERIFY(kept(procedure).empty());

        // The same with a key that went down and up meanwhile: its press is told to nobody,
        // and its going up is, so that nobody goes on waiting for it.
        Procedure tapped;
        swallowing(tapped);
        tapped.rawInputTells();
        QVERIFY(!tapped.rawKey(rawCtrl, true, 100));
        QCOMPARE(press(tapped, keyC, 200), swallowedUntold);
        QVERIFY(!tapped.rawKey(rawC, true, 200));
        QCOMPARE(release(tapped, keyC, 250), swallowedUntold);
        QVERIFY(!tapped.rawKey(rawC, false, 250));
        QVERIFY(told(tapped).isEmpty());
        QVERIFY(tapped.look(101 + moment).wake);
        QVERIFY(tapped.takeNotHanded());
        QCOMPARE(told(tapped), QStringList{u"-6"_s});
        QVERIFY(kept(tapped).empty());
    }

    void whatWaitedGoesOutInItsOrderWhenTheTwoAccountsAreAtOneAgain()
    {
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        // The hook hears first, as it does where both wait for its thread: each key waits for
        // the other account's word for it, which says that nothing before it was left out.
        QCOMPARE(press(procedure, keyA, 100), swallowedUntold);
        QCOMPARE(press(procedure, keyC, 101), swallowedUntold);
        QVERIFY(told(procedure).isEmpty());
        QVERIFY(procedure.rawKey(rawA, true, 102));
        QCOMPARE(told(procedure), QStringList{u"+4"_s});
        QVERIFY(procedure.rawKey(rawC, true, 103));
        QCOMPARE(told(procedure), QStringList{u"+6"_s});
        // The other account first: the key is handed on the moment it is heard.
        QVERIFY(!procedure.rawKey(rawA, false, 200));
        QCOMPARE(release(procedure, keyA, 201), swallowedAndTold);
        QCOMPARE(told(procedure), QStringList{u"-4"_s});
        // Ctrl is told of, and heard only a little later, in time: what is heard meanwhile
        // waits, and all of it goes out in the order it was heard in.
        QVERIFY(!procedure.rawKey(rawCtrl, true, 300));
        QVERIFY(!procedure.rawKey(rawC, false, 305));
        QCOMPARE(press(procedure, leftCtrl, 320), swallowedUntold);
        QVERIFY(told(procedure).isEmpty());
        QCOMPARE(release(procedure, keyC, 321), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+e0"_s, u"-6"_s}));
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.look(5000).wake);
        QVERIFY(!procedure.takeNotHanded());
        // Each with when it was heard, and not when it went out.
        QVERIFY(!procedure.rawKey(rawA, true, 6000));
        procedure.stillThere(6000);
        QCOMPARE(press(procedure, keyA, 6010), swallowedAndTold);
        const std::optional<Heard> next = procedure.next();
        QVERIFY(next.has_value());
        QCOMPARE(next.value_or(Heard{}).atMs, std::uint64_t{6010});
        QVERIFY(next.value_or(Heard{}).swallowed);
    }

    void aKeyThatWindowsOtherAccountNeverTellsOfIsHandedOnAfterTheMoment()
    {
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        Procedure procedure;
        swallowing(procedure);
        // Not asked for, or refused: the keyboard is not watched, and nothing waits.
        QVERIFY(!procedure.isWatched());
        procedure.rawInputTells();
        QVERIFY(procedure.isWatched());
        // It was asked for, and tells of no key (over a remote desktop none may come): the
        // first key waits for its word, for the moment and no longer.
        QCOMPARE(press(procedure, keyA, 100), swallowedUntold);
        QVERIFY(!procedure.look(100 + moment).wake);
        QVERIFY(told(procedure).isEmpty());
        QVERIFY(procedure.look(101 + moment).wake);
        QCOMPARE(told(procedure), QStringList{u"+4"_s});
        // That ends nothing. It is known from here on: no key waits for a word that does not
        // come, and whoever asks is told that the keyboard is not watched.
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.isWatched());
        procedure.stillThere(1000);
        QCOMPARE(release(procedure, keyA, 1000), swallowedAndTold);
        QCOMPARE(press(procedure, keyC, 1100), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"-4"_s, u"+6"_s}));
        // It tells of a key after all: watched again, and the next key that goes down waits for
        // its word. (One that goes up does not, where its going down was handed on. Turned
        // round: this had C's going up wait for the word.)
        QVERIFY(!procedure.rawKey(rawC, true, 1100));
        QVERIFY(procedure.isWatched());
        QCOMPARE(release(procedure, keyC, 1200), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawC, false, 1201));
        QCOMPARE(told(procedure), QStringList{u"-6"_s});
        QCOMPARE(press(procedure, keyA, 1300), swallowedUntold);
        QVERIFY(told(procedure).isEmpty());
        QVERIFY(procedure.rawKey(rawA, true, 1301));
        QCOMPARE(told(procedure), QStringList{u"+4"_s});

        // Another window's hook asked for that account, and has it now: this one is told so
        // on its own thread, and waits for no word either.
        Procedure other;
        swallowing(other);
        other.rawInputTells();
        other.rawInputTellsNoMore();
        QVERIFY(!other.isWatched());
        QCOMPARE(press(other, keyA, 100), swallowedAndTold);
    }

    void anUpThatOnlyWindowsOtherAccountToldOfIsKnownWhenControlEndsOverIt()
    {
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        QVERIFY(!procedure.rawKey(rawCtrl, true, 100));
        QCOMPARE(press(procedure, leftCtrl, 100), swallowedAndTold);
        QCOMPARE(told(procedure), QStringList{u"+e0"_s});
        // Windows leaves the hook out for Ctrl's "up": only the other account tells of it.
        QVERIFY(!procedure.rawKey(rawCtrl, false, 500));
        QVERIFY(procedure.look(501 + moment).wake);
        QVERIFY(procedure.takeNotHanded());
        // Ctrl is up, by the word of the account that told of it. It is not kept as a key that
        // is down, to be waited for at the next taking for an "up" that never comes; and
        // whoever listens is told that it went up.
        QCOMPARE(told(procedure), QStringList{u"-e0"_s});
        QVERIFY(kept(procedure).empty());
        QVERIFY(down(procedure).empty());
    }

    void aKeyThatWasDownWhenTheComparingBeganIsNotComparedUntilItHasGoneUp()
    {
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        Procedure procedure;
        procedure.begin(0);
        // Enter is still down from pressing "Control" with the keyboard as the hook goes in.
        procedure.windowsHas(keyEnter.name);
        procedure.isDownAlready(rawEnter);
        procedure.rawInputTells();
        procedure.setSwallowing(true, 0);
        // It is said again, and of one saying only one account tells (it was on its way as
        // the two began): that says nothing of the hook, and ends nothing.
        QVERIFY(!procedure.rawKey(rawEnter, true, 100));
        QVERIFY(!procedure.rawKey(rawEnter, true, 133));
        QCOMPARE(press(procedure, keyEnter, 166), swallowedAndTold);
        QCOMPARE(told(procedure), QStringList{u"w+28"_s});
        procedure.stillThere(2000);
        const Procedure::Looked looked = procedure.look(200 + 5 * moment);
        QVERIFY(!looked.wake);
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeNotHanded());
        // It goes up, heard by both. From then on it is a key like any other: told of alone,
        // it is missed.
        procedure.stillThere(3000);
        QCOMPARE(release(procedure, keyEnter, 3000), passedOnAndTold);
        QVERIFY(!procedure.rawKey(rawEnter, false, 3001));
        QCOMPARE(told(procedure), QStringList{u"-28"_s});
        QVERIFY(!procedure.look(3000 + 5 * moment).wake);
        procedure.stillThere(9000);
        QVERIFY(!procedure.rawKey(rawEnter, true, 9000));
        QVERIFY(procedure.look(9001 + moment).wake);
        QVERIFY(procedure.takeNotHanded());

        // Which keys those are, as the hook goes in: every key whose name Windows has as down,
        // also where two keys share the name. Enter is down, and which Enter is not known:
        // neither is compared until it has gone up. (What is said to be down, for a key to be
        // kept back until it was let go, is neither of them: `heldKeys`.)
        const std::map<std::pair<std::uint32_t, bool>, std::uint32_t> names = {
            {{0x1C, false}, 0x0D}, // Enter
            {{0x1C, true}, 0x0D},  // Enter on the number block
            {{0x1E, false}, 0x41}, // A
            {{0x2E, false}, 0x43}, // C
            {{0x30, true}, 0xAF},  // louder: no key of the keyboard's page
        };
        const auto nameOf = [&](std::uint32_t scanCode, bool extended) -> std::uint32_t {
            const auto found = names.find({scanCode, extended});
            return found == names.end() ? 0 : found->second;
        };
        const auto isDown = [](std::uint32_t name) { return name == 0x0D || name == 0x43 || name == 0xAF; };
        std::multiset<int> may;
        for (const input::Pressed& key : ui::hook::keysThatMayBeDown(nameOf, isDown)) {
            may.insert(input::usageOf(key));
        }
        QCOMPARE(may, (std::multiset<int>{0x06, 0x28, 0x58}));
        QVERIFY(ui::hook::heldKeys(nameOf, [](std::uint32_t name) { return name == 0x0D; }).empty());
    }

    void aHookThreadThatNoLongerLooksIsFound()
    {
        using ui::hook::LookWatch;
        constexpr std::uint64_t within = ui::hook::looksWithinMs;
        // The hook's thread says when it last looked: as it begins, and at every look.
        Procedure procedure;
        procedure.begin(1000);
        QCOMPARE(procedure.lookedAtMs(), std::uint64_t{1000});
        static_cast<void>(procedure.look(1100));
        QCOMPARE(procedure.lookedAtMs(), std::uint64_t{1100});
        // The interface thread asks four times a second. While that stamp moves, it looks.
        LookWatch watch;
        std::uint64_t now = 10'000;
        for (; now < 20'000; now += 250) {
            QVERIFY(!watch.take(now, now - 40));
        }
        // It stands still. For a few seconds that may be a PC that is busy; after them the
        // thread no longer looks.
        const std::uint64_t stood = now - 40;
        for (std::uint64_t more = 0; more < within; more += 250) {
            QVERIFY2(!watch.take(now + more, stood), qPrintable(QString::number(more)));
        }
        QVERIFY(watch.take(now + within, stood));
        // It moves again: looking, and counted from the start the next time.
        QVERIFY(!watch.take(now + within + 250, now + within + 200));
        QVERIFY(!watch.take(now + within + 500, now + within + 200));
        // The whole program was stopped for a minute, both threads with it (a debugger, a PC
        // that slept): the interface thread, back first, does not take the minute for the
        // hook's thread's. Only what it saw pass in steps of its own counts.
        LookWatch stopped;
        QVERIFY(!stopped.take(1000, 990));
        QVERIFY(!stopped.take(1250, 990));
        QVERIFY(!stopped.take(61'250, 990));
        QVERIFY(!stopped.take(61'500, 61'490));
        // And the gap is a few seconds: slow on purpose.
        QVERIFY(within >= 2000 && within <= 5000);
    }

    void howLongWindowsWaitsForAHookIsReadAndAMarginKeptUnderIt()
    {
        using ui::hook::atOneWithin;
        using ui::hook::inTimeUnder;
        using ui::hook::windowsWaitsFor;
        // 300 ms, unless this PC's registry says a shorter time; and never more than those,
        // whatever it says. (Turned round: this had a second believed where the registry says
        // one, or more. Windows may not keep to a number that was raised since somebody signed
        // in, and a key it took for itself meanwhile counted as in time.)
        QCOMPARE(windowsWaitsFor(std::nullopt), std::uint64_t{300});
        QCOMPARE(windowsWaitsFor(0U), std::uint64_t{300});
        QCOMPARE(windowsWaitsFor(200U), std::uint64_t{200});
        QCOMPARE(windowsWaitsFor(300U), std::uint64_t{300});
        QCOMPARE(windowsWaitsFor(301U), std::uint64_t{300});
        QCOMPARE(windowsWaitsFor(1000U), std::uint64_t{300});
        QCOMPARE(windowsWaitsFor(5000U), std::uint64_t{300});
        QCOMPARE(windowsWaitsFor(0xFFFF'FFFFU), std::uint64_t{300});
        // A key came in time where it waited less than that, with a margin.
        QCOMPARE(inTimeUnder(300), std::uint64_t{250});
        QCOMPARE(inTimeUnder(1000), std::uint64_t{950});
        QCOMPARE(inTimeUnder(200), std::uint64_t{150});
        QCOMPARE(inTimeUnder(60), std::uint64_t{30});
        QCOMPARE(inTimeUnder(1), std::uint64_t{0});
        // And the moment within which the two accounts have to be at one is that wait and a
        // little: a hearing that comes later than Windows waits is none.
        QCOMPARE(atOneWithin(300), std::uint64_t{400});
        QCOMPARE(atOneWithin(200), std::uint64_t{300});
        QCOMPARE(atOneWithin(1000), std::uint64_t{1100});
        QCOMPARE(ui::hook::heardWithinMs, std::uint64_t{400});

        // A PC that was told 200 ms: a key that waited 150 is not kept from Windows.
        Procedure quick;
        quick.begin(0, windowsWaitsFor(200U));
        quick.setSwallowing(true, 0);
        QCOMPARE(quick.key(keyA.name, keyA.scanCode, keyA.flags, true, 100, 149), swallowedAndTold);
        QVERIFY(!quick.takeFellBehind());
        QCOMPARE(quick.key(keyC.name, keyC.scanCode, keyC.flags, true, 200, 150), passedOnAndTold);
        QVERIFY(quick.takeFellBehind());
        QVERIFY(quick.isClosed());
        // And its two accounts have to be at one sooner.
        // (Raw Input is told to this hook from the start, as the hook's thread says it when it
        // has asked for that account. This left it unsaid, so that the comparing began at the
        // telling below; and a telling within the moment of such a beginning has a word of its
        // own: `whereTheOtherAccountBeginsToTellLaterAKeyHeardBeforeThatEndsTheControlOnceWithAWordOfItsOwn`.)
        Procedure sooner;
        sooner.begin(0, windowsWaitsFor(200U));
        sooner.rawInputTells();
        sooner.setSwallowing(true, 0);
        QVERIFY(!sooner.rawKey(rawA, true, 100));
        QVERIFY(!sooner.look(400).wake);
        QVERIFY(sooner.look(401).wake);
        QVERIFY(sooner.takeNotHanded());
        // One that was told a second: no more than Windows' own time is believed. A key that
        // waited 260 ms may be Windows' already, whatever the registry says by now. (Turned
        // round: this had a key that waited 900 ms as one that came in time.)
        Procedure slow;
        slow.begin(0, windowsWaitsFor(1000U));
        slow.setSwallowing(true, 0);
        QCOMPARE(slow.key(keyA.name, keyA.scanCode, keyA.flags, true, 250, 249), swallowedAndTold);
        QVERIFY(!slow.takeFellBehind());
        QCOMPARE(slow.key(keyC.name, keyC.scanCode, keyC.flags, true, 960, 260), passedOnAndTold);
        QVERIFY(slow.takeFellBehind());
        QVERIFY(slow.isClosed());
    }

    void noLongerWaitThanWindowsOwnIsBelievedWhateverTheRegistrySays()
    {
        using ui::hook::atOneWithin;
        using ui::hook::inTimeUnder;
        using ui::hook::windowsWaitsFor;
        // Every number the registry can hold, as far as they can differ: each of the first
        // hundred thousand, then every power of two and its neighbours up to the last. The wait
        // that is believed is the number itself up to Windows' own 300 ms, and those from there
        // on; and with it the gate's moment, and the look after it, stay well under what the
        // interface thread takes for too late.
        std::vector<std::uint32_t> said;
        for (std::uint32_t each = 0; each <= 100'000; ++each) {
            said.push_back(each);
        }
        for (std::uint32_t bit = 17; bit < 32; ++bit) {
            const std::uint32_t power = std::uint32_t{1} << bit;
            said.insert(said.end(), {power - 1, power, power + 1});
        }
        said.push_back(0xFFFF'FFFFU);
        for (const std::uint32_t one : said) {
            const std::uint64_t waits = windowsWaitsFor(one);
            const std::uint64_t should = one >= 1 && one <= 300 ? one : 300;
            if (waits != should || atOneWithin(waits) + ui::hook::lookEveryMs > ui::hook::lateAfterMs / 2 ||
                inTimeUnder(waits) >= waits) {
                QFAIL(qPrintable(QStringLiteral("the registry says %1: %2 ms are believed, the moment is %3 ms")
                                     .arg(one)
                                     .arg(waits)
                                     .arg(atOneWithin(waits))));
            }
        }
        QCOMPARE(windowsWaitsFor(std::nullopt), std::uint64_t{300});

        // The first key of a taking on a PC whose second account is asked for and tells of
        // nothing: it waits its moment for a word that does not come, goes on at the look after
        // that, and is taken by the interface thread a little later. It is typed, and not
        // dropped as one that came too late, whatever the registry says. (With a second
        // believed, it had waited longer than a key may: it was dropped, and control ended, at
        // the first key of every taking.)
        for (const std::optional<std::uint32_t> one :
             {std::optional<std::uint32_t>(), std::optional<std::uint32_t>(0U), std::optional<std::uint32_t>(120U),
              std::optional<std::uint32_t>(250U), std::optional<std::uint32_t>(300U),
              std::optional<std::uint32_t>(301U), std::optional<std::uint32_t>(899U),
              std::optional<std::uint32_t>(900U), std::optional<std::uint32_t>(1000U),
              std::optional<std::uint32_t>(5000U), std::optional<std::uint32_t>(0xFFFF'FFFFU)}) {
            const std::uint64_t moment = atOneWithin(windowsWaitsFor(one));
            Procedure procedure;
            procedure.begin(0, windowsWaitsFor(one));
            procedure.rawInputTells();
            procedure.setSwallowing(true, 0);
            QCOMPARE(press(procedure, keyA, 100), swallowedUntold);
            const std::uint64_t looks = 100 + moment + ui::hook::lookEveryMs;
            procedure.stillThere(looks - 50);
            QVERIFY2(procedure.look(looks).wake, qPrintable(QString::number(one.value_or(0))));
            const QStringList typed = written(ui::hook::whatToTell(procedure, looks + 50));
            QVERIFY2(typed == QStringList{u"+4"_s},
                     qPrintable(QStringLiteral("the registry says %1: %2").arg(one.value_or(0)).arg(typed.join(u' '))));
            QVERIFY(procedure.swallows());
        }
    }

    // --- what a fourth reviewer found ---

    void aKeyThatGoesUpDoesNotWaitWhereItsGoingDownWasHandedOn()
    {
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        // Enter goes down, heard and told of: it is down on the machine.
        QCOMPARE(press(procedure, keyEnter, 100), swallowedUntold);
        QVERIFY(procedure.rawKey(rawEnter, true, 100));
        QCOMPARE(told(procedure), QStringList{u"+28"_s});
        // It goes up, and the other account's word for that does not come. Letting go on the
        // machine what is down there needs nobody's word: handed on at once. (It waited the
        // moment, and the machine said Enter again and again meanwhile.)
        QCOMPARE(release(procedure, keyEnter, 150), swallowedAndTold);
        QCOMPARE(told(procedure), QStringList{u"-28"_s});
        // Nor does it wait behind another key's telling that is unheard.
        QVERIFY(!procedure.rawKey(rawEnter, false, 151));
        QCOMPARE(press(procedure, keyA, 200), swallowedUntold);
        QVERIFY(procedure.rawKey(rawA, true, 200));
        QVERIFY(!procedure.rawKey(rawCtrl, true, 300));
        QCOMPARE(release(procedure, keyA, 310), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+4"_s, u"-4"_s}));
        QVERIFY(procedure.swallows());

        // A key that goes down waits, as it did. And no key that goes up passes its own going
        // down: both go out when the word for the press has come, in their order.
        Procedure own;
        swallowing(own);
        own.rawInputTells();
        QCOMPARE(press(own, keyC, 100), swallowedUntold);
        QCOMPARE(release(own, keyC, 110), swallowedUntold);
        QVERIFY(told(own).isEmpty());
        // (Both are on their way: the press, and its going up behind it.)
        QCOMPARE(own.onTheirWay().count, std::uint32_t{2});
        QVERIFY(own.rawKey(rawC, true, 120));
        QCOMPARE(told(own), (QStringList{u"+6"_s, u"-6"_s}));
        // The same key said again waits for nobody's word: it goes out at once. (Turned round:
        // this recorded that a key which is said again waits for the other account's word as a
        // press does, and its going up behind it: Enter stayed down on the machine for as long
        // as that word took, or for good where one account was more sayings ahead than there
        // was room for.)
        // Its going up, heard while that saying is still to be told of: it goes up at once, as
        // an "up" of any key does where its going down was handed on. (Turned round a fourth
        // time, to what it recorded first. In between, the "up" waited for that word, since the
        // saying may have been another keyboard's, which then held the key: of every key, and
        // then of the modifiers only. Keyboards are not told apart any more, and an "up" waits
        // for nothing that has to do with whose it was: `aKeyGoesUpByItsUpAsItIsHeard`.)
        QCOMPARE(press(own, keyEnter, 200), swallowedUntold);
        QVERIFY(own.rawKey(rawEnter, true, 200));
        QCOMPARE(press(own, keyEnter, 233), swallowedAndTold);
        QCOMPARE(release(own, keyEnter, 240), swallowedAndTold);
        QCOMPARE(told(own), (QStringList{u"+28"_s, u"+28"_s, u"-28"_s}));
        QVERIFY(!own.rawKey(rawEnter, true, 241));
        QVERIFY(!own.rawKey(rawEnter, false, 242));
        QVERIFY(told(own).isEmpty());
        QVERIFY(down(own).empty());

        // And a modifier that goes up does not pass a key that went down under it and still
        // waits: that key would reach the machine bare. (A modifier is not said again by the
        // machine, and waits as it did.)
        Procedure under;
        swallowing(under);
        under.rawInputTells();
        QCOMPARE(press(under, leftShift, 100), swallowedUntold);
        QVERIFY(under.rawKey(rawShift, true, 100));
        QCOMPARE(press(under, keyA, 200), swallowedUntold);
        QCOMPARE(release(under, leftShift, 210), swallowedUntold);
        QCOMPARE(told(under), QStringList{u"+e1"_s});
        QVERIFY(under.rawKey(rawA, true, 220));
        QCOMPARE(told(under), (QStringList{u"+4"_s, u"-e1"_s}));
        // Another key that goes up does pass it: A is let go while C, pressed after it, waits.
        QCOMPARE(press(under, keyC, 300), swallowedUntold);
        QCOMPARE(release(under, keyA, 310), swallowedAndTold);
        QCOMPARE(told(under), QStringList{u"-4"_s});
        QVERIFY(under.rawKey(rawC, true, 320));
        QCOMPARE(told(under), QStringList{u"+6"_s});
    }

    void whichKeysAreOnTheirWayIsKnownAndInWhichOrderTheyWereHeard()
    {
        // For whoever sends a click after them: Shift, and then a click, is no bare click.
        // (Turned round, with its name: "how many keys that went down are on their way is
        // known". A key that goes up is one of them as well: Ctrl let go, and then a click, is
        // no click with Ctrl. And each has its number, since they are not told in the order
        // they were heard in.)
        const auto way = [](const Procedure& one) {
            const Procedure::OnTheirWay now = one.onTheirWay();
            return std::pair<std::uint64_t, std::uint32_t>(now.last, now.count);
        };
        const auto onTheirWay = [](std::uint64_t last, std::uint32_t count) {
            return std::pair<std::uint64_t, std::uint32_t>(last, count);
        };
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        QCOMPARE(way(procedure), onTheirWay(0, 0));
        // Shift waits for the other account's word, and A behind it.
        QCOMPARE(press(procedure, leftShift, 100), swallowedUntold);
        QCOMPARE(way(procedure), onTheirWay(1, 1));
        QCOMPARE(press(procedure, keyA, 110), swallowedUntold);
        QCOMPARE(way(procedure), onTheirWay(2, 2));
        // Handed on, they are still on their way until the interface thread has taken them.
        QVERIFY(procedure.rawKey(rawShift, true, 120));
        QVERIFY(procedure.rawKey(rawA, true, 121));
        QCOMPARE(way(procedure), onTheirWay(2, 2));
        QCOMPARE(procedure.next().value_or(Heard{}).number, std::uint64_t{1});
        QCOMPARE(way(procedure), onTheirWay(2, 1));
        QCOMPARE(procedure.next().value_or(Heard{}).number, std::uint64_t{2});
        // None is on its way: the numbers begin anew, and say nothing of how much was typed.
        QCOMPARE(way(procedure), onTheirWay(0, 0));
        // A key that goes up is one of them, until it was taken.
        QCOMPARE(release(procedure, keyA, 200), swallowedAndTold);
        QCOMPARE(way(procedure), onTheirWay(1, 1));
        static_cast<void>(told(procedure));
        QCOMPARE(way(procedure), onTheirWay(0, 0));

        // What is taken does not come in the order it was heard in. C goes down under Shift and
        // waits (1); Shift's going up stays behind it (2); B, which went down long before, goes
        // up and passes both (3). Whoever saw a click after the first two knows by the numbers
        // that B's going up is none of what the click has to wait for.
        QCOMPARE(press(procedure, keyB, 300), swallowedUntold);
        QVERIFY(procedure.rawKey(ui::hook::pressedOf(keyB.name, keyB.scanCode, keyB.flags), true, 300));
        static_cast<void>(told(procedure));
        QCOMPARE(press(procedure, keyC, 400), swallowedUntold);
        QCOMPARE(release(procedure, leftShift, 410), swallowedUntold);
        QCOMPARE(way(procedure), onTheirWay(2, 2));
        QCOMPARE(release(procedure, keyB, 420), swallowedAndTold);
        QCOMPARE(way(procedure), onTheirWay(3, 3));
        std::optional<Heard> next = procedure.next();
        QCOMPARE(input::usageOf(next.value_or(Heard{}).key), std::uint16_t{0x05});
        QCOMPARE(next.value_or(Heard{}).number, std::uint64_t{3});
        QCOMPARE(way(procedure), onTheirWay(3, 2));
        QVERIFY(procedure.rawKey(rawC, true, 430));
        next = procedure.next();
        QCOMPARE(input::usageOf(next.value_or(Heard{}).key), std::uint16_t{0x06});
        QCOMPARE(next.value_or(Heard{}).number, std::uint64_t{1});
        next = procedure.next();
        QCOMPARE(input::usageOf(next.value_or(Heard{}).key), std::uint16_t{0xE1});
        QVERIFY(!next.value_or(Heard{}).down);
        QCOMPARE(next.value_or(Heard{}).number, std::uint64_t{2});
        QCOMPARE(way(procedure), onTheirWay(0, 0));
        QCOMPARE(release(procedure, keyC, 440), swallowedAndTold);
        static_cast<void>(told(procedure));

        // One that waited and is dropped, because keys are heard no more, is on its way no more.
        QCOMPARE(press(procedure, keyC, 500), swallowedUntold);
        QCOMPARE(way(procedure), onTheirWay(1, 1));
        procedure.fallBehind(510);
        static_cast<void>(procedure.look(520));
        QCOMPARE(way(procedure), onTheirWay(0, 0));
        QVERIFY(told(procedure).isEmpty());
        // And where nothing waits for a second account, a key is on its way only until taken.
        Procedure plain;
        swallowing(plain);
        QCOMPARE(press(plain, keyA, 10), swallowedAndTold);
        QCOMPARE(way(plain), onTheirWay(1, 1));
        static_cast<void>(told(plain));
        QCOMPARE(way(plain), onTheirWay(0, 0));
    }

    void whatRawInputWillNotTellOfTakesNoTelling()
    {
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // A program types A. A moment later A is pressed on the keyboard, and the hook is not
        // called for it (Windows took it out, or left the key out): only Raw Input tells. The
        // typed key's hearing is not that key's, and the hook is found to hear no more.
        // (Turned round: the typed key was noted as heard, and took this telling.)
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        static_cast<void>(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, true, 100));
        static_cast<void>(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, false, 110));
        QVERIFY(!procedure.rawKey(rawA, true, 200));
        QVERIFY(!procedure.look(200 + moment).wake);
        QVERIFY(procedure.look(201 + moment).wake);
        QVERIFY(procedure.takeNotHanded());
        QVERIFY(procedure.isClosed());

        // AltGr: Windows makes up a left Ctrl before the right Alt, which Raw Input does not
        // tell of. The real left Ctrl goes down about then, unheard by the hook: found as well.
        Procedure altGr;
        swallowing(altGr);
        altGr.rawInputTells();
        static_cast<void>(press(altGr, madeUpCtrl, 100));
        QCOMPARE(press(altGr, rightAlt, 100), swallowedUntold);
        QVERIFY(altGr.rawKey(ui::hook::pressedOf(rightAlt.name, rightAlt.scanCode, rightAlt.flags), true, 100));
        QVERIFY(!altGr.rawKey(rawCtrl, true, 150));
        QVERIFY(altGr.look(151 + moment).wake);
        QVERIFY(altGr.takeNotHanded());
        // And neither waits for a telling: the made-up Ctrl is handed on as it is heard (it is
        // no key for a machine), and a typed key is handed on to nobody.
        Procedure waits;
        swallowing(waits);
        waits.rawInputTells();
        QCOMPARE(press(waits, madeUpCtrl, 100), swallowedAndTold);
        QCOMPARE(waits.onTheirWay().count, std::uint32_t{1});
        static_cast<void>(waits.next());
        static_cast<void>(waits.key(keyA.name, keyA.scanCode, keyA.flags | injected, true, 110));
        QCOMPARE(waits.onTheirWay().count, std::uint32_t{0});
        QVERIFY(!waits.look(110 + 5 * moment).wake);
        QVERIFY(waits.swallows());
    }

    void aKeyIsTheKeyItWentDownAsWhateverWindowsCallsItByThen()
    {
        // Ctrl is held, Num Lock is pressed, Ctrl is let go first. Windows calls the key Pause
        // while Ctrl is down and Num Lock after: one key, down once and up once, and what went
        // down on the machine is what goes up there. (It went down as Pause and up as Num
        // Lock: Pause stayed down on the machine until control ended.)
        Procedure procedure;
        swallowing(procedure);
        QCOMPARE(press(procedure, leftCtrl, 10), swallowedAndTold);
        QCOMPARE(press(procedure, numLockAsPause, 20), swallowedAndTold);
        QCOMPARE(release(procedure, leftCtrl, 30), swallowedAndTold);
        QCOMPARE(release(procedure, keyNumLock, 40), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+e0"_s, u"+53"_s, u"-e0"_s, u"-53"_s}));
        QVERIFY(down(procedure).empty());

        // Not by knowing that key. Whatever is told for a key that is down is that key: here
        // Num Lock as it would be were the hook told of it without the mark, "Pause" going down
        // and "Num Lock" going up. (Taken out of here: "and a key that Windows cannot name as it
        // goes up", which was let go so. What Windows cannot name is no key again, and lets go
        // of nothing: `whatWindowsCannotNameIsNoKeyGoingDownOrUp`.)
        QCOMPARE(procedure.key(0x13, 0x45, 0, true, 50), swallowedAndTold);
        QCOMPARE(procedure.key(0x90, 0x45, 0, false, 60), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+48"_s, u"-48"_s}));
        // Said again under another name, it is no other key going down: 7 on the number block,
        // which is Home once Shift is held.
        QCOMPARE(press(procedure, block7, 90), swallowedAndTold);
        QCOMPARE(press(procedure, block7AsHome, 95), swallowedAndTold);
        QCOMPARE(release(procedure, block7AsHome, 99), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+5f"_s, u"+5f"_s, u"-5f"_s}));
        // Under another number too, where it is the same key by its USB number: Pause, which
        // is Break under Ctrl, with another number and a mark. One key in the table, and none
        // once it was let go, under whichever of the two.
        QCOMPARE(press(procedure, keyPause, 110), swallowedAndTold);
        QCOMPARE(press(procedure, leftCtrl, 115), swallowedAndTold);
        QCOMPARE(press(procedure, pauseAsBreak, 120), swallowedAndTold);
        QCOMPARE(kept(procedure), (std::multiset<int>{0x48, 0xE0}));
        QCOMPARE(release(procedure, pauseAsBreak, 130), swallowedAndTold);
        QCOMPARE(release(procedure, leftCtrl, 135), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+48"_s, u"+e0"_s, u"+48"_s, u"-48"_s, u"-e0"_s}));
        QVERIFY(kept(procedure).empty());
        QVERIFY(down(procedure).empty());

        // And for this PC: a key that Windows saw go down, it sees go up, also where the key
        // was said again under another name meanwhile. 7 goes down before keys are swallowed,
        // and is Windows'. Said again as Home, it is that key said again and no press of the
        // machine's; let go as Home, Windows sees its 7 go up. (The name it was last said under
        // was taken for the name it went down under: Windows did not have a Home as down, the
        // "up" was kept from it, and the 7 stayed down on this PC.)
        Procedure windows;
        windows.begin(0);
        QCOMPARE(press(windows, block7, 10), passedOn);
        windows.setSwallowing(true, 0);
        QCOMPARE(press(windows, block7AsHome, 20), swallowedAndTold);
        QCOMPARE(release(windows, block7AsHome, 30), passedOnAndTold);
        QCOMPARE(told(windows), (QStringList{u"w+5f"_s, u"-5f"_s}));
    }

    void withTheKeyThatLeavesNoKeyDownThatIsSaid()
    {
        Procedure procedure;
        swallowing(procedure);
        press(procedure, keyA, 10);
        press(procedure, keyC, 11);
        release(procedure, keyA, 12);
        release(procedure, keyC, 13);
        // A key that goes up where none was down says nothing of the kind.
        release(procedure, keyC, 14);
        std::vector<bool> saidSo;
        while (const std::optional<Heard> next = procedure.next()) {
            saidSo.push_back(next->nothingIsDown);
        }
        QCOMPARE(saidSo, (std::vector<bool>{false, false, false, true, false}));
        QCOMPARE(written(ui::hook::whatToTell(procedure, 20)), QStringList());
        press(procedure, keyA, 30);
        release(procedure, keyA, 40);
        QCOMPARE(written(ui::hook::whatToTell(procedure, 50)), (QStringList{u"+4"_s, u"-4"_s, u"none down"_s}));

        // Not where a key that was heard before still waits to be handed on: what whoever
        // listens does on that word (it lets go of everything on the machine) may not come
        // before a key that was pressed under a modifier. Shift is down on the machine; A goes
        // down under it and waits; Shift's "up" stays behind A; C, pressed and let go long
        // before, is let go last, and with it no key is down.
        Procedure gated;
        swallowing(gated);
        gated.rawInputTells();
        press(gated, keyC, 100);
        static_cast<void>(gated.rawKey(rawC, true, 100));
        press(gated, leftShift, 110);
        static_cast<void>(gated.rawKey(rawShift, true, 110));
        press(gated, keyA, 120);
        release(gated, leftShift, 130);
        release(gated, keyA, 131);
        QCOMPARE(release(gated, keyC, 140), swallowedAndTold);
        QCOMPARE(written(ui::hook::whatToTell(gated, 150)), (QStringList{u"+6"_s, u"+e1"_s, u"-6"_s}));
        QVERIFY(gated.rawKey(rawA, true, 160));
        QCOMPARE(written(ui::hook::whatToTell(gated, 170)), (QStringList{u"+4"_s, u"-e1"_s, u"-4"_s}));
        QVERIFY(down(gated).empty());
    }

    void whatTheInterfaceThreadTellsAndInWhichOrder()
    {
        using Lost = ui::KeySource::Lost;
        using ui::hook::whatToTell;
        // Keys in the order they were handed on, each press with whose it is.
        Procedure procedure;
        procedure.begin(0);
        procedure.windowsHas(keyC.name);
        procedure.setSwallowing(true, 0);
        press(procedure, leftCtrl, 10);
        press(procedure, keyC, 20);
        release(procedure, keyC, 30);
        QCOMPARE(written(whatToTell(procedure, 40)), (QStringList{u"+e0"_s, u"w+6"_s, u"-6"_s}));
        QVERIFY(written(whatToTell(procedure, 41)).isEmpty());
        // That it runs says that the interface thread is alive.
        QCOMPARE(press(procedure, keyA, 1040), swallowedAndTold);
        static_cast<void>(whatToTell(procedure, 1050));

        // A key that waited too long is not typed late: with it the keys have fallen behind,
        // the hook is closed, and of that key and what is behind it only a going up is told.
        Procedure late;
        swallowing(late);
        press(late, keyA, 100);
        press(late, keyC, 150);
        release(late, keyA, 200);
        Procedure inTime;
        swallowing(inTime);
        press(inTime, keyA, 100);
        press(inTime, keyC, 150);
        release(inTime, keyA, 200);
        QCOMPARE(written(whatToTell(inTime, 99 + ui::hook::lateAfterMs)), (QStringList{u"+4"_s, u"+6"_s, u"-4"_s}));
        QVERIFY(!inTime.isClosed());
        QCOMPARE(written(whatToTell(late, 100 + ui::hook::lateAfterMs)), (QStringList{u"behind"_s, u"-4"_s}));
        QVERIFY(late.isClosed());
        QCOMPARE(kept(late), (std::multiset<int>{0x06}));

        // Why nothing is heard any more is said first, before any key, and one reason only:
        // the one that is known best. Here the keys that Windows acts on, though keys fell
        // behind as well. No reason is left standing for the next time.
        Procedure both;
        swallowing(both);
        press(both, leftWindows, 10);
        QCOMPARE(press(both, keyL, 20), passedOnAndTold);
        both.fallBehind(30);
        QCOMPARE(written(whatToTell(both, 40)), QStringList{lost(Lost::KeysOfWindows)});
        QVERIFY(!both.takeFellBehind());
        QVERIFY(!both.takeKeysOfWindows());
        QCOMPARE(release(both, leftWindows, 50), passedOnAndTold);
        QCOMPARE(written(whatToTell(both, 60)), QStringList{u"-e3"_s});
        // Windows' own desktop before keys that fell behind.
        Procedure desktop;
        swallowing(desktop);
        desktop.anotherDesktop(10);
        desktop.fallBehind(20);
        QCOMPARE(written(whatToTell(desktop, 30)), QStringList{lost(Lost::AnotherDesktop)});
        // Keys that fell behind, by themselves: said as that, and the hook is closed.
        Procedure behind;
        swallowing(behind);
        for (std::size_t said = 0; said < KeyRing::places; ++said) {
            press(behind, keyA, 10);
        }
        QVERIFY(!behind.isClosed());
        const QStringList said = written(whatToTell(behind, 20));
        QCOMPARE(said.first(), u"behind"_s);
        QCOMPARE(said.count(u"behind"_s), 1);
        QCOMPARE(said.size(), 1);
        QVERIFY(behind.isClosed());
    }

    void whereTheTwoAccountsDifferItIsKnownWhichToldOfAKeyTheOtherDidNot()
    {
        using Lost = ui::KeySource::Lost;
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // Windows told of a key, and the hook was not called for it: another program's hook
        // kept it, a key helper of Windows held it back, or Windows took this hook out.
        Procedure notHanded;
        swallowing(notHanded);
        notHanded.rawInputTells();
        press(notHanded, keyA, 100);
        static_cast<void>(notHanded.rawKey(rawA, true, 100));
        release(notHanded, keyA, 150);
        static_cast<void>(notHanded.rawKey(rawA, false, 150));
        static_cast<void>(told(notHanded));
        QVERIFY(!notHanded.rawKey(rawC, true, 500));
        QVERIFY(notHanded.look(501 + moment).wake);
        QCOMPARE(written(whatToTell(notHanded, 502 + moment)), QStringList{lost(Lost::NotHandedKeys)});

        // The other way round. Raw Input tells of this keyboard (it told of A), and a key is
        // heard that it does not tell of: something on this PC makes or changes keys. For a
        // moment its word may yet come. After it, nothing more is swallowed, the key never
        // went to a machine, and that is said as what it is. (It was taken for a PC that gives
        // no second account: the key went on a moment late, and the keyboard was no longer
        // watched.)
        Procedure notTold;
        swallowing(notTold);
        notTold.rawInputTells();
        press(notTold, keyA, 100);
        static_cast<void>(notTold.rawKey(rawA, true, 100));
        release(notTold, keyA, 150);
        static_cast<void>(notTold.rawKey(rawA, false, 150));
        static_cast<void>(told(notTold));
        QCOMPARE(press(notTold, keyC, 500), swallowedUntold);
        QVERIFY(!notTold.look(500 + moment).wake);
        QVERIFY(notTold.swallows());
        QVERIFY(notTold.look(501 + moment).wake);
        QVERIFY(!notTold.swallows());
        QVERIFY(notTold.isClosed());
        QCOMPARE(written(whatToTell(notTold, 502 + moment)), QStringList{lost(Lost::NotToldOfKeys)});
        QCOMPARE(kept(notTold), (std::multiset<int>{0x06}));
        QCOMPARE(notTold.onTheirWay().count, std::uint32_t{0});
        QVERIFY(!notTold.takeNotHanded());

        // Both at once: the hook heard one key, and Raw Input told of another, about then. A
        // remapped key looks so.
        Procedure differently;
        swallowing(differently);
        differently.rawInputTells();
        press(differently, keyA, 100);
        static_cast<void>(differently.rawKey(rawA, true, 100));
        release(differently, keyA, 150);
        static_cast<void>(differently.rawKey(rawA, false, 150));
        static_cast<void>(told(differently));
        QCOMPARE(press(differently, keyC, 500), swallowedUntold);
        QVERIFY(!differently.rawKey(rawEnter, true, 501));
        QVERIFY(differently.look(502 + moment).wake);
        QCOMPARE(written(whatToTell(differently, 503 + moment)), QStringList{lost(Lost::ToldDifferently)});

        // A key that was heard a moment ago, and whose telling is only on its way, says nothing
        // of the kind: Windows told of a key that was not heard, and that is all that is known.
        Procedure onItsWay;
        swallowing(onItsWay);
        onItsWay.rawInputTells();
        QVERIFY(!onItsWay.rawKey(rawEnter, true, 100));
        QCOMPARE(press(onItsWay, keyC, 100 + moment), swallowedUntold);
        QVERIFY(onItsWay.look(101 + moment).wake);
        QCOMPARE(written(whatToTell(onItsWay, 102 + moment)), QStringList{lost(Lost::NotHandedKeys)});

        // Where Raw Input was asked for and has told of no key (a remote desktop where none
        // comes), a key that waits in vain says only that: nothing ends.
        Procedure silent;
        swallowing(silent);
        silent.rawInputTells();
        QCOMPARE(press(silent, keyC, 500), swallowedUntold);
        QVERIFY(silent.look(501 + moment).wake);
        QVERIFY(silent.swallows());
        QCOMPARE(written(whatToTell(silent, 502 + moment)), QStringList{u"+6"_s});
    }

    void thatAProgramTypedIsSaidOnceAndNothingOfTheKey()
    {
        using ui::hook::whatToTell;
        Procedure procedure;
        procedure.begin(0);
        // While nothing is swallowed, a key that a program types is nobody's business.
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, true, 10), passedOn);
        QVERIFY(!procedure.takeTypedByAProgram());
        procedure.setSwallowing(true, 20);
        // While keys go to a machine: passed on as ever, and the interface thread is woken for
        // the first one, to say that such keys reach no machine. Once: not for every key of
        // whoever types from afar.
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, false, 30), passedOnAndTold);
        QCOMPARE(procedure.key(keyC.name, keyC.scanCode, keyC.flags | injectedFromBelow, true, 40), passedOn);
        QCOMPARE(procedure.key(keyC.name, keyC.scanCode, keyC.flags | injectedFromBelow, false, 50), passedOn);
        QCOMPARE(written(whatToTell(procedure, 60)), QStringList{u"typed"_s});
        QVERIFY(written(whatToTell(procedure, 61)).isEmpty());
        // Nothing of the key: not told, not down, not on its way.
        QVERIFY(down(procedure).empty());
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
        // Given back and taken again: said once more, at the first such key.
        procedure.setSwallowing(false, 70);
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, true, 80), passedOn);
        procedure.setSwallowing(true, 90);
        QVERIFY(written(whatToTell(procedure, 91)).isEmpty());
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, true, 100), passedOnAndTold);
        QCOMPARE(written(whatToTell(procedure, 110)), QStringList{u"typed"_s});
        // Not said where the keys go to no machine any more by the time it would be.
        procedure.setSwallowing(false, 120);
        procedure.setSwallowing(true, 130);
        QCOMPARE(procedure.key(keyA.name, keyA.scanCode, keyA.flags | injected, true, 140), passedOnAndTold);
        procedure.fallBehind(150);
        QCOMPARE(written(whatToTell(procedure, 160)), QStringList{u"behind"_s});
    }

    void aKeyThatIsOnlySaidAgainIsNoKeyGoingDownForTheLeaveKey_data() { eachWayOfTheAccounts(); }

    void aKeyThatIsOnlySaidAgainIsNoKeyGoingDownForTheLeaveKey()
    {
        QFETCH(int, way);
        // A key is held on a second keyboard, which goes on saying it; the leave-key is pressed
        // and let go on the first. No other key went down between: the keyboard is given back.
        // (The key that was said again counted as one, and the leave-key did not leave for as
        // long as it was held.)
        Procedure procedure;
        swallowing(procedure, static_cast<Accounts>(way));
        const Keyboard keys{&procedure, static_cast<Accounts>(way)};
        keys.press(keyA, 10);
        keys.press(rightCtrl, 20);
        keys.press(keyA, 43);
        keys.press(keyA, 76);
        QVERIFY(procedure.swallows());
        keys.release(rightCtrl, 90);
        QVERIFY(!procedure.swallows());
        // A key that goes down between, for the first time, still counts.
        Procedure used;
        swallowing(used, static_cast<Accounts>(way));
        const Keyboard usedKeys{&used, static_cast<Accounts>(way)};
        usedKeys.press(rightCtrl, 20);
        usedKeys.press(keyA, 30);
        usedKeys.press(keyA, 63);
        usedKeys.release(rightCtrl, 90);
        QVERIFY(used.swallows());
        // The watch by itself.
        LeaveWatch watch;
        QVERIFY(!watch.take(0x04, true));
        QVERIFY(!watch.take(leave, true));
        QVERIFY(!watch.take(0x04, true, true));
        QVERIFY(watch.take(leave, false));
    }

    // --- with the gate armed, as on every PC with Raw Input ---

    void aKeyThatFindsNoRoomToWaitInIsWindowsAndEndsTheSwallowing()
    {
        using ui::hook::KeysWaiting;
        // As many keys as wait at a time, each for the other account's word, which does not
        // come: by their numbers, from 1 on the top row to L.
        std::vector<Key> many;
        for (std::uint32_t scanCode = 0x02; many.size() <= KeysWaiting::places; ++scanCode) {
            if (input::usageOf({scanCode, false, 0x30}) != 0 && scanCode != leftCtrl.scanCode) {
                many.push_back({0x30 + scanCode, scanCode, 0});
            }
        }
        const Key oneMore = many.back();
        many.pop_back();
        QCOMPARE(many.size(), KeysWaiting::places);
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        std::multiset<int> usages;
        for (const Key& key : many) {
            QCOMPARE(press(procedure, key, 100), swallowedUntold);
            usages.insert(input::usageOf(ui::hook::pressedOf(key.name, key.scanCode, key.flags)));
        }
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{KeysWaiting::places});
        QVERIFY(told(procedure).isEmpty());
        // The next one finds no room. It is not kept from Windows (nothing is that cannot be
        // told), it does not go out past the others, and it is not dropped without a word:
        // keys have fallen behind, and whoever listens lets go of everything.
        QCOMPARE(press(procedure, oneMore, 110), passedOnAndTold);
        QVERIFY(told(procedure).isEmpty());
        QCOMPARE(written(ui::hook::whatToTell(procedure, 120)), QStringList{u"behind"_s});
        QVERIFY(procedure.isClosed());
        // What waited is typed nowhere, and is known as down, kept from Windows: asked of the
        // table. The one that found no room is Windows' own.
        static_cast<void>(procedure.look(130));
        QVERIFY(told(procedure).isEmpty());
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
        QCOMPARE(kept(procedure), usages);
        // Each goes up, and that is told of those that had been kept from Windows; of the one
        // that Windows had, to nobody.
        QCOMPARE(release(procedure, oneMore, 200), passedOn);
        QCOMPARE(release(procedure, many.front(), 210), passedOnAndTold);
        QCOMPARE(told(procedure).size(), qsizetype{1});

        // A key that goes up needs no room to wait in where its going down was handed on: it
        // goes at once, with the row full of other keys. (It fell behind, for want of room,
        // and Enter stayed down on the machine until whoever listens had let go of everything.)
        Procedure enter;
        swallowing(enter);
        enter.rawInputTells();
        QCOMPARE(press(enter, keyEnter, 50), swallowedUntold);
        QVERIFY(enter.rawKey(rawEnter, true, 50));
        QCOMPARE(told(enter), QStringList{u"+28"_s});
        for (const Key& key : many) {
            if (key.scanCode != keyEnter.scanCode) {
                QCOMPARE(press(enter, key, 100), swallowedUntold);
            }
        }
        QCOMPARE(press(enter, oneMore, 100), swallowedUntold);
        QCOMPARE(enter.onTheirWay().count, std::uint32_t{KeysWaiting::places});
        QVERIFY(release(enter, keyEnter, 110).tell);
        QCOMPARE(told(enter), QStringList{u"-28"_s});
        QVERIFY(!enter.takeFellBehind());
        QVERIFY(enter.swallows());

        // A modifier that goes up stays behind the keys that went down under it and wait, and
        // there it has no room: that is marked, and whoever listens lets go of everything,
        // the modifier among it. Nothing of it went out past the others.
        Procedure full;
        swallowing(full);
        full.rawInputTells();
        QCOMPARE(press(full, leftCtrl, 50), swallowedUntold);
        QVERIFY(full.rawKey(rawCtrl, true, 50));
        QCOMPARE(told(full), QStringList{u"+e0"_s});
        for (const Key& key : many) {
            QCOMPARE(press(full, key, 100), swallowedUntold);
        }
        QVERIFY(release(full, leftCtrl, 110).tell);
        QVERIFY(told(full).isEmpty());
        QVERIFY(full.takeFellBehind());
    }

    void theTwoAccountsInEitherOrderWithAFastPairOfKeys_data()
    {
        QTest::addColumn<bool>("hookFirst");
        QTest::addColumn<bool>("allOfOneFirst");
        QTest::addColumn<QStringList>("handedOn");
        // (A key that goes up is handed on as soon as its own going down was: where the hook
        // heard all four before the first telling came, A goes up on the machine before B goes
        // down there. What is typed is the same.)
        QTest::newRow("the hook hears each key first")
            << true << false << QStringList{u"+4"_s, u"+5"_s, u"-4"_s, u"-5"_s};
        QTest::newRow("the hook hears all four before Raw Input tells of the first")
            << true << true << QStringList{u"+4"_s, u"-4"_s, u"+5"_s, u"-5"_s};
        QTest::newRow("Raw Input tells of each key first")
            << false << false << QStringList{u"+4"_s, u"+5"_s, u"-4"_s, u"-5"_s};
        QTest::newRow("Raw Input tells of all four before the hook hears the first")
            << false << true << QStringList{u"+4"_s, u"+5"_s, u"-4"_s, u"-5"_s};
    }

    void theTwoAccountsInEitherOrderWithAFastPairOfKeys()
    {
        QFETCH(bool, hookFirst);
        QFETCH(bool, allOfOneFirst);
        QFETCH(QStringList, handedOn);
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // A down, B down, A up, B up, a millisecond apart.
        struct Step {
            Key key;
            bool down = false;
            std::uint64_t at = 0;
        };
        const std::array<Step, 4> steps = {
            {{keyA, true, 100}, {keyB, true, 101}, {keyA, false, 102}, {keyB, false, 103}}};
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        const auto hears = [&](const Step& step) {
            const Verdict verdict = procedure.key(step.key.name, step.key.scanCode, step.key.flags, step.down, step.at);
            QVERIFY(verdict.swallow);
        };
        const auto tells = [&](const Step& step) {
            static_cast<void>(procedure.rawKey(ui::hook::pressedOf(step.key.name, step.key.scanCode, step.key.flags),
                                               step.down, step.at));
        };
        if (allOfOneFirst) {
            for (const Step& step : steps) {
                hookFirst ? hears(step) : tells(step);
            }
            for (const Step& step : steps) {
                hookFirst ? tells(step) : hears(step);
            }
        } else {
            for (const Step& step : steps) {
                hookFirst ? hears(step) : tells(step);
                hookFirst ? tells(step) : hears(step);
            }
        }
        QCOMPARE(told(procedure), handedOn);
        // The two are at one: nothing waits, nothing ends, however long one looks.
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
        procedure.stillThere(100 + 5 * moment);
        QVERIFY(!procedure.look(103 + 5 * moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotTold());
        QVERIFY(!procedure.takeFellBehind());
        QVERIFY(procedure.isWatched());
        QVERIFY(down(procedure).empty());
    }

    void theTwoAccountsInEitherOrderWithAKeyThatIsSaidAgainThirtyTimesASecond_data()
    {
        QTest::addColumn<int>("order");
        QTest::newRow("the hook hears each saying first") << 0;
        QTest::newRow("Raw Input tells of each saying first") << 1;
        QTest::newRow("the hook hears three sayings before Raw Input tells of the first of them") << 2;
        QTest::newRow("Raw Input tells of three sayings before the hook hears the first of them") << 3;
    }

    void theTwoAccountsInEitherOrderWithAKeyThatIsSaidAgainThirtyTimesASecond()
    {
        QFETCH(int, order);
        const input::Pressed raw = ui::hook::pressedOf(keyEnter.name, keyEnter.scanCode, keyEnter.flags);
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        // Enter is held for two seconds: said every 33 ms, sixty-one times, and let go. The
        // interface thread takes what is handed on four times a second, and the hook's thread
        // looks ten times a second.
        constexpr int sayings = 61;
        int handedOn = 0;
        const auto lookAndTake = [&](std::uint64_t at) {
            QVERIFY(!procedure.look(at).wake);
            for (const QString& one : written(ui::hook::whatToTell(procedure, at))) {
                QCOMPARE(one, u"+28"_s);
                ++handedOn;
            }
        };
        std::uint64_t at = 100;
        for (int said = 0; said < sayings; said += 3) {
            const int group = std::min(3, sayings - said);
            const auto when = [&](int which) { return at + static_cast<std::uint64_t>(33 * which); };
            const auto hears = [&](int which) {
                QVERIFY(procedure.key(keyEnter.name, keyEnter.scanCode, keyEnter.flags, true, when(which)).swallow);
            };
            const auto tells = [&](int which) { static_cast<void>(procedure.rawKey(raw, true, when(which))); };
            for (int which = 0; which < group && order < 2; ++which) {
                order == 0 ? hears(which) : tells(which);
                order == 0 ? tells(which) : hears(which);
            }
            for (int which = 0; which < group && order >= 2; ++which) {
                order == 2 ? hears(which) : tells(which);
            }
            for (int which = 0; which < group && order >= 2; ++which) {
                order == 2 ? tells(which) : hears(which);
            }
            at += 99;
            lookAndTake(at);
        }
        // Every saying is handed on, but those that came while the key's own going down still
        // waited: they add nothing to that. Where one account is three sayings ahead, those are
        // two of the hook's first three, or one. (Turned round: this had all sixty-one handed
        // on in every order, each in a place of its own in the row.)
        const int addedNothing = order == 2 ? 2 : (order == 3 ? 1 : 0);
        QCOMPARE(handedOn, sayings - addedNothing);
        // Let go: told at once, by the hook's word (it waits for no telling), and with it no
        // key is down.
        if (order % 2 == 1) {
            static_cast<void>(procedure.rawKey(raw, false, at + 10));
        }
        QVERIFY(procedure.key(keyEnter.name, keyEnter.scanCode, keyEnter.flags, false, at + 10).tell);
        if (order % 2 == 0) {
            static_cast<void>(procedure.rawKey(raw, false, at + 10));
        }
        QCOMPARE(written(ui::hook::whatToTell(procedure, at + 11)), (QStringList{u"-28"_s, u"none down"_s}));
        QVERIFY(procedure.swallows());
        QVERIFY(procedure.isWatched());
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
        QVERIFY(!procedure.look(at + 5 * ui::hook::heardWithinMs).wake);
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotTold());
    }

    // --- what a fifth reviewer found ---

    void numLockIsOneKeyAtEitherOfItsTwoPlaces()
    {
        using ui::hook::whatToTell;
        // Were Windows to tell a hook of Num Lock under Ctrl without its mark, as "Pause" (not
        // known whether it does: `input::usageOf`), the key would be at one place of the table
        // while Ctrl is held and at another once Ctrl was let go, with two USB numbers. Ctrl
        // down, Num Lock down, Ctrl up, Num Lock up: what went down on the machine goes up
        // there, and with it no key is down. (The "up" found its own place not down and was
        // nothing: what had gone down stayed down on the machine, and the table never came
        // back to holding no key, for as long as that hook lived.)
        Procedure procedure;
        swallowing(procedure);
        QCOMPARE(press(procedure, leftCtrl, 10), swallowedAndTold);
        QCOMPARE(press(procedure, keyPause, 20), swallowedAndTold);
        QCOMPARE(release(procedure, leftCtrl, 30), swallowedAndTold);
        QCOMPARE(release(procedure, keyNumLock, 40), swallowedAndTold);
        QCOMPARE(written(whatToTell(procedure, 50)),
                 (QStringList{u"+e0"_s, u"+48"_s, u"-e0"_s, u"-48"_s, u"none down"_s}));
        QVERIFY(down(procedure).empty());
        QVERIFY(kept(procedure).empty());
        // The other way round: down by itself, with the mark, and let go under Ctrl.
        QCOMPARE(press(procedure, keyNumLock, 60), swallowedAndTold);
        QCOMPARE(press(procedure, leftCtrl, 70), swallowedAndTold);
        QCOMPARE(release(procedure, keyPause, 80), swallowedAndTold);
        QCOMPARE(release(procedure, leftCtrl, 90), swallowedAndTold);
        QCOMPARE(written(whatToTell(procedure, 95)),
                 (QStringList{u"+53"_s, u"+e0"_s, u"-53"_s, u"-e0"_s, u"none down"_s}));
        // And said again at the other place while it is held: the same key said again, and no
        // second key going down on the machine.
        QCOMPARE(press(procedure, keyNumLock, 100), swallowedAndTold);
        QCOMPARE(press(procedure, leftCtrl, 110), swallowedAndTold);
        QCOMPARE(press(procedure, keyPause, 120), swallowedAndTold);
        QCOMPARE(down(procedure), (std::multiset<int>{0x53, 0xE0}));
        QCOMPARE(release(procedure, leftCtrl, 130), swallowedAndTold);
        QCOMPARE(release(procedure, keyNumLock, 140), swallowedAndTold);
        QCOMPARE(written(whatToTell(procedure, 150)),
                 (QStringList{u"+53"_s, u"+e0"_s, u"+53"_s, u"-e0"_s, u"-53"_s, u"none down"_s}));
        QVERIFY(down(procedure).empty());
        // No other pair of places is one key so: Enter and the Enter of the number block are
        // two keys, and the going up of the one, whose going down this hook did not hear, lets
        // go of nothing of the other, which a finger holds.
        QCOMPARE(press(procedure, keyEnter, 200), swallowedAndTold);
        QCOMPARE(procedure.key(keyEnter.name, keyEnter.scanCode, ui::hook::markedFlag, false, 210), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+28"_s, u"-58"_s}));
        QCOMPARE(down(procedure), (std::multiset<int>{0x28}));
    }

    void anUpNamedAsNoKeyOfTheKeyboardsIsNotThatKeysAndEndsTheControl_data()
    {
        QTest::addColumn<bool>("hookFirst");
        QTest::newRow("the hook hears of the going up first") << true;
        QTest::newRow("Raw Input tells of the going up first") << false;
    }

    void anUpNamedAsNoKeyOfTheKeyboardsIsNotThatKeysAndEndsTheControl()
    {
        QFETCH(bool, hookFirst);
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // (Turned round, and named anew. It was "an up named otherwise than its down which
        // nobody has seen is let go by the other account's word": the right Shift's "up", told
        // to the hook as "Shift" with the mark, was the right Shift's to the comparing, which
        // read a key that is none with its mark as the key without it. The two accounts were at
        // one, and the key was let go by Raw Input's own word, with the control going on. That
        // reading is gone: by it a key for sound was the letter that shares its number. What
        // the letting go by that account's word stands for now:
        // `whatRawInputHasAsUpIsLetGoWhereItsTellingOfTheUpWasForgiven`.)
        // The right Shift goes down, heard and told of: it is down on the machine. Left Ctrl is
        // held with it. The Shift goes up, and the hook is told so as "Shift", with the mark
        // that the hook's right Shift comes with: no key of the keyboard's, and so nothing to
        // the comparing. (Not known to happen. It stands for every "up" that Windows may name
        // in a way not foreseen here.) It lets nothing go. Raw Input tells of the right Shift
        // going up, and that waits in vain to be heard. After the moment the control ends,
        // said as a key that Windows saw and that was not handed over; and the Shift, which
        // was told of as gone up, is let go and told as that, while Ctrl stays down in the
        // table, as a key that is still held.
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        QCOMPARE(press(procedure, leftCtrl, 90), swallowedUntold);
        QVERIFY(procedure.rawKey(rawCtrl, true, 91));
        QCOMPARE(press(procedure, keyRightShift, 100), swallowedUntold);
        QVERIFY(tellAsRawInput(procedure, rawRightShift, true, 101));
        QCOMPARE(told(procedure), (QStringList{u"+e0"_s, u"+e5"_s}));
        if (hookFirst) {
            QCOMPARE(release(procedure, keyRightShiftAsShift, 300), swallowedAndTold);
            QVERIFY(!tellAsRawInput(procedure, rawRightShift, false, 301));
        } else {
            QVERIFY(!tellAsRawInput(procedure, rawRightShift, false, 300));
            QCOMPARE(release(procedure, keyRightShiftAsShift, 301), swallowedAndTold);
        }
        // (Told as what it is: a key with no USB number going up, which is nothing to a machine.)
        QCOMPARE(written(whatToTell(procedure, 310)), QStringList{u"-0"_s});
        QCOMPARE(down(procedure), (std::multiset<int>{0xE0, 0xE5}));
        QVERIFY(!procedure.look(300 + moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(procedure.look(302 + moment).wake);
        QVERIFY(procedure.isClosed());
        QCOMPARE(written(whatToTell(procedure, 303 + moment)),
                 (QStringList{lost(ui::KeySource::Lost::NotHandedKeys), u"-e5"_s}));
        QCOMPARE(down(procedure), (std::multiset<int>{0xE0}));
        QCOMPARE(kept(procedure), (std::multiset<int>{0xE0}));
    }

    void aKeyThatAFingerHoldsIsNotLetGoOnTheOtherAccountsWord()
    {
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // What the other account's word must never do: let go a key that a finger holds. (These
        // held a letting go by that account's own word to what it might go by. It is gone, with
        // the forgiving it stood behind: nothing came to it any more. They stay, for what they
        // show whichever way a key is let go: by its own "up" as it is heard, and by nothing
        // that the other account tells meanwhile.)
        // A key that was down as the comparing began: of its sayings one account may tell and
        // the other never.
        Procedure before;
        before.begin(0);
        before.windowsHas(keyEnter.name);
        before.isDownAlready(rawEnter);
        before.rawInputTells();
        before.setSwallowing(true, 0);
        QCOMPARE(press(before, keyEnter, 100), swallowedAndTold);
        QCOMPARE(press(before, keyA, 200), swallowedUntold);
        QVERIFY(tellAsRawInput(before, rawLetterA, true, 201));
        QCOMPARE(release(before, keyA, 250), swallowedAndTold);
        QVERIFY(!tellAsRawInput(before, rawLetterA, false, 251));
        QCOMPARE(told(before), (QStringList{u"w+28"_s, u"+4"_s, u"-4"_s}));
        QCOMPARE(down(before), (std::multiset<int>{0x28}));

        // A key that went down before that account told of this keyboard at all: it went on
        // after its moment. The account then tells of another key, down and up: of the first
        // key it never said that it went down, and what it tells lets nothing of it go.
        Procedure late;
        swallowing(late);
        late.rawInputTells();
        QCOMPARE(press(late, leftShift, 100), swallowedUntold);
        QVERIFY(late.look(101 + moment).wake);
        QCOMPARE(told(late), QStringList{u"+e1"_s});
        late.stillThere(600);
        QCOMPARE(press(late, keyA, 600), swallowedAndTold);
        QVERIFY(!tellAsRawInput(late, rawLetterA, true, 601));
        QCOMPARE(release(late, keyA, 650), swallowedAndTold);
        QVERIFY(!tellAsRawInput(late, rawLetterA, false, 651));
        QVERIFY(!late.look(700).wake);
        QCOMPARE(told(late), (QStringList{u"+4"_s, u"-4"_s}));
        QCOMPARE(down(late), (std::multiset<int>{0xE1}));

        // A key that is let go and pressed again faster than the other account's words come:
        // its word for the first going up comes when the second press is heard already. That
        // press is not what it told of, and stays.
        Procedure fast;
        swallowing(fast);
        fast.rawInputTells();
        QCOMPARE(press(fast, keyA, 100), swallowedUntold);
        QVERIFY(tellAsRawInput(fast, rawLetterA, true, 100));
        QCOMPARE(release(fast, keyA, 110), swallowedAndTold);
        QCOMPARE(press(fast, keyA, 120), swallowedUntold);
        QVERIFY(!tellAsRawInput(fast, rawLetterA, false, 121));
        QCOMPARE(down(fast), (std::multiset<int>{0x04}));
        QVERIFY(tellAsRawInput(fast, rawLetterA, true, 122));
        QCOMPARE(told(fast), (QStringList{u"+4"_s, u"-4"_s, u"+4"_s}));
        QCOMPARE(down(fast), (std::multiset<int>{0x04}));

        // A key for sound comes with a letter's number and the mark. It has no USB number of
        // the keyboard's page, and is nothing to the comparing, heard or told of (no key is
        // read with its mark turned any more, by which it was that letter): the key for sound
        // going up is not the letter going up, which a finger holds.
        Procedure sound;
        swallowing(sound);
        sound.rawInputTells();
        QCOMPARE(press(sound, keyB, 100), swallowedUntold);
        QVERIFY(tellAsRawInput(sound, rawLetterB, true, 101));
        QCOMPARE(press(sound, louder, 200), swallowedAndTold);
        static_cast<void>(tellAsRawInput(sound, rawLouder, true, 201));
        QCOMPARE(release(sound, louder, 250), swallowedAndTold);
        static_cast<void>(tellAsRawInput(sound, rawLouder, false, 251));
        QVERIFY(!sound.look(300).wake);
        QCOMPARE(told(sound), (QStringList{u"+5"_s, u"+0"_s, u"-0"_s}));
        QCOMPARE(down(sound), (std::multiset<int>{0x05}));

        // A key of which Raw Input tells with a mark that the hook does not give it (foreseen of
        // no key). (Turned round: the comparing took the two for one key, read with the mark
        // turned, and the press went to the machine. No key is read so any more: by that
        // reading a key for sound was the letter that shares its number.) What Raw Input tells
        // of is no key of the keyboard's page, and nothing to the comparing: the press waits
        // for a word that does not come. After the moment the control ends, said as a key that
        // Windows did not report. The key went to no machine, and it stays down in the table,
        // kept from Windows: Raw Input's word lets nothing go.
        constexpr RawKey letterAWithAMark{0x1E, ui::hook::rawMarkedFlag, 0x41};
        for (const bool hookFirst : {true, false}) {
            Procedure odd;
            swallowing(odd);
            odd.rawInputTells();
            if (hookFirst) {
                QCOMPARE(press(odd, keyA, 100), swallowedUntold);
                QVERIFY(!tellAsRawInput(odd, letterAWithAMark, true, 101));
            } else {
                QVERIFY(!tellAsRawInput(odd, letterAWithAMark, true, 100));
                QCOMPARE(press(odd, keyA, 101), swallowedUntold);
            }
            QVERIFY(!odd.look(200).wake);
            QVERIFY(told(odd).isEmpty());
            QVERIFY(odd.look(102 + moment).wake);
            QVERIFY(odd.isClosed());
            QCOMPARE(written(ui::hook::whatToTell(odd, 103 + moment)),
                     QStringList{lost(ui::KeySource::Lost::NotToldOfKeys)});
            QCOMPARE(down(odd), (std::multiset<int>{0x04}));
            QCOMPARE(kept(odd), (std::multiset<int>{0x04}));
        }

        // And where that account is told to another window's hook from here on, its word
        // counts for nothing that is down.
        Procedure other;
        swallowing(other);
        other.rawInputTells();
        QCOMPARE(press(other, leftShift, 100), swallowedUntold);
        QVERIFY(tellAsRawInput(other, rawLeftShift, true, 101));
        static_cast<void>(told(other));
        other.rawInputTellsNoMore();
        QVERIFY(!other.look(200).wake);
        QCOMPARE(press(other, keyA, 300), swallowedAndTold);
        QCOMPARE(release(other, keyA, 350), swallowedAndTold);
        QCOMPARE(told(other), (QStringList{u"+4"_s, u"-4"_s}));
        QCOMPARE(down(other), (std::multiset<int>{0xE1}));
    }

    void whatWindowsCannotNameIsNoKeyGoingDownOrUp()
    {
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // Shift is held, and Home is pressed under it: a keyboard makes up a Shift going up
        // before Home and one going down after it. Were Windows to hand a hook those, without a
        // name and at Shift's own place (not known: it may drop them, or give them a number no
        // key has), the Shift that a finger holds does not go up for it: Shift+Home arrives as
        // that, and not as Shift up, then Home. (An "up" without a name at the place of a key
        // that was down let that key go.)
        Procedure procedure;
        swallowing(procedure);
        QCOMPARE(press(procedure, leftShift, 10), swallowedAndTold);
        QCOMPARE(procedure.key(noName, leftShift.scanCode, 0, false, 20), swallowedAndTold);
        QCOMPARE(press(procedure, keyHome, 21), swallowedAndTold);
        QCOMPARE(release(procedure, keyHome, 30), swallowedAndTold);
        QCOMPARE(procedure.key(noName, leftShift.scanCode, 0, true, 31), swallowedAndTold);
        // (Told as what it is: a key with no USB number, which is nothing to a machine.)
        QCOMPARE(told(procedure), (QStringList{u"+e1"_s, u"-0"_s, u"+4a"_s, u"-4a"_s, u"+0"_s}));
        QCOMPARE(down(procedure), (std::multiset<int>{0xE1}));
        QCOMPARE(kept(procedure), (std::multiset<int>{0xE1}));
        QCOMPARE(release(procedure, leftShift, 40), swallowedAndTold);
        QCOMPARE(written(whatToTell(procedure, 50)), (QStringList{u"-e1"_s, u"none down"_s}));
        // With the mark, as a keyboard sends it: the same. And what "goes down" so is not kept
        // as a key that is down (nothing would ever let it go, and the table would never hold
        // no key again).
        QCOMPARE(procedure.key(noName, leftShift.scanCode, ui::hook::markedFlag, false, 60), swallowedAndTold);
        QCOMPARE(procedure.key(noName, leftShift.scanCode, ui::hook::markedFlag, true, 70), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"-0"_s, u"+0"_s}));
        QVERIFY(down(procedure).empty());
        QVERIFY(kept(procedure).empty());
        // Nor is a key let go by an "up" at its place that has no name: it is held until its
        // own going up comes.
        QCOMPARE(press(procedure, keyA, 80), swallowedAndTold);
        QCOMPARE(procedure.key(noName, keyA.scanCode, 0, false, 90), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+4"_s, u"-0"_s}));
        QCOMPARE(down(procedure), (std::multiset<int>{0x04}));
        QCOMPARE(release(procedure, keyA, 95), swallowedAndTold);
        QCOMPARE(written(whatToTell(procedure, 99)), (QStringList{u"-4"_s, u"none down"_s}));

        // With the second account: Raw Input tells of a made-up Shift without a name too, which
        // is no key there. Nothing waits for it, nothing ends, and Shift stays down by both.
        QVERIFY(!asRawInputTells(rawMadeUpShift, true).has_value());
        QVERIFY(!asRawInputTells(rawMadeUpShift, false).has_value());
        Procedure both;
        swallowing(both);
        both.rawInputTells();
        QCOMPARE(press(both, leftShift, 100), swallowedUntold);
        QVERIFY(tellAsRawInput(both, rawLeftShift, true, 101));
        QVERIFY(!tellAsRawInput(both, rawMadeUpShift, false, 110));
        QCOMPARE(both.key(noName, leftShift.scanCode, ui::hook::markedFlag, false, 110), swallowedAndTold);
        QCOMPARE(press(both, keyHome, 111), swallowedUntold);
        QVERIFY(tellAsRawInput(both, rawHome, true, 112));
        QCOMPARE(release(both, keyHome, 130), swallowedAndTold);
        QVERIFY(!tellAsRawInput(both, rawHome, false, 131));
        QVERIFY(!tellAsRawInput(both, rawMadeUpShift, true, 132));
        QCOMPARE(both.key(noName, leftShift.scanCode, ui::hook::markedFlag, true, 132), swallowedAndTold);
        QCOMPARE(told(both), (QStringList{u"+e1"_s, u"-0"_s, u"+4a"_s, u"-4a"_s, u"+0"_s}));
        QVERIFY(!both.look(132 + moment + ui::hook::lookEveryMs).wake);
        QVERIFY(both.swallows());
        QVERIFY(!both.takeNotHanded());
        QVERIFY(!both.takeNotTold());
        QCOMPARE(down(both), (std::multiset<int>{0xE1}));
    }

    void aKeyThatIsSaidTenTimesBeforeTheOtherAccountsFirstWordWaitsForNoneOfThem_data()
    {
        QTest::addColumn<bool>("hookFirst");
        QTest::newRow("the hook hears ten sayings and the going up before Raw Input's first word") << true;
        QTest::newRow("Raw Input tells of ten sayings and the going up before the hook hears") << false;
    }

    void aKeyThatIsSaidTenTimesBeforeTheOtherAccountsFirstWordWaitsForNoneOfThem()
    {
        QFETCH(bool, hookFirst);
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        constexpr std::uint64_t sayings = 10;
        // Enter is held and said thirty times a second, and one account is ten sayings and the
        // going up ahead of the other (whether Windows can be so far ahead with the hook is not
        // known; nothing depends on it). A key that is only said again needs no word of its own
        // and no room of its own: nothing ends by how many they are, Enter goes down on the
        // machine once, and goes up there as soon as its going down has gone. (From the fourth
        // saying on there was no room to note it: a telling found no hearing, or a hearing no
        // telling, the control ended a moment later, and Enter's going up waited behind a
        // saying that never went, with the machine saying Enter again all the while.)
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        const auto hears = [&](bool down, std::uint64_t at) {
            return procedure.key(keyEnter.name, keyEnter.scanCode, keyEnter.flags, down, at);
        };
        const auto tells = [&](bool down, std::uint64_t at) {
            static_cast<void>(procedure.rawKey(rawEnter, down, at));
        };
        constexpr std::uint64_t firstAt = 100;
        constexpr std::uint64_t upAt = firstAt + 33 * sayings;
        for (std::uint64_t said = 0; said < sayings; ++said) {
            const std::uint64_t at = firstAt + 33 * said;
            if (hookFirst) {
                QVERIFY(hears(true, at).swallow);
            } else {
                tells(true, at);
            }
        }
        if (hookFirst) {
            // (Its going up stays behind its own going down, which waits; and nothing else is
            // on its way.)
            QCOMPARE(hears(false, upAt), swallowedUntold);
            QCOMPARE(procedure.onTheirWay().count, std::uint32_t{2});
        } else {
            tells(false, upAt);
        }
        QVERIFY(told(procedure).isEmpty());
        for (std::uint64_t said = 0; said < sayings; ++said) {
            const std::uint64_t at = upAt + 1 + said;
            if (hookFirst) {
                tells(true, at);
            } else {
                QVERIFY(hears(true, at).swallow);
            }
        }
        if (hookFirst) {
            tells(false, upAt + 20);
        } else {
            QVERIFY(hears(false, upAt + 20).swallow);
        }
        QCOMPARE(written(whatToTell(procedure, upAt + 21)), (QStringList{u"+28"_s, u"-28"_s, u"none down"_s}));
        // The two are at one: nothing waits, and nothing ends, however long one looks.
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
        procedure.stillThere(upAt + 5 * moment);
        QVERIFY(!procedure.look(upAt + 5 * moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotTold());
        QVERIFY(!procedure.takeFellBehind());
        QVERIFY(procedure.isWatched());
        QVERIFY(down(procedure).empty());
        // And the watch is not blunted by it: a saying that only Raw Input tells of, once the
        // key is held again, says that the hook did not hear, as ever.
        constexpr std::uint64_t again = upAt + 6 * moment;
        QCOMPARE(hears(true, again), swallowedUntold);
        tells(true, again + 1);
        tells(true, again + 34);
        procedure.stillThere(again + 34 + moment);
        QVERIFY(!procedure.look(again + 34 + moment).wake);
        QVERIFY(procedure.look(again + 35 + moment).wake);
        QVERIFY(procedure.takeNotHanded());
    }

    void theKeysOfWhichTheTwoAccountsTellDifferentlyPassTheGateAsRawInputTellsOfThem_data()
    {
        QTest::addColumn<quint32>("name");
        QTest::addColumn<quint32>("scanCode");
        QTest::addColumn<quint32>("flags");
        QTest::addColumn<quint32>("rawMakeCode");
        QTest::addColumn<quint32>("rawFlags");
        QTest::addColumn<quint32>("rawName");
        QTest::addColumn<int>("usage");
        constexpr quint32 marked = ui::hook::markedFlag;
        constexpr quint32 rawMarked = ui::hook::rawMarkedFlag;
        const auto key = [](const char* which, quint32 name, quint32 scanCode, quint32 flags, quint32 rawMakeCode,
                            quint32 rawFlags, quint32 rawName, int usage) {
            QTest::newRow(which) << name << scanCode << flags << rawMakeCode << rawFlags << rawName << usage;
        };
        // What the hook is told of each, and what Raw Input says of it: as I remember them
        // (the names Raw Input gives are sure, the marks fairly sure; `keyboard_hook.h`).
        key("Pause, whose first half has Ctrl's number there", 0x13, 0x45, 0, 0x1D, ui::hook::rawPauseFlag, 0x13, 0x48);
        key("Num Lock, marked here and not there", 0x90, 0x45, marked, 0x45, 0, 0x90, 0x53);
        key("Print Screen", 0x2C, 0x37, marked, 0x37, rawMarked, 0x2C, 0x46);
        key("Print Screen under Alt, with another number", 0x2C, 0x54, 0, 0x54, 0, 0x2C, 0x46);
        key("the right Shift, marked here and not there, 'Shift' there", 0xA1, 0x36, marked, 0x36, 0, 0x10, 0xE5);
        key("the left Shift, 'Shift' there", 0xA0, 0x2A, 0, 0x2A, 0, 0x10, 0xE1);
        // (Not the right Ctrl, 'Ctrl' there: pressed and let go by itself it is the way out,
        // which the tests of the leave-key try each way.)
        key("the right Alt of AltGr, 'Alt' there", 0xA5, 0x38, marked, 0x38, rawMarked, 0x12, 0xE6);
        key("Enter of the number block", 0x0D, 0x1C, marked, 0x1C, rawMarked, 0x0D, 0x58);
        key("/ of the number block", 0x6F, 0x35, marked, 0x35, rawMarked, 0x6F, 0x54);
        key("Home", 0x24, 0x47, marked, 0x47, rawMarked, 0x24, 0x4A);
        key("up", 0x26, 0x48, marked, 0x48, rawMarked, 0x26, 0x52);
        key("Page Up", 0x21, 0x49, marked, 0x49, rawMarked, 0x21, 0x4B);
        key("left", 0x25, 0x4B, marked, 0x4B, rawMarked, 0x25, 0x50);
        key("right", 0x27, 0x4D, marked, 0x4D, rawMarked, 0x27, 0x4F);
        key("End", 0x23, 0x4F, marked, 0x4F, rawMarked, 0x23, 0x4D);
        key("down", 0x28, 0x50, marked, 0x50, rawMarked, 0x28, 0x51);
        key("Page Down", 0x22, 0x51, marked, 0x51, rawMarked, 0x22, 0x4E);
        key("Insert", 0x2D, 0x52, marked, 0x52, rawMarked, 0x2D, 0x49);
        key("Del", 0x2E, 0x53, marked, 0x53, rawMarked, 0x2E, 0x4C);
        key("7 of the number block, named Home with Num Lock off", 0x24, 0x47, 0, 0x47, 0, 0x24, 0x5F);
    }

    void theKeysOfWhichTheTwoAccountsTellDifferentlyPassTheGateAsRawInputTellsOfThem()
    {
        QFETCH(quint32, name);
        QFETCH(quint32, scanCode);
        QFETCH(quint32, flags);
        QFETCH(quint32, rawMakeCode);
        QFETCH(quint32, rawFlags);
        QFETCH(quint32, rawName);
        QFETCH(int, usage);
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        const Key hooked{name, scanCode, flags};
        const RawKey raw{rawMakeCode, rawFlags, rawName};
        const QString pressed = u"+"_s + QString::number(usage, 16);
        const QString letGo = u"-"_s + QString::number(usage, 16);
        // (It is a key of its own there, going either way.)
        QVERIFY(asRawInputTells(raw, true).has_value());
        QVERIFY(asRawInputTells(raw, false).has_value());
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        // The hook hears first: the key waits for Raw Input's word, and goes with it.
        QCOMPARE(press(procedure, hooked, 100), swallowedUntold);
        QVERIFY(told(procedure).isEmpty());
        QVERIFY(tellAsRawInput(procedure, raw, true, 101));
        QCOMPARE(told(procedure), QStringList{pressed});
        QCOMPARE(release(procedure, hooked, 200), swallowedAndTold);
        QVERIFY(!tellAsRawInput(procedure, raw, false, 201));
        QCOMPARE(told(procedure), QStringList{letGo});
        // Raw Input tells first: the key goes the moment it is heard.
        QVERIFY(!tellAsRawInput(procedure, raw, true, 300));
        QCOMPARE(press(procedure, hooked, 301), swallowedAndTold);
        QVERIFY(!tellAsRawInput(procedure, raw, false, 400));
        QCOMPARE(release(procedure, hooked, 401), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{pressed, letGo}));
        // The two are at one about it: nothing waits, and nothing ends.
        procedure.stillThere(401 + 5 * moment);
        QVERIFY(!procedure.look(401 + 5 * moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(procedure.isWatched());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotTold());
        QVERIFY(down(procedure).empty());
    }

    // --- what a sixth reviewer found ---

    void aKeyIsLetGoWhenAnyKeyboardLetsGoOfIt_data()
    {
        QTest::addColumn<bool>("hookFirst");
        QTest::newRow("the hook hears each key first") << true;
        QTest::newRow("Raw Input tells of each key first") << false;
    }

    void aKeyIsLetGoWhenAnyKeyboardLetsGoOfIt()
    {
        QFETCH(bool, hookFirst);
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // (Turned round, and named anew. It was "a key that another keyboard holds is not let
        // go by its up": Raw Input's account was kept for each keyboard, and the tap's "up"
        // below let nothing go. That telling apart is gone, by the owner's decision: every
        // attempt at it left a modifier down on a machine with nobody told, or let one go
        // under a finger without a word. The rule in its place is what this holds.)
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        std::uint64_t now = 100;
        // A key as both accounts tell of it, in the one order or the other: what became of it
        // for Windows. (Which keyboard it came from is said in the comments only: neither
        // account is asked, and nothing keeps it.)
        const auto both = [&](const Key& key, const input::Pressed& raw, bool down) {
            now += 10;
            if (!hookFirst) {
                static_cast<void>(procedure.rawKey(raw, down, now));
            }
            const Verdict verdict = procedure.key(key.name, key.scanCode, key.flags, down, now);
            if (hookFirst) {
                static_cast<void>(procedure.rawKey(raw, down, now));
            }
            return verdict;
        };
        // Ctrl is held on one keyboard, and C typed and let go there: Ctrl says itself again
        // no longer.
        both(leftCtrl, rawCtrl, true);
        both(keyC, rawC, true);
        both(keyC, rawC, false);
        QCOMPARE(written(whatToTell(procedure, now)), (QStringList{u"+e0"_s, u"+6"_s, u"-6"_s}));
        // Ctrl is tapped on a second keyboard (a macro pad or a scanner sends modifiers too).
        // Its going down is the key said again; and its "up" lets the key go, for all the
        // keyboards: in the table, and for whoever listens. With it no key is down.
        QVERIFY(both(leftCtrl, rawCtrl, true).swallow);
        QVERIFY(both(leftCtrl, rawCtrl, false).swallow);
        QCOMPARE(written(whatToTell(procedure, now)), (QStringList{u"+e0"_s, u"-e0"_s, u"none down"_s}));
        QVERIFY(down(procedure).empty());
        QVERIFY(kept(procedure).empty());
        // What that costs, and it is known: the finger is still on the first keyboard's Ctrl,
        // and the next key from there arrives bare. Whoever sees it presses the key again.
        both(keyC, rawC, true);
        both(keyC, rawC, false);
        QCOMPARE(written(whatToTell(procedure, now)), (QStringList{u"+6"_s, u"-6"_s, u"none down"_s}));
        // That finger leaves the key: an "up" of a key that is not down, which is told as that
        // and is nothing to a machine.
        QVERIFY(both(leftCtrl, rawCtrl, false).tell);
        QCOMPARE(written(whatToTell(procedure, now)), QStringList{u"-e0"_s});
        QVERIFY(down(procedure).empty());
        // The two accounts were at one all the while: nothing waits, and nothing ends.
        QVERIFY(!procedure.look(now + 5 * moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotTold());
        QVERIFY(procedure.notesNoHearing());

        // Where the first keyboard goes on saying its key (it is the key that went down last
        // there), the key goes down again with its next saying, as a press like any other: up
        // on the machine for the time between, and no longer.
        Procedure saidAgain;
        swallowing(saidAgain);
        saidAgain.rawInputTells();
        const auto bothOf = [&](bool down, std::uint64_t at) {
            if (!hookFirst) {
                static_cast<void>(saidAgain.rawKey(rawShift, down, at));
            }
            static_cast<void>(saidAgain.key(leftShift.name, leftShift.scanCode, leftShift.flags, down, at));
            if (hookFirst) {
                static_cast<void>(saidAgain.rawKey(rawShift, down, at));
            }
        };
        bothOf(true, 100);
        bothOf(true, 600);
        bothOf(true, 620);
        bothOf(false, 630);
        QCOMPARE(written(whatToTell(saidAgain, 631)),
                 (QStringList{u"+e1"_s, u"+e1"_s, u"+e1"_s, u"-e1"_s, u"none down"_s}));
        bothOf(true, 633);
        QCOMPARE(written(whatToTell(saidAgain, 634)), QStringList{u"+e1"_s});
        QCOMPARE(down(saidAgain), (std::multiset<int>{0xE1}));
        QCOMPARE(kept(saidAgain), (std::multiset<int>{0xE1}));
        // The last finger leaves it: up, and nothing is down.
        bothOf(false, 700);
        QCOMPARE(written(whatToTell(saidAgain, 701)), (QStringList{u"-e1"_s, u"none down"_s}));
        QVERIFY(saidAgain.swallows());
    }

    void aKeyGoesUpByItsUpAsItIsHeard_data()
    {
        QTest::addColumn<bool>("isAModifier");
        QTest::newRow("the left Shift, a modifier") << true;
        QTest::newRow("Enter, which is no modifier") << false;
    }

    void aKeyGoesUpByItsUpAsItIsHeard()
    {
        QFETCH(bool, isAModifier);
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        const Key key = isAModifier ? leftShift : keyEnter;
        const input::Pressed raw = isAModifier ? rawShift : rawEnter;
        const int usage = isAModifier ? 0xE1 : 0x28;
        const QString pressed = u"+"_s + QString::number(usage, 16);
        const QString letGo = u"-"_s + QString::number(usage, 16);
        // (In the place of two tests, of the modifiers and of the other keys, from the time
        // when keyboards were told apart: "an up that is heard before Raw Input has said whose
        // it was lets nothing go until it has", which held a modifier's "up" back in the row
        // until that account had said which keyboard let go, and ended the control where it
        // did not say; and "a key that is no modifier goes up by its up as it is heard". An
        // "up" waits for nothing that has to do with whose it was any more, of any key.)
        // The key is held until it says itself again, and let go while Raw Input's word for
        // that saying is still on its way (known to happen where the thread was slow for a
        // moment). It goes up as it is heard, and with it no key is down.
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        QCOMPARE(press(procedure, key, 100), swallowedUntold);
        QVERIFY(procedure.rawKey(raw, true, 101));
        procedure.stillThere(500);
        QCOMPARE(press(procedure, key, 600), swallowedAndTold);
        QCOMPARE(release(procedure, key, 610), swallowedAndTold);
        QCOMPARE(written(whatToTell(procedure, 611)), (QStringList{pressed, pressed, letGo, u"none down"_s}));
        QVERIFY(down(procedure).empty());
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
        // Raw Input's words come, and say nothing new.
        QVERIFY(!procedure.rawKey(raw, true, 650));
        QVERIFY(!procedure.rawKey(raw, false, 651));
        QVERIFY(written(whatToTell(procedure, 652)).isEmpty());

        // And where that account's word for the "up" never comes (it was lost; a keyboard that
        // tells with no device): nothing waited for it, what is pressed meanwhile goes on as
        // ever, and nothing ends. What was noted of the hearing is dropped.
        procedure.stillThere(1000);
        QCOMPARE(press(procedure, key, 1000), swallowedUntold);
        QVERIFY(procedure.rawKey(raw, true, 1001));
        QCOMPARE(press(procedure, key, 1500), swallowedAndTold);
        QCOMPARE(release(procedure, key, 1510), swallowedAndTold);
        QVERIFY(!procedure.rawKey(raw, true, 1511));
        QCOMPARE(press(procedure, keyA, 1520), swallowedUntold);
        QVERIFY(procedure.rawKey(rawA, true, 1521));
        QCOMPARE(release(procedure, keyA, 1560), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawA, false, 1561));
        QCOMPARE(written(whatToTell(procedure, 1562)),
                 (QStringList{pressed, pressed, letGo, u"none down"_s, u"+4"_s, u"-4"_s, u"none down"_s}));
        QVERIFY(!procedure.look(1510 + moment).wake);
        QVERIFY(!procedure.look(1511 + moment).wake);
        QVERIFY(!procedure.notesNoHearing());
        QVERIFY(!procedure.look(1511 + 2 * moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeNotTold());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(procedure.notesNoHearing());

        // Two keyboards, and the hook a key or two ahead. The key is held on the first and
        // tapped on the second, and the hook hears both halves of the tap before Raw Input
        // tells of either: the tap's "up" lets the key go at once. (This kept the key down, in
        // the table and on the machine, until that account had said whose the "up" was.)
        Procedure two;
        swallowing(two);
        two.rawInputTells();
        QCOMPARE(press(two, key, 100), swallowedUntold);
        QVERIFY(two.rawKey(raw, true, 101));
        QCOMPARE(press(two, key, 200), swallowedAndTold);
        QCOMPARE(release(two, key, 210), swallowedAndTold);
        QCOMPARE(written(whatToTell(two, 211)), (QStringList{pressed, pressed, letGo, u"none down"_s}));
        QVERIFY(down(two).empty());
        QVERIFY(!two.rawKey(raw, true, 212));
        QVERIFY(!two.rawKey(raw, false, 213));
        QVERIFY(written(whatToTell(two, 214)).isEmpty());
        // The first keyboard's finger leaves the key later: an "up" of a key that is up.
        QCOMPARE(release(two, key, 300), swallowedAndTold);
        QVERIFY(!two.rawKey(raw, false, 301));
        QCOMPARE(written(whatToTell(two, 302)), QStringList{letGo});
        QVERIFY(!two.look(302 + 5 * moment).wake);
        QVERIFY(two.swallows());
        QVERIFY(two.notesNoHearing());
    }

    void whatIsPressedAfterAModifierWasLetGoGoesOutBehindItsUp()
    {
        using ui::hook::whatToTell;
        // (Turned round in how, with what it is for unchanged. It was "what is pressed after
        // an up that waits stays behind it": the "up" waited in the row to know whose it was,
        // and C behind it.)
        // Ctrl is held, said again, and let go while Raw Input's word for that saying is on its
        // way; and C is pressed right after, before that word has come. Ctrl goes up at once,
        // and C waits for its own word: when that has come, C goes down bare, as it was typed.
        // (Not: Ctrl let go behind what was pressed after it, Ctrl+C where "c" was typed.)
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        QCOMPARE(press(procedure, leftCtrl, 100), swallowedUntold);
        QVERIFY(procedure.rawKey(rawCtrl, true, 101));
        QCOMPARE(press(procedure, leftCtrl, 133), swallowedAndTold);
        QCOMPARE(release(procedure, leftCtrl, 140), swallowedAndTold);
        QCOMPARE(press(procedure, keyC, 141), swallowedUntold);
        QCOMPARE(written(whatToTell(procedure, 142)), (QStringList{u"+e0"_s, u"+e0"_s, u"-e0"_s, u"none down"_s}));
        // (C is on its way: a click that came now would go behind it.)
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{1});
        QVERIFY(!procedure.rawKey(rawCtrl, true, 143));
        QVERIFY(!procedure.rawKey(rawCtrl, false, 145));
        QVERIFY(written(whatToTell(procedure, 146)).isEmpty());
        QVERIFY(procedure.rawKey(rawC, true, 147));
        QCOMPARE(written(whatToTell(procedure, 148)), QStringList{u"+6"_s});
        QCOMPARE(down(procedure), (std::multiset<int>{0x06}));
        QVERIFY(procedure.swallows());
    }

    void aKeyHeldFromBeforeIsLetGoByAnyKeyboardsUp_data()
    {
        QTest::addColumn<bool>("hookFirst");
        QTest::newRow("the hook hears each key first") << true;
        QTest::newRow("Raw Input tells of each key first") << false;
    }

    void aKeyHeldFromBeforeIsLetGoByAnyKeyboardsUp()
    {
        QFETCH(bool, hookFirst);
        using ui::hook::whatToTell;
        // (Named for what is so. It was "a key held from before is held by nobody's account
        // until its keyboard says it again": with Raw Input's account kept for each keyboard,
        // a tap on a second keyboard let the key go only where the first had not said it again
        // since the hook went in. There is one rule now, whether it has or not.)
        // Ctrl is down on one keyboard as the hook goes in: Windows' own, kept back by whoever
        // listens until it is let go, and left out of the comparing until then. The first
        // keyboard says it again; it is tapped on a second; and the tap's "up" lets it go.
        // Whoever listens keeps it back no longer, and the next key goes to the machine bare,
        // with the finger still on the first keyboard's Ctrl: the rule, and what it costs
        // (`aKeyHeldFromBeforeTheKeysWereHeardIsLetGoByAnotherKeyboardsTap`, of MachineControl).
        Procedure procedure;
        procedure.begin(0);
        procedure.windowsHas(leftCtrl.name);
        procedure.isDownAlready(rawCtrl);
        procedure.rawInputTells();
        procedure.setSwallowing(true, 0);
        std::uint64_t now = 100;
        const auto both = [&](bool down) {
            now += 10;
            if (!hookFirst) {
                static_cast<void>(procedure.rawKey(rawCtrl, down, now));
            }
            const Verdict verdict = procedure.key(leftCtrl.name, leftCtrl.scanCode, leftCtrl.flags, down, now);
            if (hookFirst) {
                static_cast<void>(procedure.rawKey(rawCtrl, down, now));
            }
            return verdict;
        };
        both(true);
        both(true);
        // (Windows saw the key go down, and sees a key of that name go up: as it would by itself.)
        QVERIFY(!both(false).swallow);
        QCOMPARE(written(whatToTell(procedure, now)), (QStringList{u"w+e0"_s, u"w+e0"_s, u"-e0"_s, u"none down"_s}));
        QVERIFY(down(procedure).empty());
        // The first keyboard's finger leaves the key: an "up" of a key that is up.
        QVERIFY(both(false).tell);
        QCOMPARE(written(whatToTell(procedure, now)), QStringList{u"-e0"_s});
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotTold());
    }

    void aTellingOfRawInputsThatIsLostIsFoundOrDoesNoHarm()
    {
        using Lost = ui::KeySource::Lost;
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // Raw Input, which tells of this keyboard, loses its word for a key that goes down
        // (Windows drops what is posted to a thread that has too much waiting). The key waits
        // for that word, and its going up behind it, for which the word does come. After the
        // moment: the two differ, nothing more is swallowed, and the key went to no machine.
        Procedure lostDown;
        swallowing(lostDown);
        lostDown.rawInputTells();
        QCOMPARE(press(lostDown, keyA, 100), swallowedUntold);
        QVERIFY(lostDown.rawKey(rawA, true, 100));
        QCOMPARE(release(lostDown, keyA, 150), swallowedAndTold);
        QVERIFY(!lostDown.rawKey(rawA, false, 150));
        static_cast<void>(told(lostDown));
        lostDown.stillThere(500);
        QCOMPARE(press(lostDown, keyC, 500), swallowedUntold);
        QCOMPARE(release(lostDown, keyC, 550), swallowedUntold);
        QVERIFY(!lostDown.rawKey(rawC, false, 551));
        QVERIFY(!lostDown.look(500 + moment).wake);
        QVERIFY(told(lostDown).isEmpty());
        QVERIFY(lostDown.look(501 + moment).wake);
        QVERIFY(lostDown.isClosed());
        QCOMPARE(written(whatToTell(lostDown, 502 + moment)),
                 (QStringList{lost(Lost::NotToldOfKeys), u"-6"_s, u"none down"_s}));
        QVERIFY(down(lostDown).empty());

        // It loses its word for a key that goes up. The key was let go by the hook's own
        // hearing, as ever: nothing is down, on the machine either, and nothing ends. What was
        // noted of that hearing goes after two of the gate's moments, and the key is a key as
        // ever.
        Procedure lostUp;
        swallowing(lostUp);
        lostUp.rawInputTells();
        QCOMPARE(press(lostUp, leftCtrl, 100), swallowedUntold);
        QVERIFY(lostUp.rawKey(rawCtrl, true, 101));
        QCOMPARE(release(lostUp, leftCtrl, 150), swallowedAndTold);
        QCOMPARE(told(lostUp), (QStringList{u"+e0"_s, u"-e0"_s}));
        lostUp.stillThere(150);
        QVERIFY(!lostUp.look(151 + moment).wake);
        QVERIFY(lostUp.swallows());
        QVERIFY(!lostUp.takeNotTold());
        QVERIFY(!lostUp.takeNotHanded());
        QVERIFY(!lostUp.look(151 + 2 * moment).wake);
        QVERIFY(lostUp.notesNoHearing());
        lostUp.stillThere(2000);
        QCOMPARE(press(lostUp, leftCtrl, 2000), swallowedUntold);
        QVERIFY(lostUp.rawKey(rawCtrl, true, 2001));
        QCOMPARE(release(lostUp, leftCtrl, 2100), swallowedAndTold);
        QVERIFY(!lostUp.rawKey(rawCtrl, false, 2101));
        QCOMPARE(told(lostUp), (QStringList{u"+e0"_s, u"-e0"_s}));
        QVERIFY(down(lostUp).empty());
        QVERIFY(lostUp.swallows());

        // And where the key is pressed again within that moment: nothing differs about it. The
        // press waits for its own word, goes with it, and the control goes on. (Turned round:
        // this ended the control, as two accounts that differ about a key that is held. That
        // came of Raw Input's account being kept for each keyboard: what it had of who held the
        // key could neither be kept, or a keyboard that had let go would hold it for ever, nor
        // be forgotten under a finger. That account holds no key down any more, and there is
        // nothing to forget.)
        Procedure again;
        swallowing(again);
        again.rawInputTells();
        QCOMPARE(press(again, keyA, 100), swallowedUntold);
        QVERIFY(again.rawKey(rawA, true, 101));
        QCOMPARE(release(again, keyA, 150), swallowedAndTold);
        QCOMPARE(press(again, keyA, 250), swallowedUntold);
        QVERIFY(again.rawKey(rawA, true, 251));
        QCOMPARE(told(again), (QStringList{u"+4"_s, u"-4"_s, u"+4"_s}));
        again.stillThere(250);
        QVERIFY(!again.look(150 + moment).wake);
        QVERIFY(!again.look(151 + moment).wake);
        QVERIFY(again.swallows());
        QVERIFY(!again.isClosed());
        QCOMPARE(down(again), (std::multiset<int>{0x04}));
        // Its own "up", heard and told of: up, and nothing is down.
        again.stillThere(700);
        QCOMPARE(release(again, keyA, 700), swallowedAndTold);
        QVERIFY(!again.rawKey(rawA, false, 701));
        QCOMPARE(written(whatToTell(again, 702)), (QStringList{u"-4"_s, u"none down"_s}));
        QVERIFY(!again.look(702 + 2 * moment).wake);
        QVERIFY(again.swallows());
        QVERIFY(again.notesNoHearing());
    }

    void aTypedKeyThatRawInputTellsOfWithADeviceEndsTheControl()
    {
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // What a program types comes to the hook marked, and is told to nobody; Raw Input, as
        // far as is known, tells of it with no device, which is not brought here. Were it to
        // tell of such a key with a device after all: that telling finds no hearing (a typed
        // key's is not noted), and after the moment the control ends, with the word that
        // Windows told of a key which was not handed over. The typed key reached no machine.
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        QCOMPARE(procedure.key(keyC.name, keyC.scanCode, keyC.flags | injected, true, 500), passedOnAndTold);
        QCOMPARE(procedure.key(keyC.name, keyC.scanCode, keyC.flags | injected, false, 510), passedOn);
        QVERIFY(!procedure.rawKey(rawC, true, 501));
        QVERIFY(!procedure.rawKey(rawC, false, 511));
        QCOMPARE(written(whatToTell(procedure, 512)), QStringList{u"typed"_s});
        QVERIFY(!procedure.look(501 + moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(procedure.look(502 + moment).wake);
        QVERIFY(procedure.isClosed());
        QCOMPARE(written(whatToTell(procedure, 503 + moment)), QStringList{lost(ui::KeySource::Lost::NotHandedKeys)});
        QVERIFY(down(procedure).empty());
    }

    void aKeyboardThatGoesEndsTheSwallowingWhateverWasHeldOnIt()
    {
        using ui::hook::whatToTell;
        using Lost = ui::KeySource::Lost;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // (Turned into what is so now, and named anew. It was "a key of a keyboard that is gone
        // stays down until it is tapped elsewhere", in the place of three tests of the time in
        // which keyboards were told apart, and showed only that a key with no "up" stays down:
        // half a minute here, with nobody told, until the key was pressed and let go on another
        // keyboard. Whoever has one keyboard has no other. That part is at the end of this, for
        // what it still holds of: a keyboard that drops out with no word of Windows'.)
        // Enter is held on a keyboard, and the keyboard is unplugged, or a desk switch takes it
        // to another computer, or its link is lost: no "up" comes for the key, from either
        // account. Windows says that a keyboard went. Which one, and what was held on it, is
        // not asked and not known: nothing more is swallowed, and whoever listens is told, with
        // a word of its own, to let go of everything.
        Procedure gone;
        swallowing(gone);
        gone.rawInputTells();
        QCOMPARE(press(gone, keyEnter, 100), swallowedUntold);
        QVERIFY(gone.rawKey(rawEnter, true, 101));
        QCOMPARE(told(gone), QStringList{u"+28"_s});
        gone.stillThere(900);
        QVERIFY(gone.aKeyboardWent(900));
        QVERIFY(!gone.swallows());
        QVERIFY(gone.isClosed());
        QCOMPARE(written(whatToTell(gone, 901)), QStringList{lost(Lost::AKeyboardWent)});
        // The key is not taken to be up, since nobody knows which keyboard held it: it stays in
        // the table as one that was kept from Windows and not heard to go up, which whoever
        // listens asks of it, and keeps back at the next taking.
        QCOMPARE(kept(gone), (std::multiset<int>{0x28}));
        // Said once: the next keyboard of a dock that takes every device with it says nothing
        // more. And this hook swallows nothing ever again; the next taking puts a new one in.
        QVERIFY(!gone.aKeyboardWent(950));
        QVERIFY(!gone.setSwallowing(true, 960));
        QCOMPARE(written(whatToTell(gone, 961)), QStringList());

        // With no key held at all, the same: any keyboard that goes is enough.
        Procedure nothingHeld;
        swallowing(nothingHeld);
        nothingHeld.rawInputTells();
        QVERIFY(nothingHeld.aKeyboardWent(100));
        QVERIFY(nothingHeld.isClosed());
        QCOMPARE(written(whatToTell(nothingHeld, 101)), QStringList{lost(Lost::AKeyboardWent)});

        // A press that still waited for the two accounts to be at one is typed into nothing:
        // dropped, and its key known as kept from Windows.
        Procedure waiting;
        swallowing(waiting);
        waiting.rawInputTells();
        QCOMPARE(press(waiting, keyC, 100), swallowedUntold);
        QVERIFY(waiting.aKeyboardWent(150));
        QVERIFY(!waiting.look(200).wake);
        QCOMPARE(written(whatToTell(waiting, 201)), QStringList{lost(Lost::AKeyboardWent)});
        QCOMPARE(waiting.onTheirWay().count, std::uint32_t{0});
        QCOMPARE(kept(waiting), (std::multiset<int>{0x06}));

        // Its word is the one that is said, also where the two accounts are found to differ
        // behind it: Raw Input had told of a key that was not heard yet, and its moment runs
        // out after the keyboard went. The control has ended, and nothing more is said of it.
        Procedure differs;
        swallowing(differs);
        differs.rawInputTells();
        QVERIFY(!differs.rawKey(rawA, true, 100));
        QVERIFY(differs.aKeyboardWent(150));
        QVERIFY(!differs.look(101 + moment).wake);
        QCOMPARE(written(whatToTell(differs, 102 + moment)), QStringList{lost(Lost::AKeyboardWent)});

        // Were Windows to tell of an "up" for what was held there after all, by its other
        // account: the key is up, and told as gone up, as ever where a hook hears no more.
        Procedure toldUp;
        swallowing(toldUp);
        toldUp.rawInputTells();
        QCOMPARE(press(toldUp, leftShift, 100), swallowedUntold);
        QVERIFY(toldUp.rawKey(rawShift, true, 101));
        static_cast<void>(told(toldUp));
        QVERIFY(toldUp.aKeyboardWent(500));
        QVERIFY(toldUp.rawKey(rawShift, false, 501));
        QCOMPARE(written(whatToTell(toldUp, 502)), (QStringList{lost(Lost::AKeyboardWent), u"-e1"_s, u"none down"_s}));
        QVERIFY(kept(toldUp).empty());

        // While nothing is swallowed it is nothing: before keyboard and mouse are taken, and
        // while the hook only stays for what it kept from Windows to be let go. No machine has
        // a key down by this hook then, and it goes on at the next taking.
        Procedure idle;
        idle.begin(0);
        QVERIFY(!idle.aKeyboardWent(100));
        QVERIFY(!idle.isClosed());
        QVERIFY(idle.setSwallowing(true, 200));
        QCOMPARE(press(idle, leftShift, 300), swallowedAndTold);
        QVERIFY(!idle.setSwallowing(false, 400));
        QVERIFY(!idle.aKeyboardWent(500));
        QVERIFY(!idle.isClosed());
        QCOMPARE(written(whatToTell(idle, 501)), QStringList{u"+e1"_s});
        QVERIFY(idle.setSwallowing(true, 600));

        // Whether Windows says when a keyboard goes: not until it was asked and took the
        // asking; and no longer once its other account of the keyboard is another window's
        // hook's, which that word goes to with it.
        Procedure asked;
        swallowing(asked);
        QVERIFY(!asked.isToldOfKeyboardsGoing());
        asked.rawInputTells();
        QVERIFY(!asked.isToldOfKeyboardsGoing());
        asked.keyboardsGoingIsTold();
        QVERIFY(asked.isToldOfKeyboardsGoing());
        asked.rawInputTellsNoMore();
        QVERIFY(!asked.isToldOfKeyboardsGoing());
        asked.keyboardsGoingIsTold();
        asked.begin(0);
        QVERIFY(!asked.isToldOfKeyboardsGoing());

        // What is left as it was: a keyboard that drops out and Windows has no word for it
        // (one that sleeps behind a receiver which stays plugged in; a desk switch that keeps a
        // made-up keyboard at each computer), or a PC where that word cannot be had. Shift is
        // held on it: no "up" comes, and nothing here knows. The key stays down, in the table
        // and for whoever listens, however long: half a minute here. Nothing ends, and what is
        // typed arrives under it. It is on the list for a person at a keyboard.
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        QCOMPARE(press(procedure, leftShift, 100), swallowedUntold);
        QVERIFY(procedure.rawKey(rawShift, true, 101));
        QCOMPARE(told(procedure), QStringList{u"+e1"_s});
        for (std::uint64_t at = 1000; at <= 30'000; at += 1000) {
            procedure.stillThere(at);
            QVERIFY(!procedure.look(at).wake);
        }
        QCOMPARE(down(procedure), (std::multiset<int>{0xE1}));
        QVERIFY(procedure.swallows());
        procedure.stillThere(30'100);
        QCOMPARE(press(procedure, keyA, 30'100), swallowedUntold);
        QVERIFY(procedure.rawKey(rawA, true, 30'101));
        QCOMPARE(release(procedure, keyA, 30'150), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawA, false, 30'151));
        QCOMPARE(written(whatToTell(procedure, 30'152)), (QStringList{u"+4"_s, u"-4"_s}));
        // The way out of it that leaves control standing: the key is pressed and let go on
        // another keyboard. Its going down is the key said again, and its "up" lets the key
        // go, at once and however soon after the first keyboard was last heard of. With it no
        // key is down.
        QCOMPARE(press(procedure, leftShift, 30'200), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawShift, true, 30'201));
        QCOMPARE(release(procedure, leftShift, 30'210), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawShift, false, 30'211));
        QCOMPARE(written(whatToTell(procedure, 30'212)), (QStringList{u"+e1"_s, u"-e1"_s, u"none down"_s}));
        QVERIFY(down(procedure).empty());
        QVERIFY(kept(procedure).empty());
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotTold());

        // The other way out: the swallowing ends (keyboard and mouse are given back). Whoever
        // listens lets go of everything on the machine; and the key is known as one that was
        // kept from Windows and not heard to go up, asked of the table.
        Procedure givenBack;
        swallowing(givenBack);
        givenBack.rawInputTells();
        QCOMPARE(press(givenBack, leftShift, 100), swallowedUntold);
        QVERIFY(givenBack.rawKey(rawShift, true, 101));
        QVERIFY(!givenBack.setSwallowing(false, 5000));
        QCOMPARE(kept(givenBack), (std::multiset<int>{0xE1}));

        // And however many keys are held at one moment, on however many keyboards: there is
        // nothing to run out of, and nothing ends by it.
        Procedure many;
        swallowing(many);
        many.rawInputTells();
        for (std::uint32_t which = 0; which < 12; ++which) {
            const Key key{0x41 + which, 0x1E + which, 0};
            QCOMPARE(press(many, key, 100 + which), swallowedUntold);
            QVERIFY(many.rawKey(ui::hook::pressedOf(key.name, key.scanCode, 0), true, 100 + which));
        }
        QCOMPARE(told(many).size(), qsizetype{12});
        QVERIFY(!many.look(200).wake);
        QVERIFY(many.swallows());
        QVERIFY(!many.isClosed());
        QCOMPARE(kept(many).size(), std::size_t{12});
    }

    void theLeaveKeyGivesTheKeyboardBackByItsUpAsItIsHeardWhateverRawInputHasStillToSay()
    {
        using ui::hook::whatToTell;
        // (Named anew: it was "... whoever else may hold it", from the time when a key's "up"
        // could wait for Raw Input to say which keyboard had let go. The way out waited for
        // nobody then, and the key's own going up did; now neither waits.)
        // The leave-key is pressed, said again, and let go while Raw Input's word for that
        // saying is on its way: the keyboard is given back by the "up" as it is heard, and the
        // key is told as gone up with it. (Windows is shown that "up": nothing is kept from it
        // any more by then.)
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        QCOMPARE(press(procedure, rightCtrl, 100), swallowedUntold);
        QVERIFY(procedure.rawKey(rawRightCtrl, true, 101));
        QCOMPARE(press(procedure, rightCtrl, 133), swallowedAndTold);
        QVERIFY(procedure.swallows());
        QCOMPARE(release(procedure, rightCtrl, 140), passedOnAndTold);
        QVERIFY(!procedure.swallows());
        QVERIFY(!procedure.isClosed());
        QCOMPARE(written(whatToTell(procedure, 141)), (QStringList{u"+e4"_s, u"+e4"_s, u"-e4"_s, u"none down"_s}));
        static_cast<void>(procedure.rawKey(rawRightCtrl, true, 142));
        static_cast<void>(procedure.rawKey(rawRightCtrl, false, 143));
        QVERIFY(written(whatToTell(procedure, 144)).isEmpty());
        QVERIFY(down(procedure).empty());

        // Held on one keyboard and tapped on another: the tap's "up" lets the key go, as any
        // key's, and gives the keyboard back. (The leave-key went down, and a leave-key went
        // up, with no other key between. Giving back is the safe side.)
        Procedure two;
        swallowing(two);
        two.rawInputTells();
        QCOMPARE(press(two, rightCtrl, 100), swallowedUntold);
        QVERIFY(two.rawKey(rawRightCtrl, true, 101));
        QCOMPARE(press(two, rightCtrl, 200), swallowedAndTold);
        static_cast<void>(two.rawKey(rawRightCtrl, true, 201));
        QCOMPARE(release(two, rightCtrl, 210), passedOnAndTold);
        QVERIFY(!two.swallows());
        static_cast<void>(two.rawKey(rawRightCtrl, false, 211));
        QVERIFY(down(two).empty());
        QCOMPARE(written(whatToTell(two, 212)), (QStringList{u"+e4"_s, u"+e4"_s, u"-e4"_s, u"none down"_s}));
        // The first keyboard's finger leaves the key: nothing is swallowed by then, and of a
        // key of this PC's that was not kept from Windows nothing is told.
        QCOMPARE(release(two, rightCtrl, 300), passedOn);
        QVERIFY(written(whatToTell(two, 301)).isEmpty());
    }

    void pauseAndPrintScreenAreLetGoAsTheKeysTheyWentDownAs()
    {
        using ui::hook::rawMarkedFlag;
        using ui::hook::rawPauseFlag;
        using ui::hook::whatToTell;
        // The two keys whose number goes by what else is held. Pause goes down; Ctrl goes down;
        // Pause is said again, as Break by now, and let go as Break, while Raw Input's word for
        // that saying is on its way. It is let go at once, as the key it went down as, and
        // Ctrl stays. (Named anew: it was "... are let go by the other account's word ...",
        // and had these two wait for that word, as every "up" once did. None waits for it any
        // more: `aKeyGoesUpByItsUpAsItIsHeard`. What this holds to still: that the key is let
        // go as the key it went down as, under whichever number it comes.)
        constexpr RawKey rawPause{0x1D, rawPauseFlag, 0x13};
        constexpr RawKey rawBreak{0x46, rawMarkedFlag, 0x03};
        Procedure pause;
        swallowing(pause);
        pause.rawInputTells();
        QCOMPARE(press(pause, keyPause, 100), swallowedUntold);
        QVERIFY(tellAsRawInput(pause, rawPause, true, 101));
        QCOMPARE(press(pause, leftCtrl, 200), swallowedUntold);
        QVERIFY(pause.rawKey(rawCtrl, true, 201));
        QCOMPARE(press(pause, pauseAsBreak, 300), swallowedAndTold);
        QCOMPARE(release(pause, pauseAsBreak, 310), swallowedAndTold);
        QCOMPARE(written(whatToTell(pause, 311)), (QStringList{u"+48"_s, u"+e0"_s, u"+48"_s, u"-48"_s}));
        QCOMPARE(down(pause), (std::multiset<int>{0xE0}));
        QVERIFY(!tellAsRawInput(pause, rawBreak, true, 312));
        QVERIFY(!tellAsRawInput(pause, rawBreak, false, 313));
        QVERIFY(written(whatToTell(pause, 314)).isEmpty());
        QCOMPARE(down(pause), (std::multiset<int>{0xE0}));
        QVERIFY(pause.swallows());

        // Print Screen, which has another number under Alt (SysRq): the same.
        constexpr Key printScreen{0x2C, 0x37, ui::hook::markedFlag};
        constexpr Key sysRq{0x2C, 0x54, 0};
        constexpr RawKey rawPrintScreen{0x37, rawMarkedFlag, 0x2C};
        constexpr RawKey rawSysRq{0x54, 0, 0x2C};
        constexpr RawKey rawAlt{0x38, 0, 0x12};
        Procedure print;
        swallowing(print);
        print.rawInputTells();
        QCOMPARE(press(print, printScreen, 100), swallowedUntold);
        QVERIFY(tellAsRawInput(print, rawPrintScreen, true, 101));
        QCOMPARE(press(print, leftAlt, 200), swallowedUntold);
        QVERIFY(tellAsRawInput(print, rawAlt, true, 201));
        QCOMPARE(press(print, sysRq, 300), swallowedAndTold);
        QCOMPARE(release(print, sysRq, 310), swallowedAndTold);
        QCOMPARE(written(whatToTell(print, 311)), (QStringList{u"+46"_s, u"+e2"_s, u"+46"_s, u"-46"_s}));
        QCOMPARE(down(print), (std::multiset<int>{0xE2}));
        QVERIFY(!tellAsRawInput(print, rawSysRq, true, 312));
        QVERIFY(!tellAsRawInput(print, rawSysRq, false, 313));
        QVERIFY(written(whatToTell(print, 314)).isEmpty());
        QVERIFY(print.swallows());
        // (Taken out of here: the two let go through the letting go by that account's word
        // alone, which a keyboard that was unplugged set off. Nothing is done about a keyboard
        // that goes any more. What that letting go still covers:
        // `whatRawInputHasAsUpIsLetGoWhereItsTellingOfTheUpWasForgiven`.)
    }

    void aPressHandedOnInTheCallThatClosesTheGateIsNotTold()
    {
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // Raw Input tells of this keyboard. Ctrl goes down, and waits in vain for its word. C
        // arrives after the moment and before the thread has looked: handing it on finds that
        // the two differ, closes the hook, drops Ctrl, and puts C on its way, as a press that
        // was kept from Windows.
        const auto closedOverCtrl = [](Procedure& procedure) {
            swallowing(procedure);
            procedure.rawInputTells();
            static_cast<void>(press(procedure, keyA, 100));
            static_cast<void>(procedure.rawKey(rawA, true, 100));
            static_cast<void>(release(procedure, keyA, 150));
            static_cast<void>(procedure.rawKey(rawA, false, 150));
            static_cast<void>(told(procedure));
            procedure.stillThere(500);
            static_cast<void>(press(procedure, leftCtrl, 500));
            return press(procedure, keyC, 501 + moment);
        };
        // The interface thread had asked why nothing is heard a moment before that, and found
        // nothing; it takes the keys a moment after. (Here: the mark is taken by hand, as that
        // asking would have found none.) C is not told: on the machine Ctrl never went down,
        // and C would arrive there bare, to be let go a moment later. (It was told.)
        Procedure raced;
        QCOMPARE(closedOverCtrl(raced), swallowedAndTold);
        QVERIFY(raced.isClosed());
        QVERIFY(raced.takeNotTold());
        QCOMPARE(written(whatToTell(raced, 502 + moment)), QStringList());
        // Both are down, kept from Windows, and known as that: asked of the table.
        QCOMPARE(kept(raced), (std::multiset<int>{0x06, 0xE0}));
        QCOMPARE(raced.onTheirWay().count, std::uint32_t{0});
        // They go up, and that is told: they are waited for.
        QCOMPARE(release(raced, keyC, 2000), passedOnAndTold);
        QCOMPARE(written(whatToTell(raced, 2001)), QStringList{u"-6"_s});

        // Asked in its order, why is said first, and nothing of the press either.
        Procedure inOrder;
        QCOMPARE(closedOverCtrl(inOrder), swallowedAndTold);
        QCOMPARE(written(whatToTell(inOrder, 502 + moment)), QStringList{lost(ui::KeySource::Lost::NotToldOfKeys)});
        QVERIFY(written(whatToTell(inOrder, 503 + moment)).isEmpty());
    }

    void whereThereIsNoSecondAccountNothingIsNotedOfAHearing()
    {
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        constexpr std::uint64_t taps = 10;
        // A PC that gives no second account (it was refused; a remote desktop where none
        // comes). A is pressed and let go ten times. Nothing is noted of that but, while it is
        // down, that it is: not when it was heard, and not how often. (From the fourth press
        // of a key on a count went up with every press, and the time of the last was kept, for
        // as long as the hook lived: there was no telling to wipe them.)
        Procedure alone;
        swallowing(alone);
        for (std::uint64_t tap = 0; tap < taps; ++tap) {
            alone.stillThere(100 * tap);
            QCOMPARE(press(alone, keyA, 100 * tap), swallowedAndTold);
            QCOMPARE(release(alone, keyA, 100 * tap + 50), swallowedAndTold);
            QVERIFY2(alone.notesNoHearing(), qPrintable(QString::number(tap)));
        }
        QCOMPARE(told(alone).size(), qsizetype{2 * taps});

        // Asked for, and it tells of nothing: the first key waits its moment for a word, and
        // its hearing is noted for that long. Then it is known that no account is there to hold
        // a hearing against, and none is noted from there on.
        Procedure silent;
        swallowing(silent);
        silent.rawInputTells();
        QCOMPARE(press(silent, keyA, 100), swallowedUntold);
        QVERIFY(!silent.notesNoHearing());
        QVERIFY(silent.look(101 + moment).wake);
        QVERIFY(silent.notesNoHearing());
        silent.stillThere(1000);
        QCOMPARE(release(silent, keyA, 1000), swallowedAndTold);
        for (std::uint64_t tap = 0; tap < taps; ++tap) {
            silent.stillThere(2000 + 100 * tap);
            QCOMPARE(press(silent, keyC, 2000 + 100 * tap), swallowedAndTold);
            QCOMPARE(release(silent, keyC, 2050 + 100 * tap), swallowedAndTold);
        }
        QVERIFY(silent.notesNoHearing());
        QVERIFY(silent.swallows());

        // With a second account a hearing is noted until that account has told of the key, and
        // no longer.
        Procedure both;
        swallowing(both);
        both.rawInputTells();
        QCOMPARE(press(both, keyA, 100), swallowedUntold);
        QVERIFY(!both.notesNoHearing());
        QVERIFY(both.rawKey(rawA, true, 101));
        QVERIFY(both.notesNoHearing());
        // A held key that is said again five times before that account's word for any of them:
        // more than there is room to note the time of, so the rest are counted while they wait.
        // When all were told of, nothing is left of it: not how many they were, and not when
        // the last of them came.
        for (std::uint64_t said = 0; said < 5; ++said) {
            QVERIFY(both.key(keyA.name, keyA.scanCode, keyA.flags, true, 200 + 33 * said).swallow);
        }
        QVERIFY(!both.notesNoHearing());
        for (std::uint64_t said = 0; said < 5; ++said) {
            static_cast<void>(both.rawKey(rawA, true, 400 + said));
        }
        QVERIFY(both.notesNoHearing());
        // And a saying whose word never comes is noted for two of the gate's moments, which is
        // as far back as anything is asked of a hearing, and not for as long as the hook lives.
        // Nothing ends by it.
        both.stillThere(1000);
        QVERIFY(both.key(keyA.name, keyA.scanCode, keyA.flags, true, 1000).swallow);
        QVERIFY(!both.notesNoHearing());
        both.stillThere(1000 + 2 * moment);
        QVERIFY(!both.look(1000 + 2 * moment).wake);
        QVERIFY(!both.notesNoHearing());
        QVERIFY(!both.look(1001 + 2 * moment).wake);
        QVERIFY(both.notesNoHearing());
        QVERIFY(both.swallows());
        QCOMPARE(down(both), (std::multiset<int>{0x04}));

        // Where another window's hook has that account from here on, there is none to hold a
        // hearing against either.
        both.rawInputTellsNoMore();
        both.stillThere(3000);
        QCOMPARE(press(both, keyC, 3000), swallowedAndTold);
        QCOMPARE(release(both, keyC, 3050), swallowedAndTold);
        QVERIFY(both.notesNoHearing());
    }

    void theNumbersOfKeysOnTheirWayBeginAnewWhileAnotherThreadTakesThem()
    {
        // The hook's thread puts keys on their way while the interface thread takes them and
        // asks which are on their way: on two threads at once, as in the program. The numbers
        // begin at 1 again whenever none is on its way, which is decided by both threads in the
        // same moment. Held to, for forty thousand keys: every key is taken once, in the order
        // it was heard in; its number is the one after the last one's, or 1; and a key that was
        // on its way when that was asked has a number no higher than the last that was said
        // then, which is what a click goes by that waits behind the keys before it.
        constexpr int taps = 20'000;
        constexpr std::uint32_t atATime = 100;
        Procedure procedure;
        swallowing(procedure);
        std::atomic<bool> typed{false};
        std::thread typist([&procedure, &typed] {
            for (int tap = 0; tap < taps; ++tap) {
                // (No faster than they are taken: the ring's size is not what is tried here.)
                while (procedure.onTheirWay().count > atATime) {
                    std::this_thread::yield();
                }
                static_cast<void>(procedure.key(keyA.name, keyA.scanCode, keyA.flags, true, 0));
                static_cast<void>(procedure.key(keyA.name, keyA.scanCode, keyA.flags, false, 0));
            }
            typed.store(true);
        });
        int taken = 0;
        int beganAnew = 0;
        std::uint64_t last = 0;
        QStringList wrong;
        const auto note = [&wrong](const QString& what) {
            if (wrong.size() < 5) {
                wrong.append(what);
            }
        };
        for (bool more = true; more;) {
            // (Whether all was typed, asked before the ring is looked at: what is found empty
            // after that stays empty.)
            const bool wasTyped = typed.load();
            const Procedure::OnTheirWay way = procedure.onTheirWay();
            if (way.count > way.last || way.count > atATime + 2) {
                note(QStringLiteral("%1 on their way, the last of them number %2").arg(way.count).arg(way.last));
            }
            const std::optional<Heard> next = procedure.next();
            if (!next.has_value()) {
                more = !wasTyped;
                continue;
            }
            ++taken;
            if (next->number != last + 1 && next->number != 1) {
                note(QStringLiteral("number %1 after number %2").arg(next->number).arg(last));
            }
            if (way.count > 0 && next->number > way.last) {
                note(QStringLiteral("number %1, where the last on its way was %2").arg(next->number).arg(way.last));
            }
            if (next->down != (taken % 2 == 1)) {
                note(QStringLiteral("key %1 came out of its order").arg(taken));
            }
            beganAnew += next->number == 1 ? 1 : 0;
            last = next->number;
        }
        typist.join();
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u"; "_s)));
        QCOMPARE(taken, 2 * taps);
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
        QCOMPARE(procedure.onTheirWay().last, std::uint64_t{0});
        QVERIFY(!procedure.takeFellBehind());
        // (And they did begin anew, many times: else this showed nothing.)
        QVERIFY2(beganAnew > 10, qPrintable(QString::number(beganAnew)));
    }

    // --- what a seventh reviewer found ---

    void aModifierGoingDownOnceMoreWhileItsUpWaitsTakesItsPlaceBehindIt()
    {
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // (What it holds still, with the telling apart of keyboards gone. An "up" waited in the
        // row to know whose it was, and the same key going down behind it was a press or the
        // key said again by what that "up" turned out to be. Now an "up" waits for two things
        // only, and for neither longer than the gate's moment: for its own going down, where
        // that still waits for Raw Input's word, or the key would stay down on the machine;
        // and, where it is a modifier, for any key going down that waits, which was pressed
        // under it. The same key going down behind it is a press, always: the key is up in the
        // table by then.)
        // Shift is tapped twice, fast, with the hook's thread a quarter of a second behind,
        // which the gate's moment is to bear: a press, an "up", a press and an "up" are heard
        // before Raw Input's word for the first of them. The first "up" waits behind its own
        // press; the second press takes its place behind that "up"; the second "up" behind its
        // own press. Each press goes by its own word, and its "up" right behind it; the thread
        // looks ten times a second meanwhile, and nothing ends. What that takes of the row:
        // four places of its thirty-two, for no longer than each word is on its way.
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        procedure.stillThere(900);
        QCOMPARE(press(procedure, leftShift, 1000), swallowedUntold);
        QCOMPARE(release(procedure, leftShift, 1080), swallowedUntold);
        QVERIFY(!procedure.look(1100).wake);
        QCOMPARE(press(procedure, leftShift, 1160), swallowedUntold);
        QVERIFY(!procedure.look(1200).wake);
        QCOMPARE(release(procedure, leftShift, 1240), swallowedUntold);
        QVERIFY(written(whatToTell(procedure, 1245)).isEmpty());
        // (All four are on their way: a click that came now would go behind them.)
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{4});
        QVERIFY(down(procedure).empty());
        // The words, each a quarter of a second after its hearing.
        QVERIFY(procedure.rawKey(rawShift, true, 1250));
        QCOMPARE(written(whatToTell(procedure, 1251)), (QStringList{u"+e1"_s, u"-e1"_s}));
        QVERIFY(!procedure.look(1300).wake);
        QVERIFY(!procedure.rawKey(rawShift, false, 1330));
        QVERIFY(written(whatToTell(procedure, 1331)).isEmpty());
        QVERIFY(!procedure.look(1400).wake);
        QVERIFY(procedure.rawKey(rawShift, true, 1410));
        QCOMPARE(written(whatToTell(procedure, 1411)), (QStringList{u"+e1"_s, u"-e1"_s, u"none down"_s}));
        QVERIFY(!procedure.rawKey(rawShift, false, 1490));
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
        QVERIFY(!procedure.look(1490 + 5 * moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.takeNotHanded());
        QVERIFY(!procedure.takeNotTold());
        QVERIFY(!procedure.takeFellBehind());
        QVERIFY(procedure.notesNoHearing());

        // One keyboard, and nothing unusual done: Shift is held until it says itself again, let
        // go, and tapped again, with the thread a twentieth of a second behind. The saying and
        // the "up" go at once, since the key is down for whoever listens; the new press is kept
        // from Windows as any press, and waits for its own word. (That press ended the control
        // once, as keys that could not be passed on in time, and went to Windows.)
        Procedure again;
        swallowing(again);
        again.rawInputTells();
        QCOMPARE(press(again, leftShift, 100), swallowedUntold);
        QVERIFY(again.rawKey(rawShift, true, 101));
        again.stillThere(500);
        QCOMPARE(press(again, leftShift, 600), swallowedAndTold);
        QCOMPARE(release(again, leftShift, 610), swallowedAndTold);
        QVERIFY(!again.rawKey(rawShift, true, 650));
        QCOMPARE(press(again, leftShift, 655), swallowedUntold);
        QVERIFY(!again.isClosed());
        QVERIFY(again.swallows());
        QCOMPARE(written(whatToTell(again, 656)), (QStringList{u"+e1"_s, u"+e1"_s, u"-e1"_s, u"none down"_s}));
        QCOMPARE(again.onTheirWay().count, std::uint32_t{1});
        QCOMPARE(kept(again), (std::multiset<int>{0xE1}));
        // Raw Input's word for the "up": of the key as it went up then. What the table has as
        // down is the press since, of which that account has not told yet: it stays.
        QVERIFY(!again.rawKey(rawShift, false, 660));
        QVERIFY(written(whatToTell(again, 661)).isEmpty());
        QCOMPARE(down(again), (std::multiset<int>{0xE1}));
        QVERIFY(again.rawKey(rawShift, true, 705));
        QCOMPARE(written(whatToTell(again, 706)), QStringList{u"+e1"_s});
        QCOMPARE(again.onTheirWay().count, std::uint32_t{0});
        QCOMPARE(release(again, leftShift, 750), swallowedAndTold);
        QVERIFY(!again.rawKey(rawShift, false, 800));
        QCOMPARE(written(whatToTell(again, 801)), (QStringList{u"-e1"_s, u"none down"_s}));
        QVERIFY(!again.look(801 + 5 * moment).wake);
        QVERIFY(again.swallows());
        QVERIFY(!again.takeNotHanded());
        QVERIFY(!again.takeNotTold());
        QVERIFY(!again.takeFellBehind());

        // And a modifier's "up" behind a key that was pressed under it and still waits: C goes
        // down under Shift, Shift is let go and pressed once more, all before Raw Input's word
        // for C. Shift's "up" stays behind C, and its new press behind that: C reaches the
        // machine under Shift, as it was typed, and Shift goes up and down behind it.
        Procedure under;
        swallowing(under);
        under.rawInputTells();
        QCOMPARE(press(under, leftShift, 100), swallowedUntold);
        QVERIFY(under.rawKey(rawShift, true, 101));
        QCOMPARE(press(under, keyC, 200), swallowedUntold);
        QCOMPARE(release(under, leftShift, 210), swallowedUntold);
        QCOMPARE(press(under, leftShift, 220), swallowedUntold);
        QCOMPARE(written(whatToTell(under, 221)), QStringList{u"+e1"_s});
        QCOMPARE(under.onTheirWay().count, std::uint32_t{3});
        QVERIFY(under.rawKey(rawC, true, 230));
        QCOMPARE(written(whatToTell(under, 231)), (QStringList{u"+6"_s, u"-e1"_s}));
        QVERIFY(!under.rawKey(rawShift, false, 232));
        QVERIFY(under.rawKey(rawShift, true, 233));
        QCOMPARE(written(whatToTell(under, 234)), QStringList{u"+e1"_s});
        QCOMPARE(down(under), (std::multiset<int>{0x06, 0xE1}));
        QVERIFY(under.swallows());
    }

    void anUpThatIsNotHeardInTheFirstMomentAfterRawInputBeginsToTellEndsTheControl_data()
    {
        QTest::addColumn<bool>("isTheLeaveKey");
        QTest::newRow("the left Shift") << false;
        QTest::newRow("the leave-key") << true;
    }

    void anUpThatIsNotHeardInTheFirstMomentAfterRawInputBeginsToTellEndsTheControl()
    {
        QFETCH(bool, isTheLeaveKey);
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        const Key key = isTheLeaveKey ? rightCtrl : leftShift;
        const input::Pressed raw = isTheLeaveKey ? rawRightCtrl : rawShift;
        const int usage = isTheLeaveKey ? 0xE4 : 0xE1;
        const QString pressed = u"+"_s + QString::number(usage, 16);
        const QString letGo = u"-"_s + QString::number(usage, 16);
        // (Turned round, and named anew. It was "what Raw Input has as up is let go where its
        // telling of the up was forgiven": such a telling was forgiven after its moment, as one
        // that may be the word for a key heard before any hearing was noted, and the key was
        // let go by Raw Input's own account of what is down, with the control going on. The
        // forgiving is gone, since it forgave a key that went down unheard as well, and with it
        // that account and the letting go by its word, to which nothing else came.)
        //
        // A PC that is taken not to tell. Then Raw Input tells after all (a keyboard that it
        // tells of was plugged in): of C, a little before the hook hears it. The two agree.
        Procedure procedure;
        takenNotToTell(procedure);
        procedure.stillThere(2000);
        QVERIFY(!procedure.rawKey(rawC, true, 2000));
        QCOMPARE(press(procedure, keyC, 2010), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawC, false, 2050));
        QCOMPARE(release(procedure, keyC, 2060), swallowedAndTold);
        static_cast<void>(told(procedure));
        // Within the moment of that first telling the key goes down, heard and told of. It goes
        // up, and the hook is not called for that (Windows left it out): only Raw Input tells.
        QCOMPARE(press(procedure, key, 2100), swallowedUntold);
        QVERIFY(procedure.rawKey(raw, true, 2101));
        QCOMPARE(told(procedure), QStringList{pressed});
        QVERIFY(!procedure.rawKey(raw, false, 2300));
        QCOMPARE(down(procedure), (std::multiset<int>{usage}));
        procedure.stillThere(2300 + moment);
        QVERIFY(!procedure.look(2300 + moment).wake);
        // That telling has waited its moment to be heard, in vain, and is held against the hook
        // as at any other time: the control ends. With a word of its own, since so soon after
        // that account began to tell it cannot be known that the key was not heard. And the
        // key, which had been kept from Windows, is up by the word of the account that told of
        // it: let go, and told as gone up, so that nobody waits for it.
        QVERIFY(procedure.look(2301 + moment).wake);
        QCOMPARE(written(whatToTell(procedure, 2302 + moment)),
                 (QStringList{lost(ui::KeySource::Lost::BeganToTell), letGo, u"none down"_s}));
        QVERIFY(procedure.isClosed());
        QVERIFY(!procedure.swallows());
        QVERIFY(down(procedure).empty());
        QVERIFY(kept(procedure).empty());
    }

    void whereTheOtherAccountBeginsToTellLaterAKeyHeardBeforeThatEndsTheControlOnceWithAWordOfItsOwn()
    {
        using ui::hook::whatToTell;
        using Lost = ui::KeySource::Lost;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // (Turned round a second time, and named anew: it was "... what was heard before is not
        // held against it". First such a key ended the control with the word that Windows saw a
        // key which was not handed over: it was handed over, before any hearing was noted. Then
        // every telling of that first moment which found no hearing was forgiven, the one for a
        // key that went down unheard among them: a "c" went to the machine bare where its Ctrl
        // was left out, with nothing said. Now such a telling ends the control, once, with a
        // word that is true of both: `Lost::BeganToTell`.)
        //
        // A PC that is taken not to tell: Raw Input was asked for, and tells of no key (a
        // keyboard that it does not tell of). The first key waited its moment for a word, and
        // from then on no hearing is noted.
        Procedure procedure;
        takenNotToTell(procedure);
        QVERIFY(!procedure.isWatched());
        // A keyboard that it does tell of is plugged in, and C is tapped on it at once. The
        // hook hears both halves of the tap, noted by nobody, before Raw Input's first word:
        // its words for them come a moment later. They find no hearing, and none will come.
        procedure.stillThere(2000);
        QCOMPARE(press(procedure, keyC, 2000), swallowedAndTold);
        QCOMPARE(release(procedure, keyC, 2030), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawC, true, 2040));
        QVERIFY(!procedure.rawKey(rawC, false, 2041));
        QVERIFY(procedure.isWatched());
        QCOMPARE(told(procedure), (QStringList{u"+6"_s, u"-6"_s}));
        procedure.stillThere(2040 + moment);
        QVERIFY(!procedure.look(2040 + moment).wake);
        // Whether C was heard cannot be known here: nothing was noted of it. So the control
        // ends, as over any key that was told of and found no hearing; with the word for what
        // is known, which is not that Windows did not hand a key over. The cost of that: on a
        // PC that begins to tell while a machine is controlled, and with a key in flight at
        // that very moment, keyboard and mouse are given back once, and are taken again.
        QVERIFY(procedure.look(2041 + moment).wake);
        QVERIFY(procedure.isClosed());
        QCOMPARE(written(whatToTell(procedure, 2042 + moment)), QStringList{lost(Lost::BeganToTell)});

        // Where the first key on that keyboard is heard and told of in step, as a person types,
        // nothing ends: a key that is down as the comparing begins is not compared until it has
        // gone up. From the first telling on, hearings are noted, and the two are held against
        // each other as ever: A waits for Raw Input's word, and goes with it.
        Procedure inStep;
        takenNotToTell(inStep);
        inStep.stillThere(2000);
        QCOMPARE(press(inStep, keyC, 2000), swallowedAndTold);
        QVERIFY(!inStep.rawKey(rawC, true, 2001));
        QCOMPARE(release(inStep, keyC, 2030), swallowedAndTold);
        QVERIFY(!inStep.rawKey(rawC, false, 2031));
        QCOMPARE(told(inStep), (QStringList{u"+6"_s, u"-6"_s}));
        inStep.stillThere(2031 + 2 * moment);
        QVERIFY(!inStep.look(2031 + 2 * moment).wake);
        QVERIFY(inStep.swallows());
        inStep.stillThere(3000);
        QCOMPARE(press(inStep, keyA, 3000), swallowedUntold);
        QVERIFY(inStep.rawKey(rawA, true, 3001));
        QCOMPARE(release(inStep, keyA, 3050), swallowedAndTold);
        QVERIFY(!inStep.rawKey(rawA, false, 3051));
        QCOMPARE(told(inStep), (QStringList{u"+4"_s, u"-4"_s}));
        // And a key that it tells of after that first moment and that is not heard is found, as
        // ever, and said as what it is.
        inStep.stillThere(4000);
        QVERIFY(!inStep.rawKey(rawEnter, true, 4000));
        QVERIFY(!inStep.look(4000 + moment).wake);
        QVERIFY(inStep.look(4001 + moment).wake);
        QCOMPARE(written(whatToTell(inStep, 4002 + moment)), QStringList{lost(Lost::NotHandedKeys)});

        // A telling within that first moment whose key the hook does hear: the two agree, as
        // ever. Raw Input tells of C first, and the hook hears it a little later.
        Procedure agreed;
        takenNotToTell(agreed);
        agreed.stillThere(2000);
        QVERIFY(!agreed.rawKey(rawC, true, 2000));
        QCOMPARE(press(agreed, keyC, 2010), swallowedAndTold);
        QVERIFY(!agreed.rawKey(rawC, false, 2050));
        QCOMPARE(release(agreed, keyC, 2060), swallowedAndTold);
        QCOMPARE(told(agreed), (QStringList{u"+6"_s, u"-6"_s}));
        agreed.stillThere(2060 + 2 * moment);
        QVERIFY(!agreed.look(2060 + 2 * moment).wake);
        QVERIFY(agreed.swallows());
        QVERIFY(agreed.notesNoHearing());
    }

    void aKeyForSoundIsNotTheLetterThatSharesItsNumber()
    {
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        constexpr input::Pressed rawB{0x30, false, 0x42};
        // Volume up has the keyboard's number of B, with the mark, and no USB number of the
        // keyboard's page: Raw Input may not tell of it at all (such a key comes by another
        // way). B is held, and Volume up is tapped. Nothing of its hearings is noted, and
        // nothing is made of them. (They were noted as B's, read with the mark turned. While
        // keyboards were told apart, the "up" that was never told of had B's holders forgotten
        // and, since B was down, ended the control as a key that Windows did not report.)
        Procedure procedure;
        swallowing(procedure);
        procedure.rawInputTells();
        QCOMPARE(press(procedure, keyB, 100), swallowedUntold);
        QVERIFY(procedure.rawKey(rawB, true, 101));
        QCOMPARE(press(procedure, louder, 200), swallowedAndTold);
        QCOMPARE(release(procedure, louder, 250), swallowedAndTold);
        QCOMPARE(told(procedure), (QStringList{u"+5"_s, u"+0"_s, u"-0"_s}));
        procedure.stillThere(250);
        QVERIFY(!procedure.look(250 + moment).wake);
        QVERIFY(!procedure.look(251 + moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(!procedure.isClosed());
        QVERIFY(!procedure.takeNotTold());
        QVERIFY(procedure.notesNoHearing());
        QCOMPARE(down(procedure), (std::multiset<int>{0x05}));
        // And B is as it was, by both accounts: said again and let go, it goes up by its own
        // "up", and nothing is down. (Turned round: this had B tapped on another keyboard, and
        // staying down for the first one's sake.)
        procedure.stillThere(1000);
        QCOMPARE(press(procedure, keyB, 1000), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawB, true, 1001));
        QCOMPARE(release(procedure, keyB, 1010), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawB, false, 1011));
        QCOMPARE(written(ui::hook::whatToTell(procedure, 1012)), (QStringList{u"+5"_s, u"-5"_s, u"none down"_s}));
        QVERIFY(down(procedure).empty());
        QVERIFY(!procedure.look(1012 + 2 * moment).wake);
        QVERIFY(procedure.notesNoHearing());
    }

    void theHearingOfAKeyForSoundDoesNotTakeTheTellingOfTheLetter()
    {
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        constexpr input::Pressed rawB{0x30, false, 0x42};
        // Volume up is pressed; B is pressed a moment later, and the hook is not called for it
        // (Windows left it out, or took the hook out): only Raw Input tells. That telling waits
        // in vain for B to be heard, and the control ends. (Volume up's hearing, noted as B's,
        // took it: the two were at one, and a hook that heard nothing any more was not found
        // by that key.)
        Procedure missed;
        swallowing(missed);
        missed.rawInputTells();
        QCOMPARE(press(missed, keyA, 100), swallowedUntold);
        QVERIFY(missed.rawKey(rawA, true, 100));
        QCOMPARE(release(missed, keyA, 150), swallowedAndTold);
        QVERIFY(!missed.rawKey(rawA, false, 150));
        static_cast<void>(told(missed));
        missed.stillThere(500);
        QCOMPARE(press(missed, louder, 500), swallowedAndTold);
        QVERIFY(!missed.rawKey(rawB, true, 510));
        QVERIFY(!missed.look(510 + moment).wake);
        QVERIFY(missed.look(511 + moment).wake);
        QVERIFY(missed.isClosed());
        QCOMPARE(written(whatToTell(missed, 512 + moment)), QStringList{lost(ui::KeySource::Lost::NotHandedKeys)});

        // Where Raw Input does tell of such a key, that is nothing to the comparing either, in
        // whichever order the two come and whether the hook is called for the key or not (a
        // program that keeps the keys for sound to itself): nothing waits, and nothing ends.
        // What that costs: a hook that Windows took out is not found by such a key, and is
        // found by the next key of the keyboard's page.
        Procedure toldOf;
        swallowing(toldOf);
        toldOf.rawInputTells();
        QVERIFY(!tellAsRawInput(toldOf, rawLouder, true, 100));
        QCOMPARE(press(toldOf, louder, 101), swallowedAndTold);
        QCOMPARE(release(toldOf, louder, 150), swallowedAndTold);
        QVERIFY(!tellAsRawInput(toldOf, rawLouder, false, 151));
        QVERIFY(!tellAsRawInput(toldOf, rawLouder, true, 300));
        QVERIFY(!tellAsRawInput(toldOf, rawLouder, false, 350));
        toldOf.stillThere(351 + 2 * moment);
        QVERIFY(!toldOf.look(351 + 2 * moment).wake);
        QVERIFY(toldOf.swallows());
        QVERIFY(toldOf.notesNoHearing());
        QVERIFY(!toldOf.takeNotHanded());
    }

    // --- what an eighth reviewer found ---

    void aWordOfRawInputsIsForTheHearingItBelongsTo_data()
    {
        QTest::addColumn<bool>("isAModifier");
        QTest::newRow("Del, which is no modifier") << false;
        QTest::newRow("the left Shift, a modifier") << true;
    }

    void aWordOfRawInputsIsForTheHearingItBelongsTo()
    {
        QFETCH(bool, isAModifier);
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        constexpr input::Pressed rawDel{0x53, true, 0x2E};
        const Key key = isAModifier ? leftShift : del;
        const input::Pressed raw = isAModifier ? rawShift : rawDel;
        const int usage = isAModifier ? 0xE1 : 0x4C;
        const QString pressed = u"+"_s + QString::number(usage, 16);
        const QString letGo = u"-"_s + QString::number(usage, 16);
        // The key is held until it says itself again, let go, and pressed again, and the hook
        // hears all three before Raw Input's word for the first of them (its thread was a
        // little behind). The saying and the "up" go at once; the new press waits for the
        // other account's word, as every key that goes down.
        const auto heldLetGoAndPressedAgain = [&](Procedure& procedure) {
            swallowing(procedure);
            procedure.rawInputTells();
            static_cast<void>(press(procedure, key, 100));
            static_cast<void>(procedure.rawKey(raw, true, 101));
            static_cast<void>(press(procedure, key, 600));
            static_cast<void>(release(procedure, key, 610));
            static_cast<void>(press(procedure, key, 650));
            return written(whatToTell(procedure, 651));
        };
        Procedure procedure;
        QCOMPARE(heldLetGoAndPressedAgain(procedure), (QStringList{pressed, pressed, letGo, u"none down"_s}));
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{1});
        // Raw Input's word for the saying comes: that saying's, which was heard before the
        // key went up. The press that waits is not what it told of, and waits on. (It was
        // taken for that press's word: the press went out before its own word, and so before
        // anything that account had still to tell of between the two.)
        QVERIFY(!procedure.rawKey(raw, true, 655));
        QCOMPARE(written(whatToTell(procedure, 656)), QStringList());
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{1});
        // Nor is the press let go by that account's word for the "up" before it.
        QVERIFY(!procedure.rawKey(raw, false, 660));
        QCOMPARE(written(whatToTell(procedure, 661)), QStringList());
        QCOMPARE(down(procedure), (std::multiset<int>{usage}));
        // Its own word: now it goes.
        QVERIFY(procedure.rawKey(raw, true, 665));
        QCOMPARE(written(whatToTell(procedure, 666)), QStringList{pressed});
        QCOMPARE(procedure.onTheirWay().count, std::uint32_t{0});
        QCOMPARE(release(procedure, key, 700), swallowedAndTold);
        QVERIFY(!procedure.rawKey(raw, false, 701));
        QCOMPARE(written(whatToTell(procedure, 702)), (QStringList{letGo, u"none down"_s}));
        QVERIFY(!procedure.look(702 + 5 * moment).wake);
        QVERIFY(procedure.swallows());
        QVERIFY(procedure.notesNoHearing());

        // What the gate's rule is for: no key goes to a machine before a key that was left
        // out. Between the "up" and the new press Ctrl went down, and the hook was not called
        // for it; Raw Input tells of it in its turn, behind its word for the "up" and before
        // its word for the press. By then the press still waits, since no word was its own:
        // it never goes, and the control ends over the key that was not heard. (The press had
        // gone to the machine by the saying's word, bare, before Ctrl was told of at all.)
        Procedure leftOut;
        static_cast<void>(heldLetGoAndPressedAgain(leftOut));
        QVERIFY(!leftOut.rawKey(raw, true, 655));
        QVERIFY(!leftOut.rawKey(raw, false, 660));
        QVERIFY(!leftOut.rawKey(rawCtrl, true, 662));
        QVERIFY(!leftOut.rawKey(raw, true, 665));
        QCOMPARE(written(whatToTell(leftOut, 666)), QStringList());
        QVERIFY(!leftOut.look(662 + moment).wake);
        QVERIFY(leftOut.look(663 + moment).wake);
        QVERIFY(leftOut.isClosed());
        QCOMPARE(written(whatToTell(leftOut, 664 + moment)), QStringList{lost(ui::KeySource::Lost::NotHandedKeys)});
        QCOMPARE(kept(leftOut), (std::multiset<int>{usage}));

        // And where the first press itself still waits, with a saying of it behind: each of
        // the two presses goes by its own word, and the saying's word lets neither go.
        Procedure both;
        swallowing(both);
        both.rawInputTells();
        QCOMPARE(press(both, key, 100), swallowedUntold);
        QVERIFY(press(both, key, 133).swallow);
        QCOMPARE(release(both, key, 140), swallowedUntold);
        QCOMPARE(press(both, key, 180), swallowedUntold);
        QCOMPARE(written(whatToTell(both, 181)), QStringList());
        QVERIFY(both.rawKey(raw, true, 190));
        QCOMPARE(written(whatToTell(both, 191)), (QStringList{pressed, letGo}));
        QVERIFY(!both.rawKey(raw, true, 192));
        QCOMPARE(written(whatToTell(both, 193)), QStringList());
        QVERIFY(!both.rawKey(raw, false, 194));
        QCOMPARE(written(whatToTell(both, 195)), QStringList());
        QVERIFY(both.rawKey(raw, true, 196));
        QCOMPARE(written(whatToTell(both, 197)), QStringList{pressed});
        QVERIFY(both.swallows());
    }

    void whatWasToldIsWipedWhereItStood()
    {
        using ui::hook::Told;
        // What the interface thread has to tell stands in a row until it is said (`Telling`).
        // A key is handed on and forgotten: each place of that row is wiped as what stood
        // there is taken out, and nothing of a key is left in the memory behind it. (What was
        // taken out was left standing where it was, and so was all of it when the row was
        // forgotten: only its length was set to nothing, for as long as the program lived.)
        const auto holdsSomething = [](const Told& place) {
            const Heard& heard = place.heard;
            return place.what != Told::What::Key || heard.key.scanCode != 0 || heard.key.virtualKey != 0 ||
                   heard.key.extended || heard.down || heard.atMs != 0 || heard.swallowed || heard.nothingIsDown ||
                   heard.number != 0;
        };
        ui::hook::Telling telling;
        const auto placesThatHoldSomething = [&telling, &holdsSomething] {
            return std::ranges::count_if(telling.memory(), holdsSomething);
        };
        Procedure procedure;
        swallowing(procedure);
        const auto typeAAndC = [&procedure](std::uint64_t at) {
            procedure.stillThere(at);
            static_cast<void>(press(procedure, keyA, at));
            static_cast<void>(release(procedure, keyA, at + 10));
            static_cast<void>(press(procedure, keyC, at + 20));
            static_cast<void>(release(procedure, keyC, at + 30));
        };
        // A and C are typed. While the first of it is told, the rest stands in the row: looked
        // at from inside that telling, so that this does look at the memory it means.
        ui::KeysByHand whoeverListens;
        int keysTold = 0;
        qsizetype stoodMeanwhile = 0;
        connect(&whoeverListens, &ui::KeySource::keyChanged, &whoeverListens, [&] {
            if (keysTold == 0) {
                stoodMeanwhile = placesThatHoldSomething();
            }
            ++keysTold;
        });
        typeAAndC(100);
        telling.all(procedure, 140, whoeverListens, [] { return true; });
        QCOMPARE(keysTold, 4);
        QVERIFY2(stoodMeanwhile >= 3, qPrintable(QString::number(stoodMeanwhile)));
        QCOMPARE(placesThatHoldSomething(), qsizetype{0});
        // Where whoever was told ends the listening in the middle, what was not said goes with
        // it, and is wiped as well.
        typeAAndC(200);
        telling.all(procedure, 240, whoeverListens, [&keysTold] { return keysTold < 6; });
        QCOMPARE(keysTold, 6);
        QCOMPARE(placesThatHoldSomething(), qsizetype{0});
        // And where the row is forgotten while something stands in it (the hook is ended from
        // inside a telling).
        typeAAndC(300);
        bool isForgotten = false;
        connect(&whoeverListens, &ui::KeySource::keyChanged, &whoeverListens, [&] {
            if (keysTold == 7) {
                telling.forget();
                isForgotten = true;
            }
        });
        telling.all(procedure, 340, whoeverListens, [&isForgotten] { return !isForgotten; });
        QCOMPARE(keysTold, 7);
        QCOMPARE(placesThatHoldSomething(), qsizetype{0});
        // More than the row has room for at first: it grows, and what it outgrew is wiped
        // before it is given up. (Told in the middle of it, the row holds every key of it.)
        // Each row that is outgrown is looked at, at the moment it is given up: it stood full
        // of keys, and not a byte of it is left that is not nothing. (Nobody looked at those
        // rows: only at the one that took their place. And they were wiped by writing an empty
        // place over each, which a build that is made fast may leave out, since the memory is
        // given back two lines later.)
        int rowsOutgrown = 0;
        std::size_t placesInThem = 0;
        qsizetype keysLeftInThem = 0;
        qsizetype bytesLeftInThem = 0;
        telling.whenARowIsOutgrown([&](std::span<const Told> row) {
            ++rowsOutgrown;
            placesInThem += row.size();
            keysLeftInThem += std::ranges::count_if(row, holdsSomething);
            bytesLeftInThem +=
                std::ranges::count_if(std::as_bytes(row), [](std::byte one) { return one != std::byte{0}; });
        });
        for (std::uint64_t tap = 0; tap < 40; ++tap) {
            static_cast<void>(press(procedure, keyA, 400 + tap));
            static_cast<void>(release(procedure, keyA, 400 + tap));
        }
        isForgotten = false;
        telling.all(procedure, 450, whoeverListens, [] { return true; });
        QCOMPARE(keysTold, 7 + 80);
        QCOMPARE(placesThatHoldSomething(), qsizetype{0});
        QVERIFY2(rowsOutgrown >= 2, qPrintable(QString::number(rowsOutgrown)));
        QVERIFY2(placesInThem >= 48, qPrintable(QString::number(placesInThem)));
        QCOMPARE(keysLeftInThem, qsizetype{0});
        QCOMPARE(bytesLeftInThem, qsizetype{0});
        // And a row that is forgotten is left with nothing in it either, byte for byte.
        typeAAndC(500);
        telling.all(procedure, 540, whoeverListens, [] { return false; });
        QCOMPARE(
            std::ranges::count_if(std::as_bytes(telling.memory()), [](std::byte one) { return one != std::byte{0}; }),
            qsizetype{0});
    }

    void aKeyIsRightAtEveryStepOfEveryShortSequence_data()
    {
        QTest::addColumn<bool>("isAModifier");
        QTest::addColumn<int>("keyboards");
        QTest::addColumn<bool>("looksInTheMiddle");
        QTest::newRow("the left Shift, on one keyboard") << true << 1 << false;
        QTest::newRow("the left Shift, on two keyboards") << true << 2 << false;
        QTest::newRow("Enter, on one keyboard") << false << 1 << false;
        QTest::newRow("Enter, on two keyboards") << false << 2 << false;
        QTest::newRow("the left Shift, on two keyboards, with a look in the middle") << true << 2 << true;
    }

    void aKeyIsRightAtEveryStepOfEveryShortSequence()
    {
        QFETCH(bool, isAModifier);
        QFETCH(int, keyboards);
        QFETCH(bool, looksInTheMiddle);
        constexpr int length = 6;
        const Key key = isAModifier ? leftShift : keyEnter;
        const input::Pressed raw = isAModifier ? rawShift : rawEnter;
        const int usage = isAModifier ? 0xE1 : 0x28;
        // One key, on one keyboard or on two: every sequence of six sayings of the key (a
        // press, or the key said again by a keyboard that holds it) and goings up that fingers
        // can make, and every order in which the two accounts can come to tell of them, each in
        // the order the keys came in: the hook ahead by any number, Raw Input ahead by any
        // number, or the one and then the other.
        //
        // Held to at every step, against what the fingers did and what each account has told
        // by then, and not against the procedure's own table. The rule: the key is down from a
        // saying of it until any keyboard lets go of it.
        // * Whoever listens is told of the key going down and going up only as the rule has
        //   it, in that order: never as up while by the rule it is down (no key is let go under
        //   a finger but by the rule), and never as down where no going down was heard.
        // * A press is told only when Raw Input's own word for it has come, and while no key
        //   that Raw Input told of waits to be heard: the gate.
        // * Once it is down for whoever listens and its going up was heard, it is up for them
        //   by the end of that very step: an "up" waits for nobody's word.
        // * While nothing that Raw Input told of waits to be heard, every press whose word has
        //   come was told, and what was heard behind it.
        // * The procedure's table has the key as the rule has it; nothing ends; and at the end
        //   nothing waits. So no key stays down, for whoever listens or in the table, once
        //   every finger has left it: the last thing a finger did then was to let go.
        // * Every key is kept from Windows; and whenever something was handed on in a step,
        //   that step said that the interface thread is to be woken. (This takes what was
        //   handed on by itself, after every step: a key that was handed on with nobody woken
        //   passed, and would have lain in the ring until the next key.)
        //
        // (In the place of "a modifier is right for every short sequence with the hook ahead",
        // which looked at the end of each sequence only: a Shift that was told as up and as
        // down again under a finger that never left it passed, and so did a press that went
        // out before its word. And it held the telling apart of keyboards, which is gone.)
        //
        // What the rows "on two keyboards" hold: one stream of sayings and goings up in which
        // the key may go up once more than it went down since. That is all a second keyboard
        // is to a hook, which is not told where a key came from, and Raw Input's word is given
        // here without a keyboard as well. They are one keyboard's events with one more "up",
        // and nothing else of two keyboards is tried by them.
        //
        // What the row with a look in the middle holds: the thread looks once, half way
        // through each sequence and a tenth of a second on, and everything above holds on. No
        // moment runs out by that: nothing is dropped as overdue, and nothing ends.
        //
        // What this leaves out: a moment that runs out (each step is a millisecond after the
        // last), and so what is dropped as overdue meeting the counting of sayings (one such
        // sequence, by hand: `aPressGoesByItsOwnWordOnceSayingsThatWereNeverToldOfAreAMomentOld`;
        // more sayings than there is room to note the time of do come up here, with the hook
        // four and more ahead); a telling or a hearing that is lost; a second key, a modifier
        // or another, between; a key that was down before the hook went in; more than six; and
        // an interface thread that takes what it is handed late (here it is taken after every
        // step, by the test).
        Procedure procedure;
        int tried = 0;
        QStringList wrong;
        for (std::uint32_t moves = 0; moves < (1U << length); ++moves) {
            // (A bit for each of the six, set where the key goes down. Only what fingers can
            // do: a keyboard lets go only of a key that it holds.)
            const auto goesDown = [moves](int at) { return ((moves >> at) & 1U) != 0; };
            int holders = 0;
            bool isPossible = true;
            for (int at = 0; at < length && isPossible; ++at) {
                if (goesDown(at)) {
                    holders = std::min(holders + 1, keyboards);
                } else {
                    isPossible = holders > 0;
                    --holders;
                }
            }
            if (!isPossible) {
                continue;
            }
            // The turns of the two accounts: a bit for each turn, set where it is the hook's.
            for (std::uint32_t turns = 0; turns < (1U << (2 * length)); ++turns) {
                if (std::popcount(turns) != length) {
                    continue;
                }
                ++tried;
                procedure.begin(0);
                procedure.rawInputTells();
                procedure.setSwallowing(true, 0);
                // (Raw Input has told of this keyboard before: C, pressed and let go.)
                std::uint64_t now = 10;
                static_cast<void>(press(procedure, keyC, now));
                static_cast<void>(procedure.rawKey(rawC, true, now));
                static_cast<void>(release(procedure, keyC, now));
                static_cast<void>(procedure.rawKey(rawC, false, now));
                static_cast<void>(heard(procedure));
                // What the fingers did, as far as the hook has heard: whether the key is down
                // by the rule, and at the how-manieth hearing it went down and up, turn about.
                int heardSoFar = 0;
                int toldSoFar = 0;
                bool isDownByTheRule = false;
                std::vector<int> changedAt;
                // What whoever listens has of it, by what it was told.
                std::size_t changesTold = 0;
                bool isDownForWhoeverListens = false;
                QString found;
                for (int turn = 0; turn < 2 * length && found.isEmpty(); ++turn) {
                    ++now;
                    // Whether this step said that the interface thread is to be woken.
                    bool woke = false;
                    if (((turns >> turn) & 1U) != 0) {
                        const bool down = goesDown(heardSoFar);
                        if (down != isDownByTheRule) {
                            changedAt.push_back(heardSoFar);
                            isDownByTheRule = down;
                        }
                        const Verdict verdict = procedure.key(key.name, key.scanCode, key.flags, down, now);
                        woke = verdict.tell;
                        if (!verdict.swallow) {
                            found = u"a key was not kept from Windows"_s;
                        }
                        ++heardSoFar;
                    } else {
                        woke = procedure.rawKey(raw, goesDown(toldSoFar), now);
                        ++toldSoFar;
                    }
                    if (looksInTheMiddle && turn == length - 1) {
                        now += ui::hook::lookEveryMs;
                        procedure.stillThere(now);
                        woke = procedure.look(now).wake || woke;
                    }
                    bool wasHandedOn = false;
                    while (const std::optional<Heard> next = procedure.next()) {
                        wasHandedOn = true;
                        if (!found.isEmpty()) {
                            continue;
                        }
                        if (input::usageOf(next->key) != usage) {
                            found = u"another key was told"_s;
                            continue;
                        }
                        if (next->down == isDownForWhoeverListens) {
                            // (The key said again, or an "up" of a key that is up: nothing new.)
                            continue;
                        }
                        isDownForWhoeverListens = next->down;
                        if (changesTold == changedAt.size()) {
                            found = next->down ? u"told as down where no going down was heard that was not told"_s
                                               : u"told as up while by the rule it is down"_s;
                        } else if (next->down && toldSoFar <= changedAt.at(changesTold)) {
                            found = u"a press was told before Raw Input's word for it"_s;
                        } else if (next->down && toldSoFar > heardSoFar) {
                            found = u"a press was told while a key that Raw Input told of was not heard"_s;
                        }
                        ++changesTold;
                    }
                    if (found.isEmpty() && wasHandedOn && !woke) {
                        found = u"a key was handed on, and the interface thread was not to be woken"_s;
                    }
                    if (!found.isEmpty()) {
                        break;
                    }
                    // How many of the goings down and up are due by now: each press whose word
                    // has come, and the "up" behind it.
                    std::size_t due = 0;
                    while (due < changedAt.size() && (due % 2 == 1 || changedAt.at(due) < toldSoFar)) {
                        ++due;
                    }
                    if (procedure.isClosed() || !procedure.swallows()) {
                        found = u"it ended"_s;
                    } else if ((down(procedure).count(usage) != 0) != isDownByTheRule) {
                        found = isDownByTheRule ? u"up in the table while by the rule it is down"_s
                                                : u"down in the table while by the rule it is up"_s;
                    } else if (isDownForWhoeverListens && changesTold < changedAt.size()) {
                        found = u"still down for whoever listens when its going up was heard"_s;
                    } else if (toldSoFar <= heardSoFar && changesTold < due) {
                        found = u"a press whose word has come was not told"_s;
                    }
                }
                if (found.isEmpty()) {
                    static_cast<void>(procedure.look(now + 1));
                    if (procedure.isClosed() || procedure.onTheirWay().count != 0 || procedure.next().has_value() ||
                        isDownForWhoeverListens != isDownByTheRule) {
                        found = u"not as the rule has it once both accounts have told of everything"_s;
                    }
                }
                if (!found.isEmpty() && wrong.size() < 5) {
                    QString said;
                    for (int at = 0; at < length; ++at) {
                        said += goesDown(at) ? u'+' : u'-';
                    }
                    wrong.append(
                        u"%1 (the hook's turns, the first of them lowest: %2), after %3 heard and %4 told: %5"_s
                            .arg(said)
                            .arg(turns, 2 * length, 2, QLatin1Char('0'))
                            .arg(heardSoFar)
                            .arg(toldSoFar)
                            .arg(found));
                }
            }
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u"; "_s)));
        // (And it did try them: else this showed nothing.)
        QVERIFY2(tried > 10'000, qPrintable(QString::number(tried)));
    }

    void aModifierAndAKeyAreRightAtEveryStepOfEveryShortSequence()
    {
        constexpr int length = 5;
        // The same with two keys, a modifier and a key that is typed under it: Shift and C, on
        // one keyboard, every sequence of five sayings and goings up of the two, and every
        // order in which the two accounts can come to tell of them. Held to at every step,
        // against what the fingers did and what each account has told by then:
        // * What whoever listens is told of each key is what the rule has of it, in its order.
        // * A press is told only when Raw Input's own word for it has come, while no key that
        //   Raw Input told of waits to be heard, and behind every press that was heard before
        //   it: none goes to a machine before a key that may have been left out.
        // * Shift is never told as up before a key that went down before that "up" was heard
        //   and is not told yet: that key was pressed under it, and would arrive bare.
        // * Whatever may go by those rules has gone by the end of the step: a press whose word
        //   has come, and an "up" whose own press was told (C's at once, past a press of Shift
        //   that waits; Shift's as soon as no press before it waits).
        // * The table has each key as the rule has it, nothing ends, and at the end nothing
        //   waits.
        // * Every key is kept from Windows; and whenever something was handed on in a step,
        //   that step said that the interface thread is to be woken (this takes what was
        //   handed on by itself, after every step).
        // What this leaves out is what the test of one key leaves out, but the second key; and
        // it has no look inside a sequence, and one keyboard only.
        struct Of {
            Key key;
            input::Pressed raw;
            int usage = 0;
            bool isAModifier = false;
        };
        const std::array<Of, 2> keys = {{{leftShift, rawShift, 0xE1, true}, {keyC, rawC, 0x06, false}}};
        struct Move {
            std::size_t key = 0;
            bool down = false;
        };
        // A going down or up by the rule: of which key, and at the how-manieth hearing.
        struct Change {
            std::size_t key = 0;
            bool down = false;
            int heardAt = 0;
        };
        int sequences = 1;
        for (int at = 0; at < length; ++at) {
            sequences *= 4;
        }
        Procedure procedure;
        int tried = 0;
        QStringList wrong;
        for (int code = 0; code < sequences; ++code) {
            std::vector<Move> moves;
            std::vector<Change> changes;
            std::array<bool, 2> isDown{};
            bool isPossible = true;
            for (int at = 0, rest = code; at < length && isPossible; ++at, rest /= 4) {
                const Move move{static_cast<std::size_t>((rest % 4) / 2), (rest % 4) % 2 == 0};
                // (Only what fingers can do: a key that is up is not let go.)
                isPossible = move.down || isDown.at(move.key);
                if (move.down != isDown.at(move.key)) {
                    changes.push_back({move.key, move.down, at});
                }
                isDown.at(move.key) = move.down;
                moves.push_back(move);
            }
            if (!isPossible) {
                continue;
            }
            for (std::uint32_t turns = 0; turns < (1U << (2 * length)); ++turns) {
                if (std::popcount(turns) != length) {
                    continue;
                }
                ++tried;
                procedure.begin(0);
                procedure.rawInputTells();
                procedure.setSwallowing(true, 0);
                // (Raw Input has told of this keyboard before: A, pressed and let go.)
                std::uint64_t now = 10;
                static_cast<void>(press(procedure, keyA, now));
                static_cast<void>(procedure.rawKey(rawA, true, now));
                static_cast<void>(release(procedure, keyA, now));
                static_cast<void>(procedure.rawKey(rawA, false, now));
                static_cast<void>(heard(procedure));
                int heardSoFar = 0;
                int toldSoFar = 0;
                // What whoever listens has of each key, and which of the goings down and up it
                // was told.
                std::array<bool, 2> isDownForWhoeverListens{};
                std::vector<bool> isTold(changes.size(), false);
                // A press that was heard before the one at this place and is not told yet.
                const auto aPressWaitsBefore = [&changes, &isTold](std::size_t place) {
                    for (std::size_t before = 0; before < place; ++before) {
                        if (changes.at(before).down && !isTold.at(before)) {
                            return true;
                        }
                    }
                    return false;
                };
                QString found;
                for (int turn = 0; turn < 2 * length && found.isEmpty(); ++turn) {
                    ++now;
                    // Whether this step said that the interface thread is to be woken.
                    bool woke = false;
                    if (((turns >> turn) & 1U) != 0) {
                        const Move& move = moves.at(static_cast<std::size_t>(heardSoFar));
                        const Key& key = keys.at(move.key).key;
                        const Verdict verdict = procedure.key(key.name, key.scanCode, key.flags, move.down, now);
                        woke = verdict.tell;
                        if (!verdict.swallow) {
                            found = u"a key was not kept from Windows"_s;
                        }
                        ++heardSoFar;
                    } else {
                        const Move& move = moves.at(static_cast<std::size_t>(toldSoFar));
                        woke = procedure.rawKey(keys.at(move.key).raw, move.down, now);
                        ++toldSoFar;
                    }
                    bool wasHandedOn = false;
                    while (const std::optional<Heard> next = procedure.next()) {
                        wasHandedOn = true;
                        if (!found.isEmpty()) {
                            continue;
                        }
                        const int usage = input::usageOf(next->key);
                        if (usage != keys.at(0).usage && usage != keys.at(1).usage) {
                            found = u"another key was told"_s;
                            continue;
                        }
                        const std::size_t which = usage == keys.at(0).usage ? 0 : 1;
                        if (next->down == isDownForWhoeverListens.at(which)) {
                            // (The key said again, or an "up" of a key that is up: nothing new.)
                            continue;
                        }
                        isDownForWhoeverListens.at(which) = next->down;
                        // The first going down or up of that key which was not told yet.
                        std::size_t place = 0;
                        while (place < changes.size() && (changes.at(place).key != which || isTold.at(place))) {
                            ++place;
                        }
                        if (place == changes.size() || changes.at(place).heardAt >= heardSoFar) {
                            found = next->down ? u"told as down where no going down was heard that was not told"_s
                                               : u"told as up while by the rule it is down"_s;
                        } else if (next->down && toldSoFar <= changes.at(place).heardAt) {
                            found = u"a press was told before Raw Input's word for it"_s;
                        } else if (next->down && toldSoFar > heardSoFar) {
                            found = u"a press was told while a key that Raw Input told of was not heard"_s;
                        } else if (next->down && aPressWaitsBefore(place)) {
                            found = u"a press was told before one that was heard before it"_s;
                        } else if (!next->down && keys.at(which).isAModifier && aPressWaitsBefore(place)) {
                            found = u"the modifier was told as up before a key that was pressed under it"_s;
                        } else {
                            isTold.at(place) = true;
                        }
                    }
                    if (found.isEmpty() && wasHandedOn && !woke) {
                        found = u"a key was handed on, and the interface thread was not to be woken"_s;
                    }
                    if (!found.isEmpty()) {
                        break;
                    }
                    if (procedure.isClosed() || !procedure.swallows()) {
                        found = u"it ended"_s;
                        break;
                    }
                    // By the rule, after what was heard: which keys are down; and what of it has
                    // to have been told by now.
                    std::array<bool, 2> isDownByTheRule{};
                    std::array<bool, 2> isItsPressTold{};
                    for (std::size_t place = 0; place < changes.size() && found.isEmpty(); ++place) {
                        const Change& change = changes.at(place);
                        if (change.heardAt >= heardSoFar) {
                            break;
                        }
                        isDownByTheRule.at(change.key) = change.down;
                        if (change.down) {
                            isItsPressTold.at(change.key) = isTold.at(place);
                            if (!isTold.at(place) && toldSoFar <= heardSoFar && change.heardAt < toldSoFar &&
                                !aPressWaitsBefore(place)) {
                                found = u"a press whose word has come was not told"_s;
                            }
                        } else if (!isTold.at(place) && isItsPressTold.at(change.key) &&
                                   !(keys.at(change.key).isAModifier && aPressWaitsBefore(place))) {
                            found = u"still down for whoever listens when its going up was heard"_s;
                        }
                    }
                    const std::multiset<int> inTheTable = down(procedure);
                    for (std::size_t which = 0; which < keys.size() && found.isEmpty(); ++which) {
                        if ((inTheTable.count(keys.at(which).usage) != 0) != isDownByTheRule.at(which)) {
                            found = u"not in the table as the rule has it"_s;
                        }
                    }
                }
                if (found.isEmpty()) {
                    static_cast<void>(procedure.look(now + 1));
                    if (procedure.isClosed() || procedure.onTheirWay().count != 0 || procedure.next().has_value() ||
                        std::find(isTold.cbegin(), isTold.cend(), false) != isTold.cend()) {
                        found = u"not as the rule has it once both accounts have told of everything"_s;
                    }
                }
                if (!found.isEmpty() && wrong.size() < 5) {
                    QString said;
                    for (const Move& move : moves) {
                        said += u"%1%2 "_s.arg(move.key == 0 ? u"Shift"_s : u"C"_s).arg(move.down ? u'+' : u'-');
                    }
                    wrong.append(u"%1(the hook's turns, the first of them lowest: %2), after %3 heard and %4 told: %5"_s
                                     .arg(said)
                                     .arg(turns, 2 * length, 2, QLatin1Char('0'))
                                     .arg(heardSoFar)
                                     .arg(toldSoFar)
                                     .arg(found));
                }
            }
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u"; "_s)));
        // (And it did try them: else this showed nothing.)
        QVERIFY2(tried > 10'000, qPrintable(QString::number(tried)));
    }

    // --- what a ninth reviewer found ---

    void whatWindowsSaysOfADeviceThatComesOrGoesIsReadForAKeyboardThatWent()
    {
        using ui::hook::deviceCame;
        using ui::hook::deviceWent;
        using ui::hook::kindKeyboard;
        using ui::hook::kindMouse;
        using ui::hook::meansAKeyboardWent;
        // What Windows says of a device that goes, as made-up notices: a keyboard that went is
        // one that Windows still names as a keyboard, and one of which it says nothing any
        // more (the word is asked for of keyboards only, and a device that is gone may not be
        // there to be asked).
        QVERIFY(meansAKeyboardWent(deviceWent, kindKeyboard));
        QVERIFY(meansAKeyboardWent(deviceWent, std::nullopt));
        // A mouse, or another device that is no keyboard, going: nothing.
        QVERIFY(!meansAKeyboardWent(deviceWent, kindMouse));
        QVERIFY(!meansAKeyboardWent(deviceWent, 2U));
        QVERIFY(!meansAKeyboardWent(deviceWent, 77U));
        // A keyboard that comes ends nothing, whatever is known of it; nor does a word that is
        // neither of the two.
        QVERIFY(!meansAKeyboardWent(deviceCame, kindKeyboard));
        QVERIFY(!meansAKeyboardWent(deviceCame, std::nullopt));
        QVERIFY(!meansAKeyboardWent(0U, kindKeyboard));
        QVERIFY(!meansAKeyboardWent(3U, std::nullopt));
    }

    void aKeyThatIsNotHeardInTheFirstMomentAfterRawInputBeginsToTellEndsTheControl()
    {
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // A PC that is taken not to tell. A keyboard that Raw Input does tell of is plugged in,
        // and Enter is tapped on it, heard and told of in step: the comparing begins there.
        Procedure procedure;
        takenNotToTell(procedure);
        QVERIFY(!procedure.isWatched());
        procedure.stillThere(2000);
        QCOMPARE(press(procedure, keyEnter, 2000), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawEnter, true, 2001));
        QVERIFY(procedure.isWatched());
        QCOMPARE(release(procedure, keyEnter, 2050), swallowedAndTold);
        QVERIFY(!procedure.rawKey(rawEnter, false, 2051));
        QCOMPARE(told(procedure), (QStringList{u"+28"_s, u"-28"_s}));
        // Within the moment of that first telling Ctrl goes down, and the hook is not called
        // for it (Windows left it out): only Raw Input tells. C is typed under it, heard and
        // told of, and waits: a key that was told of has still to be heard.
        QVERIFY(!procedure.rawKey(rawCtrl, true, 2100));
        QCOMPARE(press(procedure, keyC, 2150), swallowedUntold);
        QVERIFY(!procedure.rawKey(rawC, true, 2151));
        QCOMPARE(release(procedure, keyC, 2200), swallowedUntold);
        QVERIFY(!procedure.rawKey(rawC, false, 2201));
        QVERIFY(told(procedure).isEmpty());
        procedure.stillThere(2100 + moment);
        QVERIFY(!procedure.look(2100 + moment).wake);
        // Ctrl's telling has waited its moment in vain. It went down after that account began
        // to tell, and is held against the hook as any other: the control ends, and C went to
        // no machine. Only its going up is told, which nobody is to wait for. (Every telling of
        // that first moment which found no hearing was forgiven, as one that may be the word
        // for a key heard before any hearing was noted: Ctrl's was, and "c" went out bare, with
        // nothing said. Outside that moment the same ended the control.)
        QVERIFY(procedure.look(2101 + moment).wake);
        QCOMPARE(written(whatToTell(procedure, 2102 + moment)),
                 (QStringList{lost(ui::KeySource::Lost::BeganToTell), u"-6"_s, u"none down"_s}));
        QVERIFY(procedure.isClosed());
        QVERIFY(!procedure.swallows());
        QVERIFY(kept(procedure).empty());
    }

    void aPressGoesByItsOwnWordOnceSayingsThatWereNeverToldOfAreAMomentOld()
    {
        using ui::hook::whatToTell;
        constexpr std::uint64_t moment = ui::hook::heardWithinMs;
        // Enter is held until it has said itself again five times, and let go; and Raw Input
        // tells of none of those sayings. (It is taken to tell of each. Where it does not,
        // nothing ends by that, but what was noted of them waits to be told of, and counts
        // before a press of the key that comes while it does.) The thread looks ten times a
        // second all the while.
        const auto heldAndLetGo = [](Procedure& procedure) {
            swallowing(procedure);
            procedure.rawInputTells();
            static_cast<void>(press(procedure, keyEnter, 100));
            static_cast<void>(procedure.rawKey(rawEnter, true, 101));
            for (std::uint64_t said = 0; said < 5; ++said) {
                static_cast<void>(press(procedure, keyEnter, 600 + 33 * said));
            }
            static_cast<void>(release(procedure, keyEnter, 740));
            static_cast<void>(procedure.rawKey(rawEnter, false, 741));
            static_cast<void>(told(procedure));
        };
        const auto looksUntil = [](Procedure& procedure, std::uint64_t until) {
            bool woke = false;
            for (std::uint64_t at = 800; at <= until; at += ui::hook::lookEveryMs) {
                procedure.stillThere(at);
                woke = procedure.look(at).wake || woke;
            }
            return woke;
        };
        // Enter is pressed anew a moment and a half after the last of those sayings (at 732).
        // None of them is this press's hearing, and a telling takes none that is older than
        // the moment: the press goes by its own word, and nothing ends. (What was noted of the
        // sayings stood for two moments and a look, and the new press's own hearing renewed the
        // time of those that had no room: Raw Input's word for the press was taken for a
        // saying's, the press waited on, and after its moment the control ended, as over a key
        // that Windows did not report.)
        Procedure later;
        heldAndLetGo(later);
        QVERIFY(!looksUntil(later, 1300));
        later.stillThere(1332);
        QCOMPARE(press(later, keyEnter, 732 + moment + moment / 2), swallowedUntold);
        QVERIFY(later.rawKey(rawEnter, true, 1333));
        QCOMPARE(told(later), QStringList{u"+28"_s});
        QCOMPARE(release(later, keyEnter, 1400), swallowedAndTold);
        QVERIFY(!later.rawKey(rawEnter, false, 1401));
        QCOMPARE(written(whatToTell(later, 1402)), (QStringList{u"-28"_s, u"none down"_s}));
        later.stillThere(1402 + 2 * moment);
        QVERIFY(!later.look(1402 + 2 * moment).wake);
        QVERIFY(later.swallows());
        QVERIFY(!later.isClosed());
        QVERIFY(later.notesNoHearing());

        // Pressed anew within the moment of such a saying, it is as the header has it, and not
        // mended: Raw Input's word for the press is taken for the saying's, the press waits for
        // a word that does not come, and the control ends there. The key went to no machine.
        Procedure sooner;
        heldAndLetGo(sooner);
        sooner.stillThere(800);
        QCOMPARE(press(sooner, keyEnter, 800), swallowedUntold);
        QVERIFY(!sooner.rawKey(rawEnter, true, 801));
        QVERIFY(told(sooner).isEmpty());
        sooner.stillThere(800 + moment);
        QVERIFY(!sooner.look(800 + moment).wake);
        QVERIFY(sooner.look(801 + moment).wake);
        QVERIFY(sooner.isClosed());
        QCOMPARE(written(whatToTell(sooner, 802 + moment)), QStringList{lost(ui::KeySource::Lost::NotToldOfKeys)});
        QCOMPARE(kept(sooner), (std::multiset<int>{0x28}));
    }

    void whatAThreadThatWasLeftBehindWorkedOnIsForgottenByWhicheverComesLast()
    {
        using ui::hook::LastToGo;
        // A hook's thread that did not end in the time it was given is left to end by itself,
        // and what it worked on is forgotten when it has: by the thread, where whoever waited
        // has gone on; and by whoever waited, where the thread ended in the very moment in
        // which it was given up. By one of the two, in whichever order they come. (Such a
        // thread was let go with its tables as they stood: which keys were down, and when each
        // was heard, until the program let go of the last of it.)
        LastToGo threadFirst;
        QVERIFY(!threadFirst.theThreadHasEnded());
        QVERIFY(threadFirst.isWaitedForNoLonger());
        LastToGo waiterFirst;
        QVERIFY(!waiterFirst.isWaitedForNoLonger());
        QVERIFY(waiterFirst.theThreadHasEnded());
        // And with the two at the same moment, time and again: one of them, never both and
        // never neither.
        for (int round = 0; round < 300; ++round) {
            LastToGo both;
            std::atomic<int> forgotten{0};
            std::thread thread([&both, &forgotten] {
                if (both.theThreadHasEnded()) {
                    ++forgotten;
                }
            });
            if (both.isWaitedForNoLonger()) {
                ++forgotten;
            }
            thread.join();
            QCOMPARE(forgotten.load(), 1);
        }
    }
};

QTEST_APPLESS_MAIN(TestKeyboardHook)

#include "test_keyboard_hook.moc"
