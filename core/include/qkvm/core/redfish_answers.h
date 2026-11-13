#pragma once

#include <initializer_list>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QList>
#include <QString>
#include <QStringList>

#include <qkvm/base/result.h>
#include <qkvm/core/server_types.h>

// What a server's management controller (BMC) says over Redfish, read into the program's own types
// (M7). Nothing here touches the network: these are the readers, kept apart from the lines they
// travel on so that each can be tried with whatever a controller might send, as kvmd_answers.h is
// for a kvmd.
//
// It does what the server's driver does (../app/qkvm/redfish.py), which is where real controllers'
// ways were learned; where it does otherwise, the reason stands beside it. A controller is
// somebody else's software on the network: every text of its is cut to a line (kvmd::plain), every
// number is held to a range, every place it points to is checked before anything is sent there, and
// what is not of the shape it should be is not taken for one.
namespace qkvm::core::redfish {

// Temperatures, and fans, of one server that are listed at most (the web's MAX_SENSORS).
// The most pieces of software of a controller's firmware inventory that are read (the web's MOST_IMAGES).
inline constexpr int mostImages = 16;
inline constexpr int mostSensors = 40;
// The mark a server is known by where its computer gave no number of its own when it was added,
// behind a controller that did: known by its controller's alone (the web's SAID_NONE).
inline constexpr QLatin1StringView saidNone{"server:00000000000000000000000000000000"};

// A place on the controller that it points to itself: a path below /redfish/v1 of no more than 300
// signs, of the letters a path is written in, that climbs nowhere however it is spelt (no "." or
// ".." among its parts, no backslash and no slash written as an escape). Empty for anything else it
// may claim: an address somewhere else would be handed the session.
QString link(const QJsonValue& value);
// The first of `values` that is a real name, cut to 60 signs, and not what a board says where nobody
// filled it in ("To Be Filled By O.E.M.", "Default string", "N/A"...); empty where none is.
QString named(std::initializer_list<QJsonValue> values);
// A controller's number where it is one and within reason.
std::optional<double> number(const QJsonValue& value, double least, double most);
// The session a sign-in handed out, as it was sent: itself where it is one that may go back in a
// header (the web's TOKEN, all of it: not a sign more, a line feed behind it included); empty else.
QByteArray token(const QByteArray& said);
// The mark a controller puts on the state of a thing, to be shown again by whoever changes it: its
// ETag header, as sent, or else the resource's own "@odata.etag", whichever first is one; empty: none.
QByteArray markOf(const QByteArray& etag, const QJsonObject& resource);
// What a controller answered where a resource of its was asked for (`path`): the resource; or
// "bad_response", where it answered otherwise than 200, with what is no record, or with an error said
// as 200 (some controllers do at one of their moments: no resource has "error" and lacks its own
// "@odata.id").
base::Result<QJsonObject> resourceFrom(const QString& path, int status, const QByteArray& body);

// --- the sensors, in the two shapes Redfish has for them ------------------------------------------
// The older one is a resource, Thermal, with every temperature and every fan in it (AMI's boards,
// iDRAC, iLO). The newer one (ThermalSubsystem) has the temperatures as readings in ThermalMetrics,
// and the fans as a collection, one resource each (OpenBMC has only this one).

struct Temperature {
    QString name;
    double celsius = 0;
    bool ok = true; // its health is not said, or is "OK"
    bool operator==(const Temperature&) const = default;
};
struct Fan {
    QString name;
    std::optional<double> reading;
    bool inPercent = false; // else in revolutions a minute
    bool ok = true;
    bool operator==(const Fan&) const = default;
};
// Every temperature in a Thermal resource that has a reading within reason, of the first forty.
QList<Temperature> temperaturesOf(const QJsonObject& thermal);
// Every fan in a Thermal resource that is fitted (one that is not is not a fan), of the first forty.
QList<Fan> fansOf(const QJsonObject& thermal);
// Every temperature in a ThermalMetrics resource, named after its device, else after the last part of
// the sensor's place (where that is one on the controller).
QList<Temperature> readingsOf(const QJsonObject& metrics);
// One fan of a Fans collection; none where it is not one that is fitted. Its speed in revolutions
// where those are a number; else as a share of its most where that is one; else no reading, in
// revolutions where those were said at all, and otherwise as the share the resource is by its name.
std::optional<Fan> fanOf(const QJsonValue& fan);

// --- the light to find a server by in a rack --------------------------------------------------------

// Whether it is on, and the change that lights it up and the one that puts it out. Newer controllers
// have a plain on and off for it (LocationIndicatorActive), which is taken where a controller has
// both; older ones a light with states. Where there is a choice it is lit rather than set blinking: on
// a real board a light told to blink was out again within twenty seconds.
struct Light {
    bool on = false;
    QJsonObject lightUp;
    QJsonObject putOut;
};
// None where the controller has no light it says how to switch.
std::optional<Light> locator(const QJsonObject& system);

// --- which server answers --------------------------------------------------------------------------
// Only ever compared, never shown: kept as the first 32 hexadecimal digits of a SHA-256 of the number
// in small letters. A number that is none (not of a UUID's shape; fewer than four different digits in
// it, as the all-zero and all-F numbers of boards whose maker set none) is no number.

// The computer's own number (its UUID), as a mark: "server:" and the digest; empty: it gives none.
QString serverNumber(const QJsonObject& system);
// The number of the controller's Redfish service (its service root's UUID, which anybody may read):
// the digest alone; empty: it gives none.
QString controllerNumber(const QJsonValue& root);
// What a server is remembered by: "server:" and the computer's digest (or saidNone), then the
// controller's where that was said. What is of no such shape is the computer's mark, whole.
struct Marks {
    QString server;
    QString controller; // empty: none remembered
};
Marks marksOf(const QString& machine);
// The numbers a controller's manager names, as digests: the service it hosts (ServiceEntryPointUUID, as
// OpenBMC's does) and itself (UUID: a real AMI board names its service's number there); none that is none.
// Read under a session, against the root's (controllerNumber), which anybody may read.
QStringList managerNumbers(const QJsonObject& manager);

// --- what is done to it ----------------------------------------------------------------------------

// Where an action of a resource ("#ComputerSystem.Reset", "#Manager.Reset") is sent, and the kinds of
// it the controller offers there; and where it describes them instead, where it does not there.
struct Offered {
    QString target;                   // empty: it says none that is a place of its own
    std::optional<QStringList> kinds; // said there as a list (its texts); none: not said there
    QString described;                // its "@Redfish.ActionInfo", where that is a place of its own
};
Offered offeredKinds(const QJsonObject& resource, QLatin1StringView action);
// The kinds of reset an ActionInfo describes (its "ResetType" parameter's AllowableValues); none
// where it says none as a list.
QStringList kindsIn(const QJsonObject& actionInfo);
// The kinds that would do what a button means, in the order they are tried: by what is wanted
// ("power_on", "power_off", "atx_power" by whether the server runs), and the controller's own restart
// ("reboot"). Empty for anything else.
QStringList wanted(const QString& action, bool running);
// The first of `wanted` the controller offers; the first of them all where it says nothing of what it
// offers; empty where it offers none of them.
QString kindToSend(const QStringList& wanted, const QStringList& offered);
// Whether the server runs, as its PowerState says it: what is not "Off" or "PoweringOff" does.
bool runs(const QJsonObject& system);
// What came of a command, by the controller's answer: done (200, 202, 204); refused in its own words,
// or "The controller refused (HTTP ...)" ("action_failed", any 4xx); and neither for anything else (an
// error of its own, an answer a command does not have): "timeout", it may or may not have been done.
base::Result<void> verdictOn(int status, const QByteArray& body);
// What the controller said in an error answer, in its own words; empty where it said nothing that reads as words.
QString errorSaid(const QByteArray& body);

// --- its boot override (Start into BIOS setup) ------------------------------------------------------------------

// The one-time boot targets the controller says it allows (Boot.BootSourceOverrideTarget@Redfish.AllowableValues);
// none where it says none. "BiosSetup" among them: Start into BIOS setup can be offered (the web's `boot_targets`).
QStringList bootTargets(const QJsonObject& system);
// The boot override as it stands, in small letters: (enabled, target), each empty where not said (the web's
// `boot_override`: a controller that writes "ONCE" or "biossetup" means what the standard's "Once" and "BiosSetup" do).
std::pair<QString, QString> bootOverride(const QJsonObject& system);
// Whether the boot override is off as the controller shows it: off only where it says "Disabled" in so many words; on
// where it says another state; none where it says nothing (no Boot, a null, no text): not known, and never taken for
// off (review 4's MEDIUM 2; the web's `_cleared_shown`).
std::optional<bool> overrideOff(const QJsonObject& system);
// Another tool's boot override as the controller shows it ("Pxe, Once"); empty where there is none, or only the one
// Start into BIOS setup sets (the web's `_another_override`).
QString anotherOverride(const QJsonObject& system);

// --- how it is ------------------------------------------------------------------------------------

struct Looked {
    QJsonObject system;
    QList<Temperature> temperatures;
    QList<Fan> fans;
    QString version; // the controller's firmware, as it said it
    // A critical fault it reported a moment ago, still told of (the driver holds one for a while: one
    // sensor at its limit makes a controller call itself Critical and Warning by turns): where it was.
    std::optional<QString> faultHeld;
};
struct Seen {
    Status status;
    // It calls itself Critical now: where, as said (" (CPU0_TEMP, CPU1_FAN)", or nothing), for the
    // driver to hold.
    std::optional<QString> fault;
};
// How a server is, as its card shows it: "online", or "no_signal" where it is off (to the wall, a
// server that is off is a computer without a picture: there, and dark), or "error" ("health") where
// it calls itself Critical or did a moment ago. No picture; its power is known; what is done to it.
Seen statusFrom(const Looked& looked);

} // namespace qkvm::core::redfish
