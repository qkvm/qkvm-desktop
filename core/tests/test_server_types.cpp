#include <string>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <qkvm/core/server_types.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

// The server's answers read into types: what a real server sends is taken, and what is not of that
// shape is refused whole.
class TestServerTypes : public QObject {
    Q_OBJECT

private:
    // What a QKVM says of itself, with one thing in it as a test wants it.
    static QByteArray healthWith(const QString& field, const QJsonValue& value)
    {
        QJsonObject said{{u"status"_s, u"ok"_s}, {u"version"_s, u"0.1.0"_s}, {u"api"_s, QJsonArray{u"v1"_s}}};
        said[field] = value;
        return QJsonDocument(said).toJson(QJsonDocument::Compact);
    }

private slots:
    void capabilitiesAreBoundedAndMalformedListsGrantNothing()
    {
        for (const QByteArray& value : {QByteArray("null"), QByteArray("true"), QByteArray("\"text\""),
                                        QByteArray("[\"text\",4]"), QByteArray("[\"\"]")}) {
            const auto wall = core::wallFrom("{\"devices\":[{\"id\":\"one\",\"can\":" + value + "}]}");
            QVERIFY(wall);
            QVERIFY(wall.value().machines.first().capabilities.has_value());
            QVERIFY(!core::supports(wall.value().machines.first(), nullptr, u"text"_s));
        }
        const auto wall = core::wallFrom(R"({"devices":[{"id":"one","can":["text","text","future"]}]})");
        QVERIFY(wall);
        QCOMPARE(*wall.value().machines.first().capabilities, (QStringList{u"text"_s, u"future"_s}));
        core::SavedAction saved;
        saved.types = true;
        const auto action = core::Action::run(saved);
        QVERIFY(action.savedTypes);
        QVERIFY(!core::bodyOf(action).contains("savedTypes"));
    }

    void aQkvmSaysWhichVersionsOfItsApiItSpeaks()
    {
        // As a QKVM answers.
        const base::Result<core::Health> health = core::healthFrom(
            R"({"status": "ok", "version": "0.1.0", "api": ["v1"], "page": "3f2a9c", "locked": false})");
        QVERIFY(health.ok());
        QCOMPARE(health.value().version, u"0.1.0"_s);
        QCOMPARE(health.value().apis, QStringList{u"v1"_s});
        QVERIFY(!health.value().locked);
        // And this program speaks one of them.
        QVERIFY(health.value().apis.contains(core::spokenApi));

        // A later one, which speaks others, restarted locked.
        const base::Result<core::Health> later =
            core::healthFrom(R"({"status": "ok", "version": "3.0.0", "api": ["v2", "v3", "v1234"], "locked": true})");
        QVERIFY(later.ok());
        QCOMPARE(later.value().apis, (QStringList{u"v2"_s, u"v3"_s, u"v1234"_s}));
        QVERIFY(later.value().locked);
        // One that lists none is a QKVM all the same, and so is one that says nothing else.
        const base::Result<core::Health> bare = core::healthFrom(R"({"status": "ok", "api": []})");
        QVERIFY(bare.ok());
        QVERIFY(bare.value().apis.isEmpty());
        QCOMPARE(bare.value().version, QString());
        QVERIFY(!bare.value().locked);
        // What else it says is not gone into, however it is nested.
        const QByteArray deep = QByteArray(500, '[') + QByteArray(500, ']');
        QVERIFY(core::healthFrom(R"({"status": "ok", "api": ["v1"], "page": )" + deep + "}").ok());
    }

    void whatIsNoQkvmsWordOfItselfIsRefused_data()
    {
        QTest::addColumn<QByteArray>("answer");
        QTest::newRow("nothing") << QByteArray();
        QTest::newRow("a page") << QByteArray("<html>hello</html>");
        QTest::newRow("a list") << QByteArray(R"(["v1"])");
        QTest::newRow("a word") << QByteArray(R"("ok")");
        QTest::newRow("a number") << QByteArray("7");
        QTest::newRow("broken off") << QByteArray(R"({"status": "ok", "api": ["v1")");
        QTest::newRow("no status") << QByteArray(R"({"api": ["v1"]})");
        QTest::newRow("another status") << QByteArray(R"({"status": "starting", "api": ["v1"]})");
        QTest::newRow("a status in capitals") << QByteArray(R"({"status": "OK", "api": ["v1"]})");
        QTest::newRow("a yes where the status belongs") << QByteArray(R"({"status": true, "api": ["v1"]})");
        QTest::newRow("a list where the status belongs") << QByteArray(R"({"status": ["ok"], "api": ["v1"]})");
        // As a QKVM answered before its API had versions: none this program speaks to.
        QTest::newRow("no list of versions") << QByteArray(R"({"status": "ok", "version": "0.1.0"})");
        QTest::newRow("a number where the list belongs") << QByteArray(R"({"status": "ok", "api": 1})");
        QTest::newRow("a text where the list belongs") << QByteArray(R"({"status": "ok", "api": "v1"})");
        QTest::newRow("an object where the list belongs") << QByteArray(R"({"status": "ok", "api": {"v1": true}})");
        QTest::newRow("nothing where the list belongs") << QByteArray(R"({"status": "ok", "api": null})");
        QTest::newRow("more than a qKVM says of itself")
            << QByteArray(R"({"status": "ok", "api": ["v1"]})") + QByteArray(core::largestHealthBytes, ' ');
        // Nested two thousand deep: not gone into, and nothing falls over it.
        QTest::newRow("lists two thousand deep") << QByteArray(2000, '[') + QByteArray(2000, ']');
        QTest::newRow("two thousand deep where the list belongs")
            << QByteArray(R"({"status": "ok", "api": )") + QByteArray(2000, '[') + QByteArray(2000, ']') + "}";
        QByteArray objects;
        for (int level = 0; level < 2000; ++level) {
            objects += R"({"status": "ok", "api": ["v1"], "more": )";
        }
        QTest::newRow("objects two thousand deep") << objects + "1" + QByteArray(2000, '}');
    }

    void whatIsNoQkvmsWordOfItselfIsRefused()
    {
        QFETCH(QByteArray, answer);
        const base::Result<core::Health> health = core::healthFrom(answer);
        QVERIFY(!health.ok());
        QCOMPARE(health.error().code, std::string("bad_answer"));
    }

    void ofWhatAQkvmListsOnlyWhatIsWrittenAsAVersionIsTaken_data()
    {
        QTest::addColumn<QJsonValue>("listed");
        QTest::addColumn<QStringList>("taken");
        QTest::newRow("a list of numbers") << QJsonValue(QJsonArray{1, 2}) << QStringList();
        QTest::newRow("a number among them")
            << QJsonValue(QJsonArray{u"v1"_s, 1, u"v2"_s}) << QStringList{u"v1"_s, u"v2"_s};
        QTest::newRow("a text of 5000 places")
            << QJsonValue(QJsonArray{QString(5000, u'v'), u"v1"_s, u"v"_s + QString(5000, u'1')})
            << QStringList{u"v1"_s};
        QTest::newRow("lists, objects, a yes and nothing among them")
            << QJsonValue(QJsonArray{QJsonArray{u"v1"_s}, QJsonObject{{u"v1"_s, true}}, true, QJsonValue(), u"v3"_s})
            << QStringList{u"v3"_s};
        QTest::newRow("one to four digits")
            << QJsonValue(QJsonArray{u"v"_s, u"v0"_s, u"v12"_s, u"v1234"_s, u"v12345"_s})
            << QStringList{u"v0"_s, u"v12"_s, u"v1234"_s};
        QTest::newRow("written otherwise")
            << QJsonValue(QJsonArray{u"V1"_s, u"1"_s, u"v1.0"_s, u"version1"_s, u"v-1"_s, u"v1a"_s, QString()})
            << QStringList();
        QTest::newRow("something before or behind it")
            << QJsonValue(QJsonArray{u" v1"_s, u"v1 "_s, u"v1\n"_s, u"v1/../v2"_s, u"v1?x=1"_s}) << QStringList();
        // (The digit one of another script, made from its number: a version is written with ours.)
        QTest::newRow("digits of another script")
            << QJsonValue(QJsonArray{u"v"_s + QString(QChar(0x0661))}) << QStringList();
        QTest::newRow("the same one twice")
            << QJsonValue(QJsonArray{u"v1"_s, u"v2"_s, u"v1"_s}) << QStringList{u"v1"_s, u"v2"_s};
        QJsonArray many;
        QStringList firstSixteen;
        for (int number = 1; number <= 40; ++number) {
            many.append(u"v%1"_s.arg(number));
            if (number <= 16) {
                firstSixteen.append(u"v%1"_s.arg(number));
            }
        }
        QTest::newRow("more than sixteen") << QJsonValue(many) << firstSixteen;
        // The first sixteen entries are looked at, whatever they are, and no more.
        QJsonArray late;
        for (int number = 0; number < 16; ++number) {
            late.append(number);
        }
        late.append(u"v1"_s);
        QTest::newRow("one behind sixteen that are none") << QJsonValue(late) << QStringList();
    }

    void ofWhatAQkvmListsOnlyWhatIsWrittenAsAVersionIsTaken()
    {
        QFETCH(const QJsonValue, listed);
        QFETCH(const QStringList, taken);
        // What is left out leaves the rest as it is: the answer is a QKVM's all the same.
        const base::Result<core::Health> health = core::healthFrom(healthWith(u"api"_s, listed));
        QVERIFY(health.ok());
        QCOMPARE(health.value().apis, taken);
    }

    void theVersionAQkvmNamesIsOneShortLineOfTextOrNothing()
    {
        const auto versionOf = [](const QJsonValue& version) {
            return core::healthFrom(healthWith(u"version"_s, version)).value().version;
        };
        QCOMPARE(versionOf(u"0.1.0"_s), u"0.1.0"_s);
        // Never longer than a version is, whatever was sent.
        QCOMPARE(versionOf(QString(5000, u'9')), QString(40, u'9'));
        // What does not print is no part of it: a line break, a sign that turns the writing round.
        QCOMPARE(versionOf(u"0.1.0\r\nand more"_s), u"0.1.0 and more"_s);
        QCOMPARE(versionOf(u"0.1"_s + QString(QChar(0x202E)) + u"0"_s), u"0.1 0"_s);
        // What is no text is no version: nothing is made of a number, a list, a yes or nothing.
        QCOMPARE(versionOf(1.5), QString());
        QCOMPARE(versionOf(QJsonArray{u"0.1.0"_s}), QString());
        QCOMPARE(versionOf(QJsonObject{{u"version"_s, u"0.1.0"_s}}), QString());
        QCOMPARE(versionOf(true), QString());
        QCOMPARE(versionOf(QJsonValue()), QString());
    }

    void aQkvmIsLockedOnlyWhereItSaysSoPlainly()
    {
        const auto lockedBy = [](const QJsonValue& locked) {
            return core::healthFrom(healthWith(u"locked"_s, locked)).value().locked;
        };
        QVERIFY(lockedBy(true));
        QVERIFY(!lockedBy(false));
        // Anything but a plain yes is a no.
        QVERIFY(!lockedBy(u"true"_s));
        QVERIFY(!lockedBy(1));
        QVERIFY(!lockedBy(QJsonArray{true}));
        QVERIFY(!lockedBy(QJsonObject{{u"locked"_s, true}}));
        QVERIFY(!lockedBy(QJsonValue()));
        QVERIFY(!core::healthFrom(R"({"status": "ok", "api": ["v1"]})").value().locked);
    }

    void aSessionSaysWhoIsSignedIn()
    {
        const base::Result<core::Session> nobody =
            core::sessionFrom(R"({"initialized": true, "authenticated": false, "user": null, "ask_name": true})");
        QVERIFY(nobody.ok());
        QVERIFY(nobody.value().setUp);
        QVERIFY(nobody.value().asksName);
        QVERIFY(!nobody.value().account.has_value());

        const base::Result<core::Session> somebody = core::sessionFrom(
            R"({"initialized": true, "authenticated": true, "ask_name": false,
                "user": {"id": "u1", "name": "anna", "role": "operator", "token": false}})");
        QVERIFY(somebody.ok());
        QVERIFY(somebody.value().account.has_value());
        const core::Account account = somebody.value().account.value_or(core::Account{});
        QCOMPARE(account.name, u"anna"_s);
        QCOMPARE(account.role, u"operator"_s);
        QVERIFY(!account.isDisplay);
    }

    void aRoleTheProgramDoesNotKnowMayOnlyLook()
    {
        const base::Result<core::Session> session = core::sessionFrom(
            R"({"initialized": true, "authenticated": true, "user": {"id": "u1", "name": "x", "role": "owner-of-everything"}})");
        QVERIFY(session.ok());
        QCOMPARE(session.value().account.value_or(core::Account{}).role, u"viewer"_s);
    }

    void whatIsNotASessionIsRefused_data()
    {
        QTest::addColumn<QByteArray>("answer");
        QTest::newRow("not JSON") << QByteArray("<html>hello</html>");
        QTest::newRow("a list") << QByteArray("[1, 2, 3]");
        QTest::newRow("a word where a yes or no belongs")
            << QByteArray(R"({"initialized": "yes", "authenticated": false})");
        QTest::newRow("signed in, and nobody named") << QByteArray(R"({"initialized": true, "authenticated": true})");
        QTest::newRow("signed in as somebody without a number")
            << QByteArray(R"({"initialized": true, "authenticated": true, "user": {"name": "x", "role": "admin"}})");
        QTest::newRow("nothing") << QByteArray();
    }

    void whatIsNotASessionIsRefused()
    {
        QFETCH(QByteArray, answer);
        const base::Result<core::Session> session = core::sessionFrom(answer);
        QVERIFY(!session.ok());
        QCOMPARE(session.error().code, std::string("bad_answer"));
    }

    void theWallListsItsMachinesInTheirOrder()
    {
        const base::Result<core::Wall> wall = core::wallFrom(
            R"({"interval": 2.0, "devices": [
                {"id": "a", "name": "Build server", "host": "192.168.1.21", "kind": "kvmd", "picture": true, "group": "Lab"},
                {"id": "b", "name": "Database server", "host": "192.168.1.71", "kind": "redfish", "picture": false, "group": ""}]})");
        QVERIFY(wall.ok());
        QCOMPARE(wall.value().everySeconds, 2.0);
        QCOMPARE(wall.value().liveForm, 0);
        QCOMPARE(wall.value().machines.size(), 2);
        QCOMPARE(wall.value().machines[0].name, u"Build server"_s);
        QCOMPARE(wall.value().machines[0].group, u"Lab"_s);
        QVERIFY(wall.value().machines[0].hasPicture);
        QVERIFY(!wall.value().machines[1].hasPicture);
    }

    void aServerThatHasLivePicturesSaysWhichFormOfThemItSpeaks()
    {
        QCOMPARE(core::wallFrom(R"({"live": 2, "devices": []})").value().liveForm, 2);
        QCOMPARE(core::wallFrom(R"({"live": 3, "devices": []})").value().liveForm, 3);
        QCOMPARE(core::wallFrom(R"({"live": "yes", "devices": []})").value().liveForm, 0);
        QCOMPARE(core::wallFrom(R"({"devices": []})").value().liveForm, 0);
    }

    void aPaceThatMakesNoSenseIsBroughtWithinWhatDoes()
    {
        QCOMPARE(core::wallFrom(R"({"interval": 0.001, "devices": []})").value().everySeconds, 0.25);
        QCOMPARE(core::wallFrom(R"({"interval": 100000, "devices": []})").value().everySeconds, 60.0);
        QCOMPARE(core::wallFrom(R"({"devices": []})").value().everySeconds, 1.0);
    }

    void aWallThatIsNotOneIsRefused()
    {
        QVERIFY(!core::wallFrom(R"({"interval": 1})").ok());
        QVERIFY(!core::wallFrom(R"({"devices": [{"name": "a machine without a number"}]})").ok());
        // One machine more than the program shows.
        QJsonArray many;
        for (int index = 0; index <= core::mostMachines; ++index) {
            many.append(QJsonObject{{u"id"_s, QString::number(index)}});
        }
        const base::Result<core::Wall> wall =
            core::wallFrom(QJsonDocument(QJsonObject{{u"devices"_s, many}}).toJson(QJsonDocument::Compact));
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string("too_large"));
    }

    void aNameIsNeverLongerThanACardCanShow()
    {
        const QJsonObject machine{{u"id"_s, u"a"_s}, {u"name"_s, QString(5000, u'x')}};
        const base::Result<core::Wall> wall = core::wallFrom(
            QJsonDocument(QJsonObject{{u"devices"_s, QJsonArray{machine}}}).toJson(QJsonDocument::Compact));
        QVERIFY(wall.ok());
        QCOMPARE(wall.value().machines[0].name.size(), core::longestText);
    }

    void aKvmsStatusIsRead()
    {
        const base::Result<QHash<QString, core::Status>> all = core::statusFrom(
            R"({"a": {"state": "online", "error": null, "last_seen_ago": 3,
                      "video": {"signal": true, "width": 1920, "height": 1080, "fps": 30},
                      "usb": {"keyboard": true, "mouse": true},
                      "atx": {"enabled": true, "power": true, "hdd": false},
                      "health": {"temp": 43.4, "cpu": 5, "mem": 20},
                      "about": {"model": "A KVM", "version": "1"},
                      "ports": {"active": 1, "list": [{"name": "web-01", "signal": true}, {"name": "", "signal": true},
                                                      {"name": "spare", "signal": false}]}},
                "b": {"state": "offline", "error": {"code": "unreachable", "message": "Device is unreachable"},
                      "last_seen_ago": 7200}})");
        QVERIFY(all.ok());
        const core::Status a = all.value().value(u"a"_s);
        QCOMPARE(a.state, u"online"_s);
        QCOMPARE(a.lastSeenSecondsAgo, std::optional<int>(3));
        QCOMPARE(a.width, 1920);
        QCOMPARE(a.height, 1080);
        QVERIFY(a.hasPicture);
        QVERIFY(a.powerIsKnown);
        QCOMPARE(a.power, std::optional<bool>(true));
        QCOMPARE(a.keyboard, std::optional<bool>(true));
        QCOMPARE(a.temperature, std::optional<double>(43.4));
        QCOMPARE(a.ports.size(), 3);
        QCOMPARE(a.ports[0].name, u"web-01"_s);
        QVERIFY(a.ports[2].empty);
        QCOMPARE(a.shownPort, 1);
        QVERIFY(!a.server.has_value());

        const core::Status b = all.value().value(u"b"_s);
        QCOMPARE(b.state, u"offline"_s);
        QCOMPARE(b.errorCode, u"unreachable"_s);
        QCOMPARE(b.errorMessage, u"Device is unreachable"_s);
        QCOMPARE(b.lastSeenSecondsAgo, std::optional<int>(7200));
    }

    void aFingerbotAndItsBatteryAreRead()
    {
        // A Fingerbot paired with the KVM (`fingerbot` in `can`): its battery in percent, null until it was asked,
        // and whether that is low; the record is null without one (the web's kvm.py, status).
        const base::Result<QHash<QString, core::Status>> all = core::statusFrom(
            R"({"low": {"state": "online", "fingerbot": {"battery": 12, "low": true}},
                "well": {"state": "online", "fingerbot": {"battery": 80, "low": false}},
                "unasked": {"state": "online", "fingerbot": {"battery": null, "low": null}},
                "none": {"state": "online", "fingerbot": null},
                "unsaid": {"state": "online"},
                "beyond": {"state": "online", "fingerbot": {"battery": 140, "low": true}},
                "words": {"state": "online", "fingerbot": {"battery": "12", "low": "yes"}},
                "lowWithout": {"state": "online", "fingerbot": {"low": true}}})");
        QVERIFY(all.ok());
        // How each is read, in a word: "none", or its battery ("?" where that is not known) and whether it is low.
        const auto read = [&](const char* id) {
            const std::optional<core::Fingerbot> bot = all.value().value(QString::fromLatin1(id)).fingerbot;
            if (!bot) {
                return u"none"_s;
            }
            return (bot->battery ? QString::number(*bot->battery) : u"?"_s) + (bot->low ? u" low"_s : QString());
        };
        QCOMPARE(read("low"), u"12 low"_s);
        QCOMPARE(read("well"), u"80"_s);
        QCOMPARE(read("unasked"), u"?"_s);
        QCOMPARE(read("none"), u"none"_s);
        QCOMPARE(read("unsaid"), u"none"_s);
        // What is no percentage is none, and a battery that is not known is not called low.
        QCOMPARE(read("beyond"), u"?"_s);
        QCOMPARE(read("words"), u"?"_s);
        QCOMPARE(read("lowWithout"), u"?"_s);
    }

    void whatIsInAMachinesDriveIsRead()
    {
        // The status's `media` (0.3.0): what is in the machine's virtual drive, said to everybody who sees its tile, or
        // null where nothing is (the web's kvm.py, media_now). Its name is one plain line: it is the device's to say.
        const base::Result<QHash<QString, core::Status>> all = core::statusFrom(
            R"({"cd": {"state": "online", "media": {"drive": "cd", "image": "debian-13.iso", "name": "debian-13.iso",
                       "where": null, "read_only": true, "first": null, "by": "anna", "at": 1700000000,
                       "sha256": "ab12"}},
                "flash": {"state": "online", "media": {"drive": "flash", "image": "i1", "name": "tools\nlive.img",
                          "where": "local", "read_only": false, "first": true, "by": null, "at": null,
                          "sha256": null}},
                "nothing": {"state": "online", "media": null},
                "unsaid": {"state": "online"},
                "nameless": {"state": "online", "media": {"drive": "cd", "image": "x", "name": ""}},
                "words": {"state": "online", "media": "debian-13.iso"},
                "number": {"state": "online", "media": {"drive": "cd", "image": "x", "name": 12}}})");
        QVERIFY(all.ok());
        const auto media = [&](const char* id) { return all.value().value(QString::fromLatin1(id)).media; };
        QVERIFY(media("cd").has_value());
        const core::MountedMedia cd = media("cd").value_or(core::MountedMedia{});
        QCOMPARE(cd.drive, u"cd"_s);
        QCOMPARE(cd.image, u"debian-13.iso"_s);
        QCOMPARE(cd.name, u"debian-13.iso"_s);
        QCOMPARE(cd.where, QString());
        QVERIFY(cd.readOnly);
        QCOMPARE(cd.first, std::optional<bool>());
        QCOMPARE(cd.by, u"anna"_s);
        QCOMPARE(cd.at, std::optional<qint64>(1700000000));
        QCOMPARE(cd.sha256, u"ab12"_s);
        const core::MountedMedia flash = media("flash").value_or(core::MountedMedia{});
        QCOMPARE(flash.drive, u"flash"_s);
        QCOMPARE(flash.name, u"tools live.img"_s);
        QCOMPARE(flash.where, u"local"_s);
        QVERIFY(!flash.readOnly);
        QCOMPARE(flash.first, std::optional<bool>(true));
        QCOMPARE(flash.by, QString());
        QCOMPARE(flash.at, std::optional<qint64>());
        // Nothing in it, or nothing that names an image: nothing is said.
        for (const char* none : {"nothing", "unsaid", "nameless", "words", "number"}) {
            QVERIFY2(!media(none).has_value(), none);
        }
    }

    void aServersStatusIsRead()
    {
        const base::Result<QHash<QString, core::Status>> all = core::statusFrom(
            R"({"s": {"state": "online", "error": null, "picture": false,
                      "video": {"signal": true, "width": null, "height": null, "fps": null},
                      "atx": {"enabled": true, "power": true}, "health": {"temp": 61},
                      "about": {"model": "SRV-2200"},
                      "server": {"health": "Warning", "locator": true, "processor": "A processor", "processors": 2,
                                 "memory_gib": 256,
                                 "fans": [{"name": "1", "reading": 4000, "ok": true}, {"name": "2", "reading": 0, "ok": false}]}}})");
        QVERIFY(all.ok());
        const core::Status status = all.value().value(u"s"_s);
        QVERIFY(!status.hasPicture);
        // It says nothing of a console the server could read, and so has no screen to show.
        QVERIFY(!status.hasScreen);
        QCOMPARE(status.width, 0);
        QCOMPARE(status.model, u"SRV-2200"_s);
        QVERIFY(status.server.has_value());
        const core::ServerReadings server = status.server.value_or(core::ServerReadings{});
        QCOMPARE(server.health, u"Warning"_s);
        QCOMPARE(server.locator, std::optional<bool>(true));
        QCOMPARE(server.fans, 2);
        QCOMPARE(server.fansRunning, 1);
        QCOMPARE(server.fansNotWell, 1);
        QCOMPARE(server.processors, 2);
        QCOMPARE(server.memoryGib, std::optional<double>(256));
    }

    void aServerWhoseScreenCanBeReadSaysSo()
    {
        const base::Result<QHash<QString, core::Status>> all = core::statusFrom(
            R"({"s": {"state": "online", "picture": false, "screen": true, "video": {"width": 1024, "height": 768},
                      "server": {"health": "OK"}},
                "word": {"state": "online", "picture": false, "screen": "yes", "server": {"health": "OK"}},
                "kvm": {"state": "online"}})");
        QVERIFY(all.ok());
        QVERIFY(all.value().value(u"s"_s).hasScreen);
        QCOMPARE(all.value().value(u"s"_s).width, 1024);
        // Only a plain yes is one; and a KVM, which has a picture anyway, says nothing of it.
        QVERIFY(!all.value().value(u"word"_s).hasScreen);
        QVERIFY(!all.value().value(u"kvm"_s).hasScreen);
    }

    // b5: a PC with Intel AMT is in one of three views, as the wall lists it and, fresher, as its
    // status says it; a word of a view the server has and this program does not is its first, "stills";
    // no other machine has one. A server that lists the PC with no view at all (0.2.2, which has no views
    // and no route to change one: PUT .../view is its 404) gives it none (b11, the coordinator's decision).
    void aPcWithIntelAmtSaysWhichViewItsTileIsIn()
    {
        const auto wall = core::wallFrom(R"({"devices": [
            {"id": "h", "kind": "amt", "picture": false, "amt_view": "health"},
            {"id": "n", "kind": "amt", "picture": false},
            {"id": "w", "kind": "amt", "picture": false, "amt_view": "Live"},
            {"id": "k", "kind": "kvmd", "amt_view": "health"},
            {"id": "z", "kind": "amt", "picture": false, "amt_view": null},
            {"id": "x", "kind": "amt", "picture": false, "amt_view": 3}]})");
        QVERIFY(wall.ok());
        const QList<core::Machine>& machines = wall.value().machines;
        QCOMPARE(machines[0].amtView, u"health"_s);
        QCOMPARE(machines[1].amtView, QString());
        QCOMPARE(machines[2].amtView, u"stills"_s);
        QCOMPARE(machines[3].amtView, QString());
        QCOMPARE(machines[4].amtView, QString());
        QCOMPARE(machines[5].amtView, QString());
        // Listed with none and said with none: no view; said with one by its status: that one.
        QCOMPARE(core::amtViewOf(machines[1], nullptr), QString());
        core::Status none;
        none.server = core::ServerReadings{};
        QCOMPARE(core::amtViewOf(machines[1], &none), QString());
        none.server->amtView = u"live"_s;
        QCOMPARE(core::amtViewOf(machines[1], &none), u"live"_s);
        const auto all = core::statusFrom(R"({
            "h": {"state": "online", "picture": false, "screen": false,
                  "server": {"health": null, "no_screen": "This PC is set to health only.", "amt": {"view": "live"}}},
            "w": {"state": "online", "picture": false, "server": {"amt": {"view": 3}}},
            "k": {"state": "online", "server": {"amt": {"view": "live"}}}})");
        QVERIFY(all.ok());
        const core::Status h = all.value().value(u"h"_s);
        QCOMPARE(h.server.value_or(core::ServerReadings{}).noScreen, u"This PC is set to health only."_s);
        QCOMPARE(h.server.value_or(core::ServerReadings{}).amtView, u"live"_s);
        QCOMPARE(all.value().value(u"w"_s).server.value_or(core::ServerReadings{}).amtView, QString());
        // The status's word first; else the wall's; and for what is no such PC, none.
        QCOMPARE(core::amtViewOf(machines[0], &h), u"live"_s);
        QCOMPARE(core::amtViewOf(machines[0], nullptr), u"health"_s);
        const core::Status w = all.value().value(u"w"_s);
        QCOMPARE(core::amtViewOf(machines[0], &w), u"health"_s);
        const core::Status k = all.value().value(u"k"_s);
        QCOMPARE(core::amtViewOf(machines[3], &k), QString());
        core::Machine said;
        said.kind = u"amt"_s;
        QCOMPARE(core::amtViewOf(said, nullptr), QString());
        // And a picture of such a PC is asked as of any server whose screen is read: no view is no "health".
        core::Status screen;
        screen.hasPicture = false;
        screen.hasScreen = true;
        QVERIFY(core::hasAScreenToShow(said, &screen));
    }

    // b5: a picture is asked of a machine that has one, or of a server whose screen is read; never
    // of a PC with Intel AMT in health only, whatever else its status says.
    void noPictureIsAskedOfAPcInHealthOnly()
    {
        core::Machine kvm;
        kvm.kind = u"kvmd"_s;
        QVERIFY(core::hasAScreenToShow(kvm, nullptr));
        core::Machine pc;
        pc.kind = u"amt"_s;
        pc.hasPicture = false;
        pc.amtView = u"stills"_s;
        core::Status status;
        status.hasPicture = false;
        QVERIFY(!core::hasAScreenToShow(pc, &status));
        status.hasScreen = true;
        QVERIFY(core::hasAScreenToShow(pc, &status));
        // (A status that says its screen can be had, and that the tile is in health only.)
        status.server = core::ServerReadings{};
        status.server->amtView = u"health"_s;
        QVERIFY(!core::hasAScreenToShow(pc, &status));
        status.server->amtView.clear();
        pc.amtView = u"health"_s;
        QVERIFY(!core::hasAScreenToShow(pc, &status));
    }

    // b5: a still kept between sessions says so, and how old it is in whole seconds.
    void aStillSaysHowOldItIs_data()
    {
        QTest::addColumn<QByteArray>("still");
        QTest::addColumn<QByteArray>("age");
        QTest::addColumn<bool>("isStill");
        QTest::addColumn<int>("seconds"); // -1: none said
        QTest::newRow("a still of 42 s") << QByteArray("1") << QByteArray("42") << true << 42;
        QTest::newRow("a still of 0 s") << QByteArray("1") << QByteArray(" 0 ") << true << 0;
        QTest::newRow("a still of no age") << QByteArray("1") << QByteArray() << true << -1;
        QTest::newRow("below 0") << QByteArray("1") << QByteArray("-3") << true << -1;
        QTest::newRow("no whole number") << QByteArray("1") << QByteArray("4.5") << true << -1;
        QTest::newRow("a word") << QByteArray("1") << QByteArray("soon") << true << -1;
        QTest::newRow("larger than counted") << QByteArray("1") << QByteArray("99999999999") << true << -1;
        QTest::newRow("no still") << QByteArray("0") << QByteArray("42") << false << -1;
        QTest::newRow("nothing said") << QByteArray() << QByteArray("42") << false << -1;
        QTest::newRow("another yes") << QByteArray("true") << QByteArray("42") << false << -1;
    }

    void aStillSaysHowOldItIs()
    {
        QFETCH(QByteArray, still);
        QFETCH(QByteArray, age);
        QFETCH(bool, isStill);
        QFETCH(int, seconds);
        core::Snapshot snapshot;
        core::readStill(snapshot, still, age);
        QCOMPARE(snapshot.still, isStill);
        QCOMPARE(snapshot.ageSeconds, seconds < 0 ? std::nullopt : std::optional<int>(seconds));
    }

    void theViewAPcWasPutInIsRead()
    {
        const auto health = core::amtViewFrom(R"({"ok": true, "amt_view": "health"})");
        QVERIFY(health.ok());
        QCOMPARE(health.value(), u"health"_s);
        for (const QByteArray& answer :
             {QByteArray(R"({"ok": true, "amt_view": "Health"})"), QByteArray(R"({"ok": true})"),
              QByteArray(R"({"ok": false, "amt_view": "live"})"), QByteArray("[]"), QByteArray("not json")}) {
            QVERIFY2(!core::amtViewFrom(answer).ok(), answer.constData());
            QCOMPARE(core::amtViewFrom(answer).error().code, std::string("bad_answer"));
        }
    }

    void whatAStatusLeavesOutIsNotMadeUp()
    {
        const base::Result<QHash<QString, core::Status>> all = core::statusFrom(R"({"a": {}})");
        QVERIFY(all.ok());
        const core::Status status = all.value().value(u"a"_s);
        QCOMPARE(status.state, u"connecting"_s);
        QVERIFY(!status.signal.has_value());
        QVERIFY(!status.power.has_value());
        QVERIFY(!status.temperature.has_value());
        QVERIFY(!status.lastSeenSecondsAgo.has_value());
        QVERIFY(status.ports.isEmpty());
        QCOMPARE(status.shownPort, -1);
    }

    void aPictureIsOneOnlyWhereItSaysSoAndBeginsAsOne_data()
    {
        QTest::addColumn<QByteArray>("answer");
        QTest::addColumn<QByteArray>("kind");
        QTest::addColumn<bool>("isOne");
        const QByteArray jpeg = QByteArray::fromHex("ffd8ffe000104a464946") + QByteArray(40, 'x');
        const QByteArray png = QByteArray::fromHex("89504e470d0a1a0a") + QByteArray(40, 'x');
        QTest::newRow("a JPEG") << jpeg << "image/jpeg"_ba << true;
        QTest::newRow("a PNG") << png << "image/png"_ba << true;
        QTest::newRow("its kind in capitals, with more behind it") << jpeg << " Image/JPEG ; q=1"_ba << true;
        // (The engine goes by how a picture begins: a KVM that has no signal sends a PNG.)
        QTest::newRow("a PNG that is called a JPEG") << png << "image/jpeg"_ba << true;
        QTest::newRow("a page") << "<html></html>"_ba << "text/html"_ba << false;
        QTest::newRow("a page that is called a picture") << "<html></html>"_ba << "image/jpeg"_ba << false;
        QTest::newRow("a JPEG that is called nothing") << jpeg << QByteArray() << false;
        QTest::newRow("a JPEG that is called something else") << jpeg << "application/octet-stream"_ba << false;
        QTest::newRow("a kind of picture that is not drawn") << "GIF89a"_ba + jpeg << "image/gif"_ba << false;
        QTest::newRow("nothing") << QByteArray() << "image/jpeg"_ba << false;
        QTest::newRow("the first byte of a JPEG") << QByteArray::fromHex("ff") << "image/jpeg"_ba << false;
    }

    void aPictureIsOneOnlyWhereItSaysSoAndBeginsAsOne()
    {
        QFETCH(const QByteArray, answer);
        QFETCH(const QByteArray, kind);
        QFETCH(const bool, isOne);
        const base::Result<core::Snapshot> picture = core::snapshotFrom(answer, kind, "0"_ba);
        QCOMPARE(picture.ok(), isOne);
        if (isOne) {
            // As it came, and with what was said of the machine's signal.
            QCOMPARE(picture.value().picture, answer);
            QVERIFY(!picture.value().signal);
            QVERIFY(core::snapshotFrom(answer, kind, QByteArray()).value().signal);
        } else {
            QCOMPARE(picture.error().code, std::string("bad_answer"));
        }
    }

    void whereABrowserMayBeSentIsAWebAddressAndNothingElse_data()
    {
        QTest::addColumn<QString>("written");
        QTest::addColumn<QString>("taken");

        QTest::newRow("plain") << u"http://192.168.1.22/"_s << u"http://192.168.1.22/"_s;
        QTest::newRow("encrypted, with a port and a path")
            << u"https://kvm.example:8443/kvm/?view=1"_s << u"https://kvm.example:8443/kvm/?view=1"_s;
        QTest::newRow("capitals in its kind") << u"HTTP://192.168.1.22/"_s << u"http://192.168.1.22/"_s;
        // What Windows would open with something else than a browser, or run.
        QTest::newRow("a file") << u"file:///C:/Windows/System32/calc.exe"_s << QString();
        QTest::newRow("a script") << u"javascript:alert(1)"_s << QString();
        QTest::newRow("a kind of Windows' own") << u"ms-settings:privacy"_s << QString();
        QTest::newRow("a command") << u"cmd /c calc"_s << QString();
        QTest::newRow("a share") << u"//server/share/run.bat"_s << QString();
        // The same with a host in them: what is refused is their kind, not that something is missing.
        QTest::newRow("a file on another machine") << u"file://server/share/run.bat"_s << QString();
        QTest::newRow("another kind, with a host") << u"ftp://192.168.1.22/"_s << QString();
        QTest::newRow("a kind of some program's, with a host") << u"vnc://192.168.1.22/"_s << QString();
        QTest::newRow("a program") << u"C:/Windows/System32/calc.exe"_s << QString();
        // What is no address, or more than one.
        QTest::newRow("nothing") << QString() << QString();
        QTest::newRow("no kind") << u"192.168.1.22"_s << QString();
        QTest::newRow("no host") << u"http:///console"_s << QString();
        QTest::newRow("a name and a password in it") << u"http://admin:secret@192.168.1.22/"_s << QString();
        QTest::newRow("a name in it") << u"https://192.168.1.22@elsewhere.example/"_s << QString();
        QTest::newRow("a space in it") << u"http://192.168.1.22/a b"_s << QString();
        QTest::newRow("longer than an address is") << u"http://192.168.1.22/"_s + QString(2000, u'a') << QString();
    }

    void whereABrowserMayBeSentIsAWebAddressAndNothingElse()
    {
        QFETCH(const QString, written);
        QFETCH(const QString, taken);
        QCOMPARE(core::webAddress(written).toString(), taken);
    }

    void twoAddressesAreOnePlaceByTheirKindHostAndPort_data()
    {
        QTest::addColumn<QString>("one");
        QTest::addColumn<QString>("other");
        QTest::addColumn<bool>("same");

        QTest::newRow("another page of it")
            << u"http://192.168.1.60/"_s << u"http://192.168.1.60/console?pass=1"_s << true;
        QTest::newRow("the port said and not said")
            << u"http://192.168.1.60:80/"_s << u"http://192.168.1.60/"_s << true;
        QTest::newRow("the same, encrypted") << u"https://192.168.1.60:443/"_s << u"https://192.168.1.60/x"_s << true;
        QTest::newRow("capitals in a name") << u"https://KVM.example/"_s << u"https://kvm.example/x"_s << true;
        QTest::newRow("another host") << u"http://192.168.1.60/"_s << u"http://192.168.1.61/"_s << false;
        QTest::newRow("a host that only begins so")
            << u"https://kvm.example/"_s << u"https://kvm.example.elsewhere.example/"_s << false;
        QTest::newRow("another port") << u"http://192.168.1.60/"_s << u"http://192.168.1.60:8080/"_s << false;
        QTest::newRow("not encrypted any more") << u"https://192.168.1.60/"_s << u"http://192.168.1.60/"_s << false;
        QTest::newRow("encrypted, where it was not")
            << u"http://192.168.1.60/"_s << u"https://192.168.1.60/"_s << false;
        QTest::newRow("the same port, and not encrypted")
            << u"https://192.168.1.60:8443/"_s << u"http://192.168.1.60:8443/"_s << false;
        QTest::newRow("nothing and nothing") << QString() << QString() << false;
    }

    void twoAddressesAreOnePlaceByTheirKindHostAndPort()
    {
        QFETCH(const QString, one);
        QFETCH(const QString, other);
        QFETCH(const bool, same);
        QCOMPARE(core::sameSite(QUrl(one), QUrl(other)), same);
        QCOMPARE(core::sameSite(QUrl(other), QUrl(one)), same);
    }

    void aMachinesOwnConsoleIsReadFromTheWallAndFromHowItIs()
    {
        // Built, not written out: what moc reads of a test may not hold two slashes in a row.
        const auto device = [](const QString& id, const QJsonValue& openAt) {
            return QJsonObject{{u"id"_s, id}, {u"name"_s, id}, {u"open_url"_s, openAt}};
        };
        const base::Result<core::Wall> wall = core::wallFrom(
            QJsonDocument(QJsonObject{{u"interval"_s, 1},
                                      {u"devices"_s, QJsonArray{device(u"a"_s, u"http://192.168.1.22/"_s),
                                                                device(u"b"_s, u"javascript:alert(1)"_s),
                                                                device(u"c"_s, 7), QJsonObject{{u"id"_s, u"d"_s}}}}})
                .toJson());
        QVERIFY(wall.ok());
        QCOMPARE(wall.value().machines[0].page, QUrl(u"http://192.168.1.22/"_s));
        // What is no web address is no console a browser is sent to.
        QVERIFY(wall.value().machines[1].page.isEmpty());
        QVERIFY(wall.value().machines[2].page.isEmpty());
        QVERIFY(wall.value().machines[3].page.isEmpty());

        // Whether its controller hands out a pass that leads straight in: only a plain yes is one.
        const base::Result<QHash<QString, core::Status>> all = core::statusFrom(
            R"({"s": {"console": true}, "no": {"console": false}, "word": {"console": "yes"}, "silent": {}})");
        QVERIFY(all.ok());
        QVERIFY(all.value().value(u"s"_s).opensSignedIn);
        QVERIFY(!all.value().value(u"no"_s).opensSignedIn);
        QVERIFY(!all.value().value(u"word"_s).opensSignedIn);
        QVERIFY(!all.value().value(u"silent"_s).opensSignedIn);
    }

    void whereAConsoleIsEnteredIsRead()
    {
        const auto said = [](const QJsonValue& address) {
            return QJsonDocument(QJsonObject{{u"url"_s, address}}).toJson();
        };
        const base::Result<QUrl> way = core::consoleFrom(said(u"https://192.168.1.60/console?pass=abc"_s));
        QVERIFY(way.ok());
        QCOMPARE(way.value(), QUrl(u"https://192.168.1.60/console?pass=abc"_s));
        // An answer that names no web address names no way in.
        for (const QJsonValue& other : {QJsonValue(u"file:///C:/x.exe"_s), QJsonValue(u"javascript:alert(1)"_s),
                                        QJsonValue(u"/console?pass=abc"_s), QJsonValue(7), QJsonValue()}) {
            const base::Result<QUrl> refused = core::consoleFrom(said(other));
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().code, std::string("bad_answer"));
        }
        QVERIFY(!core::consoleFrom("{}").ok());
        QVERIFY(!core::consoleFrom("[]").ok());
    }

    void whatAMachineSaysItIsIsRead()
    {
        const base::Result<QHash<QString, core::Status>> all = core::statusFrom(
            R"({"a": {"about": {"model": "KVM-A", "version": "4.82"}},
                "b": {"about": {"model": 7, "version": null}}, "c": {}})");
        QVERIFY(all.ok());
        QCOMPARE(all.value().value(u"a"_s).model, u"KVM-A"_s);
        QCOMPARE(all.value().value(u"a"_s).version, u"4.82"_s);
        // What is no text is none.
        QCOMPARE(all.value().value(u"b"_s).model, QString());
        QCOMPARE(all.value().value(u"b"_s).version, QString());
        QCOMPARE(all.value().value(u"c"_s).version, QString());
    }

    void aServersLightIsOnOrOffOrNotThere()
    {
        // The light to find a server by in the rack: only a server that has one is offered its switch.
        const base::Result<QHash<QString, core::Status>> all = core::statusFrom(
            R"({"on": {"server": {"locator": true}}, "off": {"server": {"locator": false}},
                "none": {"server": {"locator": null}}, "silent": {"server": {}}, "word": {"server": {"locator": "on"}}})");
        QVERIFY(all.ok());
        const auto light = [&](const QString& of) {
            return all.value().value(of).server.value_or(core::ServerReadings{}).locator;
        };
        QCOMPARE(light(u"on"_s), std::optional<bool>(true));
        QCOMPARE(light(u"off"_s), std::optional<bool>(false));
        QVERIFY(!light(u"none"_s).has_value());
        QVERIFY(!light(u"silent"_s).has_value());
        QVERIFY(!light(u"word"_s).has_value());
    }

    void whatIsAskedOfAMachineIsWrittenAsTheServerTakesIt_data()
    {
        QTest::addColumn<core::Action>("action");
        QTest::addColumn<QJsonObject>("written");

        QTest::newRow("a power button") << core::Action::plain(u"atx_power"_s)
                                        << QJsonObject{{u"action"_s, u"atx_power"_s}};
        QTest::newRow("keys") << core::Action::press(u"ControlLeft,AltLeft,Delete"_s)
                              << QJsonObject{{u"action"_s, u"keys"_s}, {u"keys"_s, u"ControlLeft,AltLeft,Delete"_s}};
        QTest::newRow("text and Enter") << core::Action::type(u"ipconfig"_s, true)
                                        << QJsonObject{{u"action"_s, u"text"_s},
                                                       {u"text"_s, u"ipconfig"_s},
                                                       {u"enter"_s, true}};
        QTest::newRow("text alone") << core::Action::type(u"a \"b\"\n"_s, false)
                                    << QJsonObject{
                                           {u"action"_s, u"text"_s}, {u"text"_s, u"a \"b\"\n"_s}, {u"enter"_s, false}};
        QTest::newRow("a port") << core::Action::show(2) << QJsonObject{{u"action"_s, u"port"_s}, {u"port"_s, 2}};
        QTest::newRow("a wake") << core::Action::wake(u"00:11:22:aa:bb:cc"_s)
                                << QJsonObject{{u"action"_s, u"wol"_s}, {u"mac"_s, u"00:11:22:aa:bb:cc"_s}};
        core::SavedAction saved;
        saved.id = u"a1b2c3d4"_s;
        saved.changed = 3;
        core::Action ofARun = core::Action::plain(u"power_on"_s);
        ofARun.batch = u"m1abc-x7k2q9"_s;
        QTest::newRow("one of a run on several")
            << ofARun << QJsonObject{{u"action"_s, u"power_on"_s}, {u"batch"_s, u"m1abc-x7k2q9"_s}};
        // From the machine's large view: said so (the page's `watching`); and the BIOS setup is asked from there.
        core::Action watched = core::Action::press(u"ShiftLeft"_s);
        watched.watching = true;
        QTest::newRow("from the large view")
            << watched << QJsonObject{{u"action"_s, u"keys"_s}, {u"keys"_s, u"ShiftLeft"_s}, {u"watching"_s, true}};
        core::Action setup = core::Action::plain(u"bios_setup"_s);
        setup.watching = true;
        QTest::newRow("the BIOS setup") << setup << QJsonObject{{u"action"_s, u"bios_setup"_s}, {u"watching"_s, true}};
        QTest::newRow("a saved one") << core::Action::run(saved)
                                     << QJsonObject{{u"action"_s, u"custom"_s},
                                                    {u"custom_id"_s, u"a1b2c3d4"_s},
                                                    {u"custom_changed"_s, 3}};
    }

    void whatIsAskedOfAMachineIsWrittenAsTheServerTakesIt()
    {
        QFETCH(const core::Action, action);
        QFETCH(const QJsonObject, written);
        // Exactly that: nothing of another kind of action goes along.
        QCOMPARE(QJsonDocument::fromJson(core::bodyOf(action)).object(), written);
    }

    void theOrderTheServerHandsBackIsRead()
    {
        const base::Result<QStringList> order = core::orderFrom(R"({"ok": true, "ids": ["b", "a", "s"]})");
        QVERIFY(order.ok());
        QCOMPARE(order.value(), (QStringList{u"b"_s, u"a"_s, u"s"_s}));
        // A wall without a machine has an order too.
        QVERIFY(core::orderFrom(R"({"ok": true, "ids": []})").ok());
        // What does not say that it was done, or names no machines, or something that is no
        // machine's name, is not an order.
        QVERIFY(!core::orderFrom(R"({"ids": ["b", "a"]})").ok());
        QVERIFY(!core::orderFrom(R"({"ok": true})").ok());
        QVERIFY(!core::orderFrom(R"({"ok": true, "ids": "b,a"})").ok());
        QVERIFY(!core::orderFrom(R"({"ok": true, "ids": ["b", 7]})").ok());
        QVERIFY(!core::orderFrom(R"({"ok": true, "ids": ["b", ""]})").ok());
        QVERIFY(!core::orderFrom("[]").ok());
        // Nor is a list of more machines than a wall here has.
        QJsonArray many;
        for (int number = 0; number <= core::mostMachines; ++number) {
            many.append(u"m%1"_s.arg(number));
        }
        const base::Result<QStringList> tooMany =
            core::orderFrom(QJsonDocument(QJsonObject{{u"ok"_s, true}, {u"ids"_s, many}}).toJson());
        QVERIFY(!tooMany.ok());
        QCOMPARE(tooMany.error().code, std::string("too_large"));
    }

    void aMachinesMediaListIsReadAsTheServerSaysIt()
    {
        // GET .../media (0.3.0): the images, what is mounted, what its media can do, and what stands against a change
        // (the web's server.py, media_list; kvm.py, MediaOffer).
        const base::Result<core::MediaList> list = core::mediaListFrom(
            R"({"images": [{"id": "debian-13.iso", "name": "debian-13.iso", "where": null, "size": 700000000,
                            "partial": false, "sha256": null},
                           {"id": "local:iso/win11.iso", "name": "win\t11.iso", "where": "local", "size": "big",
                            "partial": true, "sha256": "ab12"},
                           {"id": "", "name": "nameless.iso"}, {"id": "x"}, "debian-12.iso"],
                "unseen": ["The storage nfs did not answer."], "left_out": 2, "notes": ["File Sharing is on at the KVM."],
                "space": {"free": 9000000000, "size": 32000000000},
                "mounted": {"drive": "cd", "image": "debian-13.iso", "name": "debian-13.iso", "read_only": true},
                "offers": {"drives": ["cd", "flash", "tape", "cd"], "boot": ["once", "always"], "restart": true,
                           "upload": "yes", "where": "device", "writable": ["flash", "disk"], "delete": true,
                           "drops_input": true},
                "uploading": {"name": "tools.img", "size": 1000, "sent": 4000, "by": "anna", "at": 1700000000},
                "served_from": )"
            // (Not in the raw text: moc takes the two strokes of an address there for the start of a remark.)
            "\"http://192.168.1.10:8787\"}");
        QVERIFY2(list.ok(), list.ok() ? "" : list.error().message.c_str());
        const core::MediaList& is = list.value();
        QCOMPARE(is.images.size(), 2);
        QCOMPARE(is.images[0].id, u"debian-13.iso"_s);
        QCOMPARE(is.images[0].size, std::optional<qint64>(700000000));
        QVERIFY(!is.images[0].partial);
        QCOMPARE(is.images[1].id, u"local:iso/win11.iso"_s);
        QCOMPARE(is.images[1].name, u"win 11.iso"_s);
        QCOMPARE(is.images[1].where, u"local"_s);
        QCOMPARE(is.images[1].size, std::optional<qint64>());
        QVERIFY(is.images[1].partial);
        QCOMPARE(is.images[1].sha256, u"ab12"_s);
        // What names no image is left out, and counted with what the server left out.
        QCOMPARE(is.leftOut, 5);
        QCOMPARE(is.unseen, QStringList{u"The storage nfs did not answer."_s});
        QCOMPARE(is.notes, QStringList{u"File Sharing is on at the KVM."_s});
        QCOMPARE(is.free, std::optional<qint64>(9000000000));
        QCOMPARE(is.size, std::optional<qint64>(32000000000));
        QVERIFY(is.mounted.has_value());
        QCOMPARE(is.mounted.value_or(core::MountedMedia{}).image, u"debian-13.iso"_s);
        // Of what it offers, only the server's own words, each once; writable only of a drive it takes.
        QCOMPARE(is.offers.drives, (QStringList{u"cd"_s, u"flash"_s}));
        QCOMPARE(is.offers.boot, QStringList{u"once"_s});
        QVERIFY(is.offers.restart);
        QVERIFY(!is.offers.upload);
        QCOMPARE(is.offers.where, u"device"_s);
        QCOMPARE(is.offers.writable, QStringList{u"flash"_s});
        QVERIFY(is.offers.canDelete);
        QVERIFY(is.offers.dropsInput);
        QVERIFY(is.uploading.has_value());
        const core::MediaUploading up = is.uploading.value_or(core::MediaUploading{});
        QCOMPARE(up.name, u"tools.img"_s);
        QCOMPARE(up.size, qint64{1000});
        QCOMPARE(up.sent, qint64{1000});
        QCOMPARE(up.by, u"anna"_s);
        QCOMPARE(is.servedFrom, u"http://192.168.1.10:8787"_s);

        // What is no list of a machine's media is refused whole.
        QVERIFY(!core::mediaListFrom(R"({"images": []})").ok());
        QVERIFY(!core::mediaListFrom(R"({"offers": {}})").ok());
        QVERIFY(!core::mediaListFrom("<html>").ok());
        // Nothing in the drive, and no upload under way.
        const base::Result<core::MediaList> bare =
            core::mediaListFrom(R"({"images": [], "offers": {}, "mounted": null, "uploading": null})");
        QVERIFY(bare.ok());
        QVERIFY(!bare.value().mounted && !bare.value().uploading && !bare.value().free);
    }

    void anImageIdLongerThanTheServerTakesIsLeftOutAndNeverCutShort()
    {
        // The server takes an image's id of 300 at the most (its UnmountIn and DeleteIn): one longer is no id of its,
        // and cut short it could name another image, which a mount, an unmount or a deletion would then be of. Left
        // out, as an empty one is; and what is mounted under one names no image to unmount.
        const QString longest(300, u'a');
        const QString longer = longest + u'b';
        const QJsonObject said{
            {u"images"_s, QJsonArray{QJsonObject{{u"id"_s, longest}, {u"name"_s, u"a.iso"_s}},
                                     QJsonObject{{u"id"_s, longer}, {u"name"_s, u"b.iso"_s}}}},
            {u"offers"_s, QJsonObject{}},
            {u"mounted"_s, QJsonObject{{u"drive"_s, u"cd"_s}, {u"image"_s, longer}, {u"name"_s, u"b.iso"_s}}}};
        const base::Result<core::MediaList> list = core::mediaListFrom(QJsonDocument(said).toJson());
        QVERIFY(list.ok());
        QCOMPARE(list.value().images.size(), 1);
        QCOMPARE(list.value().images[0].id, longest);
        QCOMPARE(list.value().leftOut, 1);
        QVERIFY(list.value().mounted.has_value());
        const core::MountedMedia mounted = list.value().mounted.value_or(core::MountedMedia{});
        QCOMPARE(mounted.name, u"b.iso"_s);
        QVERIFY(mounted.image.isEmpty());
        // And 300 exactly is an id.
        QJsonObject exactly = said;
        exactly[u"mounted"_s] = QJsonObject{{u"drive"_s, u"cd"_s}, {u"image"_s, longest}, {u"name"_s, u"a.iso"_s}};
        const base::Result<core::MediaList> again = core::mediaListFrom(QJsonDocument(exactly).toJson());
        QVERIFY(again.ok());
        QCOMPARE(again.value().mounted.value_or(core::MountedMedia{}).image, longest);
    }

    void whatAMediaChangeAndAnUploadAnswerIsRead()
    {
        const base::Result<core::MediaDone> mounted = core::mediaDoneFrom(
            R"({"ok": true, "note": "Its remote media were switched on.", "mounted": {"drive": "cd", "image": "a", "name": "a.iso"}})");
        QVERIFY(mounted.ok());
        QCOMPARE(mounted.value().note, u"Its remote media were switched on."_s);
        QVERIFY(mounted.value().mounted.has_value());
        const base::Result<core::MediaDone> deleted = core::mediaDoneFrom(R"({"ok": true, "note": null})");
        QVERIFY(deleted.ok());
        QCOMPARE(deleted.value().note, QString());
        QVERIFY(!deleted.value().mounted);
        QVERIFY(!core::mediaDoneFrom(R"({"note": "done"})").ok());
        QVERIFY(!core::mediaDoneFrom(R"({"ok": "yes"})").ok());
        const base::Result<core::MediaUploaded> uploaded =
            core::uploadedFrom(R"({"ok": true, "image": {"id": "a.iso"}, "sha256": "0123abcd"})");
        QVERIFY(uploaded.ok());
        QCOMPARE(uploaded.value().sha256, u"0123abcd"_s);
        QVERIFY(!core::uploadedFrom(R"({"sha256": "0123abcd"})").ok());
        const base::Result<std::optional<core::MediaUploading>> none = core::uploadingFrom(R"({"uploading": null})");
        QVERIFY(none.ok() && !none.value());
        const base::Result<std::optional<core::MediaUploading>> one =
            core::uploadingFrom(R"({"uploading": {"name": "a.iso", "size": 10, "sent": 4, "by": "anna"}})");
        QVERIFY(one.ok() && one.value());
        QCOMPARE(one.value().value_or(core::MediaUploading{}).sent, qint64{4});
        QVERIFY(!core::uploadingFrom(R"({})").ok());
    }

    void whatIsAskedOfAMachinesDriveIsWrittenAsTheServerTakesIt()
    {
        core::MediaMount mount;
        mount.image = u"debian-13.iso"_s;
        mount.machine = u"Build server"_s;
        QCOMPARE(QJsonDocument::fromJson(core::mountBody(mount)).object(),
                 QJsonObject({{u"image"_s, u"debian-13.iso"_s},
                              {u"machine"_s, u"Build server"_s},
                              {u"drive"_s, u"cd"_s},
                              {u"writable"_s, false},
                              {u"boot"_s, u"none"_s},
                              {u"restart"_s, false}}));
        QCOMPARE(QJsonDocument::fromJson(core::imageBody(u"a.iso"_s, u"Build server"_s)).object(),
                 QJsonObject({{u"image"_s, u"a.iso"_s}, {u"machine"_s, u"Build server"_s}}));
    }

    void anImagesNameIsOneTheServerTakes_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("taken");
        QTest::newRow("plain") << u"debian-13.iso"_s << true;
        QTest::newRow("with blanks and signs") << u"Win 11 (x64) [en],v2~+_.iso"_s << true;
        QTest::newRow("one letter") << u"a"_s << true;
        QTest::newRow("two hundred") << QString(200, u'a') << true;
        QTest::newRow("more") << QString(201, u'a') << false;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("a dot first") << u".hidden.iso"_s << false;
        QTest::newRow("a blank first") << u" a.iso"_s << false;
        QTest::newRow("a dot last") << u"a.iso."_s << false;
        QTest::newRow("a blank last") << u"a.iso "_s << false;
        QTest::newRow("a comma first") << u",a.iso"_s << false;
        QTest::newRow("a path") << u"../a.iso"_s << false;
        QTest::newRow("a folder") << u"isos/a.iso"_s << false;
        QTest::newRow("a drive") << u"c:a.iso"_s << false;
        QTest::newRow("a line feed") << u"a\n.iso"_s << false;
        QTest::newRow("a letter that is no ASCII") << (u"d"_s + QChar(0xE9) + u"bian.iso"_s) << false;
        QTest::newRow("a JetKVM's part") << u"a.iso.incomplete"_s << false;
    }

    void anImagesNameIsOneTheServerTakes()
    {
        QFETCH(const QString, name);
        QFETCH(const bool, taken);
        QCOMPARE(core::isImageName(name), taken);
    }

    void whatTheServerSaysWhenItWasDoneIsRead()
    {
        const base::Result<core::Done> done = core::doneFrom(R"({"ok": true, "note": "already on"})");
        QVERIFY(done.ok());
        QCOMPARE(done.value().note, u"already on"_s);
        const base::Result<core::Done> plain = core::doneFrom(R"({"ok": true, "note": null})");
        QVERIFY(plain.ok());
        QCOMPARE(plain.value().note, QString());
        // An answer that does not say it was done is not taken to say so.
        QVERIFY(!core::doneFrom(R"({"note": "already on"})").ok());
        QVERIFY(!core::doneFrom(R"({"ok": "yes"})").ok());
        QVERIFY(!core::doneFrom("[]").ok());
    }

    void theSavedActionsAreRead()
    {
        const base::Result<QList<core::SavedAction>> all = core::savedActionsFrom(
            R"({"actions": [
                 {"id": "a1b2c3d4", "name": "Lock", "kind": "keys", "keys": "MetaLeft,KeyL", "enter": true,
                  "danger": false, "changed": 2},
                 {"id": "e5f6", "name": "Unlock the disks", "kind": "text", "keys": "KeyX", "enter": true,
                  "danger": true, "changed": 0}]})");
        QVERIFY(all.ok());
        const QList<core::SavedAction>& actions = all.value();
        QCOMPARE(actions.size(), 2);
        QCOMPARE(actions[0].id, u"a1b2c3d4"_s);
        QCOMPARE(actions[0].name, u"Lock"_s);
        QVERIFY(!actions[0].types);
        QCOMPARE(actions[0].keys, u"MetaLeft,KeyL"_s);
        // Enter belongs to a text; keys belong to none.
        QVERIFY(!actions[0].enter);
        QVERIFY(!actions[0].danger);
        QCOMPARE(actions[0].changed, 2);
        QVERIFY(actions[1].types);
        QCOMPARE(actions[1].keys, QString());
        QVERIFY(actions[1].enter);
        QVERIFY(actions[1].danger);
        QCOMPARE(actions[1].changed, 0);

        QVERIFY(core::savedActionsFrom(R"({"actions": []})").ok());
    }

    void savedActionsThatAreNotWhatTheServerMakesAreRefused_data()
    {
        QTest::addColumn<QByteArray>("answer");
        QTest::newRow("no list") << QByteArray(R"({"actions": {}})");
        QTest::newRow("no id") << QByteArray(R"({"actions": [{"name": "Lock", "kind": "keys"}]})");
        QTest::newRow("an id that reaches further")
            << QByteArray(R"({"actions": [{"id": "a/../b", "name": "Lock", "kind": "keys"}]})");
        QTest::newRow("no name") << QByteArray(R"({"actions": [{"id": "a1", "name": "", "kind": "keys"}]})");
        QTest::newRow("another kind") << QByteArray(R"({"actions": [{"id": "a1", "name": "Lock", "kind": "power"}]})");
        QTest::newRow("not an action") << QByteArray(R"({"actions": ["a1"]})");
    }

    void savedActionsThatAreNotWhatTheServerMakesAreRefused()
    {
        QFETCH(const QByteArray, answer);
        const base::Result<QList<core::SavedAction>> all = core::savedActionsFrom(answer);
        QVERIFY(!all.ok());
        QCOMPARE(all.error().code, std::string("bad_answer"));
    }

    void moreSavedActionsThanAreShownAreRefused()
    {
        QJsonArray many;
        for (int number = 0; number <= core::mostSavedActions; ++number) {
            many.append(QJsonObject{{u"id"_s, QString::number(number)}, {u"name"_s, u"n"_s}, {u"kind"_s, u"keys"_s}});
        }
        const base::Result<QList<core::SavedAction>> all =
            core::savedActionsFrom(QJsonDocument(QJsonObject{{u"actions"_s, many}}).toJson());
        QVERIFY(!all.ok());
        QCOMPARE(all.error().code, std::string("too_large"));
        many.removeLast();
        QVERIFY(core::savedActionsFrom(QJsonDocument(QJsonObject{{u"actions"_s, many}}).toJson()).ok());
    }

    void theComputersAKvmCanWakeAreRead()
    {
        const base::Result<QList<core::WakeTarget>> all = core::wakeTargetsFrom(
            R"({"targets": [{"mac": "00:11:22:aa:BB:cc", "name": "NAS"}, {"mac": "00-11-22-33-44-55", "name": ""}]})");
        QVERIFY(all.ok());
        const QList<core::WakeTarget>& targets = all.value();
        QCOMPARE(targets.size(), 2);
        QCOMPARE(targets[0].name, u"NAS"_s);
        QCOMPARE(targets[0].mac, u"00:11:22:aa:BB:cc"_s);
        QCOMPARE(targets[1].name, QString());
        QCOMPARE(targets[1].mac, u"00-11-22-33-44-55"_s);
        QVERIFY(core::wakeTargetsFrom(R"({"targets": []})").ok());

        // What is no address is not sent back to the server as one.
        QVERIFY(!core::wakeTargetsFrom(R"({"targets": [{"mac": "00:11:22:aa:bb", "name": "NAS"}]})").ok());
        QVERIFY(!core::wakeTargetsFrom(R"({"targets": [{"mac": "00:11:22:aa:bb:cc&x=1", "name": "NAS"}]})").ok());
        QVERIFY(!core::wakeTargetsFrom(R"({"targets": [{"name": "NAS"}]})").ok());
        QVERIFY(!core::wakeTargetsFrom(R"({"targets": {}})").ok());
    }

    void aStatusThatIsNotOneIsRefused()
    {
        QVERIFY(!core::statusFrom("[]").ok());
        QVERIFY(!core::statusFrom(R"({"a": "online"})").ok());
    }

    void aRefusalIsToldInTheServersWords()
    {
        const base::Error told =
            core::refusalFrom(409, R"({"code": "busy", "message": "Another action is running on this KVM"})");
        QCOMPARE(told.code, std::string("busy"));
        QCOMPARE(told.message, std::string("Another action is running on this KVM"));

        const base::Error untold = core::refusalFrom(502, "<html>Bad Gateway</html>");
        QCOMPARE(untold.code, std::string("refused"));
        QVERIFY(untold.message.find("502") != std::string::npos);

        // The server's word for a machine it cannot reach is the server's, and is kept as it is.
        const base::Error away =
            core::refusalFrom(502, R"({"code": "unreachable", "message": "Device is unreachable"})");
        QCOMPARE(away.code, std::string("unreachable"));
        // What this program finds itself (the server was not reached, the session is gone) it
        // writes with a colon. No answer of a server is taken for one of those: its sentence is
        // told, and its word is not.
        const base::Error posing =
            core::refusalFrom(400, R"({"code": "desktop:signed_out", "message": "Not what it says"})");
        QCOMPARE(posing.code, std::string("refused"));
        QCOMPARE(posing.message, std::string("Not what it says"));
    }

    void aFileTheServerMadeIsTakenAsItIs()
    {
        const QByteArray text = "{\"format\": \"qkvm-export\", \"sealed\": {\"v\": 1}}";
        const base::Result<core::WallFile> file = core::wallFileFrom(text, "2");
        QVERIFY(file.ok());
        QCOMPARE(file.value().text, text);
        QCOMPARE(file.value().notOpened, 2);
        // It says of itself whether it is encrypted, and that is told with it.
        QVERIFY(file.value().sealed);
        QVERIFY(!core::wallFileFrom("{\"format\": \"qkvm-export\", \"devices\": []}", {}).value().sealed);
        // As large as a file may be, and not larger.
        const QByteArray most = text + QByteArray(core::largestFileBytes - text.size(), ' ');
        QVERIFY(core::wallFileFrom(most, {}).ok());
        QVERIFY(!core::wallFileFrom(most + ' ', {}).ok());
    }

    void whatDoesNotSayThatItIsAFileOfQkvmsIsNotWrittenOutAsOne_data()
    {
        QTest::addColumn<QByteArray>("answer");
        QTest::newRow("nothing") << QByteArray();
        QTest::newRow("the page of something that stands before the server")
            << QByteArray("<html><body>Sign in to go on</body></html>");
        QTest::newRow("a list") << QByteArray("[\"qkvm-export\"]");
        QTest::newRow("something of no kind") << QByteArray("{\"devices\": []}");
        QTest::newRow("something of another kind") << QByteArray("{\"format\": \"backup\", \"devices\": []}");
        QTest::newRow("a kind that is no word") << QByteArray("{\"format\": 7}");
    }

    void whatDoesNotSayThatItIsAFileOfQkvmsIsNotWrittenOutAsOne()
    {
        QFETCH(QByteArray, answer);
        QVERIFY(!core::wallFileFrom(answer, "0").ok());
    }

    void howMuchIsNotInAFileIsANumberOrNothing()
    {
        const QByteArray file = "{\"format\": \"qkvm-export\"}";
        QCOMPARE(core::wallFileFrom(file, "0").value().notOpened, 0);
        QCOMPARE(core::wallFileFrom(file, " 3 ").value().notOpened, 3);
        QCOMPARE(core::wallFileFrom(file, {}).value().notOpened, 0);
        QCOMPARE(core::wallFileFrom(file, "many").value().notOpened, 0);
        QCOMPARE(core::wallFileFrom(file, "-3").value().notOpened, 0);
    }

    void aFileSaysOfItselfWhetherItIsEncrypted()
    {
        QVERIFY(core::isSealedFile("{\"sealed\": {\"salt\": \"x\"}, \"program\": \"0.1.0\"}"));
        QVERIFY(!core::isSealedFile("{\"devices\": [], \"program\": \"0.1.0\"}"));
        QVERIFY(!core::isSealedFile("[\"sealed\"]"));
        QVERIFY(!core::isSealedFile("a letter to somebody, sealed"));
        QVERIFY(!core::isSealedFile({}));
        // What is larger than any file of QKVM's is not looked into.
        QVERIFY(core::isSealedFile("{\"sealed\": {}}" + QByteArray(1000, ' ')));
        QVERIFY(!core::isSealedFile("{\"sealed\": {}}" + QByteArray(core::largestFileBytes, ' ')));
    }

    static QJsonObject aFileAsTheServerTellsIt()
    {
        const QJsonObject kvm{{u"name"_s, u"Build server"_s},
                              {u"host"_s, u"192.168.1.21"_s},
                              {u"kind"_s, u"kvmd"_s},
                              {u"group"_s, u"Lab"_s},
                              {u"password"_s, true},
                              {u"tls"_s, u"pinned"_s},
                              {u"open_url"_s, u"https:"_s + u"//192.168.1.21/kvm/"_s},
                              {u"new"_s, true},
                              {u"same_as"_s, QString()}};
        const QJsonObject twin{{u"name"_s, u"NAS"_s},       {u"host"_s, u"192.168.1.22:8443"_s},
                               {u"kind"_s, u"jetkvm"_s},    {u"group"_s, QString()},
                               {u"password"_s, false},      {u"tls"_s, QString()},
                               {u"open_url"_s, QString()},  {u"new"_s, false},
                               {u"same_as"_s, u"Storage"_s}};
        const QJsonObject presses{{u"name"_s, u"Lock"_s}, {u"kind"_s, u"keys"_s}, {u"keys"_s, u"MetaLeft,KeyL"_s},
                                  {u"text"_s, QString()}, {u"enter"_s, false},    {u"danger"_s, false},
                                  {u"new"_s, false}};
        const QJsonObject types{{u"name"_s, u"Restart now"_s},
                                {u"kind"_s, u"text"_s},
                                {u"keys"_s, QString()},
                                {u"text"_s, u"shutdown /r /t 0"_s},
                                {u"enter"_s, true},
                                {u"danger"_s, true},
                                {u"new"_s, true}};
        return {{u"encrypted"_s, true},
                {u"secrets"_s, true},
                {u"made"_s, 1791676800},
                {u"program"_s, u"0.1.0"_s},
                {u"devices"_s, QJsonArray{kvm, twin}},
                {u"actions"_s, QJsonArray{presses, types}},
                {u"alerts"_s, QJsonObject{{u"enabled"_s, true},
                                          {u"address"_s, true},
                                          {u"host"_s, u"hooks.example.com"_s},
                                          {u"format"_s, u"slack"_s},
                                          {u"kinds"_s, QJsonArray{u"offline"_s, u"recovered"_s}}}},
                {u"wall"_s, QJsonObject{{u"interval"_s, 2.5}, {u"update_check"_s, false}}},
                {u"left_out"_s, QJsonArray{u"device 3: no address"_s}}};
    }

    void whatAFileHoldsIsReadAsTheServerTellsIt()
    {
        const base::Result<core::FileContents> read =
            core::fileContentsFrom(QJsonDocument(aFileAsTheServerTellsIt()).toJson());
        QVERIFY(read.ok());
        const core::FileContents& file = read.value();
        QVERIFY(file.encrypted);
        QVERIFY(file.hasSecrets);
        QCOMPARE(file.madeAt, qint64{1791676800});
        QCOMPARE(file.program, u"0.1.0"_s);
        QVERIFY(file.devices.has_value());
        const QList<core::FileDevice> devices = file.devices.value_or(QList<core::FileDevice>());
        QCOMPARE(devices.size(), 2);
        QCOMPARE(devices[0].name, u"Build server"_s);
        QCOMPARE(devices[0].host, u"192.168.1.21"_s);
        QCOMPARE(devices[0].group, u"Lab"_s);
        QVERIFY(devices[0].hasPassword);
        QCOMPARE(devices[0].tls, u"pinned"_s);
        QVERIFY(devices[0].openUrl.endsWith(u"192.168.1.21/kvm/"_s));
        QVERIFY(devices[0].isNew);
        QCOMPARE(devices[0].sameAs, QString());
        QVERIFY(!devices[1].isNew);
        QVERIFY(!devices[1].hasPassword);
        QCOMPARE(devices[1].sameAs, u"Storage"_s);
        const QList<core::FileAction> actions = file.actions.value_or(QList<core::FileAction>());
        QCOMPARE(actions.size(), 2);
        QVERIFY(!actions[0].types);
        QCOMPARE(actions[0].keys, u"MetaLeft,KeyL"_s);
        QVERIFY(!actions[0].isNew);
        QVERIFY(actions[1].types);
        QCOMPARE(actions[1].text, u"shutdown /r /t 0"_s);
        QVERIFY(actions[1].enter);
        QVERIFY(actions[1].danger);
        QVERIFY(actions[1].isNew);
        const core::FileAlerts alerts = file.alerts.value_or(core::FileAlerts());
        QVERIFY(alerts.enabled);
        QVERIFY(alerts.hasAddress);
        QCOMPARE(alerts.host, u"hooks.example.com"_s);
        QCOMPARE(alerts.kinds, (QStringList{u"offline"_s, u"recovered"_s}));
        const core::FileWallSettings wall = file.wall.value_or(core::FileWallSettings());
        QCOMPARE(wall.pictureEverySeconds.value_or(0), 2.5);
        QCOMPARE(wall.asksForNewerVersions.value_or(true), false);
        QCOMPARE(file.leftOut, (QStringList{u"device 3: no address"_s}));
    }

    void aPartAFileDoesNotHaveIsNotThere()
    {
        QJsonObject told = aFileAsTheServerTellsIt();
        told[u"encrypted"_s] = false;
        told[u"secrets"_s] = false;
        told[u"made"_s] = QJsonValue::Null;
        told[u"devices"_s] = QJsonValue::Null;
        told[u"actions"_s] = QJsonValue::Null;
        told[u"alerts"_s] = QJsonValue::Null;
        told[u"wall"_s] = QJsonObject();
        told[u"left_out"_s] = QJsonArray();
        const base::Result<core::FileContents> read = core::fileContentsFrom(QJsonDocument(told).toJson());
        QVERIFY(read.ok());
        QVERIFY(!read.value().encrypted);
        QVERIFY(!read.value().hasSecrets);
        QCOMPARE(read.value().madeAt, qint64{0});
        QVERIFY(!read.value().devices.has_value());
        QVERIFY(!read.value().actions.has_value());
        QVERIFY(!read.value().alerts.has_value());
        QVERIFY(!read.value().wall.has_value());
        QVERIFY(read.value().leftOut.isEmpty());
        // A part that is there and empty is there: a file of a wall without machines.
        told[u"devices"_s] = QJsonArray();
        const base::Result<core::FileContents> empty = core::fileContentsFrom(QJsonDocument(told).toJson());
        QVERIFY(empty.ok());
        QVERIFY(empty.value().devices.has_value());
        // A saved action that presses keys has no text and no Enter, whatever is said of it.
        QJsonObject odd = aFileAsTheServerTellsIt();
        QJsonArray actions = odd[u"actions"_s].toArray();
        QJsonObject presses = actions[0].toObject();
        presses[u"text"_s] = u"not typed"_s;
        presses[u"enter"_s] = true;
        actions[0] = presses;
        odd[u"actions"_s] = actions;
        const base::Result<core::FileContents> kept = core::fileContentsFrom(QJsonDocument(odd).toJson());
        QVERIFY(kept.ok());
        const QList<core::FileAction> read2 = kept.value().actions.value_or(QList<core::FileAction>());
        QCOMPARE(read2[0].text, QString());
        QVERIFY(!read2[0].enter);
    }

    void whatIsSaidOfAFileIsKeptWithinWhatAWallHolds()
    {
        QJsonObject told = aFileAsTheServerTellsIt();
        QJsonArray devices = told[u"devices"_s].toArray();
        QJsonObject kvm = devices[0].toObject();
        kvm[u"open_url"_s] = QString(5000, u'a');
        devices[0] = kvm;
        told[u"devices"_s] = devices;
        QJsonArray actions = told[u"actions"_s].toArray();
        QJsonObject types = actions[1].toObject();
        // As long as the server lets a text be, where each of its characters takes two places.
        types[u"text"_s] = QString(2 * core::longestTypedText, u'a');
        // (Keys are pressed or a text is typed: never both, whatever is said.)
        types[u"keys"_s] = u"KeyA"_s;
        actions[1] = types;
        told[u"actions"_s] = actions;
        QJsonObject alerts = told[u"alerts"_s].toObject();
        QJsonArray kinds;
        for (int number = 0; number < 20; ++number) {
            kinds.append(u"offline"_s);
        }
        alerts[u"kinds"_s] = kinds;
        told[u"alerts"_s] = alerts;
        const base::Result<core::FileContents> read = core::fileContentsFrom(QJsonDocument(told).toJson());
        QVERIFY(read.ok());
        QCOMPARE(read.value().devices.value_or(QList<core::FileDevice>())[0].openUrl.size(), core::longestAddress);
        const QList<core::FileAction> typed = read.value().actions.value_or(QList<core::FileAction>());
        // What an action types is whole: nothing of it is cut.
        QCOMPARE(typed[1].text.size(), 2 * core::longestTypedText);
        QCOMPARE(typed[1].keys, QString());
        QCOMPARE(read.value().alerts.value_or(core::FileAlerts()).kinds.size(), 8);
        // And a text longer than the server lets one be is not shown with its end cut off: a
        // reading with one in it is none. Also where it is the text of an action that presses keys.
        types[u"text"_s] = QString(2 * core::longestTypedText + 1, u'a');
        actions[1] = types;
        told[u"actions"_s] = actions;
        QVERIFY(!core::fileContentsFrom(QJsonDocument(told).toJson()).ok());
        actions[1] = aFileAsTheServerTellsIt()[u"actions"_s].toArray()[1];
        QJsonObject presses = actions[0].toObject();
        presses[u"text"_s] = QString(2 * core::longestTypedText + 1, u'a');
        actions[0] = presses;
        told[u"actions"_s] = actions;
        QVERIFY(!core::fileContentsFrom(QJsonDocument(told).toJson()).ok());
        actions[0] = aFileAsTheServerTellsIt()[u"actions"_s].toArray()[0];
        told[u"actions"_s] = actions;
        QVERIFY(core::fileContentsFrom(QJsonDocument(told).toJson()).ok());

        // A day that is none is no day.
        for (const double made : {5e12, -5.0, 0.0}) {
            told[u"made"_s] = made;
            QCOMPARE(core::fileContentsFrom(QJsonDocument(told).toJson()).value().madeAt, qint64{0});
        }
        // What is said of a file is as long as a file may be, and no longer.
        QJsonArray leftOut;
        for (int number = 0; number < 3000; ++number) {
            leftOut.append(QString(1000, u'a'));
        }
        told[u"left_out"_s] = leftOut;
        const QByteArray large = QJsonDocument(told).toJson(QJsonDocument::Compact);
        QVERIFY(large.size() > core::largestAnswerBytes && large.size() < core::largestFileBytes);
        QCOMPARE(core::fileContentsFrom(large).value().leftOut.size(), 200);
        QCOMPARE(core::fileContentsFrom(large + QByteArray(core::largestFileBytes, ' ')).error().code,
                 std::string("too_large"));
    }

    void wallSettingsThatAreNoneAreNotTaken_data()
    {
        QTest::addColumn<QJsonObject>("wall");
        QTest::addColumn<bool>("isThere");
        QTest::addColumn<double>("pace");
        QTest::newRow("a picture never") << QJsonObject{{u"interval"_s, 0}} << false << 0.0;
        QTest::newRow("a picture before it was asked for") << QJsonObject{{u"interval"_s, -2}} << false << 0.0;
        QTest::newRow("a picture every other hour") << QJsonObject{{u"interval"_s, 7200}} << false << 0.0;
        QTest::newRow("a pace in words") << QJsonObject{{u"interval"_s, u"2"_s}} << false << 0.0;
        QTest::newRow("a picture an hour") << QJsonObject{{u"interval"_s, 3600}} << true << 3600.0;
        QTest::newRow("asking in words") << QJsonObject{{u"update_check"_s, u"yes"_s}} << false << 0.0;
        QTest::newRow("a pace, and asking in words")
            << QJsonObject{{u"interval"_s, 4}, {u"update_check"_s, 1}} << true << 4.0;
    }

    void wallSettingsThatAreNoneAreNotTaken()
    {
        QFETCH(QJsonObject, wall);
        QFETCH(bool, isThere);
        QFETCH(double, pace);
        QJsonObject told = aFileAsTheServerTellsIt();
        told[u"wall"_s] = wall;
        const base::Result<core::FileContents> read = core::fileContentsFrom(QJsonDocument(told).toJson());
        QVERIFY(read.ok());
        QCOMPARE(read.value().wall.has_value(), isThere);
        const core::FileWallSettings settings = read.value().wall.value_or(core::FileWallSettings());
        QCOMPARE(settings.pictureEverySeconds.value_or(0), pace);
        // (Whether it asks for newer versions was said in none of these as a yes or a no.)
        QVERIFY(!settings.asksForNewerVersions.has_value());
    }

    void whatDoesNotTellOfAFileIsRefused_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("nothing on whether it is encrypted") << u"encrypted"_s << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("machines that are no list") << u"devices"_s << QJsonValue(u"all of them"_s);
        QTest::newRow("a machine without a name")
            << u"devices"_s << QJsonValue(QJsonArray{QJsonObject{{u"host"_s, u"192.168.1.21"_s}}});
        QTest::newRow("a machine without a place")
            << u"devices"_s << QJsonValue(QJsonArray{QJsonObject{{u"name"_s, u"Build server"_s}}});
        QTest::newRow("actions that are no list") << u"actions"_s << QJsonValue(7);
        QTest::newRow("an action of no kind")
            << u"actions"_s << QJsonValue(QJsonArray{QJsonObject{{u"name"_s, u"Lock"_s}, {u"kind"_s, u"macro"_s}}});
        QTest::newRow("an action without a name")
            << u"actions"_s << QJsonValue(QJsonArray{QJsonObject{{u"kind"_s, u"keys"_s}}});
        QTest::newRow("alerts that are a word") << u"alerts"_s << QJsonValue(u"on"_s);
        QTest::newRow("wall settings that are a list") << u"wall"_s << QJsonValue(QJsonArray{1});
        QTest::newRow("nothing on what was left out") << u"left_out"_s << QJsonValue(QJsonValue::Undefined);
    }

    void whatDoesNotTellOfAFileIsRefused()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QJsonObject told = aFileAsTheServerTellsIt();
        if (value.isUndefined()) {
            told.remove(field);
        } else {
            told[field] = value;
        }
        QVERIFY(!core::fileContentsFrom(QJsonDocument(told).toJson()).ok());
        QVERIFY(!core::fileContentsFrom("[]").ok());
    }

    void aFileWithMoreThanAWallHoldsIsRefused()
    {
        QJsonObject told = aFileAsTheServerTellsIt();
        QJsonArray many;
        for (int number = 0; number <= core::mostMachines; ++number) {
            many.append(QJsonObject{{u"name"_s, u"m"_s}, {u"host"_s, u"h"_s}});
        }
        told[u"devices"_s] = many;
        QCOMPARE(core::fileContentsFrom(QJsonDocument(told).toJson()).error().code, std::string("too_large"));
        many.removeLast();
        told[u"devices"_s] = many;
        QVERIFY(core::fileContentsFrom(QJsonDocument(told).toJson()).ok());
        // And so is one with more saved actions than a QKVM keeps.
        QJsonArray actions;
        for (int number = 0; number <= core::mostSavedActions; ++number) {
            actions.append(QJsonObject{{u"name"_s, u"a"_s}, {u"kind"_s, u"keys"_s}});
        }
        told[u"actions"_s] = actions;
        QCOMPARE(core::fileContentsFrom(QJsonDocument(told).toJson()).error().code, std::string("too_large"));
        actions.removeLast();
        told[u"actions"_s] = actions;
        QVERIFY(core::fileContentsFrom(QJsonDocument(told).toJson()).ok());
        // What was left out is told up to two hundred entries.
        QJsonArray leftOut;
        for (int number = 0; number < 300; ++number) {
            leftOut.append(u"entry"_s);
        }
        told[u"left_out"_s] = leftOut;
        QCOMPARE(core::fileContentsFrom(QJsonDocument(told).toJson()).value().leftOut.size(), 200);
    }

    void whatWasTakenInIsCounted()
    {
        const base::Result<core::Taken> taken = core::takenFrom(
            R"({"devices": 3, "actions": 1, "alerts": false, "wall": true, "left_out": ["device 4: no address"]})");
        QVERIFY(taken.ok());
        QCOMPARE(taken.value().devices, 3);
        QCOMPARE(taken.value().actions, 1);
        QVERIFY(!taken.value().alerts);
        QVERIFY(taken.value().wall);
        const base::Result<core::Taken> other =
            core::takenFrom(R"({"devices": 0, "actions": 0, "alerts": true, "wall": false, "left_out": []})");
        QVERIFY(other.ok());
        QVERIFY(other.value().alerts);
        QVERIFY(!other.value().wall);
        QVERIFY(!core::takenFrom(R"({"devices": "3", "actions": 1, "alerts": false, "wall": true})").ok());
        QVERIFY(!core::takenFrom(R"({"devices": 3, "alerts": false, "wall": true})").ok());
        QVERIFY(!core::takenFrom(R"({"devices": 3, "actions": 1, "alerts": 0, "wall": true})").ok());
        QVERIFY(!core::takenFrom(R"({"devices": 3, "actions": 1, "alerts": false})").ok());
        QVERIFY(!core::takenFrom("[]").ok());
    }

    void theCallsForAFileCarryWhatTheServerAsksFor()
    {
        QCOMPARE(core::wallFileParts(), (QStringList{u"devices"_s, u"actions"_s, u"alerts"_s, u"wall"_s}));
        const QJsonObject asked =
            QJsonDocument::fromJson(core::exportBody(u"my own"_s, u"for the file"_s, {u"devices"_s, u"wall"_s}))
                .object();
        QCOMPARE(asked, (QJsonObject{{u"password"_s, u"my own"_s},
                                     {u"passphrase"_s, u"for the file"_s},
                                     {u"parts"_s, QJsonArray{u"devices"_s, u"wall"_s}}}));
        // Reading a file names no parts; taking one in names them, also where none is named.
        const QByteArray file = "{\"devices\": []}";
        QCOMPARE(QJsonDocument::fromJson(core::importBody(file, {}, nullptr)).object(),
                 (QJsonObject{{u"file"_s, u"{\"devices\": []}"_s}, {u"passphrase"_s, QString()}}));
        const QStringList take{u"actions"_s};
        QCOMPARE(QJsonDocument::fromJson(core::importBody(file, u"of the file"_s, &take)).object(),
                 (QJsonObject{{u"file"_s, u"{\"devices\": []}"_s},
                              {u"passphrase"_s, u"of the file"_s},
                              {u"take"_s, QJsonArray{u"actions"_s}}}));
        const QStringList none;
        QVERIFY(QJsonDocument::fromJson(core::importBody(file, {}, &none)).object().contains(u"take"_s));
    }
};

QTEST_APPLESS_MAIN(TestServerTypes)
#include "test_server_types.moc"
