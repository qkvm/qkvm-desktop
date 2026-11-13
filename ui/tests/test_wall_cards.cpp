#include <QTest>

#include "wall_cards.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using ui::WallTile;

// A machine's card from what the server says of it, in the page's words.
class TestWallCards : public QObject {
    Q_OBJECT

private:
    static core::Machine kvm()
    {
        core::Machine machine;
        machine.id = u"a"_s;
        machine.name = u"Build server"_s;
        machine.host = u"192.168.1.21"_s;
        machine.kind = u"kvmd"_s;
        return machine;
    }

    static core::Status live()
    {
        core::Status status;
        status.state = u"online"_s;
        status.signal = true;
        status.width = 1920;
        status.height = 1080;
        return status;
    }

private slots:
    // M6 2g (design 4): a JetKVM of this PC's own wall sends its card the newest key frame of its H.264, which
    // the card's stream decodes as a still; a QKVM server's JetKVM, a kvmd and a server send JPEGs and PNGs.
    void aJetKvmOfThisPcsOwnWallIsDrawnFromItsKeyFrames()
    {
        core::Machine jet = kvm();
        jet.kind = u"jetkvm"_s;
        jet.own = true;
        QCOMPARE(ui::cardCodecOf(jet), engine::Codec::KeyFrameStill);
        jet.own = false;
        QCOMPARE(ui::cardCodecOf(jet), engine::Codec::Still);
        core::Machine own = kvm();
        own.own = true;
        QCOMPARE(ui::cardCodecOf(own), engine::Codec::Still);
        QCOMPARE(ui::cardCodecOf(kvm()), engine::Codec::Still);
    }

    // M6 2h: a machine in use elsewhere (a JetKVM whose one viewer somebody else has) has "Show here" on its card,
    // of this PC's wall and of a server's alike, as the page has it; nothing else of a server's has a button.
    void aMachineInUseElsewhereOffersShowHere()
    {
        core::Status busy;
        busy.state = u"busy"_s;
        busy.errorCode = u"in_use"_s;
        core::Machine jet = kvm();
        jet.kind = u"jetkvm"_s;
        for (const bool own : {true, false}) {
            jet.own = own;
            const WallTile card = ui::cardFor(jet, &busy, 7, false);
            QCOMPARE(card.title, u"In use elsewhere"_s);
            QCOMPARE(card.action, u"Show here"_s);
            QCOMPARE(ui::cardWayOf(jet, &busy), ui::CardWay::ShowHere);
        }
        core::Status online = live();
        QCOMPARE(ui::cardWayOf(jet, &online), ui::CardWay::None);
        QCOMPARE(ui::cardWayOf(jet, nullptr), ui::CardWay::None);
    }

    // From the second reading of c7ec58c, B: a JetKVM of this PC's wall that answers, after an offer of this
    // program's came to nothing (its video does not come through to this PC), says so plainly over its card, with
    // "Show here" to try again; it is online (its dot and its alerts). A server's machine has no such word.
    void aJetKvmWhoseVideoDoesNotComeThroughSaysSoWithShowHere()
    {
        core::Status through = live();
        through.errorCode = u"not_through"_s;
        through.errorMessage = u"The JetKVM answers, but its video does not come through to this PC."_s;
        core::Machine jet = kvm();
        jet.kind = u"jetkvm"_s;
        jet.own = true;
        const WallTile card = ui::cardFor(jet, &through, 7, false);
        QCOMPARE(card.state, u"online"_s);
        QCOMPARE(card.title, u"No video from here"_s);
        QCOMPARE(card.text,
                 u"The JetKVM answers, but its video does not come through to this PC. Show here tries again."_s);
        QCOMPARE(card.action, u"Show here"_s);
        QCOMPARE(ui::cardWayOf(jet, &through), ui::CardWay::ShowHere);
        jet.own = false;
        QCOMPARE(ui::cardWayOf(jet, &through), ui::CardWay::None);
        QCOMPARE(ui::cardFor(jet, &through, 7, false).title, QString());
    }

    void aMachineSpokenToOverPlainHttpSaysSoBeforeItsAddress()
    {
        qkvm::core::Machine machine;
        machine.id = u"k1"_s;
        machine.name = u"NAS"_s;
        machine.host = u"192.168.1.22"_s;
        QCOMPARE(qkvm::ui::cardFor(machine, nullptr, 0, false).host, u"192.168.1.22"_s);
        // What this PC's own wall says of it in its details goes with its card.
        machine.note = u"Over HTTPS now"_s;
        QCOMPARE(qkvm::ui::cardFor(machine, nullptr, 0, false).note, u"Over HTTPS now"_s);
        machine.note.clear();
        machine.plain = true;
        QCOMPARE(qkvm::ui::cardFor(machine, nullptr, 0, false).host, u"http://192.168.1.22"_s);
        // One that is called by where it is keeps its name, and says the way beside it.
        machine.name = machine.host;
        const qkvm::ui::WallTile card = qkvm::ui::cardFor(machine, nullptr, 0, false);
        QCOMPARE(card.name, u"192.168.1.22"_s);
        QCOMPARE(card.host, u"http://192.168.1.22"_s);
    }

    void aMachineThatWasToldToRestartSaysSoWhileItIsAway()
    {
        using qkvm::ui::Restart;
        using qkvm::ui::WallTile;
        const auto card = [](const QString& state) {
            WallTile tile;
            tile.key = u"k1"_s;
            tile.name = u"NAS"_s;
            tile.state = state;
            tile.label = state == u"online"_s ? u"Online"_s : u"Offline"_s;
            tile.title = state == u"online"_s ? QString() : u"Offline"_s;
            tile.text = state == u"online"_s ? QString() : u"Device is unreachable"_s;
            tile.action = state == u"auth"_s ? u"Update login"_s : QString();
            tile.stream = 3;
            tile.meta = u"1920×1080"_s;
            tile.openable = true;
            return tile;
        };
        Restart restart{180'000, false};

        // Told a moment ago, and still there: it has not gone down yet. Its card is as it was.
        WallTile tile = card(u"online"_s);
        QVERIFY(qkvm::ui::whileRestarting(tile, restart, 1'000, false));
        QCOMPARE(tile, card(u"online"_s));
        QVERIFY(!restart.wentDown);

        // Away: that is called what it is, and not an outage. Neither its last picture nor a way
        // to mend a login that is only refused because the KVM is starting.
        for (const QString& state : {u"offline"_s, u"auth"_s, u"error"_s, u"no_signal"_s}) {
            tile = card(state);
            QVERIFY2(qkvm::ui::whileRestarting(tile, restart, 20'000, false), qPrintable(state));
            QCOMPARE(tile.state, u"connecting"_s);
            QCOMPARE(tile.label, u"Rebooting…"_s);
            QCOMPARE(tile.title, u"Rebooting…"_s);
            QCOMPARE(tile.text, u"Back in about a minute."_s);
            QCOMPARE(tile.action, QString());
            QCOMPARE(tile.stream, 0U);
            // What it is, and where, stays.
            QCOMPARE(tile.name, u"NAS"_s);
            QCOMPARE(tile.meta, u"1920×1080"_s);
            QVERIFY(tile.openable);
        }
        QVERIFY(restart.wentDown);

        // Back, after having been away: over, and its card is its own again.
        tile = card(u"online"_s);
        QVERIFY(!qkvm::ui::whileRestarting(tile, restart, 60'000, false));
        QCOMPARE(tile, card(u"online"_s));
    }

    void aRestartIsWaitedForNoLongerThanItTakes()
    {
        using qkvm::ui::Restart;
        using qkvm::ui::WallTile;
        WallTile away;
        away.state = u"offline"_s;
        away.title = u"Offline"_s;
        // A server's controller takes longer, and says so; and its card shows no readings meanwhile.
        away.server = {{u"power"_s, u"on"_s}};
        Restart restart{qkvm::ui::restartWithinMs, false};
        WallTile tile = away;
        QVERIFY(qkvm::ui::whileRestarting(tile, restart, qkvm::ui::restartWithinMs, true));
        QCOMPARE(tile.text, u"Back in a minute or two."_s);
        QVERIFY(tile.server.isEmpty());
        // Not back when the time is up: it is not coming, and its card says how it is.
        tile = away;
        QVERIFY(!qkvm::ui::whileRestarting(tile, restart, qkvm::ui::restartWithinMs + 1, true));
        QCOMPARE(tile, away);

        // And one that never went down at all is no longer waited for either.
        Restart never{qkvm::ui::restartWithinMs, false};
        WallTile there;
        there.state = u"online"_s;
        QVERIFY(qkvm::ui::whileRestarting(there, never, 170'000, false));
        QVERIFY(!qkvm::ui::whileRestarting(there, never, 180'001, false));
    }

    void aMachineWithAConsoleOfItsOwnSaysSoOnItsCard()
    {
        qkvm::core::Machine machine;
        machine.id = u"k1"_s;
        machine.name = u"NAS"_s;
        QVERIFY(!qkvm::ui::cardFor(machine, nullptr, 0, false).openable);
        machine.page = QUrl(u"http://192.168.1.22/"_s);
        QVERIFY(qkvm::ui::cardFor(machine, nullptr, 0, false).openable);
        // Whether or not the server has said how the machine is.
        qkvm::core::Status status;
        status.state = u"offline"_s;
        QVERIFY(qkvm::ui::cardFor(machine, &status, 0, false).openable);
    }

    void aMachineTheServerHasNotSpokenOfIsConnecting()
    {
        const WallTile card = ui::cardFor(kvm(), nullptr, 7, false);
        QCOMPARE(card.key, u"a"_s);
        QCOMPARE(card.name, u"Build server"_s);
        QCOMPARE(card.host, u"192.168.1.21"_s);
        QCOMPARE(card.stream, 7U);
        QCOMPARE(card.state, u"connecting"_s);
        QCOMPARE(card.title, u"Connecting…"_s);
    }

    void aLiveMachineSaysNothingOverItsPicture()
    {
        core::Status status = live();
        status.powerIsKnown = true;
        status.power = true;
        status.temperature = 41.4;
        const WallTile card = ui::cardFor(kvm(), &status, 7, false);
        QCOMPARE(card.state, u"online"_s);
        QCOMPARE(card.title, QString());
        QCOMPARE(card.text, QString());
        QCOMPARE(card.meta, u"1920×1080 · Power on · 41°C"_s);
        QVERIFY(card.server.isEmpty());
    }

    void noSignalSaysWhatThatUsuallyMeans()
    {
        core::Status status;
        status.state = u"no_signal"_s;
        status.signal = false;
        const WallTile card = ui::cardFor(kvm(), &status, 7, false);
        QCOMPARE(card.title, u"No signal"_s);
        QCOMPARE(card.text, u"The computer is off, asleep or not plugged into the KVM."_s);
    }

    void anOfflineMachineSaysWhyAndSinceWhen()
    {
        core::Status status;
        status.state = u"offline"_s;
        status.lastSeenSecondsAgo = 240;
        WallTile card = ui::cardFor(kvm(), &status, 7, false);
        QCOMPARE(card.title, u"Offline"_s);
        QCOMPARE(card.text, u"Device is unreachable · last seen 4 min ago"_s);

        status.errorMessage = u"Timed out"_s;
        card = ui::cardFor(kvm(), &status, 7, false);
        QCOMPARE(card.text, u"Timed out · last seen 4 min ago"_s);
    }

    void aRefusedLoginShowsTheServersSentence()
    {
        core::Status status;
        status.state = u"auth"_s;
        status.errorCode = u"auth"_s;
        status.errorMessage = u"The KVM refused the user name or password"_s;
        const WallTile card = ui::cardFor(kvm(), &status, 7, false);
        QCOMPARE(card.title, u"Login failed"_s);
        QCOMPARE(card.text, u"The KVM refused the user name or password"_s);
    }

    void aKvmOfThisPcsOwnWallHasTheWayToMendItOnItsCard()
    {
        // A KVM that the program speaks to itself: this PC's own wall says so, and nobody else.
        core::Machine own = kvm();
        own.own = true;

        // It refused the sign-in: the way to the box that changes it, in the page's words.
        core::Status refused;
        refused.state = u"auth"_s;
        refused.errorCode = u"auth_failed"_s;
        refused.errorMessage = u"Login rejected - check the username and password"_s;
        WallTile card = ui::cardFor(own, &refused, 7, false);
        QCOMPARE(card.title, u"Login failed"_s);
        QCOMPARE(card.text, u"Login rejected - check the username and password"_s);
        QCOMPARE(card.action, u"Update login"_s);
        QCOMPARE(ui::cardWayOf(own, &refused), ui::CardWay::UpdateLogin);
        // So where no password is kept for it: that is mended in the same box.
        refused.errorCode = u"no_password"_s;
        QCOMPARE(ui::cardFor(own, &refused, 7, false).action, u"Update login"_s);

        // It shows another certificate than it is known by: the way to the look at the two.
        core::Status another;
        another.state = u"error"_s;
        another.errorCode = u"cert_mismatch"_s;
        another.errorMessage = u"Certificate does not match the one saved for this device"_s;
        card = ui::cardFor(own, &another, 7, false);
        QCOMPARE(card.title, u"Problem"_s);
        QCOMPARE(card.text, u"Certificate does not match the one saved for this device"_s);
        QCOMPARE(card.action, u"Review certificate"_s);
        QCOMPARE(ui::cardWayOf(own, &another), ui::CardWay::ReviewCertificate);

        // Another JetKVM (or server) answers at its address (J5): the way to the box that changes it, where its
        // password is typed to take the one that answers now.
        core::Status otherOne;
        otherOne.state = u"error"_s;
        otherOne.errorCode = u"another_machine"_s;
        otherOne.errorMessage = u"Another JetKVM answers at this address"_s;
        QCOMPARE(ui::cardFor(own, &otherOne, 7, false).action, u"Update login"_s);
        QCOMPARE(ui::cardWayOf(own, &otherOne), ui::CardWay::UpdateLogin);
        QCOMPARE(ui::cardWayOf(kvm(), &otherOne), ui::CardWay::None); // (a server's own card: its page offers it)

        // Any other trouble has no button, and neither has a KVM that is simply there, or one
        // that was not spoken of yet.
        another.errorCode = u"bad_response"_s;
        QCOMPARE(ui::cardFor(own, &another, 7, false).action, QString());
        QCOMPARE(ui::cardWayOf(own, &another), ui::CardWay::None);
        for (const QString& state : {u"online"_s, u"no_signal"_s, u"offline"_s, u"connecting"_s}) {
            core::Status status = live();
            status.state = state;
            QVERIFY2(ui::cardFor(own, &status, 7, false).action.isEmpty(), qPrintable(state));
            QCOMPARE(ui::cardWayOf(own, &status), ui::CardWay::None);
        }
        QCOMPARE(ui::cardFor(own, nullptr, 7, false).action, QString());
        QCOMPARE(ui::cardWayOf(own, nullptr), ui::CardWay::None);
    }

    void noOtherMachineHasAButtonOnItsCard()
    {
        // What a QKVM server lists is mended on the server's page: whatever it says of a machine,
        // the card has no button, and leads nowhere.
        core::Status refused;
        refused.state = u"auth"_s;
        refused.errorCode = u"auth_failed"_s;
        core::Status another;
        another.state = u"error"_s;
        another.errorCode = u"cert_mismatch"_s;
        for (const core::Status& status : {refused, another}) {
            const WallTile card = ui::cardFor(kvm(), &status, 7, false);
            QVERIFY(!card.title.isEmpty());
            QCOMPARE(card.action, QString());
            QCOMPARE(ui::cardWayOf(kvm(), &status), ui::CardWay::None);
        }
        // Nor a server seen through its controller.
        core::Machine server = kvm();
        server.hasPicture = false;
        refused.hasPicture = false;
        refused.server = core::ServerReadings{};
        QCOMPARE(ui::cardFor(server, &refused, 0, false).action, QString());
    }

    void aStateTheProgramDoesNotKnowIsAProblem()
    {
        core::Status status;
        status.state = u"something_new"_s;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).title, u"Problem"_s);
    }

    void aPowerLightThatSaysOffUnderAPictureIsNotBelieved()
    {
        core::Status status = live();
        status.powerIsKnown = true;
        status.power = false;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta, u"1920×1080"_s);
        // Without a picture it is believed.
        status.signal = false;
        status.width = 0;
        status.height = 0;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta, u"Power off"_s);
    }

    void aFingerbotIsSaidAndItsBatteryWhereThatIsLow()
    {
        // As the page's footer says it (app.js, machineText), after the power light and before the USB: shown on the
        // card, and with it in the machine's details.
        core::Status status = live();
        status.fingerbot = core::Fingerbot{80, false};
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta, u"1920×1080 · Fingerbot"_s);
        status.fingerbot = core::Fingerbot{12, true};
        status.powerIsKnown = true;
        status.power = true;
        status.keyboard = false;
        status.temperature = 43.4;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta,
                 u"1920×1080 · Power on · Fingerbot battery low (12%) · USB not connected · 43°C"_s);
        // Its battery not asked yet: that it has one.
        status.fingerbot = core::Fingerbot{};
        QVERIFY(ui::cardFor(kvm(), &status, 7, false).meta.contains(u" · Fingerbot · "_s));
    }

    void whatIsInItsDriveIsSaidToEverybodyWhoSeesTheCard()
    {
        // As the page's footer says it (app.js, metaText): after what it says of the machine, "CD: debian-13.iso",
        // "Flash: ...", "Disk: ...", and "Media: ..." for a drive it has no word for. On every card, a server's too.
        core::Status status = live();
        status.media = core::MountedMedia{};
        status.media->drive = u"cd"_s;
        status.media->name = u"debian-13.iso"_s;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta, u"1920×1080 · CD: debian-13.iso"_s);
        status.media->drive = u"flash"_s;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta, u"1920×1080 · Flash: debian-13.iso"_s);
        status.media->drive = u"disk"_s;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta, u"1920×1080 · Disk: debian-13.iso"_s);
        status.media->drive = u"tape"_s;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta, u"1920×1080 · Media: debian-13.iso"_s);
        // Where nothing else is said of the machine, that alone.
        status.media->drive = u"cd"_s;
        status.width = 0;
        status.height = 0;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta, u"CD: debian-13.iso"_s);
        // A server whose controller shows its panel: its model, and what is in its drive.
        core::Machine server = kvm();
        server.hasPicture = false;
        status.hasPicture = false;
        status.model = u"SRV-2200"_s;
        status.server = core::ServerReadings{};
        QCOMPARE(ui::cardFor(server, &status, 0, false).meta, u"SRV-2200 · CD: debian-13.iso"_s);
        // Nothing in it: nothing more is said.
        status.media.reset();
        QCOMPARE(ui::cardFor(server, &status, 0, false).meta, u"SRV-2200"_s);
    }

    void aKeyboardThatIsNotPluggedInIsSaid()
    {
        core::Status status = live();
        status.keyboard = false;
        QCOMPARE(ui::cardFor(kvm(), &status, 7, false).meta, u"1920×1080 · USB not connected"_s);
    }

    void aKvmWithSeveralComputersShowsItsPorts()
    {
        core::Status status = live();
        status.ports = {{u"Router"_s, false}, {QString(), false}, {QString(), true}};
        status.shownPort = 1;
        const WallTile card = ui::cardFor(kvm(), &status, 7, false);
        QCOMPARE(card.meta, u"Port 2 · 1920×1080"_s);
        QCOMPARE(card.ports.size(), 3);
        const QVariantMap first = card.ports[0].toMap();
        QCOMPARE(first[u"label"_s].toString(), u"1"_s);
        QCOMPARE(first[u"hint"_s].toString(), u"Router of Build server: not on screen"_s);
        QCOMPARE(first[u"shown"_s].toBool(), false);
        const QVariantMap second = card.ports[1].toMap();
        QCOMPARE(second[u"hint"_s].toString(), u"Port 2 of Build server: on screen now"_s);
        QCOMPARE(second[u"shown"_s].toBool(), true);
        const QVariantMap third = card.ports[2].toMap();
        QCOMPARE(third[u"hint"_s].toString(), u"Port 3 of Build server: nothing connected"_s);
        QCOMPARE(third[u"empty"_s].toBool(), true);
    }

    void aServerShowsHowItIsDoingInPlaceOfAPicture()
    {
        core::Machine machine = kvm();
        machine.kind = u"redfish"_s;
        machine.hasPicture = false;
        core::Status status;
        status.state = u"online"_s;
        status.hasPicture = false;
        status.power = true;
        status.temperature = 38.0;
        status.model = u"Example board"_s;
        core::ServerReadings readings;
        readings.health = u"Warning"_s;
        readings.locator = true;
        readings.fans = 6;
        readings.fansRunning = 5;
        readings.fansNotWell = 1;
        readings.processor = u"Example processor"_s;
        readings.processors = 2;
        readings.memoryGib = 256.0;
        status.server = readings;

        // No picture of it has come: it names no stream, whatever stream is kept ready for it.
        const WallTile card = ui::cardFor(machine, &status, 7, false);
        QCOMPARE(card.stream, 0U);
        QCOMPARE(card.meta, u"Example board"_s);
        QCOMPARE(card.server[u"power"_s].toString(), u"on"_s);
        QCOMPARE(card.server[u"tone"_s].toString(), u"warn"_s);
        QCOMPARE(card.server[u"locating"_s].toBool(), true);
        QCOMPARE(card.server[u"hardware"_s].toString(), u"2 × Example processor · 256 GiB"_s);
        const QVariantList shown = card.server[u"readings"_s].toList();
        QCOMPARE(shown.size(), 3);
        QCOMPARE(shown[0].toMap()[u"label"_s].toString(), u"Health"_s);
        QCOMPARE(shown[0].toMap()[u"value"_s].toString(), u"Warning"_s);
        QCOMPARE(shown[0].toMap()[u"tone"_s].toString(), u"warn"_s);
        QCOMPARE(shown[1].toMap()[u"value"_s].toString(), u"38 °C"_s);
        QCOMPARE(shown[2].toMap()[u"label"_s].toString(), u"Fans · 1 not well"_s);
        QCOMPARE(shown[2].toMap()[u"value"_s].toString(), u"5 running"_s);

        // Only a controller that says the server is off makes it "Off".
        status.power = false;
        QCOMPARE(ui::cardFor(machine, &status, 0, false).server[u"power"_s].toString(), u"off"_s);
        status.power.reset();
        QCOMPARE(ui::cardFor(machine, &status, 0, false).server[u"power"_s].toString(), u"on"_s);
    }

    void aServersCardSaysWhyItsScreenIsNotShown()
    {
        // As the page's panel says it (app.js, panel-why): where its screen could be had and is not, why, for as long
        // as that stands; and nothing of it where nothing stands in the way.
        core::Machine machine = kvm();
        machine.kind = u"redfish"_s;
        machine.hasPicture = false;
        core::Status status;
        status.state = u"online"_s;
        status.hasPicture = false;
        status.power = true;
        core::ServerReadings readings;
        readings.health = u"OK"_s;
        status.server = readings;
        QCOMPARE(ui::cardFor(machine, &status, 0, false).server[u"why"_s].toString(), QString());
        status.server->noScreen = u"Someone else is using the server's console"_s;
        QCOMPARE(ui::cardFor(machine, &status, 0, false).server[u"why"_s].toString(),
                 u"Its screen is not shown: Someone else is using the server's console"_s);
    }

    void aServersCardSaysThatItsSetupOverrideMayBeSet()
    {
        // (Review 4's MEDIUM 1.) A start into BIOS setup that did not end: the card says it, as its driver words it,
        // for as long as that stands; nothing of it where nothing stands.
        core::Machine machine = kvm();
        machine.kind = u"redfish"_s;
        machine.hasPicture = false;
        core::Status status;
        status.state = u"online"_s;
        status.hasPicture = false;
        status.power = true;
        status.server = core::ServerReadings{};
        QCOMPARE(ui::cardFor(machine, &status, 0, false).server[u"setupLeft"_s].toString(), QString());
        status.server->setupLeft = u"Its BIOS setup boot override may be set."_s;
        QCOMPARE(ui::cardFor(machine, &status, 0, false).server[u"setupLeft"_s].toString(),
                 u"Its BIOS setup boot override may be set."_s);
    }

    void aServerThatRunsShowsItsOwnScreenOnceAPictureOfItHasCome()
    {
        core::Machine machine = kvm();
        machine.kind = u"redfish"_s;
        machine.hasPicture = false;
        core::Status status;
        status.state = u"online"_s;
        status.hasPicture = false;
        status.hasScreen = true;
        status.power = true;
        status.temperature = 38.0;
        status.model = u"Example board"_s;
        core::ServerReadings readings;
        readings.health = u"OK"_s;
        status.server = readings;

        // Before the first picture: how it is doing, and which machine it is.
        WallTile card = ui::cardFor(machine, &status, 7, false);
        QCOMPARE(card.stream, 0U);
        QVERIFY(!card.server.isEmpty());
        QCOMPARE(card.meta, u"Example board"_s);

        // With one: its screen, as a KVM's card shows a screen, and nothing said over it.
        status.width = 1024;
        status.height = 768;
        card = ui::cardFor(machine, &status, 7, true);
        QCOMPARE(card.stream, 7U);
        QVERIFY(card.server.isEmpty());
        QCOMPARE(card.title, QString());
        QCOMPARE(card.text, QString());
        QCOMPARE(card.state, u"online"_s);
        // The line under it says what the panel would have. Good health is not news.
        QCOMPARE(card.meta, u"1024×768 · 38°C"_s);

        // A fault does not cover the screen of a server that runs: the card's edge says the rest,
        // and the line names the fault.
        status.state = u"error"_s;
        status.server->health = u"Critical"_s;
        card = ui::cardFor(machine, &status, 7, true);
        QCOMPARE(card.stream, 7U);
        QVERIFY(card.server.isEmpty());
        QCOMPARE(card.title, QString());
        QCOMPARE(card.state, u"error"_s);
        QCOMPARE(card.meta, u"1024×768 · Critical · 38°C"_s);

        // Switched off, what its controller says of it is what there is to see: not a last picture.
        status.state = u"no_signal"_s;
        status.power = false;
        card = ui::cardFor(machine, &status, 7, true);
        QCOMPARE(card.stream, 0U);
        QCOMPARE(card.server[u"power"_s].toString(), u"off"_s);

        // And a server without a stream is never shown as one with a screen.
        status.state = u"online"_s;
        status.power = true;
        card = ui::cardFor(machine, &status, 0, true);
        QCOMPARE(card.stream, 0U);
        QVERIFY(!card.server.isEmpty());
    }

    // b5: a PC with Intel AMT says on its card which of its three views it is in, with the age of
    // the still it shows (the page's viewNote, after what its line says otherwise); its large view
    // says that it is live there; in health only it shows no picture and its panel says why. Any
    // other machine says a still's age where it shows one, and nothing more.
    void aPcWithIntelAmtSaysWhichViewItIsIn()
    {
        core::Machine pc;
        pc.id = u"p"_s;
        pc.name = u"Front desk"_s;
        pc.kind = u"amt"_s;
        pc.hasPicture = false;
        pc.amtView = u"health"_s;
        core::Status status = live();
        status.hasPicture = false;
        status.hasScreen = true;
        status.server = core::ServerReadings{};
        status.server->amtView = u"stills"_s;
        WallTile card = ui::cardFor(pc, &status, 7, true, ui::StillCame{137});
        QCOMPARE(card.stream, 7u);
        QCOMPARE(card.meta, u"1920×1080 · Stills, 2 min ago"_s);
        QCOMPARE(card.largeMeta, u"1920×1080 · Stills, 2 min ago"_s);
        card = ui::cardFor(pc, &status, 7, true);
        QCOMPARE(card.meta, u"1920×1080 · Stills"_s);
        QCOMPARE(card.largeMeta, u"1920×1080 · Stills on the wall, live here"_s);
        QCOMPARE(ui::viewNoteOf(pc, &status, ui::StillCame{42}, false), u"Stills, 42 s ago"_s);
        QCOMPARE(ui::viewNoteOf(pc, &status, ui::StillCame{}, false), u"Stills"_s);
        status.server->amtView = u"live"_s;
        QCOMPARE(ui::viewNoteOf(pc, &status, ui::StillCame{4}, false), u"Live KVM · still, 4 s ago"_s);
        QCOMPARE(ui::viewNoteOf(pc, &status, std::nullopt, true), u"Live KVM"_s);
        // Health only: no picture, whatever it was handed; its panel says why.
        status.server->amtView = u"health"_s;
        status.server->noScreen = u"This PC is set to health only: qKVM opens no KVM session to it."_s;
        status.width = 0;
        card = ui::cardFor(pc, &status, 7, true);
        QCOMPARE(card.stream, 0u);
        QCOMPARE(card.meta, u"Health only"_s);
        QCOMPARE(card.server.value(u"why"_s).toString(),
                 u"Its screen is not shown: This PC is set to health only: qKVM opens no KVM session to it."_s);
        // Before its status: the wall's word.
        QCOMPARE(ui::cardFor(pc, nullptr, 0, false).meta, u"Health only"_s);
        // A server that says no view of it (0.2.2): no view is said, only the age of a still it shows (b11).
        core::Machine older = pc;
        older.amtView.clear();
        core::Status olderStatus = live();
        olderStatus.hasPicture = false;
        olderStatus.hasScreen = true;
        olderStatus.server = core::ServerReadings{};
        QCOMPARE(ui::viewNoteOf(older, &olderStatus, ui::StillCame{42}, false), u"still, 42 s ago"_s);
        QCOMPARE(ui::viewNoteOf(older, &olderStatus, std::nullopt, true), QString());
        card = ui::cardFor(older, &olderStatus, 7, true);
        QCOMPARE(card.meta, u"1920×1080"_s);
        QVERIFY(card.amtView.isEmpty());
        // Any other machine.
        core::Status kvmStatus = live();
        QCOMPARE(ui::viewNoteOf(kvm(), &kvmStatus, ui::StillCame{12}, false), u"still, 12 s ago"_s);
        QCOMPARE(ui::viewNoteOf(kvm(), &kvmStatus, std::nullopt, true), QString());
        QCOMPARE(ui::cardFor(kvm(), &kvmStatus, 7, true).meta, u"1920×1080"_s);
        QVERIFY(ui::cardFor(kvm(), &kvmStatus, 7, true).server.value(u"why"_s).toString().isEmpty());
    }

    // b10: a card says how large the machine's screen is in its own pixels, as its status says it: its
    // large view is drawn by that at a scale.
    void aCardSaysHowLargeItsScreenIs()
    {
        core::Status status = live();
        QCOMPARE(ui::cardFor(kvm(), &status, 7, true).pictureSize, QSize(1920, 1080));
        status.width = 0;
        QVERIFY(!ui::cardFor(kvm(), &status, 7, true).pictureSize.isValid());
        QVERIFY(!ui::cardFor(kvm(), nullptr, 7, true).pictureSize.isValid());
    }

    void howLongAgoIsWrittenAsThePageWritesIt_data()
    {
        QTest::addColumn<int>("seconds");
        QTest::addColumn<QString>("written");
        QTest::newRow("a moment") << 3 << u"just now"_s;
        QTest::newRow("seconds") << 45 << u"45 s ago"_s;
        QTest::newRow("still seconds") << 89 << u"89 s ago"_s;
        QTest::newRow("minutes") << 90 << u"2 min ago"_s;
        QTest::newRow("many minutes") << 5399 << u"90 min ago"_s;
        QTest::newRow("hours") << 5400 << u"2 h ago"_s;
        QTest::newRow("a day and a half") << 129600 << u"2 d ago"_s;
    }

    void howLongAgoIsWrittenAsThePageWritesIt()
    {
        QFETCH(int, seconds);
        QFETCH(QString, written);
        QCOMPARE(ui::ago(seconds), written);
    }
};

QTEST_GUILESS_MAIN(TestWallCards)

#include "test_wall_cards.moc"
