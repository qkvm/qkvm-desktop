#include <qkvm/core/typed_keys.h>

#include <algorithm>
#include <array>
#include <optional>

#include <QLatin1StringView>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

// As the web's app/qkvm/jetkvm.py: its tables (_MODIFIERS, _USAGE, _CHARS) as they are there, and its key_report and
// text_reports (for the US layout), with the rule of app/qkvm/kvm.py (KEYS, MAX_TEXT) that its redfish.py asks first.
namespace qkvm::core::typed {

namespace {

struct Named {
    QLatin1StringView name;
    std::uint8_t modifier; // its bit, for one of the eight modifiers
    std::uint8_t usage;    // ...else its USB number
};

// The web's _MODIFIERS and _USAGE: names as browsers spell them (KeyboardEvent.code).
constexpr std::array<Named, 104> names = {{
    {"ControlLeft"_L1, 0x01, 0},
    {"ShiftLeft"_L1, 0x02, 0},
    {"AltLeft"_L1, 0x04, 0},
    {"MetaLeft"_L1, 0x08, 0},
    {"ControlRight"_L1, 0x10, 0},
    {"ShiftRight"_L1, 0x20, 0},
    {"AltRight"_L1, 0x40, 0},
    {"MetaRight"_L1, 0x80, 0},
    {"KeyA"_L1, 0, 0x04},
    {"KeyB"_L1, 0, 0x05},
    {"KeyC"_L1, 0, 0x06},
    {"KeyD"_L1, 0, 0x07},
    {"KeyE"_L1, 0, 0x08},
    {"KeyF"_L1, 0, 0x09},
    {"KeyG"_L1, 0, 0x0A},
    {"KeyH"_L1, 0, 0x0B},
    {"KeyI"_L1, 0, 0x0C},
    {"KeyJ"_L1, 0, 0x0D},
    {"KeyK"_L1, 0, 0x0E},
    {"KeyL"_L1, 0, 0x0F},
    {"KeyM"_L1, 0, 0x10},
    {"KeyN"_L1, 0, 0x11},
    {"KeyO"_L1, 0, 0x12},
    {"KeyP"_L1, 0, 0x13},
    {"KeyQ"_L1, 0, 0x14},
    {"KeyR"_L1, 0, 0x15},
    {"KeyS"_L1, 0, 0x16},
    {"KeyT"_L1, 0, 0x17},
    {"KeyU"_L1, 0, 0x18},
    {"KeyV"_L1, 0, 0x19},
    {"KeyW"_L1, 0, 0x1A},
    {"KeyX"_L1, 0, 0x1B},
    {"KeyY"_L1, 0, 0x1C},
    {"KeyZ"_L1, 0, 0x1D},
    {"Digit1"_L1, 0, 0x1E},
    {"Digit2"_L1, 0, 0x1F},
    {"Digit3"_L1, 0, 0x20},
    {"Digit4"_L1, 0, 0x21},
    {"Digit5"_L1, 0, 0x22},
    {"Digit6"_L1, 0, 0x23},
    {"Digit7"_L1, 0, 0x24},
    {"Digit8"_L1, 0, 0x25},
    {"Digit9"_L1, 0, 0x26},
    {"Digit0"_L1, 0, 0x27},
    {"Enter"_L1, 0, 0x28},
    {"Escape"_L1, 0, 0x29},
    {"Backspace"_L1, 0, 0x2A},
    {"Tab"_L1, 0, 0x2B},
    {"Space"_L1, 0, 0x2C},
    {"Minus"_L1, 0, 0x2D},
    {"Equal"_L1, 0, 0x2E},
    {"BracketLeft"_L1, 0, 0x2F},
    {"BracketRight"_L1, 0, 0x30},
    {"Backslash"_L1, 0, 0x31},
    {"Semicolon"_L1, 0, 0x33},
    {"Quote"_L1, 0, 0x34},
    {"Backquote"_L1, 0, 0x35},
    {"Comma"_L1, 0, 0x36},
    {"Period"_L1, 0, 0x37},
    {"Slash"_L1, 0, 0x38},
    {"CapsLock"_L1, 0, 0x39},
    {"F1"_L1, 0, 0x3A},
    {"F2"_L1, 0, 0x3B},
    {"F3"_L1, 0, 0x3C},
    {"F4"_L1, 0, 0x3D},
    {"F5"_L1, 0, 0x3E},
    {"F6"_L1, 0, 0x3F},
    {"F7"_L1, 0, 0x40},
    {"F8"_L1, 0, 0x41},
    {"F9"_L1, 0, 0x42},
    {"F10"_L1, 0, 0x43},
    {"F11"_L1, 0, 0x44},
    {"F12"_L1, 0, 0x45},
    {"PrintScreen"_L1, 0, 0x46},
    {"ScrollLock"_L1, 0, 0x47},
    {"Pause"_L1, 0, 0x48},
    {"Insert"_L1, 0, 0x49},
    {"Home"_L1, 0, 0x4A},
    {"PageUp"_L1, 0, 0x4B},
    {"Delete"_L1, 0, 0x4C},
    {"End"_L1, 0, 0x4D},
    {"PageDown"_L1, 0, 0x4E},
    {"ArrowRight"_L1, 0, 0x4F},
    {"ArrowLeft"_L1, 0, 0x50},
    {"ArrowDown"_L1, 0, 0x51},
    {"ArrowUp"_L1, 0, 0x52},
    {"NumLock"_L1, 0, 0x53},
    {"NumpadDivide"_L1, 0, 0x54},
    {"NumpadMultiply"_L1, 0, 0x55},
    {"NumpadSubtract"_L1, 0, 0x56},
    {"NumpadAdd"_L1, 0, 0x57},
    {"NumpadEnter"_L1, 0, 0x58},
    {"Numpad1"_L1, 0, 0x59},
    {"Numpad2"_L1, 0, 0x5A},
    {"Numpad3"_L1, 0, 0x5B},
    {"Numpad4"_L1, 0, 0x5C},
    {"Numpad5"_L1, 0, 0x5D},
    {"Numpad6"_L1, 0, 0x5E},
    {"Numpad7"_L1, 0, 0x5F},
    {"Numpad8"_L1, 0, 0x60},
    {"Numpad9"_L1, 0, 0x61},
    {"Numpad0"_L1, 0, 0x62},
    {"NumpadDecimal"_L1, 0, 0x63},
    {"ContextMenu"_L1, 0, 0x65},
}};

const Named* named(QStringView name)
{
    const auto found = std::ranges::find_if(names, [name](const Named& one) { return one.name == name; });
    return found != names.end() ? &*found : nullptr;
}

constexpr std::uint8_t shift = 0x02; // left Shift's bit

// The web's _CHARS: what types a character on a US keyboard (Shift or not, and the key); nothing for one it has no
// key for.
std::optional<Report> onUsKeyboard(char32_t character)
{
    const auto key = [](std::uint8_t modifiers, std::uint8_t usage) { return Report{modifiers, {usage}}; };
    if (character >= U'a' && character <= U'z') {
        return key(0, static_cast<std::uint8_t>(0x04 + (character - U'a')));
    }
    if (character >= U'A' && character <= U'Z') {
        return key(shift, static_cast<std::uint8_t>(0x04 + (character - U'A')));
    }
    if (character >= U'1' && character <= U'9') {
        return key(0, static_cast<std::uint8_t>(0x1E + (character - U'1')));
    }
    if (character == U'0') {
        return key(0, 0x27);
    }
    // "!" to ")" are Shift and 1 to 0.
    constexpr std::u32string_view aboveDigits = U"!@#$%^&*()";
    if (const auto at = aboveDigits.find(character); at != std::u32string_view::npos) {
        return key(shift, static_cast<std::uint8_t>(0x1E + at));
    }
    constexpr std::u32string_view plain = U"`-=[]\\;',./";
    constexpr std::u32string_view shifted = U"~_+{}|:\"<>?";
    constexpr std::array<std::uint8_t, 11> beside = {0x35, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x36, 0x37, 0x38};
    if (const auto at = plain.find(character); at != std::u32string_view::npos) {
        return key(0, beside.at(at));
    }
    if (const auto at = shifted.find(character); at != std::u32string_view::npos) {
        return key(shift, beside.at(at));
    }
    switch (character) {
    case U' ':
        return key(0, 0x2C);
    case U'\n':
        return key(0, 0x28);
    case U'\t':
        return key(0, 0x2B);
    default:
        return std::nullopt;
    }
}

base::Error invalid(const QString& message)
{
    return {"invalid", message.toStdString()};
}

} // namespace

base::Result<Report> combination(const QString& keys, const QString& whose)
{
    // (The rule kvmd::callFor holds an action's keys to, and the web's KEYS, as a whole match: a line feed after it
    // is no part of a name.)
    static const QRegularExpression form(u"\\A[A-Z][A-Za-z0-9]{1,15}(,[A-Z][A-Za-z0-9]{1,15}){0,7}\\z"_s);
    if (!form.match(keys).hasMatch()) {
        return invalid(u"Not a valid key combination"_s);
    }
    Report report;
    for (const QStringView name : QStringView(keys).split(u',')) {
        const Named* key = named(name);
        if (key == nullptr) {
            return invalid(u"%1 has no %2 key"_s.arg(whose, name));
        }
        if (key->modifier != 0) {
            report.modifiers |= key->modifier;
        } else {
            report.keys.push_back(key->usage);
        }
    }
    if (static_cast<qsizetype>(report.keys.size()) > mostKeysDown) {
        return invalid(u"A keyboard can hold six keys at once, plus Ctrl, Alt, Shift and Win"_s);
    }
    return report;
}

base::Result<std::vector<Report>> text(const QString& text)
{
    // Counted as the web counts it, in characters and before a "\r\n" is made one.
    const QList<uint> characters = QStringView(text).toUcs4();
    if (characters.isEmpty() || characters.size() > mostCharacters) {
        return invalid(u"Text must be 1 to %1 characters"_s.arg(mostCharacters));
    }
    std::vector<Report> reports;
    reports.reserve(static_cast<std::size_t>(characters.size()));
    qsizetype place = 0;
    for (qsizetype at = 0; at < characters.size(); ++at) {
        if (characters[at] == U'\r' && at + 1 < characters.size() && characters[at + 1] == U'\n') {
            continue; // (the "\n" that follows is its Enter)
        }
        ++place;
        std::optional<Report> typed = onUsKeyboard(static_cast<char32_t>(characters[at]));
        if (!typed) {
            return invalid(u"Character %1 of the text cannot be typed on a US keyboard layout"_s.arg(place));
        }
        reports.push_back(std::move(*typed));
    }
    return reports;
}

bool Keyboard::take(int usage, bool down)
{
    // Page 7's keys: 4 (A) to 0xE7 (the right Windows key); below are the keyboard's own words for a fault, beyond
    // nothing a keyboard sends.
    if (usage < 0x04 || usage > 0xE7) {
        return false;
    }
    const auto key = static_cast<std::uint8_t>(usage);
    if (usage >= 0xE0) {
        const auto bit = static_cast<std::uint8_t>(1U << (usage - 0xE0));
        const auto before = m_now.modifiers;
        m_now.modifiers = down ? static_cast<std::uint8_t>(before | bit) : static_cast<std::uint8_t>(before & ~bit);
        return m_now.modifiers != before;
    }
    const auto at = std::ranges::find(m_now.keys, key);
    if (down == (at != m_now.keys.end())) {
        return false;
    }
    if (!down) {
        m_now.keys.erase(at);
        return true;
    }
    if (static_cast<qsizetype>(m_now.keys.size()) == mostKeysDown) {
        m_now.keys.erase(m_now.keys.begin()); // (the one that went down first)
    }
    m_now.keys.push_back(key);
    return true;
}

std::uint8_t buttonBit(int number) noexcept
{
    switch (number) {
    case 1:
        return 1; // left
    case 2:
        return 2; // right
    case 3:
        return 4; // middle
    default:
        return 0;
    }
}

} // namespace qkvm::core::typed
