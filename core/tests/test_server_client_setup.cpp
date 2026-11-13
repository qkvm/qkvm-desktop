#include <QElapsedTimer>
#include <QTest>

#include <qkvm/core/server_client.h>

#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

// "Start into BIOS setup" over a slow controller (the web's bios_setup): the server goes no further than the server's
// restart after two minutes, and finishes what is under way before it answers, so the page waits four minutes for it
// (SETUP_PATIENCE), where an action is waited for two and a half. This one waits out more than the action's time, and
// has a program of its own beside test_core_server_client.
class TestServerClientSetup : public QObject {
    Q_OBJECT

private slots:
    void theBiosSetupIsGivenFourMinutesToBeAnswered()
    {
        StandIn server([](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, R"({"status": "ok", "version": "0.3.0", "api": ["v1"]})"};
            }
            // A controller that is slow to come to its restart: nothing is said for longer than an action is given.
            Answered late{200, R"({"ok": true, "note": null})"};
            late.afterMs = 160'000;
            return late;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        core::Action setup = core::Action::plain(u"bios_setup"_s);
        setup.watching = true;
        QList<base::Result<core::Done>> answers;
        QElapsedTimer took;
        took.start();
        client.act(u"rack"_s, setup, [&](const base::Result<core::Done>& done) { answers.append(done); });
        QVERIFY(QTest::qWaitFor([&] { return !answers.isEmpty(); }, 250'000));
        QVERIFY2(answers.constFirst().ok(),
                 answers.constFirst().ok() ? "" : answers.constFirst().error().message.c_str());
        QVERIFY2(took.elapsed() >= 159'000, qPrintable(QString::number(took.elapsed())));
        QCOMPARE(server.count("/api/v1/devices/rack/actions"), 1);
    }
};

QTEST_GUILESS_MAIN(TestServerClientSetup)
#include "test_server_client_setup.moc"
