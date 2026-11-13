#include <QTest>

#include <qkvm/core/typed_keys.h>

// Keys and a text as a USB keyboard's reports: the web's tests/test_jetkvm.py (test_keys_and_text_as_keyboard_reports)
// and what its tests/test_redfish.py holds a server's console to (a text that cannot be typed is not quoted back;
// a combination that is no keys is refused, in the console's words), and the desktop's own where a rule of the
// web's had no test that names it.
using namespace Qt::StringLiterals;
using qkvm::core::typed::Report;
namespace typed = qkvm::core::typed;

namespace {

const QString console = u"This server's console"_s;

std::vector<Report> typedAs(const QString& text)
{
    auto reports = typed::text(text);
    return reports ? std::move(reports).value() : std::vector<Report>{};
}

QString refusalOf(const qkvm::base::Result<std::vector<Report>>& reports)
{
    return reports ? QString() : QString::fromStdString(reports.error().code + ": " + reports.error().message);
}

QString refusalOf(const qkvm::base::Result<Report>& report)
{
    return report ? QString() : QString::fromStdString(report.error().code + ": " + report.error().message);
}

} // namespace

class TestTypedKeys : public QObject {
    Q_OBJECT

private slots:
    void keysPressedTogetherAreOneReport()
    {
        const auto ctrlAltDel = typed::combination(u"ControlLeft,AltLeft,Delete"_s, console);
        QVERIFY(ctrlAltDel.ok());
        QCOMPARE(ctrlAltDel.value(), (Report{0x05, {0x4C}}));
        const auto shifted = typed::combination(u"ShiftRight,F12,KeyA"_s, console);
        QVERIFY(shifted.ok());
        QCOMPARE(shifted.value(), (Report{0x20, {0x45, 0x04}}));
        QCOMPARE(refusalOf(typed::combination(u"Hyper"_s, console)),
                 u"invalid: This server's console has no Hyper key"_s);
        // (The desktop's own.) Every modifier is a bit and no key; the keys keep the order they were named in.
        const auto all = typed::combination(
            u"ControlLeft,ShiftLeft,AltLeft,MetaLeft,ControlRight,ShiftRight,AltRight,MetaRight"_s, console);
        QVERIFY(all.ok());
        QCOMPARE(all.value(), (Report{0xFF, {}}));
        QCOMPARE(typed::combination(u"Numpad0,Escape,ContextMenu"_s, console).value(), (Report{0, {0x62, 0x29, 0x65}}));
    }

    void whatIsNoCombinationIsRefused()
    {
        // As a server's console refuses it (test_redfish.py's test_an_invalid_key_combination_is_refused), and as the
        // rule for an action's keys has it: up to eight names, each a capital and one to fifteen letters and digits.
        QCOMPARE(refusalOf(typed::combination(u"NoSuchKey"_s, console)),
                 u"invalid: This server's console has no NoSuchKey key"_s);
        for (const QString& notKeys :
             {u""_s, u"keyA"_s, u"KeyA,"_s, u",KeyA"_s, u"Key A"_s, u"KeyA, KeyB"_s, u"K"_s, u"KeyAAAAAAAAAAAAAA"_s,
              u"KeyA,KeyB,KeyC,KeyD,KeyE,KeyF,KeyG,KeyH,KeyI"_s, u"KeyA\n"_s}) {
            QCOMPARE(refusalOf(typed::combination(notKeys, console)), u"invalid: Not a valid key combination"_s);
        }
        // Six keys and the modifiers are what a keyboard holds at once; a seventh key is refused, not dropped.
        QVERIFY(typed::combination(u"ControlLeft,ShiftLeft,KeyA,KeyB,KeyC,KeyD,KeyE,KeyF"_s, console).ok());
        QCOMPARE(refusalOf(typed::combination(u"KeyA,KeyB,KeyC,KeyD,KeyE,KeyF,KeyG"_s, console)),
                 u"invalid: A keyboard can hold six keys at once, plus Ctrl, Alt, Shift and Win"_s);
        // Names are the web's table's, and no more: keys it has no name for are refused, as the web refuses them.
        for (const QString& unknown : {u"IntlBackslash"_s, u"Power"_s, u"F13"_s, u"Digit10"_s, u"KeyAa"_s}) {
            QCOMPARE(refusalOf(typed::combination(unknown, console)),
                     u"invalid: This server's console has no %1 key"_s.arg(unknown));
        }
    }

    void aTextIsTypedKeyByKey()
    {
        // The web's: a letter, a blank, a capital and a question mark (Shift held), "\r\n" as one Enter.
        QCOMPARE(typedAs(u"a Z?\r\n"_s),
                 (std::vector<Report>{{0, {0x04}}, {0, {0x2C}}, {2, {0x1D}}, {2, {0x38}}, {0, {0x28}}}));
        // (The desktop's own.) Every character of a US keyboard, against the web's table.
        const QString plain = u"`-=[]\\;',./"_s;
        const QString shifted = u"~_+{}|:\"<>?"_s;
        const std::vector<std::uint8_t> keys{0x35, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x36, 0x37, 0x38};
        for (qsizetype at = 0; at < plain.size(); ++at) {
            QCOMPARE(typedAs(plain.mid(at, 1)), (std::vector<Report>{{0, {keys[static_cast<std::size_t>(at)]}}}));
            QCOMPARE(typedAs(shifted.mid(at, 1)), (std::vector<Report>{{2, {keys[static_cast<std::size_t>(at)]}}}));
        }
        QCOMPARE(typedAs(u"1!0)9(\t\n"_s), (std::vector<Report>{{0, {0x1E}},
                                                                {2, {0x1E}},
                                                                {0, {0x27}},
                                                                {2, {0x27}},
                                                                {0, {0x26}},
                                                                {2, {0x26}},
                                                                {0, {0x2B}},
                                                                {0, {0x28}}}));
        QCOMPARE(typedAs(u"az"_s), (std::vector<Report>{{0, {0x04}}, {0, {0x1D}}}));
        QCOMPARE(typedAs(u"AZ"_s), (std::vector<Report>{{2, {0x04}}, {2, {0x1D}}}));
        QCOMPARE(typedAs(u"@#$%^&*"_s),
                 (std::vector<Report>{
                     {2, {0x1F}}, {2, {0x20}}, {2, {0x21}}, {2, {0x22}}, {2, {0x23}}, {2, {0x24}}, {2, {0x25}}}));
        // Those are all: no other character up to U+2FFF has a key.
        int typeable = 0;
        for (char32_t one = 1; one < 0x3000; ++one) {
            typeable += typed::text(QString::fromUcs4(&one, 1)).ok() ? 1 : 0;
        }
        QCOMPARE(typeable, 97);
        // A lone "\r" is no key of the table.
        QCOMPARE(refusalOf(typed::text(u"a\rb"_s)),
                 u"invalid: Character 2 of the text cannot be typed on a US keyboard layout"_s);
    }

    void aTextThatCannotBeTypedIsNotQuotedBack()
    {
        // test_redfish.py's test_a_text_that_cannot_be_typed_is_not_quoted_back: where it went wrong is said, and not
        // what is there (the text may be a saved secret).
        const QChar odd(0xE9);
        const QString refused = refusalOf(typed::text(u"secret"_s + odd + u"x"_s));
        QCOMPARE(refused, u"invalid: Character 7 of the text cannot be typed on a US keyboard layout"_s);
        QVERIFY(!refused.contains(odd));
        // (The desktop's own.) Counted by characters: one from far up is one, and each "\r\n" before it is one.
        const QString farUp = QString::fromUcs4(std::u32string(1, char32_t{0x1F600}).c_str(), 1);
        QCOMPARE(refusalOf(typed::text(u"a\r\nb"_s + farUp)),
                 u"invalid: Character 4 of the text cannot be typed on a US keyboard layout"_s);
        QCOMPARE(refusalOf(typed::text(farUp + farUp)),
                 u"invalid: Character 1 of the text cannot be typed on a US keyboard layout"_s);
    }

    void aTextIsOneToTwoThousandCharacters()
    {
        // (The desktop's own: the web's MAX_TEXT, counted as Python counts, before "\r\n" is made one.)
        QCOMPARE(refusalOf(typed::text({})), u"invalid: Text must be 1 to 2000 characters"_s);
        QCOMPARE(typedAs(QString(2000, u'a')).size(), std::size_t{2000});
        QCOMPARE(refusalOf(typed::text(QString(2001, u'a'))), u"invalid: Text must be 1 to 2000 characters"_s);
        QString lines(1000, u'a');
        for (int line = 0; line < 500; ++line) {
            lines += u"\r\n"_s;
        }
        QCOMPARE(typedAs(lines).size(), std::size_t{1500});
        QCOMPARE(refusalOf(typed::text(u"a"_s + lines)), u"invalid: Text must be 1 to 2000 characters"_s);
        // A character from far up takes two places in a QString, and is one character.
        const QString farUp = QString::fromUcs4(std::u32string(1, char32_t{0x1F600}).c_str(), 1);
        QString many;
        for (int at = 0; at < 2001; ++at) {
            many += farUp;
        }
        QCOMPARE(refusalOf(typed::text(many)), u"invalid: Text must be 1 to 2000 characters"_s);
        QCOMPARE(refusalOf(typed::text(many.left(4000))),
                 u"invalid: Character 1 of the text cannot be typed on a US keyboard layout"_s);
    }

    void keysPressedOneByOneAreTheWholeKeyboard()
    {
        // The web's live.RedfishHid (its _say_key): a live line says keys one by one, by their USB numbers (as
        // input/keys.h gives them), and a server's console is told the whole keyboard each time.
        typed::Keyboard keyboard;
        QCOMPARE(keyboard.now(), Report{});
        QVERIFY(keyboard.take(0xE0, true)); // left Ctrl: a bit
        QVERIFY(keyboard.take(0x04, true));
        QVERIFY(keyboard.take(0xE5, true)); // right Shift
        QVERIFY(keyboard.take(0x05, true));
        QCOMPARE(keyboard.now(), (Report{0x21, {0x04, 0x05}}));
        QVERIFY(keyboard.take(0x04, false));
        QVERIFY(keyboard.take(0xE0, false));
        QCOMPARE(keyboard.now(), (Report{0x20, {0x05}}));
        // A key said down again, or up when it is not down, changes nothing (and is not said again).
        QVERIFY(!keyboard.take(0x05, true));
        QVERIFY(!keyboard.take(0x06, false));
        QVERIFY(!keyboard.take(0xE1, false));
        // A seventh key down lets go of the one that went down first, as a keyboard's own report does.
        for (const int usage : {0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B}) {
            QVERIFY(keyboard.take(usage, true));
        }
        QCOMPARE(keyboard.now(), (Report{0x20, {0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B}}));
        // What is no key of page 7's (0, and numbers beyond a byte) is not taken.
        QVERIFY(!keyboard.take(0, true));
        QVERIFY(!keyboard.take(0x104, true));
        QVERIFY(!keyboard.take(-4, true));
        QCOMPARE(keyboard.now(), (Report{0x20, {0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B}}));
        keyboard.clear();
        QCOMPARE(keyboard.now(), Report{});
    }

    void aMouseButtonIsItsBit()
    {
        // Left, right and middle, as a USB mouse has them; back and forward have no bit there (the web's BITS).
        QCOMPARE(typed::buttonBit(1), std::uint8_t{1});
        QCOMPARE(typed::buttonBit(2), std::uint8_t{2});
        QCOMPARE(typed::buttonBit(3), std::uint8_t{4});
        for (const int none : {0, 4, 5, 6, -1}) {
            QCOMPARE(typed::buttonBit(none), std::uint8_t{0});
        }
    }
};

QTEST_GUILESS_MAIN(TestTypedKeys)
#include "test_typed_keys.moc"
