// The wall as a file, read and written by the program itself (core/exchange), by itself and
// without a window: for a person who wants to see what a file holds, and for trying what this
// program writes against a QKVM server's own reader.
//
//     qkvm-exchange-rig --read <file> [--passphrase-from <NAME>]
//     qkvm-exchange-rig --write <file> [--passphrase-from <NAME>] [--alerts-as <word>] [--alerts-to <address>]
//     qkvm-exchange-rig --read <file> --write <file> [--passphrase-from <NAME>]
//
// The passphrase is what the environment variable NAME holds, and comes from nowhere else: not
// from the command line, where every program on the PC can read it. Without one a file is read
// as a plain one, and written plain, with no secret in it.
//
// --read says what the file holds, entry by entry, and never a password or a text that is typed:
// of those only how long they are. Nor a console link as a file had it, which may hold
// anything: the address it is by the one way there is to one, in that address's own words,
// which are ASCII (its host in its "xn--" form, its port, whether it is encrypted, all of it as
// it would be opened), or how long it is. A name of the file's is printed set off, as the one
// function for that makes it. --write writes a wall that is made up here (addresses of the kind
// manuals use, passwords that are words of this file; one device at a name in its "xn--" form,
// typed in capitals, one at such a name that a server does not take, one at a name of digits
// and hyphens, one whose console link is no address, and one whose console link this program
// leaves a device out for) as the program writes one, sealed at the full cost, and says what of
// that wall the file does not hold, or holds for this program and maybe not for a server,
// before it takes the file's text; --alerts-as and --alerts-to
// make its alerts written another way, and go to another made-up address, than the rig's own.
// With both --read and --write, what was read of the first file is written into the second, with
// the same passphrase: for seeing that a server reads back what it wrote, after this program had
// it. All of it is done off the interface thread, as the program will do it
// (core/exchange_work). Ends with 0 where the file was read or written, and with 1 where it was
// refused, with the code and the sentence it was refused in.

#include <cstdio>
#include <exception>
#include <optional>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>

#include <qkvm/core/exchange.h>
#include <qkvm/core/exchange_work.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

void say(const QString& line)
{
    std::fputs(qPrintable(line + u'\n'), stdout);
    std::fflush(stdout);
}

QString why(const base::Error& error)
{
    return u"%1: %2"_s.arg(QString::fromStdString(error.code), QString::fromStdString(error.message));
}

QString yesNo(bool value)
{
    return value ? u"yes"_s : u"no"_s;
}

// How long a secret is, and nothing else of it.
QString lengthOf(const QString& secret)
{
    return secret.isEmpty() ? u"none"_s : u"%1 places"_s.arg(secret.size());
}

// A device's console link, as far as it may be shown: the address it is by the one way there is
// to one (core::exchange::consoleAddress), in that address's own words, every one of which is
// ASCII (a name in its "xn--" form stays in it); and of a link that is none by that rule only
// how long it is. What a file kept of it is a text that may hold anything, and is printed
// nowhere.
QString shownLink(const core::exchange::LinkAsWritten& link)
{
    if (link.isEmpty()) {
        return u"none"_s;
    }
    const std::optional<core::exchange::ConsoleAddress> address = core::exchange::consoleAddress(link);
    if (!address) {
        return u"kept as written, %1 places, and no address this program would open"_s.arg(link.asAFileHadIt().size());
    }
    return u"to %1, port %2, %3%4 (%5)"_s.arg(
        address->host(), QString::number(address->port()), address->isEncrypted() ? u"encrypted"_s : u"NOT encrypted"_s,
        address->isThisPcsOwn() ? u", at this PC itself"_s : QString(), address->toOpen());
}

// A name of a file's, or another short text of one (a user, a group, the version that wrote
// it), as it is shown: set off by the one function there is for that
// (core::exchange::nameSetOff); "none" for none; and where there is one of which nothing can be
// shown, that.
QString shownName(const QString& name)
{
    QString setOff = core::exchange::nameSetOff(name);
    if (!setOff.isEmpty()) {
        return setOff;
    }
    return name.isEmpty() ? u"none"_s : u"(one of which nothing can be shown)"_s;
}

void sayContent(const core::exchange::Content& content)
{
    say(u"sealed: %1 | made: %2 | program: %3 | holds secrets: %4"_s.arg(yesNo(content.sealed))
            .arg(content.made, 0, 'f', 0)
            .arg(shownName(content.program), yesNo(content.hasSecrets())));
    if (content.devices) {
        say(u"devices: %1"_s.arg(content.devices->size()));
        for (const core::exchange::Device& device : *content.devices) {
            say(u"  %1 at %2 | user %3 | password: %4 | kind %5 | group %6 | tls %7"_s.arg(
                shownName(device.name), device.address, shownName(device.user), lengthOf(device.password), device.kind,
                shownName(device.group), device.tls));
            say(u"    certificate: %1 | console link: %2 | alerts %3 | ident \"%4\" | keep both %5"_s.arg(
                device.certificate.isEmpty() ? u"none"_s
                                             : u"sha256:"_s + QString::fromLatin1(device.certificate.toHex()),
                shownLink(device.consoleLinkAsWritten), yesNo(device.alerts), device.ident, yesNo(device.keepBoth)));
        }
    } else {
        say(u"devices: not in the file"_s);
    }
    if (content.actions) {
        say(u"saved actions: %1"_s.arg(content.actions->size()));
        for (const core::exchange::Action& action : *content.actions) {
            say(u"  %1 | %2 | Enter after it: %3 | asks first: %4"_s.arg(
                shownName(action.name),
                action.types ? u"types a text of %1"_s.arg(lengthOf(action.text)) : u"presses %1"_s.arg(action.keys),
                yesNo(action.enter), yesNo(action.danger)));
        }
    } else {
        say(u"saved actions: not in the file"_s);
    }
    if (content.alerts) {
        const core::exchange::Alerts& alerts = *content.alerts;
        say(u"alerts: enabled %1 | format %2 | offline %3 | no signal %4 | problem %5 | recovered %6 | address: %7"_s
                .arg(yesNo(alerts.enabled), alerts.format, yesNo(alerts.offline), yesNo(alerts.noSignal),
                     yesNo(alerts.problem), yesNo(alerts.recovered), lengthOf(alerts.address)));
    } else {
        say(u"alerts: not in the file"_s);
    }
    if (content.wall) {
        const core::exchange::WallSettings& wall = *content.wall;
        say(u"wall: a picture every %1 | asks for newer versions: %2"_s.arg(
            wall.pictureEverySeconds ? u"%1 s"_s.arg(*wall.pictureEverySeconds) : u"(not said)"_s,
            wall.asksForNewerVersions ? yesNo(*wall.asksForNewerVersions) : u"(not said)"_s));
    } else {
        say(u"wall: not in the file"_s);
    }
    say(u"left out: %1"_s.arg(content.leftOut.size()));
    for (const QString& sentence : content.leftOut) {
        say(u"  "_s + sentence);
    }
}

// A certificate made for nobody (the one in core/tests/data/export, which a script made up).
QString madeUpCertificate()
{
    return u"-----BEGIN CERTIFICATE-----\n"
           "MIIBHzCBxqADAgECAgID6DAKBggqhkjOPQQDAjAZMRcwFQYDVQQDDA5hbiBleGFt\n"
           "cGxlIEtWTTAeFw0yNjAxMDEwMDAwMDBaFw0zNTEyMzAwMDAwMDBaMBkxFzAVBgNV\n"
           "BAMMDmFuIGV4YW1wbGUgS1ZNMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEkoYW\n"
           "6kW9oO3ILuXrYCeiDLKpt4eq/ON7DwCe1F6av2t9ygp9QbxUMJBePxGOkOLuiQoT\n"
           "JW8q35yG1qcAPd9gujAKBggqhkjOPQQDAgNIADBFAiEA3xyTzdfJz+sYMmDqd0/9\n"
           "f18OyJhT9K1FPBtZBVRLdj8CICcxZ6T++/Z1QCNp3ZqXhAsCk9P+kXQrJuCsPA43\n"
           "COJH\n"
           "-----END CERTIFICATE-----\n"_s;
}

// A wall that is made up here: one device of each family, at each way of checking a
// certificate; a name with a sign that is no plain letter and one that takes two places in a
// text (made from their numbers); a password with the signs a file has to spell out.
core::exchange::Content madeUpWall()
{
    using core::exchange::Action;
    using core::exchange::Device;
    const char32_t wide = 0x1F5A5; // a desktop computer
    core::exchange::Content content;
    content.program = u"0.1.0"_s;

    Device first;
    first.name = u"Rig 1, t"_s + QChar(0xfc) + u"r "_s + QString::fromUcs4(&wide, 1);
    first.address = u"https://192.168.1.51"_s;
    first.user = u"admin"_s;
    first.password = u"rig password \"1\" with a \\ and an "_s + QChar(0xe4);
    first.kind = u"kvmd"_s;
    first.group = u"Rig rack"_s;
    first.tls = u"pinned"_s;
    first.certificatePem = madeUpCertificate();
    Device second;
    second.name = u"Rig 2, plain"_s;
    second.address = u"http://192.168.1.52:8080"_s;
    second.user = u"operator"_s;
    second.password = u"rig-password-2"_s;
    second.kind = u"kvmd"_s;
    second.group = u"Rig rack"_s;
    second.tls = u"pinned"_s;
    second.consoleLinkAsWritten = core::exchange::LinkAsWritten(u"https://192.168.1.52/kvm/"_s);
    second.alerts = false;
    Device third;
    third.name = u"Rig 3, by Windows"_s;
    third.address = u"https://kvm.example.net"_s;
    third.user = u"admin"_s;
    third.password = u"rig-password-3"_s;
    third.kind = u"kvmd"_s;
    third.tls = u"system"_s;
    // (Its console link is at a name in its "xn--" form under an ending Qt has on its list: a
    // QUrl shows that name in Cyrillic letters, and what --read prints of it is ASCII.)
    third.consoleLinkAsWritten = core::exchange::LinkAsWritten(u"https://xn--80ak6aa92e.com/console"_s);
    Device fourth;
    fourth.name = u"Rig 4, a small one"_s;
    fourth.address = u"http://192.168.1.60"_s;
    fourth.user = u"admin"_s;
    fourth.password = u"rig-password-4"_s;
    fourth.kind = u"jetkvm"_s;
    fourth.group = u"Desk"_s;
    fourth.tls = u"pinned"_s;
    fourth.ident = u"jetkvm:0123456789abcdef"_s;
    Device fifth;
    fifth.name = u"Rig 5, a server"_s;
    fifth.address = u"https://[2001:db8::7]:8443"_s;
    fifth.user = u"root"_s;
    fifth.password = u"rig-password-5"_s;
    fifth.kind = u"redfish"_s;
    fifth.group = u"Rig rack 2"_s;
    fifth.tls = u"insecure"_s;
    fifth.keepBoth = true;
    // One that is known as this PC's wall knows its KVMs: by what its certificate's SHA-256 is,
    // and not by the certificate. A file cannot say that, and --write says so beside the file.
    Device sixth;
    sixth.name = u"Rig 6, known by a fingerprint"_s;
    sixth.address = u"https://192.168.1.56"_s;
    sixth.user = u"admin"_s;
    sixth.password = u"rig-password-6"_s;
    sixth.kind = u"kvmd"_s;
    sixth.group = u"Rig rack"_s;
    sixth.tls = u"pinned"_s;
    sixth.certificate = QByteArray(32, '\x5a');
    // One at a name in letters beyond ASCII, in its short form, which this program speaks to and
    // a server takes; handed in as somebody may have typed it, in capitals and with a path.
    Device seventh;
    seventh.name = u"Rig 7, at a name in its xn-- form"_s;
    seventh.address = u"HTTPS://XN--mnchen-3ya.example:443/kvm/"_s;
    seventh.user = u"admin"_s;
    seventh.password = u"rig-password-7"_s;
    seventh.kind = u"kvmd"_s;
    seventh.group = u"Rig rack 2"_s;
    seventh.tls = u"system"_s;
    // And one whose console link is none this program would open, which a server keeps all the
    // same: a blank, quotes and a sign a command line reads.
    Device eighth;
    eighth.name = u"Rig 8, with a console link that is no address"_s;
    eighth.address = u"https://192.168.1.58"_s;
    eighth.user = u"admin"_s;
    eighth.password = u"rig-password-8"_s;
    eighth.kind = u"kvmd"_s;
    eighth.tls = u"insecure"_s;
    eighth.consoleLinkAsWritten = core::exchange::LinkAsWritten(u"https://bench.example/a \"b\"&c"_s);
    // And one at a name that this program speaks to and a server does not take: a picture, in
    // its "xn--" form. It is written, and --write says beside the file that a server may leave
    // it out.
    Device ninth;
    ninth.name = u"Rig 9, at a name a server may not take"_s;
    ninth.address = u"https://xn--ls8h.example"_s;
    ninth.user = u"admin"_s;
    ninth.password = u"rig-password-9"_s;
    ninth.kind = u"kvmd"_s;
    ninth.tls = u"system"_s;
    // One whose console link is one this program leaves a device out for when it reads a file
    // (numbers of the new kind with a zone), and which a server keeps: it is written as it was,
    // and --write says so beside the file.
    Device tenth;
    tenth.name = u"Rig 10, with a console link this program does not take"_s;
    tenth.address = u"https://192.168.1.59"_s;
    tenth.user = u"admin"_s;
    tenth.password = u"rig-password-10"_s;
    tenth.kind = u"kvmd"_s;
    tenth.tls = u"insecure"_s;
    tenth.consoleLinkAsWritten = core::exchange::LinkAsWritten(u"https://[fe80::1%25eth0]/"_s);
    // And one at a name of digits and hyphens, which the library some servers connect with
    // reads as numbers that are no address.
    Device eleventh;
    eleventh.name = u"Rig 11, at a name of digits and hyphens"_s;
    eleventh.address = u"https://192-168-1-20"_s;
    eleventh.user = u"admin"_s;
    eleventh.password = u"rig-password-11"_s;
    eleventh.kind = u"kvmd"_s;
    eleventh.tls = u"insecure"_s;
    content.devices =
        QList<Device>{first, second, third, fourth, fifth, sixth, seventh, eighth, ninth, tenth, eleventh};

    Action keys;
    keys.name = u"Ctrl+Alt+Del"_s;
    keys.keys = u"ControlLeft,AltLeft,Delete"_s;
    keys.danger = true;
    Action text;
    text.name = u"Sign in"_s;
    text.types = true;
    text.text = u"rig text to type, line one\nand line two"_s;
    text.enter = true;
    content.actions = QList<Action>{keys, text};

    core::exchange::Alerts alerts;
    alerts.enabled = true;
    // (Written for one service, and not left to the server: a file has to carry that.)
    alerts.format = u"ntfy"_s;
    alerts.recovered = false;
    alerts.address = u"https://hooks.example.net/rig-made-up-token"_s;
    content.alerts = alerts;

    core::exchange::WallSettings wall;
    wall.pictureEverySeconds = 2.5;
    wall.asksForNewerVersions = false;
    content.wall = wall;
    return content;
}

// What stands behind a switch on the command line; nothing where the switch is not there.
std::optional<QString> behind(const QStringList& arguments, QLatin1StringView name)
{
    const qsizetype at = arguments.indexOf(name);
    return at < 0 || at + 1 >= arguments.size() ? std::nullopt : std::optional<QString>(arguments.at(at + 1));
}

// What is said when a file was written or was not, and what the rig then ends with: the file is
// put where it was asked for, and what it does not hold of the wall is said.
void sayWritten(const QString& name, qint64 afterMs, const base::Result<core::exchange::Written>& written)
{
    say(u"after %1 ms"_s.arg(afterMs));
    if (!written) {
        say(u"refused: "_s + why(written.error()));
        QCoreApplication::exit(1);
        return;
    }
    // A person who keeps the file for the day it is needed has to have seen this: said first,
    // and only then is the text taken (which is what taking it says).
    say(u"not in the file: %1"_s.arg(written.value().notInIt().size()));
    for (const QString& line : written.value().notInIt()) {
        say(u"  "_s + line);
    }
    const QByteArray& text = written.value().file(core::exchange::NotInIt::wasLookedAt);
    QFile file(name);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size()) {
        say(u"%1 cannot be written."_s.arg(name));
        QCoreApplication::exit(2);
        return;
    }
    say(u"%1: %2 bytes written"_s.arg(name).arg(text.size()));
    QCoreApplication::exit(0);
}

int run(int argc, char** argv)
{
    const QCoreApplication app(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    const std::optional<QString> toRead = behind(arguments, "--read"_L1);
    const std::optional<QString> toWrite = behind(arguments, "--write"_L1);
    const std::optional<QString> from = behind(arguments, "--passphrase-from"_L1);
    const std::optional<QString> alertsAs = behind(arguments, "--alerts-as"_L1);
    const std::optional<QString> alertsTo = behind(arguments, "--alerts-to"_L1);
    if (!toRead && !toWrite) {
        say(u"qkvm-exchange-rig --read <file> [--passphrase-from <NAME>]"_s);
        say(u"qkvm-exchange-rig --write <file> [--passphrase-from <NAME>] [--alerts-as <word>] [--alerts-to "
            "<address>]"_s);
        say(u"qkvm-exchange-rig --read <file> --write <file> [--passphrase-from <NAME>]"_s);
        return 2;
    }
    QString passphrase;
    if (from) {
        if (!qEnvironmentVariableIsSet(qPrintable(*from))) {
            say(u"The environment variable %1 is not set."_s.arg(*from));
            return 2;
        }
        passphrase = qEnvironmentVariable(qPrintable(*from));
    }
    say(passphrase.isEmpty() ? u"no passphrase"_s : u"a passphrase of %1 places"_s.arg(passphrase.size()));
    // What a file is sealed with is a passphrase by the program's rule for one, here as there.
    const std::optional<base::Error> refused =
        toWrite && !passphrase.isEmpty() ? core::exchange::passphraseRefused(passphrase) : std::nullopt;
    if (refused) {
        say(u"refused: "_s + why(*refused));
        return 2;
    }

    core::exchange::FileWork work;
    QElapsedTimer watch;
    watch.start();
    if (toRead) {
        QFile file(*toRead);
        // (One byte more than the longest file there is, so that a longer one is seen to be.)
        constexpr qsizetype mostRead = core::exchange::largestSealed * 4 + 1;
        if (!file.open(QIODevice::ReadOnly)) {
            say(u"%1 cannot be opened."_s.arg(*toRead));
            return 2;
        }
        const QByteArray bytes = file.read(mostRead);
        say(u"%1: %2 bytes"_s.arg(*toRead).arg(bytes.size()));
        work.read(bytes, passphrase, &work, [&](const base::Result<core::exchange::Content>& content) {
            say(u"after %1 ms"_s.arg(watch.elapsed()));
            if (!content) {
                say(u"refused: "_s + why(content.error()));
                QCoreApplication::exit(1);
                return;
            }
            sayContent(content.value());
            if (!toWrite) {
                QCoreApplication::exit(0);
                return;
            }
            // What was read, written again: asked for from where the reading is told.
            watch.restart();
            work.write(content.value(), passphrase, &work, [&](const base::Result<core::exchange::Written>& written) {
                sayWritten(toWrite.value_or(QString()), watch.elapsed(), written);
            });
        });
    } else {
        core::exchange::Content wall = madeUpWall();
        core::exchange::Alerts alerts = wall.alerts.value_or(core::exchange::Alerts());
        alerts.format = alertsAs.value_or(alerts.format);
        alerts.address = alertsTo.value_or(alerts.address);
        wall.alerts = alerts;
        work.write(wall, passphrase, &work, [&](const base::Result<core::exchange::Written>& written) {
            sayWritten(toWrite.value_or(QString()), watch.elapsed(), written);
        });
    }
    return QCoreApplication::exec();
}

} // namespace

int main(int argc, char** argv)
{
    // Nothing leaves main flying (DESIGN.md section 7). What is said of it is what the library
    // that threw says, which has seen no passphrase.
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "qkvm-exchange-rig ended: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fputs("qkvm-exchange-rig ended on something that was thrown.\n", stderr);
        return 1;
    }
}
