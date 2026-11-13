#include "files_view.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <QDate>
#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QUrl>

#include <qkvm/core/server_client.h>

#include "machine_menu.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// As for an account's password on the server: a shorter passphrase is refused there too.
constexpr qsizetype shortestPassphrase = 10;

QString labelOf(const QString& part)
{
    if (part == "devices"_L1) {
        return u"KVMs and servers"_s;
    }
    if (part == "actions"_L1) {
        return u"Saved actions"_s;
    }
    return part == "alerts"_L1 ? u"Alert settings"_s : u"Wall settings"_s;
}

// Letters that are blanks, and the marks that pick one drawing of the letter before them or join
// it to the next: not drawn, and not of the kind that says so of itself. The server's own list of
// them (app/qkvm/server.py, NOT_DRAWN).
bool isNotDrawn(char32_t one)
{
    static constexpr std::array<char32_t, 8> blanks = {0x034F, 0x115F, 0x1160, 0x17B4, 0x17B5, 0x2800, 0x3164, 0xFFA0};
    return std::ranges::find(blanks, one) != blanks.end() || (one >= 0x180B && one <= 0x180F) ||
           (one >= 0xFE00 && one <= 0xFE0F) || (one >= 0xE0100 && one <= 0xE01EF);
}

// The dot this program puts between the things it says of a device or an action, and the signs
// that look like it: a name with one in it would read as a name and things said of it.
bool isLikeTheDotBetween(char32_t one)
{
    static constexpr std::array<char32_t, 10> dots = {0x00B7, 0x0387, 0x16EB, 0x2022, 0x2027,
                                                      0x2219, 0x22C5, 0x2E31, 0x30FB, 0xFF65};
    return std::ranges::find(dots, one) != dots.end();
}

// Whether a browser reads the last part of a name as a number, and so the whole of it as a
// numbered address: all digits, or sixteens (0x and what follows it). (Of a name as Qt hands it
// out, which is in small letters.)
bool isReadAsANumber(const QString& part)
{
    static const QRegularExpression number(u"\\A(?:[0-9]+|0x[0-9a-f]*)\\z"_s);
    return number.match(part).hasMatch();
}

// How a text of a file's is shown: on one line; as the lines a button will type; or in the
// letters of this alphabet only, where a letter of another would read as one it is not.
enum class Shown { OnOneLine, AsTypedLines, InPlainLettersOnly };

// What a file says, as it is shown: with nothing in it that goes unseen. A sign that draws
// nothing, or turns the line around, or begins another line where there is one line, or is a
// blank of another kind than the blank, is written out as its number. (A file may come from
// somebody else, and what is read here is what is agreed to. A name in letters that join or
// change shape by such a sign is shown with the sign written out: here, and nowhere else.)
QString seen(const QString& said, Shown how = Shown::OnOneLine)
{
    QString shown;
    const QList<uint> characters = said.toUcs4();
    for (const uint character : characters) {
        const char32_t one = character;
        if (how == Shown::AsTypedLines && one == U'\n') {
            // A line's end is Enter: said where it stands, also where nothing comes after it.
            shown += u"↵\n"_s;
            continue;
        }
        const QChar::Category kind = QChar::category(one);
        const bool isABlankOfAnotherKind = kind == QChar::Separator_Space && one != U' ';
        const bool endsALine = kind == QChar::Separator_Line || kind == QChar::Separator_Paragraph;
        const bool isNotPlain = how == Shown::InPlainLettersOnly && one > 0x7E;
        const bool isTheDotBetween = how != Shown::AsTypedLines && isLikeTheDotBetween(one);
        if (QChar::isPrint(one) && !isABlankOfAnotherKind && !endsALine && !isNotDrawn(one) && !isNotPlain &&
            !isTheDotBetween) {
            shown += QString::fromUcs4(&one, 1);
        } else {
            shown += u"<U+%1>"_s.arg(QString::number(character, 16).toUpper().rightJustified(4, u'0'));
        }
    }
    return shown;
}

// The name and the port a console link leads to, as a browser's address line would say them: a
// name in other letters in the form that cannot be taken for another, a numbered address of the
// long kind in its brackets. Nothing where the address is no plain one: where it has something in
// it that programs read differently (a browser takes a backslash for a stroke and drops a tab,
// and what is said here would be another place than the browser goes to), a name and password, or
// no place that can be read at all.
std::optional<QString> placeOf(const QString& address)
{
    const bool hasWhatIsReadDifferently = std::ranges::any_of(
        address, [](QChar one) { return one.unicode() <= 0x20 || one.unicode() == 0x7F || one == u'\\'; });
    const QUrl url(address);
    if (hasWhatIsReadDifferently || !url.isValid() || !url.userInfo().isEmpty() ||
        (url.scheme() != "https"_L1 && url.scheme() != "http"_L1)) {
        return std::nullopt;
    }
    QString host = url.host(QUrl::FullyEncoded);
    const int port = url.port();
    // (No port can be 0, and a numbered address with a part that names a network card is not
    // one a browser goes to.)
    if (host.isEmpty() || port == 0 || host.contains(u'%')) {
        return std::nullopt;
    }
    // A place written as numbers is plain in one form only: four numbers with dots between. In
    // every other (one number, sixteens, eights, a part left out) programs do not agree on
    // where it is; and where this one read such a form as four numbers, they are not what was
    // written. A name whose last part is a number is such a form to a browser.
    static const QRegularExpression fourNumbers(
        u"\\A(?:(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])\\.){3}(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])\\z"_s);
    const QStringList parts = host.split(u'.', Qt::SkipEmptyParts);
    const bool isLongNumbered = host.contains(u':');
    if (fourNumbers.match(host).hasMatch() ? !address.contains(host)
                                           : !parts.isEmpty() && isReadAsANumber(parts.constLast())) {
        return std::nullopt;
    }
    // (A long numbered address comes from Qt in one form, with nothing in it that reads as above.)
    if (isLongNumbered) {
        host = u'[' + host + u']';
    }
    const bool isTheUsual = (url.scheme() == "https"_L1 && port == 443) || (url.scheme() == "http"_L1 && port == 80);
    // (In that form the name is of plain letters only: there is nothing in it to write out.)
    return port > 0 && !isTheUsual ? host + u':' + QString::number(port) : host;
}

// A place as the server names it, a name or a number with or without a port: where it is not
// written in plain letters, in the form that cannot be taken for another, as a console link's is.
// (This is where a password goes that is typed for the device afterwards.) What is more than a
// place, or cannot be read as one, is said as that, whole: with a name before it, or something
// that begins another part of an address, a part of it would read as the place and be none.
QString placeSeen(const QString& place)
{
    const auto notPlain = [&] { return u"a place that is no plain one: "_s + seen(place, Shown::InPlainLettersOnly); };
    static const QRegularExpression moreThanAPlace(u"[/?#@\\\\]"_s);
    if (place.contains(moreThanAPlace)) {
        return notPlain();
    }
    if (std::ranges::all_of(place, [](QChar one) { return one.unicode() > 0x20 && one.unicode() < 0x7F; })) {
        return place;
    }
    const QUrl url(u"//"_s + place);
    const QString host = url.isValid() ? url.host(QUrl::FullyEncoded) : QString();
    // (Digits of another alphabet come out as these ones: whether what follows the address reads
    // them the same is not known here.)
    const QStringList parts = host.split(u'.', Qt::SkipEmptyParts);
    if (parts.isEmpty() || isReadAsANumber(parts.constLast())) {
        return notPlain();
    }
    return url.port() > 0 ? host + u':' + QString::number(url.port()) : host;
}

// What the server says where it took nothing in: its own refusals, which it gives before it
// changes anything. Whatever else goes wrong with a taking (no answer, one that was cut short, one
// from something that stands before the server) leaves open whether the file was taken in.
bool saysThatNothingWasTaken(const std::string& code)
{
    static constexpr std::array<std::string_view, 11> refusals = {
        "passphrase_needed", "wrong_passphrase", "not_a_file", "newer",  "damaged", "busy", "locked",
        "not_allowed",       "forbidden",        "not_saved",  "invalid"};
    // (And the program's own word for a file that was never sent to the server at all.)
    return code == FilesView::nothingWasSent || std::ranges::find(refusals, std::string_view(code)) != refusals.end();
}

QVariantMap line(const QString& text, const QString& typed = {})
{
    return {{u"text"_s, text}, {u"typed"_s, typed}};
}

QVariantMap part(const QString& key, const QString& label, const QString& hint, bool can, bool ticked,
                 const QVariantList& lines = {})
{
    return {{u"key"_s, key}, {u"label"_s, label},   {u"hint"_s, hint},
            {u"can"_s, can}, {u"ticked"_s, ticked}, {u"lines"_s, lines}};
}

QString ofDevice(const core::FileDevice& device)
{
    QStringList said{seen(device.name), placeSeen(device.host)};
    if (device.tls == "insecure"_L1) {
        said.append(u"its certificate is not checked"_s);
    } else if (device.tls == "system"_L1) {
        said.append(u"its certificate is checked against the public list"_s);
    }
    if (!device.hasPassword) {
        said.append(u"no password"_s);
    }
    if (!device.openUrl.isEmpty()) {
        const std::optional<QString> place = placeOf(device.openUrl);
        said.append(place ? u"console link to "_s + *place
                          : u"a console link that is no plain address: "_s +
                                seen(device.openUrl, Shown::InPlainLettersOnly));
    }
    if (!device.group.isEmpty()) {
        said.append(u"group "_s + seen(device.group));
    }
    return said.join(u" · "_s);
}

QString ofAction(const core::FileAction& action)
{
    QString said =
        seen(action.name) + (action.types ? u" · types a text"_s : u" · presses "_s + seen(keysLabel(action.keys)));
    if (action.enter) {
        said += u", then Enter"_s;
    }
    if (action.danger) {
        said += u" · asks before it runs"_s;
    }
    return said;
}

QVariantMap devicesPart(const QList<core::FileDevice>& devices)
{
    QVariantList fresh;
    QStringList twins;
    for (const core::FileDevice& device : devices) {
        if (device.isNew) {
            fresh.append(line(ofDevice(device)));
        } else if (!device.sameAs.isEmpty()) {
            twins.append(seen(device.name) + u": "_s + seen(device.sameAs));
        }
    }
    const qsizetype here = devices.size() - fresh.size() - twins.size();
    QStringList hint;
    if (here > 0) {
        hint.append(u"%1 are on this wall already and stay as they are."_s.arg(here));
    }
    if (!twins.isEmpty()) {
        hint.append(
            u"%1 are left out as the same device as one that is here or earlier in the file ("_s.arg(twins.size()) +
            twins.mid(0, 3).join(u"; "_s) + (twins.size() > 3 ? u"; …"_s : QString()) + u")."_s);
    }
    return part(u"devices"_s, u"KVMs and servers: %1 to add"_s.arg(fresh.size()), hint.join(u' '), !fresh.isEmpty(),
                !fresh.isEmpty(), fresh);
}

QVariantMap actionsPart(const QList<core::FileAction>& actions)
{
    QVariantList fresh;
    for (const core::FileAction& action : actions) {
        if (action.isNew) {
            // (What it types is shown as it will be typed, line by line: a line's end is Enter.)
            fresh.append(line(ofAction(action), action.types ? seen(action.text, Shown::AsTypedLines) : QString()));
        }
    }
    const qsizetype here = actions.size() - fresh.size();
    return part(u"actions"_s, u"Saved actions: %1 to add"_s.arg(fresh.size()),
                here > 0 ? u"%1 are left out: an action of that name is here already."_s.arg(here) : QString(),
                !fresh.isEmpty(), !fresh.isEmpty(), fresh);
}

// What replaces something that is set here is asked for, and not assumed: it is not ticked.
QVariantMap alertsPart(const core::FileAlerts& alerts)
{
    QStringList about;
    for (const QString& kind : alerts.kinds) {
        about.append(kind == "offline"_L1     ? u"went offline"_s
                     : kind == "no_signal"_L1 ? u"lost its picture"_s
                     : kind == "problem"_L1   ? u"has a problem"_s
                     : kind == "recovered"_L1 ? u"is over"_s
                                              : seen(kind));
    }
    const QString where = alerts.hasAddress
                              ? u"They will go to "_s + placeSeen(alerts.host) + u", the address in the file."_s
                              : u"The address this wall sends them to is kept."_s;
    return part(u"alerts"_s, u"Alert settings"_s,
                u"Replaces what is set here: alerts "_s + (alerts.enabled ? u"on"_s : u"off"_s) + u", told about: "_s +
                    (about.isEmpty() ? u"nothing"_s : about.join(u", "_s)) + u". "_s + where,
                true, false);
}

QVariantMap wallPart(const core::FileWallSettings& wall)
{
    QStringList sets;
    if (wall.pictureEverySeconds) {
        sets.append(u"a picture every %1 s"_s.arg(*wall.pictureEverySeconds));
    }
    if (wall.asksForNewerVersions) {
        sets.append(*wall.asksForNewerVersions ? u"asks once a day whether there is a newer version"_s
                                               : u"does not ask for newer versions"_s);
    }
    return part(u"wall"_s, u"Wall settings"_s, u"Replaces what is set here: %1."_s.arg(sets.join(u", "_s)), true,
                false);
}

} // namespace

FilesView::FilesView(QObject* parent) : QObject(parent) {}

QVariantList FilesView::partsOfAFile()
{
    QVariantList all;
    for (const QString& key : core::wallFileParts()) {
        all.append(QVariantMap{{u"key"_s, key}, {u"label"_s, labelOf(key)}});
    }
    return all;
}

quint64 FilesView::waitFor(Waits what)
{
    m_waits = what;
    return ++m_turn;
}

bool FilesView::isAnswer(quint64 turn, Waits to)
{
    if (turn != m_turn || m_waits != to) {
        return false;
    }
    m_waits = Waits::ForNothing;
    return true;
}

void FilesView::reset()
{
    // A taking that is under way is not given up at the server by this: it may take the file in
    // all the same. Said, so that the file is not handed in a second time unlooked.
    const bool wasTaking = isTaking();
    // (Nothing is waited for: whatever is answered from now on is nobody's.)
    m_waits = Waits::ForNothing;
    m_savesTo.clear();
    m_problem.clear();
    forgetTheFile();
    emit changed();
    if (wasTaking) {
        emit cutOff(u"Taking the file in was cut off before the server said what came of it. The file may have been "
                    u"taken in all the same: look at the wall before trying again."_s);
    }
}

void FilesView::forgetTheFile()
{
    m_file.clear();
    m_fileName.clear();
    m_sealed = false;
    forgetWhatWasRead();
}

void FilesView::forgetWhatWasRead()
{
    m_wasRead = false;
    m_about.clear();
    m_plainSecrets = false;
    m_canTake = false;
    m_leftOut.clear();
    if (!m_parts.isEmpty()) {
        m_parts.clear();
        emit partsChanged();
    }
}

void FilesView::fail(const QString& problem)
{
    m_problem = problem;
    emit changed();
}

void FilesView::exportWall(const QStringList& parts, const QString& passphrase, const QString& password)
{
    // (Nor from behind one of Windows' boxes that still stands, whatever became of its question.)
    if (holdsItsWindow()) {
        return;
    }
    if (parts.isEmpty()) {
        fail(u"Choose what the file is to hold."_s);
        return;
    }
    if (!passphrase.isEmpty() && passphrase.size() < shortestPassphrase) {
        fail(u"The passphrase needs 10 characters or more. Leave it empty for a file without passwords."_s);
        return;
    }
    if (password.isEmpty()) {
        fail(u"Enter your password."_s);
        return;
    }
    // Where it is to be kept is asked first: a file that nobody keeps is not made. The program
    // goes on while Windows' box is open, and whoever comes back from it to something else than
    // they left (the wall was left, the box was closed) has asked for nothing: the password and
    // the passphrase go nowhere.
    const QString suggested = suggestedFileName(!passphrase.isEmpty());
    const quint64 turn = waitFor(Waits::ForAPlace);
    m_problem.clear();
    emit changed();
    // (For as long as Windows' box is there, the window under it stays, whatever becomes of the
    // question meanwhile.)
    m_atTheDoor = true;
    const base::Result<QString> place =
        m_door.whereToSave ? m_door.whereToSave(suggested) : base::Result<QString>(QString());
    m_atTheDoor = false;
    if (!isAnswer(turn, Waits::ForAPlace)) {
        return;
    }
    if (!place) {
        fail(QString::fromStdString(place.error().message));
        return;
    }
    if (place.value().isEmpty()) {
        emit changed();
        return;
    }
    m_savesTo = place.value();
    m_savesSealed = !passphrase.isEmpty();
    m_waits = Waits::ForTheFile;
    emit changed();
    emit exportAsked(turn, parts, passphrase, password);
}

void FilesView::exported(quint64 turn, const base::Result<core::WallFile>& file)
{
    if (!isAnswer(turn, Waits::ForTheFile)) {
        return;
    }
    const QString savesTo = std::exchange(m_savesTo, {});
    if (!file) {
        fail(QString::fromStdString(file.error().message));
        return;
    }
    // What came is what was asked for, or it is not kept: a file without a passphrase, kept under
    // the name of one with and called one, would have every password of the wall in it to be read.
    if (file.value().sealed != m_savesSealed) {
        fail(u"qKVM answered with another kind of file than was asked for. It was not kept."_s);
        return;
    }
    // Whole, or not at all: a file with half a wall in it is worse than none.
    if (const base::Result<void> kept = writeWholeFile(savesTo, file.value().text); !kept) {
        fail(QString::fromStdString(kept.error().message));
        return;
    }
    emit changed();
    // A saved password or text that does not open any more is not in the file: said, since the
    // file is kept for what is in it.
    if (const int without = file.value().notOpened; without > 0) {
        emit done(
            u"Exported, without %1 not open any more. qKVM's log says which. Give %2 again on the wall, then export once more."_s
                .arg(without == 1 ? u"one saved password or text that does"_s
                                  : u"%1 saved passwords or texts that do"_s.arg(without),
                     without == 1 ? u"it"_s : u"them"_s),
            false);
        return;
    }
    emit done(saidOfAFileThatWasKept(m_savesSealed), true);
}

void FilesView::chooseFile()
{
    if (holdsItsWindow()) {
        return;
    }
    const quint64 turn = waitFor(Waits::ForAChoice);
    emit changed();
    m_atTheDoor = true;
    const base::Result<QString> chosen = m_door.whichToOpen ? m_door.whichToOpen() : base::Result<QString>(QString());
    m_atTheDoor = false;
    if (!isAnswer(turn, Waits::ForAChoice)) {
        return;
    }
    if (!chosen) {
        fail(QString::fromStdString(chosen.error().message));
        return;
    }
    if (chosen.value().isEmpty()) {
        emit changed();
        return;
    }
    // Another file: nothing of the one before.
    forgetTheFile();
    m_problem.clear();
    // (The server has the last word on what is too large. This keeps a film or a disk image from
    // being read in, and something that only calls itself a file and has no end.)
    const base::Result<QByteArray> text = readChosenFile(chosen.value(), core::largestFileBytes);
    if (!text) {
        fail(QString::fromStdString(text.error().message));
        return;
    }
    m_file = text.value();
    m_fileName = QFileInfo(chosen.value()).fileName();
    // An encrypted file says so in the open: only then is there a passphrase to ask for.
    m_sealed = core::isSealedFile(m_file);
    emit changed();
}

void FilesView::readFile(const QString& passphrase)
{
    if (isBusy()) {
        return;
    }
    if (m_file.isEmpty()) {
        fail(u"Choose a file first."_s);
        return;
    }
    m_problem.clear();
    forgetWhatWasRead();
    const quint64 turn = waitFor(Waits::ForTheReading);
    emit changed();
    emit readAsked(turn, m_file, m_sealed ? passphrase : QString());
}

void FilesView::readAnew()
{
    if (isBusy() || !m_wasRead) {
        return;
    }
    m_problem.clear();
    forgetWhatWasRead();
    emit changed();
}

void FilesView::fileWasRead(quint64 turn, const base::Result<core::FileContents>& contents)
{
    if (!isAnswer(turn, Waits::ForTheReading)) {
        return;
    }
    if (!contents) {
        if (contents.error().code == "passphrase_needed") {
            m_sealed = true;
        }
        fail(QString::fromStdString(contents.error().message));
        return;
    }
    const core::FileContents& file = contents.value();
    // Everything that is about to be taken is said, each device and each action by itself: a file
    // may come from somebody else.
    QStringList about{file.encrypted ? u"An encrypted file"_s : u"A plain file"_s};
    if (!file.program.isEmpty()) {
        about.append(u"made by qKVM "_s + seen(file.program));
    }
    if (file.madeAt > 0) {
        about.append(u"on "_s + QLocale::system().toString(QDateTime::fromSecsSinceEpoch(file.madeAt).date(),
                                                           QLocale::ShortFormat));
    }
    m_about = about.join(u", "_s) + u'.';
    m_plainSecrets = !file.encrypted && file.hasSecrets;
    if (file.devices) {
        m_parts.append(devicesPart(*file.devices));
    }
    if (file.actions) {
        m_parts.append(actionsPart(*file.actions));
    }
    if (file.alerts) {
        m_parts.append(alertsPart(*file.alerts));
    }
    if (file.wall) {
        m_parts.append(wallPart(*file.wall));
    }
    if (!m_parts.isEmpty()) {
        emit partsChanged();
    }
    m_canTake = std::ranges::any_of(std::as_const(m_parts),
                                    [](const QVariant& one) { return one.toMap().value(u"can"_s).toBool(); });
    if (const qsizetype left = file.leftOut.size(); left > 0) {
        QStringList named;
        for (const QString& entry : file.leftOut.mid(0, 5)) {
            named.append(seen(entry));
        }
        m_leftOut = u"%1 %2 left out: "_s.arg(left).arg(left == 1 ? u"entry was"_s : u"entries were"_s) +
                    named.join(u"; "_s) + (left > 5 ? u"…"_s : QString());
    }
    m_wasRead = true;
    emit changed();
}

void FilesView::takeIn(const QStringList& parts, const QString& passphrase)
{
    if (isBusy() || !m_wasRead) {
        return;
    }
    if (parts.isEmpty()) {
        fail(u"Tick what is to be taken in."_s);
        return;
    }
    m_problem.clear();
    const quint64 turn = waitFor(Waits::ForTheTaking);
    emit changed();
    emit takeAsked(turn, m_file, m_sealed ? passphrase : QString(), parts);
}

void FilesView::fileWasTaken(quint64 turn, const base::Result<core::Taken>& taken)
{
    if (!isAnswer(turn, Waits::ForTheTaking)) {
        return;
    }
    if (!taken) {
        // Where it is not the server itself that says no, whether it took the file in is not
        // known: said, so that it is not handed in a second time unlooked.
        QString said = QString::fromStdString(taken.error().message);
        if (!saysThatNothingWasTaken(taken.error().code)) {
            said += (said.endsWith(u'.') ? u" "_s : u". "_s) +
                    u"The file may have been taken in all the same: look at the wall before trying again."_s;
        }
        fail(said);
        return;
    }
    QStringList said{
        u"%1 %2 added"_s.arg(taken.value().devices).arg(taken.value().devices == 1 ? u"device"_s : u"devices"_s)};
    if (taken.value().actions > 0) {
        said.append(u"%1 saved actions"_s.arg(taken.value().actions));
    }
    if (taken.value().alerts) {
        said.append(u"alert settings replaced"_s);
    }
    if (taken.value().wall) {
        said.append(u"wall settings replaced"_s);
    }
    // The file is nobody's any more: neither its text nor what it held.
    forgetTheFile();
    emit changed();
    emit done(said.join(u", "_s), true);
}

QString FilesView::shownTypedText(const QString& text)
{
    return seen(text, Shown::AsTypedLines);
}

} // namespace qkvm::ui
