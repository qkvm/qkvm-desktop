#include <limits>
#include <optional>
#include <tuple>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>

#include <qkvm/core/kvmd_input.h>

using namespace Qt::StringLiterals;
using namespace qkvm::core;

namespace {

// A message read back as kvmd reads it: as JSON, in which the order of the names says nothing.
QJsonObject read(const QByteArray& word)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(word, &error);
    return error.error == QJsonParseError::NoError ? document.object() : QJsonObject{{u"unreadable"_s, true}};
}

QJsonObject read(const char* word)
{
    return read(QByteArray(word));
}

// The same of a word there may be none of: none reads as nothing kvmd could take.
QJsonObject read(const std::optional<QByteArray>& word)
{
    return word ? read(word.value_or(QByteArray())) : QJsonObject{{u"no word"_s, true}};
}

struct Step {
    int x = 0;
    int y = 0;
};

Step stepOf(const QByteArray& word)
{
    const QJsonObject said = read(word);
    const QJsonObject delta = said["event"_L1]["delta"_L1].toObject();
    if (said["event_type"_L1].toString() != "mouse_relative"_L1 || delta.size() != 2 || !delta["x"_L1].isDouble() ||
        !delta["y"_L1].isDouble()) {
        return {1000, 1000}; // no step at all, and found to be none
    }
    return {delta["x"_L1].toInt(), delta["y"_L1].toInt()};
}

// The keys kvmd has a name for, written out a second time, from the two tables (USB HID usage
// tables, page 7; UI Events KeyboardEvent code values) and not from the program's.
struct Key {
    int usage;
    const char* name;
};
constexpr Key keys[] = {
    {0x04, "KeyA"},
    {0x05, "KeyB"},
    {0x06, "KeyC"},
    {0x07, "KeyD"},
    {0x08, "KeyE"},
    {0x09, "KeyF"},
    {0x0A, "KeyG"},
    {0x0B, "KeyH"},
    {0x0C, "KeyI"},
    {0x0D, "KeyJ"},
    {0x0E, "KeyK"},
    {0x0F, "KeyL"},
    {0x10, "KeyM"},
    {0x11, "KeyN"},
    {0x12, "KeyO"},
    {0x13, "KeyP"},
    {0x14, "KeyQ"},
    {0x15, "KeyR"},
    {0x16, "KeyS"},
    {0x17, "KeyT"},
    {0x18, "KeyU"},
    {0x19, "KeyV"},
    {0x1A, "KeyW"},
    {0x1B, "KeyX"},
    {0x1C, "KeyY"},
    {0x1D, "KeyZ"},
    {0x1E, "Digit1"},
    {0x1F, "Digit2"},
    {0x20, "Digit3"},
    {0x21, "Digit4"},
    {0x22, "Digit5"},
    {0x23, "Digit6"},
    {0x24, "Digit7"},
    {0x25, "Digit8"},
    {0x26, "Digit9"},
    {0x27, "Digit0"},
    {0x28, "Enter"},
    {0x29, "Escape"},
    {0x2A, "Backspace"},
    {0x2B, "Tab"},
    {0x2C, "Space"},
    {0x2D, "Minus"},
    {0x2E, "Equal"},
    {0x2F, "BracketLeft"},
    {0x30, "BracketRight"},
    {0x31, "Backslash"},
    {0x33, "Semicolon"},
    {0x34, "Quote"},
    {0x35, "Backquote"},
    {0x36, "Comma"},
    {0x37, "Period"},
    {0x38, "Slash"},
    {0x39, "CapsLock"},
    {0x3A, "F1"},
    {0x3B, "F2"},
    {0x3C, "F3"},
    {0x3D, "F4"},
    {0x3E, "F5"},
    {0x3F, "F6"},
    {0x40, "F7"},
    {0x41, "F8"},
    {0x42, "F9"},
    {0x43, "F10"},
    {0x44, "F11"},
    {0x45, "F12"},
    {0x46, "PrintScreen"},
    {0x47, "ScrollLock"},
    {0x48, "Pause"},
    {0x49, "Insert"},
    {0x4A, "Home"},
    {0x4B, "PageUp"},
    {0x4C, "Delete"},
    {0x4D, "End"},
    {0x4E, "PageDown"},
    {0x4F, "ArrowRight"},
    {0x50, "ArrowLeft"},
    {0x51, "ArrowDown"},
    {0x52, "ArrowUp"},
    {0x53, "NumLock"},
    {0x54, "NumpadDivide"},
    {0x55, "NumpadMultiply"},
    {0x56, "NumpadSubtract"},
    {0x57, "NumpadAdd"},
    {0x58, "NumpadEnter"},
    {0x59, "Numpad1"},
    {0x5A, "Numpad2"},
    {0x5B, "Numpad3"},
    {0x5C, "Numpad4"},
    {0x5D, "Numpad5"},
    {0x5E, "Numpad6"},
    {0x5F, "Numpad7"},
    {0x60, "Numpad8"},
    {0x61, "Numpad9"},
    {0x62, "Numpad0"},
    {0x63, "NumpadDecimal"},
    {0x64, "IntlBackslash"},
    {0x65, "ContextMenu"},
    // (Not 0x66, the keyboard's Power key, which kvmd has: it is never said, below.)
    {0x87, "IntlRo"},
    {0x88, "KanaMode"},
    {0x89, "IntlYen"},
    {0x8A, "Convert"},
    {0x8B, "NonConvert"},
    {0xE0, "ControlLeft"},
    {0xE1, "ShiftLeft"},
    {0xE2, "AltLeft"},
    {0xE3, "MetaLeft"},
    {0xE4, "ControlRight"},
    {0xE5, "ShiftRight"},
    {0xE6, "AltRight"},
    {0xE7, "MetaRight"},
};

// The keys that matter most, a second time and by themselves: not made from the list above nor in
// its order, but by what they do and where they lie on a keyboard, with USB's numbers in tens as
// its tables print them beside the others. The list above is the program's own in the same order;
// two rows that changed places in both are found by this one.
constexpr Key mostNeeded[] = {
    // The eight that are held with others.
    {224, "ControlLeft"},
    {225, "ShiftLeft"},
    {226, "AltLeft"},
    {227, "MetaLeft"},
    {228, "ControlRight"},
    {229, "ShiftRight"},
    {230, "AltRight"},
    {231, "MetaRight"},
    // What ends, leaves, rubs out and moves on.
    {40, "Enter"},
    {41, "Escape"},
    {42, "Backspace"},
    {43, "Tab"},
    {44, "Space"},
    {76, "Delete"},
    {74, "Home"},
    {77, "End"},
    {75, "PageUp"},
    {78, "PageDown"},
    {82, "ArrowUp"},
    {81, "ArrowDown"},
    {80, "ArrowLeft"},
    {79, "ArrowRight"},
    {58, "F1"},
    {59, "F2"},
    {60, "F3"},
    {61, "F4"},
    {62, "F5"},
    {63, "F6"},
    {64, "F7"},
    {65, "F8"},
    {66, "F9"},
    {67, "F10"},
    {68, "F11"},
    {69, "F12"},
    // The digits as they lie, the nought last.
    {30, "Digit1"},
    {31, "Digit2"},
    {32, "Digit3"},
    {33, "Digit4"},
    {34, "Digit5"},
    {35, "Digit6"},
    {36, "Digit7"},
    {37, "Digit8"},
    {38, "Digit9"},
    {39, "Digit0"},
    // The letters as they lie, row by row.
    {20, "KeyQ"},
    {26, "KeyW"},
    {8, "KeyE"},
    {21, "KeyR"},
    {23, "KeyT"},
    {28, "KeyY"},
    {24, "KeyU"},
    {12, "KeyI"},
    {18, "KeyO"},
    {19, "KeyP"},
    {4, "KeyA"},
    {22, "KeyS"},
    {7, "KeyD"},
    {9, "KeyF"},
    {10, "KeyG"},
    {11, "KeyH"},
    {13, "KeyJ"},
    {14, "KeyK"},
    {15, "KeyL"},
    {29, "KeyZ"},
    {27, "KeyX"},
    {6, "KeyC"},
    {25, "KeyV"},
    {5, "KeyB"},
    {17, "KeyN"},
    {16, "KeyM"},
};

} // namespace

// What a KVM that runs kvmd is told of keyboard and mouse, word for word.
class TestKvmdInput : public QObject {
    Q_OBJECT

private slots:
    void everyKeyHasTheNameOfItsPlaceOnTheKeyboard_data()
    {
        QTest::addColumn<int>("usage");
        QTest::addColumn<QString>("name");
        for (const Key& key : keys) {
            QTest::newRow(key.name) << key.usage << QString::fromLatin1(key.name);
        }
    }

    void everyKeyHasTheNameOfItsPlaceOnTheKeyboard()
    {
        QFETCH(int, usage);
        QFETCH(QString, name);
        const auto named = kvmd::keyName(usage);
        QVERIFY(named.has_value());
        QCOMPARE(QString(named.value_or(QLatin1StringView())), name);
        // And is said by that name, going down and going up.
        for (const bool down : {true, false}) {
            const auto word = kvmd::keyWord(usage, down);
            QVERIFY(word.has_value());
            const QJsonObject expected{{u"event_type"_s, u"key"_s},
                                       {u"event"_s, QJsonObject{{u"key"_s, name}, {u"state"_s, down}}}};
            QCOMPARE(read(word), expected);
        }
    }

    void theKeysThatMatterMostHaveTheirNamesByAListOfItsOwn_data()
    {
        QTest::addColumn<int>("usage");
        QTest::addColumn<QString>("name");
        for (const Key& key : mostNeeded) {
            QTest::newRow(key.name) << key.usage << QString::fromLatin1(key.name);
        }
    }

    void theKeysThatMatterMostHaveTheirNamesByAListOfItsOwn()
    {
        QFETCH(int, usage);
        QFETCH(QString, name);
        QCOMPARE(QString(kvmd::keyName(usage).value_or(QLatin1StringView())), name);
        const QJsonObject expected{{u"event_type"_s, u"key"_s},
                                   {u"event"_s, QJsonObject{{u"key"_s, name}, {u"state"_s, true}}}};
        QCOMPARE(read(kvmd::keyWord(usage, true)), expected);
    }

    void thatListLeavesOutNoneOfTheKeysThatMatterMost()
    {
        // Twenty-six letters, ten digits, twelve function keys, eight held with others, and the
        // fourteen that end, leave, rub out and move: each once.
        QSet<int> usages;
        QSet<QString> names;
        for (const Key& key : mostNeeded) {
            usages.insert(key.usage);
            names.insert(QString::fromLatin1(key.name));
        }
        QCOMPARE(usages.size(), qsizetype{70});
        QCOMPARE(names.size(), qsizetype{70});
    }

    void lettersDigitsAndFunctionKeysFollowOneAnotherInUsbsNumbersToo()
    {
        // Counted, and from neither list: USB numbers the letters from 4 in the alphabet's order,
        // the digits from 30 with the nought last, the function keys from 58.
        for (int place = 0; place < 26; ++place) {
            const QString name = u"Key"_s + QChar(u'A' + place);
            QCOMPARE(QString(kvmd::keyName(4 + place).value_or(QLatin1StringView())), name);
        }
        for (int digit = 1; digit <= 10; ++digit) {
            const QString name = u"Digit%1"_s.arg(digit % 10);
            QCOMPARE(QString(kvmd::keyName(29 + digit).value_or(QLatin1StringView())), name);
        }
        for (int number = 1; number <= 12; ++number) {
            const QString name = u"F%1"_s.arg(number);
            QCOMPARE(QString(kvmd::keyName(57 + number).value_or(QLatin1StringView())), name);
        }
    }

    void noTwoKeysOfTheProgramsOwnTableShareAName()
    {
        // (This counted the names in the list written out above, which is the test's own, and
        // so could not find a name twice in the program's. Now the program is asked, for every
        // number a key can have.)
        QSet<QString> names;
        int named = 0;
        for (int usage = 0; usage <= 0xFFFF; ++usage) {
            const std::optional<QLatin1StringView> name = kvmd::keyName(usage);
            if (!name) {
                continue;
            }
            ++named;
            if (names.contains(QString(*name))) {
                QFAIL(qPrintable(u"The name of the key with the number %1 is another key's as well."_s.arg(usage)));
            }
            names.insert(QString(*name));
        }
        QCOMPARE(named, 110);
        QCOMPARE(names.size(), qsizetype{110});
    }

    void aNumberThatIsNoKeyHasNoName_data()
    {
        QTest::addColumn<int>("usage");
        QTest::newRow("none") << 0;
        QTest::newRow("too many keys at once") << 1;
        QTest::newRow("the keyboard's self-test failed") << 2;
        QTest::newRow("a mistake of the keyboard's") << 3;
        QTest::newRow("the key beside Enter that is not American") << 0x32;
        QTest::newRow("the keyboard's Power key, which kvmd has") << 0x66;
        QTest::newRow("the number block's =") << 0x67;
        QTest::newRow("F13") << 0x68;
        QTest::newRow("a number kept for later") << 0xA5;
        QTest::newRow("the first after the last") << 0xE8;
        QTest::newRow("the largest of two bytes") << 0xFFFF;
        QTest::newRow("a key and something above it") << 0x10004;
        QTest::newRow("less than none") << -1;
        QTest::newRow("a letter's, turned around") << -4;
        QTest::newRow("the smallest there is") << std::numeric_limits<int>::min();
        QTest::newRow("the largest there is") << std::numeric_limits<int>::max();
    }

    void aNumberThatIsNoKeyHasNoName()
    {
        QFETCH(int, usage);
        QVERIFY(!kvmd::keyName(usage).has_value());
        QVERIFY(!kvmd::keyWord(usage, true).has_value());
        QVERIFY(!kvmd::keyWord(usage, false).has_value());
    }

    void theKeyboardsPowerKeyIsNeverSaid()
    {
        // USB 0x66 shuts a machine down without the question every power action is asked first.
        // kvmd has a name for it ("Power"), and is told it in neither wording, going down or up
        // (as the web's live line, NOT_SENT).
        QVERIFY(!kvmd::keyName(0x66).has_value());
        for (const bool down : {true, false}) {
            QVERIFY(!kvmd::keyWord(0x66, down).has_value());
            QVERIFY(!kvmd::keyBytes(0x66, down).has_value());
        }
        // Its neighbours are said as ever.
        QCOMPARE(QString(kvmd::keyName(0x65).value_or(QLatin1StringView())), u"ContextMenu"_s);
        QCOMPARE(QString(kvmd::keyName(0x87).value_or(QLatin1StringView())), u"IntlRo"_s);
    }

    void noOtherNumberHasANameThanTheKeysWrittenOutHere()
    {
        QSet<int> known;
        for (const Key& key : keys) {
            known.insert(key.usage);
        }
        for (int usage = -0x200; usage <= 0x20000; ++usage) {
            if (kvmd::keyName(usage).has_value() != known.contains(usage)) {
                QFAIL(
                    qPrintable(u"The number %1 has a name where it should have none, or the other way."_s.arg(usage)));
            }
        }
    }

    void aKeyIsSaidInTheWordsOfKvmdsOwnPage()
    {
        QCOMPARE(read(kvmd::keyWord(0x04, true)), read(R"({"event_type":"key","event":{"key":"KeyA","state":true}})"));
        QCOMPARE(read(kvmd::keyWord(0xE1, false)),
                 read(R"({"event_type":"key","event":{"key":"ShiftLeft","state":false}})"));
        // One line of compact JSON, with nothing in it but the two names and what they say.
        const QByteArray word = kvmd::keyWord(0x04, true).value_or(QByteArray());
        QVERIFY(!word.contains('\n') && !word.contains(' '));
        QCOMPARE(read(word).size(), qsizetype{2});
    }

    void aButtonIsSaidByItsName_data()
    {
        QTest::addColumn<int>("number");
        QTest::addColumn<QString>("name");
        QTest::newRow("left") << 1 << u"left"_s;
        QTest::newRow("right") << 2 << u"right"_s;
        QTest::newRow("middle") << 3 << u"middle"_s;
        QTest::newRow("back") << 4 << u"up"_s;
        QTest::newRow("forward") << 5 << u"down"_s;
    }

    void aButtonIsSaidByItsName()
    {
        QFETCH(int, number);
        QFETCH(QString, name);
        for (const bool down : {true, false}) {
            const auto word = kvmd::buttonWord(number, down);
            QVERIFY(word.has_value());
            const QJsonObject expected{{u"event_type"_s, u"mouse_button"_s},
                                       {u"event"_s, QJsonObject{{u"button"_s, name}, {u"state"_s, down}}}};
            QCOMPARE(read(word), expected);
        }
    }

    void aButtonAMouseHasNotIsNotSaid()
    {
        for (const int number : {0, 6, 7, -1, 255, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
            QVERIFY(!kvmd::buttonWord(number, true).has_value());
            QVERIFY(!kvmd::buttonWord(number, false).has_value());
        }
        QCOMPARE(read(kvmd::buttonWord(1, true)),
                 read(R"({"event_type":"mouse_button","event":{"button":"left","state":true}})"));
    }

    void thePointersPlaceIsSaidFromOneEdgeOfKvmdsCountToTheOther_data()
    {
        QTest::addColumn<int>("asked");
        QTest::addColumn<int>("said");
        QTest::newRow("the one edge") << 0 << -32768;
        QTest::newRow("the other edge") << 32767 << 32767;
        // (The middle of the one count is between two of its places, and so is the other's:
        // the two places beside the one are as far from the middle as those beside the other.)
        QTest::newRow("just before the middle") << 16383 << -2;
        QTest::newRow("just after the middle") << 16384 << 1;
        QTest::newRow("a step from the one edge") << 1 << -32766;
        QTest::newRow("a step from the other") << 32766 << 32765;
        QTest::newRow("a quarter") << 8192 << -16384;
        QTest::newRow("three quarters") << 24576 << 16385;
        QTest::newRow("beyond the one edge") << -1 << -32768;
        QTest::newRow("far beyond it") << std::numeric_limits<int>::min() << -32768;
        QTest::newRow("beyond the other edge") << 32768 << 32767;
        QTest::newRow("far beyond that") << std::numeric_limits<int>::max() << 32767;
    }

    void thePointersPlaceIsSaidFromOneEdgeOfKvmdsCountToTheOther()
    {
        QFETCH(int, asked);
        QFETCH(int, said);
        // Each way by itself: the other stays where it is.
        const QJsonObject across{{u"event_type"_s, u"mouse_move"_s},
                                 {u"event"_s, QJsonObject{{u"to"_s, QJsonObject{{u"x"_s, said}, {u"y"_s, 32767}}}}}};
        QCOMPARE(read(kvmd::pointerAtWord(asked, 32767)), across);
        const QJsonObject down{{u"event_type"_s, u"mouse_move"_s},
                               {u"event"_s, QJsonObject{{u"to"_s, QJsonObject{{u"x"_s, -32768}, {u"y"_s, said}}}}}};
        QCOMPARE(read(kvmd::pointerAtWord(0, asked)), down);
    }

    void thePointersPlaceNeverGoesBackwardsAndIsAlwaysOneOfKvmds()
    {
        const auto saidFor = [](int asked) {
            return read(kvmd::pointerAtWord(asked, 0))["event"_L1]["to"_L1]["x"_L1].toInt(100'000);
        };
        int before = -32769;
        for (int asked = 0; asked <= 32767; ++asked) {
            const int said = saidFor(asked);
            QVERIFY(said >= -32768 && said <= 32767);
            QVERIFY(said > before);
            // As far from the one edge as its like on the other side is from the other.
            QCOMPARE(saidFor(32767 - asked), -1 - said);
            before = said;
        }
        QCOMPARE(read(kvmd::pointerAtWord(0, 32767)),
                 read(R"({"event_type":"mouse_move","event":{"to":{"x":-32768,"y":32767}}})"));
    }

    void aShortMoveIsOneMessage()
    {
        QCOMPARE(kvmd::pointerByWords(5, -7).size(), qsizetype{1});
        QCOMPARE(read(kvmd::pointerByWords(5, -7).at(0)),
                 read(R"({"event_type":"mouse_relative","event":{"delta":{"x":5,"y":-7}}})"));
        QCOMPARE(kvmd::pointerByWords(127, -127).size(), qsizetype{1});
        QCOMPARE(read(kvmd::pointerByWords(127, -127).at(0)),
                 read(R"({"event_type":"mouse_relative","event":{"delta":{"x":127,"y":-127}}})"));
        QCOMPARE(kvmd::pointerByWords(0, 1).size(), qsizetype{1});
        // And no move is no message.
        QVERIFY(kvmd::pointerByWords(0, 0).isEmpty());
    }

    void aLongMoveIsCutIntoSeveralThatAreTogetherThatMove_data()
    {
        QTest::addColumn<int>("dx");
        QTest::addColumn<int>("dy");
        QTest::addColumn<int>("messages");
        QTest::newRow("a step more than one takes") << 128 << 0 << 2;
        QTest::newRow("the same, the other way") << 0 << -128 << 2;
        QTest::newRow("two full ones") << 254 << -254 << 2;
        QTest::newRow("three, one way far longer") << 300 << 10 << 3;
        QTest::newRow("up and to the left") << -1000 << -333 << 8;
        QTest::newRow("one way a single step") << 1 << 2000 << 16;
        QTest::newRow("as far as is ever said") << 4064 << -4064 << 32;
    }

    void aLongMoveIsCutIntoSeveralThatAreTogetherThatMove()
    {
        QFETCH(int, dx);
        QFETCH(int, dy);
        QFETCH(int, messages);
        const QList<QByteArray> words = kvmd::pointerByWords(dx, dy);
        QCOMPARE(words.size(), qsizetype{messages});
        int x = 0;
        int y = 0;
        for (const QByteArray& word : words) {
            const Step step = stepOf(word);
            // Each is one kvmd takes, and none is no move.
            QVERIFY(step.x >= -127 && step.x <= 127);
            QVERIFY(step.y >= -127 && step.y <= 127);
            QVERIFY(step.x != 0 || step.y != 0);
            x += step.x;
            y += step.y;
            // Never off the straight line by more than the one step that rounding makes.
            QVERIFY(qAbs(qint64{x} * dy - qint64{y} * dx) <= qMax(qAbs(dx), qAbs(dy)));
        }
        QCOMPARE(x, dx);
        QCOMPARE(y, dy);
    }

    void aMoveFromNowhereIsNotAMillionMessages()
    {
        const int least = std::numeric_limits<int>::min();
        const int most = std::numeric_limits<int>::max();
        for (const auto& [dx, dy, x, y] : {std::tuple{most, 0, 4064, 0}, std::tuple{least, least, -4064, -4064},
                                           std::tuple{5000, -2, 4064, -2}, std::tuple{0, most, 0, 4064}}) {
            const QList<QByteArray> words = kvmd::pointerByWords(dx, dy);
            QCOMPARE(words.size(), qsizetype{kvmd::mostSteps});
            int wentX = 0;
            int wentY = 0;
            for (const QByteArray& word : words) {
                const Step step = stepOf(word);
                QVERIFY(qAbs(step.x) <= 127 && qAbs(step.y) <= 127);
                wentX += step.x;
                wentY += step.y;
            }
            QCOMPARE(wentX, x);
            QCOMPARE(wentY, y);
        }
    }

    void theWheelIsSaidAndCutToWhatOneMessageTakes()
    {
        QCOMPARE(read(kvmd::wheelWord(0, -5)),
                 read(R"({"event_type":"mouse_wheel","event":{"delta":{"x":0,"y":-5}}})"));
        QCOMPARE(read(kvmd::wheelWord(3, 0)), read(R"({"event_type":"mouse_wheel","event":{"delta":{"x":3,"y":0}}})"));
        QCOMPARE(read(kvmd::wheelWord(127, -127)),
                 read(R"({"event_type":"mouse_wheel","event":{"delta":{"x":127,"y":-127}}})"));
        QCOMPARE(read(kvmd::wheelWord(128, -128)),
                 read(R"({"event_type":"mouse_wheel","event":{"delta":{"x":127,"y":-127}}})"));
        QCOMPARE(read(kvmd::wheelWord(std::numeric_limits<int>::max(), std::numeric_limits<int>::min())),
                 read(R"({"event_type":"mouse_wheel","event":{"delta":{"x":127,"y":-127}}})"));
        QVERIFY(!kvmd::wheelWord(0, 0).has_value());
    }

    void theOtherWordingIsShortMessagesOfBytes()
    {
        const auto hex = [](const char* written) { return QByteArray::fromHex(written); };
        // A key: what it is, down or up, and its name.
        QCOMPARE(kvmd::keyBytes(0x04, true), hex("0101") + "KeyA");
        QCOMPARE(kvmd::keyBytes(0x04, false), hex("0100") + "KeyA");
        QCOMPARE(kvmd::keyBytes(0xE1, true), hex("0101") + "ShiftLeft");
        QCOMPARE(kvmd::keyBytes(0x4C, false), hex("0100") + "Delete");
        // A button, by the names the texts have for them.
        QCOMPARE(kvmd::buttonBytes(1, true), hex("0201") + "left");
        QCOMPARE(kvmd::buttonBytes(2, false), hex("0200") + "right");
        QCOMPARE(kvmd::buttonBytes(3, true), hex("0201") + "middle");
        QCOMPARE(kvmd::buttonBytes(4, true), hex("0201") + "up");
        QCOMPARE(kvmd::buttonBytes(5, false), hex("0200") + "down");
        // A place: two numbers of sixteen bits, the high byte first, from the one edge to the
        // other as in the texts; what is beyond is the edge.
        QCOMPARE(kvmd::pointerAtBytes(0, 0), hex("0380008000"));
        QCOMPARE(kvmd::pointerAtBytes(32767, 32767), hex("037fff7fff"));
        QCOMPARE(kvmd::pointerAtBytes(-5, 99'999), hex("0380007fff"));
        QCOMPARE(kvmd::pointerAtBytes(16384, 16383), hex("030001fffe"));
        // A move: a zero and two numbers of eight bits, each with its sign.
        QCOMPARE(kvmd::pointerByBytes(5, -3), QList<QByteArray>{hex("040005fd")});
        QCOMPARE(kvmd::pointerByBytes(-127, 127), QList<QByteArray>{hex("0400817f")});
        // The wheel, cut to what one message takes.
        QCOMPARE(kvmd::wheelBytes(0, -1), hex("050000ff"));
        QCOMPARE(kvmd::wheelBytes(500, -500), hex("05007f81"));
    }

    void whatHasNoWordInTextsHasNoneInBytes()
    {
        for (const int usage : {0, 1, 2, 3, 0xA5, 0xE8, 0xFFFF, -1}) {
            QVERIFY2(!kvmd::keyBytes(usage, true), QByteArray::number(usage).constData());
            QVERIFY(!kvmd::keyWord(usage, true));
        }
        for (const int number : {0, 6, -1, 100}) {
            QVERIFY(!kvmd::buttonBytes(number, true));
            QVERIFY(!kvmd::buttonWord(number, true));
        }
        QVERIFY(kvmd::pointerByBytes(0, 0).isEmpty());
        QVERIFY(!kvmd::wheelBytes(0, 0));
        // And every key that has a name has it in both.
        for (int usage = 0; usage < 256; ++usage) {
            QCOMPARE(kvmd::keyBytes(usage, true).has_value(), kvmd::keyWord(usage, true).has_value());
            if (const std::optional<QLatin1StringView> name = kvmd::keyName(usage)) {
                const QByteArray bytes = kvmd::keyBytes(usage, true).value_or(QByteArray());
                QCOMPARE(bytes.mid(2), QByteArray(name->data(), name->size()));
            }
        }
    }

    void aLongMoveIsCutInBytesAsItIsInTexts()
    {
        for (const auto& [dx, dy] :
             {std::pair{300, -127}, std::pair{-1000, 40}, std::pair{1, 4000}, std::pair{9'999'999, -9'999'999}}) {
            const QList<QByteArray> bytes = kvmd::pointerByBytes(dx, dy);
            QCOMPARE(bytes.size(), kvmd::pointerByWords(dx, dy).size());
            int x = 0;
            int y = 0;
            for (const QByteArray& one : bytes) {
                QCOMPARE(one.size(), 4);
                QCOMPARE(one.at(0), char{4});
                QCOMPARE(one.at(1), char{0});
                x += static_cast<qint8>(one.at(2));
                y += static_cast<qint8>(one.at(3));
            }
            const int farthest = kvmd::longestStep * kvmd::mostSteps;
            QCOMPARE(x, std::clamp(dx, -farthest, farthest));
            QCOMPARE(y, std::clamp(dy, -farthest, farthest));
        }
    }

    void areYouThereIsAskedAsKvmdsPageAsksIt()
    {
        QCOMPARE(read(kvmd::pingWord()), read(R"({"event_type":"ping","event":{}})"));
        QVERIFY(read(kvmd::pingWord())["event"_L1].isObject());
    }

    void everyNumberIsWrittenAsAWholeOne()
    {
        // kvmd takes whole numbers: none is written with a point or as a power of ten.
        for (const QByteArray& word : {kvmd::pointerAtWord(0, 32767), kvmd::pointerByWords(-127, 100).value(0),
                                       kvmd::wheelWord(-127, 127).value_or(QByteArray())}) {
            QVERIFY(!word.isEmpty());
            QVERIFY(!word.contains('.'));
            QVERIFY(!word.contains("e+") && !word.contains('E'));
        }
        QVERIFY(kvmd::pointerAtWord(0, 32767).contains("\"x\":-32768"));
        QVERIFY(kvmd::pointerAtWord(0, 32767).contains("\"y\":32767"));
    }
};

QTEST_APPLESS_MAIN(TestKvmdInput)

#include "test_kvmd_input.moc"
