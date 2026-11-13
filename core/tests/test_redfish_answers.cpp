#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <qkvm/core/redfish_answers.h>

using namespace Qt::StringLiterals;
namespace redfish = qkvm::core::redfish;
using qkvm::core::Status;

namespace {

QJsonObject parsed(const QByteArray& json)
{
    return QJsonDocument::fromJson(json).object();
}

QByteArray written(const QJsonObject& said)
{
    return QJsonDocument(said).toJson(QJsonDocument::Compact);
}

// The computer as the web's stand-in (tests/fake_redfish.py) answers it, which is what the two AMI boards
// answered: one computer, "Self".
QJsonObject aComputer()
{
    // (Its mark is put in below, and every raw string in this file is one line with its quotes in pairs: moc
    // reads a raw string as an ordinary one, and loses its way in the file at one that is not.)
    QJsonObject system =
        parsed(R"({ )"
               R"("IndicatorLED": "Off", "IndicatorLED@Redfish.AllowableValues": ["Lit", "Blinking", "Off"], )"
               R"("Id": "Self", "Model": "SRV-2200", "Manufacturer": "Example Systems", )"
               R"("HostName": null, "PowerState": "On", "BiosVersion": "F12", "SerialNumber": "SN0000000000", )"
               R"("UUID": "4c4c4544-0042-3510-8054-b9c04f4a3532", )"
               R"("Status": {"Health": "OK", "HealthRollup": "OK", "State": "Enabled"}, )"
               R"("ProcessorSummary": {"Count": 2, "Model": "Example Processor 4000"}, )"
               R"("MemorySummary": {"TotalSystemMemoryGiB": 256}, )"
               R"("Links": {"Chassis": [{"@odata.id": "/redfish/v1/Chassis/Self"}], )"
               R"("ManagedBy": [{"@odata.id": "/redfish/v1/Managers/Self"}]}, )"
               R"("Actions": {"#ComputerSystem.Reset": { )"
               R"("@Redfish.ActionInfo": "/redfish/v1/Systems/Self/ResetActionInfo", )"
               R"("target": "/redfish/v1/Systems/Self/Actions/ComputerSystem.Reset"}} )"
               R"(} )");
    system.insert(u"@odata.etag"_s, u"\"1791401332\""_s);
    return system;
}

// Its sensors in the older shape (fake_redfish.py, Thermal): a temperature that is not fitted, a fan that is
// slow and says so, and one that is not fitted.
QJsonObject itsThermal(const QJsonValue& cpu = 61, const QString& cpuHealth = u"OK"_s)
{
    QJsonObject thermal = parsed(
        R"({ )"
        R"("Temperatures": [ )"
        R"({"Name": "CPU0_TEMP", "ReadingCelsius": 61, "UpperThresholdCritical": 98, "Status": {"Health": "OK", "State": "Enabled"}}, )"
        R"({"Name": "CPU1_TEMP", "ReadingCelsius": 55, "UpperThresholdCritical": 98, "Status": {"Health": "OK", "State": "Enabled"}}, )"
        R"({"Name": "DIMM_A_TEMP", "ReadingCelsius": 45, "UpperThresholdCritical": 98, "Status": {"Health": "OK", "State": "Enabled"}}, )"
        R"({"Name": "M2_TEMP", "ReadingCelsius": null, "UpperThresholdCritical": 98, "Status": {"State": "Absent"}} )"
        R"(], )"
        R"("Fans": [ )"
        R"({"Name": "CPU0_FAN", "Reading": 1650, "ReadingUnits": "RPM", "Status": {"Health": "OK", "State": "Enabled"}}, )"
        R"({"Name": "CPU1_FAN", "Reading": 450, "ReadingUnits": "RPM", "Status": {"Health": "Warning", "State": "Enabled"}}, )"
        R"({"Name": "SYS_FAN5", "Reading": null, "ReadingUnits": "RPM", "Status": {"State": "Absent"}} )"
        R"(] )"
        R"(} )");
    QJsonArray temperatures = thermal.value("Temperatures"_L1).toArray();
    QJsonObject first = temperatures.at(0).toObject();
    first.insert(u"ReadingCelsius"_s, cpu);
    first.insert(u"Status"_s, QJsonObject{{u"Health"_s, cpuHealth}, {u"State"_s, u"Enabled"_s}});
    temperatures.replace(0, first);
    thermal.insert(u"Temperatures"_s, temperatures);
    return thermal;
}

redfish::Looked lookedAt(const QJsonObject& system, const QJsonObject& thermal = itsThermal())
{
    redfish::Looked looked;
    looked.system = system;
    looked.temperatures = redfish::temperaturesOf(thermal);
    looked.fans = redfish::fansOf(thermal);
    looked.version = u"1.42.0"_s;
    return looked;
}

QJsonObject with(QJsonObject system, const QString& name, const QJsonValue& value)
{
    system.insert(name, value);
    return system;
}

QString digestOf(const QString& number)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(number.toLower().toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// What a controller sent where a list belongs, as the web tries it (test_what_a_controller_sent_of_another_kind.py).
QList<QJsonValue> odd()
{
    return {QJsonValue(5), QJsonValue(true), QJsonValue(QJsonObject{{u"a"_s, 1}}), QJsonValue(1.5),
            QJsonValue(u"abc"_s)};
}

// What a test reads of what it has made sure is there (QVERIFY) first: the thing, or what nothing is.
qkvm::core::ServerReadings readings(const Status& status)
{
    return status.server.value_or(qkvm::core::ServerReadings{});
}
redfish::Light lit(const std::optional<redfish::Light>& light)
{
    return light.value_or(redfish::Light{});
}

} // namespace

// What a server's management controller says over Redfish, read (redfish_answers.h): ported from the web's
// own tests of the same readers (../app/tests/test_redfish.py and the others named at each).
class TestRedfishAnswers : public QObject {
    Q_OBJECT

private slots:
    void aControllerCannotSendTheProgramSomewhereElse()
    {
        // (test_redfish.py:750) The session goes with every question. A controller that names a computer at
        // some other address would be handed it there, were its links followed as given.
        const QList<QJsonValue> elsewhere{u"https://elsewhere.example/redfish/v1/Systems/1"_s,
                                          u"//elsewhere.example/x"_s,
                                          u"/api/v1/session"_s,
                                          u"/redfish/v1/Systems/1?x=1"_s,
                                          QJsonValue(7),
                                          QJsonValue(),
                                          QJsonValue(u"/redfish/v1/Systems/1#x"_s),
                                          QJsonValue(u" /redfish/v1/Systems/1"_s),
                                          QJsonValue(u"/redfish/v1/Systems/1\n"_s),
                                          QJsonValue(u"/redfish/v10"_s),
                                          QJsonValue(u"/redfish/v1//Systems"_s)};
        // Nor to some other part of itself, by way of a path that climbs back out.
        const QList<QJsonValue> climbing{u"/redfish/v1/../../api/v1/session"_s,
                                         u"/redfish/v1/%2e%2e/%2E%2E/api"_s,
                                         u"/redfish/v1/./x"_s,
                                         u"/redfish/v1/a%2f..%2fb"_s,
                                         u"/redfish/v1/a%5c.."_s,
                                         u"/redfish/v1/a%2Fb"_s,
                                         u"/redfish/v1/%2e"_s,
                                         QJsonValue(u"/redfish/v1/"_s + QString(300, u'a'))};
        // A "%" is an escape, of two hexadecimal digits, or nothing: the web refuses what is none (and what is
        // read as one here would hide a backslash or a slash: "%%5c" is "%" and "\").
        const QList<QJsonValue> noEscape{u"/redfish/v1/a%%5c.."_s, u"/redfish/v1/a%%2f.."_s, u"/redfish/v1/a%"_s,
                                         u"/redfish/v1/%"_s,       u"/redfish/v1/a%2"_s,     u"/redfish/v1/a%zz/b"_s,
                                         u"/redfish/v1/a%2g"_s};
        // Stricter than the web: an escape of an escape ("%25", which a second decoding would make another
        // thing of), and one of a byte beyond ASCII (an overlong ".", "%c0%ae", among them).
        const QList<QJsonValue> twice{u"/redfish/v1/a%252e%252e"_s, u"/redfish/v1/%25"_s, u"/redfish/v1/a%25zz"_s};
        // Nor one of a sign that does not print: a line feed, a nought, a delete.
        const QList<QJsonValue> unprinted{u"/redfish/v1/a%0a"_s, u"/redfish/v1/a%00b"_s, u"/redfish/v1/a%1F"_s,
                                          u"/redfish/v1/a%7f"_s, u"/redfish/v1/a%0D%0A"_s};
        for (const QJsonValue& claimed : unprinted) {
            QVERIFY2(redfish::link(claimed).isEmpty(), qPrintable(claimed.toVariant().toString()));
        }
        const QList<QJsonValue> beyond{u"/redfish/v1/%c0%ae%c0%ae"_s, u"/redfish/v1/caf%C3%A9"_s, u"/redfish/v1/a%80"_s,
                                       u"/redfish/v1/a%FF"_s};
        for (const QJsonValue& claimed : noEscape + twice + beyond) {
            QVERIFY2(redfish::link(claimed).isEmpty(), qPrintable(claimed.toVariant().toString()));
        }
        for (const QJsonValue& claimed : elsewhere + climbing) {
            QVERIFY2(redfish::link(claimed).isEmpty(), qPrintable(claimed.toVariant().toString()));
        }
        for (const QString& honest : {u"/redfish/v1/Systems/Self"_s, u"/redfish/v1/Chassis/Self/Thermal"_s,
                                      u"/redfish/v1/Systems/System.Embedded.1"_s, u"/redfish/v1/Systems/a%20b/"_s,
                                      u"/redfish/v1"_s, u"/redfish/v1/"_s, u"/redfish/v1/Systems/a..b"_s,
                                      u"/redfish/v1/Systems/a%2Db"_s, u"/redfish/v1/Systems/%7Ex%7e"_s}) {
            QCOMPARE(redfish::link(honest), honest);
        }
        // As long as it may be, and not a sign longer.
        const QString longest = u"/redfish/v1/"_s + QString(300 - 12, u'a');
        QCOMPARE(redfish::link(longest), longest);
        QVERIFY(redfish::link(longest + u'a').isEmpty());
    }

    void whatAControllerSaysAboutItselfIsNotTakenAtFaceValue()
    {
        // (test_redfish.py:766) A name nobody filled in, a name that is one line of script, a temperature
        // that is no number.
        QJsonObject system = aComputer();
        system.insert(u"Model"_s, u"To Be Filled By O.E.M."_s);
        system.insert(u"Manufacturer"_s, u"Rack\nMaker <script>"_s + QString(300, u'x'));
        const redfish::Seen seen = redfish::statusFrom(lookedAt(system, itsThermal(u"hot"_s)));
        QCOMPARE(seen.status.state, u"online"_s);
        QVERIFY(seen.status.model.startsWith(u"Rack Maker <script>"_s));
        QVERIFY(seen.status.model.size() <= 60);
        QCOMPARE(seen.status.temperature, std::optional<double>(55));
        const QList<redfish::Temperature> temperatures = redfish::temperaturesOf(itsThermal(u"hot"_s));
        QVERIFY(temperatures.contains(redfish::Temperature{u"CPU1_TEMP"_s, 55, true}));
        QVERIFY(std::ranges::none_of(temperatures, [](const auto& one) { return one.name == u"CPU0_TEMP"_s; }));
        QCOMPARE(redfish::named({u""_s, u"Default string"_s, u" n/a "_s, u"SRV-1100"_s}), u"SRV-1100"_s);
        for (const QString& nobody : {u"to be filled by o.e.m."_s, u"TO BE FILLED BY OEM"_s, u"System Product Name"_s,
                                      u"Not Specified"_s, u"NA"_s, u"N/A"_s, u"Unknown"_s, u"   "_s}) {
            QCOMPARE(redfish::named({nobody}), QString());
        }
        // (A name that has one of those in it is a name.)
        QCOMPARE(redfish::named({u"Unknown Systems Inc"_s}), u"Unknown Systems Inc"_s);
        QCOMPARE(redfish::named({QJsonValue(QJsonObject{{u"a"_s, 1}}), QJsonValue(true), u"X1"_s}), u"X1"_s);
    }

    void aBoardThatSaysTooMuchAboutItsProcessorIsCutShort()
    {
        // (test_redfish.py:787) The shape of what one real board answered: a name, a run of spaces, and what
        // else it knows about the processor (redfish.py:2084).
        QJsonObject system = aComputer();
        system.insert(u"ProcessorSummary"_s,
                      QJsonObject{{u"Count"_s, 1},
                                  {u"Model"_s, u"Example Processor 2000 CPU              , 3000 Mhz, 4 Core(s), "
                                               u"8 Logical Processor(s)"_s}});
        system.insert(u"Model"_s, u""_s);
        system.insert(u"Manufacturer"_s, u"To Be Filled By O.E.M."_s);
        const redfish::Seen seen = redfish::statusFrom(lookedAt(system));
        QVERIFY(seen.status.server);
        QCOMPARE(readings(seen.status).processor, u"Example Processor 2000 CPU"_s);
        QCOMPARE(readings(seen.status).processors, 1);
        // Nobody filled it in, and nothing is made up in its place.
        QCOMPARE(seen.status.model, QString());
        // One that says nothing but what follows the comma names no processor.
        system.insert(u"ProcessorSummary"_s, QJsonObject{{u"Model"_s, u", 3000 Mhz"_s}});
        QCOMPARE(readings(redfish::statusFrom(lookedAt(system)).status).processor, QString());
    }

    void aCriticalFaultIsAProblemAndAWarningIsNot()
    {
        // (test_redfish.py:799) One slow fan: what a Gigabyte board said of itself while running fine.
        QJsonObject system = aComputer();
        system.insert(u"Status"_s, QJsonObject{{u"Health"_s, u"OK"_s}, {u"HealthRollup"_s, u"Warning"_s}});
        redfish::Seen seen = redfish::statusFrom(lookedAt(system));
        QCOMPARE(seen.status.state, u"online"_s);
        QCOMPARE(readings(seen.status).health, u"Warning"_s);
        QVERIFY(!seen.fault);
        // It says where to look: the alert that goes out carries this line.
        system.insert(u"Status"_s, QJsonObject{{u"Health"_s, u"OK"_s}, {u"HealthRollup"_s, u"Critical"_s}});
        seen = redfish::statusFrom(lookedAt(system, itsThermal(61, u"Critical"_s)));
        QCOMPARE(seen.status.state, u"error"_s);
        QCOMPARE(seen.status.errorCode, u"health"_s);
        QCOMPARE(seen.status.errorMessage, u"The server reports a critical fault (CPU0_TEMP, CPU1_FAN)"_s);
        QCOMPARE(seen.fault, std::optional<QString>(u" (CPU0_TEMP, CPU1_FAN)"_s));
        // Still told how it is, and still switchable.
        QCOMPARE(seen.status.power, std::optional<bool>(true));
        QVERIFY(seen.status.capabilities.value_or(QStringList()).contains(u"reset"_s));
        QCOMPARE(readings(seen.status).fansNotWell, 1);
        // Gone again a moment later, and held: one problem, not a new one every few seconds.
        system.insert(u"Status"_s, QJsonObject{{u"Health"_s, u"OK"_s}, {u"HealthRollup"_s, u"OK"_s}});
        redfish::Looked held = lookedAt(system);
        held.faultHeld = *seen.fault;
        seen = redfish::statusFrom(held);
        QCOMPARE(seen.status.state, u"error"_s);
        QCOMPARE(seen.status.errorMessage,
                 u"The server reported a critical fault (CPU0_TEMP, CPU1_FAN) a moment ago: it comes and goes"_s);
        QVERIFY(!seen.fault);
        // A critical fault with no sensor to name, and no more than three named.
        system.insert(u"Status"_s, QJsonObject{{u"HealthRollup"_s, u"Critical"_s}});
        QCOMPARE(redfish::statusFrom(lookedAt(system, QJsonObject())).status.errorMessage,
                 u"The server reports a critical fault"_s);
        QJsonObject many = itsThermal(61, u"Critical"_s);
        QJsonArray temperatures = many.value("Temperatures"_L1).toArray();
        for (int index = 1; index < 3; ++index) {
            QJsonObject one = temperatures.at(index).toObject();
            one.insert(u"Status"_s, QJsonObject{{u"Health"_s, u"Critical"_s}});
            temperatures.replace(index, one);
        }
        many.insert(u"Temperatures"_s, temperatures);
        QCOMPARE(redfish::statusFrom(lookedAt(system, many)).status.errorMessage,
                 u"The server reports a critical fault (CPU0_TEMP, CPU1_TEMP, DIMM_A_TEMP)"_s);
    }

    void itsHealthIsTakenAsAWholeBeforeItsOwnPart()
    {
        // (redfish.py:2083) HealthRollup is the health of the server and all that is in it.
        QJsonObject system = aComputer();
        system.insert(u"Status"_s, QJsonObject{{u"Health"_s, u"OK"_s}, {u"HealthRollup"_s, u"Critical"_s}});
        QCOMPARE(redfish::statusFrom(lookedAt(system)).status.state, u"error"_s);
        system.insert(u"Status"_s, QJsonObject{{u"Health"_s, u"Critical"_s}, {u"HealthRollup"_s, u"OK"_s}});
        QCOMPARE(redfish::statusFrom(lookedAt(system)).status.state, u"online"_s);
        QCOMPARE(readings(redfish::statusFrom(lookedAt(system)).status).health, u"OK"_s);
        // Where it says nothing as a whole, its own part.
        system.insert(u"Status"_s, QJsonObject{{u"Health"_s, u"Warning"_s}, {u"HealthRollup"_s, u""_s}});
        QCOMPARE(readings(redfish::statusFrom(lookedAt(system)).status).health, u"Warning"_s);
        system.insert(u"Status"_s, QJsonObject{{u"Health"_s, u"Critical"_s}});
        QCOMPARE(redfish::statusFrom(lookedAt(system)).status.state, u"error"_s);
        system.insert(u"Status"_s, u"Critical"_s);
        QCOMPARE(readings(redfish::statusFrom(lookedAt(system)).status).health, QString());
    }

    void aServerThatIsOffIsThereAndDark()
    {
        // (test_redfish.py:147) To the wall a server that is switched off is a computer without a picture.
        for (const QString& off : {u"Off"_s, u"PoweringOff"_s}) {
            const Status status = redfish::statusFrom(lookedAt(with(aComputer(), u"PowerState"_s, off))).status;
            QCOMPARE(status.state, u"no_signal"_s);
            QCOMPARE(status.power, std::optional<bool>(false));
            QCOMPARE(status.signal, std::optional<bool>(false));
            QVERIFY(!redfish::runs(with(aComputer(), u"PowerState"_s, off)));
        }
        for (const QString& on : {u"On"_s, u"PoweringOn"_s}) {
            const Status status = redfish::statusFrom(lookedAt(with(aComputer(), u"PowerState"_s, on))).status;
            QCOMPARE(status.state, u"online"_s);
            QCOMPARE(status.power, std::optional<bool>(true));
            QVERIFY(redfish::runs(with(aComputer(), u"PowerState"_s, on)));
        }
        // Not said: not known, and not taken for off.
        QJsonObject unsaid = aComputer();
        unsaid.remove(u"PowerState"_s);
        const Status status = redfish::statusFrom(lookedAt(unsaid)).status;
        QCOMPARE(status.state, u"online"_s);
        QCOMPARE(status.power, std::nullopt);
        QVERIFY(status.powerIsKnown);
        QVERIFY(!status.hasPicture);
        QVERIFY(!status.hasScreen);
        QCOMPARE(status.version, u"1.42.0"_s);
    }

    void theCardOfAServer()
    {
        const Status status = redfish::statusFrom(lookedAt(aComputer())).status;
        QCOMPARE(status.model, u"SRV-2200"_s);
        QCOMPARE(status.temperature, std::optional<double>(61)); // the processors', the hottest of them
        QVERIFY(status.server);
        QCOMPARE(readings(status).health, u"OK"_s);
        QCOMPARE(readings(status).locator, std::optional<bool>(false));
        QCOMPARE(readings(status).processor, u"Example Processor 4000"_s);
        QCOMPARE(readings(status).processors, 2);
        QCOMPARE(readings(status).memoryGib, std::optional<double>(256));
        // Two fans fitted, both running, one not well (the one that is not fitted is no fan).
        QCOMPARE(readings(status).fans, 2);
        QCOMPARE(readings(status).fansRunning, 2);
        QCOMPARE(readings(status).fansNotWell, 1);
        QCOMPARE(status.capabilities, std::optional<QStringList>(QStringList{u"power"_s, u"reset"_s, u"power_state"_s,
                                                                             u"reboot"_s, u"locator"_s}));
        // No light to switch: no locator.
        QJsonObject dark = aComputer();
        dark.remove(u"IndicatorLED"_s);
        const Status without = redfish::statusFrom(lookedAt(dark)).status;
        QCOMPARE(without.capabilities,
                 std::optional<QStringList>(QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"reboot"_s}));
        QCOMPARE(readings(without).locator, std::nullopt);
        // No processor's temperature: the hottest of the rest.
        QJsonObject thermal = itsThermal();
        QJsonArray temperatures = thermal.value("Temperatures"_L1).toArray();
        temperatures.removeFirst();
        temperatures.removeFirst();
        thermal.insert(u"Temperatures"_s, temperatures);
        QCOMPARE(redfish::statusFrom(lookedAt(aComputer(), thermal)).status.temperature, std::optional<double>(45));
        QCOMPARE(redfish::statusFrom(lookedAt(aComputer(), QJsonObject())).status.temperature, std::nullopt);
        // Numbers held to reason: processors one to 1024 and whole, memory up to a million GiB.
        for (const QJsonValue& count : {QJsonValue(0), QJsonValue(1025), QJsonValue(2.5), QJsonValue(u"2"_s)}) {
            QJsonObject system = aComputer();
            system.insert(u"ProcessorSummary"_s, QJsonObject{{u"Count"_s, count}});
            QCOMPARE(readings(redfish::statusFrom(lookedAt(system)).status).processors, 0);
        }
        QJsonObject huge = aComputer();
        huge.insert(u"MemorySummary"_s, QJsonObject{{u"TotalSystemMemoryGiB"_s, 2e6}});
        QCOMPARE(readings(redfish::statusFrom(lookedAt(huge)).status).memoryGib, std::nullopt);
        // A fan that says no reading is fitted and not running.
        QJsonObject quiet = itsThermal();
        QJsonArray fans = quiet.value("Fans"_L1).toArray();
        QJsonObject first = fans.at(0).toObject();
        first.remove(u"Reading"_s);
        fans.replace(0, first);
        quiet.insert(u"Fans"_s, fans);
        const Status stopped = redfish::statusFrom(lookedAt(aComputer(), quiet)).status;
        QCOMPARE(readings(stopped).fans, 2);
        QCOMPARE(readings(stopped).fansRunning, 1);
    }

    void whatTheLocatorLightIsAndHowItIsSwitched()
    {
        // (test_redfish.py:933) The two boards this was built from.
        const QJsonObject usual{
            {u"IndicatorLED@Redfish.AllowableValues"_s, QJsonArray{u"Lit"_s, u"Blinking"_s, u"Off"_s}}};
        const auto light = [](QJsonObject system, const QJsonValue& state) {
            system.insert(u"IndicatorLED"_s, state);
            return redfish::locator(system);
        };
        const auto change = [](const QString& to) { return QJsonObject{{u"IndicatorLED"_s, to}}; };
        // Lit, not blinking: on a real board a light told to blink was out again by itself within twenty seconds.
        std::optional<redfish::Light> seen = light(usual, u"Off"_s);
        QVERIFY(seen);
        QVERIFY(!lit(seen).on);
        QCOMPARE(lit(seen).lightUp, change(u"Lit"_s));
        QCOMPARE(lit(seen).putOut, change(u"Off"_s));
        QVERIFY(lit(light(usual, u"Lit"_s)).on);
        QVERIFY(lit(light(usual, u"Blinking"_s)).on);
        // It does not say what it takes.
        seen = light(QJsonObject(), u"Off"_s);
        QVERIFY(!lit(seen).on);
        QCOMPARE(lit(seen).lightUp, change(u"Lit"_s));
        QCOMPARE(lit(seen).putOut, change(u"Off"_s));
        QCOMPARE(
            lit(light(QJsonObject{{u"IndicatorLED@Redfish.AllowableValues"_s, QJsonArray{u"Blinking"_s, u"Off"_s}}},
                      u"Off"_s))
                .lightUp,
            change(u"Blinking"_s));
        // A light that cannot be switched off: lit is its state of rest, and only blinking calls for attention.
        const QJsonObject resting{{u"IndicatorLED@Redfish.AllowableValues"_s, QJsonArray{u"Lit"_s, u"Blinking"_s}}};
        seen = light(resting, u"Lit"_s);
        QVERIFY(!lit(seen).on);
        QCOMPARE(lit(seen).lightUp, change(u"Blinking"_s));
        QCOMPARE(lit(seen).putOut, change(u"Lit"_s));
        QVERIFY(lit(light(resting, u"Blinking"_s)).on);
        // One that cannot be told to blink.
        seen =
            light(QJsonObject{{u"IndicatorLED@Redfish.AllowableValues"_s, QJsonArray{u"Lit"_s, u"Off"_s}}}, u"Lit"_s);
        QVERIFY(lit(seen).on);
        QCOMPARE(lit(seen).lightUp, change(u"Lit"_s));
        QCOMPARE(lit(seen).putOut, change(u"Off"_s));
        // The newer, plain way of saying it wins where a controller has both.
        QJsonObject both = usual;
        both.insert(u"IndicatorLED"_s, u"Lit"_s);
        both.insert(u"LocationIndicatorActive"_s, false);
        seen = redfish::locator(both);
        QVERIFY(!lit(seen).on);
        QCOMPARE(lit(seen).lightUp, (QJsonObject{{u"LocationIndicatorActive"_s, true}}));
        QCOMPARE(lit(seen).putOut, (QJsonObject{{u"LocationIndicatorActive"_s, false}}));
        // None.
        QJsonObject onlyOff = usual;
        onlyOff.insert(u"IndicatorLED@Redfish.AllowableValues"_s, QJsonArray{u"Off"_s});
        for (const auto& none : {redfish::locator(QJsonObject()), light(QJsonObject(), QJsonValue()),
                                 light(QJsonObject(), u"Unknown"_s), light(QJsonObject(), 1), light(onlyOff, u"Off"_s),
                                 redfish::locator(QJsonObject{{u"LocationIndicatorActive"_s, u"yes"_s}})}) {
            QVERIFY(!none);
        }
    }

    void aServerIsKnownByItsOwnNumber()
    {
        // (test_twins.py:139) Compared, never shown: kept as a digest.
        const QString number = u"4c4c4544-0042-3510-8054-b9c04f4a3532"_s;
        const QString mark = redfish::serverNumber(QJsonObject{{u"UUID"_s, number}});
        QCOMPARE(mark, u"server:"_s + digestOf(number));
        QCOMPARE(redfish::serverNumber(QJsonObject{{u"UUID"_s, number.toUpper()}}), mark);
        QVERIFY(!mark.contains(number));
        QVERIFY(!mark.contains(u"4c4c4544"_s));
        // Boards whose maker never set one answer all zeros, or all F: that is no number, and many boards
        // have it. Nor is what has fewer than four different digits in it, or what is no UUID.
        for (const QJsonValue& none :
             {QJsonValue(u"00000000-0000-0000-0000-000000000000"_s),
              QJsonValue(u"FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"_s),
              QJsonValue(u"12121212-1212-1212-1212-121212121212"_s),
              QJsonValue(u"10201010-2010-1020-1010-201010201010"_s), QJsonValue(u"not a number at all"_s),
              QJsonValue(u""_s), QJsonValue(), QJsonValue(7), QJsonValue(u"4c4c45440042351080 54b9c04f4a3532"_s),
              QJsonValue(u"{4c4c4544-0042-3510-8054-b9c04f4a3532}"_s),
              QJsonValue(u"4c4c4544-0042-3510-8054-b9c04f4a3532\n"_s),
              QJsonValue(u"4c4c4544004235108054b9c04f4a3532"_s)}) {
            QVERIFY2(redfish::serverNumber(QJsonObject{{u"UUID"_s, none}}).isEmpty(),
                     qPrintable(none.toVariant().toString()));
            QVERIFY(redfish::controllerNumber(QJsonObject{{u"UUID"_s, none}}).isEmpty());
        }
        // Different digits are counted once in small letters: "aA" is one digit, and this is none.
        for (const QString& one :
             {u"ffffffff-FFFF-ffff-FFFF-ffffffffffff"_s, u"aAbBaAbB-aAbB-aAbB-aAbB-aAbBaAbBaAbB"_s}) {
            QVERIFY2(redfish::serverNumber(QJsonObject{{u"UUID"_s, one}}).isEmpty(), qPrintable(one));
        }
        // Four different digits are a number.
        QCOMPARE(redfish::serverNumber(QJsonObject{{u"UUID"_s, u"10203010-2030-1020-3010-203010203010"_s}}),
                 u"server:"_s + digestOf(u"10203010-2030-1020-3010-203010203010"_s));
        // The controller's: the digest alone, of its service root's.
        QCOMPARE(redfish::controllerNumber(QJsonObject{{u"UUID"_s, number}}), digestOf(number));
        QVERIFY(redfish::controllerNumber(QJsonValue(u"x"_s)).isEmpty());
        // What its manager names: the service it hosts, and itself; each once, and none that is none.
        const QString itsService = u"0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0"_s;
        QCOMPARE(redfish::managerNumbers(QJsonObject{{u"ServiceEntryPointUUID"_s, number}, {u"UUID"_s, number}}),
                 QStringList{digestOf(number)});
        QCOMPARE(redfish::managerNumbers(QJsonObject{{u"ServiceEntryPointUUID"_s, itsService}, {u"UUID"_s, number}}),
                 (QStringList{digestOf(itsService), digestOf(number)}));
        QCOMPARE(redfish::managerNumbers(QJsonObject{{u"UUID"_s, u"00000000-0000-0000-0000-000000000000"_s},
                                                     {u"ServiceEntryPointUUID"_s, 7}}),
                 QStringList());
        // As remembered: the computer's mark, then the controller's.
        const QString controller = digestOf(u"0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0"_s);
        redfish::Marks marks = redfish::marksOf(mark + controller);
        QCOMPARE(marks.server, mark);
        QCOMPARE(marks.controller, controller);
        marks = redfish::marksOf(QString(redfish::saidNone) + controller);
        QCOMPARE(marks.server, QString(redfish::saidNone));
        QCOMPARE(marks.controller, controller);
        marks = redfish::marksOf(mark);
        QCOMPARE(marks.server, mark);
        QVERIFY(marks.controller.isEmpty());
        // What is of no such shape is taken whole for the computer's: no number a server gives equals it.
        for (const QString& other : {u"cert:abc"_s, u"jetkvm:"_s + digestOf(number), mark + controller + u"0"_s}) {
            marks = redfish::marksOf(other);
            QCOMPARE(marks.server, other);
            QVERIFY(marks.controller.isEmpty());
        }
        QCOMPARE(redfish::marksOf(QString()).server, QString());
    }

    void theSensorsOfTheOlderShape()
    {
        // (fake_redfish.py, Thermal) A temperature that is not fitted has no reading; a fan that is not
        // fitted is no fan.
        const QList<redfish::Temperature> temperatures = redfish::temperaturesOf(itsThermal());
        QCOMPARE(temperatures,
                 (QList<redfish::Temperature>{
                     {u"CPU0_TEMP"_s, 61, true}, {u"CPU1_TEMP"_s, 55, true}, {u"DIMM_A_TEMP"_s, 45, true}}));
        const QList<redfish::Fan> fans = redfish::fansOf(itsThermal());
        QCOMPARE(fans, (QList<redfish::Fan>{{u"CPU0_FAN"_s, 1650, false, true}, {u"CPU1_FAN"_s, 450, false, false}}));
        // Held to reason, and their names to a line.
        const QJsonObject odd = parsed(
            R"({ )"
            R"("Temperatures": [{"Name": "HOT", "ReadingCelsius": 301}, {"Name": "COLD", "ReadingCelsius": -101}, )"
            R"({"Name": "A\nB", "ReadingCelsius": 20, "Status": {"Health": "Warning"}}, "not one", )"
            R"({"ReadingCelsius": 30, "Status": {"Health": null}}], )"
            R"("Fans": [{"Name": "PCT", "Reading": 40, "ReadingUnits": "Percent", "Status": {"State": "Enabled"}}, )"
            R"({"Name": "FAST", "Reading": 2000000, "Status": {"State": "Enabled"}}, )"
            R"({"Name": "OFF", "Reading": 900, "Status": {"State": "Disabled"}}, )"
            R"({"Name": "NONE", "Reading": 900}, 7] )"
            R"(} )");
        QCOMPARE(redfish::temperaturesOf(odd),
                 (QList<redfish::Temperature>{{u"A B"_s, 20, false}, {QString(), 30, true}}));
        QCOMPARE(redfish::fansOf(odd),
                 (QList<redfish::Fan>{{u"PCT"_s, 40, true, true}, {u"FAST"_s, std::nullopt, false, true}}));
    }

    void theSensorsOfTheNewerShape()
    {
        // (fake_redfish.py, ThermalMetrics) As bmcweb says a reading: its value, and where the sensor itself
        // is; no name. The first carries its health as the older shape does.
        const QJsonObject metrics = parsed(
            R"({"Id": "ThermalMetrics", "TemperatureReadingsCelsius": [ )"
            R"({"Reading": 61, "DataSourceUri": "/redfish/v1/Chassis/chassis/Sensors/temperature_CPU0_TEMP", "Status": {"Health": "Critical"}}, )"
            R"({"Reading": 55, "DataSourceUri": "/redfish/v1/Chassis/chassis/Sensors/temperature_CPU1_TEMP/"}, )"
            R"({"Reading": 45, "DataSourceUri": "https://elsewhere.example/redfish/v1/Chassis/chassis/Sensors/x", "DeviceName": "DIMM A"}, )"
            R"({"Reading": 40, "DataSourceUri": "https://elsewhere.example/redfish/v1/Sensors/y"}, )"
            R"({"Reading": null, "DataSourceUri": "/redfish/v1/Chassis/chassis/Sensors/temperature_M2_TEMP"}]} )");
        QCOMPARE(redfish::readingsOf(metrics), (QList<redfish::Temperature>{{u"temperature_CPU0_TEMP"_s, 61, false},
                                                                            {u"temperature_CPU1_TEMP"_s, 55, true},
                                                                            {u"DIMM A"_s, 45, true},
                                                                            {QString(), 40, true}}));
        // (fake_redfish.py, FANS) A fan says its speed as a share of its most, as revolutions, or both.
        QCOMPARE(
            redfish::fanOf(parsed(
                R"({"Name": "CPU0_FAN", "SpeedPercent": {"SpeedRPM": 1650}, "Status": {"Health": "OK", "State": "Enabled"}})")),
            std::optional<redfish::Fan>(redfish::Fan{u"CPU0_FAN"_s, 1650, false, true}));
        QCOMPARE(
            redfish::fanOf(parsed(
                R"({"Name": "CPU1_FAN", "SpeedPercent": {"Reading": 18}, "Status": {"Health": "Warning", "State": "Enabled"}})")),
            std::optional<redfish::Fan>(redfish::Fan{u"CPU1_FAN"_s, 18, true, false}));
        QCOMPARE(redfish::fanOf(parsed(
                     R"({"Name": "SYS_FAN5", "SpeedPercent": {"SpeedRPM": null}, "Status": {"State": "Absent"}})")),
                 std::nullopt);
    }

    void aFanSaysItsSpeedInOneOfThreeWays()
    {
        // (redfish.py:480-500) Revolutions where they are a number, which is what the older shape gives for
        // the same fan; else the share where that is one; else no reading, in revolutions where those were
        // said at all, and otherwise as the share the resource is by its name.
        const auto fan = [](const char* speed) {
            return redfish::fanOf(
                parsed(R"({"Name": "F", "Status": {"State": "Enabled"}, "SpeedPercent": )"_ba + speed + "}"));
        };
        QCOMPARE(fan(R"({"SpeedRPM": 1200, "Reading": 30})"),
                 std::optional<redfish::Fan>(redfish::Fan{u"F"_s, 1200, false, true}));
        QCOMPARE(fan(R"({"SpeedRPM": "fast", "Reading": 30})"),
                 std::optional<redfish::Fan>(redfish::Fan{u"F"_s, 30, true, true}));
        QCOMPARE(fan(R"({"Reading": 101})"),
                 std::optional<redfish::Fan>(redfish::Fan{u"F"_s, std::nullopt, true, true}));
        QCOMPARE(fan(R"({"SpeedRPM": null})"),
                 std::optional<redfish::Fan>(redfish::Fan{u"F"_s, std::nullopt, false, true}));
        QCOMPARE(fan(R"({"SpeedRPM": -1, "Reading": null})"),
                 std::optional<redfish::Fan>(redfish::Fan{u"F"_s, std::nullopt, false, true}));
        QCOMPARE(fan("{}"), std::optional<redfish::Fan>(redfish::Fan{u"F"_s, std::nullopt, true, true}));
        QCOMPARE(fan("7"), std::nullopt);
        QCOMPARE(redfish::fanOf(QJsonValue(u"a fan"_s)), std::nullopt);
        QCOMPARE(redfish::fanOf(parsed(R"({"Name": "F", "Status": {"State": "Enabled"}})")), std::nullopt);
    }

    void noMoreThanFortySensorsAreListed()
    {
        // Of the first forty entries: a controller that lists thousands is not gone through.
        QJsonArray temperatures;
        QJsonArray fans;
        QJsonArray readings;
        for (int index = 0; index < redfish::mostSensors + 5; ++index) {
            const QString name = u"S%1"_s.arg(index);
            temperatures.append(QJsonObject{{u"Name"_s, name}, {u"ReadingCelsius"_s, 30}});
            fans.append(QJsonObject{
                {u"Name"_s, name}, {u"Reading"_s, 900}, {u"Status"_s, QJsonObject{{u"State"_s, u"Enabled"_s}}}});
            readings.append(QJsonObject{{u"DeviceName"_s, name}, {u"Reading"_s, 30}});
        }
        QCOMPARE(redfish::temperaturesOf(QJsonObject{{u"Temperatures"_s, temperatures}}).size(), 40);
        QCOMPARE(redfish::temperaturesOf(QJsonObject{{u"Temperatures"_s, temperatures}}).last().name, u"S39"_s);
        QCOMPARE(redfish::fansOf(QJsonObject{{u"Fans"_s, fans}}).size(), 40);
        QCOMPARE(redfish::readingsOf(QJsonObject{{u"TemperatureReadingsCelsius"_s, readings}}).size(), 40);
        // (Of the first forty: one without a reading among them is not made up for by the forty-first, of
        // the temperatures and the readings as of the fans.)
        QJsonObject unread = temperatures.at(0).toObject();
        unread.remove(u"ReadingCelsius"_s);
        temperatures.replace(0, unread);
        QCOMPARE(redfish::temperaturesOf(QJsonObject{{u"Temperatures"_s, temperatures}}).size(), 39);
        unread = readings.at(0).toObject();
        unread.remove(u"Reading"_s);
        readings.replace(0, unread);
        QCOMPARE(redfish::readingsOf(QJsonObject{{u"TemperatureReadingsCelsius"_s, readings}}).size(), 39);
        // (Of the first forty: one that is not fitted among them is not made up for by the forty-first.)
        QJsonObject absent = fans.at(0).toObject();
        absent.insert(u"Status"_s, QJsonObject{{u"State"_s, u"Absent"_s}});
        fans.replace(0, absent);
        QCOMPARE(redfish::fansOf(QJsonObject{{u"Fans"_s, fans}}).size(), 39);
    }

    void whereAListBelongsNothingElseIsGoneThrough()
    {
        // (test_what_a_controller_sent_of_another_kind.py:55) A controller that says so has no temperatures
        // to show: the server is there all the same.
        for (const QJsonValue& kind : odd()) {
            QVERIFY(redfish::temperaturesOf(QJsonObject{{u"Temperatures"_s, kind}}).isEmpty());
            QVERIFY(redfish::fansOf(QJsonObject{{u"Fans"_s, kind}}).isEmpty());
            QVERIFY(redfish::readingsOf(QJsonObject{{u"TemperatureReadingsCelsius"_s, kind}}).isEmpty());
            const redfish::Looked looked =
                lookedAt(aComputer(), QJsonObject{{u"Temperatures"_s, kind}, {u"Fans"_s, kind}});
            QCOMPARE(redfish::statusFrom(looked).status.state, u"online"_s);
        }
        // (:84) A controller that does not say which kinds of reset it takes, as a list, is asked for the usual
        // one, as always.
        for (const QJsonValue& kind : {QJsonValue(5), QJsonValue(true), QJsonValue(1.5)}) {
            QJsonObject system = aComputer();
            QJsonObject actions = system.value("Actions"_L1).toObject();
            QJsonObject reset = actions.value("#ComputerSystem.Reset"_L1).toObject();
            reset.insert(u"ResetType@Redfish.AllowableValues"_s, kind);
            actions.insert(u"#ComputerSystem.Reset"_s, reset);
            system.insert(u"Actions"_s, actions);
            const redfish::Offered offered = redfish::offeredKinds(system, "#ComputerSystem.Reset"_L1);
            QVERIFY(!offered.kinds);
            QCOMPARE(offered.described, u"/redfish/v1/Systems/Self/ResetActionInfo"_s);
            QVERIFY(redfish::kindsIn(QJsonObject{{u"Parameters"_s, kind}}).isEmpty());
            QVERIFY(
                redfish::kindsIn(QJsonObject{{u"Parameters"_s, QJsonArray{QJsonObject{{u"Name"_s, u"ResetType"_s},
                                                                                      {u"AllowableValues"_s, kind}}}}})
                    .isEmpty());
            QCOMPARE(redfish::kindToSend(redfish::wanted(u"atx_reset"_s, true), {}), u"ForceRestart"_s);
        }
    }

    void theKindsOfResetAControllerOffers()
    {
        // Said on the action itself: those, and its description is not read.
        QJsonObject manager =
            parsed(R"({"Actions": {"#Manager.Reset": {"target": "/redfish/v1/Managers/Self/Actions/Manager.Reset", )"
                   R"("ResetType@Redfish.AllowableValues": ["ForceRestart", 7, "GracefulRestart"], )"
                   R"("@Redfish.ActionInfo": "/redfish/v1/Managers/Self/ResetActionInfo"}}} )");
        redfish::Offered offered = redfish::offeredKinds(manager, "#Manager.Reset"_L1);
        QCOMPARE(offered.target, u"/redfish/v1/Managers/Self/Actions/Manager.Reset"_s);
        QCOMPARE(offered.kinds, std::optional<QStringList>({u"ForceRestart"_s, u"GracefulRestart"_s}));
        QVERIFY(offered.described.isEmpty());
        // Said in its description (fake_redfish.py, ResetActionInfo).
        offered = redfish::offeredKinds(aComputer(), "#ComputerSystem.Reset"_L1);
        QCOMPARE(offered.target, u"/redfish/v1/Systems/Self/Actions/ComputerSystem.Reset"_s);
        QVERIFY(!offered.kinds);
        QCOMPARE(offered.described, u"/redfish/v1/Systems/Self/ResetActionInfo"_s);
        QCOMPARE(
            redfish::kindsIn(parsed(
                R"({"Parameters": [{"Name": "Other", "AllowableValues": ["X"]}, )"
                R"({"Name": "ResetType", "Required": true, "AllowableValues": ["On", "ForceOff", "GracefulShutdown", "ForceRestart"]}]} )")),
            (QStringList{u"On"_s, u"ForceOff"_s, u"GracefulShutdown"_s, u"ForceRestart"_s}));
        // A place that is not one of its own is none: neither sent to nor read.
        manager = parsed(R"({"Actions": {"#Manager.Reset": {"target": "https://elsewhere.example/redfish/v1/x", )"
                         R"("@Redfish.ActionInfo": "/redfish/v1/../api"}}} )");
        offered = redfish::offeredKinds(manager, "#Manager.Reset"_L1);
        QVERIFY(offered.target.isEmpty());
        QVERIFY(offered.described.isEmpty());
        QVERIFY(!offered.kinds);
        // An action it does not describe at all.
        offered = redfish::offeredKinds(QJsonObject{{u"Actions"_s, 5}}, "#ComputerSystem.Reset"_L1);
        QVERIFY(offered.target.isEmpty() && offered.described.isEmpty() && !offered.kinds);
    }

    void whatAButtonAsksAControllerFor()
    {
        // (redfish.py:2216-2229) By what is wanted, not by which button.
        QCOMPARE(redfish::wanted(u"power_on"_s, false), QStringList{u"On"_s});
        QCOMPARE(redfish::wanted(u"power_off"_s, true), QStringList{u"GracefulShutdown"_s});
        QCOMPARE(redfish::wanted(u"atx_power"_s, true), QStringList{u"GracefulShutdown"_s});
        QCOMPARE(redfish::wanted(u"atx_power"_s, false), QStringList{u"On"_s});
        QCOMPARE(redfish::wanted(u"atx_power_long"_s, true), QStringList{u"ForceOff"_s});
        QCOMPARE(redfish::wanted(u"atx_reset"_s, true), (QStringList{u"ForceRestart"_s, u"PowerCycle"_s}));
        QCOMPARE(redfish::wanted(u"reboot"_s, true), (QStringList{u"ForceRestart"_s, u"GracefulRestart"_s}));
        for (const QString& other : {u"jiggler_on"_s, u"locate_on"_s, u"bios_setup"_s, u"wake"_s, u""_s}) {
            QVERIFY(redfish::wanted(other, true).isEmpty());
        }
        // The first it offers, in the order they are tried; the first where it says nothing; none of them.
        const QStringList offered{u"On"_s, u"ForceOff"_s, u"GracefulShutdown"_s, u"ForceRestart"_s};
        QCOMPARE(redfish::kindToSend(redfish::wanted(u"atx_reset"_s, true), offered), u"ForceRestart"_s);
        QCOMPARE(redfish::kindToSend(redfish::wanted(u"atx_reset"_s, true), {u"On"_s, u"PowerCycle"_s}),
                 u"PowerCycle"_s);
        QCOMPARE(redfish::kindToSend(redfish::wanted(u"atx_reset"_s, true), {u"On"_s, u"ForceOff"_s}), QString());
        QCOMPARE(redfish::kindToSend(redfish::wanted(u"power_on"_s, false), {}), u"On"_s);
        QCOMPARE(redfish::kindToSend({}, offered), QString());
        // Whether it runs: what does not say Off runs (a server whose power is not said is not taken for off).
        QJsonObject unsaid = aComputer();
        unsaid.remove(u"PowerState"_s);
        QVERIFY(redfish::runs(unsaid));
    }

    void theControllersVerdictOnACommand()
    {
        // (redfish.py:2612-2639) Only a 4xx is a no, in its own words where it has any.
        for (const int done : {200, 202, 204}) {
            QVERIFY(redfish::verdictOn(done, {}));
        }
        const QByteArray extended =
            R"({"error": {"message": "General", "@Message.ExtendedInfo": [{"Message": "The value Off for the property ResetType is not in the list of acceptable values."}]}})";
        refusedAs(redfish::verdictOn(400, extended), "action_failed",
                  "The value Off for the property ResetType is not in the list of acceptable values.");
        refusedAs(redfish::verdictOn(409, R"({"error": {"message": "Busy\nnow"}})"), "action_failed", "Busy now");
        refusedAs(redfish::verdictOn(400, R"({"error": {"message": "General", "@Message.ExtendedInfo": []}})"),
                  "action_failed", "General");
        refusedAs(redfish::verdictOn(400, R"({"error": {"message": "General", "@Message.ExtendedInfo": "odd"}})"),
                  "action_failed", "The controller refused (HTTP 400)");
        refusedAs(redfish::verdictOn(403, "not json"), "action_failed", "The controller refused (HTTP 403)");
        refusedAs(redfish::verdictOn(499, {}), "action_failed", "The controller refused (HTTP 499)");
        // (No raw string with a quote by itself: moc reads a raw string as an ordinary one.)
        const QByteArray longWords = "{\"error\": {\"message\": \"" + QByteArray(300, 'w') + "\"}}";
        QCOMPARE(redfish::verdictOn(400, longWords).error().message.size(), std::size_t{200});
        // An error of its own, or an answer a command does not have: neither yes nor no.
        refusedAs(redfish::verdictOn(500, R"({"error": {"message": "Internal error"}})"), "timeout",
                  "The controller answered with an error of its own (Internal error): it may or may not have been "
                  "done. Look at the server before pressing again.");
        refusedAs(redfish::verdictOn(503, {}), "timeout",
                  "The controller answered with an error of its own (HTTP 503): it may or may not have been done. "
                  "Look at the server before pressing again.");
        refusedAs(redfish::verdictOn(201, {}), "timeout",
                  "The controller answered with an answer a command does not have (HTTP 201): it may or may not "
                  "have been done. Look at the server before pressing again.");
        for (const int oddOne : {100, 203, 301, 307, 600}) {
            QCOMPARE(redfish::verdictOn(oddOne, {}).error().code, std::string("timeout"));
        }
    }

    void aSessionsPassIsOnlyOneWhole()
    {
        // (kvm.py:1149) It goes back in a header with every question: only what is safe there, all of it.
        for (const QByteArray& pass :
             {"0123456789abcdef0123456789abcdef"_ba, "AbC+/9xYz="_ba, "a.b_c~d-e"_ba, QByteArray(512, 'a')}) {
            QCOMPARE(redfish::token(pass), pass);
        }
        for (const QByteArray& none :
             {QByteArray(), " "_ba, QByteArray(513, 'a'), "abc\n"_ba, "abc\r\n"_ba, "\nabc"_ba, " abc"_ba, "a b"_ba,
              "a;b"_ba, "a,b"_ba, "a\"b"_ba, QByteArray("a\0b", 3), "\xc3\xa9t\xc3\xa9"_ba}) {
            QVERIFY2(redfish::token(none).isEmpty(), none.toHex().constData());
        }
    }

    void theMarkAControllerPutsOnAState()
    {
        // (redfish.py:241, 802-803) Its header first, as sent; else the resource's own; the first that is a mark.
        const QJsonObject own{{u"@odata.etag"_s, u"\"1791401332\""_s}};
        QCOMPARE(redfish::markOf(R"(W/"1791401332")"_ba, own), R"(W/"1791401332")"_ba);
        QCOMPARE(redfish::markOf({}, own), R"("1791401332")"_ba);
        QCOMPARE(redfish::markOf("no quotes"_ba, own), R"("1791401332")"_ba);
        QCOMPARE(redfish::markOf(R"("")"_ba, {}), R"("")"_ba);
        QCOMPARE(redfish::markOf(R"("MiXeD")"_ba, {}), R"("MiXeD")"_ba);
        const QByteArray longest = "\"" + QByteArray(128, 'a') + "\"";
        QCOMPARE(redfish::markOf(longest, {}), longest);
        for (const QByteArray& none : {"\"" + QByteArray(129, 'a') + "\"", R"("a b")"_ba, "\"a\"b\""_ba, R"(w/"a")"_ba,
                                       R"("a")"_ba + "\n", "\"\xc3\xa9\""_ba, "*"_ba}) {
            QVERIFY2(redfish::markOf(none, {}).isEmpty(), none.constData());
        }
        QVERIFY(redfish::markOf({}, QJsonObject{{u"@odata.etag"_s, 5}}).isEmpty());
    }

    void onlyADisabledInSoManyWordsIsAnOverrideOff()
    {
        // (Review 4's MEDIUM 2, the web's `_cleared_shown`.) A read that shows no override at all (no Boot, a null, an
        // empty one, no text) says nothing of it: not known, and never "off".
        const auto with = [](const QJsonValue& boot) { return QJsonObject{{u"Boot"_s, boot}}; };
        const auto enabled = [](const QJsonValue& said) {
            return QJsonObject{{u"BootSourceOverrideEnabled"_s, said}, {u"BootSourceOverrideTarget"_s, u"None"_s}};
        };
        QCOMPARE(redfish::overrideOff(with(enabled(u"Disabled"_s))), std::optional<bool>(true));
        QCOMPARE(redfish::overrideOff(with(enabled(u"DISABLED"_s))), std::optional<bool>(true));
        QCOMPARE(redfish::overrideOff(with(enabled(u"Once"_s))), std::optional<bool>(false));
        QCOMPARE(redfish::overrideOff(with(enabled(u"Continuous"_s))), std::optional<bool>(false));
        QCOMPARE(redfish::overrideOff(QJsonObject{}), std::optional<bool>());
        QCOMPARE(redfish::overrideOff(with(QJsonValue())), std::optional<bool>());
        QCOMPARE(redfish::overrideOff(with(QJsonObject{})), std::optional<bool>());
        QCOMPARE(redfish::overrideOff(with(enabled(QJsonValue()))), std::optional<bool>());
        QCOMPARE(redfish::overrideOff(with(enabled(u""_s))), std::optional<bool>());
        QCOMPARE(redfish::overrideOff(with(enabled(QJsonArray{}))), std::optional<bool>());
    }

    void anErrorSaidWithTwoHundredIsNoResource()
    {
        // (redfish.py:788-801) Some controllers do, at one of their moments, as the standard's record or as a
        // bare text.
        const QString path = u"/redfish/v1/Systems/Self"_s;
        const auto code = [](const auto& result) { return result ? std::string("ok") : result.error().code; };
        const auto said = [](const auto& result) { return result ? std::string() : result.error().message; };
        auto result = redfish::resourceFrom(path, 200, R"({"error": {"message": "Service Unavailable"}})");
        QCOMPARE(code(result), std::string("bad_response"));
        QCOMPARE(said(result), std::string("/redfish/v1/Systems/Self answered an error: Service Unavailable"));
        result = redfish::resourceFrom(path, 200, R"({"error": "Service Unavailable"})");
        QCOMPARE(said(result), std::string("/redfish/v1/Systems/Self answered an error: Service Unavailable"));
        result = redfish::resourceFrom(path, 200, R"({"error": {}})");
        QCOMPARE(said(result), std::string("/redfish/v1/Systems/Self answered an error: without a word"));
        // A resource that has its own address in it is one, whatever else it has.
        result = redfish::resourceFrom(path, 200, R"({"@odata.id": "/redfish/v1/Systems/Self", "error": 1})");
        QCOMPARE(code(result), std::string("ok"));
        result = redfish::resourceFrom(path, 200, written(aComputer()));
        QCOMPARE(result.value(), aComputer());
        result = redfish::resourceFrom(path, 503, written(aComputer()));
        QCOMPARE(said(result), std::string("/redfish/v1/Systems/Self failed (HTTP 503)"));
        for (const QByteArray& none : {"[1, 2]"_ba, "not json"_ba, ""_ba, "7"_ba}) {
            result = redfish::resourceFrom(path, 200, none);
            QCOMPARE(said(result), std::string("/redfish/v1/Systems/Self returned unexpected data"));
        }
    }

private:
    static void refusedAs(const qkvm::base::Result<void>& result, const char* code, const char* message)
    {
        QVERIFY(!result);
        QCOMPARE(result.error().code, std::string(code));
        QCOMPARE(result.error().message, std::string(message));
    }
};

QTEST_GUILESS_MAIN(TestRedfishAnswers)
#include "test_redfish_answers.moc"
