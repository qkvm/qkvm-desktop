#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRandomGenerator>
#include <QString>
#include <QtEndian>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <utility>
#include <vector>

#include "../../bmc/fuzz/bmc_seeds.h"
#include "../../rfb/tests/rfb_by_hand.h"
#include "../../rfb/tests/rfb_sweep_streams.h"
#include "../tests/rfb_screen_sweep_inputs.h"

// The fuzz targets' first inputs (the gate's fuzz step, tools/fuzz.ps1, by the build's fuzz_seeds): made here at build
// time, so that no seed is a file in the sources. Where a reader has an exhaustive test, its seeds are that test's own
// inputs (the RFB reader's streams, the JPEG walk's corpus, a real JPEG through the reader; bmc's frames and console
// streams); for the rest, the shapes their tests and real devices answer in, and the walls as files of the tests.
// Beside them, each target's one small seed: the barest input of its shape, that its reader takes and reads no further
// than its first step, from which tools/fuzz.ps1 -Canary -Smallest shows how far a target gets with nothing to go on.
//
//     qkvm-fuzz-seeds <folder> <smallest>     (a folder of each target's seeds in each, by the name its target says:
//                                              SEEDS)
namespace rfbt = qkvm::rfb::tests;
namespace coret = qkvm::core::tests;

namespace {

class Seeds {
public:
    explicit Seeds(QString root) : m_root(std::move(root)) {}

    bool put(const QString& target, const QByteArray& seed)
    {
        const QString folder = m_root + u'/' + target;
        if (!QDir().mkpath(folder)) {
            return fail(folder);
        }
        QFile file(folder + QStringLiteral("/seed-%1").arg(m_count[target]++, 3, 10, QChar(u'0')));
        if (!file.open(QIODevice::WriteOnly) || file.write(seed) != seed.size()) {
            return fail(file.fileName());
        }
        return true;
    }
    bool put(const QString& target, const rfbt::Bytes& seed) { return put(target, asQt(seed)); }
    // A seed of a target whose input begins with a byte of its own.
    bool put(const QString& target, char first, const QByteArray& seed) { return put(target, first + seed); }
    bool ok() const { return m_ok; }
    int made() const
    {
        int all = 0;
        for (const int one : m_count) {
            all += one;
        }
        return all;
    }

    static QByteArray asQt(const rfbt::Bytes& bytes)
    {
        return {reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
    }

private:
    bool fail(const QString& where)
    {
        std::fprintf(stderr, "could not write %s\n", qPrintable(where));
        m_ok = false;
        return false;
    }

    QString m_root;
    QHash<QString, int> m_count;
    bool m_ok = true;
};

// --- the RFB reader, and a VNC server's session ----------------------------------------------------------------------

void rfbSeeds(Seeds& seeds)
{
    for (const rfbt::Bytes& stream : rfbt::sweepStreams()) {
        seeds.put(QStringLiteral("rfb"), stream);
    }
    seeds.put(QStringLiteral("rfb"), coret::realJpegStream(coret::realJpeg16()));
    // A real JPEG of eight by eight in a corner of a screen of 256 by 256: a Tight rectangle of a part of it, as
    // servers send most.
    QImage eight(8, 8, QImage::Format_RGB888);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            eight.setPixel(x, y, qRgb(x * 32, y * 32, 128));
        }
    }
    const QByteArray small = coret::jpegOf(eight, 80);
    seeds.put(QStringLiteral("rfb"),
              rfbt::join({rfbt::greeting(), rfbt::securityTypes({1}), rfbt::word(0), rfbt::serverInit(256, 256, "s"),
                          rfbt::update({rfbt::tightJpeg(0, 0, 8, 8, rfbt::Bytes(small.begin(), small.end()))})}));
    // Intel AMT's greeting, which means 3.8; and a server that refuses, in its own words.
    seeds.put(QStringLiteral("rfb"),
              rfbt::join({rfbt::greeting("RFB 004.000\n"), rfbt::securityTypes({1}), rfbt::word(0),
                          rfbt::serverInit(16, 16, "amt"), rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(4, 4))})}));
    seeds.put(QStringLiteral("rfb"), rfbt::join({rfbt::greeting(), rfbt::securityTypes({}), rfbt::reason("Too many")}));
    // Older versions: 3.3 names the way of signing in as a word, 3.7 lists them and says no result for None.
    seeds.put(QStringLiteral("rfb"),
              rfbt::join({rfbt::greeting("RFB 003.003\n"), rfbt::word(1), rfbt::serverInit(16, 16, "three"),
                          rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(4, 4))})}));
    seeds.put(QStringLiteral("rfb"),
              rfbt::join({rfbt::greeting("RFB 003.007\n"), rfbt::securityTypes({1}), rfbt::serverInit(16, 16, "seven"),
                          rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(4, 4))})}));
}

void vncSeeds(Seeds& seeds)
{
    const rfbt::Bytes challenge = rfbt::text("0123456789abcdef");
    const auto signedIn = [&challenge](std::string_view version, std::string_view name) {
        return rfbt::join({rfbt::greeting(version), rfbt::securityTypes({2}), challenge, rfbt::word(0),
                           rfbt::serverInit(16, 16, name)});
    };
    const rfbt::Bytes raw = rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(8, 8))});
    rfbt::Deflater stream;
    const rfbt::Bytes zrle =
        rfbt::update({rfbt::zrleRect(0, 0, rfbt::quarters(8, 8), rfbt::ZrleTiles::PaletteRuns, stream)});
    for (const char how : {'\x00', '\x40', '\x7F', '\xC0'}) {
        seeds.put(QStringLiteral("vnc"), how, Seeds::asQt(rfbt::join({signedIn("RFB 003.008\n", "a guest"), raw})));
    }
    seeds.put(QStringLiteral("vnc"), '\x20',
              Seeds::asQt(rfbt::join({signedIn("RFB 003.008\n", "QEMU (guest)"), zrle})));
    // A name of every kind of sign (a blank, a tab, a line feed, a sign beyond the first plane, one that does not
    // print).
    seeds.put(QStringLiteral("vnc"), '\x30',
              Seeds::asQt(rfbt::join(
                  {signedIn("RFB 003.008\n", "a  name\twith\nsigns \xF0\x9F\x96\xA5 \xE2\x80\x8B end"), raw})));
    // The password refused, in the server's words; and VNC authentication in 3.3.
    seeds.put(QStringLiteral("vnc"), '\x40',
              Seeds::asQt(rfbt::join({rfbt::greeting(), rfbt::securityTypes({2}), challenge, rfbt::word(1),
                                      rfbt::reason("Authentication failed")})));
    seeds.put(QStringLiteral("vnc"), '\x40',
              Seeds::asQt(rfbt::join({rfbt::greeting("RFB 003.003\n"), rfbt::word(2), challenge, rfbt::word(0),
                                      rfbt::serverInit(16, 16, "old"), raw})));
    // None where it is offered beside VNC authentication.
    seeds.put(QStringLiteral("vnc"), '\x40',
              Seeds::asQt(rfbt::join({rfbt::greeting(), rfbt::securityTypes({2, 1}), rfbt::word(0),
                                      rfbt::serverInit(16, 16, "none"), raw})));
}

void jpegSeeds(Seeds& seeds)
{
    for (const QByteArray& jpeg : coret::jpegWalkCorpus()) {
        seeds.put(QStringLiteral("jpeg"), jpeg);
    }
    seeds.put(QStringLiteral("jpeg"), coret::realJpeg16());
}

// --- the inflater
// ------------------------------------------------------------------------------------------------------

QByteArray assorted(qsizetype size, quint32 seed)
{
    QRandomGenerator chance(seed);
    QByteArray made;
    while (made.size() < size) {
        switch (chance.bounded(4)) {
        case 0:
            made += "The quick brown fox jumps over the lazy dog, again and again. ";
            break;
        case 1:
            made += QByteArray(static_cast<qsizetype>(chance.bounded(300)), static_cast<char>(chance.bounded(256)));
            break;
        case 2:
            for (int one = 0; one < 64; ++one) {
                made += static_cast<char>(chance.bounded(256));
            }
            break;
        default:
            made += made.right(std::min<qsizetype>(made.size(), static_cast<qsizetype>(chance.bounded(2000))));
        }
    }
    made.truncate(size);
    return made;
}

void inflateSeeds(Seeds& seeds)
{
    // (Three bytes of the harness's own first: the room, the size of the parts, the budget.)
    const auto put = [&seeds](const QByteArray& how, const QByteArray& stream) {
        seeds.put(QStringLiteral("rfb_inflate"), how + stream);
    };
    quint32 seed = 1;
    for (const int level : {0, 1, 6, 9}) {
        for (const qsizetype size : {1, 40, 900, 6000}) {
            const QByteArray made = qCompress(assorted(size, seed++), level);
            put(QByteArray("\x10\x00\x00", 3), made.sliced(4)); // (qCompress puts the length in front: four bytes)
        }
    }
    put(QByteArray("\x01\x07\x00", 3), qCompress(QByteArray(5000, 'a'), 9).sliced(4));
    for (const rfbt::Deflater::Way way : {rfbt::Deflater::Way::Stored, rfbt::Deflater::Way::Fixed}) {
        rfbt::Deflater deflater(way);
        const QByteArray data = assorted(700, seed++);
        rfbt::Bytes all = deflater.part(rfbt::Bytes(data.begin(), data.end()));
        const rfbt::Bytes more = deflater.part(rfbt::Bytes(data.begin(), data.begin() + 100));
        all.insert(all.end(), more.begin(), more.end());
        put(QByteArray("\x02\x00\x00", 3), Seeds::asQt(all));
        const rfbt::Bytes end = deflater.end();
        all.insert(all.end(), end.begin(), end.end());
        put(QByteArray("\x02\x20\x00", 3), Seeds::asQt(all));
    }
    rfbt::Deflater empty;
    put(QByteArray("\x00\x00\x03", 3), Seeds::asQt(empty.emptyDynamicStream(12)));
}

// --- what a server's controller answers over Redfish
// -------------------------------------------------------------------

void redfishSeeds(Seeds& seeds)
{
    // (One byte of the harness's own first: the answer's HTTP status, by its table, fuzz_status.h: 0 is 200, 1 is 400,
    // 4 is 204, 6 is 403.)
    const auto put = [&seeds](char status, const char* body) { seeds.put(QStringLiteral("redfish"), status, body); };
    put('\x00',
        R"({"@odata.id":"/redfish/v1/Systems/1","Id":"1","Name":"System","Manufacturer":"To Be Filled By O.E.M.",
"Model":"A board","UUID":"4c4c4544-0042-3510-8051-b4c04f4e3132","PowerState":"On","Status":{"Health":"OK","State":"Enabled"},
"Boot":{"BootSourceOverrideEnabled":"Once","BootSourceOverrideTarget":"BiosSetup",
"BootSourceOverrideTarget@Redfish.AllowableValues":["None","Pxe","Hdd","Cd","BiosSetup"],"BootSourceOverrideMode":"UEFI"},
"Actions":{"#ComputerSystem.Reset":{"target":"/redfish/v1/Systems/1/Actions/ComputerSystem.Reset",
"ResetType@Redfish.AllowableValues":["On","ForceOff","GracefulShutdown","ForceRestart","PowerCycle"]}},
"LocationIndicatorActive":false,"IndicatorLED":"Off","@odata.etag":"W/\"1700000000\""})");
    put('\x00',
        R"({"@odata.id":"/redfish/v1/Systems/Self","PowerState":"Off","Boot":{"BootSourceOverrideEnabled":"Disabled"},
"Actions":{"#ComputerSystem.Reset":{"target":"/redfish/v1/Systems/Self/Actions/ComputerSystem.Reset",
"@Redfish.ActionInfo":"/redfish/v1/Systems/Self/ResetActionInfo"}},"IndicatorLED":"Blinking"})");
    put('\x00', R"({"@odata.id":"/redfish/v1/Systems/Self/ResetActionInfo","Parameters":[{"Name":"ResetType",
"AllowableValues":["On","ForceOff","ForceRestart","GracefulRestart"]}]})");
    put('\x00', R"({"@odata.id":"/redfish/v1/Chassis/1/Thermal","Temperatures":[{"Name":"CPU0_TEMP","ReadingCelsius":45,
"Status":{"Health":"OK","State":"Enabled"}},{"Name":"PCH","ReadingCelsius":61.5,"Status":{"Health":"Critical"}}],
"Fans":[{"Name":"CPU_FAN1","Reading":3200,"ReadingUnits":"RPM","Status":{"State":"Enabled","Health":"OK"}},
{"Name":"SYS_FAN2","Reading":40,"ReadingUnits":"Percent","Status":{"State":"Absent"}}]})");
    put('\x00', R"({"@odata.id":"/redfish/v1/Chassis/1/ThermalSubsystem/ThermalMetrics","TemperatureReadingsCelsius":[
{"DeviceName":"CPU","Reading":50,"DataSourceUri":"/redfish/v1/Chassis/1/Sensors/temperature_CPU"},
{"Reading":30,"DataSourceUri":"/redfish/v1/Chassis/1/Sensors/temperature_Inlet"}]})");
    put('\x00',
        R"({"@odata.id":"/redfish/v1/Chassis/1/ThermalSubsystem/Fans","Members":[{"@odata.id":"/redfish/v1/Chassis/1/ThermalSubsystem/Fans/Fan0",
"Name":"Fan 0","SpeedPercent":{"Reading":55,"SpeedRPM":4100},"Status":{"State":"Enabled","Health":"OK"}}],"Members@odata.count":1})");
    put('\x00',
        R"({"@odata.id":"/redfish/v1/Managers/bmc","Model":"OpenBmc","UUID":"0aa1b2c3-d4e5-f607-1829-3a4b5c6d7e8f",
"ServiceEntryPointUUID":"12345678-9abc-def0-1234-56789abcdef0","FirmwareVersion":"1.42.0",
"GraphicalConsole":{"ServiceEnabled":true,"ConnectTypesSupported":["KVMIP"],"MaxConcurrentSessions":4},
"Oem":{"OpenBmc":{"@odata.id":"/redfish/v1/Managers/bmc#/Oem/OpenBmc"}},
"Actions":{"#Manager.Reset":{"target":"/redfish/v1/Managers/bmc/Actions/Manager.Reset","ResetType@Redfish.AllowableValues":["GracefulRestart"]}}})");
    put('\x00',
        R"({"@odata.id":"/redfish/v1","RedfishVersion":"1.11.1","UUID":"fedcba98-7654-3210-fedc-ba9876543210","Vendor":"AMI",
"Systems":{"@odata.id":"/redfish/v1/Systems"},"SessionService":{"@odata.id":"/redfish/v1/SessionService"}})");
    put('\x00', R"({"@odata.id":"/redfish/v1/TaskService/Tasks/1","Id":"1","TaskState":"Running","TaskStatus":"OK",
"PercentComplete":40,"Messages":[{"MessageId":"Base.1.0.Success","Message":"Started"}]})");
    // A task's ends, and its monitor named by a Location (the first line, fuzz_redfish.h): under way, done, failed.
    put('\x00', R"({"@odata.type":"#Task.v1_4_3.Task","Id":"7","TaskState":"Completed","TaskStatus":"OK"})");
    put('\x00', R"({"@odata.id":"/redfish/v1/TaskService/Tasks/8","TaskState":"Exception","TaskStatus":"Critical",
"Messages":[{"MessageId":"Base.1.8.PropertyValueNotInList","Message":"BiosSetup is not allowed now."}]})");
    put('\x03', "https://10.0.0.1/redfish/v1/TaskService/Tasks/7/Monitor\n");
    put('\x03', "");
    // ...a Task that says what it changes, and one that names itself (outside review of D1, H1); a passing error.
    put('\x00', R"({"@odata.id":"/redfish/v1/TaskService/Tasks/1","TaskState":"Completed","TaskStatus":"OK",
"Payload":{"TargetUri":"https://10.0.0.1/redfish/v1/Systems/1/","HttpOperation":"PATCH","JsonBody":"{}"}})");
    put('\x00', R"({"@odata.id":"/redfish/v1/TaskService/Tasks/3","TaskState":"Pending","Payload":{"TargetUri":7}})");
    put('\x0D', R"({"error":{"code":"Base.1.8.ServiceTemporarilyUnavailable","message":"Busy."}})");
    put('\x0C', R"({"error":{"code":"Base.1.8.InternalError","message":"The task could not be read."}})");
    put('\x01', R"({"error":{"code":"Base.1.0.GeneralError","message":"A general error has occurred.",
"@Message.ExtendedInfo":[{"MessageId":"Base.1.0.PropertyValueNotInList","Message":"The value Cd for BootSourceOverrideTarget is not in the list."}]}})");
    put('\x06',
        R"({"error":{"code":"Base.1.0.InsufficientPrivilege","message":"There are insufficient privileges."}})");
    put('\x04', "");
    put('\x00', "a1b2c3d4e5f60718293a4b5c6d7e8f90");
    put('\x00', "server:4c4c4544c0ffee0042351080518b4c04|0aa1b2c3d4e5f60718293a4b5c6d7e8f");
}

// --- what a kvmd KVM answers, and says on its line for events
// ----------------------------------------------------------

void kvmdSeeds(Seeds& seeds)
{
    // (One byte of the harness's own first: the answer's HTTP status, by its table, fuzz_status.h, and the line its
    // texts are cut to.)
    const auto put = [&seeds](char how, const QByteArray& answer) { seeds.put(QStringLiteral("kvmd"), how, answer); };
    put('\x50',
        R"({"ok":true,"result":{"system":{"kvmd":{"version":"4.226"},"platform":{"type":"rpi","base":"Virtual Raspberry Pi",
"model":"test_model"}},"hw":{"health":{"temp":{"cpu":51.2},"throttling":{"parsed_flags":{}}}},
"meta":{"server":{"host":"kvm.example"}},"hostname":"kvm"}})");
    put('\x50', R"({"ok":true,"result":{"streamer":{"source":{"online":true,"resolution":{"width":1920,"height":1080},
"captured_fps":30},"encoder":{"type":"CPU","quality":80}},"features":{"h264":true}}})");
    put('\x50', R"({"ok":true,"result":{"busy":false,"enabled":true,"leds":{"power":true,"hdd":false},"power":"on"}})");
    put('\x50',
        R"({"ok":true,"result":{"keyboard":{"online":true,"leds":{"caps":false}},"mouse":{"online":true,"absolute":true},
"jiggler":{"enabled":false,"active":false}}})");
    put('\x50', R"({"ok":true,"result":{"model":{"ports":[{"name":"one"},{"name":"two"},{"name":""}]},
"video":{"links":[true,false,true]},"summary":{"active_port":1}}})");
    put('\x50', R"({"ok":true,"result":{"token":"a0b1c2d3e4f5.abc-def_ghi~jkl+mno/pq="}})");
    put('\x50', R"({"ok":true,"result":{"battery":101}})");
    put('\x50', R"({"ok":true,"result":{"wol":[{"name":"Server","mac":"00:11:22:33:44:55","ip":"192.168.1.10"}]}})");
    put('\x2D', R"({"ok":false,"result":{"error":"UnavailableError","error_msg":"Service Unavailable"}})"); // (503)
    put('\x50',
        R"({"event_type":"atx","event":{"enabled":true,"busy":false,"leds":{"power":false,"hdd":false},"power":"sleep"}})");
    put('\x50', R"({"event_type":"fingerbot","event":{"exist":true}})");
    put('\x50', QByteArray::fromHex("89504e470d0a1a0a0000000d494844520000078000000438080200000000"));
    put('\x50', coret::realJpeg16());
    put('\x50', "a  text \x01 with\tsigns \xE2\x80\xAE that do not print\xF0\x9F\x96\xA5");
}

void kvmdMediaSeeds(Seeds& seeds)
{
    const auto put = [&seeds](const QByteArray& lines) { seeds.put(QStringLiteral("kvmd_media"), lines); };
    const QByteArray whole =
        R"({"event_type":"msd","event":{"enabled":true,"online":true,"busy":false,"drive":{"image":{"name":"debian.iso",)"
        R"("size":661651456,"complete":true,"mod_ts":1700000000.5},"connected":true,"cdrom":true,"rw":false},)"
        R"("storage":{"images":{"debian.iso":{"size":661651456,"complete":true,"mod_ts":1700000000.5},)"
        R"("tools/memtest.img":{"size":2048000,"complete":false,"mod_ts":1700000100}},)"
        R"("parts":{"":{"size":30000000000,"free":20000000000,"writable":true}},"uploading":null}}})";
    put(whole);
    put(whole + "\n" + R"({"event_type":"msd","event":{"drive":{"connected":false,"image":null}}})" + "\n" +
        R"({"event_type":"msd","event":{"storage":{"images":{"new.iso":{"size":1048576,"complete":true}}}}})" + "\n" +
        R"({"event_type":"msd","event":{"busy":true}})");
    put(R"({"event_type":"msd","event":{"enabled":false,"online":false,"busy":false,"drive":null,"storage":null}})");
    put(R"({"ok":true,"result":{"image":{"name":"new.iso","size":1048576,"written":524288}}})");
    put(R"({"ok":false,"result":{"error":"MsdIsBusyError","error_msg":"Performing another MSD operation, please try again later"}})");
    put(R"({"event_type":"msd","event":{"enabled":true,"online":true,"busy":false,"drive":{"image":null,"connected":false,)"
        R"("cdrom":false,"rw":true},"storage":{"images":{},"parts":{"":{"size":1000,"free":10}}},"filesharing":true}})");
}

// --- a kvmd KVM's stream of pictures
// -----------------------------------------------------------------------------------

void multipartSeeds(Seeds& seeds)
{
    const QByteArray jpeg = coret::realJpeg16();
    const auto part = [](const QByteArray& word, const QByteArray& picture) {
        return "--" + word + "\r\nContent-Type: image/jpeg\r\nX-UStreamer-Width: 16\r\nContent-Length: " +
               QByteArray::number(picture.size()) + "\r\n\r\n" + picture + "\r\n";
    };
    const QByteArray word = "boundarydonotcross";
    const QByteArray kind = "multipart/x-mixed-replace;boundary=" + word + "\n";
    for (const char step : {'\x00', '\x01', '\x07', '\x40'}) {
        seeds.put(QStringLiteral("multipart"), step, kind + part(word, jpeg) + part(word, jpeg.left(40)));
    }
    seeds.put(QStringLiteral("multipart"), '\x00',
              QByteArray("multipart/x-mixed-replace; boundary=\"a quoted word\"\n") + "preamble\r\n" +
                  part("a quoted word", jpeg) + "--a quoted word--\r\n");
}

// --- the program's own JSON reader: a JetKVM's words, and walls as files
// ----------------------------------------------

void strictJsonSeeds(Seeds& seeds, const QString& examples)
{
    const auto put = [&seeds](const QByteArray& text) { seeds.put(QStringLiteral("strict_json"), text); };
    const QByteArray description = R"({"type":"answer","sdp":"v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\n"})";
    put(R"({"type":"answer","data":")" + description.toBase64() + R"("})");
    put(R"({"type":"new-ice-candidate","data":{"candidate":"candidate:1 1 UDP 2122260223 192.168.1.5 54321 typ host","sdpMid":"0","sdpMLineIndex":0}})");
    put(R"({"type":"device-metadata","data":{"deviceVersion":"0.5.9"}})");
    put(R"({"error":"Another session is in progress"})");
    put(R"({"jsonrpc":"2.0","id":1,"result":{"ready":true,"width":1920,"height":1080,"fps":59.94,"error":""}})");
    put(R"({"jsonrpc":"2.0","id":2,"error":{"code":-32603,"message":"Internal error","data":"usb gadget not ready"}})");
    put(R"({"jsonrpc":"2.0","method":"videoInputState","params":{"ready":false,"error":"no_signal","width":0,"height":0,"fps":0}})");
    put(R"({"jsonrpc":"2.0","method":"otherSessionConnected","params":null})");
    put(R"({"jsonrpc":"2.0","method":"keysDownState","params":{"modifier":2,"keys":[4,0,0,0,0,0]}})");
    put(R"({"jsonrpc":"2.0","method":"usbState","params":"configured"})");
    put(R"({"jsonrpc":"2.0","method":"atxState","params":{"power":true,"hdd":false}})");
    put(R"({"jsonrpc":"2.0","method":"localVersion","params":{"appVersion":"0.5.9","systemVersion":"0.2.7"}})");
    // (Every escape there is, and a character beyond the first plane as its two halves: the backslash made of its
    // number, as no escape is written here by hand.)
    const QByteArray slash(1, '\x5C');
    put("[1, -2.5e-3, 1E+2, true, false, null, \"" + slash + "u00e9" + slash + "ud83d" + slash + "udda5" + slash + "n" +
        slash + "t" + slash + "\"" + slash + slash + slash + "/\", {\"a\":{\"b\":[{}]}}, []]");
    // The walls as files the tests read (core/tests/data), each whole.
    for (const QString& folder : {examples, examples + QStringLiteral("/..")}) {
        const QDir dir(folder);
        for (const QString& name : dir.entryList({QStringLiteral("*.json"), QStringLiteral("*.qkvm")}, QDir::Files)) {
            QFile file(dir.filePath(name));
            if (file.open(QIODevice::ReadOnly)) {
                put(file.readAll());
            }
        }
    }
}

// --- what a far end says of its session and addresses
// ------------------------------------------------------------------

void rtcSdpSeeds(Seeds& seeds)
{
    // (One byte of the harness's own first: bit 0 this PC's loopback allowed, bit 1 only an offer's payloads.)
    const auto put = [&seeds](char how, const QByteArray& sdp) { seeds.put(QStringLiteral("rtc_sdp"), how, sdp); };
    const QByteArray janus =
        "v=0\r\no=- 1700000000 1 IN IP4 127.0.0.1\r\ns=Janus streaming\r\nt=0 0\r\na=group:BUNDLE video\r\n"
        "a=fingerprint:sha-256 "
        "AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\nc=IN IP4 127.0.0.1\r\na=mid:video\r\na=sendonly\r\na=rtcp-mux\r\n"
        "a=rtpmap:96 H264/90000\r\na=fmtp:96 profile-level-id=42e01f;packetization-mode=1;"
        "sprop-parameter-sets=Z0LgH6pA,aM48gA==\r\na=rtpmap:97 VP8/90000\r\n"
        "a=candidate:1 1 udp 2015363327 192.168.1.20 40000 typ host\r\n"
        "a=candidate:2 1 udp 1679819007 203.0.113.9 40001 typ srflx raddr 192.168.1.20 rport 40000\r\n"
        "a=end-of-candidates\r\n";
    put('\x00', janus);
    put('\x01', janus);
    const QByteArray answer =
        "v=0\r\no=- 4611731400430051336 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\n"
        "a=fingerprint:sha-256 "
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=sendonly\r\na=rtpmap:102 H264/90000\r\n"
        "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=candidate:3 1 udp 2130706431 127.0.0.1 50000 typ host\r\n"
        "a=candidate:4 1 udp 2130706431 fe80::1 50001 typ host\r\n"
        "a=candidate:5 1 udp 2130706431 a1b2c3.local 50002 typ host\r\n"
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=mid:1\r\na=sctp-port:5000\r\n";
    put('\x02', answer);
    put('\x03', answer);
}

// --- H.264 over RTP
// ------------------------------------------------------------------------------------------------------

QByteArray rtp(quint16 sequence, quint32 time, const QByteArray& payload, bool end = true)
{
    QByteArray bytes(12, '\0');
    bytes[0] = '\x80';
    bytes[1] = static_cast<char>(96 | (end ? 128 : 0));
    qToBigEndian(sequence, bytes.data() + 2);
    qToBigEndian(time, bytes.data() + 4);
    qToBigEndian<quint32>(123, bytes.data() + 8);
    return bytes + payload;
}

void h264RtpSeeds(Seeds& seeds)
{
    // (One byte of the harness's own first, bit 0: the first two are the parameter sets; then each packet behind its
    // length in two bytes.)
    const auto packets = [](char how, std::initializer_list<QByteArray> each) {
        QByteArray made(1, how);
        for (const QByteArray& one : each) {
            made.append(static_cast<char>((one.size() >> 8) & 0x7F));
            made.append(static_cast<char>(one.size() & 0xFF));
            made.append(one);
        }
        return made;
    };
    const QByteArray sps = QByteArray::fromHex("6742e01faa");
    const QByteArray pps = QByteArray::fromHex("68ceaa");
    seeds.put(QStringLiteral("h264_rtp"),
              packets('\x01', {sps, pps, rtp(1, 1, "\x41\x22"), rtp(2, 2, "\x65\x22"), rtp(3, 3, "\x41\x23")}));
    seeds.put(QStringLiteral("h264_rtp"),
              packets('\x01', {sps, pps, rtp(65535, 42, QByteArray::fromHex("7c851122"), false),
                               rtp(0, 42, QByteArray::fromHex("7c453344"))}));
    QByteArray aggregate(1, '\x78');
    for (const QByteArray& nal : {sps, pps, QByteArray("\x65\x22")}) {
        aggregate.append(static_cast<char>(0));
        aggregate.append(static_cast<char>(nal.size()));
        aggregate.append(nal);
    }
    seeds.put(QStringLiteral("h264_rtp"), packets('\x00', {rtp(1, 1, aggregate), rtp(2, 2, "\x41\x22")}));
}

// --- what a JetKVM says
// --------------------------------------------------------------------------------------------------

void jetkvmSeeds(Seeds& seeds)
{
    // (Each word behind a byte of where it came, 0 the signalling line to 7 a Retry-After, and its length in two:
    // fuzz_jetkvm.h.)
    const auto word = [](char where, const QByteArray& said) {
        QByteArray made(1, where);
        made.append(static_cast<char>((said.size() >> 8) & 0xFF));
        made.append(static_cast<char>(said.size() & 0xFF));
        return made + said;
    };
    const auto put = [&seeds](const QByteArray& words) { seeds.put(QStringLiteral("jetkvm"), words); };
    // Its answer as a JetKVM sends it: base64 of JSON whose `sdp` is its description, with no address in it that may be
    // reached (its own, by mDNS's name).
    const QByteArray sdp =
        "v=0\r\no=- 4611731400430051336 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1 2\r\n"
        "a=fingerprint:sha-256 "
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=sendonly\r\na=rtpmap:102 H264/90000\r\n"
        "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=candidate:5 1 udp 2130706431 a1b2c3d4.local 50002 typ host\r\n"
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=mid:1\r\na=sctp-port:5000\r\n";
    const QByteArray description = QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("answer")},
                                                             {QStringLiteral("sdp"), QString::fromLatin1(sdp)}})
                                       .toJson(QJsonDocument::Compact);
    const QByteArray answer = R"({"type":"answer","data":")" + description.toBase64() + R"("})";
    const QByteArray address =
        R"({"type":"new-ice-candidate","data":{"candidate":"candidate:1 1 UDP 2122260223 192.168.1.5 54321 typ host",)"
        R"("sdpMid":"0","sdpMLineIndex":0}})";
    put(word('\x00', answer));
    put(word('\x00', address));
    put(word('\x00', R"({"error":"Another session is in progress"})"));
    put(word('\x00', R"({"type":"device-metadata","data":{"deviceVersion":"0.5.9"}})") + word('\x00', answer));
    // On `rpc`: answers to its calls, and the events it says.
    const QList<QByteArray> rpc = {
        R"({"jsonrpc":"2.0","id":1,"result":{"ready":true,"width":1920,"height":1080,"fps":59.94,"error":""}})",
        R"({"jsonrpc":"2.0","id":2,"error":{"code":-32603,"message":"Internal error","data":"usb gadget not ready"}})",
        R"({"jsonrpc":"2.0","method":"videoInputState","params":{"ready":false,"error":"no_signal","width":0,"height":0,"fps":0}})",
        R"({"jsonrpc":"2.0","method":"otherSessionConnected","params":null})",
        R"({"jsonrpc":"2.0","method":"keysDownState","params":{"modifier":2,"keys":[4,0,0,0,0,0]}})",
        R"({"jsonrpc":"2.0","method":"usbState","params":"configured"})",
        R"({"jsonrpc":"2.0","method":"atxState","params":{"power":true,"hdd":false}})",
        R"({"jsonrpc":"2.0","method":"localVersion","params":{"appVersion":"0.5.9","systemVersion":"0.2.7"}})",
    };
    for (const QByteArray& one : rpc) {
        put(word('\x01', one));
    }
    // On `hidrpc`: its handshake, another version's, its keyboard.
    put(word('\x02', QByteArray("\x01\x01", 2)));
    put(word('\x02', QByteArray("\x01\x02", 2)));
    put(word('\x02', QByteArray("\x33\x02\x04\x05\x00\x00\x00\x00", 8)));
    // Its answer, then an address of its that may not be reached (a multicast one).
    put(word('\x00', answer) +
        word(
            '\x00',
            R"({"type":"new-ice-candidate","data":{"candidate":"candidate:2 1 UDP 2122260223 224.0.0.5 54321 typ host"}})"));
    // A session: its answer, then its input and its keyboard as it says them.
    put(word('\x00', answer) + word('\x01', rpc.at(2)) + word('\x02', QByteArray("\x01\x01", 2)) +
        word('\x01', rpc.at(4)) + word('\x01', rpc.at(0)));
    // To its driver: which it is, whether it is set up, its version on the line, a sign-in's pass, a wait.
    put(word('\x03', R"({"deviceId":"a1b2c3d4e5f6a7b8c9d0"})"));
    put(word('\x04', R"({"isSetup":false})"));
    put(word('\x05', R"({"type":"device-metadata","data":{"deviceVersion":"0.5.9"}})"));
    put(word('\x06', "authToken=a0b1c2d3e4f5.abc-def_ghi; Path=/; HttpOnly; SameSite=Strict"));
    put(word('\x07', "30"));
}

// --- bmc's
// ---------------------------------------------------------------------------------------------------------------

void bmcSeeds(Seeds& seeds)
{
    for (const std::vector<std::uint8_t>& seed : qkvm::bmc::fuzz::aspeedSeeds()) {
        seeds.put(QStringLiteral("bmc_aspeed"), seed);
    }
    for (const std::vector<std::uint8_t>& seed : qkvm::bmc::fuzz::consoleSeeds()) {
        seeds.put(QStringLiteral("bmc_ami_console"), seed);
    }
}

// --- each target's one small seed
// ----------------------------------------------------------------------------------------

void smallestSeeds(Seeds& seeds)
{
    // The handshake alone, no update: a server of 3.8, no password (and one that asks for VNC authentication).
    seeds.put(QStringLiteral("rfb"), coret::handshake16());
    seeds.put(QStringLiteral("vnc"), '\x40', Seeds::asQt(rfbt::join({rfbt::greeting(), rfbt::securityTypes({2})})));
    // A JPEG's first and last markers, nothing between.
    seeds.put(QStringLiteral("jpeg"), QByteArray("\xFF\xD8\xFF\xD9", 4));
    // A zlib stream of one empty stored block, fed whole.
    seeds.put(QStringLiteral("rfb_inflate"), QByteArray::fromHex("100000"
                                                                 "7801"
                                                                 "010000ffff"
                                                                 "00000001"));
    seeds.put(QStringLiteral("redfish"), '\x00', QByteArray("{}"));
    seeds.put(QStringLiteral("kvmd"), '\x50', QByteArray(R"({"ok":true,"result":{}})"));
    seeds.put(QStringLiteral("kvmd_media"), QByteArray(R"({"event_type":"msd","event":{}})"));
    seeds.put(QStringLiteral("multipart"), '\x00', QByteArray("multipart/x-mixed-replace;boundary=b\n"));
    seeds.put(QStringLiteral("strict_json"), QByteArray("{}"));
    seeds.put(QStringLiteral("rtc_sdp"), '\x00', QByteArray("v=0\r\n"));
    seeds.put(QStringLiteral("h264_rtp"), QByteArray(1, '\x00'));
    // One word on the signalling line, of nothing.
    seeds.put(QStringLiteral("jetkvm"), QByteArray("\x00\x00\x02{}", 5));
    // One frame of eight by eight that says no data.
    std::vector<std::uint8_t> frame;
    qkvm::bmc::fuzz::putFrame(frame, qkvm::bmc::aspeed::FrameHeader{8, 8, 1, 1, false, 0, 0, false}, {});
    seeds.put(QStringLiteral("bmc_aspeed"), frame);
    // A console's first word, fed whole.
    std::vector<std::uint8_t> console = {0x00, 0x10};
    const qkvm::bmc::ami::Bytes allowed = qkvm::bmc::ami::message(qkvm::bmc::ami::kind::allowed);
    console.insert(console.end(), allowed.begin(), allowed.end());
    seeds.put(QStringLiteral("bmc_ami_console"), console);
}

int made(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        std::fprintf(stderr, "qkvm-fuzz-seeds <folder> <smallest>\n");
        return 2;
    }
    Seeds seeds(QString::fromLocal8Bit(argv[1]));
    Seeds smallest(QString::fromLocal8Bit(argv[2]));
    smallestSeeds(smallest);
    rfbSeeds(seeds);
    vncSeeds(seeds);
    jpegSeeds(seeds);
    inflateSeeds(seeds);
    redfishSeeds(seeds);
    kvmdSeeds(seeds);
    kvmdMediaSeeds(seeds);
    multipartSeeds(seeds);
    strictJsonSeeds(seeds, QStringLiteral(QKVM_EXPORT_EXAMPLES));
    rtcSdpSeeds(seeds);
    h264RtpSeeds(seeds);
    jetkvmSeeds(seeds);
    bmcSeeds(seeds);
    std::printf("%d seeds made, and %d small ones\n", seeds.made(), smallest.made());
    return seeds.ok() && smallest.ok() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return made(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "the seeds could not be made: %s\n", error.what());
        return 1;
    }
}
