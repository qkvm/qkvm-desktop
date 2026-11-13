#include "win/keyboard_hook.h"
#include "win/leave_key.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <semaphore>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <QPointer>
#include <QWinEventNotifier>
#include <QWindow>

// Windows' own headers.
#include <windows.h>
#include <wtsapi32.h>

namespace qkvm::ui {

namespace hook {

namespace {

// The keyboard's numbers for the keys Windows itself takes the keyboard with, and Windows' name
// for L (its lock goes by the name, wherever a layout has the letter).
constexpr std::uint32_t scanCtrl = 0x1D;
constexpr std::uint32_t scanAlt = 0x38;
constexpr std::uint32_t scanDel = 0x53;
constexpr std::uint32_t scanLeftWindows = 0x5B;
constexpr std::uint32_t scanRightWindows = 0x5C;
constexpr std::uint32_t nameOfL = 0x4C;
// The left Ctrl that Windows makes up under AltGr: its number, which no key has, and its name.
constexpr std::uint32_t scanMadeUpCtrl = 0x21D;
constexpr std::uint32_t nameOfLeftCtrl = 0xA2;
constexpr std::uint16_t usageRightAlt = 0xE6;
// The keys the hook and Raw Input tell of in two ways: the right Shift, Num Lock and Pause. And
// the name Windows gives what is no key of its own.
constexpr std::uint32_t scanRightShift = 0x36;
constexpr std::uint32_t nameOfRightShift = 0xA1;
constexpr std::uint32_t scanNumLock = 0x45;
constexpr std::uint32_t nameOfPause = 0x13;
constexpr std::uint32_t nameOfNoKey = 0xFF;

bool isSamePlace(const input::Pressed& one, const input::Pressed& other) noexcept
{
    return one.scanCode == other.scanCode && one.extended == other.extended;
}

// Whether a time that was noted (plus one: 0 is "none") is longer ago than the moment.
bool isOverdue(std::uint64_t noted, std::uint64_t nowMs, std::uint64_t withinMs) noexcept
{
    return noted != 0 && nowMs + 1 > noted && nowMs + 1 - noted > withinMs;
}

// The oldest of what waits; nothing: none does.
template <std::size_t Many>
std::uint64_t* oldestOf(std::array<std::uint64_t, Many>& waiting) noexcept
{
    std::uint64_t* oldest = nullptr;
    for (std::uint64_t& one : waiting) {
        if (one != 0 && (oldest == nullptr || one < *oldest)) {
            oldest = &one;
        }
    }
    return oldest;
}

// Nothing is written over what held a key, or when one was heard, in Windows' own way of doing
// that, which a build that is made fast does not leave out: where the memory is given up right
// after, a plain writing of nothing over it is one that such a build may drop. What is wiped
// so is all zeros, which is what a place that holds nothing reads as.
template <typename What>
void wipe(What& what) noexcept
{
    static_assert(std::is_trivially_copyable_v<What>);
    SecureZeroMemory(&what, sizeof(What));
}

template <typename What>
void wipeRow(std::span<What> row) noexcept
{
    static_assert(std::is_trivially_copyable_v<What>);
    SecureZeroMemory(row.data(), row.size_bytes());
}

} // namespace

input::Pressed pressedOf(std::uint32_t virtualKey, std::uint32_t scanCode, std::uint32_t flags) noexcept
{
    // Set, and not shown by a test: that Windows tells the hook of the right Shift with the mark.
    // It is as I remember it; where it does not, this changes nothing.
    const bool isRightShift = scanCode == scanRightShift && virtualKey == nameOfRightShift;
    return {scanCode, (flags & markedFlag) != 0 && !isRightShift, virtualKey};
}

bool isTypedByAProgram(std::uint32_t flags) noexcept
{
    return (flags & typedByAProgramFlags) != 0;
}

std::optional<input::Pressed> fromRawInput(std::uint32_t makeCode, std::uint32_t flags,
                                           std::uint32_t virtualKey) noexcept
{
    if (virtualKey == nameOfNoKey || makeCode == 0 || makeCode >= KeysHeld::scanCodes) {
        return std::nullopt;
    }
    if ((flags & rawPauseFlag) != 0) {
        // Pause, whose first half has the number of Ctrl: the hook is told of it under Num
        // Lock's number, without the mark.
        if (virtualKey != nameOfPause) {
            return std::nullopt;
        }
        return input::Pressed{scanNumLock, false, virtualKey};
    }
    if (makeCode == scanNumLock && (flags & rawMarkedFlag) == 0) {
        // Num Lock, under whichever name (Windows calls it Pause under Ctrl): the hook is told
        // of it with the mark. (The Pause key itself came with a mark of its own, above.)
        return input::Pressed{makeCode, true, virtualKey};
    }
    return input::Pressed{makeCode, (flags & rawMarkedFlag) != 0, virtualKey};
}

std::uint64_t waitedMs(std::uint32_t nowTicks, std::uint32_t stampedTicks) noexcept
{
    // (Counted round the clock's end: both are of 32 bits. A stamp from after `now` shows as
    // more than half way round.)
    constexpr std::uint32_t halfWayRound = 0x8000'0000;
    const std::uint32_t waited = nowTicks - stampedTicks;
    return stampedTicks == 0 || waited >= halfWayRound ? 0 : waited;
}

bool isAlive(std::uint64_t nowMs, std::uint64_t saidAtMs) noexcept
{
    // (Said after `now` was read, by the other thread: alive, and no number wrapped around.)
    return nowMs <= saidAtMs || nowMs - saidAtMs < aliveForMs;
}

bool isLate(std::uint64_t nowMs, std::uint64_t heardAtMs) noexcept
{
    return nowMs > heardAtMs && nowMs - heardAtMs >= lateAfterMs;
}

bool meansNothingIsHeard(std::uint32_t sessionChange) noexcept
{
    switch (sessionChange) {
    case WTS_CONSOLE_CONNECT:
    case WTS_CONSOLE_DISCONNECT:
    case WTS_REMOTE_CONNECT:
    case WTS_REMOTE_DISCONNECT:
    case WTS_SESSION_LOCK:
    case WTS_SESSION_UNLOCK:
        return true;
    default:
        return false;
    }
}

bool meansAKeyboardWent(std::uint32_t deviceChange, std::optional<std::uint32_t> kindOfDevice) noexcept
{
    // (As Windows numbers them: this header is read where nothing of Windows is.)
    static_assert(deviceCame == std::uint32_t{GIDC_ARRIVAL} && deviceWent == std::uint32_t{GIDC_REMOVAL});
    static_assert(kindMouse == std::uint32_t{RIM_TYPEMOUSE} && kindKeyboard == std::uint32_t{RIM_TYPEKEYBOARD});
    return deviceChange == deviceWent && (!kindOfDevice.has_value() || *kindOfDevice == kindKeyboard);
}

std::uint64_t windowsWaitsFor(std::optional<std::uint32_t> saidMs) noexcept
{
    // (0 is what a registry that was emptied by hand says: Windows' own 300 ms then, as I take
    // it. And no more than those is ever believed: the shorter is the safe side.)
    if (!saidMs.has_value() || *saidMs == 0) {
        return windowsWaitsMs;
    }
    return std::min<std::uint64_t>(*saidMs, windowsWaitsMs);
}

std::uint64_t inTimeUnder(std::uint64_t windowsWaitsForMs) noexcept
{
    // (Where Windows was told to wait hardly at all, half of it: a margin that is no smaller
    // than what it is kept under would leave no key in time.)
    return windowsWaitsForMs > 2 * inTimeMarginMs ? windowsWaitsForMs - inTimeMarginMs : windowsWaitsForMs / 2;
}

std::uint64_t atOneWithin(std::uint64_t windowsWaitsForMs) noexcept
{
    return windowsWaitsForMs + atOneMarginMs;
}

void WindowsKeys::forget() noexcept
{
    wipe(m_down);
}

Verdict WindowsKeys::take(std::uint32_t virtualKey, bool down, bool canSwallow) noexcept
{
    bool& windowsHasIt = m_down[place(virtualKey)];
    if (!down) {
        // What Windows saw go down, it sees go up, whatever is swallowed just now.
        const bool swallow = canSwallow && !windowsHasIt;
        windowsHasIt = false;
        return {swallow, true};
    }
    if (!canSwallow) {
        windowsHasIt = true;
        return {false, false};
    }
    // (Held since before it could be swallowed: Windows goes on having it, and is shown no more
    // of its repeats.)
    return {true, !windowsHasIt};
}

bool KeysHeld::isAKey(const input::Pressed& key) noexcept
{
    return key.scanCode < scanCodes && key.virtualKey != nameOfNoKey;
}

std::size_t KeysHeld::placeOf(const input::Pressed& key) const noexcept
{
    const std::size_t own = place(key.scanCode, key.extended);
    if (m_keys[own].load(std::memory_order_relaxed) != 0) {
        return own;
    }
    // Not down under its own number: under another, where a key that is down is the same key by
    // its USB number. (A key that has none is no other key's.)
    if (const std::uint16_t usage = input::usageOf(key); usage != 0) {
        for (std::size_t other = 0; other < m_keys.size(); ++other) {
            const std::uint32_t slot = m_keys[other].load(std::memory_order_relaxed);
            if (slot != 0 && input::usageOf(keyAt(other, slot)) == usage) {
                return other;
            }
        }
    }
    // Num Lock, which may be told with its mark and without it, by two names that are two USB
    // numbers: while one of the two places is down, the other is that key's.
    // Set, and not shown by a test: that Windows ever tells of Num Lock so (under Ctrl, without
    // the mark, as "Pause"). I do not know that it does. Where it does not, this is never come to
    // but by the Pause key and Num Lock held together.
    if (key.scanCode == scanNumLock) {
        const std::size_t other = place(scanNumLock, !key.extended);
        if (m_keys[other].load(std::memory_order_relaxed) != 0) {
            return other;
        }
    }
    return own;
}

KeysHeld::Taken KeysHeld::take(const input::Pressed& key, bool down) noexcept
{
    if (!isAKey(key)) {
        if (key.scanCode == scanMadeUpCtrl && key.virtualKey == nameOfLeftCtrl) {
            m_madeUpCtrl.store(down, std::memory_order_relaxed);
        }
        // (What Windows cannot name lets go of nothing, at whichever key's place it comes: a
        // keyboard's made-up Shift going up is not the Shift that a finger holds.)
        return {key};
    }
    const std::size_t at = placeOf(key);
    std::atomic<std::uint32_t>& slot = m_keys[at];
    const std::uint32_t before = slot.load(std::memory_order_relaxed);
    if (before == 0) {
        // It was not down: it goes down as what it is told as now. (Going up, it is nothing here.)
        if (down) {
            slot.store(isDownBit | (key.virtualKey & nameBits), std::memory_order_relaxed);
            m_down.fetch_add(1, std::memory_order_relaxed);
        }
        return {key};
    }
    // It was down: said again, it stays what it went down as, and one that was kept from
    // Windows; let go, it is that key which is let go.
    Taken taken{keyAt(at, before), true, (before & isKeptBit) != 0};
    if (!down) {
        slot.store(0, std::memory_order_relaxed);
        taken.nothingIsDown = m_down.fetch_sub(1, std::memory_order_relaxed) == 1;
    }
    return taken;
}

void KeysHeld::keep(const input::Pressed& key) noexcept
{
    if (!isAKey(key)) {
        return;
    }
    std::atomic<std::uint32_t>& slot = m_keys[placeOf(key)];
    if (const std::uint32_t before = slot.load(std::memory_order_relaxed); before != 0) {
        slot.store(before | isKeptBit, std::memory_order_relaxed);
    }
}

std::optional<input::Pressed> KeysHeld::asItWentDown(const input::Pressed& key) const noexcept
{
    if (!isAKey(key)) {
        return std::nullopt;
    }
    const std::size_t at = placeOf(key);
    const std::uint32_t slot = m_keys[at].load(std::memory_order_relaxed);
    return slot != 0 ? std::optional<input::Pressed>(keyAt(at, slot)) : std::nullopt;
}

void KeysHeld::forget() noexcept
{
    for (std::atomic<std::uint32_t>& key : m_keys) {
        key.store(0, std::memory_order_relaxed);
    }
    m_down.store(0, std::memory_order_relaxed);
    m_madeUpCtrl.store(false, std::memory_order_relaxed);
}

bool KeysHeld::isDown(std::uint32_t scanCode, bool extended) const noexcept
{
    return scanCode < scanCodes && m_keys[place(scanCode, extended)].load(std::memory_order_relaxed) != 0;
}

bool KeysHeld::isKept(const input::Pressed& key) const noexcept
{
    return isAKey(key) && (m_keys[placeOf(key)].load(std::memory_order_relaxed) & isKeptBit) != 0;
}

std::optional<input::Pressed> KeysHeld::keptAt(std::size_t at) const noexcept
{
    if (at >= places) {
        return std::nullopt;
    }
    const std::uint32_t slot = m_keys[at].load(std::memory_order_relaxed);
    return (slot & isKeptBit) != 0 ? std::optional<input::Pressed>(keyAt(at, slot)) : std::nullopt;
}

std::optional<input::Pressed> KeysHeld::downAt(std::size_t at) const noexcept
{
    if (at >= places) {
        return std::nullopt;
    }
    const std::uint32_t slot = m_keys[at].load(std::memory_order_relaxed);
    return slot != 0 ? std::optional<input::Pressed>(keyAt(at, slot)) : std::nullopt;
}

bool KeysHeld::isTakenByWindows(const input::Pressed& key) const noexcept
{
    if (key.virtualKey == nameOfL) {
        return isDown(scanLeftWindows, true) || isDown(scanRightWindows, true);
    }
    // (Del, and the Del of the number block whatever Num Lock makes of it. Either Ctrl and
    // either Alt, and the Ctrl Windows makes up under AltGr.)
    if (key.scanCode == scanDel) {
        const bool ctrl =
            isDown(scanCtrl, false) || isDown(scanCtrl, true) || m_madeUpCtrl.load(std::memory_order_relaxed);
        return ctrl && (isDown(scanAlt, false) || isDown(scanAlt, true));
    }
    return false;
}

std::vector<input::Pressed> KeysHeld::all() const
{
    std::vector<input::Pressed> held;
    for (std::uint32_t scanCode = 0; scanCode < scanCodes; ++scanCode) {
        for (const bool extended : {false, true}) {
            const std::uint32_t name = m_keys[place(scanCode, extended)].load(std::memory_order_relaxed);
            if (name != 0) {
                held.push_back({scanCode, extended, name & nameBits});
            }
        }
    }
    return held;
}

std::vector<input::Pressed> KeysHeld::kept() const
{
    std::vector<input::Pressed> held;
    for (std::uint32_t scanCode = 0; scanCode < scanCodes; ++scanCode) {
        for (const bool extended : {false, true}) {
            const std::uint32_t name = m_keys[place(scanCode, extended)].load(std::memory_order_relaxed);
            if ((name & isKeptBit) != 0) {
                held.push_back({scanCode, extended, name & nameBits});
            }
        }
    }
    return held;
}

bool KeyRing::hasRoom() const noexcept
{
    return after(m_in.load(std::memory_order_relaxed)) != m_out.load(std::memory_order_acquire);
}

bool KeyRing::put(const Heard& heard) noexcept
{
    const std::size_t in = m_in.load(std::memory_order_relaxed);
    if (after(in) == m_out.load(std::memory_order_acquire)) {
        return false;
    }
    m_slots[in] = heard;
    m_in.store(after(in), std::memory_order_release);
    return true;
}

std::optional<Heard> KeyRing::take() noexcept
{
    const std::size_t out = m_out.load(std::memory_order_relaxed);
    if (out == m_in.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    // (Its place is this thread's until the next line but one: nothing is put there meanwhile.
    // And into what is handed back, with no second place for the key in here.)
    std::optional<Heard> heard(m_slots[out]);
    m_slots[out] = {};
    m_out.store(after(out), std::memory_order_release);
    return heard;
}

void KeyRing::clear() noexcept
{
    wipe(m_slots);
    m_in.store(0);
    m_out.store(0);
}

bool KeysWaiting::put(const Heard& heard, bool told) noexcept
{
    if (m_count == places) {
        return false;
    }
    m_slots[m_count] = {heard, told};
    ++m_count;
    return true;
}

void KeysWaiting::drop(std::size_t place) noexcept
{
    if (place >= m_count) {
        return;
    }
    for (std::size_t at = place; at + 1 < m_count; ++at) {
        m_slots[at] = m_slots[at + 1];
    }
    --m_count;
    m_slots[m_count] = {};
}

bool KeysWaiting::holdsBackTheUpOf(const input::Pressed& key, std::size_t before) const noexcept
{
    const bool isAModifier = input::isModifier(input::usageOf(key));
    for (std::size_t at = 0; at < before && at < m_count; ++at) {
        const Heard& waits = m_slots[at].heard;
        if (waits.down && (isAModifier || isSamePlace(waits.key, key))) {
            return true;
        }
    }
    return false;
}

bool KeysWaiting::holdsTheDownOf(const input::Pressed& key) const noexcept
{
    for (std::size_t at = 0; at < m_count; ++at) {
        const Heard& waits = m_slots[at].heard;
        if (waits.down && isSamePlace(waits.key, key)) {
            return true;
        }
    }
    return false;
}

void KeysWaiting::heardOnceMore(const input::Pressed& key) noexcept
{
    for (std::size_t at = 0; at < m_count; ++at) {
        One& one = m_slots[at];
        if (one.heard.down && !one.told && TwoAccounts::isSameKey(one.heard.key, key)) {
            ++one.saidSince;
        }
    }
}

void KeysWaiting::tell(const input::Pressed& key, std::size_t stillWait) noexcept
{
    // That account tells in the order the keys came in, and its word was for the oldest hearing
    // of the key going down that waited for one: `stillWait` such hearings were heard after
    // that one. So it was this press's own word where just so many were heard since it was;
    // and where the oldest was a saying of the key that is not in the row (the key was held
    // and said again, before it was let go and pressed anew), no press has its word yet.
    // (A saying's word was taken for the press that waited: the press went out before its
    // own, and so before a key that the other account had still to tell of between the two.)
    // Set, and not shown by a test: that Raw Input tells of one key's sayings in the order the
    // hook was called for them. It is as I remember it (both come from the one stream of the
    // keyboard's keys): fairly sure. Were it otherwise, a press would wait for a word that was
    // taken for another saying's, and the control would end as it does where a word is lost.
    for (std::size_t at = 0; at < m_count; ++at) {
        One& one = m_slots[at];
        if (one.heard.down && !one.told && one.saidSince == stillWait && TwoAccounts::isSameKey(one.heard.key, key)) {
            one.told = true;
            return;
        }
    }
}

void KeysWaiting::tellAll() noexcept
{
    for (std::size_t at = 0; at < m_count; ++at) {
        m_slots[at].told = true;
    }
}

void KeysWaiting::clear() noexcept
{
    wipe(m_slots);
    m_count = 0;
}

std::optional<std::size_t> TwoAccounts::numberOf(const input::Pressed& key) noexcept
{
    // As `input::usageOf` reads the keyboard's number and its mark, and only where that is a key
    // of the keyboard's page. One that is none there is not compared: not read with its mark
    // turned, by which Volume up would be B (it was read so, for the two accounts not marking
    // every key alike: the keys they are known to mark differently are made one before they
    // come here, `pressedOf` and `fromRawInput`). Nor is what Windows cannot name, or the left
    // Ctrl that it makes up under AltGr: Raw Input does not tell of that, and its hearing would
    // take the telling of a real left Ctrl.
    const std::uint16_t usage = input::usageOf(key);
    if (usage == 0 || usage >= keys) {
        return std::nullopt;
    }
    return std::size_t{usage};
}

bool TwoAccounts::isSameKey(const input::Pressed& one, const input::Pressed& other) noexcept
{
    const std::optional<std::size_t> which = numberOf(one);
    return which.has_value() && which == numberOf(other);
}

bool TwoAccounts::isLeftOut(std::size_t key, bool down, std::uint8_t side) noexcept
{
    std::uint8_t& sides = m_leftOut[key];
    if ((sides & side) == 0) {
        return false;
    }
    // (Its going up is the last of it that this account leaves out.)
    if (!down) {
        sides = static_cast<std::uint8_t>(sides & ~side);
    }
    return true;
}

void TwoAccounts::note(Waiting& waiting, std::uint64_t nowMs) noexcept
{
    for (std::uint64_t& one : waiting.at) {
        if (one == 0) {
            one = nowMs + 1;
            return;
        }
    }
    // No room to note when: one more, and the last of them came now. (More than can be counted
    // do not come: a key that is said again thirty times a second would take years.)
    if (waiting.more < std::numeric_limits<std::uint32_t>::max()) {
        ++waiting.more;
    }
    waiting.lastOfMore = nowMs + 1;
}

bool TwoAccounts::takeTheOldest(Waiting& waiting) noexcept
{
    std::uint64_t* oldest = oldestOf(waiting.at);
    if (oldest == nullptr) {
        return false;
    }
    // One of those that had no room moves up, where there are any. When it came is not known:
    // no sooner than what was noted, and no later than the last of them, which it is taken for.
    // (So it is never taken to have waited longer than it has.)
    *oldest = 0;
    if (waiting.more > 0) {
        --waiting.more;
        *oldest = waiting.lastOfMore;
    }
    // (When the last of them came is kept only while one of them waits.)
    if (waiting.more == 0) {
        waiting.lastOfMore = 0;
    }
    return true;
}

void TwoAccounts::dropTheOverdue(Waiting& waiting, std::uint64_t nowMs, std::uint64_t withinMs) noexcept
{
    for (std::uint64_t& one : waiting.at) {
        if (isOverdue(one, nowMs, withinMs)) {
            one = 0;
        }
    }
    // (Those that had no room came before the last of them did.)
    if (waiting.more > 0 && isOverdue(waiting.lastOfMore, nowMs, withinMs)) {
        waiting.more = 0;
    }
    for (std::uint64_t& one : waiting.at) {
        if (one == 0 && waiting.more > 0) {
            --waiting.more;
            one = waiting.lastOfMore;
        }
    }
    if (waiting.more == 0) {
        waiting.lastOfMore = 0;
    }
}

void TwoAccounts::forget() noexcept
{
    wipe(m_heard);
    wipe(m_told);
    wipe(m_leftOut);
    m_toldWaiting = 0;
}

void TwoAccounts::forgetWhatWasHeard() noexcept
{
    wipe(m_heard);
}

void TwoAccounts::leaveOutUntilUp(const input::Pressed& key) noexcept
{
    if (const std::optional<std::size_t> which = numberOf(key); which.has_value()) {
        m_leftOut[*which] = heardSide | toldSide;
    }
}

TwoAccounts::Compared TwoAccounts::heard(const input::Pressed& key, bool down, std::uint64_t nowMs) noexcept
{
    const std::optional<std::size_t> which = numberOf(key);
    if (!which.has_value() || isLeftOut(*which, down, heardSide)) {
        return Compared::No;
    }
    const std::size_t at = (*which * 2) + (down ? 1 : 0);
    // Raw Input told of it first, and that waits: the two agree. Otherwise this waits, to be
    // told of.
    if (takeTheOldest(m_told[at])) {
        --m_toldWaiting;
        return Compared::Told;
    }
    note(m_heard[at], nowMs);
    return Compared::Waits;
}

bool TwoAccounts::told(const input::Pressed& key, bool down, std::uint64_t nowMs, std::uint64_t withinMs) noexcept
{
    const std::optional<std::size_t> which = numberOf(key);
    if (!which.has_value() || isLeftOut(*which, down, toldSide)) {
        return false;
    }
    const std::size_t at = (*which * 2) + (down ? 1 : 0);
    // It was heard a moment ago, and that waits: the two agree. (What was heard longer ago
    // than the moment is not this key's hearing, and waits no more.)
    dropTheOverdue(m_heard[at], nowMs, withinMs);
    if (takeTheOldest(m_heard[at])) {
        return true;
    }
    // Otherwise this waits, to be heard.
    note(m_told[at], nowMs);
    ++m_toldWaiting;
    return false;
}

void TwoAccounts::pressedAnew(const input::Pressed& key, std::uint64_t nowMs, std::uint64_t withinMs) noexcept
{
    // What was heard of this key going down longer ago than the moment was a saying of it from
    // before it was let go, which the other account has not told of: no telling takes it any
    // more, and left standing it would be counted before the press that is heard now.
    if (const std::optional<std::size_t> which = numberOf(key); which.has_value()) {
        dropTheOverdue(m_heard[(*which * 2) + 1], nowMs, withinMs);
    }
}

std::size_t TwoAccounts::waitToBeTold(const input::Pressed& key, bool down) const noexcept
{
    const std::optional<std::size_t> which = numberOf(key);
    if (!which.has_value()) {
        return 0;
    }
    const Waiting& waiting = m_heard[(*which * 2) + (down ? 1 : 0)];
    return static_cast<std::size_t>(std::ranges::count_if(waiting.at, [](std::uint64_t one) { return one != 0; })) +
           waiting.more;
}

void TwoAccounts::dropWhatWasHeardBefore(std::uint64_t nowMs, std::uint64_t withinMs) noexcept
{
    for (Waiting& waiting : m_heard) {
        dropTheOverdue(waiting, nowMs, withinMs);
    }
}

bool TwoAccounts::notesNoHearing() const noexcept
{
    return std::ranges::none_of(m_heard, [](const Waiting& waiting) {
        return waiting.more > 0 || waiting.lastOfMore != 0 ||
               std::ranges::any_of(waiting.at, [](std::uint64_t one) { return one != 0; });
    });
}

bool TwoAccounts::isToldAndNotHeard(const input::Pressed& key, bool down) const noexcept
{
    const std::optional<std::size_t> which = numberOf(key);
    if (!which.has_value()) {
        return false;
    }
    // (Where more wait than there is room to note the time of, those places are all taken.)
    const Waiting& waiting = m_told[(*which * 2) + (down ? 1 : 0)];
    return std::ranges::any_of(waiting.at, [](std::uint64_t one) { return one != 0; });
}

bool TwoAccounts::isBehind(std::uint64_t nowMs, std::uint64_t withinMs) const noexcept
{
    return m_toldWaiting > 0 && std::ranges::any_of(m_told, [nowMs, withinMs](const Waiting& waiting) {
               return std::ranges::any_of(
                   waiting.at, [nowMs, withinMs](std::uint64_t one) { return isOverdue(one, nowMs, withinMs); });
           });
}

bool TwoAccounts::isBehindOfWhatWasToldAfter(std::uint64_t afterMs, std::uint64_t nowMs,
                                             std::uint64_t withinMs) const noexcept
{
    // (A time that was noted is that time plus one.)
    return m_toldWaiting > 0 && std::ranges::any_of(m_told, [afterMs, nowMs, withinMs](const Waiting& waiting) {
               return std::ranges::any_of(waiting.at, [afterMs, nowMs, withinMs](std::uint64_t one) {
                   return one > afterMs + 1 && isOverdue(one, nowMs, withinMs);
               });
           });
}

bool TwoAccounts::wasHeardAndNotTold(std::uint64_t nowMs, std::uint64_t atLeastMs,
                                     std::uint64_t atMostMs) const noexcept
{
    return std::ranges::any_of(m_heard, [nowMs, atLeastMs, atMostMs](const Waiting& waiting) {
        return std::ranges::any_of(waiting.at, [nowMs, atLeastMs, atMostMs](std::uint64_t one) {
            return isOverdue(one, nowMs, atLeastMs) && !isOverdue(one, nowMs, atMostMs);
        });
    });
}

void Procedure::begin(std::uint64_t nowMs, std::uint64_t windowsWaitsForMs, std::uint16_t leaveKey) noexcept
{
    m_leaveKey = supportedLeaveKey(leaveKey);
    m_leave = LeaveWatch(m_leaveKey);
    m_windows.forget();
    m_held.forget();
    m_ring.clear();
    m_waiting.clear();
    m_accounts.forget();
    m_rawHasTold = false;
    m_beganToTellUntil = 0;
    m_way.store(0);
    m_notTold.store(false);
    m_keyboardWent.store(false);
    m_beganToTell.store(false);
    m_goingIsTold.store(false);
    m_typedWasSeen.store(false);
    m_typedIsToBeSaid.store(false);
    m_inTimeMs = inTimeUnder(windowsWaitsForMs);
    m_atOneMs = atOneWithin(windowsWaitsForMs);
    m_sinceMs.store(nowMs);
    m_state.store(isIdle);
    m_takenAnew.store(false);
    m_fellBehind.store(false);
    m_anotherDesktop.store(false);
    m_keysOfWindows.store(false);
    m_notHanded.store(false);
    m_rawTells.store(false);
    m_wasFoundSilent.store(false);
    m_saidAtMs.store(nowMs);
    m_lookedAtMs.store(nowMs);
}

void Procedure::forget() noexcept
{
    m_state.store(isClosedForGood);
    m_windows.forget();
    m_held.forget();
    m_ring.clear();
    m_waiting.clear();
    m_accounts.forget();
    m_way.store(0);
}

bool Procedure::setSwallowing(bool swallowing, std::uint64_t nowMs) noexcept
{
    if (!swallowing) {
        stopSwallowing(nowMs);
        return false;
    }
    // (Said before it is so: a leave-key that goes up in this very moment is known as one that
    // was held. Only a hook that is not closed swallows again.)
    m_takenAnew.store(true);
    int idle = isIdle;
    if (m_state.compare_exchange_strong(idle, isSwallowing)) {
        // Swallowed anew: what a program types from here on is said once more.
        m_typedWasSeen.store(false);
    }
    return swallows();
}

std::optional<Heard> Procedure::next() noexcept
{
    std::optional<Heard> heard = m_ring.take();
    if (heard.has_value()) {
        offItsWay();
    }
    return heard;
}

std::uint64_t Procedure::onItsWay() noexcept
{
    // One more, and its number: one beyond the last one's.
    constexpr std::uint64_t oneMoreAndTheNext = (std::uint64_t{1} << wayCountBits) + 1;
    return (m_way.fetch_add(oneMoreAndTheNext) >> wayCountBits) + 1;
}

void Procedure::offItsWay() noexcept
{
    // One fewer; and with the last of them the numbers begin anew, so that they say nothing of
    // how much was typed. (Either thread, and the other may put one on its way meanwhile: tried
    // again then, with what is so by then. Nothing is waited for.)
    std::uint64_t was = m_way.load();
    while ((was & wayCountMask) != 0) {
        const std::uint64_t fewer = (was & wayCountMask) == 1 ? 0 : was - 1;
        if (m_way.compare_exchange_weak(was, fewer)) {
            return;
        }
    }
}

void Procedure::stopSwallowing(std::uint64_t nowMs) noexcept
{
    // Since when, before it is so: whoever finds that nothing is swallowed finds the time with it.
    if (m_state.load() == isSwallowing) {
        m_sinceMs.store(nowMs);
        int swallowing = isSwallowing;
        m_state.compare_exchange_strong(swallowing, isIdle);
    }
}

void Procedure::close(std::uint64_t nowMs) noexcept
{
    if (m_state.load() == isSwallowing) {
        m_sinceMs.store(nowMs);
    }
    m_state.store(isClosedForGood);
}

bool Procedure::isOver(std::uint64_t nowMs) const noexcept
{
    // (Whether, before since when: the time is written first.)
    if (m_state.load() == isSwallowing) {
        return false;
    }
    const std::uint64_t since = m_sinceMs.load();
    return nowMs >= since && nowMs - since >= staysForMs;
}

void Procedure::rawInputTellsNoMore() noexcept
{
    // Nothing is compared from here on, and nothing waits for a word that goes to another. And
    // Windows' word for a keyboard that goes is that other's with it: it comes here no more.
    m_rawTells.store(false);
    m_goingIsTold.store(false);
    m_rawHasTold = false;
    m_beganToTellUntil = 0;
    m_accounts.forget();
    m_waiting.tellAll();
}

void Procedure::intoTheRing(const Heard& heard) noexcept
{
    if (!m_ring.put(heard)) {
        // There was no room to say so: a key may be down on a machine, or down here unknown.
        m_fellBehind.store(true);
        offItsWay();
    }
}

bool Procedure::hand(Heard heard, TwoAccounts::Compared compared, bool saidAgain, std::uint64_t nowMs) noexcept
{
    // What waited and may go by now goes first: this very hearing may have made the two
    // accounts at one, and the order the keys came in is kept wherever nothing has to wait.
    const bool woke = passOn(nowMs);
    const bool isOnlySaidAgain = heard.down && saidAgain;
    if (isOnlySaidAgain && !isClosed() && m_waiting.holdsTheDownOf(heard.key)) {
        // Its own going down still waits: this adds nothing to that, and takes no room. (When
        // that one goes, the key is down, and that is all a key that is said again says.)
        return woke;
    }
    heard.number = onItsWay();
    if (isClosed()) {
        // Keys are heard no more (of what waited, only what went up was told just now). This
        // is told behind it: whoever listens lets go of everything all the same.
        intoTheRing(heard);
        return true;
    }
    if (!heard.down) {
        // A key that goes up waits for nobody's word, and for nothing that has to do with
        // whose it was: handed on at once, past whatever waits, unless it would pass what it
        // may not (KeysWaiting): its own going down, or the key would stay down on the
        // machine; and, where it is a modifier, any key going down that waits, which was
        // pressed under it and would arrive bare. (Where it waits so and the same key goes
        // down once more, that is a press like any other, behind it in the row.) And it says
        // that no key is down only where nothing waits that was heard before it.
        if (!m_waiting.holdsBackTheUpOf(heard.key, m_waiting.count())) {
            heard.nothingIsDown = heard.nothingIsDown && m_waiting.isEmpty();
            intoTheRing(heard);
            return true;
        }
        if (!m_waiting.put(heard, true)) {
            offItsWay();
            m_fellBehind.store(true);
            return true;
        }
        return woke;
    }
    // A key that is down and is only said again waits for nobody's word either, and for nothing
    // that waits: whoever listens has the key as down already, or keeps it back, and a key that
    // is said again changes neither. (So a held key that one account says many times before the
    // other's first word fills no row, and its "up" waits behind none of those sayings.)
    if (isOnlySaidAgain) {
        intoTheRing(heard);
        return true;
    }
    // A key that goes down waits for the other account's word where that account tells of this
    // keyboard, and the key is one for a machine: what has no USB number goes to none, and
    // nobody else tells of it. And it goes behind whatever waits, and not while a key that was
    // told of waits to be heard.
    const bool waitsForItsWord =
        compared == TwoAccounts::Compared::Waits && m_rawTells.load() && input::usageOf(heard.key) != 0;
    if (m_waiting.isEmpty() && !waitsForItsWord && !m_accounts.waitsToBeHeard()) {
        intoTheRing(heard);
        return true;
    }
    if (!m_waiting.put(heard, !waitsForItsWord)) {
        offItsWay();
        m_fellBehind.store(true);
        return true;
    }
    return woke;
}

bool Procedure::mayGo(const KeysWaiting::One& one, std::uint64_t nowMs) noexcept
{
    // Not while a key that was told of waits to be heard: the two are not at one.
    if (m_accounts.waitsToBeHeard()) {
        return false;
    }
    if (one.told) {
        return true;
    }
    if (nowMs <= one.heard.atMs || nowMs - one.heard.atMs <= m_atOneMs) {
        return false;
    }
    // It has waited the moment in vain for the other account's word.
    if (m_rawHasTold) {
        // That account tells of this keyboard, and not of this key: the two differ. This hook
        // swallows nothing any more, and whoever listens is told, where keys were swallowed.
        // (Why is marked before it is closed: whoever finds it closed finds the reason with it.)
        if (swallows()) {
            m_notTold.store(true);
        }
        close(nowMs);
        return false;
    }
    // It does not tell of this keyboard. Nothing waits for its word from here on, until it
    // tells of a key. And no hearing is noted from here on, since there is no account to hold
    // one against: what was noted in this first moment goes with it.
    m_rawTells.store(false);
    m_waiting.tellAll();
    m_accounts.forgetWhatWasHeard();
    return true;
}

bool Procedure::passOn(std::uint64_t nowMs) noexcept
{
    bool woke = false;
    bool closed = isClosed();
    // From the one that has waited longest. A key that goes down stays behind every such key
    // before it that stays; one that goes up goes as soon as nothing before it holds it back.
    bool aPressStays = false;
    std::size_t at = 0;
    while (at < m_waiting.count()) {
        const KeysWaiting::One one = m_waiting.oneAt(at);
        if (closed) {
            // Keys are heard no more: nothing that waited is typed. That a key went up is told,
            // so that nobody goes on waiting for it; a press is dropped, and that its key is
            // down, kept from Windows, is the table's to say (`whatIsKept`).
            if (one.heard.down) {
                offItsWay();
            } else {
                intoTheRing(one.heard);
                woke = true;
            }
            m_waiting.drop(at);
            continue;
        }
        bool goes = false;
        if (one.heard.down) {
            goes = !aPressStays && mayGo(one, nowMs);
            if (isClosed()) {
                // The two accounts differ, found just now: the row is gone through anew, as
                // the row of a hook that hears no more.
                closed = true;
                woke = true;
                at = 0;
                continue;
            }
        } else {
            goes = !m_waiting.holdsBackTheUpOf(one.heard.key, at);
        }
        if (goes && !m_ring.hasRoom()) {
            // No room to hand it on just now: it stays, and so does everything behind it.
            break;
        }
        if (!goes) {
            aPressStays = aPressStays || one.heard.down;
            ++at;
            continue;
        }
        Heard heard = one.heard;
        // (That no key is down is said only where nothing else waits that was heard before.)
        heard.nothingIsDown = heard.nothingIsDown && m_waiting.count() == 1;
        m_waiting.drop(at);
        intoTheRing(heard);
        woke = true;
    }
    return woke;
}

bool Procedure::takeWhatWasToldToGoUp(std::uint64_t nowMs) noexcept
{
    bool handedOn = false;
    for (std::size_t place = 0; place < KeysHeld::places; ++place) {
        const std::optional<input::Pressed> key = m_held.keptAt(place);
        if (!key.has_value() || !m_accounts.isToldAndNotHeard(*key, false)) {
            continue;
        }
        // By the word of the account that told of it, it is up. Left as down, it would be
        // waited for at the next taking, for an "up" that has come and gone.
        const KeysHeld::Taken taken = m_held.take(*key, false);
        if (!m_waiting.put({taken.key, false, nowMs, false, taken.nothingIsDown, onItsWay()}, true)) {
            offItsWay();
            m_fellBehind.store(true);
        }
        handedOn = true;
    }
    return handedOn;
}

Verdict Procedure::key(std::uint32_t virtualKey, std::uint32_t scanCode, std::uint32_t flags, bool down,
                       std::uint64_t nowMs, std::uint64_t waitedForMs) noexcept
{
    // What a program typed is passed on, and is nobody's here: not told, not taken for a
    // person's, and not noted for Windows' other account, which does not tell of it. Only that
    // such a key came while keys are swallowed is marked, once: it reaches no machine, and
    // whoever types it is to be told. Nothing of the key is kept.
    if (isTypedByAProgram(flags)) {
        if (swallows() && !m_typedWasSeen.exchange(true)) {
            m_typedIsToBeSaid.store(true);
            return {false, true};
        }
        return {};
    }
    // (Not wiped, and said so: what stands of the key in this call's own places as it returns,
    // here and in what it calls. The next key that is heard is written over it. What is wiped
    // is every place a key is kept in from one call to the next: the ring, the row, the tables
    // and the telling's row and its one copy.)
    const input::Pressed pressed = pressedOf(virtualKey, scanCode, flags);
    // Heard: noted for Windows' other account of the keyboard, for as long as the two are
    // compared. (Which key, which way, when; wiped when that account has told of it, or when it
    // has not for longer than that has a use: `look`.) Only where that account is told to this
    // hook: where there is none to hold a hearing against, nothing of it is noted.
    const bool isNoted = !isClosed() && m_rawTells.load();
    if (isNoted && down && !m_held.asItWentDown(pressed).has_value()) {
        // A press, of a key that was up: what still stands of its sayings from before it was
        // let go, a moment old and never told of, is none of this press's.
        m_accounts.pressedAnew(pressed, nowMs, m_atOneMs);
    }
    const TwoAccounts::Compared compared = isNoted ? m_accounts.heard(pressed, down, nowMs) : TwoAccounts::Compared::No;
    if (down && compared == TwoAccounts::Compared::Waits) {
        // One more saying of the key waits for that account's word: a press of it that waits
        // in the row has one more behind it, and that account's word for this saying will not
        // be taken for the press's own.
        m_waiting.heardOnceMore(pressed);
    }
    // The hook has stayed its time: nothing is done with a key any more, whatever has become of
    // the thread that is to take it out.
    if (isOver(nowMs)) {
        return {};
    }
    // Another window is in front, as Windows says on this very thread as the key goes down
    // (pressed, or held and said again): the key is that window's, and so is every key from here
    // on. Nothing more is swallowed, whatever the interface thread asked for: it hears of the
    // other window when Qt tells it, which waits while it is at other work. As the leave-key,
    // and before this key could be swallowed or told; what was kept from Windows is told as it
    // goes up, as ever. (Asked only while keys are swallowed: there is nothing to end otherwise.)
    if (down && m_inFront != nullptr && swallows() && m_inFront() != m_window) {
        stopSwallowing(nowMs);
    }
    // (Asked before the leave-key may end the swallowing with this very key.)
    const bool wasSwallowing = swallows();
    if (m_takenAnew.exchange(false)) {
        m_leave.takenAnew();
    }
    // The physical key. One that is down is the key it went down as, said again or let go,
    // whatever Windows calls it by now: Ctrl, Shift or Num Lock, pressed or let go meanwhile,
    // give the same key another name. What went down under the one name is told, is Windows',
    // and goes up on a machine under that one, and never under the other.
    // And the one rule for a key that goes up: it is let go here by the "up" as it is heard,
    // whichever keyboard that came from and whichever else may hold the key. A hook is not
    // told which keyboard a key came from, and keyboards are not told apart. With two in use
    // at once, a modifier that is held on the one is let go by the same modifier's "up" on
    // the other, and the next key from the first arrives bare: that cost is known, and it is
    // the side to fail on. (To keep the key down for the other keyboard's sake was tried, by
    // Raw Input's word for which keyboard told: it left a modifier down on the machine where
    // a keyboard dropped out without notice.)
    const KeysHeld::Taken taken = m_held.take(pressed, down);
    const input::Pressed& key = taken.key;
    const bool saidAgain = down && taken.wasDown;
    // The way out, and by nothing but this: whatever else is so, the leave-key gives the
    // keyboard back. (A key that is only said again is no key going down.)
    if (m_leave.take(input::usageOf(key), down, saidAgain)) {
        stopSwallowing(nowMs);
    }
    if (down && m_held.isTakenByWindows(pressed)) {
        // Windows locks the PC or shows its own screen on this key, whatever is done with it
        // here, and the keys that are let go there are heard by nobody. So before the key
        // could go to a machine: nothing more is swallowed, and whoever listens lets go of
        // everything. The key is Windows', as every key that is passed on. (Said as told: the
        // interface thread is woken, and finds nothing but the word for why.)
        keysOfWindows(nowMs);
        static_cast<void>(m_windows.take(key.virtualKey, true, false));
        return {false, true};
    }
    // An interface thread that has said nothing for a second, while keys were to be swallowed:
    // the keyboard is this PC's again, for good as far as this hook goes, and that is marked
    // for the interface thread to find when it is back. Of this key, and of every key from
    // here on, nothing is put on its way to that thread; only that one which had been kept
    // from Windows went up. (Which keys are down stays in the tables, as ever.)
    const bool isSilent = wasSwallowing && !isAlive(nowMs, m_saidAtMs.load());
    if (isSilent) {
        m_wasFoundSilent.store(true);
        fallBehind(nowMs);
    }
    // A key that comes after Windows has stopped waiting for this procedure, or so close to it
    // that it may have, has gone to Windows already, whatever is answered here: it is not the
    // machine's, and since a key was typed into this PC that was meant for a machine, nothing
    // more is swallowed.
    const bool cameInTime = waitedForMs < m_inTimeMs;
    if (wasSwallowing && !cameInTime) {
        fallBehind(nowMs);
    }
    const bool canSwallow = swallows() && cameInTime && hasRoomToTell();
    const Verdict made = m_windows.take(key.virtualKey, down, canSwallow);
    const bool isTheMachines = down ? made.swallow && made.tell : made.swallow;
    if (down && isTheMachines) {
        m_held.keep(key);
    }
    // Told while swallowing is asked for: every key that goes down and up, a press with whether
    // it is the machine's. One that went to Windows is told as Windows': a modifier that is down
    // and that whoever listens does not know of sends the next key to a machine bare. Told while
    // it is not asked for: only that a key which this hook kept from Windows went up, which
    // whoever listened waits for. By the keyboard's number for the key, and not by Windows' name:
    // of a key of this PC's that goes up under a name Windows did not have as down, nothing.
    // As the key it went down as. And handed on now, or once it may go (`hand`).
    const bool tell = (wasSwallowing && !isSilent) || (!down && taken.wasKept);
    bool wake = false;
    if (tell) {
        wake = hand({key, down, nowMs, isTheMachines, taken.nothingIsDown}, compared, saidAgain, nowMs);
    } else if (!m_waiting.isEmpty()) {
        wake = passOn(nowMs);
    }
    return {made.swallow, wake};
}

bool Procedure::beginComparingAt(const input::Pressed& key, bool down, std::uint64_t nowMs) noexcept
{
    // No hearing was noted until here. So what is down at this moment was heard before the
    // comparing, and is not compared until it has gone up: as the keys that are down when a hook
    // goes in.
    for (std::size_t place = 0; place < KeysHeld::places; ++place) {
        if (const std::optional<input::Pressed> held = m_held.downAt(place); held.has_value()) {
            m_accounts.leaveOutUntilUp(*held);
        }
    }
    // A key that was heard before this, and let go already, is another matter: the other
    // account's word for it comes within the gate's moment from here, finds no hearing, and
    // cannot be told from the word for a key that the hook was not called for. Nothing was
    // noted to tell them apart by. So it is held against the hook as any telling that waits in
    // vain, and the control ends: with a word of its own (`look`), since what is known then is
    // not that a key was not handed over. Until when a telling is one of that first moment:
    m_beganToTellUntil = nowMs + m_atOneMs + 1;
    // (Such a telling was forgiven for a while, and nothing ended by it. But so was the telling
    // of a key that went down in that moment with the hook not called for it: a Ctrl that was
    // left out, and the "c" after it went to the machine bare, with nothing said. What that
    // costs: on a PC that begins to tell while a machine is controlled, with a key in flight
    // at that very moment, the control ends once. Narrowed to "what was heard before" it
    // cannot be: of that, on such a PC, nothing is noted.)
    // And the going up of a key that is not down here was heard already, where the hook was
    // called for it at all: this telling of it waits for no hearing.
    return down || m_held.asItWentDown(key).has_value();
}

bool Procedure::rawKey(const input::Pressed& key, bool down, std::uint64_t nowMs) noexcept
{
    if (isOver(nowMs)) {
        return false;
    }
    const int state = m_state.load();
    if (state == isSwallowing && down && m_held.isTakenByWindows(key)) {
        // What Windows takes the keyboard with, known by this account where the hook was not
        // called for the key.
        keysOfWindows(nowMs);
        return true;
    }
    bool wake = false;
    if (state != isClosedForGood) {
        // It tells of this keyboard: a key that is heard to go down waits for its word from
        // here on, and one that it then does not tell of is one the two differ about. (Where it
        // was taken not to tell, the comparing begins with this key.)
        const bool isCompared = m_rawTells.exchange(true) || beginComparingAt(key, down, nowMs);
        m_rawHasTold = true;
        if (isCompared && m_accounts.told(key, down, nowMs, m_atOneMs) && down) {
            // The hook heard a saying of this key first, and this is that account's word for
            // it. Where it was a press that waits in the row, that press need not wait any
            // longer: whether it was, the row knows by how many sayings of the key still wait
            // behind the one this word was for.
            m_waiting.tell(key, m_accounts.waitToBeTold(key, true));
        }
        wake = passOn(nowMs);
    }
    if (state == isSwallowing || down || !m_held.isKept(key)) {
        return wake;
    }
    // Nothing is swallowed, and a key that was kept from Windows goes up: told by this account
    // where the hook did not hear it first (it is out, or was not called). As the key it went
    // down as; and never past its own going down, where that waits.
    const KeysHeld::Taken taken = m_held.take(key, false);
    return hand({taken.key, false, nowMs, false, taken.nothingIsDown}, TwoAccounts::Compared::No, false, nowMs) || wake;
}

Procedure::Looked Procedure::look(std::uint64_t nowMs) noexcept
{
    Looked looked;
    // It looks: said for the interface thread, which takes a stamp that stands still for a
    // thread that hangs.
    m_lookedAtMs.store(nowMs);
    if (m_accounts.isBehind(nowMs, m_atOneMs)) {
        // Raw Input told of a key, and the procedure was not called for it: Windows has taken
        // the hook out, or left a key out, or something on this PC kept the key from this hook.
        // This hook swallows nothing any more. Whoever listens is told where keys were
        // swallowed; where not, the next taking finds it closed. And where a key was heard
        // about then that Raw Input has not told of, the two told differently: of one key, it
        // may be. (About then: no sooner than half a moment ago, which a telling that is only
        // on its way does not take, and no longer ago than the key that was told of.)
        const bool heardAnother = m_accounts.wasHeardAndNotTold(nowMs, m_atOneMs / 2, 2 * m_atOneMs);
        // Or, where the comparing began at a telling a moment ago and nothing but tellings of
        // that first moment waited in vain: such a one may be the word for a key that was
        // heard before any hearing was noted. Not known which: the control ends all the same,
        // and with the word for that. (One that was told of later is held against the hook by
        // the word it always had.)
        const bool inItsFirstMoment =
            m_beganToTellUntil != 0 && !m_accounts.isBehindOfWhatWasToldAfter(m_beganToTellUntil - 1, nowMs, m_atOneMs);
        looked.wake = takeWhatWasToldToGoUp(nowMs);
        m_accounts.forget();
        // (Why is marked before it is closed: whoever finds it closed finds the reason with it.)
        if (swallows()) {
            m_beganToTell.store(inItsFirstMoment);
            m_notHanded.store(!inItsFirstMoment);
            m_notTold.store(!inItsFirstMoment && heardAnother);
            looked.wake = true;
        }
        close(nowMs);
    }
    // (No telling is one of that first moment once all of those have waited theirs.)
    if (m_beganToTellUntil != 0 && nowMs >= m_beganToTellUntil + m_atOneMs) {
        m_beganToTellUntil = 0;
    }
    // And nothing of a hearing is kept for longer than it has a use: a telling takes none that
    // is older than the moment, and what is asked of one above looks no further back than two.
    // (So of a key that went up, heard and never told of: the key was let go by that hearing,
    // nothing ends by it, and what was noted of it goes here.)
    m_accounts.dropWhatWasHeardBefore(nowMs, 2 * m_atOneMs);
    // What waited for the two accounts to be at one: handed on where they are, or where the
    // other account's word has not come within the moment; and never where this hook is closed.
    looked.wake = passOn(nowMs) || looked.wake;
    // An interface thread that hangs for longer than a hung program is worth waiting for: the
    // hook goes, whatever was asked of it. (Only where keys were to be swallowed: while they
    // are not, the interface thread says nothing, and is not waited for.)
    const std::uint64_t saidAt = m_saidAtMs.load();
    if ((swallows() || m_wasFoundSilent.load()) && nowMs > saidAt && nowMs - saidAt >= hungAfterMs) {
        fallBehind(nowMs);
        static_cast<void>(passOn(nowMs));
        looked.wake = true;
        looked.next = Next::End;
        return looked;
    }
    if (isOver(nowMs)) {
        // Its time is up: ended, unless swallowing was asked for anew in this very moment.
        int idle = isIdle;
        if (m_state.compare_exchange_strong(idle, isClosedForGood) || idle == isClosedForGood) {
            looked.next = Next::End;
        }
    }
    return looked;
}

void Procedure::fallBehind(std::uint64_t nowMs) noexcept
{
    // (Why is marked before it is closed, here and wherever it is closed for a reason: whoever
    // finds it closed finds the reason with it.)
    m_fellBehind.store(true);
    close(nowMs);
}

void Procedure::anotherDesktop(std::uint64_t nowMs) noexcept
{
    // (Said once, however often Windows says it: on the way there, and on the way back. And
    // not after the keys that brought it were seen, and said as what they are.)
    if (!isClosed()) {
        m_anotherDesktop.store(true);
        close(nowMs);
    }
}

bool Procedure::aKeyboardWent(std::uint64_t nowMs) noexcept
{
    // Only while keys are swallowed: no machine has a key down by this hook otherwise. And
    // whichever keyboard it was, with a key held or without: nothing is known of that here, and
    // to end where no key was held costs a click, where not to end leaves a key down on a
    // machine. (Why is marked before it is closed: whoever finds it closed finds the reason
    // with it.)
    if (!swallows()) {
        return false;
    }
    m_keyboardWent.store(true);
    close(nowMs);
    return true;
}

void Procedure::keysOfWindows(std::uint64_t nowMs) noexcept
{
    if (!isClosed()) {
        m_keysOfWindows.store(true);
        close(nowMs);
    }
}

std::vector<input::Pressed> heldKeys(const std::function<std::uint32_t(std::uint32_t scanCode, bool extended)>& nameOf,
                                     const std::function<bool(std::uint32_t virtualKey)>& isDown)
{
    struct Known {
        input::Pressed key;
        std::uint16_t usage = 0;
    };
    // Every physical key that has a USB number, with Windows' name for it.
    std::vector<Known> known;
    for (std::uint32_t scanCode = 1; scanCode < KeysHeld::scanCodes; ++scanCode) {
        for (const bool extended : {false, true}) {
            const std::uint32_t name = nameOf(scanCode, extended);
            if (name == 0 || name >= WindowsKeys::names) {
                continue;
            }
            const input::Pressed key{scanCode, extended, name};
            if (const std::uint16_t usage = input::usageOf(key); usage != 0) {
                known.push_back({key, usage});
            }
        }
    }
    // Under AltGr Windows has a left Ctrl of its own making as down: not said.
    const bool rightAltIsDown = std::ranges::any_of(
        known, [&](const Known& one) { return one.usage == usageRightAlt && isDown(one.key.virtualKey); });
    std::vector<input::Pressed> held;
    for (const Known& one : known) {
        const bool isItsOwn = std::ranges::none_of(known, [&](const Known& other) {
            return other.key.virtualKey == one.key.virtualKey && other.usage != one.usage;
        });
        const bool mayBeMadeUp = rightAltIsDown && one.usage == input::usageLeftCtrl;
        if (isItsOwn && !mayBeMadeUp && isDown(one.key.virtualKey)) {
            held.push_back(one.key);
        }
    }
    return held;
}

std::vector<input::Pressed>
keysThatMayBeDown(const std::function<std::uint32_t(std::uint32_t scanCode, bool extended)>& nameOf,
                  const std::function<bool(std::uint32_t virtualKey)>& isDown)
{
    std::vector<input::Pressed> may;
    for (std::uint32_t scanCode = 1; scanCode < KeysHeld::scanCodes; ++scanCode) {
        for (const bool extended : {false, true}) {
            const std::uint32_t name = nameOf(scanCode, extended);
            if (name == 0 || name >= WindowsKeys::names) {
                continue;
            }
            const input::Pressed key{scanCode, extended, name};
            if (input::usageOf(key) != 0 && isDown(name)) {
                may.push_back(key);
            }
        }
    }
    return may;
}

std::vector<input::Pressed> together(std::vector<input::Pressed> heard, const std::vector<input::Pressed>& ofWindows)
{
    const auto own = static_cast<std::ptrdiff_t>(heard.size());
    for (const input::Pressed& key : ofWindows) {
        const bool isKnown = std::any_of(heard.cbegin(), heard.cbegin() + own,
                                         [&](const input::Pressed& one) { return isSamePlace(one, key); });
        if (!isKnown) {
            heard.push_back(key);
        }
    }
    return heard;
}

void whatToTell(Procedure& procedure, std::uint64_t nowMs, const std::function<void(const Told&)>& say)
{
    // (That this runs says that the interface thread is there: one that was found silent is
    // back, and takes what was marked for it.)
    procedure.stillThere(nowMs);
    using Lost = KeySource::Lost;
    const auto told = [](Told::What what, const Heard& heard = {}, Lost why = Lost::NotHandedKeys) {
        return Told{what, heard, why};
    };
    // Why nothing is heard any more, where that is so: said first, so that whoever listens lets
    // go of everything before anything else. Nothing that waits is typed after that. (Each is
    // asked, whichever is said: none is left standing for the next time.) True: there was a
    // reason.
    const auto sayWhy = [&procedure, &say, &told, nowMs] {
        const bool keysOfWindows = procedure.takeKeysOfWindows();
        const bool another = procedure.takeAnotherDesktop();
        const bool aKeyboardWent = procedure.takeAKeyboardWent();
        const bool beganToTell = procedure.takeBeganToTell();
        const bool notHanded = procedure.takeNotHanded();
        const bool notTold = procedure.takeNotTold();
        const bool fellBehind = procedure.takeFellBehind();
        if (keysOfWindows) {
            say(told(Told::What::LostHearing, {}, Lost::KeysOfWindows));
        } else if (another) {
            say(told(Told::What::LostHearing, {}, Lost::AnotherDesktop));
        } else if (aKeyboardWent) {
            say(told(Told::What::LostHearing, {}, Lost::AKeyboardWent));
        } else if (beganToTell) {
            say(told(Told::What::LostHearing, {}, Lost::BeganToTell));
        } else if (notHanded) {
            say(told(Told::What::LostHearing, {}, notTold ? Lost::ToldDifferently : Lost::NotHandedKeys));
        } else if (notTold) {
            say(told(Told::What::LostHearing, {}, Lost::NotToldOfKeys));
        } else if (fellBehind) {
            procedure.close(nowMs);
            say(told(Told::What::FellBehind));
        }
        return keysOfWindows || another || aKeyboardWent || beganToTell || notHanded || notTold || fellBehind;
    };
    bool over = sayWhy();
    // That a program typed a key, which went to no machine: only while keys go to one.
    if (procedure.takeTypedByAProgram() && !over) {
        say(told(Told::What::TypedByAProgram));
    }
    // (Each key stands in one place of this one's own, which is wiped when the key was handed
    // on: a key is handed on and forgotten, and no copy of it is left standing here. What the
    // compiler itself keeps of one, in a place that was given back or in a register, is not
    // this one's to wipe.)
    while (std::optional<Heard> next = procedure.next()) {
        if (!over && procedure.isClosed()) {
            // The hook was closed since why was asked for, and this key was handed on in the
            // very call that closed it, or after that: it is not typed, and neither is what is
            // behind it. (The gate dropped a key before this one: a "c" whose Ctrl was dropped
            // would reach the machine bare.) Why is asked for once more, and said before what
            // follows: it was marked before the hook was closed.
            over = true;
            static_cast<void>(sayWhy());
        }
        if (!over && isLate(nowMs, next->atMs)) {
            // What waited too long is not typed late, and nothing behind it is typed at all.
            over = true;
            procedure.close(nowMs);
            say(told(Told::What::FellBehind));
        }
        // After that, only that a key went up: whoever listened waits for what was held.
        if (!over || !next->down) {
            Told key{Told::What::Key, *next, Lost::NotHandedKeys};
            say(key);
            wipe(key);
            if (!next->down && next->nothingIsDown) {
                say(told(Told::What::NothingIsDown));
            }
        }
        wipe(*next);
    }
}

std::vector<Told> whatToTell(Procedure& procedure, std::uint64_t nowMs)
{
    std::vector<Told> all;
    whatToTell(procedure, nowMs, [&all](const Told& one) { all.push_back(one); });
    return all;
}

void Telling::put(const Told& told)
{
    if (m_end == m_places.size()) {
        // No place is left behind what waits: a larger row, with what waits at its beginning.
        // The one that is outgrown is wiped before it is given up: every byte of it, and so
        // that the wiping is not left out for the memory being given back right after (it was
        // a writing of empty places over it, which a build that is made fast may drop).
        const std::size_t waiting = m_end - m_next;
        std::vector<Told> larger(std::max<std::size_t>(2 * waiting, 16));
        std::ranges::copy(std::span<const Told>(m_places).subspan(m_next, waiting), larger.begin());
        m_places.swap(larger);
        std::vector<Told>& outgrown = larger;
        wipeRow(std::span<Told>(outgrown));
        if (m_lookAtTheOutgrown && !outgrown.empty()) {
            m_lookAtTheOutgrown(outgrown);
        }
        m_next = 0;
        m_end = waiting;
    }
    m_places[m_end] = told;
    ++m_end;
}

void Telling::takeTheNext(Told& into) noexcept
{
    // A key is handed on and forgotten: its place is wiped as it is taken out. And with the
    // last of what waited the row begins at its first place again. (Into a place of whoever
    // says it, which wipes that when it was said: handed back from here, a copy of the key
    // would be left standing in a place of this one's that was given back.)
    into = m_places[m_next];
    wipe(m_places[m_next]);
    ++m_next;
    if (m_next == m_end) {
        m_next = 0;
        m_end = 0;
    }
}

void Telling::forget() noexcept
{
    // (In the way that is not left out where the row itself is given up right after, as it is
    // when the key source ends.)
    wipeRow(std::span<Told>(m_places));
    m_next = 0;
    m_end = 0;
}

void Telling::all(Procedure& procedure, std::uint64_t nowMs, KeySource& source, const std::function<bool()>& goesOn)
{
    // What the procedure has by now goes behind what waits here: that is what a pass which is
    // still being said has left, where this is asked from inside its telling. (Into the row as
    // it is made, and nowhere else.)
    whatToTell(procedure, nowMs, [this](const Told& more) { put(more); });
    while (m_next != m_end) {
        if (!goesOn()) {
            forget();
            return;
        }
        // (Out of the row before it is said: whoever is told may ask for the rest at once. Into
        // a place of this pass's own, which is wiped once the thing was said.)
        Told told;
        takeTheNext(told);
        switch (told.what) {
        case Told::What::Key:
            emit source.keyChanged(told.heard.key, told.heard.down, told.heard.swallowed, told.heard.number);
            break;
        case Told::What::FellBehind:
            emit source.fellBehind();
            break;
        case Told::What::LostHearing:
            emit source.lostHearing(told.why);
            break;
        case Told::What::NothingIsDown:
            emit source.nothingIsDown();
            break;
        case Told::What::TypedByAProgram:
            emit source.typedByAProgram();
            break;
        }
        wipe(told);
    }
}

} // namespace hook

namespace {

// What one hook works on, from when it is put in until its thread has gone: the procedure, the
// two words between the threads, and what the thread says of itself. Its thread and the
// interface thread each hold it, and it ends with whichever lets go last: a thread that hangs,
// and is left to end by itself, touches nothing that is gone.
struct Run {
    explicit Run(std::uintptr_t window)
        : window(window), wake(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
          stop(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {}
    ~Run()
    {
        if (wake != nullptr) {
            CloseHandle(wake);
        }
        if (stop != nullptr) {
            CloseHandle(stop);
        }
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    Run(Run&&) = delete;
    Run& operator=(Run&&) = delete;

    // The window this hook is for, by the handle Windows knows it by: what is in front is asked
    // against it, with every key that goes down (`Procedure::isFor`). With `number` below, which
    // hook of which window this is.
    const std::uintptr_t window;
    hook::Procedure procedure;
    // The hook's thread to the interface thread: there is something for it. And the other way:
    // the hook is to go.
    HANDLE wake;
    HANDLE stop;
    // What the hook's thread says: the hook is in, or Windows has refused it (`listens` is which);
    // and that it has gone: from then on it hears nothing of Windows.
    std::binary_semaphore started{0};
    std::binary_semaphore ended{0};
    std::atomic<bool> listens{false};
    std::atomic<bool> gone{false};
    // Which of the two forgets what the procedure holds, where the thread did not end in the
    // time it was given and was left to end by itself.
    hook::LastToGo lastToGo;
    // The keys that the window's hook before this one had as down as it ended: Windows may not
    // know of them. Said before the thread begins, and read by it, which wipes them then.
    std::vector<input::Pressed> wasDown;
    // The how-manieth hook of the program this is.
    std::uint64_t number = 0;
};

// How many hooks the program's windows have put in so far. A hook that is not the last of them
// may have been kept from keys that the later one swallowed: what it has as down is not what is
// down, and it does not go on at a taking. (On the interface thread, where hooks are begun.)
std::uint64_t hooksBegun = 0;

// What the procedure works on, and what it wakes the interface thread with: the hook thread's
// own, so that the procedure, which Windows calls with nothing of ours, finds them without a lock.
thread_local hook::Procedure* procedureOfThisThread = nullptr;
thread_local HANDLE wakeOfThisThread = nullptr;

// Windows tells one window of a program of the raw keyboard: the one that asked last. Whose it
// is at this moment, among the hooks of this program's windows: the hook of the window that took
// keyboard and mouse last. An older hook, in only for what it kept from Windows to be let go,
// sees here that the account is another's now. (The lock is for asking and for giving it up, on
// a hook's thread outside its procedure. The asking comes once the hook is in, so a key may wait
// for it: no longer than another hook's asking or giving up takes.)
std::mutex rawKeyboardIsAskedFor;
std::atomic<const Run*> rawKeyboardIsToldTo{nullptr};

// Windows' name for a physical key, and whether Windows has the key of a name as down.
std::uint32_t nameOfKey(std::uint32_t scanCode, bool extended)
{
    // (Windows takes a key with the mark as its number with E0 before it.)
    return MapVirtualKeyW(extended ? (0xE000U | scanCode) : scanCode, MAPVK_VSC_TO_VK_EX);
}

bool isDownForWindows(std::uint32_t virtualKey)
{
    return GetAsyncKeyState(static_cast<int>(virtualKey)) < 0;
}

// Which window Windows has in front: asked by the procedure, on the hook's thread, as a key goes
// down while keys are swallowed. (No window, in a moment in which none has the keyboard: that is
// another window than the hook's, and nothing is swallowed.)
// Set, and not shown by a test: that the window a QWindow's handle names is the one Windows says
// is in front while that QWindow is the active one. Its popups and boxes are drawn inside it (no
// window of their own), and the relative mouse holds the pointer by the same comparison
// (windowsPointerSystem). What the procedure does with the answer is tested.
std::uintptr_t windowInFront() noexcept
{
    return reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
}

// For how long Windows waits for a hook on this PC, where that was set: a number in the
// registry, under the person's own desktop settings. Read, and never written. Nothing: not set.
// Set, and not shown by a test: that this is where Windows keeps it, in milliseconds. It is as
// Windows' own description of such hooks has it, as I remember it; I am fairly sure. (What is
// made of the number, and that no longer wait than Windows' own is believed, is
// `hook::windowsWaitsFor`, which is tested.)
std::optional<std::uint32_t> hooksTimeoutSaid()
{
    DWORD said = 0;
    DWORD size = sizeof(DWORD);
    const LSTATUS read = RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"LowLevelHooksTimeout",
                                      RRF_RT_REG_DWORD, nullptr, &said, &size);
    return read == ERROR_SUCCESS ? std::optional<std::uint32_t>(said) : std::nullopt;
}

// The hook's procedure. Windows calls it for every key before any program is told of the key,
// and waits for it: no memory is taken in it, no lock is waited for, nothing is logged.
LRESULT CALLBACK heard(int code, WPARAM what, LPARAM more)
{
    hook::Procedure* procedure = procedureOfThisThread;
    if (code == HC_ACTION && procedure != nullptr) {
        const auto* told = reinterpret_cast<const KBDLLHOOKSTRUCT*>(more);
        const bool down = what == WM_KEYDOWN || what == WM_SYSKEYDOWN;
        const std::uint64_t now = GetTickCount64();
        // Set, and not shown by a test: that the key's stamp is of the clock GetTickCount reads.
        const std::uint64_t waited = hook::waitedMs(static_cast<std::uint32_t>(now), told->time);
        const hook::Verdict verdict = procedure->key(told->vkCode, told->scanCode, told->flags, down, now, waited);
        if (verdict.tell) {
            SetEvent(wakeOfThisThread);
        }
        if (verdict.swallow) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, what, more);
}

// Windows shows another desktop, or this one again.
void CALLBACK desktopSwitched(HWINEVENTHOOK /*hook*/, DWORD /*event*/, HWND /*window*/, LONG /*object*/, LONG /*child*/,
                              DWORD /*thread*/, DWORD /*time*/)
{
    if (procedureOfThisThread != nullptr) {
        procedureOfThisThread->anotherDesktop(GetTickCount64());
        SetEvent(wakeOfThisThread);
    }
}

// What Windows tells a window of the session it is in: locked, unlocked, taken to another screen
// or back. The desktop's own event may come only on the way back; this is Windows' word for it.
LRESULT CALLBACK sessionChanged(HWND window, UINT message, WPARAM what, LPARAM more)
{
    if (message == WM_WTSSESSION_CHANGE && procedureOfThisThread != nullptr &&
        hook::meansNothingIsHeard(static_cast<std::uint32_t>(what))) {
        procedureOfThisThread->anotherDesktop(GetTickCount64());
        SetEvent(wakeOfThisThread);
    }
    return DefWindowProcW(window, message, what, more);
}

// What kind of device Windows says one is that it names in its word for a device that comes or
// goes (RID_DEVICE_INFO::dwType); nothing: Windows does not say, as it may not of one that is
// gone. Asked and forgotten: nothing of a device is kept.
// Set, and not shown by a test: that Windows can be asked this at all of a device that went. As
// I remember it, it cannot (fairly sure, not certain): then every such device is taken for a
// keyboard, which is the kind the word was asked for.
std::optional<std::uint32_t> kindOfDevice(HANDLE device)
{
    RID_DEVICE_INFO said{};
    said.cbSize = sizeof(RID_DEVICE_INFO);
    UINT size = sizeof(RID_DEVICE_INFO);
    const UINT read = GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &said, &size);
    return read != static_cast<UINT>(-1) && read >= sizeof(RID_DEVICE_INFO) ? std::optional<std::uint32_t>(said.dwType)
                                                                            : std::nullopt;
}

// What Raw Input tells of the keyboard: Windows' other account of it, beside the hook's. And
// Windows' word for a keyboard that comes or goes, which is asked for with that account.
LRESULT CALLBACK rawKeyboardTold(HWND window, UINT message, WPARAM what, LPARAM more)
{
    hook::Procedure* procedure = procedureOfThisThread;
    if (message == WM_INPUT_DEVICE_CHANGE && procedure != nullptr && what == GIDC_REMOVAL) {
        // A device of the kind that was asked for, a keyboard, was taken away from this PC:
        // unplugged, taken to another computer by a desk switch that cuts it off, its link
        // lost where Windows takes the device away with the link. A key that was held on it is
        // never heard to go up. Which keyboard it was is not looked at and not kept (its
        // handle is asked what kind of device it is, and forgotten): any keyboard that goes
        // ends the swallowing (`Procedure::aKeyboardWent`). One that comes ends nothing.
        // Set, and not shown by a test, each from memory: that Windows posts this word to the
        // window that asked for the raw keyboard with RIDEV_DEVNOTIFY, with GIDC_REMOVAL and
        // the device's handle (sure: it is how Windows describes it); that it does so for
        // keyboards only where only the keyboard was asked for (fairly sure); that a
        // message-only window is told as it is told of keys (fairly sure). What Windows counts
        // as a keyboard is more than what has letters on it: a mouse with keys of its own, a
        // headset, a dock or a receiver for both keyboard and mouse may be one, and then goes
        // as one. Not known: what comes over a remote desktop; and whether a keyboard on
        // Bluetooth that sleeps is taken away (one behind a receiver that stays plugged in is
        // not: nothing is said of it, fairly sure).
        if (hook::meansAKeyboardWent(static_cast<std::uint32_t>(what), kindOfDevice(reinterpret_cast<HANDLE>(more))) &&
            procedure->aKeyboardWent(GetTickCount64())) {
            SetEvent(wakeOfThisThread);
        }
    }
    if (message == WM_INPUT && procedure != nullptr) {
        // Read into a place of this procedure's own: no memory is taken. A key of the keyboard
        // fits; what would not is no key, and is not read.
        RAWINPUT told{};
        UINT size = sizeof(RAWINPUT);
        const UINT read =
            GetRawInputData(reinterpret_cast<HRAWINPUT>(more), RID_INPUT, &told, &size, sizeof(RAWINPUTHEADER));
        // Only a key of a real keyboard: one that comes with a device. Which device, is not
        // looked at and not kept: keyboards are not told apart. Set, and not shown by a test:
        // that what a program types comes with no device, and what Windows tells of a remote
        // keyboard.
        if (read != static_cast<UINT>(-1) && read >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD) &&
            told.header.dwType == RIM_TYPEKEYBOARD && told.header.hDevice != nullptr) {
            const RAWKEYBOARD& key = told.data.keyboard;
            std::optional<input::Pressed> pressed = hook::fromRawInput(key.MakeCode, key.Flags, key.VKey);
            if (pressed.has_value() &&
                procedure->rawKey(*pressed, (key.Flags & hook::rawUpFlag) == 0, GetTickCount64())) {
                SetEvent(wakeOfThisThread);
            }
            if (pressed.has_value()) {
                hook::wipe(*pressed);
            }
        }
        // The key was handed on, or was none: what was read of it is not left standing in this
        // thread's own place. (Wiped so that it is not left out for the place being given up
        // right after. What Windows itself holds of it, it clears up below.)
        hook::wipe(told);
    }
    // (Windows clears up what it told of in here.)
    return DefWindowProcW(window, message, what, more);
}

// A window of a kind, made once for the program: a second time Windows says that the kind is
// there, which is as well.
HWND windowOfKind(const wchar_t* kind, WNDPROC procedure, HWND parent)
{
    HINSTANCE program = GetModuleHandleW(nullptr);
    WNDCLASSEXW made{};
    made.cbSize = sizeof(WNDCLASSEXW);
    made.lpfnWndProc = procedure;
    made.hInstance = program;
    made.lpszClassName = kind;
    RegisterClassExW(&made);
    return CreateWindowExW(0, kind, L"", WS_OVERLAPPED, 0, 0, 0, 0, parent, nullptr, program, nullptr);
}

// A real window that is never shown, for Windows to tell of the session: the hook thread's own.
// Nothing where Windows makes none, or does not take it for that: then the desktop's event and
// the keys Windows takes the keyboard with are what there is.
HWND sessionWindow()
{
    HWND window = windowOfKind(L"QkvmDesktopSessionWatch", &sessionChanged, nullptr);
    if (window != nullptr && WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION) == FALSE) {
        DestroyWindow(window);
        window = nullptr;
    }
    return window;
}

// What Windows gave of the raw keyboard when it was asked for: nothing; its keys; or its keys
// and its word for a keyboard that comes or goes.
enum class RawKeyboard { Refused, Keys, KeysAndGoing };

// Raw Input is asked to tell this window of the keyboard: a message-only window, which is never
// shown and gets no broadcast, the hook thread's own. Nothing where there is no such window, or
// Windows will not tell it: then that account says nothing, and a hook that Windows took out is
// not found.
// Set, and not shown by a test: all of it. That Raw Input tells such a window of every key while
// another program is in front, whatever the hook swallows; and that asking for it changes nothing
// for the program's own windows, which go on being told of keys as before.
RawKeyboard askForRawKeyboard(const Run& run, HWND window)
{
    if (window == nullptr) {
        return RawKeyboard::Refused;
    }
    constexpr USHORT genericDesktop = 0x01;
    constexpr USHORT keyboard = 0x06;
    // Of every key, whichever program is in front. And of keyboards that come and go
    // (RIDEV_DEVNOTIFY): a key that was held on one as it was taken away is never heard to go
    // up, and the swallowing ends on that word. Which keyboard a key came from is still kept
    // nowhere. Where Windows refuses the two together, the keys alone are asked for: a
    // keyboard that goes is then not noticed, and whoever asks is told so
    // (`isToldOfKeyboardsGoing`).
    // Set, and not shown by a test: that Windows takes the two together (fairly sure: both are
    // of Windows since Vista, and commonly asked for so), and that it then says once, for
    // each keyboard that is there already, that it "came" (fairly sure; nothing is made of it).
    RAWINPUTDEVICE asked{genericDesktop, keyboard, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
    const std::scoped_lock lock(rawKeyboardIsAskedFor);
    RawKeyboard given = RawKeyboard::KeysAndGoing;
    if (RegisterRawInputDevices(&asked, 1, sizeof(RAWINPUTDEVICE)) == FALSE) {
        asked.dwFlags = RIDEV_INPUTSINK;
        if (RegisterRawInputDevices(&asked, 1, sizeof(RAWINPUTDEVICE)) == FALSE) {
            return RawKeyboard::Refused;
        }
        given = RawKeyboard::Keys;
    }
    rawKeyboardIsToldTo.store(&run);
    return given;
}

void endRawKeyboard(const Run& run, HWND window)
{
    if (window == nullptr) {
        return;
    }
    {
        // (Only where it is still this hook's: another window's hook may have asked since.)
        const std::scoped_lock lock(rawKeyboardIsAskedFor);
        if (rawKeyboardIsToldTo.load() == &run) {
            constexpr USHORT genericDesktop = 0x01;
            constexpr USHORT keyboard = 0x06;
            RAWINPUTDEVICE asked{genericDesktop, keyboard, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&asked, 1, sizeof(RAWINPUTDEVICE));
            rawKeyboardIsToldTo.store(nullptr);
        }
    }
    DestroyWindow(window);
}

// The hook's thread, from begin() until the hook's time is over. It works on its run and on
// nothing else: whoever began it may be gone by the time it ends.
// Set, and not shown by a test: all of it, since no test puts a hook in. That the hook goes in
// after everything that takes time; that Raw Input is asked for only once the hook is in, with
// what is down asked right after the two; and that this thread takes its hook out by itself
// when the procedure says so (when it says so is `Procedure::look`, which is tested).
void listen(const std::shared_ptr<Run>& run)
{
    hook::Procedure& procedure = run->procedure;
    // Every key of the PC waits for this thread while the hook is in: it goes before the
    // program's other work.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    procedureOfThisThread = &procedure;
    wakeOfThisThread = run->wake;
    // Everything that takes time first, while no key waits for this thread: the two windows,
    // the session (which speaks to another process) and the desktop's event.
    HWND rawKeyboard = windowOfKind(L"QkvmDesktopRawKeyboard", &rawKeyboardTold, HWND_MESSAGE);
    HWND session = sessionWindow();
    HWINEVENTHOOK desktop = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr,
                                            &desktopSwitched, 0, 0, WINEVENT_OUTOFCONTEXT);
    for (const input::Pressed& key : run->wasDown) {
        procedure.isDownAlready(key);
    }
    // (Read, and of no more use: which keys those were is not left standing in the run.)
    hook::wipeRow(std::span<input::Pressed>(run->wasDown));
    run->wasDown.clear();
    // The hook, and only then Windows' other account. (Asked for the other way round, a key
    // between the two was told of by the one and could never be heard by the other: a held key
    // that is said again, Enter still down from pressing "Control" with the keyboard.) And right
    // after the two what is down at this moment, as Windows has it: such a key is not compared
    // until it has gone up. (No key is handed to the procedure between these: Windows hands this
    // thread its keys only while it looks at its messages.)
    // Set, and not closed, since no order of the two askings closes it: what a key does in the
    // instant between them (microseconds: nothing is done between the two but the asking). In
    // this order such a key is heard, when this thread first looks, and never told of. One
    // that goes down then waits its moment for a word that does not come: the PC is taken not
    // to tell, until that account's first word, or, where that word has come by then, the
    // control ends as over a key that Windows did not report, which is not true of it. One
    // that was held and goes up then (Windows still has it as down while the hook's word for
    // it is waited for) is left out of the comparing by the hearing no longer, and by the
    // telling for ever: its next press is heard, its word is taken for nothing, and the
    // control ends there with that same sentence. In the other order the same key is told of
    // and never heard, and ends the control as one that was not handed over. What would close
    // it is not built: a key that only one of the two leaves out any more, a moment after the
    // other had its "up", is left out by neither.
    HHOOK listener = SetWindowsHookExW(WH_KEYBOARD_LL, &heard, GetModuleHandleW(nullptr), 0);
    bool isTold = false;
    if (listener != nullptr) {
        const RawKeyboard given = askForRawKeyboard(*run, rawKeyboard);
        isTold = given != RawKeyboard::Refused;
        for (int virtualKey = 1; virtualKey < static_cast<int>(hook::WindowsKeys::names); ++virtualKey) {
            if (GetAsyncKeyState(virtualKey) < 0) {
                procedure.windowsHas(static_cast<std::uint32_t>(virtualKey));
            }
        }
        if (GetAsyncKeyState(leaveKeyOfWindows(procedure.leaveKey())) < 0) {
            procedure.leaveKeyIsHeld();
        }
        for (const input::Pressed& key : hook::keysThatMayBeDown(&nameOfKey, &isDownForWindows)) {
            procedure.isDownAlready(key);
        }
        if (isTold) {
            procedure.rawInputTells();
        }
        if (given == RawKeyboard::KeysAndGoing) {
            procedure.keyboardsGoingIsTold();
        }
    }
    run->listens.store(listener != nullptr);
    run->started.release();
    if (listener != nullptr) {
        // Windows calls the procedure from inside the looking at messages. Told to stop through
        // an event of its own, which cannot fail to arrive as a message can. And woken every so
        // often to look: whether Raw Input told of a key that the procedure did not hear,
        // whether a key waits for the two to be at one, and whether the hook is to go. Always
        // after everything that had come was looked at, so that a look which comes late (the
        // PC was busy, the program was stopped) finds what was only late already matched. Each
        // look says that this thread still looks.
        MSG message{};
        DWORD woke = MsgWaitForMultipleObjects(1, &run->stop, FALSE, hook::lookEveryMs, QS_ALLINPUT);
        while (woke == WAIT_OBJECT_0 + 1 || woke == WAIT_TIMEOUT) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                DispatchMessageW(&message);
            }
            // Another window's hook has asked for the raw keyboard since: it is told to that one.
            if (isTold && rawKeyboardIsToldTo.load() != run.get()) {
                isTold = false;
                procedure.rawInputTellsNoMore();
            }
            const hook::Procedure::Looked looked = procedure.look(GetTickCount64());
            if (looked.wake) {
                SetEvent(run->wake);
            }
            if (looked.next == hook::Procedure::Next::End) {
                // The hook has stayed its time, or the interface thread hangs: out, whatever
                // has become of that thread.
                break;
            }
            woke = MsgWaitForMultipleObjects(1, &run->stop, FALSE, hook::lookEveryMs, QS_ALLINPUT);
        }
        UnhookWindowsHookEx(listener);
        if (woke == WAIT_FAILED) {
            // Windows would not wait, and nobody told this to stop: whoever listens is told
            // that keys are not passed on any more.
            procedure.fallBehind(GetTickCount64());
            SetEvent(run->wake);
        }
    }
    // Whatever was asked of it meanwhile: this hook is out, and swallows nothing.
    procedure.close(GetTickCount64());
    if (session != nullptr) {
        WTSUnRegisterSessionNotification(session);
        DestroyWindow(session);
    }
    endRawKeyboard(*run, rawKeyboard);
    if (desktop != nullptr) {
        UnhookWinEvent(desktop);
    }
    procedureOfThisThread = nullptr;
    wakeOfThisThread = nullptr;
    run->gone.store(true);
    // The last thing it does with what it worked on. Where whoever began this has waited for
    // it in vain and gone on, nobody else forgets what the procedure holds: which keys are
    // down, and when each was heard. (It was let go as it stood, until the run itself went.)
    if (run->lastToGo.theThreadHasEnded()) {
        procedure.forget();
    }
    run->ended.release();
}

class WindowsKeyboard final : public KeySource {
public:
    explicit WindowsKeyboard(QWindow& window) : m_window(&window), m_hooksTimeout(hooksTimeoutSaid())
    {
        connect(&m_woken, &QWinEventNotifier::activated, this, &WindowsKeyboard::handOn);
    }

    ~WindowsKeyboard() override { stop(); }

    WindowsKeyboard(const WindowsKeyboard&) = delete;
    WindowsKeyboard& operator=(const WindowsKeyboard&) = delete;
    WindowsKeyboard(WindowsKeyboard&&) = delete;
    WindowsKeyboard& operator=(WindowsKeyboard&&) = delete;

    bool begin(std::uint16_t leaveKey = input::usageRightCtrl) override
    {
        // The window the keys are for, by the handle Windows knows it by now. (Its native window
        // is there: keys are taken for a window that is in front. Where the window is gone, or
        // Windows does not know it, nothing listens.)
        const std::uintptr_t window = m_window != nullptr ? static_cast<std::uintptr_t>(m_window->winId()) : 0;
        // It goes on where its hook is in for that very window, has not been closed, is the last
        // hook the program put in and still the one Raw Input tells of the keyboard, and its
        // thread still looks. Otherwise anew, with a hook that knows nothing of before but which
        // keys the one before it had as down.
        if (m_run != nullptr && m_run->window == window && m_run->procedure.leaveKey() == supportedLeaveKey(leaveKey) &&
            !m_run->procedure.isClosed() && !m_run->gone.load() && m_run->number == hooksBegun &&
            rawKeyboardIsToldTo.load() == m_run.get() && looksStill(*m_run)) {
            return true;
        }
        std::vector<input::Pressed> wasDown;
        if (m_run != nullptr) {
            wasDown = m_run->procedure.whatIsDown();
        }
        stop();
        if (window == 0) {
            return false;
        }
        const std::shared_ptr<Run> run = std::make_shared<Run>(window);
        if (run->wake == nullptr || run->stop == nullptr) {
            return false;
        }
        run->wasDown = std::move(wasDown);
        run->number = ++hooksBegun;
        run->procedure.begin(GetTickCount64(), hook::windowsWaitsFor(m_hooksTimeout), leaveKey);
        // (Before its thread begins, which reads it from then on.)
        run->procedure.isFor(window, &windowInFront);
        std::thread thread;
        try {
            thread = std::thread([run] { listen(run); });
        } catch (const std::system_error&) {
            return false;
        }
        // Until the hook is in, or Windows has refused it: a moment, and never for ever. A
        // thread that does not say either in time is left to end by itself, and nothing is taken.
        const bool started = run->started.try_acquire_for(std::chrono::milliseconds(hook::startsWithinMs));
        if (!started || !run->listens.load()) {
            letGo(*run, thread);
            return false;
        }
        m_run = run;
        m_thread = std::move(thread);
        m_looks = hook::LookWatch();
        m_woken.setHandle(m_run->wake);
        m_woken.setEnabled(true);
        return true;
    }

    // The ring is emptied unread, and that loses nothing that matters. What was swallowed and
    // not handed on yet was never down on a machine; what was down there is let go by whoever
    // ends the control; and which keys are still kept from Windows is not the ring's to say but
    // the table's, which whoever stopped the swallowing asked then (`whatIsKept`).
    void end() override { stop(); }

    std::vector<input::Pressed> whatIsDown() const override
    {
        // What the procedure heard go down since the hook is in, kept from Windows or not; and
        // of Windows what it has as down, which is all there is to know of the time before.
        return hook::together(m_run != nullptr ? m_run->procedure.whatIsDown() : std::vector<input::Pressed>(),
                              hook::heldKeys(&nameOfKey, &isDownForWindows));
    }

    std::vector<input::Pressed> whatIsKept() const override
    {
        return m_run != nullptr ? m_run->procedure.whatIsKept() : std::vector<input::Pressed>();
    }

    bool isWatched() const override
    {
        return m_run != nullptr && !m_run->gone.load() && rawKeyboardIsToldTo.load() == m_run.get() &&
               m_run->procedure.isWatched();
    }

    bool isToldOfKeyboardsGoing() const override
    {
        return m_run != nullptr && !m_run->gone.load() && rawKeyboardIsToldTo.load() == m_run.get() &&
               m_run->procedure.isToldOfKeyboardsGoing();
    }

    bool swallows() const override { return m_run != nullptr && m_run->procedure.swallows(); }

    void setSwallowing(bool swallowing) override
    {
        if (m_run != nullptr) {
            m_run->procedure.setSwallowing(swallowing, GetTickCount64());
        }
    }

    void stillThere() override
    {
        // A word, and nothing else: this PC is sent nothing.
        if (m_run == nullptr) {
            return;
        }
        hook::Procedure& procedure = m_run->procedure;
        const std::uint64_t now = GetTickCount64();
        procedure.stillThere(now);
        // And a look at whether the hook's thread still looks, while keys are swallowed. Where
        // it hangs, Windows has freed this PC's keyboard by itself; but what is down on the
        // machine stays down, and nothing here would say so.
        // Set, and not shown by a test: that it is asked here. (When a thread no longer looks
        // is `LookWatch`, which is tested.)
        if (procedure.swallows() && m_looks.take(now, procedure.lookedAtMs())) {
            procedure.close(now);
            emit lostHearing(Lost::NotLooking);
        }
    }

    OnTheirWay onTheirWay() const override
    {
        if (m_run == nullptr) {
            return {};
        }
        const hook::Procedure::OnTheirWay way = m_run->procedure.onTheirWay();
        return {way.last, static_cast<int>(way.count)};
    }

    void tellNow() override { handOn(); }

private:
    // Its thread has looked within the time that a thread which looks is given.
    static bool looksStill(const Run& run)
    {
        const std::uint64_t now = GetTickCount64();
        const std::uint64_t looked = run.procedure.lookedAtMs();
        return now <= looked || now - looked < hook::looksWithinMs;
    }

    // The hook is to go, and swallows nothing from this moment on. Its thread is waited for, a
    // moment and never for ever: one that does not end in time is left to end by itself, with
    // its run, which it holds and which nothing else touches from here on.
    // Set, and not shown by a test: that a thread which hangs is left behind so, and the program
    // goes on. (That a closed procedure swallows nothing, whoever asks, is tested; and that of
    // the two exactly one forgets what such a thread worked on, `hook::LastToGo`.)
    static void letGo(Run& run, std::thread& thread)
    {
        run.procedure.close(GetTickCount64());
        SetEvent(run.stop);
        if (run.ended.try_acquire_for(std::chrono::milliseconds(hook::endsWithinMs))) {
            thread.join();
            // (Its thread is gone: nothing of what was typed stays.)
            run.procedure.forget();
        } else {
            // Left to end by itself: it forgets what it worked on when it has ended, as the
            // last thing it does with it. Unless it ended in this very moment, and is past
            // that: then it touches nothing of it any more, and it is forgotten here.
            thread.detach();
            if (run.lastToGo.isWaitedForNoLonger()) {
                run.procedure.forget();
            }
        }
    }

    void stop()
    {
        if (m_run == nullptr) {
            return;
        }
        // (No longer woken by it, before it may be gone. And what was still to be told of it
        // went with it.)
        m_woken.setEnabled(false);
        letGo(*m_run, m_thread);
        m_run.reset();
        m_telling.forget();
    }

    // The interface thread: what the procedure has for whoever listens is told, in its order.
    // (What is told, in which order, and that what waits is told to whoever asks for it from
    // inside the telling of a key, is `hook::whatToTell` and `hook::Telling`, which are tested.
    // Set, and not shown by a test: that it is told from here, each time the hook's thread
    // wakes this one.)
    void handOn()
    {
        const std::shared_ptr<Run> run = m_run;
        if (run == nullptr) {
            return;
        }
        // (Whoever is told may end the listening in the middle of this: looked at each time.
        // What is not told then went with the hook.)
        m_telling.all(run->procedure, GetTickCount64(), *this, [this, &run] { return m_run == run; });
    }

    // The window the keys are for.
    QPointer<QWindow> m_window;
    // What Windows' registry says of how long it waits for a hook, read once; nothing: not said.
    std::optional<std::uint32_t> m_hooksTimeout;
    // What the hook that is in works on; nothing: none is.
    std::shared_ptr<Run> m_run;
    std::thread m_thread;
    hook::LookWatch m_looks;
    // What is being told to whoever listens, and has not all been said yet.
    hook::Telling m_telling;
    QWinEventNotifier m_woken;
};

} // namespace

std::unique_ptr<KeySource> windowsKeyboard(QWindow& window)
{
    return std::make_unique<WindowsKeyboard>(window);
}

} // namespace qkvm::ui
