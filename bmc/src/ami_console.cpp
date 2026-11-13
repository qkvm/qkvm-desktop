#include <qkvm/bmc/ami_console.h>

#include <algorithm>
// Built with the standard library's own checks in every build (bmc/CMakeLists.txt): a build without them is refused
// here, and never loses them without a word.
#if !_MSVC_STL_HARDENING_ARRAY || !_MSVC_STL_HARDENING_SPAN || !_MSVC_STL_HARDENING_VECTOR
#error "This reads what a management controller sends, and is built with _MSVC_STL_HARDENING (bmc/CMakeLists.txt)"
#endif
#include <string>

// As the web's app/qkvm/ami.py: the same bytes written, the same messages read, and the same refused.
namespace qkvm::bmc::ami {

namespace {

// Sizes of the fields of the pass message (text, padded with zeros).
constexpr std::size_t passField = 129;
constexpr std::size_t addressField = 65;
constexpr std::size_t nameField = 129;
constexpr std::size_t macField = 49;

// Where the facts of a frame's first piece say what they say.
constexpr std::size_t factWidth = 4;
constexpr std::size_t factHeight = 6;
constexpr std::size_t factGridWidth = 13;
constexpr std::size_t factGridHeight = 15;
constexpr std::size_t factStrength = 44;
constexpr std::size_t factLumaForColour = 45;
constexpr std::size_t factCorrectionStrength = 47;
constexpr std::size_t factEncrypted = 53;
constexpr std::size_t factHalfColour = 55;
constexpr std::size_t factLength = 69; // three bytes

base::Error garbled(std::string what)
{
    return {"garbled", std::move(what)};
}

void put16(Bytes& into, unsigned value)
{
    into.push_back(static_cast<std::uint8_t>(value));
    into.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put32(Bytes& into, std::size_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        into.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append(Bytes& into, const Bytes& more)
{
    into.insert(into.end(), more.begin(), more.end());
}

// A text in a field of `size` bytes: ASCII, one "?" for every character that is not (as the web's encodes it, with
// "replace"), at most `size - 1` of them, and zeros to the end. The text is taken as UTF-8: a byte that begins a
// character outside ASCII is its "?", and the bytes that continue it are nothing more.
void putText(Bytes& into, std::string_view text, std::size_t size, bool endsInAZero = true)
{
    std::size_t put = 0;
    for (const char one : text) {
        if (put == size - (endsInAZero ? 1 : 0)) {
            break;
        }
        const auto byte = static_cast<unsigned char>(one);
        if (byte < 0x80) {
            into.push_back(byte);
            ++put;
        } else if (byte >= 0xC0) {
            into.push_back('?');
            ++put;
        }
    }
    into.insert(into.end(), size - put, 0);
}

unsigned read16(std::span<const std::uint8_t> at)
{
    return static_cast<unsigned>(at[0]) | (static_cast<unsigned>(at[1]) << 8);
}

// The virtual USB device a report is for, as the IUSB header names it: its type, protocol, device and interface.
struct Device {
    std::uint8_t type;
    std::uint8_t protocol;
    std::uint8_t number;
    std::uint8_t face; // its interface

    static const Device keyboard;
    static const Device mouse;
};
const Device Device::keyboard{0x30, 0x10, 2, 0};
const Device Device::mouse{0x31, 0x20, 2, 1};

// The header's first bytes: "IUSB" and four blanks, then its version (1.0) and its length.
constexpr std::string_view iusb = "IUSB    ";
constexpr std::uint8_t iusbMajor = 1;
constexpr std::uint8_t iusbMinor = 0;
constexpr std::uint8_t iusbBytes = 32;
constexpr std::size_t iusbSum = 11;       // the byte that makes the header sum to zero
constexpr std::uint8_t fromRemote = 0x80; // the report goes to the server, not from it

// One keyboard or mouse message: the IUSB header for `device`, the report's length, the report.
Bytes hidMessage(const Device& device, std::uint32_t sequence, const Bytes& report)
{
    Bytes body(iusb.begin(), iusb.end());
    body.insert(body.end(), {iusbMajor, iusbMinor, iusbBytes, 0});
    put32(body, report.size() + 1); // the report and its one length byte
    body.insert(body.end(), {0, device.type, device.protocol, fromRemote, device.number, device.face, 0, 0});
    put32(body, sequence & 0x7FFFFFFFU);
    body.insert(body.end(), 4, 0);
    unsigned sum = 0;
    for (const std::uint8_t one : body) {
        sum += one;
    }
    body[iusbSum] = static_cast<std::uint8_t>(0x100 - (sum & 0xFF));
    body.push_back(static_cast<std::uint8_t>(report.size()));
    append(body, report);
    return message(kind::hid, 0, body);
}

} // namespace

std::string_view endedBecause(std::uint16_t status) noexcept
{
    switch (status) {
    case 2:
        return "its console service was restarted";
    case 5:
        return "its console settings were changed";
    case 7:
        return "its web session was signed out";
    case 8:
        return "its licence for the console ran out";
    case timeUp:
        return "the session's time was up";
    case 10:
        return "the session was ended at the controller";
    case 11:
        return "its web server was restarted";
    case 12:
        return "the controller was restarted";
    case 13:
        return "the controller was put back to how it came";
    default:
        return {};
    }
}

Bytes message(std::uint16_t kind, std::uint16_t status, std::span<const std::uint8_t> body)
{
    Bytes found;
    found.reserve(headBytes + body.size());
    put16(found, kind);
    put32(found, body.size());
    put16(found, status);
    found.insert(found.end(), body.begin(), body.end());
    return found;
}

Bytes hello(std::string_view consolePass, std::string_view ownAddress, std::string_view controllerAddress,
            std::string_view name, bool completeFirst)
{
    Bytes body = {0};
    putText(body, consolePass, passField);
    putText(body, ownAddress, addressField);
    putText(body, name, nameField);
    putText(body, "00-00-00-00-00-00", macField);
    putText(body, controllerAddress, addressField);
    Bytes found = completeFirst ? message(kind::complete, 1) : Bytes{};
    append(found, message(kind::pass, 1, body));
    append(found, message(kind::resume));
    return found;
}

Bytes letIn(std::span<const std::uint8_t> who)
{
    constexpr auto number = static_cast<std::uint16_t>(sharing::asked | answer::letIn << 8);
    Bytes found = message(kind::sharing, number, who);
    append(found, message(kind::nextHolder, number, who));
    return found;
}

Bytes goodbye()
{
    return message(kind::stop);
}

Bytes answerToLicence(std::string_view webSession)
{
    Bytes body;
    putText(body, webSession, 35, false);
    return message(kind::webSession, 0, body);
}

Bytes keyboard(std::uint8_t modifiers, std::span<const std::uint8_t> keys, std::uint32_t sequence)
{
    // The modifiers, a zero, and six places for keys, the first six that are down.
    Bytes report(2 + mostKeys, 0);
    report[0] = modifiers;
    const std::span<const std::uint8_t> held = keys.first(std::min(keys.size(), mostKeys));
    std::ranges::copy(held, report.begin() + 2);
    return hidMessage(Device::keyboard, sequence, report);
}

Bytes mouseAbsolute(std::uint8_t buttons, int x, int y, int wheel, std::uint32_t sequence)
{
    const auto across = static_cast<unsigned>(std::clamp(x, 0, farthest));
    const auto down = static_cast<unsigned>(std::clamp(y, 0, farthest));
    Bytes report{buttons};
    put16(report, across);
    put16(report, down);
    report.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(std::clamp(wheel, -127, 127))));
    return hidMessage(Device::mouse, sequence, report);
}

Bytes mouseRelative(std::uint8_t buttons, int dx, int dy, int wheel, std::uint32_t sequence)
{
    const auto signedByte = [](int value) {
        return static_cast<std::uint8_t>(static_cast<std::int8_t>(std::clamp(value, -127, 127)));
    };
    const Bytes report{buttons, signedByte(dx), signedByte(dy), signedByte(wheel)};
    return hidMessage(Device::mouse, sequence, report);
}

base::Result<std::vector<Message>> Messages::feed(std::span<const std::uint8_t> data)
{
    if (data.size() > largestArrival) {
        // (Kept and then copied out message by message, a gigabyte would cost two.)
        return garbled("more in one go than a console sends");
    }
    m_waiting.insert(m_waiting.end(), data.begin(), data.end());
    const std::span<const std::uint8_t> waiting(m_waiting);
    std::vector<Message> found;
    std::size_t at = 0;
    while (waiting.size() - at >= headBytes) {
        const std::span<const std::uint8_t> head = waiting.subspan(at, headBytes);
        const auto kindOf = static_cast<std::uint16_t>(read16(head.subspan(0, 2)));
        const std::size_t length = read16(head.subspan(2, 2)) | (std::size_t{read16(head.subspan(4, 2))} << 16);
        const auto status = static_cast<std::uint16_t>(read16(head.subspan(6, 2)));
        if (length > longest) {
            return garbled("a message of " + std::to_string(length) + " bytes");
        }
        if (waiting.size() - at < headBytes + length) {
            break;
        }
        if (found.size() >= mostAtOnce) {
            return garbled("more messages at once than a console sends");
        }
        const std::span<const std::uint8_t> body = waiting.subspan(at + headBytes, length);
        found.push_back({kindOf, status, Bytes(body.begin(), body.end())});
        at += headBytes + length;
    }
    // Once, and not after every message: what is left is moved up each time.
    m_waiting.erase(m_waiting.begin(), m_waiting.begin() + static_cast<std::ptrdiff_t>(at));
    return found;
}

base::Result<std::optional<WholeFrame>> Frames::piece(std::span<const std::uint8_t> body)
{
    std::span<const std::uint8_t> data = body.size() >= 2 ? body.subspan(2) : std::span<const std::uint8_t>{};
    if (!m_header) {
        if (data.size() < frameFacts) {
            return garbled("a frame that begins with less than what says how to read it");
        }
        const std::span<const std::uint8_t> facts = data.first(frameFacts);
        data = data.subspan(frameFacts);
        if (facts[factEncrypted]) {
            return garbled("a picture that is sent encrypted: this is not read yet");
        }
        const std::size_t wanted = std::size_t{facts[factLength]} | (std::size_t{facts[factLength + 1]} << 8) |
                                   (std::size_t{facts[factLength + 2]} << 16);
        if (wanted > longest) {
            return garbled("a frame longer than a screen can be"); // said here, before its pieces are collected
        }
        m_header = aspeed::FrameHeader{static_cast<int>(read16(facts.subspan(factWidth, 2))),
                                       static_cast<int>(read16(facts.subspan(factHeight, 2))),
                                       static_cast<int>(read16(facts.subspan(factGridWidth, 2))),
                                       static_cast<int>(read16(facts.subspan(factGridHeight, 2))),
                                       facts[factHalfColour] == 1,
                                       facts[factStrength],
                                       facts[factCorrectionStrength],
                                       facts[factLumaForColour] == 1};
        m_wanted = wanted;
        m_data.clear();
    }
    // What a piece brings beyond its frame's end is no frame's (the web's drops it too): it is not kept.
    const std::size_t taken = std::min(data.size(), m_wanted - m_data.size());
    m_data.insert(m_data.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(taken));
    if (m_data.size() < m_wanted) {
        return std::optional<WholeFrame>{};
    }
    std::optional<WholeFrame> whole = WholeFrame{*m_header, std::move(m_data)};
    m_header.reset();
    m_data = Bytes{};
    return whole;
}

} // namespace qkvm::bmc::ami
