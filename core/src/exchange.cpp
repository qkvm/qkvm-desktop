#include <qkvm/core/exchange.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QLocale>
#include <QSet>
#include <QUrl>

#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/sealing.h>
#include <qkvm/core/strict_json.h>

#include "windows_certificate.h"

using namespace Qt::StringLiterals;

namespace qkvm::core::exchange {

namespace {

// --- the server's words -------------------------------------------------------------------------

// The sentences here are the server's (../app/qkvm/exchange.py), word for word, and are
// translated where the page's are.
QString words(const char* text)
{
    return QCoreApplication::translate("Exchange", text);
}

base::Error refusal(const char* code, const char* sentence)
{
    return {code, words(sentence).toStdString()};
}

base::Error notOne()
{
    return refusal(notAFile, "This is not a file that qKVM made");
}

base::Error largerThanAny()
{
    return refusal(tooLarge, "The file is larger than anything qKVM writes");
}

base::Error notThePassphrase()
{
    return refusal(wrongPassphrase, "That is not the passphrase this file was made with");
}

base::Error doesNotOpen()
{
    return refusal(damaged, "The file is damaged: what is in it does not open with its own key");
}

base::Error holdsMoreThanAFile()
{
    return refusal(tooLarge, "This wall holds more than a file of qKVM's can. Nothing was written.");
}

constexpr QLatin1StringView formatOfAFile{"qkvm-export"};
constexpr int versionOfAFile = 1;
// What a file's own key is sealed for, and what its content is sealed for (vault.py,
// exchange.py): neither opens as the other, nor as a device's password of a wall.
constexpr QByteArrayView sealedAsAKey{"kvm-wall data key"};
constexpr QByteArrayView sealedAsContent{"export 1"};
constexpr qsizetype saltBytesAsWritten = 16;
// When a file was made, in seconds since 1970: from nought to below this (the server's bound).
constexpr double latestMade = 1e11;

// --- texts, as the server's language has them ---------------------------------------------------

// A blank, to the server's language (its str.isspace): what it takes off the ends of a text,
// and what it cuts a text into words at. Not Qt's own idea of one, which leaves out four.
bool isBlank(QChar sign) noexcept
{
    const char16_t unit = sign.unicode();
    return (unit >= 0x09 && unit <= 0x0D) || (unit >= 0x1C && unit <= 0x20) || unit == 0x85 || unit == 0xA0 ||
           unit == 0x1680 || (unit >= 0x2000 && unit <= 0x200A) || unit == 0x2028 || unit == 0x2029 || unit == 0x202F ||
           unit == 0x205F || unit == 0x3000;
}

QStringView stripped(QStringView text) noexcept
{
    qsizetype from = 0;
    qsizetype to = text.size();
    while (from < to && isBlank(text.at(from))) {
        ++from;
    }
    while (to > from && isBlank(text.at(to - 1))) {
        --to;
    }
    return text.mid(from, to - from);
}

// How many signs a text has, as the server counts: one that takes two places here is one.
qsizetype signsOf(QStringView text) noexcept
{
    qsizetype signs = 0;
    for (const QChar unit : text) {
        signs += unit.isLowSurrogate() ? 0 : 1;
    }
    return signs;
}

// What ends a line or moves the cursor, to the server's rule for a name (a control sign).
bool isControl(char16_t unit) noexcept
{
    return unit < 0x20 || (unit >= 0x7F && unit <= 0x9F);
}

// What ends a line without being a control sign, and what turns round the direction of the
// text after it: with those a name could be made to read as another one.
bool endsOrTurnsALine(char16_t unit) noexcept
{
    return unit == 0x2028 || unit == 0x2029 || (unit >= 0x202A && unit <= 0x202E) || (unit >= 0x2066 && unit <= 0x2069);
}

// A text as one line that reads as it is written, or nothing where it cannot be made one: the
// server's rule for a name (as_a_line in store.py). A tab is a blank; a blank of no width is
// nothing; what ends a line, moves the cursor or turns the writing round makes it no line.
std::optional<QString> asALine(QStringView text)
{
    constexpr std::array<char16_t, 3> notKept{0x200B, 0x2060, 0xFEFF};
    QString line;
    line.reserve(text.size());
    for (qsizetype at = 0; at < text.size(); ++at) {
        const QChar sign = text.at(at);
        const char16_t unit = sign.unicode();
        // Half a character: no file that was read has one, and no line does.
        const bool firstHalfAlone =
            sign.isHighSurrogate() && (at + 1 >= text.size() || !text.at(at + 1).isLowSurrogate());
        const bool secondHalfAlone = sign.isLowSurrogate() && (at == 0 || !text.at(at - 1).isHighSurrogate());
        if (firstHalfAlone || secondHalfAlone) {
            return std::nullopt;
        }
        if (unit == u'\t') {
            line += u' ';
        } else if (std::ranges::find(notKept, unit) != notKept.end()) {
            continue;
        } else if (isControl(unit) || endsOrTurnsALine(unit)) {
            return std::nullopt;
        } else {
            line += sign;
        }
    }
    return line;
}

// A value as a line of at most so many signs, with the blanks around it gone; nothing where it
// is not one (the server's _short).
std::optional<QString> shortLine(const json::Value* value, qsizetype most)
{
    if (value == nullptr || !value->isText()) {
        return std::nullopt;
    }
    const std::optional<QString> line = asALine(stripped(value->text));
    if (!line) {
        return std::nullopt;
    }
    const QStringView kept = stripped(*line);
    return signsOf(kept) > most ? std::nullopt : std::optional<QString>(kept.toString());
}

// The same where an entry may leave the field out, and then has this in it.
std::optional<QString> shortLineOr(const json::Value* value, qsizetype most, const QString& otherwise)
{
    return value == nullptr ? std::optional<QString>(otherwise) : shortLine(value, most);
}

bool flagOr(const json::Value* value, bool otherwise) noexcept
{
    return value != nullptr && value->isFlag() ? value->flag : otherwise;
}

bool isTextOf(const json::Value* value, QLatin1StringView text) noexcept
{
    return value != nullptr && value->isText() && value->text == text;
}

// One of the twenty-six in its small form, and every other sign as it is. Not Qt's own way of
// making small letters or of comparing without regard to them: that knows letters of other
// alphabets which become one of these (the long s, U+017F, becomes "s", and the sign for
// Kelvin, U+212A, "k"), and to the server's reader of addresses a way of speaking is made of
// the twenty-six alone: "http", a long s and "://" is none to it. (kvm_probe.cpp has the same,
// for the same reason.)
constexpr char16_t inSmall(char16_t sign) noexcept
{
    return sign >= u'A' && sign <= u'Z' ? static_cast<char16_t>(sign - u'A' + u'a') : sign;
}

// Whether a text begins with these plain signs, in letters small or large: compared sign for
// sign, and the twenty-six alone have a large form.
bool startsWithInAnyCase(QStringView text, QLatin1StringView start) noexcept
{
    if (text.size() < start.size()) {
        return false;
    }
    for (qsizetype at = 0; at < start.size(); ++at) {
        if (inSmall(text.at(at).unicode()) != inSmall(start.at(at).unicode())) {
            return false;
        }
    }
    return true;
}

bool isADigit(QChar sign) noexcept
{
    return sign >= u'0' && sign <= u'9';
}

// Four numbers with dots between them, each written the plain way: one to three digits, no
// nought in front, none above 255 (as the server's language reads an address of that kind).
bool isFourNumbers(QStringView host)
{
    constexpr qsizetype numbers = 4;
    constexpr qsizetype mostDigits = 3;
    constexpr int largest = 255;
    const QList<QStringView> parts = host.split(u'.');
    return parts.size() == numbers && std::ranges::all_of(parts, [](QStringView part) {
               if (part.isEmpty() || part.size() > mostDigits || !std::ranges::all_of(part, isADigit) ||
                   (part.size() > 1 && part.front() == u'0')) {
                   return false;
               }
               int number = 0;
               for (const QChar digit : part) {
                   number = number * 10 + (digit.unicode() - u'0');
               }
               return number <= largest;
           });
}

// Whether a host is digits, any one sign, digits, any one sign, digits, any one sign, digits:
// what the library a QKVM server connects with takes for four numbers, and then asks whether
// they are. (As it is on the PC this was written on, that library means a dot by "any one
// sign". Its versions before that are remembered to have had it as it is here, by an oversight
// of theirs: "1-2-3-4", "1a2b3c4" and "1234567" were of that shape too, and were refused as
// numbers that are no address. Which a server has is not known here. So for where alerts go
// this is the one that refuses more; and a device at such a name, which this program speaks
// to, is written with a line beside the file.)
bool looksLikeFourNumbers(QStringView host) noexcept
{
    // Every sign that is no digit is one of the three in between; and the digits around them
    // can be cut into four runs in all, with a digit to stand in between wherever a run is cut.
    qsizetype between = 0;
    qsizetype runs = 0;
    qsizetype digits = 0;
    for (const QChar sign : host) {
        if (isADigit(sign)) {
            ++digits;
            continue;
        }
        if (digits == 0) {
            return false;
        }
        runs += (digits + 1) / 2;
        digits = 0;
        ++between;
    }
    constexpr qsizetype numbers = 4;
    return digits > 0 && between < numbers && runs + (digits + 1) / 2 >= numbers;
}

// --- a name, as a sentence shows it -------------------------------------------------------------

// The marks that say which way the writing runs and are no turn of it to the rule for a line
// (which lets them pass, as the server's does): they move what stands beside a name.
constexpr bool saysWhichWayTheWritingRuns(char32_t sign) noexcept
{
    return sign == 0x200E || sign == 0x200F || sign == 0x061C;
}

// A double quote, and what looks like one: the list exchange.h has at nameSetOff().
constexpr bool looksLikeADoubleQuote(char32_t sign) noexcept
{
    return sign == U'"' || (sign >= 0x201C && sign <= 0x201F) || sign == 0x2033 || sign == 0x2036 || sign == 0x02BA ||
           sign == 0x02DD || sign == 0x02EE || sign == 0x3003 || (sign >= 0x301D && sign <= 0x301F) || sign == 0xFF02;
}

// Whether there is nothing to see of a sign: a blank, a sign that prints nothing, a mark that
// stands on a letter (and so on nothing, where a name has nothing else), and the fillers that
// are letters or pictures by their kind and show nothing.
bool showsNothing(char32_t sign) noexcept
{
    constexpr std::array<char32_t, 5> fillers{0x115F, 0x1160, 0x2800, 0x3164, 0xFFA0};
    if (std::ranges::find(fillers, sign) != fillers.end()) {
        return true;
    }
    switch (QChar::category(sign)) {
    case QChar::Separator_Space:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Mark_NonSpacing:
    case QChar::Mark_Enclosing:
        return true;
    default:
        return false;
    }
}

// --- bytes written as text ----------------------------------------------------------------------

constexpr int notOfTheAlphabet = -1;

int sixBitsOf(char16_t sign) noexcept
{
    if (sign >= u'A' && sign <= u'Z') {
        return sign - u'A';
    }
    if (sign >= u'a' && sign <= u'z') {
        return sign - u'a' + 26;
    }
    if (sign >= u'0' && sign <= u'9') {
        return sign - u'0' + 52;
    }
    if (sign == u'+') {
        return 62;
    }
    return sign == u'/' ? 63 : notOfTheAlphabet;
}

// Bytes from base64, by the book: the sixty-four signs and nothing else, in fours, filled up
// with "=" at the end and nowhere else, and no bit set that belongs to no byte. (The server's
// reader passes over whatever is not of the alphabet. Here that is no base64.)
std::optional<QByteArray> fromBase64(QStringView text, qsizetype mostBytes)
{
    const qsizetype length = text.size();
    if (length % 4 != 0 || length / 4 > mostBytes / 3 + 1) {
        return std::nullopt;
    }
    QByteArray bytes;
    bytes.reserve(length / 4 * 3);
    for (qsizetype at = 0; at < length; at += 4) {
        std::array<int, 4> six{};
        int filled = 0; // how many "=" stand at the end of this four
        for (std::size_t index = 0; index < six.size(); ++index) {
            const char16_t sign = text.at(at + static_cast<qsizetype>(index)).unicode();
            if (sign == u'=') {
                if (at + 4 != length || index < 2) {
                    return std::nullopt;
                }
                ++filled;
            } else {
                six.at(index) = sixBitsOf(sign);
                if (filled > 0 || six.at(index) == notOfTheAlphabet) {
                    return std::nullopt;
                }
            }
        }
        if ((filled == 2 && (six[1] & 0x0F) != 0) || (filled == 1 && (six[2] & 0x03) != 0)) {
            return std::nullopt;
        }
        const int group = (six[0] << 18) | (six[1] << 12) | (six[2] << 6) | six[3];
        bytes += static_cast<char>((group >> 16) & 0xFF);
        if (filled < 2) {
            bytes += static_cast<char>((group >> 8) & 0xFF);
        }
        if (filled < 1) {
            bytes += static_cast<char>(group & 0xFF);
        }
    }
    return bytes.size() > mostBytes ? std::nullopt : std::optional<QByteArray>(bytes);
}

std::optional<QByteArray> bytesOf(const json::Value* value, qsizetype mostBytes)
{
    return value != nullptr && value->isText() ? fromBase64(value->text, mostBytes) : std::nullopt;
}

// --- the lock, and what is sealed ---------------------------------------------------------------

// A number that is whole and no more than `most`. (The server makes a whole number of whatever
// its language will: of a text of digits, of a yes, of 4096.9. Here it is a number or nothing.)
std::optional<quint64> wholeNumber(const json::Value* value, quint64 most) noexcept
{
    if (value == nullptr || !value->isNumber()) {
        return std::nullopt;
    }
    const double number = value->number;
    if (!(number >= 0) || number > static_cast<double>(most) || number != std::floor(number)) {
        return std::nullopt;
    }
    return static_cast<quint64>(number);
}

struct SealedBytes {
    QByteArray nonce;
    QByteArray data;
};

// Something sealed, as a file writes it: {"nonce": ..., "data": ...}, each base64 by the book,
// the nonce of twelve bytes and the data of a tag's sixteen at the least.
std::optional<SealedBytes> sealedFrom(const json::Value* said)
{
    if (said == nullptr || !said->isObject()) {
        return std::nullopt;
    }
    std::optional<QByteArray> nonce = bytesOf(said->find(u"nonce"), sealing::nonceBytes);
    std::optional<QByteArray> data = bytesOf(said->find(u"data"), largestSealed);
    if (!nonce || !data || nonce->size() != sealing::nonceBytes || data->size() < sealing::tagBytes) {
        return std::nullopt;
    }
    return SealedBytes{std::move(*nonce), std::move(*data)};
}

struct Lock {
    scrypt::Cost cost;
    QByteArray salt;
    SealedBytes key;
};

// The lock of a sealed file, where it is one that is worked on at all: nothing is, of which the
// passphrase's work would be more than a file may ask for, and nothing that is not written as
// the server writes one.
std::optional<Lock> lockFrom(const json::Value* said)
{
    if (said == nullptr || !said->isObject()) {
        return std::nullopt;
    }
    const json::Value* kdf = said->find(u"kdf");
    if (kdf == nullptr || !kdf->isObject() || !isTextOf(kdf->find(u"name"), "scrypt"_L1)) {
        return std::nullopt;
    }
    // (Far above what a file may ask for, and far below where a number is no longer exact.)
    constexpr quint64 largestLookedAt = quint64{1} << 30;
    const std::optional<quint64> n = wholeNumber(kdf->find(u"n"), largestLookedAt);
    const std::optional<quint64> r = wholeNumber(kdf->find(u"r"), largestLookedAt);
    const std::optional<quint64> p = wholeNumber(kdf->find(u"p"), largestLookedAt);
    if (!n || !r || !p) {
        return std::nullopt;
    }
    const scrypt::Cost cost{*n, static_cast<quint32>(*r), static_cast<quint32>(*p)};
    if (!mayBeAsked(cost)) {
        return std::nullopt;
    }
    std::optional<QByteArray> salt = bytesOf(kdf->find(u"salt"), longestSalt);
    std::optional<SealedBytes> key = sealedFrom(said->find(u"key"));
    if (!salt || !key) {
        return std::nullopt;
    }
    return Lock{cost, std::move(*salt), std::move(*key)};
}

// --- a certificate ------------------------------------------------------------------------------

constexpr int deepestInACertificate = 24;
constexpr int mostBytesOfALength = 4;

// How many values lie in `der`, one after the other and nothing left over; nothing where that
// is not so, or where a length, of one of them or of anything inside one, is written in more
// bytes than it takes. The server's rule (_parts in exchange.py), step for step: a length has
// one spelling, and a certificate written with another is not the one a device presents.
std::optional<int> partsOf(QByteArrayView der, int depth)
{
    if (depth > deepestInACertificate) {
        return std::nullopt;
    }
    const auto byteAt = [&der](qsizetype where) { return static_cast<uchar>(der.at(where)); };
    qsizetype at = 0;
    int parts = 0;
    while (at < der.size()) {
        // (A tag of more than one byte: nothing in a certificate has one.)
        if (at + 2 > der.size() || (byteAt(at) & 0x1F) == 0x1F) {
            return std::nullopt;
        }
        const bool constructed = (byteAt(at) & 0x20) != 0;
        const uchar first = byteAt(at + 1);
        at += 2;
        qsizetype length = first;
        if (first >= 0x80) {
            const int count = first & 0x7F;
            if (count < 1 || count > mostBytesOfALength || at + count > der.size()) {
                return std::nullopt;
            }
            length = 0;
            for (int index = 0; index < count; ++index) {
                length = (length << 8) | byteAt(at + index);
            }
            if (byteAt(at) == 0 || length < 0x80) {
                return std::nullopt;
            }
            at += count;
        }
        if (length > der.size() - at) {
            return std::nullopt;
        }
        if (constructed && !partsOf(der.sliced(at, length), depth + 1)) {
            return std::nullopt;
        }
        at += length;
        ++parts;
    }
    return parts;
}

struct Certificate {
    QString pem;
    QByteArray sha256;
};

constexpr QLatin1StringView headOfACertificate{"-----BEGIN CERTIFICATE-----"};
constexpr QLatin1StringView footOfACertificate{"-----END CERTIFICATE-----"};
constexpr qsizetype signsInALineOfPem = 64;

// The one measure of how long a certificate may be: the signs of its text as a file holds it,
// 20,000 at the most. It is the server's (`len(value) > _MOST_CERTIFICATE` in _certificate, of
// the text as it stands in the file it reads, before a blank is taken off it), and it is asked
// of the same thing wherever it is asked here: of the text that was read, and of the text that
// is kept (asKept: in lines of 64, which is longer by a line's end for every 64 signs), since
// that is the text of the next file, where a server measures it again. Asked of what was read
// alone, a certificate of some 14,700 to 14,960 bytes that a file had in ONE line was taken,
// and was over the measure as it was kept: no file could be written of a wall with its device.
bool fitsAFile(QStringView certificate) noexcept
{
    return signsOf(certificate) <= longestCertificate;
}

// The certificate a device is known by, as a file names it: kept as the server keeps one, and
// its SHA-256, which is what a KVM is known by on this PC. Nothing: it is none.
//
// Not the server's rule to the letter, which is that of two libraries that are not here; where
// this could differ from it, this takes less:
// * One certificate between its two lines, and nothing before, behind or between: the server
//   takes the first of several and drops the rest.
// * Its bytes in base64 by the book, with nothing in between but blanks and ends of lines.
// * One piece by the server's own rule (partsOf), which the server asks only of what its first
//   reader refuses.
// * Whether it is a certificate at all is asked of Windows, which is who reads what a KVM
//   shows; the server asks its own two readers. Those could take one that Windows does not, and
//   the other way round. Whatever is taken here is only ever compared with what a KVM shows, by
//   its SHA-256: one that no KVM shows lets nothing in.
//
// This is all of it but the asking of Windows: the bytes between the two lines, where they
// are one piece that Windows may be asked about.
std::optional<QByteArray> bytesOfACertificate(QStringView written)
{
    if (!fitsAFile(written) || !std::ranges::all_of(written, [](QChar sign) { return sign.unicode() < 0x80; })) {
        return std::nullopt;
    }
    const QStringView text = stripped(written);
    if (!text.startsWith(headOfACertificate) || !text.endsWith(footOfACertificate) ||
        text.size() < headOfACertificate.size() + footOfACertificate.size()) {
        return std::nullopt;
    }
    QString base64;
    base64.reserve(text.size());
    for (const QChar sign :
         text.mid(headOfACertificate.size(), text.size() - headOfACertificate.size() - footOfACertificate.size())) {
        if (sign != u' ' && sign != u'\t' && sign != u'\r' && sign != u'\n') {
            base64 += sign;
        }
    }
    std::optional<QByteArray> der = fromBase64(base64, longestCertificate);
    constexpr qsizetype fewestBytes = 4;
    if (!der || der->size() < fewestBytes || static_cast<uchar>(der->at(0)) != 0x30 || partsOf(*der, 0) != 1) {
        return std::nullopt;
    }
    return der;
}

// A certificate as the server writes one: its bytes in base64, in lines of 64.
Certificate asKept(const QByteArray& der)
{
    const QString all = QString::fromLatin1(der.toBase64());
    QString pem(headOfACertificate);
    pem += u'\n';
    for (qsizetype at = 0; at < all.size(); at += signsInALineOfPem) {
        pem += QStringView(all).mid(at, signsInALineOfPem);
        pem += u'\n';
    }
    pem += footOfACertificate;
    pem += u'\n';
    return Certificate{pem, QCryptographicHash::hash(der, QCryptographicHash::Sha256)};
}

// The certificates of one file that were looked at, by how they were written: one that many
// devices share (a maker's, on every board of a rack) is read once.
//
// And no more than so many of one file are asked of Windows in vain. The server has a limit of
// the kind (MOST_ODD, _Odd in exchange.py): the different certificates of one file that its
// first reader, which goes by the book, refuses are asked of its second, which is slow; 256 of
// them, and after that every such one is none. Which those are cannot be said here: Windows is
// the one reader there is, and what Windows takes is taken whether the server would have had
// to ask twice or not. What is done here is the nearest thing that can be: once Windows has
// refused so many different ones of a file, a certificate not seen before is none, unasked, in
// the server's sentence. From there on this takes less than the server (which still takes what
// its first reader takes). Before that it may take more: of a file with more than 256 different
// certificates that are odd to the server and good to Windows, the server takes the first 256,
// and this all of them.
//
// A certificate that is one, and is longer than a file holds once it is kept (fitsAFile), is
// not taken either, and is said as that: the server takes it, and then writes a file that its
// own reader leaves the device out of. Here less is taken.
class Certificates {
public:
    struct Looked {
        std::optional<Certificate> certificate; // nothing: none is taken
        bool longerThanAFileAsKept = false;     // and why: it is one, and too long as it is kept
    };

    // The certificate a text is, or nothing where none is taken.
    Looked of(const QString& written)
    {
        QHash<QString, Looked>::const_iterator known = m_looked.constFind(written);
        if (known == m_looked.constEnd()) {
            known = m_looked.insert(written, read(written));
        }
        return known.value();
    }

private:
    Looked read(QStringView written)
    {
        const std::optional<QByteArray> der = bytesOfACertificate(written);
        if (!der || m_refusedByWindows >= mostOddCertificates) {
            return {};
        }
        if (!windowsReadsAsCertificate(*der)) {
            ++m_refusedByWindows;
            return {};
        }
        Certificate kept = asKept(*der);
        if (!fitsAFile(kept.pem)) {
            return {std::nullopt, true};
        }
        return {std::move(kept), false};
    }

    QHash<QString, Looked> m_looked;
    int m_refusedByWindows = 0;
};

// --- a device -----------------------------------------------------------------------------------

// How a name in letters beyond ASCII begins where it is written in its short form (punycode).
constexpr QLatin1StringView punycodeMark{"xn--"};

// Where an address of the web begins that somebody wrote: behind "http://" or "https://", in
// letters small or large (the twenty-six, and no sign that only counts as one of them); -1
// where it begins with neither.
qsizetype behindTheScheme(QStringView text) noexcept
{
    if (startsWithInAnyCase(text, "https://"_L1)) {
        return 8;
    }
    return startsWithInAnyCase(text, "http://"_L1) ? 7 : -1;
}

// The host and port of such an address, with a name and password where it has them: as far as
// where a path, a question mark or a cross begins.
QStringView authorityOf(QStringView text, qsizetype behind) noexcept
{
    const QStringView rest = text.mid(behind);
    const auto ends = [](QChar sign) { return sign == u'/' || sign == u'?' || sign == u'#'; };
    return rest.left(std::ranges::find_if(rest, ends) - rest.begin());
}

// Whether a text is made of nothing but the signs from behind the blank to the tilde.
bool isPlain(QStringView text) noexcept
{
    return std::ranges::all_of(text, [](QChar sign) { return sign.unicode() > 0x20 && sign.unicode() < 0x7F; });
}

// Whether nothing in a text ends a line, moves the cursor or turns the writing round.
bool staysALine(QStringView text) noexcept
{
    return std::ranges::none_of(
        text, [](QChar sign) { return isControl(sign.unicode()) || endsOrTurnsALine(sign.unicode()); });
}

// Whether what stands before a link's path, with a bracket in it, is a host in brackets: one
// pair of brackets, at its very beginning; in them numbers of the new kind by the program's
// own rule for them (kvm::addressFrom: groups of up to four digits to the base of sixteen, one
// "::" at the most, four plain numbers at the end where the book allows them, and no zone);
// and behind them nothing, or a colon and whatever is written for a port, which the server
// does not look at for a link either.
//
// By a rule that is written in this program, and not by what Qt reads out of brackets. Qt
// takes "[0x1::1]" and "[::+1]" (core/tests/test_kvm_probe.cpp pins that), which the server's
// reader refuses; and Qt refuses a port no computer has behind them, which the server's reader
// does not look at. Asked of the server's own reader as it ran on Python 3.14.4
// (build/mine/x21_odd.py, and the rows of core/tests/test_exchange.cpp): what this takes, it
// takes; and it takes two things more, which are left out here: a zone behind the numbers
// ("%eth0"), and the other thing the books let brackets hold ("[v1.x]"). Something before the
// brackets or behind them it refuses, as this does. (Its versions before that one are
// remembered to have looked only at what stands between the first pair of brackets.)
bool isAHostInBrackets(QStringView authority)
{
    const qsizetype close = authority.indexOf(u']');
    if (!authority.startsWith(u'[') || close < 0 || authority.count(u'[') != 1 || authority.count(u']') != 1) {
        return false;
    }
    const QStringView behindThem = authority.mid(close + 1);
    if (!behindThem.isEmpty() && behindThem.front() != u':') {
        return false;
    }
    return kvm::addressFrom(u"https://"_s + authority.left(close + 1).toString()).ok();
}

// Whether the server's reader of addresses lets pass what stands before a link's path, where
// that has signs beyond ASCII in it (_checknetloc in its language's urllib): it refuses one in
// which such a sign, made the plain signs it counts as, is one an address is cut at (the stroke
// of full width, the sign for "care of").
//
// Asked of Qt's tables of the signs, which may be of another year than the server's. A sign
// they do not know is taken for one the server refuses; one that Qt knows and the server does
// not passes there, and here only where it counts as nothing an address is cut at.
bool passesAsTheServersReaderHasIt(QStringView authority)
{
    if (std::ranges::all_of(authority, [](QChar sign) { return sign.unicode() < 0x80; })) {
        return true;
    }
    QString looked;
    looked.reserve(authority.size());
    for (const QChar sign : authority) {
        // (Signs that are in it already are not what is looked for.)
        if (sign != u'@' && sign != u':' && sign != u'#' && sign != u'?') {
            looked += sign;
        }
    }
    const QList<uint> signs = looked.toUcs4();
    if (std::ranges::any_of(signs, [](uint sign) { return QChar::category(sign) == QChar::Other_NotAssigned; })) {
        return false;
    }
    const QString plain = looked.normalized(QString::NormalizationForm_KC);
    constexpr QLatin1StringView cutAt{"/?#@:"};
    return plain == looked || std::ranges::none_of(plain, [cutAt](QChar sign) { return cutAt.contains(sign); });
}

// What is made of where "Open console" goes.
struct ConsoleLink {
    // Why the device it belongs to is left out, in the server's words; nullptr: it is not.
    const char* deviceRefused = nullptr;
    // A server leaves the device out for it too, for certain: by its own rule (a name or a
    // password in the link; nothing that begins as an address of the web, or nothing where a
    // host stands), and not by what its language's reader of addresses makes of a host in one
    // version or another.
    bool aServerRefusesItForCertain = false;
    QString link; // as it is kept: the text with the blanks around it gone; empty: none
};

// Where "Open console" goes, as a file names it.
//
// A device is left out over its link where the server leaves it out: the server's rule
// (console_link in exchange.py) is that the link begins with http:// or https://, has
// something where a host stands, and has no name or password in it; its language's reader of
// addresses, which that rule asks, drops a tab and a line's end wherever they stand before it
// reads, and refuses a few things in other words than the rule's own (brackets that are not a
// pair around numbers of the new kind, and the signs passesAsTheServersReaderHasIt looks for).
//
// Where that reader's word cannot be said here, or is one thing in one version of it and
// another in the next, the device is left out as well, and there this takes less than the
// server: a bracket anywhere but around a host at the very beginning, brackets around a zone
// or around the other thing the books let them hold (isAHostInBrackets), and a sign that
// prints nothing in front of the link. Nothing is taken that a server might refuse.
//
// The link of a device that is taken is kept as it is written, whatever is in it: a server may
// be handed the file again, and has it back as it was, and a wall that holds such a device can
// be written. That is no promise that the link can be opened, or shown: what is kept is a text
// (LinkAsWritten), and the one way from it to an address is consoleAddress(), which takes far
// less. (A link with a line's end or a turn of the writing in it was not kept here once, and
// the device taken without it. Then the device could be read from a server's file and not be
// written into the next one.)
//
// For the writer the two kinds of no are two things (aServerRefusesItForCertain). A link that
// only this reader leaves a device out for is written as it was, with a line beside the file.
// One that a server refuses for certain stops the file: neither program would read the device
// back, and a link with a name and a password in it, written as it was, is a password in a
// file that may be a plain one.
ConsoleLink consoleLinkFrom(QStringView written)
{
    ConsoleLink made;
    const QStringView text = stripped(written);
    if (text.isEmpty()) {
        return made;
    }
    // As the server's reader of addresses sees it: without tabs and ends of lines.
    QString seen;
    seen.reserve(text.size());
    for (const QChar sign : text) {
        if (sign != u'\t' && sign != u'\r' && sign != u'\n') {
            seen += sign;
        }
    }
    const qsizetype behind = behindTheScheme(seen);
    const QStringView authority = behind < 0 ? QStringView() : authorityOf(seen, behind);
    if (authority.isEmpty()) {
        made.deviceRefused = "the console link must start with http:// or https://";
        // For certain where a server finds no such beginning either: its reader of addresses
        // drops control signs and blanks in front before it looks, so a link with one of those
        // in front is kept there, and only left out here.
        constexpr char16_t lastDroppedInFront = 0x20;
        qsizetype from = 0;
        while (from < seen.size() && seen.at(from).unicode() <= lastDroppedInFront) {
            ++from;
        }
        const QStringView asAServerSeesIt = QStringView(seen).mid(from);
        const qsizetype there = behindTheScheme(asAServerSeesIt);
        made.aServerRefusesItForCertain = there < 0 || authorityOf(asAServerSeesIt, there).isEmpty();
        return made;
    }
    if (authority.contains(u'@')) {
        // The link is handed to everybody who may look at the tile.
        made.deviceRefused = "the console link must not have a user name or password in it";
        made.aServerRefusesItForCertain = true;
        return made;
    }
    // With a bracket in it, what stands there is a host in brackets by the rule written here;
    // without one, it is what the server's own reader lets pass.
    const bool brackets = authority.contains(u'[') || authority.contains(u']');
    const bool theServerTakesIt =
        brackets ? isPlain(authority) && isAHostInBrackets(authority) : passesAsTheServersReaderHasIt(authority);
    if (!theServerTakesIt) {
        made.deviceRefused = "its console link is not one";
        return made;
    }
    made.link = text.toString();
    return made;
}

bool isOfAHostName(QChar sign) noexcept
{
    const char16_t letter = inSmall(sign.unicode());
    return (letter >= u'a' && letter <= u'z') || isADigit(sign) || sign == u'.' || sign == u'_' || sign == u'-';
}

// Whether what stands behind an address's colon is a port, or nothing: digits alone, however
// many noughts in front, and 1 to 65535.
bool isAPortOrNone(QStringView text) noexcept
{
    constexpr int largestPort = 65535;
    constexpr qsizetype mostDigits = 5;
    int port = 0;
    qsizetype counted = 0; // its digits, the noughts in front aside
    for (const QChar sign : text) {
        if (!isADigit(sign)) {
            return false;
        }
        port = port * 10 + (sign.unicode() - u'0');
        if (port != 0 && ++counted > mostDigits) {
            return false;
        }
    }
    return text.isEmpty() || (port >= 1 && port <= largestPort);
}

// Why a text that is no address of a device is none, in words, as far as that can be told
// here: asked in the order in which the program's own rule looks at an address
// (kvm::addressFrom, which says of one only that it is none).
//
// The address itself is in no sentence, of the reader or of the writer. The server's sentence
// shows it, and the first version of this one did unless it had an "@" in it; but a user name
// and a password are not always in front of one: "%40" is an "@" to whatever reads the address
// next, a password stands where a port does in "https://192.168.1.5:hunter2", and a token
// behind a question mark. Whether an address holds a secret cannot be told by its signs. So
// one that was refused is not shown at all: the sentence says which device it was, by its
// number and its name, and what was wrong with its address.
const char* whyNoAddress(const QString& written)
{
    if (written.size() > kvm::longestTypedAddress) {
        return "its address is longer than an address may be";
    }
    const QString text = written.trimmed();
    qsizetype behind = 0;
    if (kvm::schemeWasTyped(text)) {
        behind = behindTheScheme(text);
        if (behind < 0) {
            return "its address does not begin with http:// or https://";
        }
    }
    const QStringView authority = authorityOf(text, behind);
    if (authority.contains(u'@')) {
        return "its address has a user name or a password in it";
    }
    if (authority.startsWith(u'[')) {
        const qsizetype close = authority.indexOf(u']');
        return close >= 0 && kvm::addressFrom(u"https://"_s + authority.left(close + 1).toString()).ok()
                   ? "what stands behind the brackets in its address is no port"
                   : "what stands in brackets in its address is no address of numbers";
    }
    const qsizetype colon = authority.indexOf(u':');
    QStringView host = colon < 0 ? authority : authority.left(colon);
    if (!std::ranges::all_of(host, isOfAHostName)) {
        return "its address has a sign in it that no name of a host has";
    }
    while (host.endsWith(u'.')) {
        host.chop(1);
    }
    if (host.isEmpty()) {
        return "its address names no host";
    }
    const QStringView last = host.mid(host.lastIndexOf(u'.') + 1);
    if ((std::ranges::all_of(last, isADigit) || startsWithInAnyCase(last, "0x"_L1)) && !isFourNumbers(host)) {
        return "the numbers in its address are not an address";
    }
    if (colon >= 0 && !isAPortOrNone(authority.mid(colon + 1))) {
        return "what stands behind the colon in its address is no port";
    }
    // (What Qt, which would have to connect to it, will not have as a name: a part of it that
    // is longer than one may be or begins with a hyphen, a name in its "xn--" form that is none.)
    return "its address is not one this program can connect to";
}

// What a device says it is, as a driver gives it: two to twelve small letters, a colon, and
// sixteen to sixty-four digits to the base of sixteen. Not the marks a QKVM makes itself
// ("own:", "cert:"): those are worked out anew on the wall that takes a file in.
bool isAnIdent(QStringView text) noexcept
{
    const qsizetype colon = text.indexOf(u':');
    constexpr qsizetype fewestLetters = 2;
    constexpr qsizetype mostLetters = 12;
    constexpr qsizetype fewestDigits = 16;
    constexpr qsizetype mostDigits = 64;
    if (colon < fewestLetters || colon > mostLetters) {
        return false;
    }
    const QStringView family = text.left(colon);
    const QStringView digits = text.mid(colon + 1);
    return family != "own"_L1 && family != "cert"_L1 && digits.size() >= fewestDigits && digits.size() <= mostDigits &&
           std::ranges::all_of(family, [](QChar sign) { return sign >= u'a' && sign <= u'z'; }) &&
           std::ranges::all_of(
               digits, [](QChar sign) { return (sign >= u'0' && sign <= u'9') || (sign >= u'a' && sign <= u'f'); });
}

// What a server's card of 0.1.x kept as what the server is (the web's _SERVER_BEFORE): "server:" and the 32 digits
// of the number its computer gives for itself.
bool isAServersNumberOfBefore(QStringView text) noexcept
{
    constexpr QLatin1StringView mark{"server:"};
    constexpr qsizetype digits = 32;
    return text.size() == mark.size() + digits && text.startsWith(mark) &&
           std::ranges::all_of(text.mid(mark.size()), [](QChar sign) {
               return (sign >= u'0' && sign <= u'9') || (sign >= u'a' && sign <= u'f');
           });
}

constexpr std::array<QLatin1StringView, 3> kindsOfDevice{"kvmd"_L1, "jetkvm"_L1, "redfish"_L1};
constexpr std::array<QLatin1StringView, 3> waysOfChecking{"pinned"_L1, "system"_L1, "insecure"_L1};

bool isOneOf(const json::Value* value, std::span<const QLatin1StringView> those) noexcept
{
    return value != nullptr && value->isText() &&
           std::ranges::any_of(those, [value](QLatin1StringView one) { return value->text == one; });
}

// Who asks what a file holds: somebody who reads one; or the writer, which reads what it is
// about to write, to see that it is what a file holds.
enum class Asked { ByAReader, ByTheWriter };

// What is said of an entry of a file's list of devices that is left out.
struct SaidOfADevice {
    QString why;       // why it is left out, to whoever reads a file
    QString whose;     // its name, where the sentence is to say by it which device it was
    QString toAWriter; // the same to whoever writes one, of whose wall it would be an entry
};

// One device of a file, by every rule of the server's reader and in its order (_devices in
// exchange.py). Nothing where it breaks one, and `said.why` says which, in the server's words;
// or in this program's own for an address that is none, which the server's sentence shows.
//
// Whether an address is one is the program's own rule for an address (kvm::addressFrom, which
// the box that adds a KVM asks too): the server's (device_url in its store.py) as far as that
// can be said without the server's libraries. Where the three differ:
// * A tab or a line's end inside the address, and a sign that prints nothing in front of it,
//   are dropped by the server's reader and are no address here; brackets hold numbers of the
//   new kind and nothing else here; and the sign for Kelvin is a "k" to the server, which
//   makes small letters of a host by its language's rule, and no letter of a name here. Here
//   less is taken (kvm_probe.h).
// * A name written in letters beyond ASCII is none to all three: the server's rule and the
//   program's both ask for letters of the twenty-six, digits, dots, hyphens and underscores.
// * A name with a part in its "xn--" form (punycode) is made of those. It is one here where
//   it is one to Qt, which has the last word in the program's rule; and one to the server
//   where it is one to the library its lines are made with, which the server asks only where
//   the whole host begins with "xn--" (by IDNA 2008, as far as is remembered). Qt does NOT
//   read such a part under every ending. As the tests show it
//   (whatIsTakenOfANameWithAPartInItsXnForm... in core/tests/test_exchange.cpp), "taken"
//   being what the program's rule and this reader make of "https://" and the name:
//
//       the part           no ending   .lan     .example   .de       .com
//       xn--zzzzzz         taken       taken    taken      refused   refused
//       xn--mnchen-3ya     taken       taken    taken      taken     taken
//       xn--ls8h           taken       taken    taken      taken     taken
//
//   The first is none to Qt where Qt reads it (and was none to the server's own reader, which
//   was asked under ".de"), and it is refused only under the endings that Qt has a list of:
//   under any other ending, and with none, it is taken, so there Qt does not ask what such a
//   part spells out. The third is a picture, which the server's own reader did not take when
//   it was asked (under ".example": build/mine/x21_odd.py). So what is taken here
//   and what a server takes differ both ways, and by the ending: where a part is none to Qt
//   and is not the first part, the server takes it and this does not (less is taken); and a
//   name that Qt does not read, or reads by its own rule, is taken here and may be none to a
//   server (MORE is taken). This is one of the two places where this reader takes what a
//   server's may refuse, and it does because the program speaks to that device: what it
//   speaks to is read, and is written.
// * The other: a name of digits with single signs between them ("192-168-1-20", "1.2.3-4"),
//   which is a name here, since its last part is not digits alone, and which the library some
//   servers connect with reads as four numbers that are none (looksLikeFourNumbers).
// Whoever writes a file is told of a device at either kind of name beside the file, for every
// such name and by no guess at which of them a server refuses (aServerMayNotTake).
//   (A device at any name in the "xn--" form was left out here for one round, and then a wall
//   with one KVM at such a name could not be written at all. And for one round the line beside
//   the file was for those names of which a guess said that a server refuses them.)
std::optional<Device> deviceFrom(const json::Value& entry, Certificates& certificates, SaidOfADevice& said, Asked asked)
{
    const auto no = [&said](const char* sentence, const char* toAWriter = nullptr) {
        said.why = words(sentence);
        said.toAWriter = toAWriter != nullptr ? words(toAWriter) : said.why;
        return std::nullopt;
    };
    if (!entry.isObject()) {
        return no("not an entry");
    }
    const json::Value* url = entry.find(u"url");
    if (url == nullptr || !url->isText() || signsOf(url->text) > longestAddress || !url->text.contains("://"_L1)) {
        return no("it has no address", "its address is longer than a file holds");
    }
    const std::optional<QString> name = shortLineOr(entry.find(u"name"), longestName, QString());
    const base::Result<QUrl> where = kvm::addressFrom(url->text);
    if (!where) {
        // Not shown: what was wrong with it, and whose it was (whyNoAddress).
        said.whose = name.value_or(QString());
        return no(whyNoAddress(url->text));
    }
    const QString address = where.value().toString(QUrl::FullyEncoded);
    const std::optional<QString> user = shortLineOr(entry.find(u"username"), longestUser, u"admin"_s);
    const std::optional<QString> group = shortLineOr(entry.find(u"group"), longestGroupAsWritten, QString());
    if (!name || !user || !group) {
        return no("its name, user name or group is not a short line of text");
    }
    const json::Value* password = entry.find(u"password");
    if (password != nullptr && (!password->isText() || signsOf(password->text) > longestPassword)) {
        return no("its password is not one", "its password is longer than a file holds");
    }
    if (!isOneOf(entry.find(u"kind"), kindsOfDevice)) {
        return no("it is not a kind of device this version knows");
    }
    const json::Value* tls = entry.find(u"tls");
    if (tls != nullptr && !isOneOf(tls, waysOfChecking)) {
        return no("how its certificate is checked is not said in a way this version knows");
    }
    // A group's blanks, of whatever kind and however many in a row, are one plain blank.
    QString oneGroup;
    bool blankBefore = false;
    for (const QChar sign : std::as_const(*group)) {
        if (isBlank(sign)) {
            blankBefore = !oneGroup.isEmpty();
            continue;
        }
        if (std::exchange(blankBefore, false)) {
            oneGroup += u' ';
        }
        oneGroup += sign;
    }
    if (signsOf(oneGroup) > longestGroup) {
        return no("its group's name is too long");
    }
    // (The one rule for a link that keeps a file a file: a text of 255 signs at the most.)
    const json::Value* link = entry.find(u"open_url");
    if (link != nullptr && (!link->isText() || signsOf(link->text) > longestConsoleLink)) {
        return no("its console link is not one", "its console link is longer than a file holds");
    }
    // What its card checks before anything is sent to the machine: one that is no such number is not "none",
    // which would have the card take whatever answers first (the server's rule, in its order).
    const json::Value* machine = entry.find(u"machine");
    if (machine != nullptr && !(machine->isText() && (machine->text.isEmpty() || isAnIdent(machine->text)))) {
        return no("the number its machine gave for itself is not one");
    }

    Device device;
    device.address = address;
    device.name = name->isEmpty() ? nameOfADeviceWithoutOne(address) : *name;
    device.user = user->isEmpty() ? u"admin"_s : *user;
    device.password = password != nullptr ? password->text : QString();
    device.kind = entry.find(u"kind")->text;
    device.group = oneGroup;
    const bool encrypted = address.startsWith("https://"_L1);
    const QString checked = tls != nullptr ? tls->text : u"pinned"_s;
    // Over plain HTTP there is no certificate to check, whatever the file says about one.
    device.tls = encrypted ? checked : u"pinned"_s;
    const json::Value* pem = entry.find(u"cert_pem");
    const bool namesOne =
        pem != nullptr && pem->kind != json::Value::Kind::Null && !(pem->isText() && pem->text.isEmpty());
    if (encrypted && checked == "pinned"_L1 && namesOne) {
        if (!pem->isText()) {
            return no("its certificate is not one");
        }
        const Certificates::Looked looked = certificates.of(pem->text);
        if (!looked.certificate) {
            return looked.longerThanAFileAsKept ? no("its certificate is longer than a file holds once it is "
                                                     "written in lines of 64, as qKVM keeps one")
                                                : no("its certificate is not one");
        }
        device.certificatePem = looked.certificate->pem;
        device.certificate = looked.certificate->sha256;
    }
    if (link != nullptr) {
        ConsoleLink made = consoleLinkFrom(link->text);
        // To a reader, a link the server would leave the device out for costs the device. To
        // the writer, which reads what it is about to write, a link is a text as a file had it:
        // written as it was, whatever this program makes of it, with a line beside the file
        // where this reader would leave the device out for it (notInAFile). Unless a server
        // refuses it for certain: then no program reads the device back, and the file is not
        // written (and no link with a password in it goes into a file that may be a plain one).
        if (made.deviceRefused != nullptr && (asked == Asked::ByAReader || made.aServerRefusesItForCertain)) {
            return no(made.deviceRefused);
        }
        device.consoleLinkAsWritten =
            made.deviceRefused != nullptr ? LinkAsWritten(link->text) : LinkAsWritten(std::move(made.link));
    }
    device.alerts = flagOr(entry.find(u"alerts"), true);
    const json::Value* ident = entry.find(u"ident");
    device.ident = ident != nullptr && ident->isText() && isAnIdent(ident->text) ? ident->text : QString();
    device.machine = machine != nullptr ? machine->text : QString();
    // (A server's, from a file of 0.1.x, which kept it as what the server is: taken as its machine, as at an
    // upgrade; the web's machine_from_before. Only where the file has no machine at all.)
    if (machine == nullptr && device.kind == "redfish"_L1 && isAServersNumberOfBefore(device.ident)) {
        device.machine = device.ident;
    }
    const json::Value* keepBoth = entry.find(u"keep_both");
    device.keepBoth = keepBoth != nullptr && keepBoth->isFlag() && keepBoth->flag;
    return device;
}

// The first thing of what was to be written that a file does not hold: for the sentence that is
// said to whoever writes one, who has a wall and no file. (Whoever reads one is told of every
// entry, in `leftOut`.)
struct NotHeld {
    enum class Part { Nothing, Device, Action, Alerts, Wall };
    Part part = Part::Nothing;
    qsizetype index = 0; // of the device or the saved action, as the file has them
    QString why;
    // Whether it is this program alone that does not write it, and a server's file may hold it.
    bool onlyHere = false;
};

void noteNotHeld(NotHeld* notHeld, NotHeld::Part part, qsizetype index, const QString& why, bool onlyHere = false)
{
    if (notHeld != nullptr && notHeld->part == NotHeld::Part::Nothing) {
        *notHeld = NotHeld{part, index, why, onlyHere};
    }
}

// The sentence for a device of a file that is left out: by its number in the file, and by its
// name where it has one to show (nameSetOff: a name is the file's free text, and cannot end
// its own setting-off).
QString ofADeviceLeftOut(qsizetype index, const QString& name, const QString& why)
{
    const QString number = QString::number(index + 1);
    const QString shown = nameSetOff(name);
    return shown.isEmpty() ? words("Device %1: %2").arg(number, why)
                           : words("Device %1 (%2): %3").arg(number, shown, why);
}

QList<Device> devicesFrom(const json::Value& said, QStringList& leftOut, NotHeld* notHeld)
{
    QList<Device> devices;
    if (!said.isList()) {
        leftOut.append(words("The list of devices is not a list"));
        return devices;
    }
    if (std::cmp_greater(said.list.size(), mostDevices)) {
        leftOut.append(words("More than %1 devices: the ones after that").arg(mostDevices));
    }
    const qsizetype looked = std::min<qsizetype>(static_cast<qsizetype>(said.list.size()), mostDevices);
    Certificates certificates;
    QSet<QString> found;
    const Asked asked = notHeld != nullptr ? Asked::ByTheWriter : Asked::ByAReader;
    for (qsizetype index = 0; index < looked; ++index) {
        SaidOfADevice of;
        std::optional<Device> device =
            deviceFrom(said.list.at(static_cast<std::size_t>(index)), certificates, of, asked);
        if (!device) {
            leftOut.append(ofADeviceLeftOut(index, of.whose, of.why));
            noteNotHeld(notHeld, NotHeld::Part::Device, index, of.toAWriter);
        } else if (found.contains(device->address)) {
            leftOut.append(ofADeviceLeftOut(index, device->name, words("the file has that address twice")));
            noteNotHeld(notHeld, NotHeld::Part::Device, index,
                        words("another device of this wall is at the same address"));
        } else {
            found.insert(device->address);
            // Where it stood, as the sentences above count: for whoever shows it beside them.
            device->place = PlaceInAFile(index + 1);
            devices.append(std::move(*device));
        }
    }
    return devices;
}

// --- a saved action -----------------------------------------------------------------------------

// Up to eight key names as browsers and kvmd spell them, with commas between: a capital letter
// and one to fifteen letters or digits behind it (KEYS in kvm.py).
bool areKeys(QStringView keys)
{
    constexpr qsizetype mostKeys = 8;
    constexpr qsizetype shortestKey = 2;
    constexpr qsizetype longestKey = 16;
    const auto isLetterOrDigit = [](QChar sign) {
        return (sign >= u'A' && sign <= u'Z') || (sign >= u'a' && sign <= u'z') || (sign >= u'0' && sign <= u'9');
    };
    const QList<QStringView> names = keys.split(u',');
    return names.size() <= mostKeys && std::ranges::all_of(names, [&isLetterOrDigit](QStringView name) {
               return name.size() >= shortestKey && name.size() <= longestKey && name.front() >= u'A' &&
                      name.front() <= u'Z' && std::ranges::all_of(name, isLetterOrDigit);
           });
}

std::optional<Action> actionFrom(const json::Value& entry, QString& why)
{
    const auto no = [&why](const char* sentence) {
        why = words(sentence);
        return std::nullopt;
    };
    if (!entry.isObject()) {
        return no("not an entry");
    }
    const std::optional<QString> name = shortLine(entry.find(u"name"), longestActionName);
    if (!name || name->isEmpty()) {
        return no("it has no name");
    }
    const json::Value* kind = entry.find(u"kind");
    Action action;
    action.name = *name;
    if (isTextOf(kind, "keys"_L1)) {
        const json::Value* keys = entry.find(u"keys");
        if (keys == nullptr || !keys->isText() || !areKeys(keys->text)) {
            return no("its keys are not keys this version knows");
        }
        // (As the web's reader: kvm.py's POWER_KEY_REFUSED, begun small.)
        if (kvmd::namesThePowerKey(keys->text)) {
            return no("the keyboard's Power key is not sent. A machine's power is under Power.");
        }
        action.keys = keys->text;
    } else if (isTextOf(kind, "text"_L1)) {
        const json::Value* text = entry.find(u"text");
        if (text == nullptr || !text->isText() || text->text.isEmpty() || signsOf(text->text) > longestTypedText) {
            return no("it has no text to type, or more than can be typed at once");
        }
        action.types = true;
        action.text = text->text;
    } else {
        return no("it is not a kind of action this version knows");
    }
    action.enter = action.types && flagOr(entry.find(u"enter"), false);
    action.danger = flagOr(entry.find(u"danger"), false);
    return action;
}

QList<Action> actionsFrom(const json::Value& said, QStringList& leftOut, NotHeld* notHeld)
{
    QList<Action> actions;
    if (!said.isList()) {
        leftOut.append(words("The list of saved actions is not a list"));
        return actions;
    }
    if (std::cmp_greater(said.list.size(), mostActions)) {
        leftOut.append(words("More than %1 saved actions: the ones after that").arg(mostActions));
    }
    const qsizetype looked = std::min<qsizetype>(static_cast<qsizetype>(said.list.size()), mostActions);
    for (qsizetype index = 0; index < looked; ++index) {
        QString why;
        std::optional<Action> action = actionFrom(said.list.at(static_cast<std::size_t>(index)), why);
        if (action) {
            actions.append(std::move(*action));
        } else {
            leftOut.append(words("Saved action %1: %2").arg(QString::number(index + 1), why));
            noteNotHeld(notHeld, NotHeld::Part::Action, index, why);
        }
    }
    return actions;
}

// --- the settings -------------------------------------------------------------------------------

// Whether a value counts as something where the server asks "is there one?": not nothing, not
// no, not nought, and not empty.
bool isSomething(const json::Value& value) noexcept
{
    switch (value.kind) {
    case json::Value::Kind::Null:
        return false;
    case json::Value::Kind::Flag:
        return value.flag;
    case json::Value::Kind::Number:
        return value.number != 0;
    case json::Value::Kind::Text:
        return !value.text.isEmpty();
    case json::Value::Kind::List:
        return !value.list.empty();
    case json::Value::Kind::Object:
        return !value.members.empty();
    }
    return false;
}

// Where alerts go, as it may be kept; nothing where it is no address of the web.
//
// The server's rule (check_url in its alerts.py), sign for sign as far as that can be said
// without the three libraries it asks: the text with the blanks around it gone has 500 signs
// at the most; nowhere in it is a sign the server's language calls a blank, one below 32, or
// 127; it begins with http:// or https://, in letters small or large; and behind that, before
// its path, stands a host that is not empty, with a name and a password before an "@" where
// it has them (it is kept as a secret and shown to nobody), and a port behind a colon. A host
// that looks like four numbers is four numbers, written the plain way.
//
// Where the libraries' word cannot be said here, this takes less than the server:
// * Before the path nothing but plain letters, digits and signs: a host in letters beyond
//   ASCII is one the server takes where a library reads it as a name, and a name or password
//   in such letters where they count as nothing an address is cut at.
// * A host that begins with "xn--" is none: the server asks the same library whether it is a
//   name written in its short form.
// * A port is digits and nothing else. (The server takes what its language reads as a whole
//   number: a plus or a minus in front, an underscore between two digits.) How large it is
//   nobody asks, there or here.
// * Brackets stand nowhere but around the host, and hold numbers of the new kind by the rule
//   that is written in this program (kvm::addressFrom, and never what Qt reads out of
//   brackets): not one with a zone, and not the other thing the books let brackets hold.
// * Nothing that turns the writing round, and none of the signs from 128 to 159, which move
//   the cursor: the server takes those behind the host.
// What stands behind the host is kept as it is written otherwise, as the server keeps it.
//
// What is not kept is one of three things, and is said as the one it is: no web address by
// what the server's rule says in so many words (a server does not keep it either); an address
// at a host that begins with "xn--" and is in order otherwise (a server keeps it where its
// library has it as a name: a server's own file holds one); or one of the other places above
// where this takes less than a server may.
struct AlertAddress {
    enum class Is { Kept, NotOne, AtAnXnName, AServerMay };
    Is is = Is::NotOne;
    QString kept;
};

AlertAddress alertAddressFrom(const json::Value& value)
{
    using Is = AlertAddress::Is;
    if (!value.isText()) {
        return {Is::NotOne, QString()};
    }
    const QStringView text = stripped(value.text);
    const auto notOfAnAddress = [](QChar sign) {
        return isBlank(sign) || sign.unicode() < 0x20 || sign.unicode() == 0x7F;
    };
    if (signsOf(text) > longestAlertAddress || std::ranges::any_of(text, notOfAnAddress)) {
        return {Is::NotOne, QString()};
    }
    const qsizetype behind = behindTheScheme(text);
    const QStringView authority = behind < 0 ? QStringView() : authorityOf(text, behind);
    if (authority.isEmpty()) {
        return {Is::NotOne, QString()};
    }
    if (!staysALine(text) || !isPlain(authority)) {
        return {Is::AServerMay, QString()};
    }
    // Behind the last "@", where there is one: the host, and its port behind a colon.
    const QStringView where = authority.mid(authority.lastIndexOf(u'@') + 1);
    const bool numbers = where.startsWith(u'[');
    const qsizetype hostEnds = numbers ? where.indexOf(u']') + 1 : where.indexOf(u':');
    const QStringView host = numbers || hostEnds >= 0 ? where.left(hostEnds) : where;
    const QStringView behindHost = where.mid(host.size());
    if (host.isEmpty()) {
        return {Is::NotOne, QString()};
    }
    const qsizetype bracketsOfAHost = numbers ? 1 : 0;
    if ((!behindHost.isEmpty() && behindHost.front() != u':') || !std::ranges::all_of(behindHost.mid(1), isADigit) ||
        authority.count(u'[') != bracketsOfAHost || authority.count(u']') != bracketsOfAHost) {
        return {Is::AServerMay, QString()};
    }
    if (numbers) {
        return kvm::addressFrom(u"https://"_s + host.toString()).ok() ? AlertAddress{Is::Kept, text.toString()}
                                                                      : AlertAddress{Is::AServerMay, QString()};
    }
    if (startsWithInAnyCase(host, punycodeMark)) {
        return {Is::AtAnXnName, QString()};
    }
    if (looksLikeFourNumbers(host) && !isFourNumbers(host)) {
        // Digits with three dots between them that are no four numbers are none to every
        // server; what only looks like four numbers to one version of its library, to some.
        const bool withDots = host.count(u'.') == 3 &&
                              std::ranges::all_of(host, [](QChar sign) { return isADigit(sign) || sign == u'.'; });
        return {withDots ? Is::NotOne : Is::AServerMay, QString()};
    }
    return {Is::Kept, text.toString()};
}

// How alerts are written for the service they go to: the server's words for it (FORMATS in its
// alerts.py), of which the first leaves it to the server.
constexpr std::array<QLatin1StringView, 5> waysOfWritingAlerts{"auto"_L1, "json"_L1, "ntfy"_L1, "slack"_L1,
                                                               "discord"_L1};

std::optional<Alerts> alertsFrom(const json::Value& said, QStringList& leftOut, NotHeld* notHeld)
{
    if (!said.isObject()) {
        leftOut.append(words("The alert settings are not settings"));
        return std::nullopt;
    }
    Alerts alerts;
    alerts.enabled = flagOr(said.find(u"enabled"), false);
    // A word of the server's list as it is, and "auto" for anything else, of which nothing is
    // said: as the server reads it.
    const json::Value* format = said.find(u"format");
    alerts.format = isOneOf(format, waysOfWritingAlerts) ? format->text : u"auto"_s;
    alerts.offline = flagOr(said.find(u"offline"), true);
    alerts.noSignal = flagOr(said.find(u"no_signal"), true);
    alerts.problem = flagOr(said.find(u"problem"), true);
    alerts.recovered = flagOr(said.find(u"recovered"), true);
    const json::Value* address = said.find(u"webhook");
    if (address != nullptr && isSomething(*address)) {
        // What is said of an address that is not kept is what is so: the server's sentence
        // where a server would say it; and where a server may keep the address, that. ("Not a
        // web address that a file holds" was said to a writer of an address at a name in its
        // "xn--" form, which a server's own file holds.)
        AlertAddress made = alertAddressFrom(*address);
        switch (made.is) {
        case AlertAddress::Is::Kept:
            alerts.address = std::move(made.kept);
            break;
        case AlertAddress::Is::NotOne:
            leftOut.append(words("Alert settings: the address alerts go to is not a web address"));
            noteNotHeld(notHeld, NotHeld::Part::Alerts, 0, words("the address alerts go to is not a web address"));
            break;
        case AlertAddress::Is::AtAnXnName:
            leftOut.append(words("Alert settings: the address alerts go to is at a name in its xn-- form, which this "
                                 "program does not take from a file (a qKVM server may)"));
            noteNotHeld(notHeld, NotHeld::Part::Alerts, 0,
                        words("the address alerts go to is at a name in its xn-- form: a qKVM server may take such an "
                              "address, and this program cannot tell whether it does"),
                        true);
            break;
        case AlertAddress::Is::AServerMay:
            leftOut.append(words("Alert settings: the address alerts go to is one this program does not take from a "
                                 "file (a qKVM server may)"));
            noteNotHeld(notHeld, NotHeld::Part::Alerts, 0,
                        words("the address alerts go to is one that a qKVM server may take, and this program cannot "
                              "tell whether it does"),
                        true);
            break;
        }
    }
    return alerts;
}

constexpr double shortestInterval = 0.2;
constexpr double longestInterval = 60;

std::optional<WallSettings> wallFrom(const json::Value& said, QStringList& leftOut, NotHeld* notHeld)
{
    if (!said.isObject()) {
        leftOut.append(words("The wall's settings are not settings"));
        return std::nullopt;
    }
    WallSettings wall;
    const json::Value* interval = said.find(u"interval");
    if (interval != nullptr && interval->isNumber() && interval->number >= shortestInterval &&
        interval->number <= longestInterval) {
        wall.pictureEverySeconds = interval->number;
    } else if (interval != nullptr) {
        leftOut.append(words("Wall settings: how often pictures are fetched is not a time this version allows"));
        noteNotHeld(notHeld, NotHeld::Part::Wall, 0,
                    words("how often pictures are fetched is not a time a file holds (a fifth of a second to a "
                          "minute)"));
    }
    const json::Value* asks = said.find(u"update_check");
    if (asks != nullptr && asks->isFlag()) {
        wall.asksForNewerVersions = asks->flag;
    }
    return wall;
}

// What a file holds, once it is an object that was read: every part by the server's rule, in
// the server's order. `notHeld`, where the writer asks: the first thing that did not pass.
Content contentFrom(const json::Value& said, bool sealed, NotHeld* notHeld = nullptr)
{
    Content content;
    content.sealed = sealed;
    const json::Value* made = said.find(u"made");
    if (made != nullptr && made->isNumber() && made->number >= 0 && made->number < latestMade) {
        content.made = made->number;
    }
    content.program = shortLine(said.find(u"program"), longestProgram).value_or(QString());
    const json::Value* devices = said.find(u"devices");
    if (devices != nullptr) {
        content.devices = devicesFrom(*devices, content.leftOut, notHeld);
    }
    const json::Value* actions = said.find(u"actions");
    if (actions != nullptr) {
        content.actions = actionsFrom(*actions, content.leftOut, notHeld);
    }
    const json::Value* alerts = said.find(u"alerts");
    if (alerts != nullptr) {
        content.alerts = alertsFrom(*alerts, content.leftOut, notHeld);
    }
    const json::Value* wall = said.find(u"wall");
    if (wall != nullptr) {
        content.wall = wallFrom(*wall, content.leftOut, notHeld);
    }
    return content;
}

// --- reading ------------------------------------------------------------------------------------

struct Outside {
    json::Value said;
    bool sealed = false;
};

// A file as far as anybody may read it: that it is one of QKVM's, of a version this reads, and
// whether it is sealed (the server's _parsed, in its order).
base::Result<Outside> outsideOf(const QByteArray& file)
{
    // Its length in signs, as the server counts it. A sign has four bytes at the most: what has
    // more bytes than that many signs would is too long without being counted.
    constexpr qsizetype mostBytesOfASign = 4;
    if (file.size() > largestSealed * mostBytesOfASign) {
        return largerThanAny();
    }
    const std::optional<qsizetype> signs = json::signsIn(file);
    if (!signs) {
        return notOne();
    }
    if (*signs > largestSealed) {
        return largerThanAny();
    }
    base::Result<json::Value> said = json::parse(file);
    if (!said || !said.value().isObject() || !isTextOf(said.value().find(u"format"), formatOfAFile)) {
        return notOne();
    }
    Outside outside;
    outside.said = std::move(said).value();
    outside.sealed = outside.said.find(u"sealed") != nullptr;
    if (!outside.sealed && *signs > largestPlain) {
        return largerThanAny();
    }
    // A version is a whole number from 1 on, written as one: "1.0" is none, and neither is a yes.
    const json::Value* version = outside.said.find(u"version");
    if (version == nullptr || !version->isNumber() || !version->whole || version->number < 1) {
        return notOne();
    }
    if (version->number > versionOfAFile) {
        return refusal(newer, "This file was made by a later version of qKVM than this one. Update this one first.");
    }
    return base::Result<Outside>(std::move(outside));
}

// What a sealed file holds, opened: the object that is written inside it.
base::Result<json::Value> insideOf(const json::Value& said, QByteArrayView passphrase, bool isAPassphrase,
                                   const Derive& derive, const std::stop_token& stop)
{
    if (passphrase.isEmpty() && isAPassphrase) {
        return refusal(passphraseNeeded, "This file is encrypted. Enter the passphrase it was made with.");
    }
    // The lock first, and all of it before any work: what asks for more than a file may is not
    // worked on, and no memory is asked for on its word.
    const std::optional<Lock> lock = lockFrom(said.find(u"lock"));
    if (!lock || !isAPassphrase) {
        return notThePassphrase();
    }
    const base::Result<SecretKey> derived = derive(passphrase, lock->salt, lock->cost, stop);
    if (!derived) {
        return derived.error();
    }
    const base::Result<SecretBytes> opened =
        sealing::open(derived.value(), lock->key.nonce, lock->key.data, sealedAsAKey);
    if (!opened) {
        return std::string(opened.error().code) == sealing::doesNotOpen ? notThePassphrase() : opened.error();
    }
    // The file's own key has 32 bytes. (The server would go on with one of 16 or of 24 as well,
    // which it never writes; any other length it calls damaged, and so is every other here.)
    if (opened.value().size() != SecretKey::bytes) {
        return doesNotOpen();
    }
    SecretKey fileKey;
    std::copy_n(opened.value().data(), SecretKey::bytes, fileKey.data());
    const std::optional<SealedBytes> sealed = sealedFrom(said.find(u"sealed"));
    if (!sealed) {
        return doesNotOpen();
    }
    const base::Result<SecretBytes> inside = sealing::open(fileKey, sealed->nonce, sealed->data, sealedAsContent);
    if (!inside) {
        return std::string(inside.error().code) == sealing::doesNotOpen ? doesNotOpen() : inside.error();
    }
    const std::optional<qsizetype> signs = json::signsIn(inside.value().view());
    if (!signs) {
        return doesNotOpen();
    }
    if (*signs > largestPlain) {
        return largerThanAny();
    }
    base::Result<json::Value> written = json::parse(inside.value().view());
    if (!written) {
        return doesNotOpen();
    }
    if (!written.value().isObject()) {
        return refusal(damaged, "The file is damaged: what is in it is not what qKVM writes");
    }
    return written;
}

base::Result<Content> readFile(const QByteArray& file, QByteArrayView passphrase, bool isAPassphrase,
                               const Derive& derive, const std::stop_token& stop)
{
    base::Result<Outside> outside = outsideOf(file);
    if (!outside) {
        return outside.error();
    }
    if (!outside.value().sealed) {
        return contentFrom(outside.value().said, false);
    }
    const base::Result<json::Value> inside = insideOf(outside.value().said, passphrase, isAPassphrase, derive, stop);
    if (!inside) {
        return inside.error();
    }
    return contentFrom(inside.value(), true);
}

// --- writing ------------------------------------------------------------------------------------

// A JSON text, written out as the server's language writes one: with nothing in it but the
// signs from the blank to the tilde (every other one by its number), and either in one line
// without a blank, or spread over lines, two blanks deeper for each thing in another. Into room
// that was asked for once: what does not fit is not written, and `fits` says so.
class Script {
public:
    Script(qsizetype room, bool spread) : m_text(room), m_spread(spread) {}

    bool fits() const noexcept { return m_fits; }
    const SecretBytes& written() const noexcept { return m_text; }

    void open(char bracket)
    {
        before();
        put(QByteArrayView(&bracket, 1));
        m_inside.push_back(0);
    }

    void close(char bracket)
    {
        const int things = m_inside.back();
        m_inside.pop_back();
        if (m_spread && things > 0) {
            newLine();
        }
        put(QByteArrayView(&bracket, 1));
    }

    void name(QByteArrayView key)
    {
        before();
        put("\"");
        put(key);
        put(m_spread ? QByteArrayView("\": ") : QByteArrayView("\":"));
        m_named = true;
    }

    void text(QStringView value)
    {
        before();
        put("\"");
        for (const QChar sign : value) {
            const char16_t unit = sign.unicode();
            if (unit == u'"') {
                put("\\\"");
            } else if (unit == u'\\') {
                put("\\\\");
            } else if (unit == u'\n') {
                put("\\n");
            } else if (unit == u'\r') {
                put("\\r");
            } else if (unit == u'\t') {
                put("\\t");
            } else if (unit == u'\b') {
                put("\\b");
            } else if (unit == u'\f') {
                put("\\f");
            } else if (unit >= u' ' && unit <= u'~') {
                const char plain = static_cast<char>(unit);
                put(QByteArrayView(&plain, 1));
            } else {
                // By its number: a backslash, the letter u and four digits; a sign of two
                // places as its two halves.
                put("\\");
                put("u");
                put(QByteArray::number(static_cast<uint>(unit), 16).rightJustified(4, '0'));
            }
        }
        put("\"");
    }

    // A text that is known to need nothing spelled out: bytes as base64.
    void plainText(QByteArrayView value)
    {
        before();
        put("\"");
        put(value);
        put("\"");
    }

    void flag(bool value)
    {
        before();
        put(value ? QByteArrayView("true") : QByteArrayView("false"));
    }

    void whole(qint64 value)
    {
        before();
        put(QByteArray::number(value));
    }

    // A number with a fraction, in the fewest digits that say it; and with ".0" behind one that
    // has none, as the server's language writes a number of that kind.
    void real(double value)
    {
        before();
        QByteArray digits = QByteArray::number(value, 'g', QLocale::FloatingPointShortest);
        if (!digits.contains('.') && !digits.contains('e')) {
            digits += ".0";
        }
        put(digits);
    }

    void nothing()
    {
        before();
        put("null");
    }

    void lineEnd() { put("\n"); }

private:
    // What stands before a thing: nothing behind its own name; else a comma behind the thing
    // before it, and a new line where the text is spread.
    void before()
    {
        if (std::exchange(m_named, false) || m_inside.empty()) {
            return;
        }
        const bool first = m_inside.back() == 0;
        ++m_inside.back();
        if (!first) {
            put(",");
        }
        if (m_spread) {
            newLine();
        }
    }

    void newLine()
    {
        put("\n");
        put(QByteArray(static_cast<qsizetype>(m_inside.size()) * 2, ' '));
    }

    void put(QByteArrayView bytes) noexcept
    {
        if (!m_text.append(bytes)) {
            m_fits = false;
        }
    }

    SecretBytes m_text;
    bool m_spread;
    bool m_fits = true;
    bool m_named = false;
    std::vector<int> m_inside; // for each thing that is open: how many things it has so far
};

// A file's parts written out, as the server gathers them (gather in exchange.py): every field
// in the server's order, and with `secrets` false nothing that is kept encrypted on a wall.
void writeParts(Script& script, const Content& content, bool secrets)
{
    if (content.devices) {
        script.name("devices");
        script.open('[');
        for (const Device& device : *content.devices) {
            script.open('{');
            script.name("name");
            script.text(device.name);
            script.name("url");
            script.text(device.address);
            script.name("username");
            script.text(device.user);
            if (secrets) {
                script.name("password");
                script.text(device.password);
            }
            script.name("kind");
            script.text(device.kind);
            script.name("group");
            script.text(device.group);
            script.name("tls");
            script.text(device.tls);
            script.name("cert_pem");
            if (device.certificatePem.isEmpty()) {
                script.nothing();
            } else {
                script.text(device.certificatePem);
            }
            script.name("open_url");
            script.text(device.consoleLinkAsWritten.asAFileHadIt());
            script.name("alerts");
            script.flag(device.alerts);
            script.name("ident");
            script.text(device.ident);
            if (device.keepBoth) {
                script.name("keep_both");
                script.flag(true);
            }
            // As the server writes it, last, and also where none is kept (""): a file without the field is one of
            // 0.1.x, whose server's number is taken from its `ident`. What is no such number is written as none.
            script.name("machine");
            script.text(isAnIdent(device.machine) ? device.machine : QString());
            script.close('}');
        }
        script.close(']');
    }
    if (content.actions) {
        script.name("actions");
        script.open('[');
        for (const Action& action : *content.actions) {
            // A saved text without its text is nothing to keep: in a plain file those are not
            // there at all.
            if (action.types && !secrets) {
                continue;
            }
            script.open('{');
            script.name("name");
            script.text(action.name);
            script.name("kind");
            script.text(action.types ? u"text" : u"keys");
            script.name("keys");
            script.text(action.keys);
            if (action.types) {
                script.name("text");
                script.text(action.text);
            }
            script.name("enter");
            script.flag(action.enter);
            script.name("danger");
            script.flag(action.danger);
            script.close('}');
        }
        script.close(']');
    }
    if (content.alerts) {
        const Alerts& alerts = *content.alerts;
        script.name("alerts");
        script.open('{');
        script.name("enabled");
        script.flag(alerts.enabled);
        script.name("format");
        script.text(alerts.format);
        script.name("offline");
        script.flag(alerts.offline);
        script.name("no_signal");
        script.flag(alerts.noSignal);
        script.name("problem");
        script.flag(alerts.problem);
        script.name("recovered");
        script.flag(alerts.recovered);
        if (secrets) {
            script.name("webhook");
            script.text(alerts.address);
        }
        script.close('}');
    }
    if (content.wall) {
        const WallSettings& wall = *content.wall;
        script.name("wall");
        script.open('{');
        if (wall.pictureEverySeconds) {
            script.name("interval");
            script.real(*wall.pictureEverySeconds);
        }
        if (wall.asksForNewerVersions) {
            script.name("update_check");
            script.flag(*wall.asksForNewerVersions);
        }
        script.close('}');
    }
}

void writeHead(Script& script)
{
    script.name("format");
    script.plainText(QByteArrayView(formatOfAFile.data(), formatOfAFile.size()));
    script.name("version");
    script.whole(versionOfAFile);
}

// Why more of a part than a file holds is not written, in the server's sentence.
base::Error moreThanAFileHolds(qsizetype has, const char* what, qsizetype most)
{
    return {tooLarge, words("This wall has %1 %2, and a file of qKVM's holds %3. Nothing was written: leave that part "
                            "out, or have fewer.")
                          .arg(QString::number(has), words(what), QString::number(most))
                          .toStdString()};
}

// Which thing of a wall it is that a file does not hold, for a sentence: an entry by its place on
// the wall and its name (as nameSetOff makes it: the one way a name is shown), the settings as
// what they are.
QString whichOfTheWall(NotHeld::Part part, qsizetype index, const QString& name)
{
    const QString shown = nameSetOff(name);
    const QString number = QString::number(index + 1);
    switch (part) {
    case NotHeld::Part::Device:
        return shown.isEmpty() ? words("device %1 of this wall").arg(number)
                               : words("device %1 of this wall (%2)").arg(number, shown);
    case NotHeld::Part::Action:
        return shown.isEmpty() ? words("saved action %1 of this wall").arg(number)
                               : words("saved action %1 of this wall (%2)").arg(number, shown);
    case NotHeld::Part::Alerts:
        return words("what this wall has set for alerts");
    case NotHeld::Part::Wall:
        return words("what this wall has set for itself");
    case NotHeld::Part::Nothing:
        break;
    }
    return words("this wall");
}

// What is said to whoever writes a file, where something of the wall is not written: which
// thing, and what of it. In words for somebody who has a wall and wants a file, and not in the
// reader's, which speak of a file that is there. And in the words that are so: that no file of
// QKVM's holds it; or, with `onlyHere`, that this program does not write it, where a server's
// own file may hold it.
base::Error notHeldByAFile(const QString& which, const QString& why, bool onlyHere = false)
{
    const QString said =
        onlyHere ? words("Nothing was written: this program does not write %1 into a file: %2").arg(which, why)
                 : words("Nothing was written: %1 cannot be put into a file of qKVM's: %2").arg(which, why);
    return {notWritten, said.toStdString()};
}

// Whether a QKVM server may not take a device at this address, which this program speaks to
// and reads back; and by which of the two rules. An address as it is written the one way
// (kvm::addressFrom: "https://host", "http://host:port", all of it plain and in small letters).
// By two rules that are said, and by no guess at what a server's library makes of a name:
// * A part of the name begins with "xn--", anywhere in it. (The server asks the library its
//   lines are made with whether such a name is one, where the whole host begins with "xn--";
//   Qt, which has the last word here, reads a part by another rule, and only under the endings
//   it has a list of: deviceFrom has the table. For one round this was told by the letters a
//   name stands for: a line for a heart, and none for "xn--zzzzzz.lan", which is taken here
//   and is none where Qt or a server reads it. A line too many costs a look; a line too few is
//   a backup that is short of a KVM with nobody told.)
// * The name reads as digits, a sign, digits, a sign, digits, a sign, digits, and is not four
//   plain numbers: "192-168-1-20", "1.2.3-4" (looksLikeFourNumbers says whose rule that is).
enum class AServerMayNotTake { Nothing, ANameInItsXnForm, ANameThatReadsAsNumbers };

AServerMayNotTake aServerMayNotTake(QStringView address)
{
    const qsizetype behind = behindTheScheme(address);
    const QStringView authority = behind < 0 ? QStringView() : authorityOf(address, behind);
    if (authority.isEmpty() || authority.startsWith(u'[')) {
        return AServerMayNotTake::Nothing; // (numbers of the new kind are numbers to everybody)
    }
    const qsizetype colon = authority.indexOf(u':');
    const QStringView host = colon < 0 ? authority : authority.left(colon);
    const QList<QStringView> parts = host.split(u'.');
    if (std::ranges::any_of(parts, [](QStringView part) { return startsWithInAnyCase(part, punycodeMark); })) {
        return AServerMayNotTake::ANameInItsXnForm;
    }
    return looksLikeFourNumbers(host) && !isFourNumbers(host) ? AServerMayNotTake::ANameThatReadsAsNumbers
                                                              : AServerMayNotTake::Nothing;
}

// What a file does not say of the wall it is written from, or says to this program and maybe
// not to a server: a line for each such thing of each device, which names the device by the
// one function there is for that (deviceOfTheWall). Asked of the devices as they are written:
// with their addresses made the one way, so that "HTTPS://" is over HTTPS too.
// * A device at a name that a QKVM server may not take (aServerMayNotTake): it is in the file,
//   this program reads it back, and a server may not.
// * A device whose console link this reader would leave the device out for (consoleLinkFrom):
//   the link is written as it was, and a server may leave the device out as this reader does.
//   Which links a server keeps is not guessed: its language's reader of addresses takes some
//   in one version and not in the next.
// * A device that is known by its certificate's SHA-256 and by no certificate. (This PC's wall
//   knows its KVMs so: by what they showed, and not by the certificate itself.) A file holds a
//   certificate or none, and is written with none: "pinned, and no certificate is remembered",
//   which to a server that reads it is "known by whatever it shows first".
QStringList notInAFile(const Content& content)
{
    QStringList lines;
    const QList<Device> devices = content.devices.value_or(QList<Device>());
    for (qsizetype index = 0; index < devices.size(); ++index) {
        const Device& device = devices.at(index);
        const QString which = deviceOfTheWall(index, device.name);
        switch (aServerMayNotTake(device.address)) {
        case AServerMayNotTake::ANameInItsXnForm:
            lines.append(words("%1: its address is a name with a part in its xn-- form. This program speaks to it and "
                               "reads it back from this file; a qKVM server may not take a name in its xn-- form: "
                               "read the file there and look.")
                             .arg(which));
            break;
        case AServerMayNotTake::ANameThatReadsAsNumbers:
            lines.append(words("%1: its address is a name of digits with single signs between them, which a qKVM "
                               "server may read as numbers that are no address, and not take: read the file there "
                               "and look.")
                             .arg(which));
            break;
        case AServerMayNotTake::Nothing:
            break;
        }
        if (consoleLinkFrom(device.consoleLinkAsWritten.asAFileHadIt()).deviceRefused != nullptr) {
            lines.append(words("%1: its console link is written as it is. Reading this file, this program leaves "
                               "this device out for it, and a qKVM server may too: read the file there and look.")
                             .arg(which));
        }
        if (device.tls == "pinned"_L1 && device.address.startsWith("https://"_L1) && !device.certificate.isEmpty() &&
            device.certificatePem.isEmpty()) {
            lines.append(words("%1: only the fingerprint of its certificate is known here, and a file holds a "
                               "certificate itself or none. A qKVM server that reads this file will take the "
                               "first certificate this device shows.")
                             .arg(which));
        }
    }
    return lines;
}

base::Result<SecretKey> randomKey()
{
    SecretKey key;
    const base::Result<void> made = sealing::random(std::span<quint8>(key.data(), SecretKey::bytes));
    if (!made) {
        return made.error();
    }
    return base::Result<SecretKey>(std::move(key));
}

base::Result<QByteArray> randomBytes(qsizetype count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    const base::Result<void> made =
        sealing::random(std::span<quint8>(reinterpret_cast<quint8*>(bytes.data()), static_cast<std::size_t>(count)));
    if (!made) {
        return made.error();
    }
    return bytes;
}

// Whether a host, as an address has it written the one way, is this PC itself: the list
// ConsoleAddress::isThisPcsOwn has. (A name is asked by its letters and numbers as numbers: no
// name is looked up.)
bool isThisPcsOwn(const QString& host)
{
    if (host == "localhost"_L1 || host.endsWith(".localhost"_L1)) {
        return true;
    }
    const QHostAddress numbers(host.startsWith(u'[') ? host.mid(1, host.size() - 2) : host);
    return !numbers.isNull() && (numbers.isLoopback() || numbers == QHostAddress(QHostAddress::AnyIPv4) ||
                                 numbers == QHostAddress(QHostAddress::AnyIPv6));
}

} // namespace

// A device of the wall at the beginning of a line that is handed back beside a file: by its
// place on the wall, and by its name as nameSetOff makes it where it has one to show. (The
// name as it was handed in stood there once: a device without one was a line that named
// nothing, and a name with a line's end behind it broke its line in two.)
QString deviceOfTheWall(qsizetype index, const QString& name)
{
    const QString shown = nameSetOff(name);
    const QString number = QString::number(index + 1);
    return shown.isEmpty() ? words("Device %1 of this wall").arg(number)
                           : words("Device %1 of this wall (%2)").arg(number, shown);
}

QString nameOfADeviceWithoutOne(const QString& address)
{
    // (No longer than a name may be, as a device is named that is added without a name.)
    return address.mid(address.indexOf("://"_L1) + 3).left(longestName);
}

std::optional<KeptCertificate> certificateFrom(const QString& written)
{
    // (As one of a file with this one certificate in it: every rule of the reader's, and Windows
    // asked once.)
    Certificates ofOne;
    const Certificates::Looked looked = ofOne.of(written);
    if (!looked.certificate) {
        return std::nullopt;
    }
    return KeptCertificate{looked.certificate->pem, looked.certificate->sha256};
}

std::optional<KeptCertificate> certificateOf(const QByteArray& der)
{
    // No more bytes than the signs a file holds of one are made a text of at all.
    if (der.isEmpty() || der.size() > longestCertificate) {
        return std::nullopt;
    }
    return certificateFrom(asKept(der).pem);
}

bool mayBeAsked(const scrypt::Cost& cost) noexcept
{
    constexpr quint64 leastN = quint64{1} << 10;
    constexpr quint64 mostN = quint64{1} << 20;
    constexpr quint32 mostR = 16;
    constexpr quint32 mostP = 4;
    constexpr quint64 mostWork = quint64{1} << 21;
    // What the work itself insists on: N below 2^(16r), which only r = 1 can fall short of here.
    constexpr quint64 mostNWithTheSmallestBlock = quint64{1} << 16;
    return cost.n >= leastN && cost.n <= mostN && (cost.n & (cost.n - 1)) == 0 && cost.r >= 1 && cost.r <= mostR &&
           cost.p >= 1 && cost.p <= mostP && cost.n * cost.r * cost.p <= mostWork &&
           (cost.r > 1 || cost.n < mostNWithTheSmallestBlock);
}

bool Content::hasSecrets() const
{
    const auto hasPassword = [](const Device& device) { return !device.password.isEmpty(); };
    const auto hasText = [](const Action& action) { return !action.text.isEmpty(); };
    return (devices && std::ranges::any_of(*devices, hasPassword)) ||
           (actions && std::ranges::any_of(*actions, hasText)) || (alerts && !alerts->address.isEmpty());
}

Derive windowsDerive()
{
    return [](QByteArrayView passphrase, QByteArrayView salt, const scrypt::Cost& cost,
              const std::stop_token& stop) -> base::Result<SecretKey> {
        SecretKey key;
        const base::Result<void> made =
            scrypt::derive(passphrase, salt, cost, std::span<quint8>(key.data(), SecretKey::bytes), stop);
        if (!made) {
            return made.error();
        }
        return base::Result<SecretKey>(std::move(key));
    };
}

base::Result<bool> isSealed(const QByteArray& file)
{
    const base::Result<Outside> outside = outsideOf(file);
    if (!outside) {
        return outside.error();
    }
    return outside.value().sealed;
}

base::Result<Content> read(const QByteArray& file, const QString& passphrase, const std::stop_token& stop)
{
    return readWith(file, SecretBytes::utf8Of(passphrase), windowsDerive(), stop);
}

base::Result<Content> readWith(const QByteArray& file, const Passphrase& passphrase, const Derive& derive,
                               const std::stop_token& stop)
{
    // A passphrase with half a character in it is no text anything was sealed with: a plain file
    // is read all the same, and a sealed one does not open.
    return readFile(file, passphrase ? passphrase->view() : QByteArrayView(), passphrase.has_value(), derive, stop);
}

std::optional<base::Error> passphraseRefused(const QString& passphrase)
{
    if (!SecretBytes::utf8Of(passphrase)) {
        return base::Error{"invalid", words("A passphrase cannot have half a character in it.").toStdString()};
    }
    const qsizetype signs = signsOf(passphrase);
    if (signs < shortestPassphrase) {
        return base::Error{"invalid",
                           words("A passphrase has at least %1 characters.").arg(shortestPassphrase).toStdString()};
    }
    if (signs > longestPassphrase) {
        return base::Error{"invalid",
                           words("A passphrase has at most %1 characters.").arg(longestPassphrase).toStdString()};
    }
    return std::nullopt;
}

base::Result<Written> write(const Content& content, const QString& passphrase, const std::stop_token& stop)
{
    return writeWith(content, SecretBytes::utf8Of(passphrase), Sealing(), stop);
}

base::Result<Written> writeWith(const Content& handedIn, const Passphrase& typed, const Sealing& how,
                                const std::stop_token& stop)
{
    if (!typed) {
        return base::Error{"invalid", words("A passphrase cannot have half a character in it.").toStdString()};
    }
    const SecretBytes& passphrase = *typed;
    const bool sealed = passphrase.size() > 0;
    // More than a file holds is not written: a file that turns out to be too large on the day
    // it is needed, or to have lost its last devices, is worse than none.
    if (handedIn.devices && handedIn.devices->size() > mostDevices) {
        return moreThanAFileHolds(handedIn.devices->size(), "devices", mostDevices);
    }
    if (handedIn.actions && handedIn.actions->size() > mostActions) {
        return moreThanAFileHolds(handedIn.actions->size(), "saved actions", mostActions);
    }
    if (sealed && !mayBeAsked(how.cost)) {
        return base::Error{scrypt::notACost, words("These are not settings a key can be made with.").toStdString()};
    }
    // Whatever was handed in as an address is taken by the program's own rule first, and
    // written the one way, as a file has it and as the reader would make it: what is asked of
    // an address from here on ("is it over HTTPS?") is asked of that, and not of what somebody
    // typed in capitals. One that is none is not written, and not shown in saying so.
    Content content = handedIn;
    if (content.devices) {
        QList<Device>& devices = *content.devices;
        for (qsizetype at = 0; at < devices.size(); ++at) {
            const base::Result<QUrl> where = kvm::addressFrom(devices.at(at).address);
            if (!where) {
                return notHeldByAFile(whichOfTheWall(NotHeld::Part::Device, at, devices.at(at).name),
                                      words(whyNoAddress(devices.at(at).address)));
            }
            devices[at].address = where.value().toString(QUrl::FullyEncoded);
        }
    }
    // What the file holds, written out: all of it for a sealed one, in one line; and for a
    // plain one, which is the file, spread over lines. Into room for the longest there may be:
    // what does not fit there is more than a file holds.
    Script inside(largestPlain, !sealed);
    inside.open('{');
    writeHead(inside);
    inside.name("made");
    // (When it was made, where that is a time a file can say; the time it is now otherwise.)
    const bool saysWhen = content.made > 0 && content.made < latestMade;
    inside.whole(saysWhen ? std::llround(content.made) : QDateTime::currentSecsSinceEpoch());
    inside.name("program");
    inside.text(content.program);
    writeParts(inside, content, sealed);
    inside.close('}');
    if (!sealed) {
        inside.lineEnd();
    }
    if (!inside.fits()) {
        return holdsMoreThanAFile();
    }
    // Read once as a file would be, before anything is made of the passphrase: by this reader's
    // rules but for a console link, which is a text as a file had it and is written as it was
    // (notInAFile says of it beside the file). What this reader takes is what the program
    // speaks to, so a wall of the program's own passes whole. Where something of it does not,
    // nothing is written, and the first such thing is named (exchange.h says why nothing, and
    // not the rest).
    const base::Result<json::Value> again = json::parse(inside.written().view());
    NotHeld notHeld;
    const bool readBackWhole =
        again && again.value().isObject() && contentFrom(again.value(), sealed, &notHeld).leftOut.isEmpty();
    if (!readBackWhole && notHeld.part == NotHeld::Part::Nothing) {
        // (A text with half a character in it: a file would have to spell out what no file may.)
        return base::Error{notWritten, words("Nothing was written: it holds what no file can").toStdString()};
    }
    if (!readBackWhole) {
        qsizetype place = notHeld.index;
        QString name;
        if (notHeld.part == NotHeld::Part::Device) {
            name = content.devices.value_or(QList<Device>()).value(place).name;
        } else if (notHeld.part == NotHeld::Part::Action) {
            // Its place on the wall, and not in the file: a plain file is written without the
            // actions that type a text.
            const QList<Action> actions = content.actions.value_or(QList<Action>());
            qsizetype writtenBefore = 0;
            for (qsizetype at = 0; at < actions.size(); ++at) {
                if (actions.at(at).types && !sealed) {
                    continue;
                }
                if (writtenBefore == notHeld.index) {
                    place = at;
                    name = actions.at(at).name;
                    break;
                }
                ++writtenBefore;
            }
        }
        return notHeldByAFile(whichOfTheWall(notHeld.part, place, name), notHeld.why, notHeld.onlyHere);
    }
    // Nor with a word for how alerts are written that is not of the server's list: read back,
    // there and here, it would be "auto", and nobody would have been told.
    if (content.alerts && !std::ranges::any_of(waysOfWritingAlerts, [&content](QLatin1StringView way) {
            return content.alerts->format == way;
        })) {
        return notHeldByAFile(whichOfTheWall(NotHeld::Part::Alerts, 0, QString()),
                              words("how alerts are written is not said in a way qKVM knows"));
    }
    const QStringList notInIt = notInAFile(content);
    if (!sealed) {
        return Written(QByteArray(inside.written().view().data(), inside.written().size()), notInIt);
    }

    // A key of this file's own, and what lets the passphrase open it.
    const base::Result<QByteArray> salt = randomBytes(saltBytesAsWritten);
    const base::Result<QByteArray> nonceOfTheKey = randomBytes(sealing::nonceBytes);
    const base::Result<QByteArray> nonceOfTheContent = randomBytes(sealing::nonceBytes);
    const base::Result<SecretKey> fileKey = randomKey();
    if (!salt || !nonceOfTheKey || !nonceOfTheContent || !fileKey) {
        return base::Error{sealing::windowsRefused, words("Windows could not do the encryption.").toStdString()};
    }
    const base::Result<SecretKey> derived = how.derive(passphrase.view(), salt.value(), how.cost, stop);
    if (!derived) {
        return derived.error();
    }
    const base::Result<QByteArray> lockedKey =
        sealing::seal(derived.value(), nonceOfTheKey.value(), fileKey.value().view(), sealedAsAKey);
    if (!lockedKey) {
        return lockedKey.error();
    }
    const base::Result<QByteArray> body =
        sealing::seal(fileKey.value(), nonceOfTheContent.value(), inside.written().view(), sealedAsContent);
    if (!body) {
        return body.error();
    }

    Script file(largestSealed, true);
    file.open('{');
    writeHead(file);
    file.name("lock");
    file.open('{');
    file.name("kdf");
    file.open('{');
    file.name("name");
    file.plainText("scrypt");
    file.name("n");
    file.whole(static_cast<qint64>(how.cost.n));
    file.name("r");
    file.whole(how.cost.r);
    file.name("p");
    file.whole(how.cost.p);
    file.name("salt");
    file.plainText(salt.value().toBase64());
    file.close('}');
    file.name("key");
    file.open('{');
    file.name("nonce");
    file.plainText(nonceOfTheKey.value().toBase64());
    file.name("data");
    file.plainText(lockedKey.value().toBase64());
    file.close('}');
    file.close('}');
    file.name("sealed");
    file.open('{');
    file.name("nonce");
    file.plainText(nonceOfTheContent.value().toBase64());
    file.name("data");
    file.plainText(body.value().toBase64());
    file.close('}');
    file.close('}');
    file.lineEnd();
    if (!file.fits()) {
        return holdsMoreThanAFile();
    }
    return Written(QByteArray(file.written().view().data(), file.written().size()), notInIt);
}

QString nameSetOff(const QString& name)
{
    const std::optional<QString> line = asALine(stripped(name));
    if (!line || signsOf(stripped(*line)) > longestName) {
        return {};
    }
    // In double quotes, and with no double quote in between: what is shown of a name cannot
    // close its own quotes, whatever the name is.
    QString shown(u'"');
    bool somethingToSee = false;
    const QList<uint> signs = stripped(*line).toUcs4();
    for (const uint one : signs) {
        const char32_t sign = one;
        if (saysWhichWayTheWritingRuns(sign)) {
            continue;
        }
        somethingToSee = somethingToSee || !showsNothing(sign);
        if (sign != U'\'' && !looksLikeADoubleQuote(sign)) {
            shown += QString::fromUcs4(&sign, 1);
        } else if (!shown.endsWith(u'\'')) {
            // (One apostrophe for as many as stand side by side, whether the name has them or
            // they were made of quotes: two of them look like a double quote.)
            shown += u'\'';
        }
    }
    return somethingToSee ? shown + u'"' : QString();
}

std::optional<ConsoleAddress> consoleAddress(const LinkAsWritten& link)
{
    const QString& text = link.asAFileHadIt();
    // The plain signs from behind the blank to the tilde, and of those none that a command line
    // or a browser reads as more than a sign of an address: quotes, what joins and redirects
    // commands and names their variables, and the backslash, which a browser reads as a stroke
    // (two names with one between them go to the first, and read as the second).
    constexpr QLatin1StringView never{"\"'`&|<>^%\\"};
    const auto mayStand = [never](QChar sign) {
        return sign.unicode() > 0x20 && sign.unicode() < 0x7F && !never.contains(sign);
    };
    if (text.isEmpty() || text.size() > longestConsoleLink || !std::ranges::all_of(text, mayStand)) {
        return std::nullopt;
    }
    const qsizetype behind = behindTheScheme(text);
    if (behind < 0) {
        return std::nullopt;
    }
    // Its host and port by the program's own rule for an address: a host that is one, a port
    // that is one, and nobody's name or password.
    const QStringView authority = authorityOf(text, behind);
    const QStringView behindThem = QStringView(text).mid(behind + authority.size());
    const base::Result<QUrl> where = kvm::addressFrom(text.left(behind) + authority.toString());
    if (!where) {
        return std::nullopt;
    }
    // And Qt, which is who would hand it on, reads the same out of all of it, by the book. (A
    // dot at a name's end is dropped by the program's rule and kept by Qt: such a link is none.)
    const QUrl address(text, QUrl::StrictMode);
    constexpr qsizetype behindHttps = 8;
    const bool encrypted = behind == behindHttps;
    const int usual = encrypted ? 443 : 80;
    if (!address.isValid() || address.scheme() != where.value().scheme() || !address.userInfo().isEmpty() ||
        address.host(QUrl::FullyEncoded) != where.value().host(QUrl::FullyEncoded) ||
        address.port(usual) != where.value().port(usual)) {
        return std::nullopt;
    }
    // What stands behind the host and port, as Qt writes it with everything spelled out that an
    // address spells out: the same, sign for sign, as the link has it. Where Qt would write a
    // sign of it otherwise, what would be opened is not what was written.
    QString byQt = address.path(QUrl::FullyEncoded);
    if (address.hasQuery()) {
        byQt += u'?' + address.query(QUrl::FullyEncoded);
    }
    if (address.hasFragment()) {
        byQt += u'#' + address.fragment(QUrl::FullyEncoded);
    }
    if (byQt != behindThem) {
        return std::nullopt;
    }
    // Its words, each put together here, and none of them a word of a QUrl's that shows a name
    // in the letters it stands for. The host and port are the text the program's rule writes an
    // address as (the text a device's address is kept as): QUrl's ONE form in which a name stays
    // in its "xn--" form, and one that rule has compared with the plain signs that were written.
    const QString oneWay = where.value().toString(QUrl::FullyEncoded);
    const QStringView hostAndPort = QStringView(oneWay).mid(oneWay.indexOf("://"_L1) + 3);
    const qsizetype colon = hostAndPort.indexOf(u':');
    const qsizetype hostEnds =
        hostAndPort.startsWith(u'[') ? hostAndPort.indexOf(u']') + 1 : (colon < 0 ? hostAndPort.size() : colon);
    ConsoleAddress made;
    made.m_host = hostAndPort.left(hostEnds).toString();
    made.m_port = where.value().port(usual);
    made.m_encrypted = encrypted;
    made.m_toOpen = oneWay + behindThem;
    made.m_thisPcsOwn = isThisPcsOwn(made.m_host);
    // And every sign of every word is looked at once more, and not taken on trust from what
    // made it: one that is not plain, or a host that is not what stands in the address right
    // behind how it is spoken to, and nothing is handed out.
    const QString upToItsHost = (encrypted ? u"https://"_s : u"http://"_s) + made.m_host;
    if (made.m_host.isEmpty() || !isPlain(made.m_toOpen) || !made.m_toOpen.startsWith(upToItsHost)) {
        return std::nullopt;
    }
    return made;
}

bool isADevicesOwnNumber(QStringView text) noexcept
{
    return isAnIdent(text);
}

} // namespace qkvm::core::exchange
