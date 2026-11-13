#include <functional>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <qkvm/core/server_client.h>

#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

// A machine's virtual media through a QKVM server (0.3.0): its list, a mount, an unmount and a deletion, as the
// server's routes take them (FOR-THE-DESKTOP-APP, the 0.3.0 row; app/qkvm/server.py, media_*), and what comes back
// said as the server says it.
class TestServerClientMedia : public QObject {
    Q_OBJECT

private:
    static constexpr const char* aQkvm = R"({"status": "ok", "version": "0.3.0", "api": ["v1"]})";

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

    static QByteArray aList()
    {
        return R"({"images": [{"id": "debian-13.iso", "name": "debian-13.iso", "where": null, "size": 700000000,
                               "partial": false, "sha256": null}],
                   "unseen": [], "left_out": 0, "notes": [], "space": {"free": 9000000000, "size": 32000000000},
                   "mounted": null,
                   "offers": {"drives": ["cd", "flash"], "boot": [], "restart": true, "upload": true,
                              "where": "device", "writable": ["flash"], "delete": true, "drops_input": false},
                   "uploading": null})";
    }

    struct Wall {
        std::function<Answered(const Asked&)> media;
        StandIn server{[this](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, aQkvm};
            }
            return media ? media(asked) : Answered{404, R"({"code": "not_found", "message": "Unknown"})"};
        }};
        core::ServerClient client;
        Wall()
        {
            static_cast<void>(client.setAddress(server.address()));
            client.setSession(QByteArray(43, 's'));
        }
    };

private slots:
    void theImagesAndWhatIsMountedAreAskedOfTheMachine()
    {
        Wall wall;
        wall.media = [](const Asked&) { return Answered{200, aList()}; };
        const base::Result<core::MediaList> list =
            answerTo<core::MediaList>([&](const auto& done) { wall.client.askMedia(u"kvm 1"_s, done); });
        QVERIFY2(list.ok(), list.ok() ? "" : list.error().message.c_str());
        QCOMPARE(list.value().images.size(), 1);
        QCOMPARE(list.value().offers.where, u"device"_s);
        QCOMPARE(wall.server.asked.size(), 1);
        const Asked& asked = wall.server.asked.first();
        QCOMPARE(asked.method, QByteArray("GET"));
        // The machine by its id, written as an address writes it; with the session.
        QCOMPARE(asked.path, QByteArray("/api/v1/devices/kvm%201/media"));
        QCOMPARE(asked.headers.value("cookie"), "qkvm_session=" + QByteArray(43, 's'));
    }

    void aMountSendsTheImageAndTheMachineAsTheQuestionNamedThem()
    {
        Wall wall;
        wall.media = [](const Asked&) {
            return Answered{200, R"({"ok": true, "note": null, "mounted": {"drive": "flash", "image": "debian-13.iso",
                                     "name": "debian-13.iso", "read_only": false}})"};
        };
        core::MediaMount mount;
        mount.image = u"debian-13.iso"_s;
        mount.machine = u"Build server"_s;
        mount.drive = u"flash"_s;
        mount.writable = true;
        mount.boot = u"none"_s;
        mount.restart = true;
        const base::Result<core::MediaDone> done =
            answerTo<core::MediaDone>([&](const auto& told) { wall.client.mountMedia(u"kvm"_s, mount, told); });
        QVERIFY2(done.ok(), done.ok() ? "" : done.error().message.c_str());
        QVERIFY(done.value().mounted.has_value());
        QCOMPARE(wall.server.asked.size(), 1);
        const Asked& asked = wall.server.asked.first();
        QCOMPARE(asked.method, QByteArray("POST"));
        QCOMPARE(asked.path, QByteArray("/api/v1/devices/kvm/media"));
        QCOMPARE(asked.headers.value("x-qkvm"), QByteArray("1"));
        QCOMPARE(QJsonDocument::fromJson(asked.body).object(), QJsonObject({{u"image"_s, u"debian-13.iso"_s},
                                                                            {u"machine"_s, u"Build server"_s},
                                                                            {u"drive"_s, u"flash"_s},
                                                                            {u"writable"_s, true},
                                                                            {u"boot"_s, u"none"_s},
                                                                            {u"restart"_s, true}}));
    }

    void anUnmountAndADeletionNameTheImageAndTheMachine()
    {
        Wall wall;
        wall.media = [](const Asked&) { return Answered{200, R"({"ok": true, "note": "Nothing was mounted."})"}; };
        const base::Result<core::MediaDone> out = answerTo<core::MediaDone>(
            [&](const auto& told) { wall.client.unmountMedia(u"kvm"_s, u"debian-13.iso"_s, u"Build server"_s, told); });
        QVERIFY(out.ok());
        QCOMPARE(out.value().note, u"Nothing was mounted."_s);
        const base::Result<core::MediaDone> gone = answerTo<core::MediaDone>(
            [&](const auto& told) { wall.client.deleteMedia(u"kvm"_s, u"old.iso"_s, u"Build server"_s, told); });
        QVERIFY(gone.ok());
        QCOMPARE(wall.server.asked.size(), 2);
        QCOMPARE(wall.server.asked[0].path, QByteArray("/api/v1/devices/kvm/media/unmount"));
        QCOMPARE(wall.server.asked[1].path, QByteArray("/api/v1/devices/kvm/media/delete"));
        for (const Asked& asked : std::as_const(wall.server.asked)) {
            QCOMPARE(asked.method, QByteArray("POST"));
        }
        QCOMPARE(QJsonDocument::fromJson(wall.server.asked[0].body).object(),
                 QJsonObject({{u"image"_s, u"debian-13.iso"_s}, {u"machine"_s, u"Build server"_s}}));
        QCOMPARE(QJsonDocument::fromJson(wall.server.asked[1].body).object(),
                 QJsonObject({{u"image"_s, u"old.iso"_s}, {u"machine"_s, u"Build server"_s}}));
    }

    void whatTheServerRefusesIsSaidInItsWords_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("code");
        const QByteArray changed =
            R"({"code": "changed", "message": "The confirmation named Build server, and this machine is called Web now. Nothing was mounted: look at it again."})";
        QTest::newRow("another machine by then") << 409 << changed << "changed"_ba;
        QTest::newRow("a token")
            << 403 << R"({"code": "not_allowed", "message": "Virtual media is for a person, not for a token"})"_ba
            << "not_allowed"_ba;
        QTest::newRow("busy") << 409 << R"({"code": "busy", "message": "Something else is being done to it."})"_ba
                              << "busy"_ba;
        QTest::newRow("no virtual media")
            << 404 << R"({"code": "no_virtual_media", "message": "Build server has no virtual media"})"_ba
            << "no_virtual_media"_ba;
        // What came back to something that was to be done and is no word of the server's: it may have been done.
        QTest::newRow("a gateway's error")
            << 502 << "<html>Bad Gateway</html>"_ba << QByteArray(core::ServerClient::unreachable);
        QTest::newRow("a yes not understood")
            << 200 << "<html>OK</html>"_ba << QByteArray(core::ServerClient::unreachable);
        // And the server's own 500, an error inside it after which it may have been done (the merged review, 5).
        QTest::newRow("the server's own error")
            << 500
            << R"({"code": "internal", "message": "Something went wrong inside qKVM. The details are in its log."})"_ba
            << "internal"_ba;
        QTest::newRow("the server's own error in another word")
            << 500 << R"({"code": "failed", "message": "Something failed inside it."})"_ba << "internal"_ba;
    }

    void whatTheServerRefusesIsSaidInItsWords()
    {
        QFETCH(const int, status);
        QFETCH(const QByteArray, body);
        QFETCH(const QByteArray, code);
        Wall wall;
        wall.media = [&](const Asked&) { return Answered{status, body}; };
        core::MediaMount mount;
        mount.image = u"debian-13.iso"_s;
        mount.machine = u"Build server"_s;
        const base::Result<core::MediaDone> done =
            answerTo<core::MediaDone>([&](const auto& told) { wall.client.mountMedia(u"kvm"_s, mount, told); });
        QVERIFY(!done.ok());
        QCOMPARE(QByteArray::fromStdString(done.error().code), code);
        if (status < 500 && status != 200) {
            QCOMPARE(done.error().message,
                     QJsonDocument::fromJson(body).object().value(u"message"_s).toString().toStdString());
        }
        // A no where the server said one; whatever else came of it may have been done, and is not offered again.
        QCOMPARE(wall.client.mayHaveBeenDone(u"kvm"_s, done.error()), status >= 500 || status == 200);
        // Sent once, and nothing more after it.
        QCOMPARE(wall.server.count("/api/v1/devices/kvm/media"), 1);
    }
};

QTEST_MAIN(TestServerClientMedia)
#include "test_server_client_media.moc"
