#include <qkvm/core/own_actions.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include <QByteArray>
#include <QByteArrayView>
#include <QCoreApplication>
#include <QRandomGenerator>
#include <QStringDecoder>
#include <QStringView>

#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// A sentence of this program's own, where the server has none.
QString words(const char* text)
{
    return QCoreApplication::translate("OwnActions", text);
}

// The first line of every entry a text is kept in: what it is, and which form of it.
constexpr QByteArrayView secretMark = "QKVM-ACTION-1";
// The two places a writing of a text is kept in.
constexpr QChar placeA = u'a';
constexpr QChar placeB = u'b';
// A writing's own mark is this long: a number of sixty-four bits, in hex.
constexpr int markSigns = 16;
// An id of the program's making is this long: eight of 0-9 and a-f.
constexpr int idSigns = 8;

// The longest text an action types fits the parts a writing may have; and a part, with what
// stands before it in its entry (the three lines: no more than sixty bytes), fits one entry of
// Windows' store.
constexpr qsizetype mostBytesOfASign = 4;
constexpr qsizetype mostBeforeAPart = 60;
static_assert(longestTypedText * mostBytesOfASign <= OwnActions::partBytes * OwnActions::mostParts);
static_assert(OwnActions::partBytes + mostBeforeAPart <= largestSecretBytes);
static_assert(OwnActions::mostParts <= 9, "a part's number is one digit in its entry");

bool isSmallHex(QStringView text, qsizetype signs)
{
    return text.size() == signs && std::ranges::all_of(text, [](QChar sign) {
               return (sign >= u'0' && sign <= u'9') || (sign >= u'a' && sign <= u'f');
           });
}

// Which writing of its text an action has, as the list names it (OwnAction::writing): the place
// it is kept in, and its mark. Nothing: the list names none, or what stands there is not written
// as the program writes one.
struct Writing {
    QChar place;
    QByteArray mark;
};
std::optional<Writing> writingOf(const QString& named)
{
    if (named.size() != markSigns + 2 || (named.at(0) != placeA && named.at(0) != placeB) || named.at(1) != u':' ||
        !isSmallHex(QStringView(named).sliced(2), markSigns)) {
        return std::nullopt;
    }
    return Writing{named.at(0), named.sliced(2).toLatin1()};
}

// What stands before a part's bytes in its entry: the three lines (own_actions.h).
QByteArray beforeAPart(const QString& id, const QByteArray& mark, int part, int parts)
{
    QByteArray lines = secretMark.toByteArray();
    lines += '\n';
    lines += id.toLatin1();
    lines += '\n';
    lines += mark;
    lines += ' ';
    lines += QByteArray::number(part);
    lines += '/';
    lines += QByteArray::number(parts);
    lines += '\n';
    return lines;
}

// What an entry says of itself before its bytes, read line by line and not by what the writer
// above would have written: which form it is of, whose it is, of which writing, and which part
// of how many. Nothing: it has no three lines, or its third is not written as one is (the
// mark, a blank, one digit, a stroke, one digit).
struct Said {
    QByteArrayView form;
    QByteArrayView id;
    QByteArrayView mark;
    int part = 0;
    int parts = 0;
    QByteArrayView bytes;
};
std::optional<Said> saidBy(QByteArrayView entry)
{
    const qsizetype first = entry.indexOf('\n');
    const qsizetype second = first < 0 ? -1 : entry.indexOf('\n', first + 1);
    const qsizetype third = second < 0 ? -1 : entry.indexOf('\n', second + 1);
    if (third < 0) {
        return std::nullopt;
    }
    const QByteArrayView line = entry.sliced(second + 1, third - second - 1);
    const auto isDigit = [&line](qsizetype at) { return line.at(at) >= '0' && line.at(at) <= '9'; };
    if (line.size() != markSigns + 4 || line.at(markSigns) != ' ' || !isDigit(markSigns + 1) ||
        line.at(markSigns + 2) != '/' || !isDigit(markSigns + 3)) {
        return std::nullopt;
    }
    Said said;
    said.form = entry.first(first);
    said.id = entry.sliced(first + 1, second - first - 1);
    said.mark = line.first(markSigns);
    said.part = line.at(markSigns + 1) - '0';
    said.parts = line.at(markSigns + 3) - '0';
    said.bytes = entry.sliced(third + 1);
    return said;
}

void writeOver(QByteArray& bytes)
{
    // (Its own bytes: where they are shared with somebody, a copy is made first and that is
    // what is written over, which costs nothing but the copy.)
    if (!bytes.isEmpty()) {
        wipe(bytes.data(), static_cast<std::size_t>(bytes.size()));
    }
    bytes.clear();
}

void writeOver(QString& text)
{
    if (!text.isEmpty()) {
        wipe(text.data(), static_cast<std::size_t>(text.size()) * sizeof(QChar));
    }
    text.clear();
}

// How many signs a text has, as the server counts: one that takes two places here is one.
qsizetype signsOf(const QString& text)
{
    return std::ranges::count_if(text, [](QChar unit) { return !unit.isLowSurrogate(); });
}

// The text these bytes are, sign for sign: one that begins with U+FEFF begins with it here too
// (Qt's usual reading of bytes would drop it). Nothing where they are no text, no text at all, or
// more than an action types: that is nothing this program saved. (Counted here, and not by the
// rule a text is saved by, which would make another copy of it on its way.)
std::optional<QString> textFrom(const SecretBytes& bytes)
{
    QStringDecoder signs(QStringConverter::Utf8,
                         QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
    QString text = signs.decode(bytes.view());
    // Bytes that are no text are no text this program wrote;
    bool isAText = !signs.hasError();
    // and neither is nothing, or more than an action types.
    isAText = isAText && !text.isEmpty() && signsOf(text) <= longestTypedText;
    if (!isAText) {
        writeOver(text);
        return std::nullopt;
    }
    return std::optional<QString>(std::move(text));
}

// The server's words for these: a box and a notice show them as they are.
base::Error unknownAction()
{
    return {"not_found", "Unknown action"};
}

base::Error noLongerThere()
{
    return {"not_found", "That saved action no longer exists"};
}

base::Error chooseAKey()
{
    return {"invalid", "Choose at least one key"};
}

base::Error enterTheText()
{
    return {"invalid", "Enter the text to type"};
}

base::Error notSaved(const char* what, const base::Error& why)
{
    return {"not_saved", std::string(what) + " could not be saved (" + why.message + "), so nothing was changed."};
}

// (The server says these two in the words of the library that reads its forms. The rule is the
// same, and here it is said as the rule, as a name that is too long is: kvm_adding.cpp.)
base::Error tooLong(const char* field, qsizetype longest)
{
    return {"invalid", std::string(field) + ": this may have " + std::to_string(longest) + " characters at the most"};
}

// And this program's own, where the server has no words.
base::Error halfASign()
{
    return {"invalid", "text: " + words("this has half a character in it, which no keyboard types").toStdString()};
}

base::Error tooMany()
{
    return {"too_many", words("No more than %1 saved actions are kept. Remove one before adding another.")
                            .arg(mostSavedActions)
                            .toStdString()};
}

// What an action is called in a sentence: its name, set off by the one function for that
// (exchange::nameSetOff: in double quotes, and so that it cannot end its own setting-off). A
// name is free text, and `x" was removed. Run "y` is not to read as the program's own words.
QString calledInASentence(const OwnAction& action)
{
    const QString shown = exchange::nameSetOff(action.name);
    return shown.isEmpty() ? words("This saved action") : shown;
}

// The server's sentence for an action that was edited since it was looked at: its own for a run,
// and the same with another last word for a change and a removal, which the server takes unasked.
base::Error wasChanged(const OwnAction& action, const char* before)
{
    return {"changed",
            (calledInASentence(action) + u" was changed since you opened this list. Look at it again before "_s +
             QLatin1StringView(before) + u" it."_s)
                .toStdString()};
}

// The server's code for a text that is not to be had ("no_text"), and a sentence of this
// program's own: the server's speaks of a text that "does not open", which is its vault's way.
base::Error noText(const OwnAction& action)
{
    return {"no_text", words("Windows does not hand out the text that was saved for %1, or none is kept on this PC. "
                             "Nothing was typed. Edit the action and type its text again.")
                           .arg(calledInASentence(action))
                           .toStdString()};
}

// What is kept of what was typed, where every rule holds that the server's reader of a form has
// for it: the name, and the bytes of the text where one was typed.
struct Read {
    QString name;
    std::optional<SecretBytes> text;
};
base::Result<Read> readFrom(const OwnActions::Typed& typed)
{
    // In the order the server says them in: the name; then what it reads at all as keys and as
    // a text, whichever kind the action is of.
    base::Result<QString> name = action::nameFrom(typed.name);
    if (!name) {
        return name.error();
    }
    if (signsOf(typed.keys) > action::longestKeysAsTyped) {
        return tooLong("keys", action::longestKeysAsTyped);
    }
    Read read;
    read.name = std::move(name).value();
    if (!typed.text.isEmpty()) {
        if (!action::isTypedAsOne(typed.text)) {
            return tooLong("text", longestTypedText);
        }
        // Its bytes, in memory that is written over. Half a sign has none: what would be typed
        // in its place is not what was typed into the box.
        read.text = SecretBytes::utf8Of(typed.text);
        if (!read.text) {
            return halfASign();
        }
    }
    return base::Result<Read>(std::move(read));
}

// The server's check of an action by its kind (check_action): keys that are keys for one that
// presses, and that do not name the keyboard's Power key (in its words, after the keys' form, as
// the server asks them); and a text for one that types, unless it has one that stays (`existing`
// types, and the list names a writing of its text).
std::optional<base::Error> refusedByItsKind(const OwnActions::Typed& typed, const OwnAction* existing)
{
    if (!typed.types) {
        if (const base::Result<kvmd::Call> call = kvmd::callFor(Action::press(typed.keys), 0); !call) {
            return call.error().message == kvmd::powerKeyRefused ? call.error() : chooseAKey();
        }
    }
    const bool keepsItsText = existing != nullptr && existing->types && writingOf(existing->writing).has_value();
    if (typed.types && typed.text.isEmpty() && !keepsItsText) {
        return enterTheText();
    }
    return std::nullopt;
}

// An action as what was typed makes it (the server's fill_action), but for its text: the keys
// only of one that presses, Enter only of one that types.
void fill(OwnAction& action, const QString& name, const OwnActions::Typed& typed)
{
    action.name = name;
    action.types = typed.types;
    action.keys = typed.types ? QString() : typed.keys;
    action.enter = typed.enter && typed.types;
    action.danger = typed.danger;
}

QString newId(const QList<OwnAction>& beside)
{
    // Eight of 0-9 and a-f, and not one that is taken: as a KVM's is made.
    for (;;) {
        const QString id = u"%1"_s.arg(QRandomGenerator::system()->generate(), idSigns, 16, QLatin1Char('0'));
        const bool taken = std::ranges::any_of(beside, [&id](const OwnAction& one) { return one.id == id; });
        if (!taken) {
            return id;
        }
    }
}

qsizetype placeOnTheList(const QList<OwnAction>& actions, const QString& id)
{
    const auto found = std::ranges::find_if(actions, [&id](const OwnAction& one) { return one.id == id; });
    return found == actions.end() ? -1 : found - actions.begin();
}

} // namespace

namespace action {

base::Result<QString> nameFrom(const QString& typed)
{
    return kvm::neededLineFrom(typed, "name"_L1, exchange::longestActionName);
}

bool areKeys(const QString& keys)
{
    return kvmd::callFor(Action::press(keys), 0).ok();
}

bool isTypedAsOne(const QString& text)
{
    base::Result<kvmd::Call> call = kvmd::callFor(Action::type(text, false), 0);
    if (!call) {
        return false;
    }
    // (What the rule made of the text to send it: written over, since a text is a secret.)
    writeOver(call.value().body);
    return true;
}

bool namesAWriting(const QString& writing)
{
    return writingOf(writing).has_value();
}

bool isOne(const OwnAction& action)
{
    const base::Result<QString> name = nameFrom(action.name);
    if (!isSmallHex(action.id, idSigns) || !name || name.value() != action.name || action.changed < 0 ||
        action.changed > mostEdits) {
        return false;
    }
    if (action.types) {
        return action.keys.isEmpty() && (action.writing.isEmpty() || namesAWriting(action.writing));
    }
    return areKeys(action.keys) && !action.enter && action.writing.isEmpty();
}

SavedAction asHandedOut(const OwnAction& action)
{
    SavedAction shown;
    shown.id = action.id;
    shown.name = action.name;
    shown.types = action.types;
    shown.keys = action.keys;
    shown.enter = action.enter;
    shown.danger = action.danger;
    shown.changed = action.changed;
    return shown;
}

} // namespace action

OwnActions::ForOneRun::ForOneRun(Action action) : m_action(std::move(action)) {}

OwnActions::ForOneRun::ForOneRun(ForOneRun&& other) noexcept : m_action(std::move(other.m_action)) {}

OwnActions::ForOneRun::~ForOneRun()
{
    writeOver(m_action.text);
}

OwnActions::OwnActions(Settings& settings, SecretStore& secrets, QObject* parent)
    : QObject(parent), m_settings(settings), m_secrets(secrets)
{}

const QList<OwnAction>& OwnActions::all() const
{
    return m_settings.remembered().actions;
}

QList<SavedAction> OwnActions::handedOut() const
{
    QList<SavedAction> shown;
    for (const OwnAction& action : all()) {
        shown.append(action::asHandedOut(action));
    }
    return shown;
}

QString OwnActions::secretOf(const QString& id, QChar place, int part)
{
    return u"action:"_s + id + u':' + place + QString::number(part);
}

void OwnActions::forgetPlace(const QString& id, QChar place)
{
    for (int part = 1; part <= mostParts; ++part) {
        m_secrets.forget(secretOf(id, place, part));
    }
}

base::Result<QString> OwnActions::keepText(const QString& id, QChar place, const SecretBytes& text)
{
    const QByteArray mark =
        QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(markSigns, '0');
    const QByteArrayView bytes = text.view();
    const int parts = static_cast<int>((bytes.size() + partBytes - 1) / partBytes);
    for (int part = 1; part <= parts; ++part) {
        const QByteArrayView ofThisPart =
            bytes.sliced((part - 1) * partBytes).first(std::min(partBytes, bytes.size() - (part - 1) * partBytes));
        const QByteArray lines = beforeAPart(id, mark, part, parts);
        // (Room for all of it before any of it is there: what holds the text is not moved while
        // it grows, and is written over here.)
        QByteArray entry;
        entry.reserve(lines.size() + ofThisPart.size());
        entry.append(lines);
        entry.append(ofThisPart);
        const base::Result<void> kept = m_secrets.keep(secretOf(id, place, part), entry);
        writeOver(entry);
        if (!kept) {
            // Whole or not at all: where one entry cannot be written, none of this writing stays.
            forgetPlace(id, place);
            return kept.error();
        }
    }
    // What an earlier writing left in this place behind these parts is nobody's.
    for (int part = parts + 1; part <= mostParts; ++part) {
        m_secrets.forget(secretOf(id, place, part));
    }
    return place + u":"_s + QString::fromLatin1(mark);
}

std::optional<SecretBytes> OwnActions::textOf(const OwnAction& action)
{
    const std::optional<Writing> named = writingOf(action.writing);
    if (!action.types || !named) {
        return std::nullopt;
    }
    const QByteArray id = action.id.toLatin1();
    SecretBytes text(partBytes * mostParts);
    int parts = 1;
    for (int part = 1; part <= parts; ++part) {
        QByteArray entry = m_secrets.kept(secretOf(action.id, named->place, part));
        const std::optional<Said> said = saidBy(entry);
        if (!said) {
            writeOver(entry);
            return std::nullopt;
        }
        // Each entry says that it is of this form,
        bool taken = said->form == secretMark;
        // that it is this action's,
        taken = taken && said->id == id;
        // of the writing the list names,
        taken = taken && said->mark == named->mark;
        // and this part of it, of as many as a text has and as the first part said.
        taken = taken && said->part == part && said->parts >= 1 && said->parts <= mostParts &&
                (part == 1 || said->parts == parts);
        // Every part but the last is full, and the last holds something.
        taken = taken && !said->bytes.isEmpty() && said->bytes.size() <= partBytes &&
                (part == said->parts || said->bytes.size() == partBytes);
        taken = taken && text.append(said->bytes);
        if (taken) {
            parts = said->parts;
        }
        writeOver(entry);
        if (!taken) {
            return std::nullopt;
        }
    }
    return std::optional<SecretBytes>(std::move(text));
}

base::Result<void> OwnActions::remember(const QList<OwnAction>& actions, const char* what)
{
    const Remembered before = m_settings.remembered();
    Remembered after = before;
    after.actions = actions;
    if (const base::Result<void> written = m_settings.remember(after); !written) {
        // What is remembered changes even when the writing fails: so it is changed back, and
        // the list is what the file still says.
        static_cast<void>(m_settings.remember(before));
        return notSaved(what, written.error());
    }
    return {};
}

QString OwnActions::nameForImport(const QString& name)
{
    QString seen;
    for (const char32_t sign : name.normalized(QString::NormalizationForm_KC).toUcs4()) {
        const bool hidden = QChar::category(sign) == QChar::Other_Format || sign == 0x034F || sign == 0x115F ||
                            sign == 0x1160 || sign == 0x17B4 || sign == 0x17B5 || sign == 0x2800 || sign == 0x3164 ||
                            sign == 0xFFA0 || (sign >= 0x180B && sign <= 0x180F) ||
                            (sign >= 0xFE00 && sign <= 0xFE0F) || (sign >= 0xE0100 && sign <= 0xE01EF);
        if (!hidden) {
            // Qt's case fold is simple (one character). The web comparison uses full
            // folding: preserve expansions such as sharp S and Greek iota subscripts.
            const QString folded = QString::fromUcs4(&sign, 1).toCaseFolded();
            const QString upper = folded.toUpper();
            if (sign == 0x0130) {
                seen.append(u'i');
                seen.append(QChar(0x0307));
            } else {
                seen.append(upper.size() > folded.size() ? upper.toCaseFolded() : folded);
            }
        }
    }
    return seen.simplified();
}

bool OwnActions::hasName(const QString& name) const
{
    const QString called = nameForImport(name);
    return std::ranges::any_of(all(), [&called](const OwnAction& one) { return nameForImport(one.name) == called; });
}

base::Result<SavedAction> OwnActions::takeFromAFile(const exchange::Action& action)
{
    if (hasName(action.name)) {
        return base::Error{"duplicate", "A saved action of this name is already here. Nothing was replaced."};
    }
    return add(Typed{action.name, action.types, action.keys, action.text, action.enter, action.danger});
}

base::Result<SavedAction> OwnActions::add(const Typed& typed)
{
    if (all().size() >= mostSavedActions) {
        return tooMany();
    }
    base::Result<Read> read = readFrom(typed);
    if (!read) {
        return read.error();
    }
    if (const std::optional<base::Error> refusal = refusedByItsKind(typed, nullptr)) {
        return *refusal;
    }
    OwnAction fresh;
    fresh.id = newId(all());
    fill(fresh, read.value().name, typed);
    // The text first, as a KVM's password is kept before the KVM is on its list: an action that
    // types and has no text types nothing. (One that types has a text here: refusedByItsKind.)
    if (const std::optional<SecretBytes>& text = read.value().text; fresh.types && text) {
        const base::Result<QString> writing = keepText(fresh.id, placeA, *text);
        if (!writing) {
            return notSaved("The new action", writing.error());
        }
        fresh.writing = writing.value();
    }
    QList<OwnAction> with = all();
    with.append(fresh);
    if (const base::Result<void> written = remember(with, "The new action"); !written) {
        if (fresh.types) {
            forgetPlace(fresh.id, placeA);
        }
        return written.error();
    }
    emit changed();
    return action::asHandedOut(fresh);
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): its own copy of the id, as LocalWall takes one
base::Result<SavedAction> OwnActions::change(QString id, int lookedAt, const Typed& typed)
{
    // In the server's order: what its reader of a form refuses; then whether the action is
    // there; then what it checks of an action by its kind. And between those, this program's
    // own: whether the action is still what was looked at.
    base::Result<Read> read = readFrom(typed);
    if (!read) {
        return read.error();
    }
    const qsizetype at = placeOnTheList(all(), id);
    if (at < 0) {
        return unknownAction();
    }
    const OwnAction before = all().at(at);
    if (before.changed != lookedAt) {
        return wasChanged(before, "changing");
    }
    // A change counts one up, or two where it cannot be written down: past the most that is read
    // back, the action would be gone at the next start, without a word. Only a settings' file
    // made by hand comes so far.
    if (before.changed > action::mostEdits - 2) {
        return base::Error{
            "invalid",
            words("This action was changed as often as an action can be. Remove it, and save it anew.").toStdString()};
    }
    if (const std::optional<base::Error> refusal = refusedByItsKind(typed, &before)) {
        return *refusal;
    }
    OwnAction after = before;
    fill(after, read.value().name, typed);
    after.changed = before.changed + 1;
    // Its text from now on: none of one that presses; the one that is saved, where none was
    // typed; or the one that was typed, as a new writing in the place the one before is not in,
    // so that the one before is as it was until the list names the new one.
    const std::optional<Writing> had = writingOf(before.writing);
    std::optional<QChar> wroteInto;
    if (!after.types) {
        after.writing.clear();
    } else if (const std::optional<SecretBytes>& text = read.value().text) {
        const QChar place = had && had->place == placeA ? placeB : placeA;
        const base::Result<QString> writing = keepText(id, place, *text);
        if (!writing) {
            return notSaved("The change to this action", writing.error());
        }
        after.writing = writing.value();
        wroteInto = place;
    }
    QList<OwnAction> with = all();
    with[at] = after;
    if (const base::Result<void> written = remember(with, "The change to this action"); !written) {
        // Taken back whole: the new text is taken out again, and the action is what it was.
        if (wroteInto) {
            forgetPlace(id, *wroteInto);
        }
        // And it stands under a number nobody was shown, as the server does it: not the one it
        // had, and not the next, which is what the change that was taken back would have been
        // shown under and what the next change would be. (Noted, and written with whatever is
        // written next: nothing can be written just now.)
        Remembered asItWas = m_settings.remembered();
        asItWas.actions[at].changed = before.changed + 2;
        m_settings.note(asItWas);
        emit changed();
        return written.error();
    }
    // What it no longer has is forgotten, now that the list names what it has.
    if (had && after.writing != before.writing) {
        forgetPlace(id, had->place);
    }
    emit changed();
    return action::asHandedOut(after);
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): its own copy of the id, as LocalWall takes one
base::Result<void> OwnActions::remove(QString id, int lookedAt)
{
    const qsizetype at = placeOnTheList(all(), id);
    if (at < 0) {
        return unknownAction();
    }
    if (const OwnAction& gone = all().at(at); gone.changed != lookedAt) {
        return wasChanged(gone, "removing");
    }
    QList<OwnAction> without = all();
    without.removeAt(at);
    if (const base::Result<void> written = remember(without, "Removing this action"); !written) {
        return written.error();
    }
    // Its text goes with it: whatever is kept under its id, in either place.
    forgetPlace(id, placeA);
    forgetPlace(id, placeB);
    emit changed();
    return {};
}

base::Result<OwnAction> OwnActions::asLookedAt(const QString& id, int lookedAt) const
{
    const qsizetype at = placeOnTheList(all(), id);
    if (at < 0) {
        return noLongerThere();
    }
    const OwnAction& now = all().at(at);
    if (now.changed != lookedAt) {
        // What was looked at is not what would run: it may type where it pressed a key, and
        // want to be asked about where it did not.
        return wasChanged(now, "running");
    }
    return now;
}

base::Result<OwnActions::ForOneRun> OwnActions::forOneRun(const OwnAction& action)
{
    if (!action.types) {
        return ForOneRun(Action::press(action.keys));
    }
    // Whatever is kept for it, nothing included, is looked at as a text: an action that types
    // has one, and one whose text is not there would type nothing and press Enter, which sends
    // off whatever stands at that machine's prompt.
    const std::optional<SecretBytes> bytes = textOf(action);
    std::optional<QString> text = bytes ? textFrom(*bytes) : std::nullopt;
    if (!text) {
        return noText(action);
    }
    Action typed = Action::type(QString(), action.enter);
    typed.text = std::move(*text);
    return ForOneRun(std::move(typed));
}

OwnActions::WithTheirTexts OwnActions::textsHandedOutForASealedFile()
{
    WithTheirTexts handed;
    const QList<OwnAction> actions = all();
    for (qsizetype index = 0; index < actions.size(); ++index) {
        const OwnAction& action = actions.at(index);
        exchange::Action one;
        one.name = action.name;
        one.types = action.types;
        one.keys = action.keys;
        one.enter = action.enter;
        one.danger = action.danger;
        if (action.types) {
            const std::optional<SecretBytes> bytes = textOf(action);
            std::optional<QString> text = bytes ? textFrom(*bytes) : std::nullopt;
            if (text) {
                one.text = std::move(*text);
            } else {
                handed.withoutText.append(index);
            }
        }
        handed.actions.append(std::move(one));
    }
    return handed;
}

} // namespace qkvm::core
