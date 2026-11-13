#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/own_actions.h>
#include <qkvm/core/wall_feed.h>

#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::LocalWall;
using core::OwnAction;
using core::OwnActions;
using core::OwnKvm;
using core::SavedAction;
using core::testing::Answered;
using core::testing::good;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;
using core::testing::refusal;
using core::testing::sign;

namespace {

core::KvmdDevice::Pace quick()
{
    core::KvmdDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.freshMs = 200;
    pace.firstContactMs = 400;
    pace.patienceMs = 400;
    return pace;
}

// A secret store in memory that takes in one entry what Windows' own takes and no more, and
// counts how often something was read from it. Nothing of a test reaches the Credential Manager
// of whoever runs it (but for the one test at the end, which says so).
class AsWindowsKeeps final : public core::SecretStore {
public:
    base::Result<void> keep(const QString& name, const QByteArray& secret) override
    {
        if (secret.isEmpty() || secret.size() > core::largestSecretBytes) {
            ++inMemory.keeps;
            return base::Error{"secrets", "What was to be kept is not of a size Windows keeps."};
        }
        return inMemory.keep(name, secret);
    }
    QByteArray kept(const QString& name) override
    {
        ++reads;
        return inMemory.kept(name);
    }
    void forget(const QString& name) override { inMemory.forget(name); }

    MemorySecrets inMemory;
    int reads = 0;
};

// What saved actions are kept in, for a test: the settings in a folder that goes with the test,
// and the store in memory.
struct Kept {
    QTemporaryDir folder;
    QString file = folder.filePath(u"settings.json"_s);
    core::Settings settings{file};
    AsWindowsKeeps secrets;
    QHash<QString, QByteArray>& entries = secrets.inMemory.all;
};

// And this PC's own wall over them, with drivers that do not wait long.
struct Own : Kept {
    LocalWall wall{settings, secrets, quick()};
    OwnActions& actions = wall.actions();

    // Puts a made-up KVM on the wall, and says what the wall calls it.
    QString add(const MadeUpKvm& kvm, const QString& name = u"Build server"_s)
    {
        OwnKvm own;
        own.address = kvm.stand->address();
        own.name = name;
        return wall.add(own, u"secret"_s).value();
    }
};

OwnActions::Typed pressing(const QString& name, const QString& keys, bool danger = false)
{
    OwnActions::Typed typed;
    typed.name = name;
    typed.keys = keys;
    typed.danger = danger;
    return typed;
}

OwnActions::Typed typing(const QString& name, const QString& text, bool enter = false, bool danger = false)
{
    OwnActions::Typed typed;
    typed.name = name;
    typed.types = true;
    typed.text = text;
    typed.enter = enter;
    typed.danger = danger;
    return typed;
}

// A refusal as a test compares one: its code, and its sentence.
QString said(const base::Error& error)
{
    return QString::fromStdString(error.code) + u": "_s + QString::fromStdString(error.message);
}
template <typename Answer>
QString refusalOf(const base::Result<Answer>& result)
{
    return result.ok() ? u"(not refused)"_s : said(result.error());
}

// One entry of a text as the store holds it (own_actions.h), written out here and not asked of
// the program: what stands in the store is what a test looks at.
QByteArray entryOf(const QString& id, const QByteArray& mark, int part, int parts, const QByteArray& bytes)
{
    return "QKVM-ACTION-1\n" + id.toLatin1() + '\n' + mark + ' ' + QByteArray::number(part) + '/' +
           QByteArray::number(parts) + '\n' + bytes;
}
// The mark of the writing the list names for an action, and the place that writing is in.
QByteArray markOf(const OwnAction& action)
{
    return action.writing.sliced(2).toLatin1();
}
QChar placeOf(const OwnAction& action)
{
    return action.writing.at(0);
}

// An action as a wall hands it out, said in one line.
QString inOneLine(const SavedAction& action)
{
    const auto flag = [](bool is) { return is ? u"true"_s : u"false"_s; };
    return u"%1 %2 [%3] keys=%4 enter=%5 danger=%6 changed=%7"_s
        .arg(action.id, action.name, action.types ? u"types"_s : u"presses"_s, action.keys)
        .arg(flag(action.enter), flag(action.danger))
        .arg(action.changed);
}
QStringList inLines(const QList<SavedAction>& actions)
{
    QStringList lines;
    for (const SavedAction& action : actions) {
        lines.append(inOneLine(action));
    }
    return lines;
}

// The sign U+FEFF as a text of that one sign. Made of its number as a place of a text, and not
// by sign() (made_up_kvm.h): that reads its number as a text of four bytes a sign, where a first
// U+FEFF says which way round the bytes are, and is dropped. (So a row of this test was a text
// without the sign once, and passed whatever became of such a sign: seen when the rule was
// taken out and nothing failed.)
QString bom()
{
    return QString(QChar(0xFEFF));
}

// So many signs of three bytes each, and not the same one over and over: parts of a text that
// changed places would be another text.
QString threeBytesEach(int signs)
{
    QString text;
    for (int number = 0; number < signs; ++number) {
        text += QChar(0x4E00 + number % 997);
    }
    return text;
}

// What the made-up KVM did when it typed these bytes once.
QList<QByteArray> typedOnce(const QString& text, bool enter)
{
    return {"type " + text.toUtf8() + (enter ? "\n" : "")};
}

// What changes the store behind an action's back, for the test of what is no text.
struct Scene {
    QHash<QString, QByteArray>& entries;
    QString id;
    QByteArray mark;
    QString otherId;
    QByteArray otherMark;
    // The bytes of the action's text, which is kept in two parts.
    QByteArray bytes;

    QString a(int part) const { return OwnActions::secretOf(id, u'a', part); }
    QString b(int part) const { return OwnActions::secretOf(id, u'b', part); }
    QByteArray first() const { return bytes.first(OwnActions::partBytes); }
    QByteArray second() const { return bytes.sliced(OwnActions::partBytes); }
    // So many signs of four bytes each, as their bytes: 625 of them fill a part.
    static QByteArray faces(int signs) { return sign(0x1F600).repeated(signs).toUtf8(); }
    // Nothing is kept for the action any more, in either place.
    void empty() const
    {
        for (int part = 1; part <= OwnActions::mostParts; ++part) {
            entries.remove(a(part));
            entries.remove(b(part));
        }
    }
};
using Tampering = std::function<void(const Scene&)>;

// Everything Qt was told while a test ran, by whatever of the program said it.
QStringList& messages()
{
    static QStringList all;
    return all;
}
// Who was told before: what ends the program is said to that one as well.
QtMessageHandler& toldBefore()
{
    static QtMessageHandler handler = nullptr;
    return handler;
}
void noteMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    messages().append(message);
    // (A failed assertion is the last word of a test that went wrong, and nothing comes after
    // it: unsaid, the program ended without a word of why.)
    if (type == QtFatalMsg && toldBefore() != nullptr) {
        toldBefore()(type, context, message);
    }
}

} // namespace

Q_DECLARE_METATYPE(Tampering)

// The saved actions of this PC's own wall: what is typed in, how their texts are kept, and their
// runs against made-up KVMs.
//
// A text an action types is compared with QVERIFY here and never with QCOMPARE: a comparison that
// fails prints what it compared, and no text is to stand in what a test prints.
class TestOwnActions : public QObject {
    Q_OBJECT

private:
    template <typename Answer>
    static base::Result<Answer> answerTo(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
    {
        QList<base::Result<Answer>> answers;
        ask([&](base::Result<Answer> answer) { answers.append(std::move(answer)); });
        if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 20000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return answers.takeFirst();
    }
    static base::Result<core::Done> doneBy(LocalWall& wall, const QString& id, const core::Action& what)
    {
        return answerTo<core::Done>([&](auto done) { wall.act(id, what, std::move(done)); });
    }
    static QList<SavedAction> savedOf(LocalWall& wall)
    {
        return answerTo<QList<SavedAction>>([&](auto done) { wall.askSavedActions(std::move(done)); }).value();
    }

    // Under which Windows' own store is met by the one test that meets it, and what it used there.
    QString m_under = u"QKVM Desktop test of saved actions %1"_s.arg(QCoreApplication::applicationPid());
    QStringList m_used;

private slots:
    void initTestCase()
    {
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void cleanup()
    {
        const std::unique_ptr<core::SecretStore> windows = core::windowsSecrets(m_under);
        for (const QString& used : std::as_const(m_used)) {
            windows->forget(used);
        }
        m_used.clear();
    }

    // --- what is typed in ------------------------------------------------------------------------

    void whatIsTypedInIsRefusedInTheServersWords_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("types");
        QTest::addColumn<QString>("keys");
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("refused");
        const QString needed = u"invalid: name: a name is needed"_s;
        const QString oneLine = u"invalid: name: this has to be one line: no line breaks or control characters, and "
                                "nothing that turns the direction of the writing round"_s;
        const QString sixty = u"invalid: name: this may have 60 characters at the most"_s;
        const QString aKey = u"invalid: Choose at least one key"_s;
        const QString lock = u"MetaLeft,KeyL"_s;
        QTest::newRow("no name") << QString() << false << lock << QString() << needed;
        QTest::newRow("a name of blanks") << u"  \t "_s << false << lock << QString() << needed;
        QTest::newRow("a name of blanks of no width") << sign(0x200B) + bom() << false << lock << QString() << needed;
        QTest::newRow("a name of two lines") << u"Lock\nscreen"_s << false << lock << QString() << oneLine;
        QTest::newRow("a name that turns the writing round")
            << u"Lock"_s + sign(0x202E) + u"neercs"_s << false << lock << QString() << oneLine;
        QTest::newRow("a name with half a sign in it")
            << u"Lock"_s + QChar(0xD800) << false << lock << QString() << oneLine;
        QTest::newRow("a name of sixty-one signs") << QString(61, u'n') << false << lock << QString() << sixty;
        QTest::newRow("a name typed longer than four times sixty")
            << u"n"_s + QString(240, u' ') << false << lock << QString() << sixty;
        QTest::newRow("no keys") << u"Lock"_s << false << QString() << QString() << aKey;
        QTest::newRow("keys that are no names") << u"Lock"_s << false << u"ctrl+alt+del"_s << QString() << aKey;
        QTest::newRow("nine keys") << u"Lock"_s << false << u"KeyA,KeyB,KeyC,KeyD,KeyE,KeyF,KeyG,KeyH,KeyI"_s
                                   << QString() << aKey;
        QTest::newRow("keys and the end of a line") << u"Lock"_s << false << u"KeyA\n"_s << QString() << aKey;
        QTest::newRow("a key and more") << u"Lock"_s << false << u"KeyA&button=power"_s << QString() << aKey;
        // The keyboard's Power key shuts a machine down without the power question: refused where
        // it is saved, as the server's check_action refuses it (422 invalid, POWER_KEY_REFUSED).
        const QString power = u"invalid: The keyboard's Power key is not sent. A machine's power is under Power."_s;
        QTest::newRow("the keyboard's Power key") << u"Off"_s << false << u"Power"_s << QString() << power;
        QTest::newRow("the keyboard's Power key held with others")
            << u"Off"_s << false << u"ControlLeft,AltLeft,Power"_s << QString() << power;
        QTest::newRow("keys of more than 160 signs, of one that presses")
            << u"Lock"_s << false << QString(161, u'K') << QString()
            << u"invalid: keys: this may have 160 characters at the most"_s;
        QTest::newRow("keys of more than 160 signs, of one that types")
            << u"Sign in"_s << true << QString(161, u'K') << u"a text"_s
            << u"invalid: keys: this may have 160 characters at the most"_s;
        QTest::newRow("no text") << u"Sign in"_s << true << QString() << QString()
                                 << u"invalid: Enter the text to type"_s;
        QTest::newRow("a text of two thousand and one signs")
            << u"Sign in"_s << true << QString() << QString(2001, u'x')
            << u"invalid: text: this may have 2000 characters at the most"_s;
        QTest::newRow("a text of two thousand and one signs from far up")
            << u"Sign in"_s << true << QString() << sign(0x1F600).repeated(2001)
            << u"invalid: text: this may have 2000 characters at the most"_s;
        QTest::newRow("a text of more than two thousand signs, of one that presses")
            << u"Lock"_s << false << lock << QString(2001, u'x')
            << u"invalid: text: this may have 2000 characters at the most"_s;
        QTest::newRow("a text with half a sign in it")
            << u"Sign in"_s << true << QString() << u"pass"_s + QChar(0xD800)
            << u"invalid: text: this has half a character in it, which no keyboard types"_s;
    }

    void whatIsTypedInIsRefusedInTheServersWords()
    {
        QFETCH(QString, name);
        QFETCH(bool, types);
        QFETCH(QString, keys);
        QFETCH(QString, text);
        QFETCH(QString, refused);
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        // One that is there, to be changed to what was typed.
        const SavedAction there = actions.add(pressing(u"There"_s, u"F1"_s)).value();
        const QSignalSpy changed(&actions, &OwnActions::changed);
        OwnActions::Typed typed;
        typed.name = name;
        typed.types = types;
        typed.keys = keys;
        typed.text = text;
        // Refused as a new action and as a change, each in the same code and sentence.
        QCOMPARE(refusalOf(actions.add(typed)), refused);
        QCOMPARE(refusalOf(actions.change(there.id, there.changed, typed)), refused);
        // Nothing came of either: the list is what it was, in memory and in the file, and
        // nothing is in the store.
        QCOMPARE(inLines(actions.handedOut()), QStringList{inOneLine(there)});
        QCOMPARE(core::Settings(kept.file).remembered().actions, actions.all());
        QVERIFY(kept.entries.isEmpty());
        QVERIFY(changed.isEmpty());
    }

    void whatIsTypedInIsTakenUpToWhereTheRulesEnd()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        // A name of sixty signs; and what is kept of a name: without the blanks around it, a tab
        // as a blank, a blank of no width as nothing.
        QCOMPARE(actions.add(pressing(QString(60, u'n'), u"F1"_s)).value().name, QString(60, u'n'));
        QCOMPARE(actions.add(pressing(u"  Lock\tthe"_s + sign(0x200B) + u" screen  "_s, u"F1"_s)).value().name,
                 u"Lock the screen"_s);
        // One key, and eight.
        QCOMPARE(actions.add(pressing(u"Help"_s, u"F1"_s)).value().keys, u"F1"_s);
        const QString eight = u"KeyA,KeyB,KeyC,KeyD,KeyE,KeyF,KeyG,KeyH"_s;
        QCOMPARE(actions.add(pressing(u"Eight"_s, eight)).value().keys, eight);
        // A text of two thousand signs, of one place each and of two (a sign from far up is one
        // sign, as the server counts).
        QVERIFY(actions.add(typing(u"Long"_s, QString(2000, u'x'))).ok());
        QVERIFY(actions.add(typing(u"Faces"_s, sign(0x1F600).repeated(2000))).ok());
        // And as on the server, the text by itself is what is counted: two thousand signs and
        // Enter are saved. (A KVM's driver then refuses the run: below.)
        QVERIFY(actions.add(typing(u"Long, and Enter"_s, QString(2000, u'x'), true)).ok());
        // What the server reads as keys at all is 160 signs, of one that types too.
        OwnActions::Typed typed = typing(u"Sign in"_s, u"a text"_s);
        typed.keys = QString(160, u'K');
        QVERIFY(actions.add(typed).ok());
        QCOMPARE(actions.all().size(), 8);
    }

    void anActionIsKeptAsItWasTypedAndHandedOutWithoutItsText()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const QSignalSpy changed(&actions, &OwnActions::changed);
        // One that presses: Enter is not of it, whatever was ticked, and a text that was typed
        // beside it is kept nowhere.
        OwnActions::Typed lock = pressing(u"Lock"_s, u"MetaLeft,KeyL"_s);
        lock.enter = true;
        lock.text = u"typed beside the keys"_s;
        const SavedAction pressed = actions.add(lock).value();
        QVERIFY(QRegularExpression(u"\\A[0-9a-f]{8}\\z"_s).match(pressed.id).hasMatch());
        QCOMPARE(inOneLine(pressed),
                 pressed.id + u" Lock [presses] keys=MetaLeft,KeyL enter=false danger=false changed=0"_s);
        QVERIFY(kept.entries.isEmpty());
        QCOMPARE(actions.all().constFirst().writing, QString());
        // One that types: the keys that were typed beside it are not of it; Enter and "dangerous"
        // are as ticked; and the list names a writing of its text, in the first of the two places.
        OwnActions::Typed signIn = typing(u"Sign in"_s, u"a text to type"_s, true, true);
        signIn.keys = u"F1"_s;
        const SavedAction typed = actions.add(signIn).value();
        QCOMPARE(inOneLine(typed), typed.id + u" Sign in [types] keys= enter=true danger=true changed=0"_s);
        QVERIFY(QRegularExpression(u"\\Aa:[0-9a-f]{16}\\z"_s).match(actions.all().at(1).writing).hasMatch());
        // Two actions may have one name, as on the server: each has an id of its own.
        const SavedAction again = actions.add(pressing(u"Lock"_s, u"F2"_s)).value();
        QVERIFY(again.id != pressed.id && again.id != typed.id);
        // In the order they were added in; said once for each; and there at the next start.
        QCOMPARE(inLines(actions.handedOut()), (QStringList{inOneLine(pressed), inOneLine(typed), inOneLine(again)}));
        QCOMPARE(changed.count(), 3);
        QCOMPARE(core::Settings(kept.file).remembered().actions, actions.all());
    }

    void theTextAnActionTypesIsInNoFileNoSentenceNoLogAndNoAnswer()
    {
        const QString first = u"Tr0ub4dor-und-3-first"_s;
        const QString second = u"Tr0ub4dor-und-3-second"_s;
        const auto isIn = [&first, &second](const QByteArray& bytes) {
            for (const QString& text : {first, second}) {
                const QByteArray plain = text.toUtf8();
                const QByteArray wide(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
                if (bytes.contains(plain) || bytes.contains(wide) || bytes.contains(plain.toHex()) ||
                    bytes.contains(plain.toBase64())) {
                    return true;
                }
            }
            return false;
        };
        messages().clear();
        const QtMessageHandler before = qInstallMessageHandler(noteMessage);
        toldBefore() = before;
        const auto handlerBack = qScopeGuard([before] { qInstallMessageHandler(before); });
        // Everything the program said to anybody, gathered as it comes.
        QStringList told;
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        // How many files there are under the settings' folder, whatever they are (the settings,
        // and what was written beside them on the way); -1: one of them holds a text.
        const auto filesWithoutIt = [&own, &isIn] {
            int files = 0;
            QDirIterator all(own.folder.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
            while (all.hasNext()) {
                QFile file(all.next());
                if (!file.open(QIODevice::ReadOnly) || isIn(file.readAll())) {
                    return -1;
                }
                ++files;
            }
            return files;
        };
        const auto added = own.actions.add(typing(u"Sign in"_s, first, true));
        QVERIFY(added.ok());
        told.append(inOneLine(added.value()));
        QVERIFY(filesWithoutIt() >= 1);
        // (One that is added and never changed: what the file says of it at the end is what an
        // adding wrote.)
        const auto beside = own.actions.add(typing(u"Sign out"_s, second));
        QVERIFY(beside.ok());
        told.append(inOneLine(beside.value()));
        const auto altered = own.actions.change(added.value().id, 0, typing(u"Sign in"_s, second, true, true));
        QVERIFY(altered.ok());
        told.append(inOneLine(altered.value()));
        QVERIFY(filesWithoutIt() >= 1);
        told.append(inLines(savedOf(own.wall)));
        for (const OwnAction& action : own.actions.all()) {
            told.append(action.id + action.name + action.keys + action.writing);
        }
        // Refusals with a text in what was typed, and of a run: none has it in its sentence.
        told.append(refusalOf(own.actions.add(typing(QString(), first))));
        told.append(refusalOf(own.actions.add(typing(u"Two\nlines"_s, first))));
        told.append(refusalOf(own.actions.change(added.value().id, 0, typing(u"Sign in"_s, first))));
        own.secrets.inMemory.refuses = true;
        told.append(refusalOf(own.actions.add(typing(u"Not kept"_s, first))));
        told.append(refusalOf(own.actions.change(altered.value().id, 1, typing(u"Sign in"_s, first))));
        own.secrets.inMemory.refuses = false;
        {
            QFile held(own.file);
            QVERIFY(held.open(QIODevice::ReadOnly));
            told.append(refusalOf(own.actions.add(typing(u"Not written"_s, first))));
            told.append(refusalOf(own.actions.change(altered.value().id, 1, typing(u"Sign in"_s, first))));
        }
        told.append(refusalOf(doneBy(own.wall, machine, core::Action::run(added.value()))));
        const QList<SavedAction> listed = savedOf(own.wall);
        QCOMPARE(listed.size(), 2);
        const SavedAction& now = listed.constFirst();
        const auto ran = doneBy(own.wall, machine, core::Action::run(now));
        QVERIFY(ran.ok());
        told.append(ran.value().note);
        // It was typed, so it was there to be found.
        QVERIFY(kvm.did == typedOnce(second, true));
        told.append(messages());
        QVERIFY(told.size() > 14);
        for (const QString& one : std::as_const(told)) {
            QVERIFY(!isIn(one.toUtf8()));
        }
        // No file holds it at the end either, after all that was refused and taken back.
        QVERIFY(filesWithoutIt() >= 1);
        // And it is where it belongs, and there alone: in the store.
        bool isKept = false;
        for (const QByteArray& entry : std::as_const(own.entries)) {
            isKept = isKept || isIn(entry);
        }
        QVERIFY(isKept);
    }

    // --- how a text is kept ----------------------------------------------------------------------

    void aLongTextIsKeptInAsManyEntriesAsItTakesAndEachIsOneWindowsKeeps()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        // Two thousand signs of three bytes each: six thousand bytes, where one entry of Windows'
        // store holds 2560.
        const QString text = threeBytesEach(2000);
        const QByteArray bytes = text.toUtf8();
        QCOMPARE(bytes.size(), 6000);
        const SavedAction added = actions.add(typing(u"Long"_s, text)).value();
        const OwnAction action = actions.all().constFirst();
        const QByteArray mark = markOf(action);
        // Three entries, under the action's id, in the first place: each says what it is, whose,
        // of which writing and which part, and holds its part of the bytes. (A part ends in the
        // middle of a sign: they are joined as bytes.)
        QCOMPARE(kept.entries.size(), 3);
        QVERIFY(kept.entries.value(u"action:%1:a1"_s.arg(added.id)) ==
                entryOf(added.id, mark, 1, 3, bytes.first(2500)));
        QVERIFY(kept.entries.value(u"action:%1:a2"_s.arg(added.id)) ==
                entryOf(added.id, mark, 2, 3, bytes.sliced(2500, 2500)));
        QVERIFY(kept.entries.value(u"action:%1:a3"_s.arg(added.id)) ==
                entryOf(added.id, mark, 3, 3, bytes.sliced(5000)));
        for (const QByteArray& entry : std::as_const(kept.entries)) {
            QVERIFY(entry.size() <= core::largestSecretBytes);
        }
        // The longest there is, two thousand signs of four bytes each, takes four.
        const SavedAction faces = actions.add(typing(u"Faces"_s, sign(0x1F600).repeated(2000))).value();
        QCOMPARE(kept.entries.size(), 7);
        QVERIFY(kept.entries.contains(OwnActions::secretOf(faces.id, u'a', 4)));
        // And a short one, one.
        const SavedAction brief = actions.add(typing(u"Short"_s, u"ls"_s)).value();
        QCOMPARE(kept.entries.size(), 8);
        QVERIFY(kept.entries.value(OwnActions::secretOf(brief.id, u'a', 1)) ==
                entryOf(brief.id, markOf(actions.all().constLast()), 1, 1, "ls"));
        // Nothing of the store was read for any of it.
        QCOMPARE(kept.secrets.reads, 0);
    }

    void whereOneEntryOfATextCannotBeWrittenNoneOfItStays()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const QSignalSpy changed(&actions, &OwnActions::changed);
        const QString text(2000, QChar(0x20AC));
        // The third entry is not taken.
        kept.secrets.inMemory.refusesTheKeep = kept.secrets.inMemory.keeps + 3;
        const auto refused = actions.add(typing(u"Long"_s, text));
        QCOMPARE(refusalOf(refused),
                 u"not_saved: The new action could not be saved (Windows did not keep it.), so nothing was changed."_s);
        QCOMPARE(kept.secrets.inMemory.keeps, 3);
        // Nothing of it stays: not the two entries that were written, and no action without a text.
        QVERIFY(kept.entries.isEmpty());
        QVERIFY(actions.all().isEmpty());
        QVERIFY(core::Settings(kept.file).remembered().actions.isEmpty());
        QVERIFY(changed.isEmpty());

        // So with a change: of the new text none stays, and the text the action had is as it was.
        const SavedAction added = actions.add(typing(u"Short"_s, u"the text it has"_s)).value();
        const QHash<QString, QByteArray> entriesBefore = kept.entries;
        const QList<OwnAction> before = actions.all();
        kept.secrets.inMemory.refusesTheKeep = kept.secrets.inMemory.keeps + 2;
        QCOMPARE(refusalOf(actions.change(added.id, added.changed, typing(u"Long now"_s, text))),
                 u"not_saved: The change to this action could not be saved (Windows did not keep it.), so nothing was "
                 "changed."_s);
        QVERIFY(kept.entries == entriesBefore);
        // (The list was never touched: the action is in the state it was in, number and all.)
        QCOMPARE(actions.all(), before);
        QCOMPARE(changed.count(), 1);
    }

    void noMoreThanFiveHundredAreKept()
    {
        Kept kept;
        // A list of four hundred and ninety-nine, as a file has it.
        core::Remembered full;
        for (int number = 0; number < core::mostSavedActions - 1; ++number) {
            OwnAction action;
            action.id = u"%1"_s.arg(number, 8, 16, QLatin1Char('0'));
            action.name = u"Action %1"_s.arg(number);
            action.keys = u"F1"_s;
            full.actions.append(action);
        }
        QVERIFY(kept.settings.remember(full).ok());
        OwnActions actions(kept.settings, kept.secrets);
        // The five hundredth is taken, and the next is not, whichever kind it is of.
        const auto last = actions.add(pressing(u"The five hundredth"_s, u"F2"_s));
        QVERIFY(last.ok());
        const QString full500 =
            u"too_many: No more than 500 saved actions are kept. Remove one before adding another."_s;
        QCOMPARE(refusalOf(actions.add(pressing(u"One more"_s, u"F3"_s))), full500);
        QCOMPARE(refusalOf(actions.add(typing(u"One more"_s, u"a text"_s))), full500);
        QCOMPARE(actions.all().size(), 500);
        QVERIFY(kept.entries.isEmpty());
        QCOMPARE(core::Settings(kept.file).remembered().actions.size(), 500);
        // Where one is removed, there is room for one.
        QVERIFY(actions.remove(last.value().id, last.value().changed).ok());
        QVERIFY(actions.add(typing(u"One more"_s, u"a text"_s)).ok());
        QCOMPARE(actions.all().size(), 500);
    }

    // --- changing and removing -------------------------------------------------------------------

    void aChangeCountsUpAndKeepsItsPlaceOnTheList()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const SavedAction first = actions.add(pressing(u"First"_s, u"F1"_s)).value();
        const SavedAction lock = actions.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s)).value();
        const SavedAction last = actions.add(pressing(u"Last"_s, u"F3"_s)).value();
        const QSignalSpy changed(&actions, &OwnActions::changed);
        const SavedAction now =
            actions.change(lock.id, lock.changed, pressing(u"Lock it"_s, u"MetaLeft,KeyL"_s, true)).value();
        QCOMPARE(inOneLine(now),
                 lock.id + u" Lock it [presses] keys=MetaLeft,KeyL enter=false danger=true changed=1"_s);
        QCOMPARE(inLines(actions.handedOut()), (QStringList{inOneLine(first), inOneLine(now), inOneLine(last)}));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(core::Settings(kept.file).remembered().actions, actions.all());
        // And once more, from the state it is in now.
        QCOMPARE(actions.change(lock.id, 1, pressing(u"Lock it"_s, u"MetaLeft,KeyL"_s)).value().changed, 2);
    }

    void anEmptyTextOnAChangeKeepsTheTextThatIsSaved()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const SavedAction added = actions.add(typing(u"Sign in"_s, u"the text it has"_s)).value();
        const QString writing = actions.all().constFirst().writing;
        const QHash<QString, QByteArray> entriesBefore = kept.entries;
        const int keeps = kept.secrets.inMemory.keeps;
        const SavedAction now =
            actions.change(added.id, added.changed, typing(u"Sign in again"_s, QString(), true)).value();
        QCOMPARE(inOneLine(now), added.id + u" Sign in again [types] keys= enter=true danger=false changed=1"_s);
        // Its text is the one it had, as the same writing; and nothing of the store was read or
        // written for that.
        QCOMPARE(actions.all().constFirst().writing, writing);
        QVERIFY(kept.entries == entriesBefore);
        QCOMPARE(kept.secrets.inMemory.keeps, keeps);
        QCOMPARE(kept.secrets.reads, 0);
    }

    void aNewTextOnAChangeIsANewWritingAndTheOneBeforeIsForgotten()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const SavedAction added = actions.add(typing(u"Sign in"_s, u"the first text"_s)).value();
        const OwnAction first = actions.all().constFirst();
        QCOMPARE(placeOf(first), u'a');
        // In the other place, under a mark of its own; and then the first place is empty.
        QVERIFY(actions.change(added.id, 0, typing(u"Sign in"_s, u"the second text"_s)).ok());
        const OwnAction second = actions.all().constFirst();
        QCOMPARE(placeOf(second), u'b');
        QVERIFY(markOf(second) != markOf(first));
        QCOMPARE(kept.entries.size(), 1);
        QVERIFY(kept.entries.value(OwnActions::secretOf(added.id, u'b', 1)) ==
                entryOf(added.id, markOf(second), 1, 1, "the second text"));
        // And back into the first place with the third; a long one after a short one, and a
        // short one after a long one, each time with nothing of the one before left.
        const QString longOne(2000, QChar(0x20AC));
        QVERIFY(actions.change(added.id, 1, typing(u"Sign in"_s, longOne)).ok());
        QCOMPARE(placeOf(actions.all().constFirst()), u'a');
        QCOMPARE(kept.entries.size(), 3);
        QVERIFY(actions.change(added.id, 2, typing(u"Sign in"_s, u"the fourth text"_s)).ok());
        const OwnAction fourth = actions.all().constFirst();
        QCOMPARE(placeOf(fourth), u'b');
        QCOMPARE(kept.entries.size(), 1);
        QVERIFY(kept.entries.value(OwnActions::secretOf(added.id, u'b', 1)) ==
                entryOf(added.id, markOf(fourth), 1, 1, "the fourth text"));
        // Nothing of the store was read for any of it: no text passes through a change.
        QCOMPARE(kept.secrets.reads, 0);
    }

    void anActionThatPressesFromNowOnHasItsTextForgotten()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const SavedAction added = actions.add(typing(u"Sign in"_s, u"the text it has"_s, true)).value();
        const SavedAction pressed = actions.change(added.id, 0, pressing(u"Sign in"_s, u"Enter"_s)).value();
        QCOMPARE(inOneLine(pressed), added.id + u" Sign in [presses] keys=Enter enter=false danger=false changed=1"_s);
        QVERIFY(kept.entries.isEmpty());
        QCOMPARE(actions.all().constFirst().writing, QString());
        // To type again it needs a text: it has none that could stay.
        QCOMPARE(refusalOf(actions.change(added.id, 1, typing(u"Sign in"_s, QString()))),
                 u"invalid: Enter the text to type"_s);
        QVERIFY(actions.change(added.id, 1, typing(u"Sign in"_s, u"typed anew"_s)).ok());
        QCOMPARE(kept.entries.size(), 1);
    }

    void aChangeNamesTheStateThatWasLookedAt()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const SavedAction looked = actions.add(typing(u"Sign in"_s, u"the text it has"_s)).value();
        // Two windows: the one changes it, the other still holds what it looked at.
        QVERIFY(actions.change(looked.id, looked.changed, typing(u"Sign in there"_s, QString(), true)).ok());
        const QList<OwnAction> asItIs = actions.all();
        const QHash<QString, QByteArray> entries = kept.entries;
        const QSignalSpy changed(&actions, &OwnActions::changed);
        QCOMPARE(refusalOf(actions.change(looked.id, looked.changed, pressing(u"Mine"_s, u"F1"_s))),
                 u"changed: \"Sign in there\" was changed since you opened this list. Look at it again before changing "
                 "it."_s);
        // A number it never had is no state that was looked at either.
        QVERIFY(!actions.change(looked.id, 7, typing(u"Mine"_s, u"another text"_s)).ok());
        QVERIFY(!actions.change(looked.id, -1, pressing(u"Mine"_s, u"F1"_s)).ok());
        QCOMPARE(actions.all(), asItIs);
        QVERIFY(kept.entries == entries);
        QVERIFY(changed.isEmpty());
        // One that is not on the list, in the server's words.
        QCOMPARE(refusalOf(actions.change(u"00000000"_s, 0, pressing(u"Mine"_s, u"F1"_s))),
                 u"not_found: Unknown action"_s);
        // Looked at as it is, it is changed. (Named by the list's own text for it, which is gone
        // under the call.)
        QVERIFY(actions.change(actions.all().constFirst().id, 1, pressing(u"Mine"_s, u"F1"_s)).ok());
        QCOMPARE(actions.all().constFirst().name, u"Mine"_s);
    }

    void aRemovalNamesTheStateThatWasLookedAt()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const SavedAction looked = actions.add(typing(u"Sign in"_s, u"the text it has"_s)).value();
        // A box that was left open over a change: what it would remove is not what it shows.
        QVERIFY(actions.change(looked.id, looked.changed, typing(u"Sign in there"_s, QString(), true)).ok());
        const QList<OwnAction> asItIs = actions.all();
        const QHash<QString, QByteArray> entries = kept.entries;
        const QSignalSpy changed(&actions, &OwnActions::changed);
        QCOMPARE(refusalOf(actions.remove(looked.id, looked.changed)),
                 u"changed: \"Sign in there\" was changed since you opened this list. Look at it again before removing "
                 "it."_s);
        QVERIFY(!actions.remove(looked.id, -1).ok());
        QCOMPARE(actions.all(), asItIs);
        QVERIFY(kept.entries == entries);
        QVERIFY(changed.isEmpty());
        QCOMPARE(refusalOf(actions.remove(u"00000000"_s, 0)), u"not_found: Unknown action"_s);
        // Looked at as it is, it is removed.
        QVERIFY(actions.remove(actions.all().constFirst().id, 1).ok());
        QVERIFY(actions.all().isEmpty());
    }

    void aNameCannotEndItsOwnSettingOffInASentence()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const QString name = u"x\" was removed. Run \"y"_s;
        const SavedAction added = actions.add(pressing(name, u"F1"_s)).value();
        QCOMPARE(added.name, name);
        const QString sentence = refusalOf(actions.remove(added.id, 5));
        QCOMPARE(sentence, u"changed: \"x' was removed. Run 'y\" was changed since you opened this list. Look at it "
                           "again before removing it."_s);
        QCOMPARE(sentence.count(u'"'), 2);
    }

    void removingAnActionForgetsItsTextInEitherPlace()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const SavedAction stays = actions.add(typing(u"Stays"_s, u"its own text"_s)).value();
        const SavedAction added = actions.add(typing(u"Long"_s, QString(2000, QChar(0x20AC)))).value();
        // (What a change that was cut off may have left in the other place goes too.)
        kept.entries.insert(OwnActions::secretOf(added.id, u'b', 1), "left behind");
        kept.entries.insert(OwnActions::secretOf(added.id, u'b', 4), "left behind");
        QCOMPARE(kept.entries.size(), 6);
        const QSignalSpy changed(&actions, &OwnActions::changed);
        QVERIFY(actions.remove(added.id, added.changed).ok());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(inLines(actions.handedOut()), QStringList{inOneLine(stays)});
        QCOMPARE(kept.entries.keys(), QStringList{OwnActions::secretOf(stays.id, u'a', 1)});
        QCOMPARE(core::Settings(kept.file).remembered().actions, actions.all());
    }

    void whatCannotBeWrittenDownIsNotAdded()
    {
        // The settings are to go where no file can be: under a file.
        const QTemporaryDir folder;
        QFile inTheWay(folder.filePath(u"in-the-way"_s));
        QVERIFY(inTheWay.open(QIODevice::WriteOnly));
        inTheWay.close();
        core::Settings settings(folder.filePath(u"in-the-way/settings.json"_s));
        AsWindowsKeeps secrets;
        OwnActions actions(settings, secrets);
        const QSignalSpy changed(&actions, &OwnActions::changed);
        const QString refused = refusalOf(actions.add(typing(u"Sign in"_s, QString(2000, QChar(0x20AC)), true)));
        QVERIFY2(refused.startsWith(u"not_saved: The new action could not be saved ("_s), qPrintable(refused));
        QVERIFY2(refused.endsWith(u"), so nothing was changed."_s), qPrintable(refused));
        // Nothing of it is anywhere: not on the list, and no text of an action that is not there.
        QCOMPARE(secrets.inMemory.keeps, 3);
        QVERIFY(secrets.inMemory.all.isEmpty());
        QVERIFY(actions.all().isEmpty());
        QVERIFY(changed.isEmpty());
        QVERIFY(!actions.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s)).ok());
        QVERIFY(actions.all().isEmpty());
    }

    void aChangeThatCouldNotBeWrittenDownIsTakenBackAndStandsUnderANumberNobodyWasShown()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const SavedAction added = actions.add(typing(u"Sign in"_s, u"the text it has"_s)).value();
        const OwnAction before = actions.all().constFirst();
        const QHash<QString, QByteArray> entriesBefore = kept.entries;
        const QSignalSpy changed(&actions, &OwnActions::changed);
        {
            // (Something holds the settings' file: it cannot be replaced.)
            QFile held(kept.file);
            QVERIFY(held.open(QIODevice::ReadOnly));
            const QString refused = refusalOf(
                actions.change(added.id, added.changed, typing(u"Sign in elsewhere"_s, u"another text"_s, true, true)));
            QVERIFY2(refused.startsWith(u"not_saved: The change to this action could not be saved ("_s),
                     qPrintable(refused));
            QVERIFY2(refused.endsWith(u"), so nothing was changed."_s), qPrintable(refused));
            // It is what it was, its text too: the new one is nowhere.
            OwnAction asItWas = before;
            asItWas.changed = 2;
            QCOMPARE(actions.all(), QList<OwnAction>{asItWas});
            QVERIFY(kept.entries == entriesBefore);
            // And under a number nobody was shown: not the one it had, and not the next. That
            // is said, since whoever holds the list holds a number that is no longer its.
            QCOMPARE(changed.count(), 1);
            // The same of one that would have pressed from now on, from the number it has now.
            QVERIFY(!actions.change(added.id, 2, pressing(u"Sign in"_s, u"Enter"_s)).ok());
            QCOMPARE(actions.all().constFirst().changed, 4);
            QVERIFY(actions.all().constFirst().types);
            QVERIFY(kept.entries == entriesBefore);
            QCOMPARE(changed.count(), 2);
        }
        // What it stands under is written with the next thing that is.
        QCOMPARE(actions.change(added.id, 4, typing(u"Sign in"_s, QString())).value().changed, 5);
        QCOMPARE(core::Settings(kept.file).remembered().actions, actions.all());
        QVERIFY(kept.entries == entriesBefore);
    }

    void anActionChangedAsOftenAsAnyMayBeIsNotChangedOnceMoreAndStaysOnTheList()
    {
        // (Only a settings' file made by hand has such a count. A change adds one to it, or two
        // where it could not be written down: one that took an action past the most that is read
        // back would have it gone, without a word, at the next start.)
        Kept kept;
        {
            OwnActions first(kept.settings, kept.secrets);
            QVERIFY(first.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s)).ok());
        }
        const auto plant = [&kept](int changed) {
            core::Remembered planted = kept.settings.remembered();
            planted.actions[0].changed = changed;
            return kept.settings.remember(planted).ok();
        };
        for (const int most : {core::action::mostEdits, core::action::mostEdits - 1}) {
            QVERIFY(plant(most));
            OwnActions actions(kept.settings, kept.secrets);
            const OwnAction before = actions.all().constFirst();
            QCOMPARE(before.changed, most);
            const QString refused =
                refusalOf(actions.change(before.id, before.changed, pressing(u"Lock now"_s, u"MetaLeft,KeyL"_s)));
            QVERIFY2(refused.startsWith(u"invalid: "_s), qPrintable(refused));
            QCOMPARE(actions.all(), QList<OwnAction>{before});
            QCOMPARE(core::Settings(kept.file).remembered().actions, QList<OwnAction>{before});
        }
        // Below that it is changed as any other, and read back as it was written.
        QVERIFY(plant(core::action::mostEdits - 2));
        OwnActions actions(kept.settings, kept.secrets);
        const OwnAction before = actions.all().constFirst();
        QVERIFY(actions.change(before.id, before.changed, pressing(u"Lock now"_s, u"MetaLeft,KeyL"_s)).ok());
        QCOMPARE(actions.all().constFirst().changed, core::action::mostEdits - 1);
        QCOMPARE(core::Settings(kept.file).remembered().actions, actions.all());
    }

    void aRemovalThatCouldNotBeWrittenDownLeavesTheActionAndItsText()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const SavedAction added = actions.add(typing(u"Sign in"_s, u"the text it has"_s)).value();
        const QList<OwnAction> before = actions.all();
        const QHash<QString, QByteArray> entriesBefore = kept.entries;
        const QSignalSpy changed(&actions, &OwnActions::changed);
        {
            QFile held(kept.file);
            QVERIFY(held.open(QIODevice::ReadOnly));
            const QString refused = refusalOf(actions.remove(added.id, added.changed));
            QVERIFY2(refused.startsWith(u"not_saved: Removing this action could not be saved ("_s),
                     qPrintable(refused));
            QCOMPARE(actions.all(), before);
            QVERIFY(kept.entries == entriesBefore);
            QVERIFY(changed.isEmpty());
        }
        QVERIFY(actions.remove(added.id, added.changed).ok());
        QVERIFY(kept.entries.isEmpty());
    }

    // --- running ---------------------------------------------------------------------------------

    void theWallHandsItsSavedActionsOutAsAServerDoesAndLater()
    {
        Own own;
        const SavedAction lock = own.actions.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s)).value();
        const SavedAction signIn = own.actions.add(typing(u"Sign in"_s, u"a text"_s, true, true)).value();
        QList<base::Result<QList<SavedAction>>> answers;
        own.wall.askSavedActions([&](const base::Result<QList<SavedAction>>& answer) { answers.append(answer); });
        // Later, as everything is answered: never from inside the asking.
        QVERIFY(answers.isEmpty());
        QTRY_COMPARE(answers.size(), 1);
        QCOMPARE(inLines(answers.constFirst().value()), (QStringList{inOneLine(lock), inOneLine(signIn)}));
        QCOMPARE(own.secrets.reads, 0);
    }

    void aRunPressesExactlyTheKeys()
    {
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        const SavedAction lock = own.actions.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s)).value();
        const SavedAction three =
            own.actions.add(pressing(u"Three fingers"_s, u"ControlLeft,AltLeft,Delete"_s)).value();
        QVERIFY(doneBy(own.wall, machine, core::Action::run(three)).ok());
        QVERIFY(doneBy(own.wall, machine, core::Action::run(lock)).ok());
        QCOMPARE(kvm.did, (QList<QByteArray>{"keys ControlLeft,AltLeft,Delete"_ba, "keys MetaLeft,KeyL"_ba}));
        // Nothing was typed, and nothing of the store was read.
        QCOMPARE(kvm.asked("/api/hid/print"), 0);
        QCOMPARE(own.secrets.reads, 0);
    }

    void aRunTypesExactlyTheTextWithEnterOrWithout_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("enter");
        QTest::newRow("a line") << u"ipconfig /all"_s << false;
        QTest::newRow("a line and Enter") << u"ipconfig /all"_s << true;
        QTest::newRow("blanks around it") << u"  ls -l  "_s << false;
        QTest::newRow("quotes, strokes and what a command line reads")
            << u"a \"b\" 'c' \\ & | < > ^ % $ ` ~ #"_s << true;
        QTest::newRow("ends of lines and a tab inside") << u"one\ntwo\r\n\tthree"_s << false;
        QTest::newRow("an end of a line at its end, and Enter") << u"one\n"_s << true;
        QTest::newRow("letters of other writings and a sign from far up")
            << sign(0xE4) + sign(0x20AC) + sign(0x4E2D) + sign(0x1F600) << true;
        QTest::newRow("U+FEFF in front") << bom() + u"pass"_s << false;
        QTest::newRow("U+FEFF in front and inside, and Enter") << bom() + u"pa"_s + bom() + u"ss"_s << true;
        QTest::newRow("a blank of no width inside") << u"pa"_s + sign(0x200B) + u"ss"_s << false;
        QTest::newRow("a sign that turns the writing round") << u"pa"_s + sign(0x202E) + u"ss"_s << true;
        QTest::newRow("a nought among its signs") << u"a"_s + QChar(0) + u"b"_s << false;
        QTest::newRow("one blank") << u" "_s << true;
    }

    void aRunTypesExactlyTheTextWithEnterOrWithout()
    {
        QFETCH(QString, text);
        QFETCH(bool, enter);
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        const SavedAction saved = own.actions.add(typing(u"Typed"_s, text, enter)).value();
        QVERIFY(doneBy(own.wall, machine, core::Action::run(saved)).ok());
        // Once, sign for sign as it was typed in, with Enter behind it where that was ticked.
        QCOMPARE(kvm.did.size(), 1);
        QVERIFY(kvm.did == typedOnce(text, enter));
        QCOMPARE(kvm.last("/api/hid/print")->path, "/api/hid/print?limit=0"_ba);
        // Read from the store for that run: its one entry, once.
        QCOMPARE(own.secrets.reads, 1);
        // And a second run reads it again, and types the same.
        QVERIFY(doneBy(own.wall, machine, core::Action::run(saved)).ok());
        QCOMPARE(own.secrets.reads, 2);
        QVERIFY(kvm.did == typedOnce(text, enter) + typedOnce(text, enter));
    }

    void aTextOfTwoThousandSignsOfThreeBytesEachIsKeptAndTypedWhole()
    {
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        const QString text = threeBytesEach(2000);
        const qsizetype entriesBefore = own.entries.size();
        const SavedAction saved = own.actions.add(typing(u"Long"_s, text)).value();
        QCOMPARE(own.entries.size(), entriesBefore + 3);
        QVERIFY(doneBy(own.wall, machine, core::Action::run(saved)).ok());
        QCOMPARE(kvm.last("/api/hid/print")->body.size(), 6000);
        QVERIFY(kvm.did == typedOnce(text, false));
        QCOMPARE(own.secrets.reads, 3);
        // So with the longest there is: two thousand signs of four bytes each, in four entries.
        const QString faces = sign(0x1F600).repeated(2000);
        const SavedAction longest = own.actions.add(typing(u"Faces"_s, faces)).value();
        QVERIFY(doneBy(own.wall, machine, core::Action::run(longest)).ok());
        QCOMPARE(kvm.last("/api/hid/print")->body.size(), 8000);
        QVERIFY(kvm.did == typedOnce(text, false) + typedOnce(faces, false));
    }

    void twoThousandSignsAndEnterAreRefusedByTheKvmsDriverAsOnTheServer()
    {
        // The server saves such an action and its driver refuses the run: Enter is one sign more
        // than a KVM types at once. So here.
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        const SavedAction saved = own.actions.add(typing(u"Long, and Enter"_s, QString(2000, u'x'), true)).value();
        QCOMPARE(refusalOf(doneBy(own.wall, machine, core::Action::run(saved))),
                 u"invalid: Text must be 1 to 2000 characters"_s);
        QVERIFY(kvm.did.isEmpty());
        QCOMPARE(kvm.asked("/api/hid/print"), 0);
        // And the KVM is free for the next thing.
        QVERIFY(doneBy(own.wall, machine, core::Action::press(u"F1"_s)).ok());
    }

    void anActionThatIsMarkedAsDangerousRunsAsAnyOther()
    {
        // Asking first is for whoever shows it, which is why it is handed out as dangerous.
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        QVERIFY(own.actions.add(pressing(u"Three fingers"_s, u"ControlLeft,AltLeft,Delete"_s, true)).ok());
        QVERIFY(own.actions.add(typing(u"Wipe"_s, u"a text"_s, true, true)).ok());
        const QList<SavedAction> handed = savedOf(own.wall);
        QCOMPARE(handed.size(), 2);
        QVERIFY(handed.at(0).danger && handed.at(1).danger);
        QVERIFY(doneBy(own.wall, machine, core::Action::run(handed.at(0))).ok());
        QVERIFY(doneBy(own.wall, machine, core::Action::run(handed.at(1))).ok());
        QVERIFY(kvm.did == QList<QByteArray>{"keys ControlLeft,AltLeft,Delete"_ba} + typedOnce(u"a text"_s, true));
    }

    void whatWasLookedAtIsWhatRuns()
    {
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        // A window lists an action that presses a key, and holds that list.
        const SavedAction looked = own.actions.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s)).value();
        // Changed meanwhile: it types now, where it pressed.
        const SavedAction now =
            own.actions.change(looked.id, looked.changed, typing(u"Lock"_s, u"a text"_s, true)).value();
        const auto refused = doneBy(own.wall, machine, core::Action::run(looked));
        QCOMPARE(refusalOf(refused),
                 u"changed: \"Lock\" was changed since you opened this list. Look at it again before running it."_s);
        QVERIFY(!own.wall.mayHaveBeenDone(machine, refused.error()));
        // Nothing went to the KVM for it, and nothing of the store was read.
        QVERIFY(kvm.did.isEmpty());
        QCOMPARE(own.secrets.reads, 0);
        // Looked at again, it runs as what it is now.
        QVERIFY(doneBy(own.wall, machine, core::Action::run(now)).ok());
        QVERIFY(kvm.did == typedOnce(u"a text"_s, true));
        // Removed meanwhile: the server's words for one that is not there any more.
        QVERIFY(own.actions.remove(now.id, now.changed).ok());
        const auto gone = doneBy(own.wall, machine, core::Action::run(now));
        QCOMPARE(refusalOf(gone), u"not_found: That saved action no longer exists"_s);
        QVERIFY(!own.wall.mayHaveBeenDone(machine, gone.error()));
        QCOMPARE(refusalOf(doneBy(own.wall, machine, core::Action::run(SavedAction{}))),
                 u"not_found: That saved action no longer exists"_s);
        QCOMPARE(kvm.did.size(), 1);
        // And to a KVM that is not on the wall, nothing, whatever the action.
        QCOMPARE(refusalOf(doneBy(own.wall, u"nobody"_s, core::Action::run(now))), u"not_found: Unknown device"_s);
    }

    void afterAChangeThatCouldNotBeWrittenDownNoNumberThatWasOrWouldHaveBeenShownRuns()
    {
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        const SavedAction looked = own.actions.add(typing(u"Sign in"_s, u"the text it has"_s)).value();
        {
            QFile held(own.file);
            QVERIFY(held.open(QIODevice::ReadOnly));
            QVERIFY(!own.actions.change(looked.id, looked.changed, typing(u"Sign in"_s, u"another text"_s, true)).ok());
        }
        // Not under the number it had, and not under the one the change would have been shown under.
        SavedAction next = looked;
        next.changed = looked.changed + 1;
        const QString changed =
            u"changed: \"Sign in\" was changed since you opened this list. Look at it again before running it."_s;
        QCOMPARE(refusalOf(doneBy(own.wall, machine, core::Action::run(looked))), changed);
        QCOMPARE(refusalOf(doneBy(own.wall, machine, core::Action::run(next))), changed);
        QVERIFY(kvm.did.isEmpty());
        // As the list hands it out now, it runs: and is what it was, not what the change that
        // was taken back would have made it.
        const QList<SavedAction> handed = savedOf(own.wall);
        QCOMPARE(handed.size(), 1);
        const SavedAction& listed = handed.constFirst();
        QCOMPARE(listed.changed, looked.changed + 2);
        QVERIFY(doneBy(own.wall, machine, core::Action::run(listed)).ok());
        QVERIFY(kvm.did == typedOnce(u"the text it has"_s, false));
    }

    void oneThingAtATimeGoesToAKvmAlsoWhereItIsASavedAction()
    {
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        const SavedAction slow = own.actions.add(typing(u"Slow"_s, u"a long text"_s)).value();
        const SavedAction lock = own.actions.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s)).value();
        const SavedAction second = own.actions.add(typing(u"Second"_s, u"another text"_s, true)).value();
        // The KVM takes its time over the first text.
        Answered late = good();
        late.afterMs = 600;
        kvm.first["/api/hid/print"_ba].append(late);
        QList<base::Result<core::Done>> firstDone;
        own.wall.act(machine, core::Action::run(slow),
                     [&](const base::Result<core::Done>& result) { firstDone.append(result); });
        const int reads = own.secrets.reads;
        QCOMPARE(reads, 1);
        // While it is out, a second saved action is refused in the server's words, whichever kind
        // it is of; and the text of one that types is not read for a run that does not go out.
        const QString busy =
            u"busy: Something else is being done to \"Build server\" at this moment. Wait for it to finish."_s;
        QCOMPARE(refusalOf(doneBy(own.wall, machine, core::Action::run(lock))), busy);
        QCOMPARE(refusalOf(doneBy(own.wall, machine, core::Action::run(second))), busy);
        QCOMPARE(own.secrets.reads, reads);
        // So is anything else under a saved action that is out.
        QCOMPARE(refusalOf(doneBy(own.wall, machine, core::Action::press(u"F1"_s))), busy);
        QTRY_COMPARE(firstDone.size(), 1);
        QVERIFY(firstDone.constFirst().ok());
        // The KVM was asked to type once, the first text, and nothing else was asked of it for
        // what was refused. (The answer that came late was the test's own, in the KVM's place.)
        QCOMPARE(kvm.asked("/api/hid/print"), 1);
        QVERIFY(kvm.last("/api/hid/print")->body == "a long text"_ba);
        QVERIFY(kvm.did.isEmpty());
        // And after it the next.
        QVERIFY(doneBy(own.wall, machine, core::Action::run(lock)).ok());
        QVERIFY(doneBy(own.wall, machine, core::Action::run(second)).ok());
        QVERIFY(kvm.did == QList<QByteArray>{"keys MetaLeft,KeyL"_ba} + typedOnce(u"another text"_s, true));
    }

    void whatAKvmWillNotDoAndWhatMayHaveBeenDoneAreSaidAsOfAnyAction()
    {
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        const SavedAction lock = own.actions.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s)).value();
        const SavedAction signIn = own.actions.add(typing(u"Sign in"_s, u"a text"_s, true)).value();
        // What the KVM says no to is a no, in its own words.
        kvm.instead.insert("/api/hid/events/send_shortcut"_ba, refusal(400, u"BadRequestError"_s, u"No such key"_s));
        const auto refused = doneBy(own.wall, machine, core::Action::run(lock));
        QCOMPARE(refusalOf(refused), u"action_failed: No such key"_s);
        QVERIFY(!own.wall.mayHaveBeenDone(machine, refused.error()));
        // An error in the middle of a text leaves open whether it was typed.
        kvm.instead.insert("/api/hid/print"_ba, Answered{502, "<html>gateway</html>"_ba, "text/html"_ba});
        const auto unknown = doneBy(own.wall, machine, core::Action::run(signIn));
        QVERIFY(!unknown.ok());
        QCOMPARE(unknown.error().code, std::string("action_unknown"));
        QVERIFY(own.wall.mayHaveBeenDone(machine, unknown.error()));
        // And the KVM is free again after each.
        kvm.instead.clear();
        QVERIFY(doneBy(own.wall, machine, core::Action::run(lock)).ok());
    }

    void keysThatNameTheKeyboardsPowerKeyAreRefusedBeforeAnythingIsSent()
    {
        // The keyboard's Power key shuts a machine down without the power question. kvmd would be
        // told the name as it came, and has it: a keys action naming it is invalid on this PC's
        // wall as on the server (422), and nothing goes to the KVM.
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        for (const QString& keys : {u"Power"_s, u"ControlLeft,AltLeft,Power"_s, u"Power,KeyA"_s}) {
            const auto refused = doneBy(own.wall, machine, core::Action::press(keys));
            QCOMPARE(refusalOf(refused),
                     u"invalid: The keyboard's Power key is not sent. A machine's power is under Power."_s);
            QVERIFY(!own.wall.mayHaveBeenDone(machine, refused.error()));
        }
        QCOMPARE(kvm.asked("/api/hid/events/send_shortcut"), 0);
        QVERIFY(kvm.did.isEmpty());
        // And the KVM is free for the next thing.
        QVERIFY(doneBy(own.wall, machine, core::Action::press(u"F1"_s)).ok());
        QCOMPARE(kvm.did, QList<QByteArray>{"keys F1"_ba});
    }

    void anActionWhoseTextIsNotKeptSaysSoAndTypesNothing()
    {
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        const SavedAction saved = own.actions.add(typing(u"Sign in"_s, u"the text it had"_s, true)).value();
        QCOMPARE(doneBy(own.wall, machine, core::Action::plain(u"jiggler_on"_s)).ok(), true);
        // Windows no longer has it (or does not hand it out: the store does not tell the two apart).
        own.entries.remove(OwnActions::secretOf(saved.id, u'a', 1));
        const auto refused = doneBy(own.wall, machine, core::Action::run(saved));
        QCOMPARE(refusalOf(refused),
                 u"no_text: Windows does not hand out the text that was saved for \"Sign in\", or none is kept on this "
                 "PC. Nothing was typed. Edit the action and type its text again."_s);
        QVERIFY(!own.wall.mayHaveBeenDone(machine, refused.error()));
        // Nothing was typed: not Enter by itself either, which would send off whatever stands at
        // that machine's prompt.
        QCOMPARE(kvm.asked("/api/hid/print"), 0);
        QCOMPARE(kvm.did, QList<QByteArray>{"jiggler 1"_ba});
        // The action is on the list as it was, and the KVM is free for the next thing.
        QCOMPARE(inLines(savedOf(own.wall)), QStringList{inOneLine(saved)});
        QVERIFY(doneBy(own.wall, machine, core::Action::press(u"F1"_s)).ok());
        // A change with no text typed keeps "the text that is saved", and is taken, as on the
        // server: the list names one. The run still says that there is none.
        const SavedAction kept =
            own.actions.change(saved.id, saved.changed, typing(u"Sign in"_s, QString(), true)).value();
        QCOMPARE(doneBy(own.wall, machine, core::Action::run(kept)).error().code, std::string("no_text"));
        // Typed anew, it runs.
        const SavedAction anew =
            own.actions.change(saved.id, kept.changed, typing(u"Sign in"_s, u"typed anew"_s, true)).value();
        QVERIFY(doneBy(own.wall, machine, core::Action::run(anew)).ok());
        QVERIFY(kvm.did.constLast() == typedOnce(u"typed anew"_s, true).constFirst());
    }

    void whatTheStoreHoldsInAnotherFormOrForAnotherActionIsNoText_data()
    {
        QTest::addColumn<Tampering>("tampering");
        const auto row = [](const char* name, const Tampering& tampering) { QTest::newRow(name) << tampering; };
        row("nothing at all", [](const Scene& scene) { scene.empty(); });
        row("the text alone", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), "the text by itself");
        });
        row("another first line", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1),
                                 "QKVM-ACTION-2\n" + scene.id.toLatin1() + '\n' + scene.mark + " 1/1\ntext");
        });
        row("no first line", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), scene.id.toLatin1() + '\n' + scene.mark + " 1/1\ntext");
        });
        row("a KVM's password", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), core::testing::savedFor("secret", u"https://192.168.1.20"_s));
        });
        row("saved for another action", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), entryOf(scene.otherId, scene.mark, 1, 1, "text"));
        });
        row("another action's own entry", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), scene.entries.value(OwnActions::secretOf(scene.otherId, u'a', 1)));
        });
        row("of another writing", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.otherMark, 1, 1, "text"));
        });
        row("of a writing whose mark only begins the same", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark + "0", 1, 1, "text"));
        });
        row("in the other place", [](const Scene& scene) {
            const QByteArray first = scene.entries.value(scene.a(1));
            const QByteArray second = scene.entries.value(scene.a(2));
            scene.empty();
            scene.entries.insert(scene.b(1), first);
            scene.entries.insert(scene.b(2), second);
        });
        row("the second part is missing", [](const Scene& scene) { scene.entries.remove(scene.a(2)); });
        row("the first part is missing", [](const Scene& scene) { scene.entries.remove(scene.a(1)); });
        row("the second part is of another writing", [](const Scene& scene) {
            scene.entries.insert(scene.a(2), entryOf(scene.id, scene.otherMark, 2, 2, scene.second()));
        });
        row("the second part is for another action", [](const Scene& scene) {
            scene.entries.insert(scene.a(2), entryOf(scene.otherId, scene.mark, 2, 2, scene.second()));
        });
        // (From here on in signs of four bytes each, of which a part holds a whole number: what
        // these entries would make, joined, is a text; so each is no text for what its row says,
        // and not because its bytes are none.)
        row("the second part is the first over again", [](const Scene& scene) {
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 1, 2, Scene::faces(625)));
            scene.entries.insert(scene.a(2), entryOf(scene.id, scene.mark, 1, 2, Scene::faces(625)));
        });
        row("the second part says it is one of three", [](const Scene& scene) {
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 1, 2, Scene::faces(625)));
            scene.entries.insert(scene.a(2), entryOf(scene.id, scene.mark, 2, 3, Scene::faces(625)));
        });
        row("the first of two parts is not full", [](const Scene& scene) {
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 1, 2, Scene::faces(624)));
            scene.entries.insert(scene.a(2), entryOf(scene.id, scene.mark, 2, 2, Scene::faces(10)));
        });
        row("a last part of nothing", [](const Scene& scene) {
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 1, 2, Scene::faces(625)));
            scene.entries.insert(scene.a(2), entryOf(scene.id, scene.mark, 2, 2, QByteArray()));
        });
        row("a part of more than a part holds", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 1, 1, Scene::faces(626)));
        });
        row("more parts than a text has", [](const Scene& scene) {
            for (int part = 1; part <= 5; ++part) {
                scene.entries.insert(scene.a(part), entryOf(scene.id, scene.mark, part, 5, Scene::faces(625)));
            }
        });
        row("no parts", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 1, 0, Scene::faces(625)));
        });
        row("a part before the first", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 0, 1, Scene::faces(5)));
        });
        row("bytes that are no text", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 1, 1, QByteArray("\xff\xfe text", 7)));
        });
        row("half a sign at its end", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 1, 1, QByteArray("text\xe2\x82", 6)));
        });
        row("more signs than an action types", [](const Scene& scene) {
            scene.empty();
            scene.entries.insert(scene.a(1), entryOf(scene.id, scene.mark, 1, 1, QByteArray(2001, 'x')));
        });
    }

    void whatTheStoreHoldsInAnotherFormOrForAnotherActionIsNoText()
    {
        QFETCH(Tampering, tampering);
        Own own;
        MadeUpKvm kvm;
        const QString machine = own.add(kvm);
        // An action whose text is kept in two parts, and another beside it.
        const QString text = threeBytesEach(1000);
        const qsizetype entriesBefore = own.entries.size();
        const SavedAction saved = own.actions.add(typing(u"Sign in"_s, text, true)).value();
        const SavedAction other = own.actions.add(typing(u"Other"_s, u"the other's text"_s)).value();
        QCOMPARE(own.entries.size(), entriesBefore + 3);
        // As it was saved it is its text: so what follows is the tampering's doing.
        QVERIFY(doneBy(own.wall, machine, core::Action::run(saved)).ok());
        QVERIFY(kvm.did == typedOnce(text, true));
        const Scene scene{
            own.entries,  saved.id, markOf(own.actions.all().at(0)), other.id, markOf(own.actions.all().at(1)),
            text.toUtf8()};
        tampering(scene);
        // It is no text of this action's: the run says so and types nothing, Enter neither.
        const auto refused = doneBy(own.wall, machine, core::Action::run(saved));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("no_text"));
        QCOMPARE(kvm.did.size(), 1);
        QCOMPARE(kvm.asked("/api/hid/print"), 1);
        // Nor is it handed out for a sealed file: this action is one of those without a text.
        const OwnActions::WithTheirTexts handed = own.actions.textsHandedOutForASealedFile();
        QCOMPARE(handed.withoutText, QList<qsizetype>{0});
        QVERIFY(handed.actions.at(0).text.isEmpty());
        // The other action types its own text as ever.
        QVERIFY(doneBy(own.wall, machine, core::Action::run(other)).ok());
        QVERIFY(kvm.did.constLast() == typedOnce(u"the other's text"_s, false).constFirst());
    }

    void theTextsAreHandedOutForASealedFileByTheDoorThatIsNamedForIt()
    {
        Kept kept;
        OwnActions actions(kept.settings, kept.secrets);
        const QString longOne(2000, QChar(0x20AC));
        QVERIFY(actions.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s, true)).ok());
        QVERIFY(actions.add(typing(u"Sign in"_s, bom() + u"a text\n"_s, true)).ok());
        QVERIFY(actions.add(typing(u"Long"_s, longOne, false, true)).ok());
        const SavedAction lost = actions.add(typing(u"Lost"_s, u"gone"_s)).value();
        kept.entries.remove(OwnActions::secretOf(lost.id, u'a', 1));
        const OwnActions::WithTheirTexts handed = actions.textsHandedOutForASealedFile();
        // Each as a file holds one, in the list's order; the two that type, with their texts as
        // they were typed in; and the one whose text is not kept, without, and named.
        QCOMPARE(handed.actions.size(), 4);
        core::exchange::Action lock;
        lock.name = u"Lock"_s;
        lock.keys = u"MetaLeft,KeyL"_s;
        lock.danger = true;
        QVERIFY(handed.actions.at(0) == lock);
        QVERIFY(handed.actions.at(1).types && handed.actions.at(1).enter && !handed.actions.at(1).danger);
        QVERIFY(handed.actions.at(1).text == bom() + u"a text\n"_s);
        QCOMPARE(handed.actions.at(1).text.size(), 8);
        QVERIFY(handed.actions.at(2).text == longOne);
        QVERIFY(handed.actions.at(2).danger && !handed.actions.at(2).enter);
        QVERIFY(handed.actions.at(3).types && handed.actions.at(3).text.isEmpty());
        QCOMPARE(handed.withoutText, QList<qsizetype>{3});
    }

    // --- read back -------------------------------------------------------------------------------

    void aFileThatSaysAnActionTypesAndHasNoSecretForItHasTheActionWhichSaysSoWhenItIsRun()
    {
        // The settings as they came to this PC from another, or from a copy: the store here holds
        // nothing for them.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QJsonArray actions{
            QJsonObject{{u"id"_s, u"0a1b2c3d"_s},
                        {u"name"_s, u"Sign in"_s},
                        {u"kind"_s, u"text"_s},
                        {u"keys"_s, u""_s},
                        {u"enter"_s, true},
                        {u"danger"_s, true},
                        {u"changed"_s, 3},
                        {u"writing"_s, u"b:0123456789abcdef"_s}},
            // (And one that names no writing of its text that could be one.)
            QJsonObject{{u"id"_s, u"0a1b2c3e"_s},
                        {u"name"_s, u"Sign out"_s},
                        {u"kind"_s, u"text"_s},
                        {u"keys"_s, u""_s},
                        {u"enter"_s, false},
                        {u"danger"_s, false},
                        {u"changed"_s, 0},
                        {u"writing"_s, u"the text itself, as somebody wrote it in"_s}},
        };
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(QJsonDocument(QJsonObject{{u"actions"_s, actions}}).toJson());
        written.close();
        core::Settings settings(file);
        AsWindowsKeeps secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        OwnKvm own;
        own.address = kvm.stand->address();
        own.name = u"Build server"_s;
        const QString machine = wall.add(own, u"secret"_s).value();
        // Both are on the list, as the file has them.
        const QList<SavedAction> handed = savedOf(wall);
        QCOMPARE(inLines(handed),
                 (QStringList{u"0a1b2c3d Sign in [types] keys= enter=true danger=true changed=3"_s,
                              u"0a1b2c3e Sign out [types] keys= enter=false danger=false changed=0"_s}));
        QCOMPARE(wall.actions().all().at(1).writing, QString());
        // Run, each says that its text is not kept, and nothing is typed.
        for (const SavedAction& one : handed) {
            const auto refused = doneBy(wall, machine, core::Action::run(one));
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().code, std::string("no_text"));
            QVERIFY(QString::fromStdString(refused.error().message).contains(u"Nothing was typed."_s));
        }
        QVERIFY(kvm.did.isEmpty());
        // The first keeps "its text" where none is typed, as far as the list knows; the second
        // has none that could stay, and wants it typed. Typed, each types.
        QVERIFY(wall.actions().change(u"0a1b2c3d"_s, 3, typing(u"Sign in"_s, QString(), true, true)).ok());
        QCOMPARE(refusalOf(wall.actions().change(u"0a1b2c3e"_s, 0, typing(u"Sign out"_s, QString()))),
                 u"invalid: Enter the text to type"_s);
        const SavedAction anew = wall.actions().change(u"0a1b2c3e"_s, 0, typing(u"Sign out"_s, u"exit"_s)).value();
        QVERIFY(doneBy(wall, machine, core::Action::run(anew)).ok());
        QVERIFY(kvm.did == typedOnce(u"exit"_s, false));
    }

    void theListAndItsTextsAreThereAtTheNextStart()
    {
        Kept kept;
        MadeUpKvm kvm;
        SavedAction lock;
        SavedAction signIn;
        {
            LocalWall wall(kept.settings, kept.secrets, quick());
            lock = wall.actions().add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s, true)).value();
            signIn = wall.actions().add(typing(u"Sign in"_s, u"the first text"_s)).value();
            signIn = wall.actions().change(signIn.id, 0, typing(u"Sign in"_s, u"the text it has"_s, true)).value();
        }
        // The next start of the program: other settings, read from the file, and another wall.
        // The store is the one thing that stays.
        core::Settings settings(kept.file);
        LocalWall wall(settings, kept.secrets, quick());
        OwnKvm own;
        own.address = kvm.stand->address();
        const QString machine = wall.add(own, u"secret"_s).value();
        const QList<SavedAction> handed = savedOf(wall);
        QCOMPARE(inLines(handed), (QStringList{inOneLine(lock), inOneLine(signIn)}));
        QVERIFY(doneBy(wall, machine, core::Action::run(handed.at(1))).ok());
        QVERIFY(doneBy(wall, machine, core::Action::run(handed.at(0))).ok());
        QVERIFY(kvm.did == typedOnce(u"the text it has"_s, true) + QList<QByteArray>{"keys MetaLeft,KeyL"_ba});
    }

    // --- whoever shows the wall ------------------------------------------------------------------

    void aChangeOfTheListArrivesAtWhoeverShowsTheWallByTheWayAChangeOfTheWallDoes()
    {
        Own own;
        MadeUpKvm kvm;
        own.add(kvm);
        // Something that shows the wall: its feed asks for the wall anew when the wall says that it
        // changed, as a window of the program has it (ServerSession::useOwnWall); and the saved
        // actions are asked for when the feed says that the wall is another. (A window asks for
        // them when a machine's menu is opened, MachineActions::askForSaved, and not yet on the
        // feed's word: that is what ui/ is to add, for a menu that is open over a change.)
        struct Shows {
            explicit Shows(LocalWall& wall) : feed(wall, 100)
            {
                QObject::connect(&wall, &LocalWall::changed, &feed, &core::WallFeed::askAnew);
                QObject::connect(&feed, &core::WallFeed::wallChanged, &feed, [this, &wall] {
                    wall.askSavedActions([this, still = QPointer<core::WallFeed>(&feed)](
                                             const base::Result<QList<SavedAction>>& answer) {
                        if (still != nullptr && answer.ok()) {
                            saved = inLines(answer.value());
                            ++told;
                        }
                    });
                });
                feed.start();
            }
            core::WallFeed feed;
            QStringList saved;
            int told = 0;
        };
        Shows one(own.wall);
        Shows other(own.wall);
        QTRY_VERIFY(one.told >= 1 && other.told >= 1);
        QVERIFY(one.saved.isEmpty() && other.saved.isEmpty());
        // One is added: both have it, without asking by themselves.
        const SavedAction lock = own.actions.add(pressing(u"Lock"_s, u"MetaLeft,KeyL"_s)).value();
        QTRY_COMPARE(one.saved, QStringList{inOneLine(lock)});
        QTRY_COMPARE(other.saved, QStringList{inOneLine(lock)});
        // Several within one turn of the loop, of every kind: both come to the list as it is.
        const SavedAction signIn = own.actions.add(typing(u"Sign in"_s, u"a text"_s, true)).value();
        QVERIFY(own.actions.change(lock.id, lock.changed, pressing(u"Lock it"_s, u"MetaLeft,KeyL"_s, true)).ok());
        QVERIFY(own.actions.add(pressing(u"Help"_s, u"F1"_s)).ok());
        QVERIFY(own.actions.remove(signIn.id, signIn.changed).ok());
        const QStringList asItIs = inLines(own.actions.handedOut());
        QCOMPARE(asItIs.size(), 2);
        QTRY_COMPARE(one.saved, asItIs);
        QTRY_COMPARE(other.saved, asItIs);
        // And so does the number an action stands under after a change that could not be
        // written down.
        {
            QFile held(own.file);
            QVERIFY(held.open(QIODevice::ReadOnly));
            QVERIFY(!own.actions.change(lock.id, 1, pressing(u"Lock"_s, u"F2"_s)).ok());
        }
        const QStringList after = inLines(own.actions.handedOut());
        QVERIFY(after != asItIs);
        QTRY_COMPARE(one.saved, after);
        QTRY_COMPARE(other.saved, after);
    }

    // --- Windows' own store ----------------------------------------------------------------------

    void aLongTextIsKeptInWindowsOwnStoreAndTakenOutOfItAgain()
    {
        // Windows Credential Manager itself, of whoever runs the test: under a name of the test's
        // own, which no entry of the program has, and with everything taken out again at the end
        // (cleanup, above, whatever comes of the test). The text is one a test made up.
        const std::unique_ptr<core::SecretStore> windows = core::windowsSecrets(m_under);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        OwnActions actions(settings, *windows);
        const QString text = threeBytesEach(2000);
        const auto added = actions.add(typing(u"A test's own"_s, text, true));
        if (added.ok()) {
            for (const QChar place : {u'a', u'b'}) {
                for (int part = 1; part <= OwnActions::mostParts; ++part) {
                    m_used.append(OwnActions::secretOf(added.value().id, place, part));
                }
            }
        }
        QVERIFY2(added.ok(), qPrintable(refusalOf(added)));
        const QString id = added.value().id;
        // Three entries, and no fourth; and it is read back from them whole.
        for (int part = 1; part <= 3; ++part) {
            QVERIFY(!windows->kept(OwnActions::secretOf(id, u'a', part)).isEmpty());
        }
        QVERIFY(windows->kept(OwnActions::secretOf(id, u'a', 4)).isEmpty());
        {
            const OwnActions::WithTheirTexts handed = actions.textsHandedOutForASealedFile();
            QVERIFY(handed.withoutText.isEmpty());
            QVERIFY(handed.actions.constFirst().text == text);
            QVERIFY(handed.actions.constFirst().enter);
        }
        // Changed to a short one: that is in the other place, and the first is empty again.
        QVERIFY(actions.change(id, 0, typing(u"A test's own"_s, u"short"_s)).ok());
        QVERIFY(!windows->kept(OwnActions::secretOf(id, u'b', 1)).isEmpty());
        for (int part = 1; part <= OwnActions::mostParts; ++part) {
            QVERIFY(windows->kept(OwnActions::secretOf(id, u'a', part)).isEmpty());
        }
        {
            const OwnActions::WithTheirTexts handed = actions.textsHandedOutForASealedFile();
            QVERIFY(handed.withoutText.isEmpty());
            QVERIFY(handed.actions.constFirst().text == u"short"_s);
        }
        // Removed: nothing of it is in Windows' store.
        QVERIFY(actions.remove(id, 1).ok());
        for (const QString& used : std::as_const(m_used)) {
            QVERIFY(windows->kept(used).isEmpty());
        }
    }
};

QTEST_GUILESS_MAIN(TestOwnActions)
#include "test_own_actions.moc"
