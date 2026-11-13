#include <functional>

#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <qkvm/core/settings.h>

#include "look.h"
#include "win/display_awake.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

// How the program looks, as far as it is chosen: the theme; and the display kept awake while a
// window fills a screen.
class TestLook : public QObject {
    Q_OBJECT

private:
    // What the application was told last, in place of Qt: its platform without a screen, which the
    // tests run on, takes no telling.
    Qt::ColorScheme m_applied = Qt::ColorScheme::Unknown;
    int m_told = 0;

    std::function<void(Qt::ColorScheme)> noting()
    {
        return [this](Qt::ColorScheme scheme) {
            m_applied = scheme;
            ++m_told;
        };
    }

    Qt::ColorScheme applied() const { return m_applied; }

private slots:
    void aPcThatNeverChoseIsDark()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        m_applied = Qt::ColorScheme::Light;
        const ui::Look look(settings, noting());
        QCOMPARE(look.theme(), u"dark"_s);
        QCOMPARE(applied(), Qt::ColorScheme::Dark);
        QCOMPARE(look.themeSymbol(), u"moon"_s);
        QCOMPARE(look.themeHint(), u"Dark theme (click for light theme)"_s);
    }

    void theButtonGoesFromOneThemeToTheNextAndRoundAgain()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        ui::Look look(settings, noting());
        const QSignalSpy changed(&look, &ui::Look::themeChanged);

        look.nextTheme();
        QCOMPARE(look.theme(), u"light"_s);
        QCOMPARE(applied(), Qt::ColorScheme::Light);
        QCOMPARE(look.themeSymbol(), u"sun"_s);
        QCOMPARE(look.themeHint(), u"Light theme (click for system theme)"_s);
        // Written down when it is chosen.
        QCOMPARE(core::Settings(file).remembered().theme, u"light"_s);

        // As Windows is set: Qt is told "none of the two", and follows Windows.
        look.nextTheme();
        QCOMPARE(look.theme(), u"system"_s);
        QCOMPARE(applied(), Qt::ColorScheme::Unknown);
        QCOMPARE(look.themeSymbol(), u"system"_s);
        QCOMPARE(look.themeHint(), u"System theme (click for dark theme)"_s);
        QCOMPARE(core::Settings(file).remembered().theme, u"system"_s);

        look.nextTheme();
        QCOMPARE(look.theme(), u"dark"_s);
        QCOMPARE(applied(), Qt::ColorScheme::Dark);
        QCOMPARE(core::Settings(file).remembered().theme, u"dark"_s);
        QCOMPARE(changed.count(), 3);
    }

    void theThemeThatWasChosenIsTheThemeAtTheNextStart()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        {
            core::Settings settings(file);
            ui::Look look(settings, noting());
            look.nextTheme();
        }
        m_applied = Qt::ColorScheme::Dark;
        const int toldBefore = m_told;
        core::Settings settings(file);
        const ui::Look look(settings, noting());
        // Told at the start, once.
        QCOMPARE(m_told, toldBefore + 1);
        QCOMPARE(look.theme(), u"light"_s);
        QCOMPARE(applied(), Qt::ColorScheme::Light);
        // And nothing else that is remembered was lost by writing it down.
        core::Remembered more = settings.remembered();
        more.server = u"192.168.1.10:8787"_s;
        QVERIFY(settings.remember(more).ok());
        ui::Look again(settings, noting());
        again.nextTheme();
        QCOMPARE(core::Settings(file).remembered().server, u"192.168.1.10:8787"_s);
        QCOMPARE(core::Settings(file).remembered().theme, u"system"_s);
    }

    void aThemeIsChosenByItsNameToo()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        ui::Look look(settings, noting());
        const QSignalSpy changed(&look, &ui::Look::themeChanged);
        const int toldBefore = m_told;

        look.choose(u"system"_s);
        QCOMPARE(look.theme(), u"system"_s);
        QCOMPARE(applied(), Qt::ColorScheme::Unknown);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(core::Settings(file).remembered().theme, u"system"_s);
        look.choose(u"light"_s);
        QCOMPARE(look.theme(), u"light"_s);
        QCOMPARE(applied(), Qt::ColorScheme::Light);
        QCOMPARE(core::Settings(file).remembered().theme, u"light"_s);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(m_told, toldBefore + 2);

        // The one that is chosen already, and what is none of the three: nothing happens.
        look.choose(u"light"_s);
        look.choose(u"pink"_s);
        look.choose(QString());
        QCOMPARE(look.theme(), u"light"_s);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(m_told, toldBefore + 2);
        QCOMPARE(core::Settings(file).remembered().theme, u"light"_s);
    }

    void theDisplayIsKeptAwakeWhileAWindowFillsAScreenAndNoLonger()
    {
        QWindow window;
        QList<bool> asked;
        ui::AwakeInFullScreen awake([&](bool on) { asked.append(on); });
        awake.watch(window);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // An ordinary window keeps nobody awake.
        QVERIFY(asked.isEmpty());
        window.showFullScreen();
        QTRY_COMPARE(asked, QList<bool>{true});
        // Still in full screen: not asked a second time.
        window.setTitle(u"still"_s);
        QTest::qWait(100);
        QCOMPARE(asked.size(), 1);
        window.showNormal();
        QTRY_COMPARE(asked, (QList<bool>{true, false}));
        window.showMaximized();
        QTest::qWait(100);
        QCOMPARE(asked.size(), 2);
        window.showFullScreen();
        QTRY_COMPARE(asked.size(), 3);
        // Hidden or closed while it filled the screen: let go.
        window.hide();
        QTRY_COMPARE(asked, (QList<bool>{true, false, true, false}));
    }

    void ofSeveralWindowsOneThatFillsAScreenKeepsTheDisplayAwake()
    {
        QWindow first;
        QWindow second;
        QList<bool> asked;
        {
            ui::AwakeInFullScreen awake([&](bool on) { asked.append(on); });
            awake.watch(first);
            awake.watch(second);
            first.show();
            second.show();
            QVERIFY(QTest::qWaitForWindowExposed(&second));
            QVERIFY(asked.isEmpty());
            first.showFullScreen();
            QTRY_COMPARE(asked, QList<bool>{true});
            second.showFullScreen();
            QTRY_COMPARE(second.visibility(), QWindow::FullScreen);
            QCOMPARE(asked.size(), 1);
            // One of them leaves full screen: the other still fills its screen, and is looked at
            // without anybody touching the PC.
            second.showNormal();
            QTRY_COMPARE(second.visibility(), QWindow::Windowed);
            QCOMPARE(asked.size(), 1);
            // The one that fills a screen is closed, and gone: let go.
            awake.forget(first);
            QCOMPARE(asked, (QList<bool>{true, false}));
            // What it does from now on is nobody's.
            first.showNormal();
            first.showFullScreen();
            QTRY_COMPARE(first.visibility(), QWindow::FullScreen);
            QCOMPARE(asked.size(), 2);
            // A window that fills a screen already when it is first watched holds the display.
            awake.watch(first);
            QCOMPARE(asked, (QList<bool>{true, false, true}));
        }
        // The program ends while a window fills a screen: let go.
        QCOMPARE(asked, (QList<bool>{true, false, true, false}));
    }

    void windowsIsToldToKeepTheDisplayAwake()
    {
        // Asked of Windows itself. That it lets go again cannot be seen from here: another program
        // may be holding the display too.
        ui::keepDisplayAwake(true);
        QVERIFY(ui::displayIsKeptAwake());
        ui::keepDisplayAwake(false);
    }
};

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestLook test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_look.moc"
