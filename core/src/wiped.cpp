#include <qkvm/core/wiped.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <QChar>

// Windows' own header.
#include <windows.h>

namespace qkvm::core {

namespace {

// The UTF-8 of a text, a byte at a time to `put`, nothing made of it on the way; a surrogate by itself handed to
// `alone` (none: left out, as QString::toUtf8 leaves it out).
template <typename Put, typename Alone>
void eachUtf8(QStringView text, Put&& put, Alone&& alone)
{
    for (qsizetype at = 0; at < text.size(); ++at) {
        char32_t sign = text.at(at).unicode();
        if (QChar::isHighSurrogate(sign) && at + 1 < text.size() && text.at(at + 1).isLowSurrogate()) {
            sign = QChar::surrogateToUcs4(text.at(at), text.at(at + 1));
            ++at;
        } else if (QChar::isSurrogate(sign)) {
            alone(static_cast<char16_t>(sign));
            continue;
        }
        if (sign < 0x80) {
            put(static_cast<char>(sign));
        } else if (sign < 0x800) {
            put(static_cast<char>(0xC0 | (sign >> 6)));
            put(static_cast<char>(0x80 | (sign & 0x3F)));
        } else if (sign < 0x10000) {
            put(static_cast<char>(0xE0 | (sign >> 12)));
            put(static_cast<char>(0x80 | ((sign >> 6) & 0x3F)));
            put(static_cast<char>(0x80 | (sign & 0x3F)));
        } else {
            put(static_cast<char>(0xF0 | (sign >> 18)));
            put(static_cast<char>(0x80 | ((sign >> 12) & 0x3F)));
            put(static_cast<char>(0x80 | ((sign >> 6) & 0x3F)));
            put(static_cast<char>(0x80 | (sign & 0x3F)));
        }
    }
}

template <typename Put>
void eachUtf8(QStringView text, Put&& put)
{
    eachUtf8(text, put, [](char16_t) {});
}

// (As each writes them: QUrl in capitals, QJsonDocument in small letters.)
constexpr char hexDigits[] = "0123456789ABCDEF";
constexpr char smallHexDigits[] = "0123456789abcdef";

// One byte of a form's value: as it is, or percent-encoded (QUrl::toPercentEncoding's unreserved signs as they are).
template <typename Put>
void formByte(char byte, Put&& put)
{
    const auto sign = static_cast<unsigned char>(byte);
    const bool asItIs = (sign >= 'A' && sign <= 'Z') || (sign >= 'a' && sign <= 'z') || (sign >= '0' && sign <= '9') ||
                        sign == '-' || sign == '.' || sign == '_' || sign == '~';
    if (asItIs) {
        put(byte);
        return;
    }
    put('%');
    put(hexDigits[sign >> 4]);
    put(hexDigits[sign & 0x0F]);
}

// One byte of a JSON string, as QJsonDocument writes it.
template <typename Put>
void jsonByte(char byte, Put&& put)
{
    const auto sign = static_cast<unsigned char>(byte);
    const auto setOff = [&put](char what) {
        put('\\');
        put(what);
    };
    switch (sign) {
    case '"':
        setOff('"');
        return;
    case '\\':
        setOff('\\');
        return;
    case '\b':
        setOff('b');
        return;
    case '\f':
        setOff('f');
        return;
    case '\n':
        setOff('n');
        return;
    case '\r':
        setOff('r');
        return;
    case '\t':
        setOff('t');
        return;
    default:
        break;
    }
    if (sign < 0x20) {
        put('\\');
        put('u');
        put('0');
        put('0');
        put(smallHexDigits[sign >> 4]);
        put(smallHexDigits[sign & 0x0F]);
        return;
    }
    put(byte);
}

// Written twice: once to count, once into what was made at that size.
template <typename Write>
QByteArray madeOnce(Write&& write)
{
    qsizetype size = 0;
    write([&size](char) { ++size; });
    QByteArray made(size, Qt::Uninitialized);
    char* to = made.data();
    write([&to](char byte) { *to++ = byte; });
    return made;
}

} // namespace

void wipe(void* at, std::size_t bytes) noexcept
{
    if (at != nullptr && bytes > 0) {
        SecureZeroMemory(at, bytes);
    }
}

SecretKey::SecretKey(SecretKey&& other) noexcept : m_bytes(other.m_bytes)
{
    wipe(other.m_bytes.data(), other.m_bytes.size());
}

SecretKey& SecretKey::operator=(SecretKey&& other) noexcept
{
    if (this != &other) {
        m_bytes = other.m_bytes;
        wipe(other.m_bytes.data(), other.m_bytes.size());
    }
    return *this;
}

SecretKey::~SecretKey()
{
    wipe(m_bytes.data(), m_bytes.size());
}

SecretKey SecretKey::copy() const noexcept
{
    SecretKey second;
    second.m_bytes = m_bytes;
    return second;
}

QByteArrayView SecretKey::view() const noexcept
{
    return {reinterpret_cast<const char*>(m_bytes.data()), bytes};
}

SecretBytes::SecretBytes(qsizetype count)
    : m_bytes(std::make_unique<quint8[]>(static_cast<std::size_t>(count))), m_room(count)
{}

SecretBytes::SecretBytes(SecretBytes&& other) noexcept
    : m_bytes(std::move(other.m_bytes)), m_room(std::exchange(other.m_room, 0)), m_size(std::exchange(other.m_size, 0))
{}

SecretBytes& SecretBytes::operator=(SecretBytes&& other) noexcept
{
    if (this != &other) {
        letGo();
        m_bytes = std::move(other.m_bytes);
        m_room = std::exchange(other.m_room, 0);
        m_size = std::exchange(other.m_size, 0);
    }
    return *this;
}

SecretBytes::~SecretBytes()
{
    letGo();
}

void SecretBytes::letGo() noexcept
{
    // All of the room, not what is in use now: more may have been in use before.
    wipe(m_bytes.get(), static_cast<std::size_t>(m_room));
    m_bytes.reset();
    m_room = 0;
    m_size = 0;
}

std::optional<SecretBytes> SecretBytes::utf8Of(QStringView text)
{
    // A sign of the text takes three bytes at the most for each place it has there (four for
    // one that has two places).
    SecretBytes bytes(text.size() * 3 + 1);
    quint8* to = bytes.data();
    for (qsizetype at = 0; at < text.size(); ++at) {
        char32_t sign = text.at(at).unicode();
        if (QChar::isHighSurrogate(sign)) {
            if (at + 1 >= text.size() || !text.at(at + 1).isLowSurrogate()) {
                return std::nullopt;
            }
            sign = QChar::surrogateToUcs4(text.at(at), text.at(at + 1));
            ++at;
        } else if (QChar::isLowSurrogate(sign)) {
            return std::nullopt;
        }
        if (sign < 0x80) {
            *to++ = static_cast<quint8>(sign);
        } else if (sign < 0x800) {
            *to++ = static_cast<quint8>(0xC0 | (sign >> 6));
            *to++ = static_cast<quint8>(0x80 | (sign & 0x3F));
        } else if (sign < 0x10000) {
            *to++ = static_cast<quint8>(0xE0 | (sign >> 12));
            *to++ = static_cast<quint8>(0x80 | ((sign >> 6) & 0x3F));
            *to++ = static_cast<quint8>(0x80 | (sign & 0x3F));
        } else {
            *to++ = static_cast<quint8>(0xF0 | (sign >> 18));
            *to++ = static_cast<quint8>(0x80 | ((sign >> 12) & 0x3F));
            *to++ = static_cast<quint8>(0x80 | ((sign >> 6) & 0x3F));
            *to++ = static_cast<quint8>(0x80 | (sign & 0x3F));
        }
    }
    bytes.resize(to - bytes.data());
    return bytes;
}

SecretBytes SecretBytes::copyOf(QByteArrayView bytes)
{
    SecretBytes second(bytes.size() + 1);
    second.append(bytes);
    return second;
}

QByteArrayView SecretBytes::view() const noexcept
{
    return {reinterpret_cast<const char*>(m_bytes.get()), m_size};
}

void SecretBytes::resize(qsizetype count) noexcept
{
    m_size = std::clamp<qsizetype>(count, 0, m_room);
}

void wipeInPlace(QByteArray& bytes) noexcept
{
    // (Its own bytes only: data() makes no copy of what it holds alone.)
    if (!bytes.isEmpty() && bytes.isDetached()) {
        wipe(bytes.data(), static_cast<std::size_t>(bytes.size()));
    }
}

void wipeInPlace(QString& text) noexcept
{
    if (!text.isEmpty() && text.isDetached()) {
        wipe(text.data(), static_cast<std::size_t>(text.size()) * sizeof(QChar));
    }
}

SecretText SecretText::takenFrom(QString& text)
{
    SecretText taken;
    if (!text.isEmpty()) {
        taken.m_chars = std::make_unique<char16_t[]>(static_cast<std::size_t>(text.size()));
        std::copy_n(text.utf16(), text.size(), taken.m_chars.get());
        taken.m_size = text.size();
    }
    wipeInPlace(text);
    text = QString();
    return taken;
}

SecretText::SecretText(SecretText&& other) noexcept
    : m_chars(std::move(other.m_chars)), m_size(std::exchange(other.m_size, 0))
{}

SecretText& SecretText::operator=(SecretText&& other) noexcept
{
    if (this != &other) {
        letGo();
        m_chars = std::move(other.m_chars);
        m_size = std::exchange(other.m_size, 0);
    }
    return *this;
}

SecretText::~SecretText()
{
    letGo();
}

void SecretText::letGo() noexcept
{
    wipe(m_chars.get(), static_cast<std::size_t>(m_size) * sizeof(char16_t));
    m_chars.reset();
    m_size = 0;
}

SecretBytes secretUtf8(std::initializer_list<QStringView> texts)
{
    const auto write = [&texts](auto&& put) {
        bool first = true;
        for (const QStringView text : texts) {
            if (!std::exchange(first, false)) {
                put('\0');
            }
            eachUtf8(text, put);
        }
    };
    qsizetype size = 0;
    write([&size](char) { ++size; });
    // (One more than it holds: room is never none.)
    SecretBytes made(size + 1);
    quint8* to = made.data();
    write([&to](char byte) { *to++ = static_cast<quint8>(byte); });
    made.resize(size);
    return made;
}

QByteArray secretForm(std::initializer_list<std::pair<QByteArrayView, QStringView>> fields)
{
    return madeOnce([&fields](auto&& put) {
        bool first = true;
        for (const auto& [name, value] : fields) {
            if (!std::exchange(first, false)) {
                put('&');
            }
            for (const char byte : name) {
                put(byte);
            }
            put('=');
            eachUtf8(value, [&put](char byte) { formByte(byte, put); });
        }
    });
}

QByteArray secretJson(std::initializer_list<std::pair<QByteArrayView, QStringView>> members)
{
    return madeOnce([&members](auto&& put) {
        put('{');
        bool first = true;
        for (const auto& [key, value] : members) {
            if (!std::exchange(first, false)) {
                put(',');
            }
            put('"');
            for (const char byte : key) {
                jsonByte(byte, put);
            }
            put('"');
            put(':');
            put('"');
            // (A surrogate by itself as QJsonDocument writes one: by its number.)
            eachUtf8(
                value, [&put](char byte) { jsonByte(byte, put); },
                [&put](char16_t sign) {
                    put('\\');
                    put('u');
                    for (const int shift : {12, 8, 4, 0}) {
                        put(smallHexDigits[(sign >> shift) & 0x0F]);
                    }
                });
            put('"');
        }
        put('}');
    });
}

QByteArray takenOut(void* at, std::size_t bytes, qsizetype largest)
{
    if (at == nullptr || bytes == 0) {
        return {};
    }
    // (Written over also where the copy cannot be made: on every way out of here.)
    struct WrittenOver {
        void* at;
        std::size_t bytes;
        ~WrittenOver() { wipe(at, bytes); }
    } const over{at, bytes};
    if (bytes > static_cast<std::size_t>(std::max<qsizetype>(0, largest))) {
        return {};
    }
    return QByteArray(static_cast<const char*>(over.at), static_cast<qsizetype>(over.bytes));
}

bool SecretBytes::append(QByteArrayView more) noexcept
{
    if (more.size() > m_room - m_size) {
        return false;
    }
    if (!more.isEmpty()) {
        std::memcpy(m_bytes.get() + m_size, more.data(), static_cast<std::size_t>(more.size()));
        m_size += more.size();
    }
    return true;
}

} // namespace qkvm::core
