#include <qkvm/core/strict_json.h>

#include <charconv>
#include <cmath>
#include <string>
#include <system_error>
#include <utility>

#include <QChar>
#include <QLatin1StringView>

namespace qkvm::core::json {

namespace {

// One sign of a text in UTF-8, read by the book: `at` is moved behind it. Nothing where the
// bytes are no sign: one written longer than it needs to be, a surrogate, one above the last
// there is, a byte that only follows others, or a sign that is cut off.
std::optional<char32_t> signAt(const uchar*& at, const uchar* end) noexcept
{
    const uchar first = *at;
    if (first < 0x80) {
        ++at;
        return first;
    }
    // How many bytes follow, the least and the most the second of them may be, and what the
    // first gives.
    int following = 0;
    uchar least = 0x80;
    uchar most = 0xBF;
    char32_t sign = 0;
    if (first >= 0xC2 && first <= 0xDF) {
        following = 1;
        sign = first & 0x1FU;
    } else if (first >= 0xE0 && first <= 0xEF) {
        following = 2;
        sign = first & 0x0FU;
        least = first == 0xE0 ? uchar{0xA0} : uchar{0x80};
        most = first == 0xED ? uchar{0x9F} : uchar{0xBF};
    } else if (first >= 0xF0 && first <= 0xF4) {
        following = 3;
        sign = first & 0x07U;
        least = first == 0xF0 ? uchar{0x90} : uchar{0x80};
        most = first == 0xF4 ? uchar{0x8F} : uchar{0xBF};
    } else {
        return std::nullopt;
    }
    if (end - at <= following) {
        return std::nullopt;
    }
    for (int index = 1; index <= following; ++index) {
        const uchar next = at[index];
        if (next < (index == 1 ? least : 0x80) || next > (index == 1 ? most : 0xBF)) {
            return std::nullopt;
        }
        sign = (sign << 6) | (next & 0x3F);
    }
    at += following + 1;
    return sign;
}

bool isDigit(uchar sign) noexcept
{
    return sign >= '0' && sign <= '9';
}

std::optional<uint> hexDigit(uchar sign) noexcept
{
    if (isDigit(sign)) {
        return static_cast<uint>(sign - '0');
    }
    if (sign >= 'a' && sign <= 'f') {
        return static_cast<uint>(sign - 'a' + 10);
    }
    if (sign >= 'A' && sign <= 'F') {
        return static_cast<uint>(sign - 'A' + 10);
    }
    return std::nullopt;
}

class Reader {
public:
    explicit Reader(QByteArrayView bytes)
        : m_at(reinterpret_cast<const uchar*>(bytes.data())), m_end(m_at + bytes.size())
    {}

    base::Result<Value> all()
    {
        Value value;
        if (!read(value, 0)) {
            return refusal();
        }
        skipBlanks();
        if (m_at != m_end) {
            m_why = notJson;
            return refusal();
        }
        return base::Result<Value>(std::move(value));
    }

private:
    // (Its sentence is shown to nobody: whoever reads a file says its own.)
    base::Error refusal() const { return {m_why != nullptr ? m_why : notJson, "This is not a text of JSON."}; }

    bool no(const char* why) noexcept
    {
        m_why = why;
        return false;
    }

    void skipBlanks() noexcept
    {
        while (m_at != m_end && (*m_at == ' ' || *m_at == '\t' || *m_at == '\n' || *m_at == '\r')) {
            ++m_at;
        }
    }

    bool takes(char sign) noexcept
    {
        if (m_at != m_end && *m_at == static_cast<uchar>(sign)) {
            ++m_at;
            return true;
        }
        return false;
    }

    bool word(QByteArrayView written) noexcept
    {
        if (m_end - m_at < written.size() || QByteArrayView(m_at, written.size()) != written) {
            return no(notJson);
        }
        m_at += written.size();
        return true;
    }

    // One value, and what is in it: one in another no deeper than deepestNesting.
    bool read(Value& into, int depth)
    {
        skipBlanks();
        if (m_at == m_end) {
            return no(notJson);
        }
        ++m_values;
        if (m_values > mostValues) {
            return no(tooMany);
        }
        const uchar first = *m_at;
        if (first == '{' || first == '[') {
            if (depth >= deepestNesting) {
                return no(tooDeep);
            }
            ++m_at;
            return first == '{' ? object(into, depth) : list(into, depth);
        }
        if (first == '"') {
            ++m_at;
            into.kind = Value::Kind::Text;
            return text(into.text);
        }
        if (first == '-' || isDigit(first)) {
            return number(into);
        }
        if (first == 't') {
            into.kind = Value::Kind::Flag;
            into.flag = true;
            return word("true");
        }
        if (first == 'f') {
            into.kind = Value::Kind::Flag;
            return word("false");
        }
        if (first == 'n') {
            return word("null");
        }
        return no(notJson);
    }

    bool object(Value& into, int depth)
    {
        into.kind = Value::Kind::Object;
        skipBlanks();
        if (takes('}')) {
            return true;
        }
        while (true) {
            skipBlanks();
            if (!takes('"')) {
                return no(notJson);
            }
            Member member;
            if (!text(member.name)) {
                return false;
            }
            skipBlanks();
            if (!takes(':')) {
                return no(notJson);
            }
            if (!read(member.value, depth + 1)) {
                return false;
            }
            into.members.push_back(std::move(member));
            skipBlanks();
            if (takes('}')) {
                return true;
            }
            if (!takes(',')) {
                return no(notJson);
            }
        }
    }

    bool list(Value& into, int depth)
    {
        into.kind = Value::Kind::List;
        skipBlanks();
        if (takes(']')) {
            return true;
        }
        while (true) {
            Value value;
            if (!read(value, depth + 1)) {
                return false;
            }
            into.list.push_back(std::move(value));
            skipBlanks();
            if (takes(']')) {
                return true;
            }
            if (!takes(',')) {
                return no(notJson);
            }
        }
    }

    // Four digits to the base of sixteen, as they follow a backslash and the letter u.
    std::optional<uint> fourHex() noexcept
    {
        if (m_end - m_at < 4) {
            return std::nullopt;
        }
        uint unit = 0;
        for (int index = 0; index < 4; ++index) {
            const std::optional<uint> digit = hexDigit(m_at[index]);
            if (!digit) {
                return std::nullopt;
            }
            unit = unit * 16 + *digit;
        }
        m_at += 4;
        return unit;
    }

    // What follows a backslash. A sign that takes two places is spelled as its two halves, one
    // behind the other; a half by itself is no sign.
    bool escape(QString& into)
    {
        if (m_at == m_end) {
            return no(notJson);
        }
        const uchar named = *m_at++;
        switch (named) {
        case '"':
        case '\\':
        case '/':
            into += QChar(static_cast<char16_t>(named));
            return true;
        case 'b':
            into += QChar(u'\b');
            return true;
        case 'f':
            into += QChar(u'\f');
            return true;
        case 'n':
            into += QChar(u'\n');
            return true;
        case 'r':
            into += QChar(u'\r');
            return true;
        case 't':
            into += QChar(u'\t');
            return true;
        case 'u':
            break;
        default:
            return no(notJson);
        }
        const std::optional<uint> unit = fourHex();
        if (!unit) {
            return no(notJson);
        }
        if (QChar::isLowSurrogate(*unit)) {
            return no(halfACharacter);
        }
        if (!QChar::isHighSurrogate(*unit)) {
            into += QChar(static_cast<char16_t>(*unit));
            return true;
        }
        if (m_end - m_at < 2 || m_at[0] != '\\' || m_at[1] != 'u') {
            return no(halfACharacter);
        }
        m_at += 2;
        const std::optional<uint> second = fourHex();
        if (!second) {
            return no(notJson);
        }
        if (!QChar::isLowSurrogate(*second)) {
            return no(halfACharacter);
        }
        into += QChar(static_cast<char16_t>(*unit));
        into += QChar(static_cast<char16_t>(*second));
        return true;
    }

    // A text, from behind its first quote to behind its last.
    bool text(QString& into)
    {
        while (true) {
            // What needs no looking at, taken in one piece.
            const uchar* plain = m_at;
            while (m_at != m_end && *m_at >= 0x20 && *m_at < 0x80 && *m_at != '"' && *m_at != '\\') {
                ++m_at;
            }
            if (m_at != plain) {
                into += QLatin1StringView(reinterpret_cast<const char*>(plain), m_at - plain);
            }
            if (m_at == m_end) {
                return no(notJson);
            }
            const uchar sign = *m_at;
            if (sign == '"') {
                ++m_at;
                return true;
            }
            if (sign == '\\') {
                ++m_at;
                if (!escape(into)) {
                    return false;
                }
            } else if (sign < 0x20) {
                // A line's end, a tab or their like inside a text: spelled out, or not there.
                return no(notJson);
            } else {
                const std::optional<char32_t> wide = signAt(m_at, m_end);
                if (!wide) {
                    return no(notUtf8);
                }
                into += QChar::fromUcs4(*wide);
            }
        }
    }

    bool number(Value& into)
    {
        const uchar* start = m_at;
        bool whole = true;
        takes('-');
        if (m_at == m_end || !isDigit(*m_at)) {
            return no(notJson);
        }
        // A nought stands by itself; any other digit may have more behind it.
        if (!takes('0')) {
            while (m_at != m_end && isDigit(*m_at)) {
                ++m_at;
            }
        }
        if (takes('.')) {
            whole = false;
            if (m_at == m_end || !isDigit(*m_at)) {
                return no(notJson);
            }
            while (m_at != m_end && isDigit(*m_at)) {
                ++m_at;
            }
        }
        if (takes('e') || takes('E')) {
            whole = false;
            if (!takes('+')) {
                takes('-');
            }
            if (m_at == m_end || !isDigit(*m_at)) {
                return no(notJson);
            }
            while (m_at != m_end && isDigit(*m_at)) {
                ++m_at;
            }
        }
        if (m_at - start > longestNumber) {
            return no(notANumber);
        }
        double value = 0;
        const char* first = reinterpret_cast<const char*>(start);
        const char* last = reinterpret_cast<const char*>(m_at);
        const std::from_chars_result made = std::from_chars(first, last, value);
        if (made.ec != std::errc() || made.ptr != last || !std::isfinite(value)) {
            return no(notANumber);
        }
        into.kind = Value::Kind::Number;
        into.number = value;
        into.whole = whole;
        return true;
    }

    const uchar* m_at;
    const uchar* m_end;
    qsizetype m_values = 0;
    const char* m_why = nullptr;
};

} // namespace

const Value* Value::find(QStringView name) const noexcept
{
    for (auto member = members.rbegin(); member != members.rend(); ++member) {
        if (member->name == name) {
            return &member->value;
        }
    }
    return nullptr;
}

base::Result<Value> parse(QByteArrayView text)
{
    return Reader(text).all();
}

std::optional<qsizetype> signsIn(QByteArrayView text) noexcept
{
    const auto* at = reinterpret_cast<const uchar*>(text.data());
    const uchar* end = at + text.size();
    qsizetype signs = 0;
    while (at != end) {
        if (*at < 0x80) {
            ++at;
        } else if (!signAt(at, end)) {
            return std::nullopt;
        }
        ++signs;
    }
    return signs;
}

} // namespace qkvm::core::json
