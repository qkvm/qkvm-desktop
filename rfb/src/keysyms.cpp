// The keys, from X11's keysym list and the USB HID usage tables (rfb.py's KEYSYMS, keysym_for, keysym_of).
#include <qkvm/rfb/rfb.h>

#include <utility>

namespace qkvm::rfb {

namespace {

// HID keyboard usage -> X11 keysym, as the key is without Shift and with it, on the US layout where the key has a
// character (the same twice where it has none).
struct Key {
    int usage;
    std::uint32_t plain;
    std::uint32_t shifted;
};

constexpr std::array<Key, 17> signs{{
    {0x1E, '1', '!'},
    {0x1F, '2', '@'},
    {0x20, '3', '#'},
    {0x21, '4', '$'},
    {0x22, '5', '%'},
    {0x23, '6', '^'},
    {0x24, '7', '&'},
    {0x25, '8', '*'},
    {0x26, '9', '('},
    {0x27, '0', ')'},
    {0x2D, 0x2D, 0x5F}, // - _
    {0x2E, 0x3D, 0x2B}, // = +
    {0x2F, 0x5B, 0x7B}, // [ {
    {0x30, 0x5D, 0x7D}, // ] }
    {0x31, 0x5C, 0x7C}, // \ |
    {0x32, 0x23, 0x7E}, // non-US # ~
    {0x33, 0x3B, 0x3A}, // ; :
}};
constexpr std::array<Key, 5> moreSigns{{
    {0x34, 0x27, 0x22}, // ' "
    {0x35, 0x60, 0x7E}, // ` ~
    {0x36, 0x2C, 0x3C}, // , <
    {0x37, 0x2E, 0x3E}, // . >
    {0x38, 0x2F, 0x3F}, // / ?
}};
constexpr std::array<std::pair<int, std::uint32_t>, 58> named{{
    {0x28, 0xFF0D}, {0x29, 0xFF1B}, {0x2A, 0xFF08}, {0x2B, 0xFF09},
    {0x2C, 0x0020},                                                 // Return, Escape, BackSpace, Tab, space
    {0x39, 0xFFE5}, {0x46, 0xFF61}, {0x47, 0xFF14}, {0x48, 0xFF13}, // Caps_Lock, Print, Scroll_Lock, Pause
    {0x49, 0xFF63}, {0x4A, 0xFF50}, {0x4B, 0xFF55}, {0x4C, 0xFFFF},
    {0x4D, 0xFF57}, {0x4E, 0xFF56},                                 // Insert .. Page_Down
    {0x4F, 0xFF53}, {0x50, 0xFF51}, {0x51, 0xFF54}, {0x52, 0xFF52}, // Right, Left, Down, Up
    {0x53, 0xFF7F}, {0x54, 0xFFAF}, {0x55, 0xFFAA}, {0x56, 0xFFAD},
    {0x57, 0xFFAB}, {0x58, 0xFF8D}, // Num_Lock .. KP_Enter
    {0x62, 0xFFB0}, {0x63, 0xFFAE}, // KP_0, KP_Decimal
    {0x65, 0xFF67}, {0x67, 0xFFBD}, {0x74, 0xFF62}, {0x75, 0xFF6A},
    {0x76, 0xFF67}, {0x77, 0xFF60}, // Menu .. Select
    {0x78, 0xFF69}, {0x79, 0xFF66}, {0x7A, 0xFF65}, {0x7E, 0xFF68},
    {0x85, 0xFFAC}, {0x86, 0xFFBD}, // Cancel .. KP_Equal
    {0x9A, 0xFF15}, {0x9B, 0xFF69}, {0x9C, 0xFF0B}, {0x9E, 0xFF0D},
    {0xBA, 0xFF89}, {0xCD, 0xFF80},                                 // Sys_Req .. KP_Space
    {0xE0, 0xFFE3}, {0xE1, 0xFFE1}, {0xE2, 0xFFE9}, {0xE3, 0xFFEB}, // Control_L, Shift_L, Alt_L, Super_L
    {0xE4, 0xFFE4}, {0xE5, 0xFFE2}, {0xE6, 0xFFEA}, {0xE7, 0xFFEC}, // Control_R, Shift_R, Alt_R, Super_R
    {0x59, 0xFFB1}, {0x5A, 0xFFB2}, {0x5B, 0xFFB3}, {0x5C, 0xFFB4},
    {0x5D, 0xFFB5}, // KP_1 .. KP_5
}};
constexpr std::array<std::pair<int, std::uint32_t>, 4> moreNamed{{
    {0x5E, 0xFFB6}, {0x5F, 0xFFB7}, {0x60, 0xFFB8}, {0x61, 0xFFB9}, // KP_6 .. KP_9
}};

} // namespace

std::optional<std::uint32_t> keysymFor(int usage, bool shift)
{
    if (usage >= 0x04 && usage <= 0x1D) { // a .. z, A .. Z
        const auto letter = static_cast<std::uint32_t>(usage - 0x04);
        return (shift ? 0x41U : 0x61U) + letter;
    }
    if (usage >= 0x3A && usage <= 0x45) { // F1 .. F12
        return 0xFFBEU + static_cast<std::uint32_t>(usage - 0x3A);
    }
    if (usage >= 0x68 && usage <= 0x73) { // F13 .. F24
        return 0xFFCAU + static_cast<std::uint32_t>(usage - 0x68);
    }
    for (const auto& list : {std::span<const Key>(signs), std::span<const Key>(moreSigns)}) {
        for (const Key& key : list) {
            if (key.usage == usage) {
                return shift ? key.shifted : key.plain;
            }
        }
    }
    if (usage == 0x64) { // non-US \ |
        return shift ? 0x7CU : 0x5CU;
    }
    for (const auto& list : {std::span<const std::pair<int, std::uint32_t>>(named),
                             std::span<const std::pair<int, std::uint32_t>>(moreNamed)}) {
        for (const auto& [key, keysym] : list) {
            if (key == usage) {
                return keysym;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> keysymOf(char32_t character)
{
    const auto code = static_cast<std::uint32_t>(character);
    if (character == U'\n') {
        return 0xFF0DU;
    }
    if (character == U'\t') {
        return 0xFF09U;
    }
    if ((code >= 0x20 && code <= 0x7E) || (code >= 0xA0 && code <= 0xFF)) {
        return code;
    }
    if (code < 0x100 || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) {
        return std::nullopt; // a control character, or half of one, or none
    }
    return unicodeKeysym + code;
}

} // namespace qkvm::rfb
