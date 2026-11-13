#include <qkvm/core/kvmd_input.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

using namespace Qt::StringLiterals;

namespace qkvm::core::kvmd {

namespace {

struct Named {
    int usage;
    QLatin1StringView name;
};

// A key's USB number (HID usage tables, page 7) and the web's name for the same place on the
// keyboard (UI Events, KeyboardEvent code values), which is the name kvmd takes. Only the keys
// kvmd is known to have: a name it does not know it lets fall without a word, and a key that
// does nothing is better told here than found out on a server.
// Not in it: 0x32, the key beside Enter on keyboards that are not American, which has no name of
// its own on the web, and which Windows tells of by the American key's number anyway (0x31); the
// function keys beyond F12, "=" and "," on the number block, and the two keys that switch a Korean
// keyboard, which have names on the web and none kvmd is known to take. Nor 0x66, the keyboard's Power
// key ("Power"), which kvmd has: it shuts a machine down without the question every power action is
// asked first, and is never said to a machine (as the web's live line, NOT_SENT; kvmd::namesThePowerKey).
constexpr std::array<Named, 110> names = {{
    {0x04, "KeyA"_L1},
    {0x05, "KeyB"_L1},
    {0x06, "KeyC"_L1},
    {0x07, "KeyD"_L1},
    {0x08, "KeyE"_L1},
    {0x09, "KeyF"_L1},
    {0x0A, "KeyG"_L1},
    {0x0B, "KeyH"_L1},
    {0x0C, "KeyI"_L1},
    {0x0D, "KeyJ"_L1},
    {0x0E, "KeyK"_L1},
    {0x0F, "KeyL"_L1},
    {0x10, "KeyM"_L1},
    {0x11, "KeyN"_L1},
    {0x12, "KeyO"_L1},
    {0x13, "KeyP"_L1},
    {0x14, "KeyQ"_L1},
    {0x15, "KeyR"_L1},
    {0x16, "KeyS"_L1},
    {0x17, "KeyT"_L1},
    {0x18, "KeyU"_L1},
    {0x19, "KeyV"_L1},
    {0x1A, "KeyW"_L1},
    {0x1B, "KeyX"_L1},
    {0x1C, "KeyY"_L1},
    {0x1D, "KeyZ"_L1},
    {0x1E, "Digit1"_L1},
    {0x1F, "Digit2"_L1},
    {0x20, "Digit3"_L1},
    {0x21, "Digit4"_L1},
    {0x22, "Digit5"_L1},
    {0x23, "Digit6"_L1},
    {0x24, "Digit7"_L1},
    {0x25, "Digit8"_L1},
    {0x26, "Digit9"_L1},
    {0x27, "Digit0"_L1},
    {0x28, "Enter"_L1},
    {0x29, "Escape"_L1},
    {0x2A, "Backspace"_L1},
    {0x2B, "Tab"_L1},
    {0x2C, "Space"_L1},
    {0x2D, "Minus"_L1},
    {0x2E, "Equal"_L1},
    {0x2F, "BracketLeft"_L1},
    {0x30, "BracketRight"_L1},
    {0x31, "Backslash"_L1},
    {0x33, "Semicolon"_L1},
    {0x34, "Quote"_L1},
    {0x35, "Backquote"_L1},
    {0x36, "Comma"_L1},
    {0x37, "Period"_L1},
    {0x38, "Slash"_L1},
    {0x39, "CapsLock"_L1},
    {0x3A, "F1"_L1},
    {0x3B, "F2"_L1},
    {0x3C, "F3"_L1},
    {0x3D, "F4"_L1},
    {0x3E, "F5"_L1},
    {0x3F, "F6"_L1},
    {0x40, "F7"_L1},
    {0x41, "F8"_L1},
    {0x42, "F9"_L1},
    {0x43, "F10"_L1},
    {0x44, "F11"_L1},
    {0x45, "F12"_L1},
    {0x46, "PrintScreen"_L1},
    {0x47, "ScrollLock"_L1},
    {0x48, "Pause"_L1},
    {0x49, "Insert"_L1},
    {0x4A, "Home"_L1},
    {0x4B, "PageUp"_L1},
    {0x4C, "Delete"_L1},
    {0x4D, "End"_L1},
    {0x4E, "PageDown"_L1},
    {0x4F, "ArrowRight"_L1},
    {0x50, "ArrowLeft"_L1},
    {0x51, "ArrowDown"_L1},
    {0x52, "ArrowUp"_L1},
    {0x53, "NumLock"_L1},
    {0x54, "NumpadDivide"_L1},
    {0x55, "NumpadMultiply"_L1},
    {0x56, "NumpadSubtract"_L1},
    {0x57, "NumpadAdd"_L1},
    {0x58, "NumpadEnter"_L1},
    {0x59, "Numpad1"_L1},
    {0x5A, "Numpad2"_L1},
    {0x5B, "Numpad3"_L1},
    {0x5C, "Numpad4"_L1},
    {0x5D, "Numpad5"_L1},
    {0x5E, "Numpad6"_L1},
    {0x5F, "Numpad7"_L1},
    {0x60, "Numpad8"_L1},
    {0x61, "Numpad9"_L1},
    {0x62, "Numpad0"_L1},
    {0x63, "NumpadDecimal"_L1},
    {0x64, "IntlBackslash"_L1},
    {0x65, "ContextMenu"_L1},
    {0x87, "IntlRo"_L1},
    {0x88, "KanaMode"_L1},
    {0x89, "IntlYen"_L1},
    {0x8A, "Convert"_L1},
    {0x8B, "NonConvert"_L1},
    {0xE0, "ControlLeft"_L1},
    {0xE1, "ShiftLeft"_L1},
    {0xE2, "AltLeft"_L1},
    {0xE3, "MetaLeft"_L1},
    {0xE4, "ControlRight"_L1},
    {0xE5, "ShiftRight"_L1},
    {0xE6, "AltRight"_L1},
    {0xE7, "MetaRight"_L1},
}};

// kvmd's names for the buttons of a mouse, by their numbers here: 4 is "back", 5 "forward".
constexpr std::array<QLatin1StringView, 5> buttons = {"left"_L1, "right"_L1, "middle"_L1, "up"_L1, "down"_L1};

// One message as kvmd takes them all: what it is about, and what is said of that.
QByteArray written(QLatin1StringView kind, const QJsonObject& event)
{
    return QJsonDocument(QJsonObject{{u"event_type"_s, kind}, {u"event"_s, event}}).toJson(QJsonDocument::Compact);
}

QJsonObject both(int x, int y)
{
    return {{u"x"_s, x}, {u"y"_s, y}};
}

// A place from 0 to 32767 as kvmd counts it, from the one edge to the other: both ends on the
// ends, and the middle in the middle.
int placed(int asked)
{
    const qint64 within = std::clamp(asked, 0, farthestAsked);
    const qint64 span = qint64{rightmostSaid} - leftmostSaid;
    return static_cast<int>((within * span + farthestAsked / 2) / farthestAsked + leftmostSaid);
}

// A move as the messages it is cut into: as few as take the longer way of the two, each a like
// part of the whole. Where the move has got to after each of them is on the straight line, and
// after the last it is all of it.
struct Step {
    int x = 0;
    int y = 0;
};

QList<Step> stepsOf(int dx, int dy)
{
    const int farthest = longestStep * mostSteps;
    const int x = std::clamp(dx, -farthest, farthest);
    const int y = std::clamp(dy, -farthest, farthest);
    const int steps = (std::max(std::abs(x), std::abs(y)) + longestStep - 1) / longestStep;
    QList<Step> all;
    all.reserve(steps);
    for (int step = 1; step <= steps; ++step) {
        all.append({x * step / steps - x * (step - 1) / steps, y * step / steps - y * (step - 1) / steps});
    }
    return all;
}

// What each message of bytes begins with.
constexpr char aKey = 1;
constexpr char aButton = 2;
constexpr char aPlace = 3;
constexpr char aMove = 4;
constexpr char aTurn = 5;

// A message that names something: what it is, down or up, and the name.
QByteArray named(char kind, bool down, QLatin1StringView name)
{
    QByteArray bytes;
    bytes.reserve(2 + name.size());
    bytes.append(kind);
    bytes.append(down ? char{1} : char{0});
    bytes.append(name.data(), name.size());
    return bytes;
}

// A message of two small numbers: what it is, a zero, and the two, each in a byte.
QByteArray twoSmall(char kind, int x, int y)
{
    QByteArray bytes;
    bytes.append(kind);
    bytes.append(char{0});
    bytes.append(static_cast<char>(static_cast<qint8>(x)));
    bytes.append(static_cast<char>(static_cast<qint8>(y)));
    return bytes;
}

} // namespace

std::optional<QLatin1StringView> keyName(int usage)
{
    const auto found = std::ranges::find(names, usage, &Named::usage);
    if (found == names.end()) {
        return std::nullopt;
    }
    return found->name;
}

std::optional<QByteArray> keyWord(int usage, bool down)
{
    const std::optional<QLatin1StringView> name = keyName(usage);
    if (!name) {
        return std::nullopt;
    }
    return written("key"_L1, {{u"key"_s, *name}, {u"state"_s, down}});
}

std::optional<QByteArray> buttonWord(int number, bool down)
{
    if (number < 1 || number > static_cast<int>(buttons.size())) {
        return std::nullopt;
    }
    return written("mouse_button"_L1,
                   {{u"button"_s, buttons.at(static_cast<std::size_t>(number - 1))}, {u"state"_s, down}});
}

QByteArray pointerAtWord(int x, int y)
{
    return written("mouse_move"_L1, {{u"to"_s, both(placed(x), placed(y))}});
}

QList<QByteArray> pointerByWords(int dx, int dy)
{
    QList<QByteArray> words;
    for (const Step& step : stepsOf(dx, dy)) {
        words.append(written("mouse_relative"_L1, {{u"delta"_s, both(step.x, step.y)}}));
    }
    return words;
}

std::optional<QByteArray> wheelWord(int dx, int dy)
{
    const int x = std::clamp(dx, -longestStep, longestStep);
    const int y = std::clamp(dy, -longestStep, longestStep);
    if (x == 0 && y == 0) {
        return std::nullopt;
    }
    return written("mouse_wheel"_L1, {{u"delta"_s, both(x, y)}});
}

QByteArray pingWord()
{
    return written("ping"_L1, {});
}

std::optional<QByteArray> keyBytes(int usage, bool down)
{
    const std::optional<QLatin1StringView> name = keyName(usage);
    if (!name) {
        return std::nullopt;
    }
    return named(aKey, down, *name);
}

std::optional<QByteArray> buttonBytes(int number, bool down)
{
    if (number < 1 || number > static_cast<int>(buttons.size())) {
        return std::nullopt;
    }
    return named(aButton, down, buttons.at(static_cast<std::size_t>(number - 1)));
}

QByteArray pointerAtBytes(int x, int y)
{
    const auto toX = static_cast<quint16>(static_cast<qint16>(placed(x)));
    const auto toY = static_cast<quint16>(static_cast<qint16>(placed(y)));
    QByteArray bytes;
    bytes.append(aPlace);
    bytes.append(static_cast<char>(toX >> 8));
    bytes.append(static_cast<char>(toX & 0xFF));
    bytes.append(static_cast<char>(toY >> 8));
    bytes.append(static_cast<char>(toY & 0xFF));
    return bytes;
}

QList<QByteArray> pointerByBytes(int dx, int dy)
{
    QList<QByteArray> all;
    for (const Step& step : stepsOf(dx, dy)) {
        all.append(twoSmall(aMove, step.x, step.y));
    }
    return all;
}

std::optional<QByteArray> wheelBytes(int dx, int dy)
{
    const int x = std::clamp(dx, -longestStep, longestStep);
    const int y = std::clamp(dy, -longestStep, longestStep);
    if (x == 0 && y == 0) {
        return std::nullopt;
    }
    return twoSmall(aTurn, x, y);
}

} // namespace qkvm::core::kvmd
