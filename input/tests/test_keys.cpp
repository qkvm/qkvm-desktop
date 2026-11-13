#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include <QList>
#include <QTest>

#include <qkvm/input/keys.h>

using qkvm::input::ControlKeys;
using qkvm::input::KeysDown;
using qkvm::input::usageOf;

Q_DECLARE_METATYPE(qkvm::input::ControlKeys::Does)

// A key is where it is on the keyboard: what Windows tells of it becomes the number USB has for
// it. And what a key does while a machine is controlled.
class TestKeys : public QObject {
    Q_OBJECT

private slots:
    void aPhysicalKeyIsItsUsbNumber_data()
    {
        QTest::addColumn<quint32>("scanCode");
        QTest::addColumn<bool>("extended");
        QTest::addColumn<quint32>("virtualKey");
        QTest::addColumn<int>("usage");
        const auto key = [](const char* name, quint32 scanCode, bool extended, quint32 virtualKey, int usage) {
            QTest::newRow(name) << scanCode << extended << virtualKey << usage;
        };
        // Letters by where they are, not by what a layout prints on them.
        key("A", 0x1E, false, 0x41, 0x04);
        key("Z on a US keyboard", 0x2C, false, 0x5A, 0x1D);
        key("the same key on a German one, where it is Y", 0x2C, false, 0x59, 0x1D);
        key("Q, which is A on a French one", 0x10, false, 0x41, 0x14);
        key("M", 0x32, false, 0x4D, 0x10);
        key("1", 0x02, false, 0x31, 0x1E);
        key("0", 0x0B, false, 0x30, 0x27);
        key("Esc", 0x01, false, 0x1B, 0x29);
        key("Backspace", 0x0E, false, 0x08, 0x2A);
        key("Tab", 0x0F, false, 0x09, 0x2B);
        key("Space", 0x39, false, 0x20, 0x2C);
        key("Enter", 0x1C, false, 0x0D, 0x28);
        key("Enter on the number block", 0x1C, true, 0x0D, 0x58);
        key("Caps Lock", 0x3A, false, 0x14, 0x39);
        key("the key above Enter", 0x2B, false, 0xDC, 0x31);
        key("the key beside left Shift", 0x56, false, 0xE2, 0x64);
        key("/", 0x35, false, 0xBF, 0x38);
        key("/ on the number block", 0x35, true, 0x6F, 0x54);
        // Both of each: a KVM types the one that was pressed.
        key("left Ctrl", 0x1D, false, 0xA2, 0xE0);
        key("right Ctrl", 0x1D, true, 0xA3, 0xE4);
        key("left Shift", 0x2A, false, 0xA0, 0xE1);
        key("right Shift", 0x36, false, 0xA1, 0xE5);
        key("left Alt", 0x38, false, 0xA4, 0xE2);
        key("right Alt", 0x38, true, 0xA5, 0xE6);
        key("left Windows key", 0x5B, true, 0x5B, 0xE3);
        key("right Windows key", 0x5C, true, 0x5C, 0xE7);
        key("the menu key", 0x5D, true, 0x5D, 0x65);
        // The arrows and the block they were once part of.
        key("up", 0x48, true, 0x26, 0x52);
        key("8 on the number block", 0x48, false, 0x68, 0x60);
        key("down", 0x50, true, 0x28, 0x51);
        key("left", 0x4B, true, 0x25, 0x50);
        key("right", 0x4D, true, 0x27, 0x4F);
        key("Home", 0x47, true, 0x24, 0x4A);
        key("7 on the number block", 0x47, false, 0x67, 0x5F);
        key("End", 0x4F, true, 0x23, 0x4D);
        key("Page Up", 0x49, true, 0x21, 0x4B);
        key("Page Down", 0x51, true, 0x22, 0x4E);
        key("Insert", 0x52, true, 0x2D, 0x49);
        key("Delete", 0x53, true, 0x2E, 0x4C);
        key(". on the number block", 0x53, false, 0x6E, 0x63);
        key("* on the number block", 0x37, false, 0x6A, 0x55);
        key("+ on the number block", 0x4E, false, 0x6B, 0x57);
        // The row on top.
        key("F1", 0x3B, false, 0x70, 0x3A);
        key("F10", 0x44, false, 0x79, 0x43);
        key("F11", 0x57, false, 0x7A, 0x44);
        key("F12", 0x58, false, 0x7B, 0x45);
        key("F13", 0x64, false, 0x7C, 0x68);
        key("F24", 0x76, false, 0x87, 0x73);
        // The odd ones.
        key("Print Screen", 0x37, true, 0x2C, 0x46);
        key("Print Screen with Alt", 0x54, false, 0x2C, 0x46);
        key("Scroll Lock", 0x46, false, 0x91, 0x47);
        key("Pause, which is told as Num Lock's number without the mark", 0x45, false, 0x13, 0x48);
        key("Pause with Ctrl", 0x46, true, 0x03, 0x48);
        key("Num Lock, which is told with the mark", 0x45, true, 0x90, 0x53);
        key("Num Lock, were it told without", 0x45, false, 0x90, 0x53);
        // (Windows names Num Lock "Pause" while Ctrl is held. It is the key it is: sent as
        // Pause, the machine, which has Ctrl down, would make Break of it.)
        key("Num Lock under Ctrl, which Windows calls Pause", 0x45, true, 0x13, 0x53);
        key("yen", 0x7D, false, 0xDC, 0x89);
        key("ro", 0x73, false, 0xC1, 0x87);
    }

    void aPhysicalKeyIsItsUsbNumber()
    {
        QFETCH(quint32, scanCode);
        QFETCH(bool, extended);
        QFETCH(quint32, virtualKey);
        QFETCH(int, usage);
        QCOMPARE(int{usageOf({scanCode, extended, virtualKey})}, usage);
    }

    void whatNoFingerPressedIsNoKey()
    {
        // The left Ctrl that Windows puts before the right Alt of a layout with AltGr.
        QCOMPARE(int{usageOf({0x21D, false, 0xA2})}, 0);
        // A key Windows has no name for.
        QCOMPARE(int{usageOf({0x1E, false, 0xFF})}, 0);
        // Numbers no key has; and the keys for sound and the like, which are not the keyboard's.
        QCOMPARE(int{usageOf({0x5A, false, 0})}, 0);
        QCOMPARE(int{usageOf({0x1FF, false, 0})}, 0);
        QCOMPARE(int{usageOf({0x9E, false, 0x41})}, 0);
        QCOMPARE(int{usageOf({0xFFFFFFFF, true, 0})}, 0);
        QCOMPARE(int{usageOf({0x20, true, 0xAD})}, 0);
        QCOMPARE(int{usageOf({0, false, 0})}, 0);
    }

    void noTwoKeysAreTheSameUsbNumberButTheOnesThatAre()
    {
        std::map<int, std::vector<quint32>> keysOf;
        for (quint32 scanCode = 0; scanCode < 0x100; ++scanCode) {
            for (const bool extended : {false, true}) {
                if (const int usage = usageOf({scanCode, extended, 0}); usage != 0) {
                    keysOf[usage].push_back(scanCode | (extended ? 0x100U : 0U));
                }
            }
        }
        // Print Screen with and without Alt; Num Lock with and without the mark.
        const std::map<int, std::vector<quint32>> twice = {{0x46, {0x137, 0x54}}, {0x53, {0x45, 0x145}}};
        for (const auto& [usage, keys] : keysOf) {
            if (const auto known = twice.find(usage); known != twice.end()) {
                QCOMPARE(keys, known->second);
            } else {
                QVERIFY2(keys.size() == 1, qPrintable(QString::number(usage, 16)));
            }
        }
        // Every letter, digit and modifier of a keyboard is there: 4 to 0x39 without a gap but
        // the one USB keeps for the other key above Enter, and all eight modifiers.
        for (int usage = 0x04; usage <= 0x39; ++usage) {
            QVERIFY2(usage == 0x32 || keysOf.contains(usage), qPrintable(QString::number(usage, 16)));
        }
        for (int usage = 0xE0; usage <= 0xE7; ++usage) {
            QVERIFY2(keysOf.contains(usage), qPrintable(QString::number(usage, 16)));
        }
    }

    void aKeyIsSaidDownOnceAndUpOnlyIfItWasDown()
    {
        KeysDown down;
        QVERIFY(!down.any());
        QVERIFY(down.take(0x04, true));
        // Windows says a held key again and again: the far machine repeats by itself.
        QVERIFY(!down.take(0x04, true));
        QVERIFY(!down.take(0x04, true));
        QVERIFY(down.take(0xE1, true));
        QVERIFY(down.any());
        QVERIFY(down.take(0x04, false));
        QVERIFY(!down.take(0x04, false));
        // A key that was down before the machine was taken goes up here and nowhere else.
        QVERIFY(!down.take(0x2C, false));
        // What is no key is nothing.
        QVERIFY(!down.take(0, true));
        QVERIFY(down.any());
        // Everything was let go at the far end: the next press is a first one again.
        down.clear();
        QVERIFY(!down.any());
        QVERIFY(!down.take(0xE1, false));
        QVERIFY(down.take(0xE1, true));
    }

    void everyKeyGoesToTheMachineButTheLeaveKey()
    {
        using Does = ControlKeys::Does;
        using Step = ControlKeys::Step;
        ControlKeys keys;
        QCOMPARE(int{keys.leaveKey()}, 0xE4);
        QCOMPARE(keys.take(0x04, true), (Step{Does::Send, 0x04, true}));
        QCOMPARE(keys.take(0x04, false), (Step{Does::Send, 0x04, false}));
        // The left Ctrl is a key like any other, and so are the Windows key and Alt+Tab.
        QCOMPARE(keys.take(0xE0, true), (Step{Does::Send, 0xE0, true}));
        QCOMPARE(keys.take(0xE3, true), (Step{Does::Send, 0xE3, true}));
        QCOMPARE(keys.take(0xE3, false), (Step{Does::Send, 0xE3, false}));
        QCOMPARE(keys.take(0xE0, false), (Step{Does::Send, 0xE0, false}));
        // What is no key is nothing.
        QCOMPARE(keys.take(0, true), Step{});

        // The leave-key by itself: kept back going down, and the way out when it is let go.
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0xE4, true), Step{}); // held: said again
        QCOMPARE(keys.take(0xE4, false), (Step{Does::Leave, 0, false}));
        // Let go without having gone down while this was looked at: nothing.
        QCOMPARE(keys.take(0xE4, false), Step{});
    }

    void theLeaveKeyWithDelIsCtrlAltDel()
    {
        using Does = ControlKeys::Does;
        using Step = ControlKeys::Step;
        ControlKeys keys;
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0x4C, true), (Step{Does::CtrlAltDel, 0, false}));
        // Held, Del is said again: pressed once, it is once.
        QCOMPARE(keys.take(0x4C, true), Step{});
        // Neither key reaches the machine, going up either; and that was no leaving.
        QCOMPARE(keys.take(0x4C, false), Step{});
        QCOMPARE(keys.take(0xE4, false), Step{});
        // Del by itself is Del.
        QCOMPARE(keys.take(0x4C, true), (Step{Does::Send, 0x4C, true}));
        QCOMPARE(keys.take(0x4C, false), (Step{Does::Send, 0x4C, false}));
    }

    void anotherKeyWithTheLeaveKeyDoesNothingAndLeavesNothing()
    {
        using Does = ControlKeys::Does;
        using Step = ControlKeys::Step;
        ControlKeys keys;
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0x06, true), Step{});
        QCOMPARE(keys.take(0xE4, true), Step{}); // held: said again, and still used
        // The leave-key is let go first, the other key after it: its going up is kept back
        // too, since it never went down at the far end.
        QCOMPARE(keys.take(0xE4, false), Step{});
        QCOMPARE(keys.take(0x06, false), Step{});
        // After that the key is a key again.
        QCOMPARE(keys.take(0x06, true), (Step{Does::Send, 0x06, true}));
        QCOMPARE(keys.take(0x06, false), (Step{Does::Send, 0x06, false}));
        // A key that was down before the leave-key went down goes up at the far end, where it
        // is down.
        QCOMPARE(keys.take(0x07, true), (Step{Does::Send, 0x07, true}));
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0x07, false), (Step{Does::Send, 0x07, false}));
        QCOMPARE(keys.take(0xE4, false), (Step{Does::Leave, 0, false}));
    }

    void aKeyThatWentDownUnderTheLeaveKeyIsNeverSentAlsoNotByItsRepeat()
    {
        using Does = ControlKeys::Does;
        using Step = ControlKeys::Step;
        ControlKeys keys;
        // The right Ctrl used as a Ctrl: held with C for longer than a keyboard waits before it
        // says a key again, and let go first.
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0x06, true), Step{});
        QCOMPARE(keys.take(0x06, true), Step{});
        QCOMPARE(keys.take(0xE4, false), Step{});
        // C is still held, and is said again: it went down under the leave-key and is the
        // leave-key's until it was let go itself. Sent now, the machine would hold a bare "c".
        QCOMPARE(keys.take(0x06, true), Step{});
        QCOMPARE(keys.take(0x06, true), Step{});
        QCOMPARE(keys.take(0x06, false), Step{});
        // Let go, it is a key again.
        QCOMPARE(keys.take(0x06, true), (Step{Does::Send, 0x06, true}));
        QCOMPARE(keys.take(0x06, true), (Step{Does::Send, 0x06, true}));
        QCOMPARE(keys.take(0x06, false), (Step{Does::Send, 0x06, false}));

        // And so with the Del of Ctrl+Alt+Del: said again after the leave-key was let go, it is
        // no Del for the machine, and no second Ctrl+Alt+Del.
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0x4C, true), (Step{Does::CtrlAltDel, 0, false}));
        QCOMPARE(keys.take(0xE4, false), Step{});
        QCOMPARE(keys.take(0x4C, true), Step{});
        QCOMPARE(keys.take(0x4C, true), Step{});
        // The leave-key goes down once more while Del is still held: Del said again under it is
        // not pressed anew. And it is no key going down: the leave-key was pressed and let go
        // with no other key going down between, and that leaves. (Turned round: this recorded
        // that a key which is only said again uses the leave-key, so that it did not leave
        // while a key was held.)
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0x4C, true), Step{});
        QCOMPARE(keys.take(0xE4, false), (Step{Does::Leave, 0, false}));
        QCOMPARE(keys.take(0x4C, true), Step{});
        QCOMPARE(keys.take(0x4C, false), Step{});
        QCOMPARE(keys.take(0x4C, true), (Step{Does::Send, 0x4C, true}));
        QCOMPARE(keys.take(0x4C, false), (Step{Does::Send, 0x4C, false}));

        // Two keys under it, let go in any order: each is kept back until it was let go itself.
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0x28, true), Step{}); // Enter
        QCOMPARE(keys.take(0x2A, true), Step{}); // Backspace
        QCOMPARE(keys.take(0xE4, false), Step{});
        QCOMPARE(keys.take(0x2A, true), Step{});
        QCOMPARE(keys.take(0x28, false), Step{});
        QCOMPARE(keys.take(0x2A, true), Step{});
        QCOMPARE(keys.take(0x28, true), (Step{Does::Send, 0x28, true}));
        QCOMPARE(keys.take(0x2A, false), Step{});
        QCOMPARE(keys.take(0x28, false), (Step{Does::Send, 0x28, false}));
    }

    void aKeyThatIsOnlySaidAgainUsesTheLeaveKeyForNothing()
    {
        using Does = ControlKeys::Does;
        using Step = ControlKeys::Step;
        ControlKeys keys;
        // A is down at the far end. The leave-key goes down, and A is said again under it, as
        // Windows says a held key (a second keyboard goes on saying its key while the leave-key
        // is pressed on the first): that is no key going down. A stays the machine's, and the
        // leave-key, let go, leaves.
        QCOMPARE(keys.take(0x04, true), (Step{Does::Send, 0x04, true}));
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0x04, true, true), (Step{Does::Send, 0x04, true}));
        QCOMPARE(keys.take(0x04, true, true), (Step{Does::Send, 0x04, true}));
        QVERIFY(!keys.keepsBackAModifier());
        QVERIFY(keys.keptBack().empty());
        QCOMPARE(keys.take(0xE4, false), (Step{Does::Leave, 0, false}));
        // A key that goes down under it does use it, said again or not after that.
        QCOMPARE(keys.take(0xE4, true), Step{});
        QCOMPARE(keys.take(0xE1, true), Step{});
        QCOMPARE(keys.take(0xE1, true, true), Step{});
        QVERIFY(keys.keepsBackAModifier());
        QCOMPARE(keys.keptBack(), (std::set<std::uint16_t>{0xE1}));
        QCOMPARE(keys.take(0xE4, false), Step{});
        QCOMPARE(keys.take(0xE1, false), Step{});
        QVERIFY(keys.keptBack().empty());
    }

    void anotherLeaveKeyCanBeChosen()
    {
        using Does = ControlKeys::Does;
        using Step = ControlKeys::Step;
        ControlKeys keys(0x47); // Scroll Lock
        QCOMPARE(int{keys.leaveKey()}, 0x47);
        QCOMPARE(keys.take(0xE4, true), (Step{Does::Send, 0xE4, true}));
        QCOMPARE(keys.take(0xE4, false), (Step{Does::Send, 0xE4, false}));
        QCOMPARE(keys.take(0x47, true), Step{});
        QCOMPARE(keys.take(0x4C, true), (Step{Does::CtrlAltDel, 0, false}));
        QCOMPARE(keys.take(0x4C, false), Step{});
        QCOMPARE(keys.take(0x47, false), Step{});
        QCOMPARE(keys.take(0x47, true), Step{});
        QCOMPARE(keys.take(0x47, false), (Step{Does::Leave, 0, false}));
    }
};

QTEST_APPLESS_MAIN(TestKeys)

#include "test_keys.moc"
