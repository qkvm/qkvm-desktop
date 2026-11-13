#include <vector>

#include <QTest>

#include <qkvm/core/jetkvm_layouts.h>
#include <qkvm/core/typed_keys.h>

using namespace Qt::StringLiterals;
namespace jetkvm = qkvm::core::jetkvm;
namespace typed = qkvm::core::typed;
namespace json = qkvm::core::json;

namespace {

typed::Report press(int modifiers, int usage)
{
    typed::Report report;
    report.modifiers = static_cast<std::uint8_t>(modifiers);
    report.keys.push_back(static_cast<std::uint8_t>(usage));
    return report;
}

QString of(char32_t character)
{
    return QString::fromUcs4(&character, 1);
}

json::Value text(const QString& said)
{
    json::Value value;
    value.kind = json::Value::Kind::Text;
    value.text = said;
    return value;
}

} // namespace

// Text typed on a JetKVM in the layout its computer is set to: the web's tables (app/qkvm/layouts.py, from Unicode
// CLDR's data), made into C++ at build time, checked against characters of the web's module.
class TestJetKvmLayouts : public QObject {
    Q_OBJECT

private slots:
    void theNineteenLayoutsAreThereWithTheirNames()
    {
        QCOMPARE(jetkvm::layouts().size(), 19);
        QVERIFY(jetkvm::layouts().contains(u"ja-JP"_s));
        QCOMPARE(jetkvm::titleOf(u"de-DE"_s), u"German"_s);
        QCOMPARE(jetkvm::titleOf(u"pl-PL"_s), u"Polish (Programmers)"_s);
        QCOMPARE(jetkvm::titleOf(u"xx-YY"_s), QString());
    }

    // As the web's module has them: on German "z" is the key where a US keyboard has Y, "@" AltGr and Q, the euro
    // AltGr and E.
    void aCharacterIsPressedByItsLayoutsKeys()
    {
        const auto reports = jetkvm::textReports(u"z@"_s + of(0x20AC), u"de-DE"_s);
        QVERIFY(reports);
        QCOMPARE(reports.value(), (std::vector<typed::Report>{press(0, 0x1C), press(0x40, 0x14), press(0x40, 0x08)}));
        const auto russian = jetkvm::textReports(of(0x0436), u"ru-RU"_s);
        QVERIFY(russian);
        QCOMPARE(russian.value(), std::vector<typed::Report>{press(0, 0x33)});
    }

    // A character a dead key makes is that key and the next.
    void aDeadKeysCharacterIsTwoPresses()
    {
        const auto reports = jetkvm::textReports(u"a"_s + of(0x00EA), u"fr-FR"_s);
        QVERIFY(reports);
        QCOMPARE(reports.value(), (std::vector<typed::Report>{press(0, 0x14), press(0, 0x2F), press(0, 0x08)}));
    }

    // Refused before anything is typed, by its place and never itself (a text may be a password).
    void aCharacterTheLayoutCannotTypeIsRefused()
    {
        const auto reports = jetkvm::textReports(u"ab"_s + of(0x0436), u"de-DE"_s);
        QVERIFY(!reports);
        QCOMPARE(reports.error().code, std::string("invalid"));
        QCOMPARE(reports.error().message,
                 std::string("Nothing was typed: character 3 of the text cannot be typed on the "
                             "German keyboard layout this JetKVM's computer is set to"));
        const auto us = jetkvm::textReports(u"a"_s + of(0x00E9), u"en-US"_s);
        QVERIFY(!us);
        QCOMPARE(us.error().message, std::string("Nothing was typed: character 2 of the text cannot be typed on the US "
                                                 "keyboard layout this JetKVM's computer is set to"));
    }

    void aLayoutWithNoTableIsRefused()
    {
        const auto reports = jetkvm::textReports(u"a"_s, u"(something)"_s);
        QVERIFY(!reports);
        QCOMPARE(reports.error().code, std::string("invalid"));
        QCOMPARE(reports.error().message,
                 std::string("Nothing was typed: this JetKVM's computer is set to the keyboard "
                             "layout (something), which qKVM has no table for"));
    }

    // US English is the US table every other part of the program types with.
    void theUsLayoutIsTheUsTable()
    {
        const auto reports = jetkvm::textReports(u"Ab!\t"_s, u"en-US"_s);
        QVERIFY(reports);
        QCOMPARE(reports.value(), typed::text(u"Ab!\t"_s).value());
    }

    void aWindowsLineEndIsOneEnter()
    {
        const auto reports = jetkvm::textReports(u"a\r\nb"_s, u"de-DE"_s);
        QVERIFY(reports);
        QCOMPARE(reports.value(), (std::vector<typed::Report>{press(0, 0x04), press(0, 0x28), press(0, 0x05)}));
    }

    void tooMuchOrNothingIsRefused()
    {
        QVERIFY(!jetkvm::textReports(QString(), u"de-DE"_s));
        QVERIFY(!jetkvm::textReports(QString(typed::mostCharacters + 1, u'a'), u"de-DE"_s));
        QVERIFY(jetkvm::textReports(QString(typed::mostCharacters, u'a'), u"de-DE"_s));
    }

    // What a JetKVM says of its layout, as the web takes it: nothing is US English, its own page's way.
    void whatItSaysIsTakenAsTheWebTakesIt()
    {
        QCOMPARE(jetkvm::layoutOf(text(QString())), u"en-US"_s);
        QCOMPARE(jetkvm::layoutOf(text(u"de-DE"_s)), u"de-DE"_s);
        QCOMPARE(jetkvm::layoutOf(text(u"en_US"_s)), u"en-US"_s);
        QCOMPARE(jetkvm::layoutOf(text(u"not a layout"_s)), u"(not a layout)"_s);
        json::Value number;
        number.kind = json::Value::Kind::Number;
        number.number = 5;
        QCOMPARE(jetkvm::layoutOf(number), u"(5)"_s);
        QCOMPARE(jetkvm::layoutOf(json::Value{}), u"(none)"_s);
    }
};

QTEST_GUILESS_MAIN(TestJetKvmLayouts)
#include "test_jetkvm_layouts.moc"
