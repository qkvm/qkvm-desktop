#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

// Bytes that are a secret for as long as they are held: a passphrase's, a key's, what a sealed
// file holds. What held them is written over before it is given back (SPEC.md section 8), on
// every way out: these do it when they go, and none of them can be copied by oversight.
namespace qkvm::core {

// Writes noughts over memory, in a way the compiler does not leave out (SecureZeroMemory).
void wipe(void* at, std::size_t bytes) noexcept;

// A key of 32 bytes: one that was derived from a passphrase, or a file's own.
class SecretKey {
public:
    static constexpr qsizetype bytes = 32;

    SecretKey() = default;
    SecretKey(const SecretKey&) = delete;
    SecretKey& operator=(const SecretKey&) = delete;
    // What is moved from is wiped: there is one of it afterwards, as before.
    SecretKey(SecretKey&& other) noexcept;
    SecretKey& operator=(SecretKey&& other) noexcept;
    ~SecretKey();

    // A second one, where Windows wants a key it may write to. Said out loud, never by itself.
    SecretKey copy() const noexcept;

    quint8* data() noexcept { return m_bytes.data(); }
    const quint8* data() const noexcept { return m_bytes.data(); }
    QByteArrayView view() const noexcept;

private:
    std::array<quint8, bytes> m_bytes{};
};

// Room for so many bytes, asked for once and never moved: what is put into it stands in one
// place, and that place is wiped when this goes. (A QByteArray that grows leaves what it held
// behind, wherever it stood before.)
class SecretBytes {
public:
    SecretBytes() = default;
    explicit SecretBytes(qsizetype count);
    SecretBytes(const SecretBytes&) = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;
    SecretBytes(SecretBytes&& other) noexcept;
    SecretBytes& operator=(SecretBytes&& other) noexcept;
    ~SecretBytes();

    // The bytes of a text, as UTF-8. Nothing where the text holds half a character (a surrogate
    // by itself), which no bytes say.
    static std::optional<SecretBytes> utf8Of(QStringView text);
    static SecretBytes copyOf(QByteArrayView bytes);

    quint8* data() noexcept { return m_bytes.get(); }
    const quint8* data() const noexcept { return m_bytes.get(); }
    qsizetype size() const noexcept { return m_size; }
    qsizetype room() const noexcept { return m_room; }
    QByteArrayView view() const noexcept;

    // So many of the bytes are in use from now on: never more than there is room for.
    void resize(qsizetype count) noexcept;
    // False, and nothing added, where it does not fit.
    bool append(QByteArrayView more) noexcept;

private:
    void letGo() noexcept;

    std::unique_ptr<quint8[]> m_bytes;
    qsizetype m_room = 0;
    qsizetype m_size = 0;
};

// Writes noughts over what a QByteArray or a QString holds, where it is the one that holds it: it
// is as long as it was, of noughts. Where another holds the same (Qt's copies are one until one
// of them is written to: a secret store in memory, a driver that was handed the text), nothing is
// done here: what they hold is theirs, and writing would only make a copy to write over.
void wipeInPlace(QByteArray& bytes) noexcept;
void wipeInPlace(QString& text) noexcept;

// A QByteArray or a QString that is a secret where it is held, written over (wipeInPlace) when
// this goes: on every way out of where both stand, an exception's too. Made after what it holds,
// so that it goes first. What is handed on (`handOn`) is the receiver's, and is not written over
// here: what it holds is nothing from then on.
template <typename Text>
class WipedOnLeaving {
public:
    explicit WipedOnLeaving(Text& held) noexcept : m_held(held) {}
    WipedOnLeaving(const WipedOnLeaving&) = delete;
    WipedOnLeaving& operator=(const WipedOnLeaving&) = delete;
    WipedOnLeaving(WipedOnLeaving&&) = delete;
    WipedOnLeaving& operator=(WipedOnLeaving&&) = delete;
    ~WipedOnLeaving() { wipeInPlace(m_held); }

    Text handOn() noexcept { return std::exchange(m_held, Text()); }

private:
    Text& m_held;
};

// A text that is a secret for as long as it is held: a KVM's password, in its driver for as long as that signs in with
// it (from the outside review of the desktop, F5). Characters of its own, which no QString shares (Qt's copies are one
// until one of them is written to, and what another holds cannot be written over here: WipedOnLeaving passes it by),
// in one place that is written over when this goes, or is given another. Never copied by oversight; what is moved from
// holds nothing afterwards.
class SecretText {
public:
    SecretText() = default;
    // A copy of its own of `text`; and `text` written over where it held its characters alone (wipeInPlace), and
    // emptied. (A driver is handed a QString: where it was handed the only one, nothing of it is left behind.)
    static SecretText takenFrom(QString& text);
    SecretText(const SecretText&) = delete;
    SecretText& operator=(const SecretText&) = delete;
    SecretText(SecretText&& other) noexcept;
    SecretText& operator=(SecretText&& other) noexcept;
    ~SecretText();

    QStringView view() const noexcept { return {m_chars.get(), m_size}; }
    bool isEmpty() const noexcept { return m_size == 0; }

private:
    void letGo() noexcept;

    std::unique_ptr<char16_t[]> m_chars;
    qsizetype m_size = 0;
};

// The UTF-8 of these texts, one after the other with a nought between each two, in one place that is written over when
// it goes (a surrogate by itself left out, as QString::toUtf8 leaves it out): what a digest of several, a secret among
// them, is made of (SignInPacing::what). Nothing of them is made on the way.
SecretBytes secretUtf8(std::initializer_list<QStringView> texts);

// What a sign-in sends that carries a secret, made once at the size it is to be, of the texts as they are, with nothing
// made beside it: a QByteArray that grows, and every text made of another on the way (UTF-8, a percent-encoded one, a
// JSON document), leaves the secret behind wherever each stood (F5). What is made is the caller's, to make as it goes
// and to write over where nothing went (WipedOnLeaving); what went is written over by its asker once Qt has let go of
// it (DeviceAsker; from the merged review, 3). Each text goes as UTF-8; a surrogate by itself as each of the two writes
// it (QUrl leaves it out, as QString::toUtf8 does; QJsonDocument writes its number).
//
// A form's fields, as a browser sends them (application/x-www-form-urlencoded): "name=value&...", each value
// percent-encoded as QUrl::toPercentEncoding does it (letters, digits and "-._~" as they are).
QByteArray secretForm(std::initializer_list<std::pair<QByteArrayView, QStringView>> fields);
// A JSON object of strings, as QJsonDocument writes it compactly ({"key":"value",...}), in the order given: a quote
// and a backslash set off, the control characters by their short names or their numbers, everything else as it is.
QByteArray secretJson(std::initializer_list<std::pair<QByteArrayView, QStringView>> members);

// What somebody else's memory holds, copied out (what Windows hands over for an entry of its
// store, which Windows frees); and all of it written over there, whatever came of the copy, so
// that what is freed holds nothing of the secret. Copied only where it is no longer than
// `largest`; nothing otherwise, and written over all the same. Nothing at all where it is nowhere.
QByteArray takenOut(void* at, std::size_t bytes, qsizetype largest);

} // namespace qkvm::core
