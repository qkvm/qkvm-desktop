#include <functional>
#include <memory>
#include <optional>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "../../core/tests/memory_secrets.h"
#include <qkvm/core/exchange.h>
#include <qkvm/core/server_client.h>

#include "fixture_phase.h"

#include "actions_view.h"
#include "application.h"
#include "bulk_run.h"
#include "control.h"
#include "details_view.h"
#include "keys_by_hand.h"
#include "machine_menu.h"
#include "media_view.h"
#include "server_source.h"
#include "shared_live.h"
#include "wall_cards.h"
#include "wall_model.h"
#include "wall_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

// Run by tools/test_web_compat.py against the real Python app with disposable devices.
// Kept separate from ctest: a normal unit run must never discover or use a user's server.
//
// The cases after the live line's (b11) accept what 0.3.0 added against the server itself, each family by the web's own
// stand-in: what the server answers is read through the program's own client, and what the program shows of it is what
// its own pieces make of that (the machine's menu, its card, the boxes and views), and what it sends is seen at the
// stand-in (the fixture's looks). Where the server is one that lacks a feature (0.2.2), the case asks that nothing of
// it is offered. What the program showed and sent is kept in desktop-observations.json beside the fixture's records.
class TestWebCompat : public QObject {
    Q_OBJECT
private:
    QJsonObject m_fixture;
    core::ServerClient m_client;
    // What the program showed and sent in the 0.3.0 cases, by case.
    QJsonObject m_desktop;
    // Every code of the server's that a case met in a refusal (its words, checked against Words.qml at the end).
    QSet<QString> m_codes;
    int m_looks = 0;

    QString id(const char* name) const { return m_fixture.value(QLatin1StringView(name)).toString(); }
    QJsonObject phase(const QString& name) { return fixturePhase(QDir(id("phases")), name); }
    // What the fixture's stand-in of this name was sent since the last look at it.
    QJsonObject look(const QString& fake) { return phase(u"look-%1-%2"_s.arg(fake).arg(++m_looks)); }
    // Has the fixture do something to the server or a stand-in meanwhile.
    bool meanwhile(const QString& what) { return phase(u"do-%1-%2"_s.arg(what).arg(++m_looks))[u"ok"_s].toBool(); }
    // Whether the server is of a version that has this (0.3.0's features: the fixture's expectations).
    bool expects(const char* feature) const
    {
        return m_fixture[u"expect"_s].toObject()[QLatin1StringView(feature)].toBool();
    }
    void heard(const base::Error& error)
    {
        if (!error.code.empty() && error.code.find(':') == std::string::npos) {
            m_codes.insert(QString::fromStdString(error.code));
        }
    }
    static QJsonObject said(const base::Error& error)
    {
        return {{u"code"_s, QString::fromStdString(error.code)}, {u"message"_s, QString::fromStdString(error.message)}};
    }

    std::optional<core::Wall> wallNow()
    {
        std::optional<base::Result<core::Wall>> wall;
        m_client.askWall([&](auto result) { wall = std::move(result); });
        if (!QTest::qWaitFor([&] { return wall.has_value(); }, 15'000) || !wall || !wall->ok()) {
            return std::nullopt;
        }
        return wall->value();
    }
    std::optional<core::Machine> machineOf(const QString& key)
    {
        const auto wall = wallNow();
        if (!wall) {
            return std::nullopt;
        }
        for (const auto& machine : wall->machines) {
            if (machine.id == key) {
                return machine;
            }
        }
        return std::nullopt;
    }
    // The machine's status once it is as `ready` asks, asked every half second; nothing when it was not within the
    // time.
    std::optional<core::Status> statusWhen(const QString& key, const std::function<bool(const core::Status&)>& ready,
                                           int withinMs = 60'000, const std::function<void()>& meanwhile = {})
    {
        QElapsedTimer clock;
        clock.start();
        std::optional<core::Status> last;
        while (clock.elapsed() < withinMs) {
            if (meanwhile) {
                meanwhile();
            }
            std::optional<base::Result<QHash<QString, core::Status>>> status;
            m_client.askStatus([&](auto result) { status = std::move(result); });
            if (QTest::qWaitFor([&] { return status.has_value(); }, 15'000) && status && status->ok() &&
                status->value().contains(key)) {
                last = status->value().value(key);
                if (ready(*last)) {
                    return last;
                }
            }
            QTest::qWait(500);
        }
        qWarning() << "STATUS never ready" << key << (last ? last->state : u"none"_s)
                   << (last && last->capabilities ? last->capabilities->join(u',') : QString());
        return std::nullopt;
    }
    base::Result<core::Done> actNow(const QString& machine, const core::Action& action,
                                    core::ServerClient* other = nullptr, int withinMs = 300'000)
    {
        std::optional<base::Result<core::Done>> answer;
        (other != nullptr ? *other : m_client).act(machine, action, [&](auto result) { answer = std::move(result); });
        if (!QTest::qWaitFor([&] { return answer.has_value(); }, withinMs) || !answer) {
            return base::Error{"test:no_answer", "No answer within the test's time."};
        }
        if (!answer->ok()) {
            heard(answer->error());
        }
        return *answer;
    }
    base::Result<core::Snapshot> snapshotNow(const QString& machine, int width = 640)
    {
        std::optional<base::Result<core::Snapshot>> answer;
        m_client.askSnapshot(machine, width, 1, [&](auto result) { answer = std::move(result); });
        if (!QTest::qWaitFor([&] { return answer.has_value(); }, 20'000) || !answer) {
            return base::Error{"test:no_answer", "No answer within the test's time."};
        }
        return *answer;
    }
    std::unique_ptr<core::ServerClient> signedIn(const QString& name)
    {
        auto client = std::make_unique<core::ServerClient>();
        if (!client->setAddress(id("url"))) {
            return {};
        }
        std::optional<base::Result<core::Account>> account;
        client->signIn(name, id("password"), {}, false, [&](auto result) { account = std::move(result); });
        if (!QTest::qWaitFor([&] { return account.has_value(); }, 15'000) || !account || !account->ok()) {
            return {};
        }
        return client;
    }
    // The menu of a machine as an admin of the server sees it, in person (who may have its media).
    ui::Menu menuOf(const core::Machine& machine, const core::Status& status,
                    const QList<core::SavedAction>& saved = {})
    {
        ui::MenuOf of;
        of.machine = machine;
        of.status = status;
        of.saved = saved;
        of.media = true;
        return ui::menuFor(of);
    }
    static QJsonArray rowsOf(const ui::Menu& menu)
    {
        QJsonArray rows;
        for (const auto& section : menu.sections) {
            for (const auto& row : section.rows) {
                rows.append(row.enabled ? row.id : row.id + u" (not offered: "_s + row.hint + u')');
            }
        }
        return rows;
    }
    // The key events an RFB stand-in got that went down, by their X11 keysyms; and an AMI console's reports' keys.
    static QList<int> downs(const QJsonObject& seen)
    {
        QList<int> found;
        for (const auto& key : seen[u"keys"_s].toArray()) {
            const QJsonArray event = key.toArray();
            if (event.size() == 2 && event.at(1).isBool() && event.at(1).toBool()) {
                found.append(event.at(0).toInt());
            } else if (event.size() == 2 && event.at(1).isArray()) {
                for (const auto& usage : event.at(1).toArray()) {
                    found.append(event.at(0).toInt() << 8 | usage.toInt());
                }
            }
        }
        return found;
    }

    std::unique_ptr<core::Live> open(const QString& machine, core::ServerClient* other = nullptr)
    {
        std::unique_ptr<core::Live> result;
        bool done = false;
        (other != nullptr ? *other : m_client).openLive(machine, {640, 15}, [&](auto answer) {
            if (answer) {
                result = std::move(answer).value();
            }
            done = true;
        });
        if (!QTest::qWaitFor([&] { return done; }, 15'000)) {
            return {};
        }
        if (result && !QTest::qWaitFor([&] { return result->isOpen(); }, 15'000)) {
            return {};
        }
        return result;
    }

private slots:
    void initTestCase()
    {
        QFile file(qEnvironmentVariable("QKVM_WEB_FIXTURE"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        m_fixture = QJsonDocument::fromJson(file.readAll()).object();
        const QUrl url(id("url"));
        QCOMPARE(url.host(), u"127.0.0.1"_s);
        QVERIFY(url.port() > 0 && url.port() != 8787 && url.port() != 8788);
        QVERIFY(m_client.setAddress(url.toString()));
        std::optional<base::Result<core::Account>> answer;
        m_client.signIn(u"admin"_s, id("password"), {}, false, [&](auto result) { answer = std::move(result); });
        QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 15'000);
        if (!answer) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(answer->ok());
    }

    // What the program showed and sent, beside the fixture's records of what the server and the stand-ins were asked.
    void cleanupTestCase()
    {
        QFile written(QFileInfo(id("phases")).dir().filePath(u"desktop-observations.json"_s));
        if (written.open(QIODevice::WriteOnly)) {
            written.write(QJsonDocument(m_desktop).toJson());
        }
    }

    void nativeMfaChallengeAndAuthenticatorCode()
    {
        core::ServerClient mfa;
        QVERIFY(mfa.setAddress(id("url")));
        std::optional<base::Result<core::Account>> result;
        mfa.signIn(u"native_mfa"_s, id("password"), {}, false, [&](auto answer) { result = std::move(answer); });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 15'000);
        if (!result) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(!result->ok());
        QCOMPARE(result->error().code, std::string("code_required"));
        QVERIFY(mfa.session().isEmpty());
        const auto response = phase(u"mfa-code"_s);
        QVERIFY(response.value(u"ok"_s).toBool());
        result.reset();
        mfa.signIn(u"native_mfa"_s, id("password"), response.value(u"code"_s).toString(), false,
                   [&](auto answer) { result = std::move(answer); });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 15'000);
        if (!result) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(result->ok());
        QCOMPARE(result->value().role, u"viewer"_s);
        QVERIFY(!mfa.session().isEmpty());
    }

    void requiredMfaEnrollmentStopsTheNativeSource()
    {
        const QUrl url(id("enrollment_url"));
        QCOMPARE(url.host(), u"127.0.0.1"_s);
        QVERIFY(url.port() > 0 && url.port() != 8787 && url.port() != 8788);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        core::Settings settings(profile.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        ui::ServerSource source(u"fixture"_s, settings, secrets);
        source.connectTo(url.toString());
        QTRY_COMPARE(source.shown().stage, u"signIn"_s);
        source.signIn(u"native_enroll"_s, id("password"), {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(source.shown().problem.contains(u"two-factor"_s), 15'000);
        QVERIFY(!source.isAtTheWall());
        QVERIFY(source.feed().machines().isEmpty());
        QVERIFY(secrets.all.isEmpty());
    }

    void nativeInventoryJpegAndAdvertisedMouseModes()
    {
        std::optional<base::Result<core::Wall>> wall;
        m_client.askWall([&](auto result) { wall = std::move(result); });
        QTRY_VERIFY(wall.has_value());
        if (!wall) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(wall->ok());
        QVERIFY(!wall->value().machines.isEmpty());
        QVERIFY(wall->value().machines.first().capabilities.has_value());
        for (const auto* name : {"absolute", "relative"}) {
            auto live = open(id(name));
            QVERIFY(live);
            QTRY_VERIFY(live->pointerMode() != core::Live::Pointer::Unknown);
            const bool relative = QByteArray(name) == "relative";
            QCOMPARE(live->pointerMode(), relative ? core::Live::Pointer::Relative : core::Live::Pointer::Absolute);
            bool decoded = false;
            connect(live.get(), &core::Live::pictureCame, this, [&](const core::Live::Picture& picture) {
                decoded = !picture.h264 && !QImage::fromData(picture.data, "JPEG").isNull();
            });
            // A still screen is deduplicated by the real server. Subscribe, then request it.
            live->askForAKeyFrame();
            QTRY_VERIFY_WITH_TIMEOUT(decoded, 15'000);
            ui::ControlView view;
            ui::KeysByHand keys;
            ui::MachineControl control(view, keys);
            control.setAllowed(true);
            control.setInFront(true);
            control.have(live.get(), id(name));
            QCOMPARE(view.isRelative(), relative);
            QVERIFY(!view.mouseSelectable());
            control.take();
            QTRY_VERIFY_WITH_TIMEOUT(control.isOn(), 15'000);
            // Both are attempted; unsupported messages must never reach the fake device.
            live->pointerAt(1200, 2300);
            live->pointerBy(7, -3);
            live->button(1, true);
            live->button(1, false);
            QTest::qWait(200);
            control.giveBack();
            QTRY_VERIFY(!view.isBusy());
        }
    }

    void admissionAfterFiveSecondsAndImmediateCancellation()
    {
        auto live = open(id("slow"));
        QVERIFY(live);
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl control(view, keys);
        control.setAllowed(true);
        control.setInFront(true);
        control.have(live.get(), id("slow"));
        control.take();
        QTest::qWait(5100);
        QVERIFY(control.isAsking());
        QTRY_VERIFY_WITH_TIMEOUT(control.isOn(), 15'000);
        control.giveBack();
        QTRY_VERIFY(!view.isBusy());
        control.take();
        QVERIFY(control.isAsking());
        control.giveBack();
        QVERIFY(!control.isOn() && !control.isAsking() && !keys.swallows());
        QTest::qWait(7000);
        QVERIFY(!control.isOn() && !live->hasControl() && !keys.swallows());
    }

    void nativeFloatingKeyboardUsesOnlyItsOwnedLiveChannel()
    {
        auto live = open(id("absolute"));
        QVERIFY(live);
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl control(view, keys);
        control.setAllowed(true);
        control.setInFront(true);
        control.have(live.get(), id("absolute"));
        ui::WallView wall;
        ui::WallTile tile;
        tile.key = id("absolute");
        tile.name = u"Fixture keyboard"_s;
        tile.state = u"online"_s;
        wall.machines()->setTiles({tile});
        wall.enlarge(tile.key);
        ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlEngine engine;
        QStringList complaints;
        connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError>& errors) {
            for (const auto& error : errors)
                complaints.append(error.toString());
        });
        QQmlComponent component(&engine);
        component.loadFromModule("Qkvm", "Enlarged");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{u"view"_s, QVariant::fromValue(&wall)},
                                                   {u"actions"_s, QVariant::fromValue(&actions)},
                                                   {u"control"_s, QVariant::fromValue(&view)}}));
        auto* console = qobject_cast<QQuickItem*>(root.get());
        QVERIFY(console != nullptr);
        QQuickWindow window;
        window.resize(1280, 800);
        console->setParentItem(window.contentItem());
        console->setSize(QSizeF(1280, 800));
        window.show();
        window.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        // Repeater delegates belong to the visual tree, not necessarily the QObject tree.
        const std::function<QQuickItem*(QQuickItem*, const QString&)> findItem =
            [&](QQuickItem* under, const QString& name) -> QQuickItem* {
            if (under->objectName() == name)
                return under;
            for (auto* child : under->childItems()) {
                if (auto* found = findItem(child, name))
                    return found;
            }
            return nullptr;
        };
        const auto click = [&](const QString& name) {
            QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
            window.update();
            if (!QTest::qWaitFor([&] { return !frames.isEmpty(); }))
                return false;
            auto* button = findItem(console, name);
            if (button == nullptr || !button->isVisible() || !button->isEnabled())
                return false;
            QSignalSpy clicked(button, SIGNAL(clicked()));
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                              button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
            return clicked.size() == 1;
        };
        control.take();
        QTRY_VERIFY(control.isOn());
        QVERIFY(click(u"controlKeyboard"_s));
        const auto folder = QFileInfo(id("phases")).dir();
        QVERIFY(window.grabWindow().save(folder.filePath(u"native-keyboard-opening.png"_s)));
        QTRY_VERIFY(view.localInput());
        QVERIFY(live->hasControl() && !keys.swallows());
        auto* keyboard = console->findChild<QQuickItem*>(u"floatingKeyboard"_s);
        QVERIFY(keyboard != nullptr && keyboard->isVisible());
        QVERIFY(click(u"screenKey:225"_s));
        QVERIFY(click(u"screenKey:5"_s));
        QTest::qWait(200); // Allow the fixture's HID receiver to observe the completed chord.
        QVERIFY(window.grabWindow().save(folder.filePath(u"native-keyboard.png"_s)));
        QVERIFY(click(u"keyboardClose"_s));
        QTRY_VERIFY(!view.localInput() && keys.swallows());
        // Main's Actions/confirmation dialogs use the same pause, retaining ownership.
        QVERIFY(console->setProperty("externalPanel", true));
        QTRY_VERIFY(view.localInput() && !keys.swallows());
        QVERIFY(live->hasControl());
        keys.press({0x1e, false, 0x41});
        keys.release({0x1e, false, 0x41}); // A local menu key must not reach the device.
        QVERIFY(console->setProperty("externalPanel", false));
        QTRY_VERIFY(!view.localInput() && keys.swallows());
        control.giveBack();
        QTRY_VERIFY(!view.isBusy());
        view.tapKeys({6}); // No owner: this must never reach the device.
        QVERIFY(!keyboard->isVisible());
        QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(u'\n')));
    }

    void confirmedTextReleasesSendsOnceAndReacquires()
    {
        auto live = open(id("absolute"));
        QVERIFY(live);
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl control(view, keys);
        control.setAllowed(true);
        control.setInFront(true);
        control.have(live.get(), id("absolute"));
        control.take();
        QTRY_VERIFY(control.isOn());
        control.pauseForText(id("absolute"));
        QVERIFY(live->hasControl() && !keys.swallows());
        control.cancelText();
        QVERIFY(control.isOn() && live->hasControl());
        control.pauseForText(id("absolute"));
        int calls = 0;
        std::optional<base::Result<core::Done>> answer;
        control.textAfterRelease(
            id("absolute"),
            [&](const auto& finish) {
                ++calls;
                m_client.act(id("absolute"), core::Action::type(u"native fixture"_s, false), [&, finish](auto result) {
                    const bool success = result.ok();
                    answer = std::move(result);
                    finish(success);
                });
            },
            [] { QFAIL("Release was not acknowledged"); });
        QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 15'000);
        if (!answer) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(answer->ok());
        QCOMPARE(calls, 1);
        QTRY_VERIFY(control.isOn());
        control.giveBack();
        QTRY_VERIFY(!view.isBusy());
    }

    void browserOwnershipBlocksNativeControlAndText()
    {
        auto live = open(id("contested"));
        QVERIFY(live);
        QSignalSpy replies(live.get(), &core::Live::controlChanged);
        live->askControl(true);
        QTRY_VERIFY(!replies.isEmpty());
        QVERIFY(!live->hasControl());
        QCOMPARE(replies.first().at(1).toString(), u"taken"_s);
        std::optional<base::Result<core::Done>> answer;
        m_client.act(id("contested"), core::Action::type(u"must not arrive"_s, false),
                     [&](auto result) { answer = std::move(result); });
        QTRY_VERIFY(answer.has_value());
        if (!answer) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(!answer->ok());
        QCOMPARE(QString::fromStdString(answer->error().code), u"busy"_s);
    }

    void twoNativeWindowsShareFeedButNotControl()
    {
        ui::SharedLive pool;
        std::unique_ptr<core::Live> first;
        std::unique_ptr<core::Live> second;
        pool.open(u"fixture"_s, m_client, m_client, id("relative"), {640, 15}, [&](auto result) {
            if (result)
                first = std::move(result).value();
        });
        pool.open(u"fixture"_s, m_client, m_client, id("relative"), {640, 15}, [&](auto result) {
            if (result)
                second = std::move(result).value();
        });
        QTRY_VERIFY(first && second && first->isOpen() && second->isOpen());
        QTRY_COMPARE(second->pointerMode(), core::Live::Pointer::Relative);
        first->askControl(true);
        QTRY_VERIFY(first->hasControl());
        QSignalSpy refused(second.get(), &core::Live::controlChanged);
        second->askControl(true);
        QTRY_COMPARE(refused.size(), 1);
        QVERIFY(first->hasControl() && !second->hasControl());
        QCOMPARE(refused.first().at(1).toString(), u"desktop:control_busy"_s);
        second->askControl(false);
        QVERIFY(first->hasControl());
        first->askControl(false);
        QTRY_VERIFY(!first->hasControl());
    }

    void nonAdminCannotForceTakeoverEvenWithTheCorrectOwner()
    {
        core::ServerClient client;
        QVERIFY(client.setAddress(id("url")));
        std::optional<base::Result<core::Account>> account;
        client.signIn(u"native_operator"_s, id("password"), {}, false,
                      [&](auto result) { account = std::move(result); });
        QTRY_VERIFY(account.has_value());
        QVERIFY(account && account->ok());
        auto live = open(id("ownership"), &client);
        QVERIFY(live);
        QSignalSpy answers(live.get(), &core::Live::controlChanged);
        live->askControl(true);
        QTRY_COMPARE(live->controlOwner().name, u"browser_owner"_s);
        answers.clear();
        live->takeOver(live->controlOwner().reference);
        QTRY_COMPARE(answers.size(), 1);
        QVERIFY(!live->hasControl());
        QCOMPARE(answers.first().at(1).toString(), u"taken"_s);
    }

    void nativeAndBrowserTakeoverReleaseInputAndRecheckTheHolderAndAdmin()
    {
        core::ServerClient client;
        QVERIFY(client.setAddress(id("url")));
        std::optional<base::Result<core::Account>> account;
        client.signIn(u"native_admin"_s, id("password"), {}, false, [&](auto result) { account = std::move(result); });
        QTRY_VERIFY(account.has_value());
        QVERIFY(account && account->ok());
        auto live = open(id("ownership"), &client);
        QVERIFY(live);
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl control(view, keys);
        control.setAllowed(true);
        control.setTakeoverAllowed(true);
        control.setInFront(true);
        control.have(live.get(), id("ownership"));
        control.take();
        QTRY_COMPARE(view.ownerName(), u"browser_owner"_s);
        QTRY_VERIFY(!view.isBusy() && view.takeoverPossible());
        view.take();
        QVERIFY(view.takeoverPending());
        view.answerTakeover(false);
        QVERIFY(phase(u"cancelled-takeover"_s).value(u"ok"_s).toBool());
        QVERIFY(!view.isOn());
        view.take();
        view.answerTakeover(true);
        QTRY_VERIFY(view.isOn());
        QVERIFY(view.ownerName().isEmpty());
        QVERIFY(phase(u"native-took-over"_s).value(u"ok"_s).toBool());
        keys.press({0x2A, false, 0xA0}); // Simulated Left Shift; no real keyboard hook.
        QVERIFY(phase(u"browser-cancelled"_s).value(u"ok"_s).toBool());
        QVERIFY(view.isOn());
        QVERIFY(phase(u"browser-takes-over"_s).value(u"ok"_s).toBool());
        QTRY_VERIFY(!view.isOn());
        QVERIFY(!keys.swallows());
        QTRY_COMPARE(view.ownerName(), u"browser_owner"_s);
        keys.release({0x2A, false, 0xA0});
        keys.press({0x30, false, 0x42}); // A displaced native window must not type B.
        keys.release({0x30, false, 0x42});
        QTRY_VERIFY(!view.isBusy() && view.takeoverPossible());
        view.take();
        QVERIFY(view.takeoverPending());
        QVERIFY(phase(u"replace-owner"_s).value(u"ok"_s).toBool());
        // No owner broadcast exists: the server must reject the old confirmation itself.
        view.answerTakeover(true);
        QTRY_COMPARE(view.ownerName(), u"replacement"_s);
        QVERIFY(!view.isOn());
        QTRY_VERIFY(!view.isBusy() && view.takeoverPossible());
        view.take();
        QCOMPARE(view.takeoverOwner(), u"replacement"_s);
        view.answerTakeover(false);
        QVERIFY(phase(u"restore-browser-owner"_s).value(u"ok"_s).toBool());
        control.take(); // Refresh with an ordinary request, never an automatic takeover.
        QTRY_COMPARE(view.ownerName(), u"browser_owner"_s);
        QTRY_VERIFY(!view.isBusy() && view.takeoverPossible());
        view.take();
        QVERIFY(phase(u"arm-takeover-delay"_s).value(u"ok"_s).toBool());
        view.answerTakeover(true);
        QVERIFY(view.isAsking());
        QVERIFY(phase(u"demote-during-takeover"_s).value(u"ok"_s).toBool());
        QTRY_VERIFY(!view.isAsking() && !view.isBusy());
        QVERIFY(!view.isOn());
        QVERIFY(view.problem().contains(u"only an admin"_s));
        QVERIFY(phase(u"demotion-protected-holder"_s).value(u"ok"_s).toBool());
        control.setTakeoverAllowed(false);
        QVERIFY(!view.takeoverPossible());
    }

    void plainAndSealedPortableFilesAreReadByBothSides()
    {
        // This PC's own wall takes the families it has drivers for (kvmd, JetKVM, servers over Redfish): the server's
        // PCs with Intel AMT, its VNC servers and its Proxmox guests are left out of the local reading, each with a
        // sentence. How many it takes, by the kinds the plain file says (it is the first read).
        const int all = m_fixture[u"device_count"_s].toInt();
        int takenHere = -1;
        for (const QString& passphrase : {QString(), u"fabricated export passphrase"_s}) {
            std::optional<base::Result<core::WallFile>> exported;
            m_client.exportWall(id("password"), passphrase, {u"devices"_s, u"actions"_s, u"wall"_s},
                                [&](auto result) { exported = std::move(result); });
            QTRY_VERIFY_WITH_TIMEOUT(exported.has_value(), 20'000);
            if (!exported) {
                QFAIL("The asynchronous operation did not return a result.");
            }
            QVERIFY(exported->ok());
            QCOMPARE(exported->value().sealed, !passphrase.isEmpty());
            std::optional<base::Result<core::FileContents>> read;
            m_client.readFile(exported->value().text, passphrase, [&](auto result) { read = std::move(result); });
            QTRY_VERIFY_WITH_TIMEOUT(read.has_value(), 20'000);
            if (!read) {
                QFAIL("The asynchronous operation did not return a result.");
            }
            QVERIFY(read->ok());
            QVERIFY(read->value().devices.has_value());
            QCOMPARE(read->value().devices->size(), all);
            if (passphrase.isEmpty()) {
                takenHere = 0;
                for (const auto& device : QJsonDocument::fromJson(exported->value().text)[u"devices"_s].toArray()) {
                    const QString kind = device[u"kind"_s].toString();
                    takenHere += kind == u"kvmd"_s || kind == u"jetkvm"_s || kind == u"redfish"_s ? 1 : 0;
                }
                QVERIFY(takenHere >= 5);
            }
            // The native local codec opens a real server file, then the server reads its rewrite.
            const auto local = core::exchange::read(exported->value().text, passphrase);
            QVERIFY(local);
            QVERIFY(local.value().devices.has_value());
            QCOMPARE(local.value().devices->size(), takenHere);
            QCOMPARE(local.value().leftOut.size(), all - takenHere);
            m_desktop[u"file_left_out_here"_s] = QJsonArray::fromStringList(local.value().leftOut);
            const auto written = core::exchange::write(local.value(), passphrase);
            QVERIFY(written);
            QVERIFY(written.value().notInIt().isEmpty());
            read.reset();
            m_client.readFile(written.value().file(core::exchange::NotInIt::wasLookedAt), passphrase,
                              [&](auto result) { read = std::move(result); });
            QTRY_VERIFY_WITH_TIMEOUT(read.has_value(), 20'000);
            if (!read) {
                QFAIL("The asynchronous operation did not return a result.");
            }
            QVERIFY(read->ok());
            QCOMPARE(read->value().devices->size(), takenHere);
            if (!passphrase.isEmpty()) {
                const auto wrong = core::exchange::read(exported->value().text, u"wrong fabricated passphrase"_s);
                QVERIFY(!wrong);
                read.reset();
                m_client.readFile(exported->value().text, u"wrong fabricated passphrase"_s,
                                  [&](auto result) { read = std::move(result); });
                QTRY_VERIFY_WITH_TIMEOUT(read.has_value(), 20'000);
                if (!read) {
                    QFAIL("The asynchronous operation did not return a result.");
                }
                QVERIFY(!read->ok());
                QCOMPARE(read->error().code, wrong.error().code);
            } else {
                QByteArray boundary = exported->value().text;
                boundary.append(QByteArray(4'000'000 - boundary.size(), ' '));
                QVERIFY(core::exchange::read(boundary, {}));
                read.reset();
                m_client.readFile(boundary, {}, [&](auto result) { read = std::move(result); });
                QTRY_VERIFY_WITH_TIMEOUT(read.has_value(), 20'000);
                if (!read) {
                    QFAIL("The asynchronous operation did not return a result.");
                }
                QVERIFY(read->ok());
                boundary.append(' ');
                const auto oversized = core::exchange::read(boundary, {});
                QVERIFY(!oversized);
                QCOMPARE(oversized.error().code, std::string("too_large"));
                read.reset();
                m_client.readFile(boundary, {}, [&](auto result) { read = std::move(result); });
                QTRY_VERIFY_WITH_TIMEOUT(read.has_value(), 20'000);
                if (!read) {
                    QFAIL("The asynchronous operation did not return a result.");
                }
                QVERIFY(!read->ok());
                QCOMPARE(read->error().code, std::string("too_large"));
            }
        }
    }

    void roleAndDevicePermissionChangesRevokeNativeControl()
    {
        core::ServerClient operatorClient;
        QVERIFY(operatorClient.setAddress(id("url")));
        std::optional<base::Result<core::Account>> account;
        operatorClient.signIn(u"native_operator"_s, id("password"), {}, false,
                              [&](auto result) { account = std::move(result); });
        QTRY_VERIFY_WITH_TIMEOUT(account.has_value(), 15'000);
        if (!account) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(account->ok());
        auto live = open(id("absolute"), &operatorClient);
        QVERIFY(live);
        live->askControl(true);
        QTRY_VERIFY(live->hasControl());
        live->key(0xE0, true);
        QTest::qWait(100);
        QVERIFY(phase(u"demote"_s).value(u"ok"_s).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!live->hasControl(), 15'000);
        std::optional<base::Result<core::Session>> session;
        operatorClient.askSession([&](auto result) { session = std::move(result); });
        QTRY_VERIFY(session.has_value());
        if (!session) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(session->ok());
        const auto refreshedAccount = session->value().account;
        if (!refreshedAccount) {
            QFAIL("The refreshed session has no account.");
        }
        QCOMPARE(refreshedAccount->role, u"viewer"_s);
        std::optional<base::Result<core::Done>> action;
        operatorClient.act(id("absolute"), core::Action::type(u"revoked must not arrive"_s, false),
                           [&](auto result) { action = std::move(result); });
        QTRY_VERIFY(action.has_value());
        if (!action) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(!action->ok());
        QVERIFY(phase(u"hide-device"_s).value(u"ok"_s).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!live->isOpen(), 15'000);
        std::optional<base::Result<core::Wall>> wall;
        operatorClient.askWall([&](auto result) { wall = std::move(result); });
        QTRY_VERIFY(wall.has_value());
        if (!wall) {
            QFAIL("The asynchronous operation did not return a result.");
        }
        QVERIFY(wall->ok());
        QVERIFY(wall->value().machines.isEmpty());
    }

    void historyAndActivityAgreeWithTheBrowser()
    {
        ui::DetailsView details;
        details.bind(&m_client, id("contested"), u"joint-admin"_s, true, true);
        details.setOpened(true);
        QTRY_VERIFY_WITH_TIMEOUT(!details.busy(), 15'000);
        QVERIFY2(details.historyProblem().isEmpty(), qPrintable(details.historyProblem()));
        QVERIFY2(details.graphProblem().isEmpty(), qPrintable(details.graphProblem()));
        QVERIFY2(details.activityProblem().isEmpty(), qPrintable(details.activityProblem()));
        QVERIFY(details.summary()[u"known"_s].toBool());
        QVERIFY(!details.graphs().isEmpty());
        const auto lines = details.graphs().first().toMap()[u"lines"_s].toList();
        QVERIFY(!lines.isEmpty());
        QCOMPARE(lines.first().toMap()[u"latest"_s].toString(), u"42.5"_s);
        bool controlSeen = false;
        for (const auto& entry : details.activity())
            controlSeen = controlSeen || entry.toMap()[u"what"_s].toString() == u"Keyboard and mouse taken"_s;
        QVERIFY(controlSeen);
        const auto browser = phase(u"details"_s);
        QVERIFY(browser[u"ok"_s].toBool());
        QVERIFY(browser[u"known"_s].toBool());
        QCOMPARE(browser[u"temperature"_s].toDouble(), 42.5);
        details.setOpened(false);
        QVERIFY(details.summary().isEmpty());
        QVERIFY(details.activity().isEmpty());
    }

    // b1: keys, Type text and the saved actions in the menu of a machine that has no picture of its own (a server of
    // AMI's or OpenBMC's, a PC with Intel AMT, a Proxmox guest) and of a VNC server, by what the server says it can;
    // and what the menu's rows send arrives at the machine.
    void aMachinesMenuOffersKeysTextAndSavedActionsByWhatTheServerSaysItCan()
    {
        std::optional<base::Result<QList<core::SavedAction>>> listed;
        m_client.askSavedActions([&](auto result) { listed = std::move(result); });
        QTRY_VERIFY_WITH_TIMEOUT(listed.has_value(), 15'000);
        QVERIFY(listed && listed->ok());
        const QList<core::SavedAction> saved = listed->value();
        const QString keysId = m_fixture[u"saved"_s].toObject()[u"keys"_s].toString();
        const QString textId = m_fixture[u"saved"_s].toObject()[u"text"_s].toString();
        QJsonObject shown;
        for (const char* name : {"ami", "openbmc", "amt", "proxmox", "vnc"}) {
            const QString key = id(name);
            const bool keysExpected = QByteArray(name) != "openbmc" || expects("openbmc_keys");
            // (Its picture asked for meanwhile, as of a tile on a screen: a server offers keys over a console it
            // reads.)
            const auto status = statusWhen(
                key,
                [&](const core::Status& s) {
                    return s.state == u"online"_s && s.capabilities &&
                           s.capabilities->contains(u"keys"_s) == keysExpected;
                },
                60'000, [&] { static_cast<void>(snapshotNow(key)); });
            QVERIFY2(status, name);
            const auto machine = machineOf(key);
            QVERIFY2(machine, name);
            const bool keys = core::supports(*machine, &*status, u"keys"_s);
            const bool text = core::supports(*machine, &*status, u"text"_s);
            QCOMPARE(keys, keysExpected);
            const ui::Menu menu = menuOf(*machine, *status, saved);
            shown[QLatin1StringView(name)] =
                QJsonObject{{u"can"_s, QJsonArray::fromStringList(*status->capabilities)}, {u"rows"_s, rowsOf(menu)}};
            QVERIFY2((menu.row(u"sendKeys"_s) != nullptr) == keys, name);
            QVERIFY2((menu.row(u"keys:ControlLeft,AltLeft,Delete"_s) != nullptr) == keys, name);
            QVERIFY2((menu.row(u"typeText"_s) != nullptr) == text, name);
            QVERIFY2((menu.row(u"saved:"_s + keysId) != nullptr) == keys, name);
            QVERIFY2((menu.row(u"saved:"_s + textId) != nullptr) == text, name);
            if (!keys) {
                continue;
            }
            // What the rows send arrives: the saved keys, and a text typed from its box, at the machine's own keyboard.
            static_cast<void>(look(QString::fromLatin1(name)));
            const ui::MenuRow* run = menu.row(u"saved:"_s + keysId);
            QVERIFY(run && run->ask);
            const auto pressed = actNow(key, run->ask->action);
            QVERIFY2(pressed.ok(), qPrintable(QString::fromLatin1(name) + u": "_s +
                                              (pressed ? QString() : QString::fromStdString(pressed.error().message))));
            const auto typing = ui::askToType(machine->name, u"joint"_s, false);
            QVERIFY(typing);
            const auto typed = actNow(key, typing->action);
            QVERIFY2(typed.ok(), qPrintable(QString::fromLatin1(name) + u": "_s +
                                            (typed ? QString() : QString::fromStdString(typed.error().message))));
            // (An AMI console's reports say the modifiers and the keys: Ctrl and Alt, 0x05, with Delete, 0x4C. RFB says
            // each key by its X11 keysym.)
            const bool ami = QByteArray(name) == "ami";
            const QList<int> chord = ami ? QList<int>{0x054C} : QList<int>{0xFFE3, 0xFFE9, 0xFFFF};
            const QList<int> word =
                ami ? QList<int>{0x0D, 0x12, 0x0C, 0x11, 0x17} : QList<int>{0x6A, 0x6F, 0x69, 0x6E, 0x74};
            QList<int> arrived;
            const auto has = [&](const QList<int>& wanted) {
                qsizetype at = 0;
                for (const int one : arrived) {
                    if (at < wanted.size() && one == wanted.at(at)) {
                        ++at;
                    }
                }
                return at == wanted.size();
            };
            const auto bothArrived = [&] {
                arrived.append(downs(look(QString::fromLatin1(name))));
                return has(chord) && has(word);
            };
            QTRY_VERIFY_WITH_TIMEOUT(bothArrived(), 20'000);
            QJsonArray seen;
            for (const int one : arrived) {
                seen.append(one);
            }
            QJsonObject entry = shown[QLatin1StringView(name)].toObject();
            entry[u"arrived"_s] = seen;
            shown[QLatin1StringView(name)] = entry;
        }
        m_desktop[u"menus"_s] = shown;
    }

    // a1, b9: the live line's passes, as many as an account may have (32), and the 33rd refused `429 busy` with its
    // `Retry-After`: none is asked for before that time (refused here in the server's words), and one is given after
    // it.
    void aLiveLineTooManyIsRefusedWithItsWaitAndNoneIsAskedForBeforeIt()
    {
        auto lines = signedIn(u"native_lines"_s);
        QVERIFY(lines);
        const auto ask = [&] {
            std::optional<base::Result<QByteArray>> pass;
            lines->askLive(id("absolute"), [&](auto result) { pass = std::move(result); });
            return QTest::qWaitFor([&] { return pass.has_value(); }, 15'000) && pass
                       ? *pass
                       : base::Result<QByteArray>(base::Error{"test:no_answer", ""});
        };
        int given = 0;
        std::optional<base::Error> refused;
        for (int asked = 0; asked < 40 && !refused; ++asked) {
            const auto pass = ask();
            if (pass) {
                ++given;
            } else {
                refused = pass.error();
            }
        }
        if (!refused) {
            QFAIL("No pass was refused.");
        }
        heard(*refused);
        QCOMPARE(given, 32);
        QCOMPARE(refused->code, std::string("busy"));
        // The server's own count of what it was asked: the 33rd answered 429, with how long to wait.
        QJsonArray lives;
        const auto refusedThere = [&] {
            lives = look(u"lives"_s)[u"lives"_s].toArray();
            return !lives.isEmpty() && lives.last()[0].toInt() == 429;
        };
        QTRY_VERIFY_WITH_TIMEOUT(refusedThere(), 10'000);
        QCOMPARE(lives.last()[1].toString(), u"30"_s);
        const qsizetype before = lives.size();
        QElapsedTimer clock;
        clock.start();
        const auto again = ask();
        QVERIFY(!again);
        QCOMPARE(again.error().code, refused->code);
        QCOMPARE(again.error().message, refused->message);
        QTest::qWait(1000);
        QCOMPARE(look(u"lives"_s)[u"lives"_s].toArray().size(), before);
        // After the server's wait (its 30 s, as the passes run out), a pass is asked for and given.
        QTest::qWait(static_cast<int>(std::max<qint64>(0, 31'000 - clock.elapsed())));
        const auto after = ask();
        QVERIFY2(after.ok(), after ? "" : after.error().message.c_str());
        QTRY_COMPARE_WITH_TIMEOUT(look(u"lives"_s)[u"lives"_s].toArray().size(), before + 1, 10'000);
        m_desktop[u"lines"_s] = QJsonObject{{u"given"_s, given},
                                            {u"refused"_s, said(*refused)},
                                            {u"refused_here"_s, said(again.error())},
                                            {u"given_after_wait"_s, true}};
    }

    // a1, b9: a press whose answer broke off on its way (`502 dropped`) may have been done, as `timeout` may: a run on
    // several machines counts it so and never sends it again; what was refused (the board's no) is sent again.
    void aPressThatBrokeOffMayHaveBeenDoneAndARunOnSeveralNeverSendsItAgain()
    {
        QList<core::Machine> targets;
        QList<core::Status> statuses;
        for (const char* name : {"presser", "refuser", "dropper"}) {
            const auto status = statusWhen(id(name), [](const core::Status& s) {
                return s.state == u"online"_s && s.capabilities && s.capabilities->contains(u"reset"_s);
            });
            QVERIFY2(status, name);
            const auto machine = machineOf(id(name));
            QVERIFY2(machine, name);
            targets.append(*machine);
            statuses.append(*status);
            static_cast<void>(look(QString::fromLatin1(name)));
        }
        const ui::Menu menu = menuOf(targets.first(), statuses.first());
        const ui::MenuRow* reset = menu.row(u"power:atx_reset"_s);
        QVERIFY(reset && reset->ask);
        QHash<QString, int> sent;
        QHash<QString, base::Error> failed;
        ui::BulkRun::Ways ways;
        ways.underWay = [](const QString&) { return QString(); };
        ways.began = [](const QString&, const QString&) {};
        ways.ended = [](const QString&) {};
        ways.send = [&](const core::Machine& machine, const core::Action& action,
                        std::function<void(base::Result<core::Done>)> done) {
            ++sent[machine.id];
            m_client.act(machine.id, action, [&, key = machine.id, done = std::move(done)](auto result) {
                if (!result.ok()) {
                    failed.insert(key, result.error());
                    heard(result.error());
                }
                done(std::move(result));
            });
        };
        ways.mayHaveBeenDone = [&](const core::Machine& machine, const base::Error& error) {
            return m_client.mayHaveBeenDone(machine.id, error);
        };
        ways.done = [](const core::Machine&, const ui::Ask&) {};
        ui::BulkRun run(ways);
        QSignalSpy finished(&run, &ui::BulkRun::finished);
        run.start(targets, *reset->ask, std::nullopt, u"joint-bulk"_s);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 120'000);
        const auto states = [&] {
            QHash<QString, QString> by;
            for (const QVariant& row : run.shown()[u"rows"_s].toList()) {
                by.insert(row.toMap()[u"name"_s].toString(), row.toMap()[u"state"_s].toString());
            }
            return by;
        };
        const auto first = states();
        QCOMPARE(first.value(u"Fixture presser"_s), u"ok"_s);
        QCOMPARE(first.value(u"Fixture refuser"_s), u"bad"_s);
        QCOMPARE(first.value(u"Fixture dropper"_s), u"unknown"_s);
        QCOMPARE(failed.value(id("dropper")).code, std::string("dropped"));
        QVERIFY(m_client.mayHaveBeenDone(id("dropper"), failed.value(id("dropper"))));
        QCOMPARE(failed.value(id("refuser")).code, std::string("action_failed"));
        QVERIFY(!m_client.mayHaveBeenDone(id("refuser"), failed.value(id("refuser"))));
        QVERIFY(!run.shown()[u"again"_s].toString().isEmpty());
        run.retry();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 120'000);
        const auto second = states();
        QCOMPARE(second.value(u"Fixture dropper"_s), u"unknown"_s);
        QCOMPARE(sent.value(id("presser")), 1);
        QCOMPARE(sent.value(id("refuser")), 2);
        QCOMPARE(sent.value(id("dropper")), 1);
        // And at the KVMs: the press that broke off went out once, the refused one twice.
        const auto pressed = [&](const char* name) { return look(QString::fromLatin1(name))[u"actions"_s].toArray(); };
        QCOMPARE(pressed("dropper"), QJsonArray{u"atx reset"_s});
        QCOMPARE(pressed("refuser"), (QJsonArray{u"atx reset"_s, u"atx reset"_s}));
        QCOMPARE(pressed("presser"), QJsonArray{u"atx reset"_s});
        QJsonObject rows;
        for (auto row = first.begin(); row != first.end(); ++row) {
            rows[row.key()] = row.value();
        }
        m_desktop[u"bulk"_s] = QJsonObject{{u"first"_s, rows},
                                           {u"dropped"_s, said(failed.value(id("dropper")))},
                                           {u"refused"_s, said(failed.value(id("refuser")))}};
    }

    // 3: a VNC server and an OpenBMC console live through the server, and keyboard and mouse taken there (0.3.0); a
    // server that does not say so for one (0.2.2) is not asked.
    void aVncServerAndAnOpenBmcConsoleAreLiveWithControlThroughTheServer()
    {
        QJsonObject shown;
        for (const char* name : {"vnc", "openbmc"}) {
            const QString key = id(name);
            const bool vnc = QByteArray(name) == "vnc";
            const bool liveExpected = !vnc || expects("vnc_live");
            const bool controlExpected = vnc ? expects("vnc_live") : expects("openbmc_keys");
            const auto status = statusWhen(key, [&](const core::Status& s) {
                return s.state == u"online"_s && s.capabilities &&
                       s.capabilities->contains(u"live"_s) == liveExpected && (vnc || s.hasScreen);
            });
            QVERIFY2(status, name);
            const auto machine = machineOf(key);
            QVERIFY(machine);
            const bool live = core::supports(*machine, &*status, u"live"_s);
            const bool control = core::supports(*machine, &*status, u"control"_s);
            QCOMPARE(live, liveExpected);
            QCOMPARE(control, controlExpected);
            QJsonObject entry{{u"live"_s, live}, {u"control"_s, control}};
            if (!live) {
                // Not opened by the program (MergedWall asks `live` first); the server would refuse it.
                std::optional<base::Result<QByteArray>> pass;
                m_client.askLive(key, [&](auto result) { pass = std::move(result); });
                QTRY_VERIFY_WITH_TIMEOUT(pass.has_value(), 15'000);
                QVERIFY(pass && !pass->ok());
                heard(pass->error());
                entry[u"server_refused_a_pass"_s] = said(pass->error());
                shown[QLatin1StringView(name)] = entry;
                continue;
            }
            auto line = open(key);
            QVERIFY2(line, name);
            bool decoded = false;
            connect(line.get(), &core::Live::pictureCame, this, [&](const core::Live::Picture& picture) {
                decoded = decoded || (!picture.h264 && !QImage::fromData(picture.data, "JPEG").isNull());
            });
            line->askForAKeyFrame();
            QTRY_VERIFY_WITH_TIMEOUT(decoded, 20'000);
            static_cast<void>(look(QString::fromLatin1(name)));
            ui::ControlView view;
            ui::KeysByHand keys;
            ui::MachineControl hands(view, keys);
            hands.setAllowed(control);
            hands.setInFront(true);
            hands.have(line.get(), key);
            hands.take();
            if (!control) {
                // Not offered: nothing is asked, and nothing reaches the machine.
                QTest::qWait(1000);
                QVERIFY(!hands.isOn() && !line->hasControl());
                QVERIFY(look(QString::fromLatin1(name))[u"keys"_s].toArray().isEmpty());
                shown[QLatin1StringView(name)] = entry;
                continue;
            }
            QTRY_VERIFY_WITH_TIMEOUT(hands.isOn(), 20'000);
            line->key(0xE1, true); // ShiftLeft, held through the giving back
            line->key(0x04, true);
            line->key(0x04, false);
            line->pointerAt(16384, 16384);
            line->button(1, true);
            line->button(1, false);
            QList<int> arrived;
            int pointers = 0;
            const auto allArrived = [&] {
                const QJsonObject seen = look(QString::fromLatin1(name));
                arrived.append(downs(seen));
                pointers += seen[u"pointers"_s].toInt();
                return arrived.contains(0xFFE1) && arrived.contains(0x61) && pointers >= 2;
            };
            QTRY_VERIFY_WITH_TIMEOUT(allArrived(), 20'000);
            hands.giveBack();
            QTRY_VERIFY(!view.isBusy());
            // What was down is let go of at the machine.
            const auto nothingDown = [&] {
                const QJsonObject seen = look(QString::fromLatin1(name));
                return seen[u"down"_s].toArray().isEmpty() && seen[u"buttons"_s].toInt() == 0;
            };
            QTRY_VERIFY_WITH_TIMEOUT(nothingDown(), 20'000);
            entry[u"keys_arrived"_s] = true;
            shown[QLatin1StringView(name)] = entry;
        }
        m_desktop[u"vnc_openbmc"_s] = shown;
    }

    // b5: a PC with Intel AMT, in the view its tile is in (`amt_view`): changed by an operator in person (a viewer is
    // refused), its still and how old it is, and in health only the sentence why there is no screen. A server that has
    // no views (0.2.2) is offered none.
    void aPcWithIntelAmtIsShownInItsViewWithItsStillsAge()
    {
        const QString key = id("amt");
        const auto status =
            statusWhen(key, [](const core::Status& s) { return s.state == u"online"_s && s.hasScreen; });
        const auto machine = machineOf(key);
        if (!status || !machine) {
            QFAIL("The PC never showed its screen.");
        }
        QJsonObject shown{{u"listed_view"_s, machine->amtView},
                          {u"status_view"_s, status->server ? status->server->amtView : QString()}};
        const ui::Menu menu = menuOf(*machine, *status);
        shown[u"rows"_s] = rowsOf(menu);
        if (!expects("amt_view")) {
            // The server says no view of the PC (0.2.2): nothing of the views is offered, said or sent (the fixture
            // finds no PUT .../view among what the server was asked). The still and its age are as they are.
            const ui::WallTile card = ui::cardFor(*machine, &*status, 0, false);
            shown[u"card_meta"_s] = card.meta;
            m_desktop[u"amt_views"_s] = shown;
            QVERIFY(machine->amtView.isEmpty());
            QVERIFY(core::amtViewOf(*machine, &*status).isEmpty());
            for (const QString& row : {u"view:health"_s, u"view:stills"_s, u"view:live"_s}) {
                QVERIFY2(menu.row(row) == nullptr, qPrintable(row));
            }
            QVERIFY(card.amtView.isEmpty());
            for (const QString& word : {u"Health only"_s, u"Stills"_s, u"Live KVM"_s}) {
                QVERIFY2(!card.meta.contains(word), qPrintable(card.meta));
            }
            return;
        }
        QCOMPARE(machine->amtView, u"stills"_s);
        QCOMPARE(core::amtViewOf(*machine, &*status), u"stills"_s);
        QVERIFY(menu.row(u"view:health"_s) && menu.row(u"view:health"_s)->enabled);
        QVERIFY(menu.row(u"view:stills"_s) && !menu.row(u"view:stills"_s)->enabled);
        // A still, with how old it is, as a tile that is only shown gets it.
        std::optional<core::Snapshot> still;
        const auto stillCame = [&] {
            const auto snapshot = snapshotNow(key);
            if (snapshot) {
                still = snapshot.value();
            }
            return snapshot.ok();
        };
        QTRY_VERIFY_WITH_TIMEOUT(stillCame(), 60'000);
        if (!still) {
            QFAIL("No still came.");
        }
        QVERIFY(still->still);
        QVERIFY(still->ageSeconds.has_value());
        const QString note = ui::viewNoteOf(*machine, &*status, ui::StillCame{still->ageSeconds}, false);
        shown[u"still_age"_s] = *still->ageSeconds;
        shown[u"card_note"_s] = note;
        QVERIFY2(note.startsWith(u"Stills"_s) && (note.contains(u"ago"_s) || note.contains(u"just now"_s)),
                 qPrintable(note));
        // A viewer may not change it; an operator or an admin, in person, may.
        auto looker = signedIn(u"native_looker"_s);
        QVERIFY(looker);
        std::optional<base::Result<QString>> refused;
        looker->setView(key, u"health"_s, [&](auto result) { refused = std::move(result); });
        QTRY_VERIFY_WITH_TIMEOUT(refused.has_value(), 15'000);
        if (!refused || refused->ok()) {
            QFAIL("A viewer changed the view.");
        }
        heard(refused->error());
        QCOMPARE(refused->error().code, std::string("not_allowed"));
        std::optional<base::Result<QString>> changed;
        m_client.setView(key, u"health"_s, [&](auto result) { changed = std::move(result); });
        QTRY_VERIFY_WITH_TIMEOUT(changed.has_value(), 15'000);
        if (!changed || !changed->ok()) {
            QFAIL("The view was not changed.");
        }
        QCOMPARE(changed->value(), u"health"_s);
        const auto health = statusWhen(key, [](const core::Status& s) {
            return s.server && s.server->amtView == u"health"_s && !s.server->noScreen.isEmpty();
        });
        if (!health || !health->server) {
            QFAIL("The status never said health only.");
        }
        QVERIFY(!core::hasAScreenToShow(*machine, &*health));
        const ui::Menu healthMenu = menuOf(*machine, *health);
        QVERIFY(healthMenu.row(u"typeText"_s) == nullptr);
        QVERIFY(healthMenu.row(u"view:health"_s) && !healthMenu.row(u"view:health"_s)->enabled);
        const ui::WallTile card = ui::cardFor(*machine, &*health, 0, false);
        shown[u"no_screen"_s] = health->server->noScreen;
        shown[u"health_card"_s] = QJsonObject{{u"title"_s, card.title}, {u"text"_s, card.text}, {u"meta"_s, card.meta}};
        QCOMPARE(ui::viewNoteOf(*machine, &*health, std::nullopt, false), u"Health only"_s);
        changed.reset();
        m_client.setView(key, u"stills"_s, [&](auto result) { changed = std::move(result); });
        QTRY_VERIFY_WITH_TIMEOUT(changed.has_value(), 15'000);
        QVERIFY(changed && changed->ok());
        m_desktop[u"amt_views"_s] = shown;
    }

    // b6: virtual media through the server (0.3.0), on a kvmd KVM: what is in its drive on its card; the box's list,
    // a mount asked first naming the machine and the image (and refused where the machine is called otherwise by then),
    // unmounted, an image carried through as it is read with how far it has come, and deleted. A server without media
    // (0.2.2) has no Media row on any machine.
    void mediaThroughTheServerListMountUnmountUploadAndDelete()
    {
        const auto wall = wallNow();
        QVERIFY(wall);
        if (!expects("media")) {
            QJsonArray none;
            for (const auto& machine : wall->machines) {
                const auto status = statusWhen(machine.id, [](const core::Status&) { return true; }, 5'000);
                const ui::Menu menu = menuOf(machine, status.value_or(core::Status{}));
                QVERIFY2(menu.row(u"media"_s) == nullptr, qPrintable(machine.name));
                none.append(machine.name);
            }
            m_desktop[u"media"_s] = QJsonObject{{u"no_media_row_on"_s, none}};
            return;
        }
        const QString key = id("media");
        const auto status = statusWhen(key, [](const core::Status& s) {
            return s.state == u"online"_s && s.capabilities && s.capabilities->contains(u"media"_s);
        });
        QVERIFY(status);
        QVERIFY(!status->media);
        const auto machine = machineOf(key);
        QVERIFY(machine);
        QVERIFY(menuOf(*machine, *status).row(u"media"_s) != nullptr);
        ui::MenuOf notAdmin;
        notAdmin.machine = *machine;
        notAdmin.status = *status;
        QVERIFY(ui::menuFor(notAdmin).row(u"media"_s) == nullptr);
        static_cast<void>(look(u"media"_s));
        // (A tile changed on the server is a new driver there, which has no media until its first look finds them: the
        // server's `404 no_virtual_media` meanwhile. Asked until it lists them again.)
        const auto listsAgain = [&] {
            std::optional<base::Result<core::MediaList>> list;
            m_client.askMedia(key, [&](auto result) { list = std::move(result); });
            return QTest::qWaitFor([&] { return list.has_value(); }, 15'000) && list && list->ok();
        };
        QStringList told;
        QStringList saidAll;
        ui::MediaView::UnderWay underWay;
        ui::MediaView view;
        connect(&view, &ui::MediaView::told, &view,
                [&](const QString& said, bool good) { told.append((good ? u"ok: "_s : u"no: "_s) + said); });
        connect(&view, &ui::MediaView::changed, &view, [&] {
            if (saidAll.isEmpty() || saidAll.last() != view.said()) {
                saidAll.append(view.said());
            }
        });
        view.open(&m_client, key, machine->name, key, &underWay, key);
        const auto names = [&] {
            QStringList all;
            for (const QVariant& image : view.images()) {
                all.append(image.toMap()[u"name"_s].toString());
            }
            return all;
        };
        QTRY_VERIFY_WITH_TIMEOUT(names().contains(u"debian-13.iso"_s), 20'000);
        view.pick(u"debian-13.iso"_s);
        view.chooseDrive(u"cd"_s);
        QVERIFY(view.canMount());
        view.mount();
        QVariantMap question = view.question();
        QVERIFY(question[u"text"_s].toString().contains(u"Fixture media"_s));
        QVERIFY(question[u"text"_s].toString().contains(u"debian-13.iso"_s));
        // Called otherwise on the server while the question stands: the server does nothing, and says so.
        QVERIFY(meanwhile(u"rename-media"_s));
        QTRY_VERIFY_WITH_TIMEOUT(listsAgain(), 60'000);
        view.answer(true);
        QTRY_VERIFY_WITH_TIMEOUT(!view.busy() && view.saidBad(), 60'000);
        const QString changedSaid = view.said();
        QVERIFY2(changedSaid.contains(u"Fixture media renamed"_s), qPrintable(changedSaid));
        QVERIFY(look(u"media"_s)[u"drive"_s].toObject()[u"connected"_s].toBool() == false);
        QVERIFY(meanwhile(u"name-media-back"_s));
        QTRY_VERIFY_WITH_TIMEOUT(listsAgain(), 60'000);
        view.rename(machine->name);
        view.pick(u"debian-13.iso"_s);
        view.chooseDrive(u"cd"_s);
        view.mount();
        QVERIFY(view.question()[u"text"_s].toString().contains(u"Fixture media"_s));
        view.answer(true);
        QTRY_VERIFY_WITH_TIMEOUT(!view.busy(), 60'000);
        QVERIFY2(!view.saidBad(), qPrintable(view.said()));
        QJsonObject drive;
        const auto connected = [&] {
            drive = look(u"media"_s)[u"drive"_s].toObject();
            return drive[u"connected"_s].toBool();
        };
        QTRY_VERIFY_WITH_TIMEOUT(connected(), 20'000);
        QCOMPARE(drive[u"image"_s].toString(), u"debian-13.iso"_s);
        QVERIFY(drive[u"cdrom"_s].toBool());
        // On the tile: what is in its drive, for everybody who sees it.
        const auto mounted = statusWhen(key, [](const core::Status& s) { return s.media.has_value(); });
        QVERIFY(mounted);
        const ui::WallTile card = ui::cardFor(*machine, &*mounted, 0, true);
        QVERIFY2(card.meta.contains(u"CD: debian-13.iso"_s), qPrintable(card.meta));
        // Taken out again.
        QTRY_VERIFY_WITH_TIMEOUT(view.canUnmount(), 20'000);
        view.unmount();
        QVERIFY(view.question()[u"text"_s].toString().contains(u"debian-13.iso"_s));
        view.answer(true);
        QTRY_VERIFY_WITH_TIMEOUT(!view.busy(), 60'000);
        QVERIFY2(!view.saidBad(), qPrintable(view.said()));
        QTRY_VERIFY_WITH_TIMEOUT(!look(u"media"_s)[u"drive"_s].toObject()[u"connected"_s].toBool(), 20'000);
        // An image carried through as it is read: 48 MB, which the stand-in takes at 6 MB a second, its progress asked.
        QTemporaryDir folder(QFileInfo(id("phases")).dir().filePath(u"upload-XXXXXX"_s));
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(u"joint-upload.img"_s);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QByteArray piece(qsizetype{1024} * 1024, 0);
            for (qsizetype megabyte = 0; megabyte < 48; ++megabyte) {
                for (qsizetype at = 0; at < piece.size(); ++at) {
                    piece[at] = static_cast<char>((at * 13 + megabyte * 7 + 5) & 0xFF);
                }
                hash.addData(piece);
                QCOMPARE(file.write(piece), piece.size());
            }
        }
        view.setImageDoor([path] { return base::Result<QString>(path); });
        QTRY_VERIFY_WITH_TIMEOUT(view.canUpload(), 20'000);
        view.upload();
        QTRY_VERIFY_WITH_TIMEOUT(!view.question().isEmpty(), 10'000);
        QVERIFY(view.question()[u"text"_s].toString().contains(u"joint-upload.img"_s));
        QVERIFY(view.question()[u"text"_s].toString().contains(u"Fixture media"_s));
        view.answer(true);
        QTRY_VERIFY_WITH_TIMEOUT(view.working(), 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(!view.working() && !view.busy(), 120'000);
        QVERIFY2(!view.saidBad(), qPrintable(view.said()));
        bool partway = false;
        for (const QString& one : saidAll) {
            partway = partway || (one.startsWith(u"Uploading joint-upload.img: "_s) && !one.contains(u"(100 %)"_s));
        }
        QVERIFY2(partway, qPrintable(saidAll.join(u" | "_s)));
        QJsonObject seen;
        const auto carriedThere = [&] {
            seen = look(u"media"_s);
            return seen[u"images"_s].toArray().contains(u"joint-upload.img"_s);
        };
        QTRY_VERIFY_WITH_TIMEOUT(carriedThere(), 20'000);
        const QJsonObject write = seen[u"writes"_s].toArray().last().toObject();
        QCOMPARE(write[u"name"_s].toString(), u"joint-upload.img"_s);
        QCOMPARE(write[u"received"_s].toInteger(), qint64{48} * 1024 * 1024);
        QVERIFY(write[u"complete"_s].toBool());
        QCOMPARE(seen[u"sha256"_s].toObject()[u"joint-upload.img"_s].toString(),
                 QString::fromLatin1(hash.result().toHex()));
        // And deleted.
        QTRY_VERIFY_WITH_TIMEOUT(names().contains(u"joint-upload.img"_s), 20'000);
        view.pick(u"joint-upload.img"_s);
        QVERIFY(view.canDelete());
        view.remove();
        QVERIFY(view.question()[u"text"_s].toString().contains(u"joint-upload.img"_s));
        view.answer(true);
        QTRY_VERIFY_WITH_TIMEOUT(!view.busy(), 60'000);
        QVERIFY2(!view.saidBad(), qPrintable(view.said()));
        QTRY_VERIFY_WITH_TIMEOUT(!look(u"media"_s)[u"images"_s].toArray().contains(u"joint-upload.img"_s), 20'000);
        view.close();
        m_desktop[u"media"_s] = QJsonObject{{u"changed_meanwhile"_s, changedSaid},
                                            {u"card_meta"_s, card.meta},
                                            {u"told"_s, QJsonArray::fromStringList(told)},
                                            {u"said"_s, QJsonArray::fromStringList(saidAll)}};
    }

    // b4: Start into BIOS setup, asked first; the server's own words where it may be set (`504 timeout`), which is
    // something that may have been done; its answer waited for as long as the page waits (four minutes), past the
    // two and a half any other action is given; and `"watching": true` from the large view, for which a PC with Intel
    // AMT opens its screen before it starts.
    void startIntoBiosSetupIsWaitedForAndMayBeSet()
    {
        QJsonObject shown;
        QHash<QString, core::Action> asked;
        for (const char* name : {"setup", "slowsetup", "amtsetup"}) {
            const auto status = statusWhen(id(name), [](const core::Status& s) {
                return s.capabilities && s.capabilities->contains(u"bios_setup"_s);
            });
            const auto machine = machineOf(id(name));
            if (!status || !machine) {
                QFAIL(name);
            }
            const ui::Menu menu = menuOf(*machine, *status);
            const ui::MenuRow* row = menu.row(u"power:bios_setup"_s);
            if (row == nullptr || !row->ask) {
                QFAIL(name);
            }
            QVERIFY(row->ask->confirm);
            QVERIFY(row->ask->question.contains(machine->name));
            asked.insert(QString::fromLatin1(name), row->ask->action);
            static_cast<void>(look(QString::fromLatin1(name)));
        }
        const auto setupOf = [&](const char* name) {
            core::Action action = asked.value(QString::fromLatin1(name));
            action.watching = true; // (sent from the machine's large view, which the program opens first)
            return action;
        };
        // The slow controller first: its answer comes after the others are done.
        std::optional<base::Result<core::Done>> slow;
        QElapsedTimer slowClock;
        slowClock.start();
        m_client.act(id("slowsetup"), setupOf("slowsetup"), [&](auto result) { slow = std::move(result); });
        // A controller that does it: the setup boot set, and the server restarted into it (the one-time override then
        // gone with that boot).
        const auto done = actNow(id("setup"), setupOf("setup"));
        QVERIFY2(done.ok(), done ? "" : done.error().message.c_str());
        const QJsonObject controller = look(u"setup"_s);
        shown[u"setup_controller"_s] = controller;
        QVERIFY(controller[u"actions"_s].toArray().contains(u"system ForceRestart"_s));
        // A PC with Intel AMT that is off, from its large view: its screen is opened before it is switched on.
        const auto amt = actNow(id("amtsetup"), setupOf("amtsetup"));
        QVERIFY2(amt.ok(), amt ? "" : amt.error().message.c_str());
        const QJsonObject pc = look(u"amtsetup"_s);
        shown[u"amt"_s] = pc;
        QVERIFY(pc[u"opened_before_on"_s].toBool());
        QCOMPARE(pc[u"actions"_s].toArray(), QJsonArray{2});
        // The slow one: answered by the server past two and a half minutes, in its own words.
        QTRY_VERIFY_WITH_TIMEOUT(slow.has_value(), 250'000);
        const qint64 waited = slowClock.elapsed();
        shown[u"slow_seconds"_s] = static_cast<double>(waited) / 1000.0;
        if (!slow || slow->ok()) {
            QFAIL("The slow controller's start into BIOS setup was not answered as may be set.");
        }
        heard(slow->error());
        shown[u"slow"_s] = said(slow->error());
        QVERIFY2(waited > 150'000, qPrintable(QString::number(waited)));
        QVERIFY2(slow->error().code.find(':') == std::string::npos, slow->error().message.c_str());
        QCOMPARE(slow->error().code, std::string("timeout"));
        QVERIFY(m_client.mayHaveBeenDone(id("slowsetup"), slow->error()));
        m_desktop[u"bios_setup"_s] = shown;
    }

    // b7: a screen watch's alert in Details, in the page's words (which watch, after how many minutes, and when it is
    // over), and the picture that set it off, for an admin. (0.2.2 has no screen watch.)
    void aScreenAlertIsSaidWithItsWatchMinutesAndPictureAndWhenItIsOver()
    {
        if (!expects("screen_watch")) {
            m_desktop[u"screen_alerts"_s] = QJsonObject{{u"server_has_none"_s, true}};
            return;
        }
        const QString key = id("watch");
        ui::DetailsView details;
        details.bind(&m_client, key, u"joint-admin"_s, true, true);
        details.setOpened(true);
        const auto rowOf = [&](const QString& what) {
            const QVariantList rows = details.activity();
            for (qsizetype at = 0; at < rows.size(); ++at) {
                if (rows.at(at).toMap()[u"what"_s].toString() == what) {
                    return static_cast<int>(at);
                }
            }
            return -1;
        };
        // The tile on a screen (its pictures asked for), until its unchanged screen is told.
        const auto shownUntil = [&](const QString& what) {
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 60'000) {
                for (int picture = 0; picture < 6; ++picture) {
                    static_cast<void>(snapshotNow(key, 320));
                    QTest::qWait(300);
                }
                details.refresh();
                if (QTest::qWaitFor([&] { return !details.busy(); }, 15'000) && rowOf(what) >= 0) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY2(shownUntil(u"Alert: Screen unchanged for 1 minute"_s), "the unchanged screen was never said");
        QVERIFY(meanwhile(u"change-watch"_s));
        QVERIFY2(shownUntil(u"Alert: Back to normal: the screen changes again"_s),
                 "the screen's change was never said");
        const int row = rowOf(u"Alert: Screen unchanged for 1 minute"_s);
        QVERIFY(details.activity().at(row).toMap()[u"picture"_s].toBool());
        QVERIFY(!details.activity()
                     .at(rowOf(u"Alert: Back to normal: the screen changes again"_s))
                     .toMap()[u"picture"_s]
                     .toBool());
        details.showPicture(details.activity().at(row).toMap()[u"row"_s].toInt());
        QTRY_COMPARE_WITH_TIMEOUT(details.picture()[u"state"_s].toString(), u"shown"_s, 15'000);
        QCOMPARE(details.pictureImage().size(), QSize(320, 180));
        QJsonArray rows;
        for (const QVariant& one : details.activity()) {
            rows.append(one.toMap()[u"what"_s].toString());
        }
        m_desktop[u"screen_alerts"_s] =
            QJsonObject{{u"activity"_s, rows}, {u"picture_title"_s, details.picture()[u"title"_s].toString()}};
        details.closePicture();
        details.setOpened(false);
    }

    // b3: a Fingerbot paired with a KVM: its two rows in the menu and its low battery said, as the status says them;
    // what is pressed arrives at the Comet as its own page sends it.
    void aFingerbotsRowsAndItsLowBatteryAndItsPresses()
    {
        const QString key = id("fingerbot");
        const auto status = statusWhen(
            key,
            [](const core::Status& s) {
                return s.capabilities && s.capabilities->contains(u"fingerbot"_s) && s.fingerbot &&
                       s.fingerbot->battery.has_value();
            },
            60'000, [&] { static_cast<void>(snapshotNow(key, 480)); });
        const auto machine = machineOf(key);
        if (!status || !status->fingerbot || !status->fingerbot->battery || !machine) {
            QFAIL("The Fingerbot's battery was never said.");
        }
        QCOMPARE(*status->fingerbot->battery, 12);
        QVERIFY(status->fingerbot->low);
        const ui::Menu menu = menuOf(*machine, *status);
        QVERIFY(menu.row(u"power:fingerbot_power"_s) && menu.row(u"power:fingerbot_power_long"_s));
        QVERIFY2(menu.battery.contains(u"(12 %)"_s), qPrintable(menu.battery));
        const ui::WallTile card = ui::cardFor(*machine, &*status, 0, true);
        QVERIFY2(card.meta.contains(u"Fingerbot battery low (12%)"_s), qPrintable(card.meta));
        // A KVM without one has neither row.
        const auto plain = statusWhen(id("absolute"), [](const core::Status& s) { return s.capabilities.has_value(); });
        QVERIFY(plain);
        const auto plainMachine = machineOf(id("absolute"));
        QVERIFY(plainMachine);
        QVERIFY(menuOf(*plainMachine, *plain).row(u"power:fingerbot_power"_s) == nullptr);
        static_cast<void>(look(u"fingerbot"_s));
        for (const char* name : {"power:fingerbot_power", "power:fingerbot_power_long"}) {
            const ui::MenuRow* row = menu.row(QString::fromLatin1(name));
            if (row == nullptr || !row->ask) {
                QFAIL(name);
            }
            const auto pressed = actNow(key, row->ask->action);
            QVERIFY2(pressed.ok(), pressed ? "" : pressed.error().message.c_str());
            // (The server holds the machine a moment after a press, for the arm to come back: one thing at a time,
            // 1.5 s. Not five: the server closes a connection that was idle five seconds, and a press sent on it just
            // then never reaches it, "could not be reached" here: seen once, said in the report of b11.)
            QTest::qWait(2500);
        }
        QJsonArray presses;
        const auto bothPressed = [&] {
            for (const auto& one : look(u"fingerbot"_s)[u"actions"_s].toArray()) {
                presses.append(one);
            }
            return presses.size() >= 2;
        };
        QTRY_VERIFY_WITH_TIMEOUT(bothPressed(), 15'000);
        QCOMPARE(presses, (QJsonArray{u"fingerbot 500 2"_s, u"fingerbot 8000 2"_s}));
        m_desktop[u"fingerbot"_s] =
            QJsonObject{{u"battery"_s, menu.battery}, {u"card_meta"_s, card.meta}, {u"presses"_s, presses}};
    }

    // b8: Words.qml against the words the server itself serves (`/static/words.json`): every state, code, refusal,
    // kind and word of `can` it can say is in Words.qml, under the server's label; 0.3.0's file is the one copied
    // beside the program (ui/words.json); and every code a case here was refused with is one of them.
    void everyWordTheServerCanSayIsInWordsQml()
    {
        QNetworkAccessManager network;
        std::unique_ptr<QNetworkReply> reply(network.get(QNetworkRequest(QUrl(id("url") + u"/static/words.json"_s))));
        QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 15'000);
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        const QByteArray served = reply->readAll();
        QFile copy(QStringLiteral(QKVM_WORDS_JSON));
        QVERIFY(copy.open(QIODevice::ReadOnly));
        const QByteArray copied = copy.readAll();
        const auto sha = [](const QByteArray& bytes) {
            return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        };
        QJsonObject shown{{u"served_sha256"_s, sha(served)}, {u"copy_sha256"_s, sha(copied)}};
        if (expects("words_copied")) {
            QCOMPARE(sha(served), sha(copied));
        }
        const QJsonObject server = QJsonDocument::fromJson(served).object();
        QVERIFY(!server.isEmpty());
        QQmlEngine engine;
        auto* words = engine.singletonInstance<QObject*>("Qkvm", "Words");
        QVERIFY(words != nullptr);
        const auto asWritten = [](QString label) { return label.replace("..."_L1, QString(QChar(0x2026))); };
        QJsonArray lacking;
        QJsonArray otherwise;
        for (const auto& [section, property] :
             {std::pair{"states", "states"}, std::pair{"server_states", "serverStates"}, std::pair{"kinds", "kinds"},
              std::pair{"can", "can"}, std::pair{"codes", "codes"}, std::pair{"refusals", "refusals"}}) {
            const QJsonObject theirs = server[QLatin1StringView(section)].toObject();
            QVERIFY2(!theirs.isEmpty(), section);
            const QVariantMap ours = words->property(property).toMap();
            for (auto word = theirs.begin(); word != theirs.end(); ++word) {
                const QString where = QLatin1StringView(section) + u'.' + word.key();
                if (!ours.contains(word.key())) {
                    lacking.append(where);
                    continue;
                }
                if (QByteArray(section) == "refusals") {
                    QList<int> said;
                    for (const QVariant& one : ours.value(word.key()).toList()) {
                        said.append(one.toInt());
                    }
                    QList<int> theirStatuses;
                    for (const auto& one : word.value().toArray()) {
                        theirStatuses.append(one.toInt());
                    }
                    std::ranges::sort(said);
                    std::ranges::sort(theirStatuses);
                    if (said != theirStatuses) {
                        otherwise.append(where);
                    }
                } else if (ours.value(word.key()).toString() != asWritten(word.value().toString())) {
                    otherwise.append(where + u": "_s + word.value().toString());
                }
            }
        }
        // What the cases here were refused with: each a code of the server's file (or the live line's own, which the
        // file does not hold: the README's "Live view and control").
        QJsonArray unknown;
        const QVariantMap codes = words->property("codes").toMap();
        const QVariantMap refusals = words->property("refusals").toMap();
        for (const QString& code : std::as_const(m_codes)) {
            if (!codes.contains(code) && !refusals.contains(code)) {
                unknown.append(code);
            }
        }
        shown[u"lacking"_s] = lacking;
        shown[u"labelled_otherwise"_s] = otherwise;
        shown[u"codes_met"_s] = QJsonArray::fromStringList(QStringList(m_codes.begin(), m_codes.end()));
        shown[u"codes_met_unknown"_s] = unknown;
        m_desktop[u"words"_s] = shown;
        QVERIFY2(lacking.isEmpty(), qPrintable(QJsonDocument(lacking).toJson(QJsonDocument::Compact)));
        QVERIFY2(unknown.isEmpty(), qPrintable(QJsonDocument(unknown).toJson(QJsonDocument::Compact)));
        if (expects("words_copied")) {
            QVERIFY2(otherwise.isEmpty(), qPrintable(QJsonDocument(otherwise).toJson(QJsonDocument::Compact)));
        }
    }

    void signingOutRevokesTheNativeLiveConnection()
    {
        auto live = open(id("absolute"));
        QVERIFY(live);
        QSignalSpy closed(live.get(), &core::Live::closed);
        bool done = false;
        m_client.signOut([&](const auto&) { done = true; });
        QTRY_VERIFY(done);
        QTRY_VERIFY_WITH_TIMEOUT(!closed.isEmpty(), 20'000);
        QVERIFY(!live->hasControl());
    }
};

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QPA_FONTDIR", qgetenv("SystemRoot") + "/Fonts");
    QGuiApplication app(argc, argv);
    ui::prepareApplication();
    TestWebCompat test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_web_compat.moc"
