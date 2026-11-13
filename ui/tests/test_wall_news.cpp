#include <utility>

#include <QTest>

#include "wall_model.h"
#include "wall_news.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// A wall of machines given as "key name state": what its card would say of the state in a word
// is the state in capitals, so that a test can tell that word from the name.
QList<ui::WallTile> wallOf(const QStringList& machines)
{
    QList<ui::WallTile> tiles;
    for (const QString& machine : machines) {
        const QStringList parts = machine.split(u' ');
        ui::WallTile tile;
        tile.key = parts.value(0);
        tile.name = parts.value(1);
        tile.state = parts.value(2);
        tile.label = parts.value(2).toUpper();
        tiles.append(tile);
    }
    return tiles;
}

// What is news after the wall was like each of these, one after the other, with nobody told in
// between: "away: a b; back: c".
QString newsOver(const QList<QStringList>& walls)
{
    ui::WallModel model;
    ui::WallLook known;
    ui::WallNews news;
    for (const QStringList& wall : walls) {
        model.setTiles(wallOf(wall));
        ui::add(news, ui::newsBetween(known, model));
        ui::remember(known, model);
    }
    QStringList away;
    QStringList back;
    for (const ui::Told& told : std::as_const(news.away)) {
        away.append(told.key);
    }
    for (const ui::Told& told : std::as_const(news.back)) {
        back.append(told.key);
    }
    return u"away: %1; back: %2"_s.arg(away.join(u' '), back.join(u' '));
}

ui::Told told(const QString& key, const QString& label = u"OFFLINE"_s)
{
    return {key, u"The "_s + key, label};
}

} // namespace

// What is told of the wall to somebody who is not looking at it.
class TestWallNews : public QObject {
    Q_OBJECT

private slots:
    void whatIsKnownOfAMachineIsHowTheBarCountedItLast()
    {
        ui::WallModel model;
        model.setTiles(wallOf({u"a NAS online"_s, u"b Build offline"_s, u"c Lab no_signal"_s, u"d New connecting"_s,
                               u"e Locked auth"_s}));
        ui::WallLook known;
        ui::remember(known, model);
        // Of a machine that is only being connected to, nothing is known yet.
        QCOMPARE(known.size(), 4);
        QCOMPARE(known.value(u"a"_s), (ui::Looked{u"online"_s, u"NAS"_s, u"ONLINE"_s}));
        QCOMPARE(known.value(u"b"_s), (ui::Looked{u"attention"_s, u"Build"_s, u"OFFLINE"_s}));
        QCOMPARE(known.value(u"c"_s), (ui::Looked{u"no_signal"_s, u"Lab"_s, u"NO_SIGNAL"_s}));
        QCOMPARE(known.value(u"e"_s), (ui::Looked{u"attention"_s, u"Locked"_s, u"AUTH"_s}));

        // Being connected to again, a machine keeps what was known of it; one that left the wall
        // is forgotten; and one that is some way now is known that way.
        model.setTiles(wallOf({u"a NAS connecting"_s, u"c Lab online"_s, u"d New offline"_s}));
        ui::remember(known, model);
        QCOMPARE(known.size(), 3);
        QCOMPARE(known.value(u"a"_s), (ui::Looked{u"online"_s, u"NAS"_s, u"ONLINE"_s}));
        QCOMPARE(known.value(u"c"_s), (ui::Looked{u"online"_s, u"Lab"_s, u"ONLINE"_s}));
        QCOMPARE(known.value(u"d"_s), (ui::Looked{u"attention"_s, u"New"_s, u"OFFLINE"_s}));
    }

    void whatIsNewsAndWhatIsNot_data()
    {
        QTest::addColumn<QList<QStringList>>("walls");
        QTest::addColumn<QString>("news");
        const auto row = [](const char* name, const QList<QStringList>& walls, const QString& news) {
            QTest::newRow(name) << walls << news;
        };
        row("nothing changed", {{u"a A online"_s, u"b B offline"_s}, {u"a A online"_s, u"b B offline"_s}},
            u"away: ; back: "_s);
        row("a machine goes away", {{u"a A online"_s}, {u"a A offline"_s}}, u"away: a; back: "_s);
        row("a machine comes back", {{u"a A offline"_s}, {u"a A online"_s}}, u"away: ; back: a"_s);
        row("its KVM is back and its computer is off", {{u"a A offline"_s}, {u"a A no_signal"_s}},
            u"away: ; back: a"_s);
        row("a computer goes to sleep", {{u"a A online"_s}, {u"a A no_signal"_s}}, u"away: ; back: "_s);
        row("a computer wakes up", {{u"a A no_signal"_s}, {u"a A online"_s}}, u"away: ; back: "_s);
        row("a sleeping computer's KVM goes away", {{u"a A no_signal"_s}, {u"a A error"_s}}, u"away: a; back: "_s);
        row("wrong in another way than before", {{u"a A offline"_s}, {u"a A auth"_s}}, u"away: ; back: "_s);
        row("a machine is new on the wall, and is away", {{u"a A online"_s}, {u"a A online"_s, u"b B offline"_s}},
            u"away: ; back: "_s);
        row("a machine is taken off the wall", {{u"a A online"_s, u"b B offline"_s}, {u"a A online"_s}},
            u"away: ; back: "_s);
        row("several at once, in the wall's order",
            {{u"a A online"_s, u"b B offline"_s, u"c C online"_s, u"d D offline"_s, u"e E online"_s},
             {u"e E offline"_s, u"d D online"_s, u"c C offline"_s, u"b B online"_s, u"a A online"_s}},
            u"away: e c; back: d b"_s);
        row("the wall is filled for the first time", {{}, {u"a A offline"_s, u"b B online"_s}}, u"away: ; back: "_s);
        // Of a machine that is being connected to, nothing is said until it is some way again.
        row("the first that is known of a machine is that it cannot be reached",
            {{u"a A connecting"_s}, {u"a A offline"_s}}, u"away: ; back: "_s);
        row("the first that is known of a machine is that it is there", {{u"a A connecting"_s}, {u"a A online"_s}},
            u"away: ; back: "_s);
        row("a machine is being connected to again",
            {{u"a A online"_s, u"b B offline"_s}, {u"a A connecting"_s, u"b B connecting"_s}}, u"away: ; back: "_s);
        // As after QKVM was started again: every machine is connected to anew, and is then what
        // it was, or is not.
        row("connected to anew, and as before",
            {{u"a A online"_s, u"b B offline"_s},
             {u"a A connecting"_s, u"b B connecting"_s},
             {u"a A online"_s, u"b B offline"_s}},
            u"away: ; back: "_s);
        row("connected to anew, and not as before",
            {{u"a A online"_s, u"b B offline"_s},
             {u"a A connecting"_s, u"b B connecting"_s},
             {u"a A offline"_s, u"b B online"_s}},
            u"away: a; back: b"_s);
        // Before anybody was told.
        row("away and back again", {{u"a A online"_s}, {u"a A offline"_s}, {u"a A online"_s}}, u"away: ; back: "_s);
        row("back and away again", {{u"a A offline"_s}, {u"a A online"_s}, {u"a A offline"_s}}, u"away: ; back: "_s);
        row("away, back and away", {{u"a A online"_s}, {u"a A offline"_s}, {u"a A online"_s}, {u"a A error"_s}},
            u"away: a; back: "_s);
    }

    void whatIsNewsAndWhatIsNot()
    {
        QFETCH(QList<QStringList>, walls);
        QFETCH(QString, news);
        QCOMPARE(newsOver(walls), news);
    }

    void newsNamesTheMachineAndSaysHowItIsNow()
    {
        ui::WallModel model;
        model.setTiles(wallOf({u"a NAS online"_s, u"b Build offline"_s}));
        ui::WallLook known;
        ui::remember(known, model);
        model.setTiles(wallOf({u"a NAS auth"_s, u"b Build no_signal"_s}));
        const ui::WallNews news = ui::newsBetween(known, model);
        QCOMPARE(news.away, QList<ui::Told>{(ui::Told{u"a"_s, u"NAS"_s, u"AUTH"_s})});
        QCOMPARE(news.back, QList<ui::Told>{(ui::Told{u"b"_s, u"Build"_s, u"NO_SIGNAL"_s})});
    }

    void newsThatNobodyWasToldYetIsPutTogether()
    {
        ui::WallNews news;
        QVERIFY(news.isEmpty());
        ui::add(news, {{told(u"a"_s)}, {}});
        ui::add(news, {{told(u"b"_s)}, {told(u"c"_s, u"ONLINE"_s)}});
        QCOMPARE(news.away, (QList<ui::Told>{told(u"a"_s), told(u"b"_s)}));
        QCOMPARE(news.back, QList<ui::Told>{told(u"c"_s, u"ONLINE"_s)});
        QVERIFY(!news.isEmpty());

        // Away and back again before anybody was told: no news. And the other way round.
        ui::add(news, {{}, {told(u"a"_s, u"ONLINE"_s)}});
        QCOMPARE(news.away, QList<ui::Told>{told(u"b"_s)});
        QCOMPARE(news.back, QList<ui::Told>{told(u"c"_s, u"ONLINE"_s)});
        ui::add(news, {{told(u"c"_s)}, {}});
        QCOMPARE(news.away, QList<ui::Told>{told(u"b"_s)});
        QVERIFY(news.back.isEmpty());
        // The same news twice is news once.
        ui::add(news, {{told(u"b"_s)}, {}});
        QCOMPARE(news.away, QList<ui::Told>{told(u"b"_s)});
        ui::add(news, {{}, {told(u"d"_s, u"ONLINE"_s)}});
        ui::add(news, {{}, {told(u"d"_s, u"ONLINE"_s)}});
        QCOMPARE(news.back, QList<ui::Told>{told(u"d"_s, u"ONLINE"_s)});
    }

    void newsIsSaidAsWindowsShowsIt_data()
    {
        QTest::addColumn<QStringList>("away");
        QTest::addColumn<QStringList>("back");
        QTest::addColumn<QString>("title");
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("warning");
        QTest::newRow("one machine away")
            << QStringList{u"NAS"_s} << QStringList() << u"NAS needs attention"_s << u"Offline"_s << true;
        QTest::newRow("several away") << QStringList{u"NAS"_s, u"Build server"_s, u"Lab"_s} << QStringList()
                                      << u"3 KVMs need attention"_s << u"NAS, Build server, Lab"_s << true;
        QTest::newRow("one machine back")
            << QStringList() << QStringList{u"NAS"_s} << u"NAS is back"_s << u"Online"_s << false;
        QTest::newRow("several back") << QStringList() << QStringList{u"NAS"_s, u"Lab"_s} << u"2 KVMs are back"_s
                                      << u"NAS, Lab"_s << false;
        QTest::newRow("some away and some back")
            << QStringList{u"NAS"_s, u"Lab"_s} << QStringList{u"Build server"_s} << u"2 need attention, 1 back"_s
            << u"Need attention: NAS, Lab. Back: Build server."_s << true;
    }

    void newsIsSaidAsWindowsShowsIt()
    {
        QFETCH(QStringList, away);
        QFETCH(QStringList, back);
        QFETCH(QString, title);
        QFETCH(QString, text);
        QFETCH(bool, warning);
        ui::WallNews news;
        for (const QString& name : std::as_const(away)) {
            news.away.append({name, name, u"Offline"_s});
        }
        for (const QString& name : std::as_const(back)) {
            news.back.append({name, name, u"Online"_s});
        }
        const std::optional<ui::Message> message = ui::messageOf(news);
        QVERIFY(message.has_value());
        QCOMPARE(message.value_or(ui::Message{}), (ui::Message{title, text, warning}));
    }

    void noNewsIsNotSaid() { QVERIFY(!ui::messageOf({}).has_value()); }

    void aLongListOfNamesEndsWhereWindowsHasNoMoreRoom()
    {
        ui::WallNews news;
        for (int at = 1; at <= 64; ++at) {
            news.away.append({u"k%1"_s.arg(at), u"Rack server %1"_s.arg(at), u"Offline"_s});
        }
        const ui::Message many = ui::messageOf(news).value_or(ui::Message{});
        QCOMPARE(many.title, u"64 KVMs need attention"_s);
        QVERIFY2(many.text.startsWith(u"Rack server 1, Rack server 2, "_s), qPrintable(many.text));
        QVERIFY2(many.text.size() <= 250, qPrintable(many.text));
        // How many more is how many are not named.
        const qsizetype namedOnes = many.text.count(u"Rack server"_s);
        QVERIFY(namedOnes >= 10);
        QVERIFY2(many.text.endsWith(u" and %1 more"_s.arg(64 - namedOnes)), qPrintable(many.text));

        // With some back as well, the two lists share the room.
        for (int at = 1; at <= 64; ++at) {
            news.back.append({u"b%1"_s.arg(at), u"Office PC %1"_s.arg(at), u"Online"_s});
        }
        const ui::Message both = ui::messageOf(news).value_or(ui::Message{});
        QCOMPARE(both.title, u"64 need attention, 64 back"_s);
        QVERIFY2(both.text.size() <= 250, qPrintable(both.text));
        QVERIFY(both.text.contains(u"Rack server 1"_s));
        QVERIFY(both.text.contains(u"Office PC 1"_s));

        // A name that is longer than a heading has room for is cut, and says that it was.
        ui::WallNews one;
        one.away.append({u"k"_s, QString(120, u'n'), u"Offline"_s});
        const ui::Message longName = ui::messageOf(one).value_or(ui::Message{});
        QVERIFY2(longName.title.size() <= 63, qPrintable(longName.title));
        QVERIFY(longName.title.startsWith(u"nnnnnnnn"_s));
        QVERIFY(longName.title.endsWith(u"… needs attention"_s));
        one.back = std::exchange(one.away, {});
        const ui::Message longBack = ui::messageOf(one).value_or(ui::Message{});
        QVERIFY2(longBack.title.size() <= 63, qPrintable(longBack.title));
        QVERIFY(longBack.title.endsWith(u"… is back"_s));
    }
};

QTEST_GUILESS_MAIN(TestWallNews)
#include "test_wall_news.moc"
