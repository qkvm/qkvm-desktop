#include <utility>

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/server_types.h>
#include <qkvm/core/settings.h>

#include "files_view.h"
#include "memory_secrets.h"
#include "server_source.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

namespace {

// A QKVM as far as these tests go: one KVM on its wall, and whoever asks is signed in already as
// an admin, or nobody is.
Answered aQkvm(const Asked& asked, bool signedIn)
{
    if (asked.path == "/healthz") {
        return {200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"], "page": "3f2a9c", "locked": false})"};
    }
    if (asked.path == "/api/v1/session") {
        return {200, signedIn ? QByteArray(R"({"initialized": true, "authenticated": true, "ask_name": false,
                                    "user": {"id": "u1", "name": "anna", "role": "admin", "token": false}})")
                              : QByteArray(R"({"initialized": true, "authenticated": false, "ask_name": false,
                                    "user": null})")};
    }
    if (asked.path == "/api/v1/wall") {
        return {200, R"({"interval": 0.25, "devices": [
            {"id": "a", "name": "Build server", "host": "192.168.1.21", "kind": "kvmd", "picture": true}]})"};
    }
    if (asked.path == "/api/v1/status") {
        return {200, R"({"a": {"state": "online"}})"};
    }
    return {200, core::testing::aPicture(), "image/jpeg"};
}

// What a source says of itself, as "what happened: stage, whether somebody is at its wall, who,
// whether its feed runs", each at the moment it says it.
struct Heard {
    explicit Heard(ui::ServerSource& source) : m_source(source)
    {
        QObject::connect(&source, &ui::ServerSource::entered, &source,
                         [this](const core::Account& account) { note(u"entered "_s + account.name); });
        QObject::connect(&source, &ui::ServerSource::whoChanged, &source,
                         [this](const core::Account& account) { note(u"who "_s + account.name); });
        QObject::connect(&source, &ui::ServerSource::left, &source, [this] { note(u"left"_s); });
        QObject::connect(&source, &ui::ServerSource::emptied, &source, [this] { note(u"emptied"_s); });
        QObject::connect(&source, &ui::ServerSource::changed, &source, [this] { note(u"changed"_s); });
        QObject::connect(&source, &ui::ServerSource::stateChanged, &source, [this] { note(u"state"_s); });
    }

    void note(const QString& what)
    {
        const ui::SessionView::Shown shown = m_source.shown();
        said.append(u"%1: %2, %3, %4, %5"_s.arg(what, shown.stage, m_source.isAtTheWall() ? u"at"_s : u"away"_s,
                                                shown.account.isEmpty() ? u"nobody"_s : shown.account,
                                                m_source.feed().isRunning() ? u"runs"_s : u"stopped"_s));
    }

    QStringList said;

private:
    ui::ServerSource& m_source;
};

} // namespace

// One QKVM server's conversation, as a class by itself: what it holds that the session around it
// does not show.
class TestServerSource : public QObject {
    Q_OBJECT

private slots:
    void whatHappensIsSaidAtTheMomentItHappensAndInTheOrderItHappens()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const StandIn server([](const Asked& asked) { return aQkvm(asked, true); });
        ui::ServerSource source(u"a1b2c3d4"_s, settings, secrets, 100);
        Heard heard(source);
        QCOMPARE(source.id(), u"a1b2c3d4"_s);

        // Named, and come to: whoever is at its wall is said before the state that follows is
        // shown, and the feed starts after that; what is shown is said before the state is.
        source.connectTo(server.address());
        QTRY_VERIFY(source.feed().isRunning());
        QCOMPARE(heard.said,
                 (QStringList{u"changed: address, away, nobody, stopped"_s, u"state: address, away, nobody, stopped"_s,
                              u"entered anna: address, away, anna, stopped"_s, u"changed: wall, at, anna, stopped"_s,
                              u"state: wall, at, anna, stopped"_s}));
        QTRY_COMPARE(source.feed().machines().size(), 1);
        QVERIFY(source.mayBeLive());
        QVERIFY(source.manages());

        // Left: nobody is at the wall from the moment its feed has stopped, while who was there is
        // still known; what was shown of it goes once that is forgotten; and then the state.
        heard.said.clear();
        source.leave();
        QCOMPARE(heard.said, (QStringList{u"left: wall, at, anna, stopped"_s, u"emptied: wall, at, nobody, stopped"_s,
                                          u"changed: address, away, nobody, stopped"_s,
                                          u"state: address, away, nobody, stopped"_s}));
        QVERIFY(!source.mayBeLive());
        QVERIFY(!source.manages());
        // (Its feed still knows the machines it had: what the windows had of them, they let go of.)
        QCOMPARE(source.feed().machines().size(), 1);
    }

    void itsTurnRisesWithEveryQuestionWhereItsWallIsLeftAndWhereItStandsAside()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const StandIn server([](const Asked& asked) { return aQkvm(asked, true); });
        ui::ServerSource source(u"a1b2c3d4"_s, settings, secrets, 100);
        quint64 turn = source.turn();
        const auto rose = [&] {
            const bool did = source.turn() > turn;
            turn = source.turn();
            return did;
        };

        // What answers there is asked, and then who is signed in: a turn each.
        source.connectTo(server.address());
        QVERIFY(rose());
        QTRY_VERIFY(source.isAtTheWall());
        QVERIFY(rose());
        // The looks at the machines are not its questions, and neither is the asking who it is now
        // that comes with every fifteenth of them (as the windows' wall tells it of each).
        for (int look = 0; look < 15; ++look) {
            source.looked();
        }
        QTRY_COMPARE(server.count("/api/v1/session"), 2);
        QVERIFY(!rose());

        // Its wall is left, back to saying which server: whatever is out is nobody's. (Leaving the
        // wall asks nothing, so it is the leaving itself that raised it.)
        source.leave();
        QVERIFY(rose());
        QVERIFY(!source.isAtTheWall());
        // The same where it is left by signing out there.
        source.connectTo(server.address());
        QVERIFY(rose());
        QTRY_VERIFY(source.isAtTheWall());
        QVERIFY(rose());
        source.signOut();
        QVERIFY(rose());
        QVERIFY(!source.isAtTheWall());
        QCOMPARE(source.shown().stage, u"signIn"_s);

        // Standing aside for this PC's own wall: the wall is left, and whatever is out is nobody's.
        // (Back to saying which server first, from the line for a password: that ends a turn too.)
        source.leave();
        QVERIFY(rose());
        source.connectTo(server.address());
        QVERIFY(rose());
        QTRY_VERIFY(source.isAtTheWall());
        QVERIFY(rose());
        source.standAside();
        QVERIFY(rose());
        QVERIFY(!source.isAtTheWall());
        QVERIFY(!source.feed().isRunning());
        QCOMPARE(source.shown().stage, u"address"_s);
        QCOMPARE(source.shown().problem, QString());
        // Also where no wall was up: a question that was out is nobody's either.
        source.connectTo(server.address());
        QVERIFY(rose());
        source.standAside();
        QVERIFY(rose());
        QTest::qWait(300);
        QVERIFY(!source.isAtTheWall());
        QCOMPARE(source.shown().stage, u"address"_s);
        QVERIFY(!source.shown().busy);
        QVERIFY(!rose());
    }

    void twoSourcesAtOnceEachHaveTheirOwnConversation()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const StandIn signedIn([](const Asked& asked) { return aQkvm(asked, true); });
        const StandIn asksForAPassword([](const Asked& asked) { return aQkvm(asked, false); });
        ui::ServerSource first(u"a1b2c3d4"_s, settings, secrets, 100);
        ui::ServerSource second(u"e5f6a7b8"_s, settings, secrets, 100);
        first.connectTo(signedIn.address());
        second.connectTo(asksForAPassword.address());
        QTRY_VERIFY(first.feed().isRunning());
        QTRY_COMPARE(second.shown().stage, u"signIn"_s);
        QCOMPARE(first.shown().stage, u"wall"_s);
        QCOMPARE(first.shown().address, u"127.0.0.1:%1"_s.arg(signedIn.serverPort()));
        QCOMPARE(second.shown().address, u"127.0.0.1:%1"_s.arg(asksForAPassword.serverPort()));

        // One that leaves its wall leaves the other as it is, its turn too; and the other's
        // feed asks the other's server alone.
        const quint64 secondsTurn = second.turn();
        first.leave();
        QCOMPARE(second.shown().stage, u"signIn"_s);
        QCOMPARE(second.turn(), secondsTurn);
        QVERIFY(!second.feed().isRunning());
        QCOMPARE(asksForAPassword.count("/api/v1/status"), 0);
        QCOMPARE(asksForAPassword.count("/api/v1/wall"), 0);
    }

    void whatWasTypedForAFileGoesNowhereWhereTheWallWasComeToAgainByItsLinkMeanwhile()
    {
        // A screen signed in by its display link asks for the wall as a file, with a password;
        // before that goes, what answers at the address is asked what it is, slowly. Meanwhile
        // the server forgets the session, and the screen is back at its wall by its link: the
        // password goes nowhere. (The session's own test of this asks it of a program that is a
        // wall screen, which asks nothing of the wall as a file since R4b: so it is asked here of
        // the conversation by itself.)
        bool signedIn = false;
        bool slowOnce = false;
        int displays = 0;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/api/v1/display") {
                signedIn = true;
                ++displays;
                return Answered{
                    200,
                    R"({"ok": true, "user": {"id": "token:t1", "name": "Lobby screen", "role": "admin", "token": true}})"};
            }
            if (!signedIn && asked.path != "/healthz" && asked.path != "/api/v1/session") {
                return Answered{401, R"({"code": "unauthorized", "message": "Sign in first"})"};
            }
            Answered answer = aQkvm(asked, signedIn);
            if (asked.path == "/healthz" && std::exchange(slowOnce, false)) {
                answer.afterMs = 700;
            }
            return answer;
        });
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        ui::ServerSource source(u"a1b2c3d4"_s, settings, secrets, 100);
        ui::FilesView files;
        const QString keptAt = folder.filePath(u"wall.qkvm"_s);
        files.setDoor({[keptAt](const QString&) { return base::Result<QString>(keptAt); },
                       [] { return base::Result<QString>(QString()); }});
        const QPointer<ui::FilesView> asked(&files);
        QObject::connect(&files, &ui::FilesView::exportAsked, &source,
                         [&](quint64 turn, const QStringList& parts, const QString& passphrase,
                             const QString& password) { source.exportWall(asked, turn, parts, passphrase, password); });
        source.connectTo(server.address() + u"/#display&key=qkvm_"_s + QString(43, u'd'));
        QTRY_COMPARE(source.feed().machines().size(), 1);
        QTRY_VERIFY(server.count("/api/v1/status") >= 2);
        slowOnce = true;
        files.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"the right one"_s);
        signedIn = false;
        QTRY_COMPARE(displays, 2);
        QTRY_VERIFY(!files.isBusy());
        QCOMPARE(files.problem(), u"The wall was left before this could be sent. Nothing was sent."_s);
        QTest::qWait(300);
        QCOMPARE(server.count("/api/v1/export"), 0);
        for (const Asked& one : std::as_const(server.asked)) {
            QVERIFY2(!one.body.contains("the right one") && !one.body.contains("a passphrase of mine"),
                     one.path.constData());
        }
        source.feed().stop();
    }
};

QTEST_GUILESS_MAIN(TestServerSource)
#include "test_server_source.moc"
