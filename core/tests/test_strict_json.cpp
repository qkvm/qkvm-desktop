#include <cstddef>
#include <optional>
#include <string>

#include <QTest>

#include <qkvm/core/strict_json.h>

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::json::Value;

namespace {

// Why a text was refused, or "read" where it was not.
QByteArray saidOf(const QByteArray& text)
{
    const base::Result<Value> read = core::json::parse(text);
    return read ? QByteArray("read") : QByteArray(read.error().code.c_str());
}

// The one thing in a list of one, which is how a test hands a value over.
Value onlyOf(const QByteArray& text)
{
    const base::Result<Value> read = core::json::parse("[" + text + "]");
    return read && read.value().isList() && read.value().list.size() == 1 ? read.value().list.front() : Value();
}

// A text of JSON with this between its quotes, in a list of one.
QByteArray listOfText(const QByteArray& between)
{
    return "[\"" + between + "\"]";
}

// A sign spelled out by its number, as JSON spells one: a backslash, the letter, and its number
// in four digits to the base of sixteen. Put together here, and written nowhere in this file as
// it stands in a text of JSON: a tool that edits sources has made the sign itself of such a
// spelling before, and nobody reading the file could have known.
QByteArray spelled(uint unit, char letter = 'u')
{
    return QByteArray(1, '\\') + letter + QByteArray::number(unit, 16).rightJustified(4, '0');
}

QByteArray nested(int deep)
{
    return QByteArray(deep, '[') + QByteArray(deep, ']');
}

// The halves of a sign that takes two places (the grinning face, U+1F600).
constexpr uint firstHalf = 0xd83d;
constexpr uint secondHalf = 0xde00;

} // namespace

// The reader of JSON that a file anybody made is read with: what it takes is what a QKVM
// server's reader takes, and what that one refuses it refuses, sign for sign.
class TestStrictJson : public QObject {
    Q_OBJECT

private slots:
    void whatAFileIsMadeOfIsRead()
    {
        const base::Result<Value> read = core::json::parse(
            R"( {"format": "qkvm-export", "version": 1, "made": 1791832242.5, "list": [true, false, null, -0.25e1],)"
            "\r\n\t"
            R"("nested": {"empty": {}, "none": []}} )");
        QVERIFY(read.ok());
        const Value& said = read.value();
        QVERIFY(said.isObject());
        QCOMPARE(said.members.size(), std::size_t{5});
        QCOMPARE(said.members.front().name, u"format"_s);
        QVERIFY(said.find(u"format") != nullptr && said.find(u"format")->isText());
        QCOMPARE(said.find(u"format")->text, u"qkvm-export"_s);
        QVERIFY(said.find(u"version")->isNumber() && said.find(u"version")->whole);
        QCOMPARE(said.find(u"version")->number, 1.0);
        QVERIFY(said.find(u"made")->isNumber() && !said.find(u"made")->whole);
        QCOMPARE(said.find(u"made")->number, 1791832242.5);
        const Value* list = said.find(u"list");
        QVERIFY(list != nullptr && list->isList());
        QCOMPARE(list->list.size(), std::size_t{4});
        QVERIFY(list->list.at(0).isFlag() && list->list.at(0).flag);
        QVERIFY(list->list.at(1).isFlag() && !list->list.at(1).flag);
        QCOMPARE(list->list.at(2).kind, Value::Kind::Null);
        QCOMPARE(list->list.at(3).number, -2.5);
        QVERIFY(said.find(u"nested")->find(u"empty")->isObject());
        QVERIFY(said.find(u"nested")->find(u"none")->isList());
        QVERIFY(said.find(u"not there") == nullptr);
        // A value of any kind is a text of JSON by itself, as it is to the server's reader.
        QCOMPARE(saidOf("7"), QByteArray("read"));
        QCOMPARE(saidOf(R"("qkvm-export")"), QByteArray("read"));
        QCOMPARE(saidOf(" null "), QByteArray("read"));
    }

    void aNameThatStandsTwiceMeansWhatWasWrittenLast()
    {
        const base::Result<Value> read = core::json::parse(R"({"kind": "kvmd", "name": "one", "kind": "ipmi"})");
        QVERIFY(read.ok());
        QCOMPARE(read.value().members.size(), std::size_t{3});
        QCOMPARE(read.value().find(u"kind")->text, u"ipmi"_s);
    }

    void aTextIsReadSignForSign()
    {
        // What is spelled out with a backslash: each of the eight, and a sign by its number.
        // (Named first: Qt's reader of this file takes the text for the end of the line below.)
        const QByteArray withEach = "\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"";
        const QString eachRead = u"a\"b\\c/d\be\ff\ng\rh\ti"_s;
        QCOMPARE(onlyOf(withEach).text, eachRead);
        QCOMPARE(onlyOf("\"a" + spelled(0xfc) + "b" + spelled(0x20ac) + "c\"").text,
                 u"a"_s + QChar(0xfc) + u'b' + QChar(0x20ac) + u'c');
        // A sign that takes two places: as its bytes, and as its two halves spelled out, in
        // digits small or large.
        const char32_t wide = 0x1F600;
        const QString grin = QString::fromUcs4(&wide, 1);
        QCOMPARE(onlyOf("\"" + grin.toUtf8() + "\"").text, grin);
        QCOMPARE(onlyOf("\"" + spelled(firstHalf) + spelled(secondHalf) + "\"").text, grin);
        QCOMPARE(onlyOf("\"" + spelled(firstHalf).toUpper().replace('U', 'u') +
                        spelled(secondHalf).toUpper().replace('U', 'u') + "\"")
                     .text,
                 grin);
        // Signs of two and of three bytes, and a nought spelled out, which is a sign like any other.
        QCOMPARE(onlyOf("\"" + QString(QChar(0xfc)).toUtf8() + QString(QChar(0x20ac)).toUtf8() + "\"").text,
                 QString(QChar(0xfc)) + QChar(0x20ac));
        QCOMPARE(onlyOf("\"a" + spelled(0) + "b\"").text, u"a"_s + QChar(0) + u"b"_s);
        QCOMPARE(onlyOf(R"("")").text, QString());
        // The last sign there is, and the last before the surrogates and the first behind them.
        QCOMPARE(onlyOf(QByteArray("\"\xF4\x8F\xBF\xBF\xED\x9F\xBF\xEE\x80\x80\"")).text.size(), qsizetype{4});
    }

    void whatIsNoJsonByTheBookIsRefused_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::newRow("nothing") << QByteArray();
        QTest::newRow("blanks only") << QByteArray(" \n\t ");
        QTest::newRow("words") << QByteArray("not json at all");
        QTest::newRow("an object that does not end") << QByteArray(R"({"a": 1)");
        QTest::newRow("a list that does not end") << QByteArray("[1, 2");
        QTest::newRow("a text that does not end") << QByteArray("[\"abc");
        QTest::newRow("something behind the value") << QByteArray(R"({"a": 1} x)");
        QTest::newRow("two values") << QByteArray("{} {}");
        QTest::newRow("a comma behind the last of a list") << QByteArray("[1, 2,]");
        QTest::newRow("a comma behind the last of an object") << QByteArray(R"({"a": 1,})");
        QTest::newRow("no comma between two") << QByteArray("[1 2]");
        QTest::newRow("a name without quotes") << QByteArray("{a: 1}");
        QTest::newRow("a name in single quotes") << QByteArray("{'a': 1}");
        QTest::newRow("a name that is a number") << QByteArray("{1: 1}");
        QTest::newRow("no colon") << QByteArray(R"({"a" 1})");
        QTest::newRow("a comment") << QByteArray("[1] // one");
        QTest::newRow("a yes in capitals") << QByteArray("[True]");
        QTest::newRow("half a word") << QByteArray("[tru]");
        QTest::newRow("a word run on") << QByteArray("[nullx]");
        // The mark some programs put at a file's head: the server's reader refuses it.
        QTest::newRow("a byte order mark") << QByteArray("\xEF\xBB\xBF{}");
        QTest::newRow("a blank of another kind between two") << QByteArray("[1,\xC2\xA0") + QByteArray("2]");
        QTest::newRow("a form feed between two") << QByteArray("[1,\f2]");
        QTest::newRow("a nought byte behind it") << QByteArray("[1]\0", 4);
        // In a text: an escape that is none, and what has to be spelled out and is not. (Qt's
        // reader takes each of these.)
        QTest::newRow("an escape that is none") << QByteArray("[\"a\\qb\"]");
        QTest::newRow("a quote escaped as in other languages") << QByteArray("[\"a\\'b\"]");
        QTest::newRow("a number spelled with too few digits") << listOfText(spelled(0xfc).chopped(1));
        QTest::newRow("a number spelled with a digit that is none") << listOfText(spelled(0xfc).chopped(1) + "g");
        QTest::newRow("a number spelled with a capital letter") << listOfText(spelled(0xfc, 'U'));
        QTest::newRow("a backslash at the end") << QByteArray("[\"a\\");
        QTest::newRow("a line's end inside a text") << QByteArray("[\"two\nlines\"]");
        QTest::newRow("a tab inside a text") << QByteArray("[\"a\tb\"]");
        QTest::newRow("a nought byte inside a text") << QByteArray("[\"a\0b\"]", 7);
        // Numbers written otherwise than the book has them. (Qt's reader takes some of these.)
        QTest::newRow("a number with a nought in front") << QByteArray("[01]");
        QTest::newRow("a number that ends in its point") << QByteArray("[1.]");
        QTest::newRow("a number that begins with its point") << QByteArray("[.5]");
        QTest::newRow("a minus and a point") << QByteArray("[-.5]");
        QTest::newRow("a minus alone") << QByteArray("[-]");
        QTest::newRow("a plus in front") << QByteArray("[+1]");
        QTest::newRow("an exponent without digits") << QByteArray("[1e]");
        QTest::newRow("an exponent with a sign and no digits") << QByteArray("[1e+]");
        QTest::newRow("a number to the base of sixteen") << QByteArray("[0x10]");
        QTest::newRow("a number with a line under it") << QByteArray("[4_096]");
        // What the server's reader takes and this one does not: there is no such number.
        QTest::newRow("not a number, as the server's language writes it") << QByteArray("[NaN]");
        QTest::newRow("infinity, as the server's language writes it") << QByteArray("[Infinity]");
        QTest::newRow("minus infinity") << QByteArray("[-Infinity]");
    }

    void whatIsNoJsonByTheBookIsRefused()
    {
        QFETCH(QByteArray, text);
        QCOMPARE(saidOf(text), QByteArray(core::json::notJson));
    }

    void halfACharacterIsRefusedWhereverItStands_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::newRow("the first half alone") << listOfText("x" + spelled(firstHalf));
        QTest::newRow("the second half alone") << listOfText(spelled(secondHalf) + "x");
        QTest::newRow("the first half, and a letter") << listOfText(spelled(firstHalf) + "x");
        QTest::newRow("the first half twice") << listOfText(spelled(firstHalf) + spelled(firstHalf));
        QTest::newRow("the halves the wrong way round") << listOfText(spelled(secondHalf) + spelled(firstHalf));
        QTest::newRow("the first half, and a sign spelled out that is no half")
            << listOfText(spelled(firstHalf) + spelled(0x41));
        QTest::newRow("the first of all first halves") << listOfText(spelled(0xd800));
        QTest::newRow("the last of all second halves") << listOfText(spelled(0xdfff));
        QTest::newRow("in a name") << QByteArray("{\"x" + spelled(firstHalf) + "\": 1}");
        QTest::newRow("deep inside") << QByteArray("{\"a\": [{\"b\": {\"c\": \"" + spelled(firstHalf) + "\"}}]}");
    }

    void halfACharacterIsRefusedWhereverItStands()
    {
        QFETCH(QByteArray, text);
        QCOMPARE(saidOf(text), QByteArray(core::json::halfACharacter));
    }

    void bytesThatAreNoSignAreRefused_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("a byte that only follows others") << QByteArray("\x80");
        QTest::newRow("a sign cut off") << QByteArray("\xC3");
        QTest::newRow("a sign of three bytes cut off") << QByteArray("\xE2\x82");
        QTest::newRow("a sign of four bytes cut off") << QByteArray("\xF0\x9F\x98");
        QTest::newRow("a sign written longer than it is (two bytes)") << QByteArray("\xC0\xAF");
        QTest::newRow("a sign written longer than it is (three bytes)") << QByteArray("\xE0\x80\xAF");
        QTest::newRow("a sign written longer than it is (four bytes)") << QByteArray("\xF0\x80\x80\xAF");
        QTest::newRow("the first half of a pair, as bytes") << QByteArray("\xED\xA0\x80");
        QTest::newRow("the second half of a pair, as bytes") << QByteArray("\xED\xBF\xBF");
        QTest::newRow("a sign above the last there is") << QByteArray("\xF4\x90\x80\x80");
        QTest::newRow("a byte no sign begins with") << QByteArray("\xF8\x88\x80\x80\x80");
        QTest::newRow("a byte that is never there") << QByteArray("\xFF");
        QTest::newRow("a second byte that is none") << QByteArray("\xC3\x28");
    }

    void bytesThatAreNoSignAreRefused()
    {
        QFETCH(QByteArray, bytes);
        QCOMPARE(saidOf(listOfText("a" + bytes + "b")), QByteArray(core::json::notUtf8));
        QVERIFY(!core::json::signsIn("ab" + bytes + "cd").has_value());
        QVERIFY(!core::json::signsIn(bytes).has_value());
    }

    void signsAreCountedAndNotBytes()
    {
        const char32_t wide = 0x1F600;
        const QString text = u"a"_s + QChar(0xfc) + QChar(0x20ac) + QString::fromUcs4(&wide, 1);
        QCOMPARE(text.size(), qsizetype{5});
        QCOMPARE(text.toUtf8().size(), qsizetype{10});
        QCOMPARE(core::json::signsIn(text.toUtf8()), std::optional<qsizetype>(4));
        QCOMPARE(core::json::signsIn(QByteArray()), std::optional<qsizetype>(0));
        QCOMPARE(core::json::signsIn(QByteArray("a\0b", 3)), std::optional<qsizetype>(3));
    }

    void aNumberIsOneAPcsOwnNumbersHold_data()
    {
        QTest::addColumn<QByteArray>("written");
        QTest::addColumn<bool>("taken");
        QTest::addColumn<double>("number");
        QTest::addColumn<bool>("whole");
        QTest::newRow("nought") << QByteArray("0") << true << 0.0 << true;
        QTest::newRow("minus nought") << QByteArray("-0") << true << 0.0 << true;
        QTest::newRow("a whole one") << QByteArray("131072") << true << 131072.0 << true;
        QTest::newRow("the same with a point") << QByteArray("131072.0") << true << 131072.0 << false;
        QTest::newRow("the same with an exponent") << QByteArray("1.31072e5") << true << 131072.0 << false;
        QTest::newRow("an exponent in capitals, with a plus") << QByteArray("1E+2") << true << 100.0 << false;
        QTest::newRow("a small one") << QByteArray("2.5e-3") << true << 0.0025 << false;
        QTest::newRow("a whole one that is long") << QByteArray("99999999999999999999") << true << 1e20 << true;
        QTest::newRow("sixty-four signs") << QByteArray(64, '1') << true << 1.1111111111111111e63 << true;
        QTest::newRow("sixty-five signs") << QByteArray(65, '1') << false << 0.0 << false;
        QTest::newRow("too large to be one") << QByteArray("1e999") << false << 0.0 << false;
        QTest::newRow("too small to be one") << QByteArray("1e-999") << false << 0.0 << false;
    }

    void aNumberIsOneAPcsOwnNumbersHold()
    {
        QFETCH(QByteArray, written);
        QFETCH(bool, taken);
        QFETCH(double, number);
        QFETCH(bool, whole);
        QCOMPARE(saidOf("[" + written + "]"), taken ? QByteArray("read") : QByteArray(core::json::notANumber));
        if (taken) {
            const Value read = onlyOf(written);
            QVERIFY(read.isNumber());
            QCOMPARE(read.number, number);
            QCOMPARE(read.whole, whole);
        }
    }

    void nothingIsNestedDeeperThanTheLimit()
    {
        QCOMPARE(saidOf(nested(core::json::deepestNesting)), QByteArray("read"));
        QCOMPARE(saidOf(nested(core::json::deepestNesting + 1)), QByteArray(core::json::tooDeep));
        // A hundred thousand deep, and a hundred thousand that never close: refused as soon as
        // it is seen, with nothing that grows with it.
        QCOMPARE(saidOf(nested(100000)), QByteArray(core::json::tooDeep));
        QCOMPARE(saidOf(QByteArray(100000, '[')), QByteArray(core::json::tooDeep));
        QByteArray objects;
        for (int level = 0; level <= core::json::deepestNesting; ++level) {
            objects += R"({"a":)";
        }
        QCOMPARE(saidOf(objects + "1" + QByteArray(core::json::deepestNesting + 1, '}')),
                 QByteArray(core::json::tooDeep));
    }

    void aTextHoldsNoMoreValuesThanTheLimit()
    {
        // A list is one value, and each thing in it is one more.
        QByteArray many = "[0";
        many += QByteArray(",0").repeated(core::json::mostValues - 2);
        QCOMPARE(saidOf(many + "]"), QByteArray("read"));
        QCOMPARE(saidOf(many + ",0]"), QByteArray(core::json::tooMany));
    }
};

QTEST_GUILESS_MAIN(TestStrictJson)
#include "test_strict_json.moc"
