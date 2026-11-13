#pragma once

#include <functional>
#include <optional>
#include <stop_token>
#include <utility>

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>

#include <qkvm/base/result.h>
#include <qkvm/core/scrypt.h>
#include <qkvm/core/wiped.h>

// The wall as a file, read and written by the program itself (SPEC.md section 4, M3c): the file
// a QKVM server writes and reads (../app/qkvm/exchange.py), so that a server's KVMs come to this
// PC with their passwords, and this PC's go to a server. One format, written two ways: sealed
// with a passphrase, and then with every secret in it (.qkvm); or plain, with every secret left
// out (.json).
//
// What is read is somebody's file, and not this program's own. Nothing is taken from it that the
// server's reader would refuse; where this reader cannot say what the server's would (its
// language's own readers of an address and of a certificate are not here), it is the stricter of
// the two, and exchange.cpp says so where it is. A hostile file costs a bounded amount of memory
// and time and nothing else: the most it may ask of the passphrase's work is `mayBeAsked`.
//
// What comes of reading is what the file holds, and nothing is put anywhere: whoever puts
// devices on a wall decides which of them, and a device of a family this program does not speak
// to is handed back as what it is. A device's address is one where it is one to the program
// itself (kvm::addressFrom, which is the rule the box that adds a KVM has): what this program
// speaks to and a server takes is read, and is written. That rule is the one place where this
// reader takes what a server's may refuse, and it is two kinds of name (exchange.cpp, at
// deviceFrom, has the table): one with a part in its "xn--" form, which Qt reads by one rule
// under the endings it has a list of, and takes as it is under the others, and the server's
// library by another; and one of digits with single signs between them, which the library some
// servers connect with reads as numbers that are no address. The program speaks to a KVM at such a name, so it is read
// and written; and whoever writes a file with one is told beside the file, for every such name and by no guess at which
// of them a server refuses, that a server may not take it.
//
// No secret is in any error or any sentence of what was left out: not the passphrase, not a
// key, not a password, not a text. And no address that was refused: a user name and a password
// may stand in one, and whether they do cannot be told by its signs (an "@" spelled out, a
// password where a port stands, a token behind a question mark). The sentence says which device
// it was, by its number and its name, and what was wrong with its address, in words. A name is
// the file's free text: it stands in a sentence only as nameSetOff() makes it. The
// passphrase's bytes, the key made of it and the file's own key are wiped before their memory is
// given back, and so is the file's content as it stood decrypted. What is handed in as a text
// (the passphrase as it was typed) and what is handed back (a `Content`, with the passwords in
// it) are the caller's, and are wiped by nobody: Qt's texts cannot be, and neither can what the
// reading of JSON made of the content on its way.
namespace qkvm::core::exchange {

// The server's numbers (exchange.py). Lengths are counted in signs (code points), as the server
// counts them: a sign that takes two places in a text of this program's is one.
inline constexpr qsizetype largestPlain = 4'000'000; // signs of what a file holds, written out plain
inline constexpr qsizetype largestSealed = largestPlain * 4 / 3 + 4096;
inline constexpr qsizetype mostDevices = 2000;
inline constexpr qsizetype mostActions = 500;
inline constexpr qsizetype shortestPassphrase = 10;
inline constexpr qsizetype longestPassphrase = 256;
inline constexpr qsizetype longestName = 80;
inline constexpr qsizetype longestUser = 64;
inline constexpr qsizetype longestGroup = 40;
inline constexpr qsizetype longestGroupAsWritten = 200; // before its blanks are made one each
inline constexpr qsizetype longestPassword = 256;
inline constexpr qsizetype longestAddress = 255;
inline constexpr qsizetype longestConsoleLink = 255;
// The one measure of a certificate: the signs of its text as a file holds it. The server asks
// it of the text it reads, as it stands in the file. Here it is asked of that, and of the text
// that is kept (in lines of 64), which is what stands in the next file: a certificate is taken
// where both fit, so that what was read can be written.
inline constexpr qsizetype longestCertificate = 20'000;
inline constexpr qsizetype longestActionName = 60;
inline constexpr qsizetype longestTypedText = 2000;
inline constexpr qsizetype longestProgram = 20;
inline constexpr qsizetype longestAlertAddress = 500; // (check_url in the server's alerts.py)
// So many different certificates of one file that are none to Windows are asked of it, and
// after that one not seen before is none, unasked (MOST_ODD in exchange.py; exchange.cpp says
// what of the server's rule that is, and what of it cannot be said here).
inline constexpr int mostOddCertificates = 256;
// Of this reader's own, where the server has no number: a salt longer than this is none of
// QKVM's (it writes sixteen bytes), and the first step of the passphrase's work costs as much as
// its salt is long.
inline constexpr qsizetype longestSalt = 1024;

// The cost a file is written with (vault.py), and the most a file may ask for when it is read.
inline constexpr scrypt::Cost costAsWritten{quint64{1} << 17, 8, 1};
// Whether a file may ask for this: 2^10 <= N <= 2^20 and a power of two, 1 <= r <= 16,
// 1 <= p <= 4, N * r * p <= 2^21, and N below 2^(16r). No more than twice the work this program
// asks for itself, and 256 MiB: what a file says is what every try of a passphrase costs, and
// the file can be edited.
bool mayBeAsked(const scrypt::Cost& cost) noexcept;

// How a file was refused, in the server's codes, each with the server's sentence.
inline constexpr const char* notAFile = "not_a_file";
inline constexpr const char* newer = "newer";
inline constexpr const char* passphraseNeeded = "passphrase_needed";
inline constexpr const char* wrongPassphrase = "wrong_passphrase";
inline constexpr const char* damaged = "damaged";
inline constexpr const char* tooLarge = "too_large";
// And in codes of the program's own, written with a colon so that no file can say them. What was
// to be written is something no file holds (`notWritten`: its sentence names the first such
// entry of the wall, and what of it); and what scrypt.h and sealing.h say: `scrypt::noMemory`,
// `scrypt::stopped`, and "desktop:crypto" where Windows itself could not.
inline constexpr const char* notWritten = "desktop:not_written";

// Where "Open console" goes, as a file had it: a TEXT, and not an address. It is what the server
// keeps, so that a server that is handed the file again has it back as it was: one that begins
// with http:// or https://, has something where a host stands and no "@" before its path. Its
// host may be in letters beyond ASCII or have a blank in it or be none ("http://:80/"), its port
// may be one no computer has, and its path may hold anything: quotes, signs a command line reads
// (& | < > ^ %), a backslash (which a browser reads as a stroke), a line's end, a sign that turns
// the writing round, signs that print nothing.
//
// So nobody opens it, shows it or writes it to a log as it stands, and it is no QString that
// could be handed on by oversight. There are two ways out of it: asAFileHadIt(), for writing it
// into a file again and for comparing; and consoleAddress(), below, which is the ONE way to an
// address that may be opened or shown.
//
// That is what one is that was READ from a file. One that is handed to the writer is whatever
// the wall has, and is written as it is where it has 255 signs at the most: the writer does not
// judge it, and says beside the file where this reader would leave its device out for it.
class LinkAsWritten {
public:
    LinkAsWritten() = default;
    explicit LinkAsWritten(QString text) : m_text(std::move(text)) {}

    // None: "Open console" goes to the device itself.
    bool isEmpty() const noexcept { return m_text.isEmpty(); }
    // The text. For a file, and for nothing a person sees or a program is started with.
    const QString& asAFileHadIt() const noexcept { return m_text; }

    bool operator==(const LinkAsWritten&) const = default;

private:
    QString m_text;
};

// An address that may be opened and shown: what consoleAddress() hands out, and all it hands
// out. Every word of it is ASCII, and there is no way from it to a name in other letters: a
// name in its "xn--" form stays in that form. It is no QUrl, and holds none: a QUrl's own words
// (host(), toString(), toDisplayString()) show such a name in the letters it stands for under
// the endings Qt has a list of, so that "xn--80ak6aa92e.com" reads as the Cyrillic look-alike
// of a well-known name. Whoever opens it hands toOpen() to what opens addresses; whoever shows
// it shows host() first; and whoever wants the letters a name stands for makes them of host()
// and shows them BESIDE the "xn--" form, never in its place.
class ConsoleAddress {
public:
    // All of it, as it is to be opened, written the one way: "https://" or "http://" in small
    // letters, the host as host() has it, the port where it is not the one such an address has
    // anyway, and what stands behind them as the link had it.
    const QString& toOpen() const noexcept { return m_toOpen; }
    // Its host: a name in small letters, with every part in its "xn--" form that was written
    // in it; four plain numbers; or numbers of the new kind in brackets, the short way.
    const QString& host() const noexcept { return m_host; }
    // The port it is opened at: the one that was written, or the one it has anyway (443, 80).
    int port() const noexcept { return m_port; }
    // Whether it is spoken to over HTTPS. What is typed into a page that was opened over plain
    // HTTP goes in the clear.
    bool isEncrypted() const noexcept { return m_encrypted; }
    // Whether it is at this PC itself: "localhost" or a name that ends in ".localhost",
    // 127.x.x.x, [::1], and the address of no place (0.0.0.0, [::]), which many a system
    // takes for itself. Such a link is handed out (a console is often reached through a tunnel
    // that ends at this PC), and a file from somebody else can point "Open console" at
    // whatever listens here: whoever shows a link from a file says so where this is true.
    bool isThisPcsOwn() const noexcept { return m_thisPcsOwn; }

    bool operator==(const ConsoleAddress&) const = default;

private:
    // Made by consoleAddress() alone, which looks at every sign of every word.
    friend std::optional<ConsoleAddress> consoleAddress(const LinkAsWritten& link);
    ConsoleAddress() = default;

    QString m_toOpen;
    QString m_host;
    int m_port = 0;
    bool m_encrypted = false;
    bool m_thisPcsOwn = false;
};

// The address a console link is, where it is one that may be opened and shown; nothing where it
// is not, and nothing for no link. By the strict rule, which takes far less than a file may hold:
// * nothing in it but the plain signs from behind the blank to the tilde, and of those none of
//   " ' ` & | < > ^ % and no backslash: no blank, no letter beyond ASCII, nothing that prints
//   nothing;
// * "http://" or "https://" in front, in letters small or large (compared sign for sign);
// * behind that a host and a port by the program's own rule for a KVM's address
//   (kvm::addressFrom): a name of letters, digits, dots, hyphens and underscores, four numbers
//   written the plain way, or numbers of the new kind in brackets; a port from 1 to 65535, or
//   none; nobody's name and nobody's password; and no dot at the name's end, which that rule
//   drops (what would be opened is then not what was written);
// * and Qt, which is who would hand it on, reads it by the book (QUrl's strict mode) as an
//   address of that same scheme, host and port, and writes what stands behind them, fully
//   spelled out, sign for sign as the link has it.
// An "@" behind the host, in a path or behind a question mark or a cross, is a sign of an
// address and names nobody; a cross and what stands behind it are part of what is opened.
// (A link with "%20" or "a=1&b=2" in it is none by this rule. Whoever builds "Open console"
// and wants those decides which of them a browser may be handed, here, with tests.)
std::optional<ConsoleAddress> consoleAddress(const LinkAsWritten& link);

// A name from a file, set off for a sentence or a list: in double quotes, and so that it cannot
// end its own setting-off. It is the ONE way a name of a file's is shown, here and by whoever
// shows a device of a file. Nothing where there is no name to show, and then the device is told
// by its number or its address.
// * A name is shown where it is a short line of text by the rule a file has for one; nothing
//   otherwise.
// * No double quote stands in what is shown: one in the name, and each sign that looks like
//   one (the curled and low ones U+201C to U+201F, the double primes U+2033 and U+2036,
//   U+02BA, U+02DD, U+02EE, U+3003, U+301D to U+301F, and the one of full width U+FF02), is
//   shown as an apostrophe; and of apostrophes side by side, the name's own or made so, one is
//   shown, since two look like a double quote. So `x"): taken. Device 2 ("Rack 1` cannot
//   close its quotes and go on as the sentence. (Signs that look like an apostrophe are many,
//   and two of those may look like a double quote still: what is sure is the sign, and not
//   the look.)
// * The three marks that say which way the writing runs, and are let through by the rule for
//   a line (U+200E, U+200F, U+061C), are not shown.
// * A name of nothing to see is no name: one made only of blanks, of signs that print nothing
//   (U+00AD, a joiner), of marks that stand on a letter with no letter there, and of the
//   fillers that count as letters and show nothing (U+115F, U+1160, U+3164, U+FFA0) or as a
//   picture of nothing (U+2800).
// What a file's name is KEPT as (Device::name) is what the server keeps of it, and is not this.
QString nameSetOff(const QString& name);

// Where a device stood in the list of devices of the file it was read from: counted from 1, as
// the sentences of what was left out count ("Device 3: ..."), so that a device that is shown and
// a sentence beside it speak of one list. 0: it was read from no file.
//
// It is no part of what a device is. Two devices are the same wherever each of them stood, a
// device is written without it, and what was written and read back is what was written: so
// every place compares as equal to every other.
class PlaceInAFile {
public:
    constexpr PlaceInAFile() noexcept = default;
    constexpr explicit PlaceInAFile(qsizetype counted) noexcept : m_counted(counted) {}

    constexpr qsizetype counted() const noexcept { return m_counted; }

    constexpr bool operator==(const PlaceInAFile& /*other*/) const noexcept { return true; }

private:
    qsizetype m_counted = 0;
};

// One device of a file, as this reader took it. Not everything here passed the server's rule
// for it: the address passed the program's own (kvm::addressFrom), which takes two kinds of
// name a server may not; the certificate passed Windows, where the server asks two libraries
// of its own; and each of the other fields passed the server's rule for it as far as that is
// said in exchange.cpp, which says where it takes less. (None of these types can be written to
// a log as it is, and none is to be taught to: a device has its password in it, an action its
// text.)
struct Device {
    QString name;     // never empty: one that has none is named by its address
    QString address;  // "https://host", "http://host:port": written the one way (kvm::addressFrom)
    QString user;     // never empty: "admin" where the file names nobody
    QString password; // as it stands in the file, whatever is in it; empty: none
    QString kind;     // "kvmd", "jetkvm" or "redfish"
    QString group;
    QString tls; // how its certificate is checked: "pinned", "system" or "insecure"
    // Over HTTPS and "pinned": the certificate it is known by, as the server keeps one (PEM, one
    // certificate, in lines of 64), and that certificate's SHA-256, which is what a KVM is known
    // by here. Both empty: none is remembered.
    QString certificatePem;
    QByteArray certificate;
    // Where "Open console" goes, as the file had it: a text for the next file, and no address
    // (LinkAsWritten, above). What may be opened or shown of it is consoleAddress() of it.
    LinkAsWritten consoleLinkAsWritten;
    bool alerts = true;
    QString ident; // what the device says it is, as a driver gave it; empty: nothing (isADevicesOwnNumber)
    // The machine its card was added for, by the numbers it gave for itself (OwnKvm::machine; the web's `machine`),
    // as a driver gives them (isADevicesOwnNumber); empty: none kept. A file that has another shape there leaves the
    // device out: no "none", which would have its card take whatever answers first. A server's from a file of
    // 0.1.x, which has no `machine` and kept the number its computer gives as its `ident`: that.
    QString machine;
    bool keepBoth = false; // somebody said it is to have a tile of its own beside its twin
    // Where it stood in the file it was read from (PlaceInAFile, above): nothing a file holds.
    PlaceInAFile place;

    bool operator==(const Device&) const = default;
};

// A certificate as a file holds one, and as this PC's wall keeps one beside a KVM that is known
// by it: its text as the server keeps one (PEM, one certificate, in lines of 64), and its
// SHA-256, which is what a KVM is known by here.
struct KeptCertificate {
    QString pem;
    QByteArray sha256;

    bool operator==(const KeptCertificate&) const = default;
};

// The certificate a text is, by every rule this reader has for the certificate of a device in a
// file (exchange.cpp, at bytesOfACertificate): one certificate between its two lines, no longer
// than a file holds, as it was written and as it is kept, and one that Windows reads as a
// certificate. Nothing: it is none of that, and nothing is made of it.
std::optional<KeptCertificate> certificateFrom(const QString& written);
// Whether a text is a number a device gives for itself, as a driver gives one (Device::ident,
// OwnKvm::number): two to twelve small letters, a colon, and sixteen to sixty-four digits to the base
// of sixteen; and not one of the marks a QKVM makes itself ("own:", "cert:").
bool isADevicesOwnNumber(QStringView text) noexcept;
// The same of a certificate's bytes as a KVM showed them on a line (DER): what a file would hold
// of it. Nothing where a file would hold none of it: it is too long for one, or Windows does
// not read these bytes as a certificate.
std::optional<KeptCertificate> certificateOf(const QByteArray& der);

// What this reader names a device that has no name in its file (`Device::name` is never empty):
// its address without the way it is spoken to ("host", "host:port"), no longer than a name may
// be. As the server names one. Whoever takes a device from a file onto a wall of its own asks
// by this whether the device has a name at all: a KVM that is named by its address would keep
// the address it had as its name when it is moved to another (wall_exchange.cpp).
QString nameOfADeviceWithoutOne(const QString& address);

// A device of a wall as a line beside a file begins with it: by its place on the wall, counted
// from 0 here and from 1 in the line, and by its name as nameSetOff() makes it where it has one
// to show. The one way such a line names its device, for the writer's own lines and for those of
// whoever hands the writer a wall and has something to say of a device of it beside the file.
QString deviceOfTheWall(qsizetype index, const QString& name);

// One saved action: keys pressed together, or a text that is typed.
struct Action {
    QString name;
    bool types = false; // it types `text`; otherwise it presses `keys`
    QString keys;       // "ControlLeft,AltLeft,Delete"
    QString text;       // as it stands in the file, whatever is in it
    bool enter = false; // Enter after its text
    bool danger = false;

    bool operator==(const Action&) const = default;
};

struct Alerts {
    bool enabled = false;
    // How alerts are written for the service they go to, in the server's word for it: "auto",
    // "json", "ntfy", "slack" or "discord" (FORMATS in its alerts.py). Read from a file it is
    // one of those, and "auto" where the file has another word, as the server reads it; and
    // nothing is written with another word.
    QString format = QStringLiteral("auto");
    bool offline = true;
    bool noSignal = true;
    bool problem = true;
    bool recovered = true;
    QString address; // where alerts go (it often holds a token); empty: the file names none

    bool operator==(const Alerts&) const = default;
};

struct WallSettings {
    std::optional<double> pictureEverySeconds;
    std::optional<bool> asksForNewerVersions;

    bool operator==(const WallSettings&) const = default;
};

// What a file holds, as far as it passed. Each part is nothing where the file has no such part.
struct Content {
    std::optional<QList<Device>> devices;
    std::optional<QList<Action>> actions;
    std::optional<Alerts> alerts;
    std::optional<WallSettings> wall;
    // What did not pass, each in a sentence: an entry that is left out, or the one thing of the
    // settings that is not kept while the rest is. In the server's sentences where the server
    // would say one, but for three things. A device whose address is none: the server's
    // sentence shows the address, and this one says what is wrong with it, and whose device it
    // was. A name, wherever a sentence has one: set off as nameSetOff() makes it, which the
    // server's is not. And what a server takes and this reader does not (a certificate that is
    // too long once it is kept in lines, an address for alerts that only a library of the
    // server's can judge): said as that, and not in the server's words for what is none.
    QStringList leftOut;
    bool sealed = false; // it was encrypted
    double made = 0;     // seconds since 1970; 0: the file does not say
    QString program;     // the version that wrote it

    // Whether it holds anything that is kept encrypted on a wall: a password, a text to type,
    // an address alerts go to.
    bool hasSecrets() const;

    bool operator==(const Content&) const = default;
};

// Whether a file says of itself that it is sealed, and so has a passphrase to be asked for.
// Refused as read() refuses what is no file of QKVM's at all. Quick.
base::Result<bool> isSealed(const QByteArray& file);

// What a file holds: `file` is its bytes as they are, `passphrase` what somebody typed for it,
// empty for none. Refused in the server's codes and sentences:
//   too_large           more signs than a file of QKVM's has
//   not_a_file          not JSON, not a QKVM file, or no version this reads
//   newer               written by a later QKVM
//   passphrase_needed   it is sealed, and no passphrase was given
//   wrong_passphrase    its lock does not open with this passphrase, or is none
//   damaged             its content does not open with its own key, or is not what QKVM writes
// An entry that breaks a rule is left out with a sentence (`leftOut`) and the rest is read.
//
// SLOW for a sealed file, and not for the interface thread: the passphrase is put through as
// much work as the file asks for (about 128 MiB and half a second as QKVM writes one, where this
// was written; twice that at the most). `stop`: nobody waits any more (`scrypt::stopped`). From
// any thread; off the interface thread there is FileWork (exchange_work.h).
base::Result<Content> read(const QByteArray& file, const QString& passphrase, const std::stop_token& stop = {});

// What whoever takes a file's text says in taking it: that what the file does not hold of the
// wall it was written from (Written::notInIt) was looked at first, by whoever asked for the file
// where there is anything to look at. One word, said by its name and in no other way: not by
// empty braces and not by a number that is made one. (It was the first of a list of words
// once, and both of those say the first of a list: `written.file({})` compiled.)
class NotInIt {
public:
    static const NotInIt wasLookedAt;

private:
    constexpr NotInIt() noexcept = default;
};
inline constexpr NotInIt NotInIt::wasLookedAt{};

// A file as it was written, and what of the wall it was written from is not in it. The two do
// not come apart by oversight: there is no way to the text that does not say that the lines
// were looked at, and whoever forgets them does not compile.
class Written {
public:
    Written() = default;
    Written(QByteArray file, QStringList notInIt) : m_file(std::move(file)), m_notInIt(std::move(notInIt)) {}

    // A line for each device of which the file says less than the wall knows, or says it to
    // this program and maybe not to a server. Each names its device by its place on the wall,
    // and by its name as nameSetOff() makes it where it has one to show.
    // * A device at a name with a part in its "xn--" form, anywhere in the name: it is in the
    //   file, and this program reads it back; a server may not take such a name. For every
    //   such name, and by no guess at which of them a server refuses.
    // * A device at a name that reads as digits, a sign, digits, a sign, digits, a sign,
    //   digits, and is not four plain numbers ("192-168-1-20", "1.2.3-4"): the library some
    //   servers connect with reads such a name as numbers that are no address.
    // * A device whose console link is one this reader would leave the device out for: the
    //   link is written as it was (it is a text as a file had it), and a server may leave the
    //   device out as this reader does.
    // * A device that is known by its certificate's SHA-256 alone (`certificate` without
    //   `certificatePem`, over HTTPS and "pinned") is written as "pinned, and no certificate is
    //   remembered": a file holds the certificate itself, or none. A server that reads it takes
    //   the first certificate that device shows: what was a pin is trust at first sight there.
    const QStringList& notInIt() const noexcept { return m_notInIt; }
    // The file's text.
    const QByteArray& file(NotInIt /*wasLookedAt*/) const noexcept { return m_file; }

private:
    QByteArray m_file;
    QStringList m_notInIt;
};

// The text of a file that holds `content`: sealed with `passphrase` as the server seals one
// (N = 2^17, r = 8, p = 1, a salt of 16 bytes, nonces of 12, all of them new), or plain where
// the passphrase is empty, and then without any secret: no password, no saved text, no address
// alerts go to. `leftOut` and `sealed` of the content are not looked at; `made` is written
// where it is a time a file can say (above 0, and below what a reader takes), in whole seconds,
// and the time it is now otherwise.
//
// Whatever is handed in as a device's address is taken by the program's own rule first
// (kvm::addressFrom) and written the one way, as a file has it: "HTTPS://KVM.Example:443/" is
// written as "https://kvm.example", and what is said of a device goes by that.
//
// What is written is what this reader reads back, but for one thing that is said beside the
// file: a console link is a text as a file had it, and is written as it was, whatever this
// program would make of it (a line of `notInIt` says where this reader would leave the device
// out for it). And what this reader reads back is what a server's reads, but for what a line
// of `notInIt` says too: a name a server may not take. So that is the promise: whatever of the
// wall a server may not read from this file is either not written at all, with a sentence, or
// is written and named in a line beside the file. Nothing of it is guessed to be well.
//
// More devices or saved actions than a file holds, and more than a file's length, are
// `too_large` in the server's sentences, before anything is made of the passphrase. And what
// was to be written is read once as a file would be. What this reader takes is what the program
// speaks to, so a wall of the program's own is written whole; where an entry is something no
// file of QKVM's holds all the same (an address that is none, a name of ninety signs, two
// devices at one address, a console link of more than 255 signs, a word for how alerts are
// written that the server does not have), NOTHING is written, and the sentence (`notWritten`)
// names that entry by its place on the wall and its name, and says what of it, in words for
// somebody who is writing a file. Nothing, and not the rest with a line in `notInIt`: a file is
// kept for the day its wall is gone, and one that is there, and is short of a device that
// nobody remembers having been told of, is believed to hold it. No file and a sentence cannot
// be taken for a backup. (Whoever calls may take the entry off what is handed in and ask again:
// then it is a person who left it out, and knows.) Where it is this program alone that does not
// write a thing a server's file may hold (an address for alerts that only a library of the
// server's can judge), the sentence says that, and not that no file holds it.
//
// What a file cannot say of the wall, or says to this program and maybe not to a server, is
// handed back beside it (`Written::notInIt`), and the file is written all the same: whether
// that is good enough is the caller's to say, and to tell whoever asked for the file. This does
// not decide it silently.
//
// Whether the passphrase is one (passphraseRefused) is the caller's to ask first: this seals with
// whatever is given. SLOW with a passphrase, as read() is.
base::Result<Written> write(const Content& content, const QString& passphrase, const std::stop_token& stop = {});

// Why this is no passphrase to seal a file with: fewer than ten signs, more than 256, or half a
// character in it. Nothing: it is one. The rule is the server's; the sentences are this
// program's ("invalid").
std::optional<base::Error> passphraseRefused(const QString& passphrase);

// --- for the tests, and for FileWork ---------------------------------------------------------

// The passphrase's work, as read() and write() have it done: scrypt::derive into a key. A test
// hands in one that counts how often it was asked.
using Derive = std::function<base::Result<SecretKey>(QByteArrayView passphrase, QByteArrayView salt,
                                                     const scrypt::Cost& cost, const std::stop_token& stop)>;
Derive windowsDerive();

// How a file is sealed. The program itself seals with `Sealing()`; a test names a cost that it
// can pay a hundred times. No cost is written that a file may not ask for (`mayBeAsked`).
struct Sealing {
    scrypt::Cost cost = costAsWritten;
    Derive derive = windowsDerive();
};

// What somebody typed as a passphrase, as its bytes (SecretBytes::utf8Of), which are wiped when
// this goes. Nothing: what was typed is no text, since it has half a character in it. A plain
// file is read all the same, a sealed one does not open with it, and none is written with it.
using Passphrase = std::optional<SecretBytes>;

// read() and write() with the passphrase as its bytes, and with the work of a test's choosing.
base::Result<Content> readWith(const QByteArray& file, const Passphrase& passphrase, const Derive& derive,
                               const std::stop_token& stop = {});
base::Result<Written> writeWith(const Content& content, const Passphrase& passphrase, const Sealing& how,
                                const std::stop_token& stop = {});

} // namespace qkvm::core::exchange
