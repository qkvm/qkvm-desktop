#pragma once

#include <cstdint>
#include <vector>

#include <QString>
#include <QStringView>

#include <qkvm/base/result.h>

// Keys pressed together and a text typed, as the reports of a USB keyboard that a machine is sent one after another:
// for a device that is handed a keyboard's reports and not words (a server's console, ServerScreen). As the web's
// app/qkvm/jetkvm.py has it (key_report, and text_reports for the US layout, with their tables), which the web's
// redfish.py shares for a server's console. A KVM that runs kvmd is told the names and the text themselves
// (kvmd_answers.h) and needs none of this. A key here is a place on the keyboard: the machine makes a letter of it
// with its own layout, and a text is typed as on a US one.
namespace qkvm::core::typed {

// A whole keyboard, as a USB boot keyboard says it: the modifiers held, a bit each (bit 0 left Ctrl, then left
// Shift, left Alt, left Windows, and the same four of the right), and the other keys down by their USB numbers (HID
// usage page 7), six at the most.
struct Report {
    std::uint8_t modifiers = 0;
    std::vector<std::uint8_t> keys;

    bool operator==(const Report&) const = default;
};
inline constexpr qsizetype mostKeysDown = 6;
// The most characters one text is (as kvmd_answers.h's mostTypedCharacters, and the server's MAX_TEXT).
inline constexpr qsizetype mostCharacters = 2000;

// Keys pressed together, by the names browsers give their places (KeyboardEvent.code), with commas between:
// "ControlLeft,AltLeft,Delete". Refused ("invalid") where it is not keys as an action names them (up to eight
// names, each a capital and one to fifteen letters and digits: the rule kvmd::callFor holds them to), where a name
// is not in the table ("<whose> has no <name> key"), and where more than six are not modifiers. The table is the
// web's: the eight modifiers, the letters and digits, Enter to Slash, Caps Lock, F1 to F12, the keys above the
// arrows, the arrows, the number block and the menu key.
base::Result<Report> combination(const QString& keys, const QString& whose);

// What types `text` on a computer set to the US layout, character by character: each pressed with Shift where it
// needs it (a capital, "!" and the like), "\n" as Enter and "\t" as Tab, a "\r\n" as one Enter. Refused ("invalid")
// where it is nothing or more than `mostCharacters` characters, and where a character has no key there: which, by
// its place (counted from one, in the text with each "\r\n" made one), never what it is, since a text may be a
// password and what is said of it may be written into a log.
base::Result<std::vector<Report>> text(const QString& text);

// The keyboard of a machine controlled live, as a line says its keys one by one (Live::key: a key's USB number, as
// input/keys.h gives it, down or up), kept as the whole keyboard that each report to a server's console is: the
// modifiers as bits, the other keys in the order they went down. As the web's live.RedfishHid keeps it.
class Keyboard {
public:
    // A key went down or up. Whether the keyboard changed: not for a key said down that is down already, nor for one
    // said up that is not down, nor for a number that is no key (page 7's keys are 4 to 0xE7). A seventh key down
    // lets go of the one that went down first, as a keyboard's own report does.
    bool take(int usage, bool down);
    const Report& now() const noexcept { return m_now; }
    // Nothing is down (everything was let go of at the far end).
    void clear() noexcept { m_now = {}; }

private:
    Report m_now;
};

// A button of the mouse as a server's console takes it: its bit in the report (1 left, 2 right, 3 middle, as
// Live::button numbers them); 0 for back and forward, which have none there, and for any other number.
std::uint8_t buttonBit(int number) noexcept;

} // namespace qkvm::core::typed
