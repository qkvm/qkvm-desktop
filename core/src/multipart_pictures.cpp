#include <qkvm/core/multipart_pictures.h>

#include <algorithm>
#include <utility>

namespace qkvm::core {

namespace {

// A Content-Type longer than this names no stream a KVM has reason to send.
constexpr qsizetype longestKind = 1024;
// The longest word RFC 2046 lets stand between the parts.
constexpr qsizetype longestWord = 70;
// The room a picture begins with. Most are smaller than that, and are then had in one piece.
constexpr qsizetype firstRoom = qsizetype{256} * 1024;

bool isBlank(char sign)
{
    return sign == ' ' || sign == '\t';
}

bool isLetterOrDigit(char sign)
{
    return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') || (sign >= 'A' && sign <= 'Z');
}

// What RFC 2046 lets the word be made of, the blank aside.
bool isOfAWord(char sign)
{
    static constexpr QByteArrayView others("'()+_,-./:=?");
    return isLetterOrDigit(sign) || others.contains(sign);
}

// What the name of a parameter is made of (RFC 7230's token).
bool isOfAName(char sign)
{
    static constexpr QByteArrayView others("!#$%&'*+-.^_`|~");
    return isLetterOrDigit(sign) || others.contains(sign);
}

bool isAWord(QByteArrayView word, bool blanksToo)
{
    if (word.isEmpty() || word.size() > longestWord || word.back() == ' ') {
        return false;
    }
    return std::ranges::all_of(word, [blanksToo](char sign) { return isOfAWord(sign) || (blanksToo && sign == ' '); });
}

} // namespace

std::optional<QByteArray> boundaryFrom(QByteArrayView contentType)
{
    static constexpr QByteArrayView kind("multipart/x-mixed-replace");
    const qsizetype size = contentType.size();
    if (size > longestKind) {
        return std::nullopt;
    }
    qsizetype at = 0;
    const auto passBlanks = [&] {
        while (at < size && isBlank(contentType[at])) {
            ++at;
        }
    };
    passBlanks();
    if (size - at < kind.size() || contentType.sliced(at, kind.size()).compare(kind, Qt::CaseInsensitive) != 0) {
        return std::nullopt;
    }
    at += kind.size();
    // What follows the kind: ";", a name, "=", and what it is, in quotes or without, any number
    // of times. All of it has to read as that, also what is not the word.
    std::optional<QByteArray> word;
    for (;;) {
        passBlanks();
        if (at == size) {
            return word;
        }
        if (contentType[at] != ';') {
            return std::nullopt;
        }
        ++at;
        passBlanks();
        const qsizetype nameAt = at;
        while (at < size && isOfAName(contentType[at])) {
            ++at;
        }
        const QByteArrayView name = contentType.sliced(nameAt, at - nameAt);
        if (name.isEmpty() || at == size || contentType[at] != '=') {
            return std::nullopt;
        }
        ++at;
        QByteArray value;
        const bool quoted = at < size && contentType[at] == '"';
        if (quoted) {
            ++at;
            for (;;) {
                if (at == size) {
                    return std::nullopt; // the quotes are never closed
                }
                char sign = contentType[at++];
                if (sign == '"') {
                    break;
                }
                if (sign == '\\') {
                    if (at == size) {
                        return std::nullopt;
                    }
                    sign = contentType[at++];
                }
                value.append(sign);
            }
        } else {
            while (at < size && contentType[at] != ';' && !isBlank(contentType[at])) {
                value.append(contentType[at++]);
            }
        }
        if (name.compare("boundary", Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (word || !isAWord(value, quoted)) {
            return std::nullopt; // named twice, or no word
        }
        word = value;
    }
}

MultipartPictures::MultipartPictures(const QByteArray& boundary) : MultipartPictures(boundary, Limits{}) {}

MultipartPictures::MultipartPictures(const QByteArray& boundary, Limits limits) : m_dashes("--" + boundary)
{
    // A limit is made smaller for a test, and never larger than the program's own.
    m_limits.headerBytes = std::clamp<qsizetype>(limits.headerBytes, 0, Limits{}.headerBytes);
    m_limits.pictureBytes = std::clamp<qsizetype>(limits.pictureBytes, 0, Limits{}.pictureBytes);
    if (!isAWord(boundary, true)) {
        m_end = End::NoBoundary;
    }
}

qsizetype MultipartPictures::feed(QByteArrayView bytes)
{
    qsizetype taken = 0;
    while (taken < bytes.size() && m_end == End::None && !m_ready) {
        if (m_at == At::Body) {
            taken += body(bytes.sliced(taken));
        } else {
            step(bytes[taken]);
            ++taken;
        }
    }
    return taken;
}

std::optional<QByteArray> MultipartPictures::next()
{
    return std::exchange(m_ready, std::nullopt);
}

qsizetype MultipartPictures::room() const
{
    return m_line.capacity() + m_picture.capacity() + (m_ready ? m_ready->capacity() : 0);
}

void MultipartPictures::step(char byte)
{
    switch (m_at) {
    case At::LineStart:
        if (byte == m_dashes.at(m_matched)) {
            if (++m_matched == m_dashes.size()) {
                m_at = At::Tail;
                m_tail = 0;
            }
            return;
        }
        // Not the boundary: a line to pass over, with what looked like the beginning of one.
        m_at = At::Passing;
        m_cr = byte == '\r';
        passedOver(m_matched + 1);
        return;
    case At::Passing:
        if (m_cr && byte == '\n') {
            m_at = At::LineStart;
            m_matched = 0;
        }
        m_cr = byte == '\r';
        passedOver(1);
        return;
    case At::Tail:
        if (m_tail == 0 && (byte == '\r' || byte == '-')) {
            m_tail = byte == '\r' ? 1 : 2;
            return;
        }
        if (m_tail == 1 && byte == '\n') {
            // A part begins.
            m_first = false;
            m_at = At::Headers;
            m_cr = false;
            m_headerBytes = 0;
            m_stated = -1;
            m_line.resize(0);
            return;
        }
        if (m_tail == 2 && byte == '-') {
            over(End::Closed);
            return;
        }
        if (m_tail == 1 || byte == '\n') {
            over(End::BadLineEnd);
            return;
        }
        if (!m_first) {
            over(End::WrongBoundary);
            return;
        }
        // Before the first boundary a line may begin as it does and be another.
        m_at = At::Passing;
        m_cr = byte == '\r';
        passedOver(m_dashes.size() + m_tail / 2 + 1);
        return;
    case At::Headers:
        if (++m_headerBytes > m_limits.headerBytes) {
            over(End::HeadersTooLong);
        } else if (m_cr) {
            if (byte == '\n') {
                m_cr = false;
                headerLine();
            } else {
                over(End::BadLineEnd);
            }
        } else if (byte == '\r') {
            m_cr = true;
        } else if (byte == '\n') {
            over(End::BadLineEnd);
        } else {
            m_line.append(byte);
        }
        return;
    case At::AfterBody: {
        // The line end behind the picture, and "--<word>" after it.
        const char expected = m_matched == 0 ? '\r' : m_matched == 1 ? '\n' : m_dashes.at(m_matched - 2);
        if (byte != expected) {
            const bool inTheLineEnd = m_matched == 1 || (m_matched == 0 && byte == '\n');
            over(inTheLineEnd ? End::BadLineEnd : End::WrongBoundary);
        } else if (++m_matched == m_dashes.size() + 2) {
            m_at = At::Tail;
            m_tail = 0;
        }
        return;
    }
    case At::Body:
        return; // taken in pieces, by body()
    }
}

void MultipartPictures::passedOver(qsizetype bytes)
{
    m_passed += bytes;
    if (m_passed > m_limits.headerBytes) {
        over(End::NoBoundary);
    }
}

void MultipartPictures::headerLine()
{
    if (m_line.isEmpty()) {
        // The empty line: the picture follows.
        if (m_stated < 0) {
            over(End::NoLength);
            return;
        }
        m_at = At::Body;
        m_left = m_stated;
        m_picture.reserve(std::min(m_stated, firstRoom));
        return;
    }
    const QByteArrayView line(m_line);
    const qsizetype colon = line.indexOf(':');
    // Every other line is passed over, whatever it says.
    if (colon >= 0 && line.first(colon).compare("content-length", Qt::CaseInsensitive) == 0) {
        QByteArrayView said = line.sliced(colon + 1);
        while (!said.isEmpty() && isBlank(said.front())) {
            said = said.sliced(1);
        }
        while (!said.isEmpty() && isBlank(said.back())) {
            said.chop(1);
        }
        const bool isNumber =
            !said.isEmpty() && std::ranges::all_of(said, [](char sign) { return sign >= '0' && sign <= '9'; });
        if (m_stated >= 0 || !isNumber) {
            over(End::BadLength);
            return;
        }
        // Counted only as far as it could still be taken: no row of digits runs it over.
        qsizetype stated = 0;
        for (const char digit : said) {
            stated = std::min(stated * 10 + (digit - '0'), m_limits.pictureBytes + 1);
        }
        if (stated == 0 || stated > m_limits.pictureBytes) {
            over(stated == 0 ? End::BadLength : End::TooLarge);
            return;
        }
        m_stated = stated;
    }
    m_line.resize(0);
}

qsizetype MultipartPictures::body(QByteArrayView bytes)
{
    const qsizetype taken = std::min(m_left, bytes.size());
    const qsizetype needed = m_picture.size() + taken;
    if (needed > m_picture.capacity()) {
        // Room for what has come and as much again, and never for more than the part stated.
        m_picture.reserve(std::min(m_stated, std::max(needed, 2 * m_picture.capacity())));
    }
    m_picture.append(bytes.first(taken));
    m_left -= taken;
    if (m_left == 0) {
        m_ready = std::exchange(m_picture, {});
        m_at = At::AfterBody;
        m_matched = 0;
    }
    return taken;
}

void MultipartPictures::over(End why)
{
    m_end = why;
    // Nothing of the part under way is kept.
    m_line = {};
    m_picture = {};
}

} // namespace qkvm::core
