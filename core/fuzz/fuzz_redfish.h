#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QString>
#include <QStringList>
#include <qkvm/base/fuzz_promise.h>
#include <qkvm/core/redfish_answers.h>

#include <algorithm>
#include <cstdint>
#include <span>

#include "fuzz_status.h"

// The harness of what a server's management controller answers over Redfish (fuzz_redfish.cpp, libFuzzer's way in;
// test_core_fuzz_found, what it once found): redfish_answers.cpp and what it reads with are compiled into the fuzz
// target. An input is one byte that says the answer's HTTP status (fuzz_status.h: 0 is 200), then the body of the
// answer, which every reader of the drivers is given, as the resource it is where it is one, and each of its members
// and of the members of its lists as a fan of a collection and a place it points to. Held to the readers' promises: a
// place on the controller is one below /redfish/v1 that climbs nowhere, a name is short, the lists are of their most,
// a mark is a mark, a task is followed nowhere but in its service.
//
// Its canary (fuzz_promise.h): a Thermal resource taken, from whose lists of members a temperature of 90 degrees or
// more was read beside a fan and its reading (the seeds' temperatures are all lower).
namespace qkvm::fuzz {

namespace redfishs {

// A place on the controller, however it is spelt: below /redfish/v1, of 300 signs at the most, and climbing nowhere
// once its escapes are read (no "." or ".." among its parts, no backslash, no slash written as an escape).
inline void place(const QString& link)
{
    if (link.isEmpty()) {
        return;
    }
    const QString spelt = QString::fromLatin1(QByteArray::fromPercentEncoding(link.toLatin1()));
    const QStringList parts = spelt.split(u'/');
    promise(link.startsWith(QLatin1StringView("/redfish/v1")) && link.size() <= 300 && !spelt.contains(u'\\') &&
                spelt.count(u'/') == link.count(u'/') && !parts.contains(QStringLiteral(".")) &&
                !parts.contains(QStringLiteral("..")),
            "a place the controller points to is one somewhere else");
}

inline void value(const QJsonValue& one)
{
    namespace redfish = core::redfish;
    place(redfish::link(one));
    if (const std::optional<redfish::Fan> fan = redfish::fanOf(one)) {
        promise(fan->name.size() <= 40, "a fan's name is longer than its line");
    }
    static_cast<void>(redfish::number(one, -1000, 1000));
    promise(redfish::named({one}).size() <= 60, "a name is longer than sixty signs");
}

} // namespace redfishs

inline void readRedfish(std::span<const std::uint8_t> input)
{
    namespace redfish = core::redfish;
    if (input.empty()) {
        return;
    }
    const int status = statusOf(input[0]);
    const QByteArray body(reinterpret_cast<const char*>(input.data()) + 1, static_cast<qsizetype>(input.size() - 1));
    static_cast<void>(redfish::verdictOn(status, body));
    promise(redfish::errorSaid(body).size() <= 200, "a controller's words are longer than their line");
    const QByteArray token = redfish::token(body);
    promise(token.isEmpty() || (token.size() <= body.size() && !token.contains('\n') && !token.contains('\r')),
            "a session that cannot go back in a header");
    const redfish::Marks marks = redfish::marksOf(QString::fromUtf8(body));
    static_cast<void>(marks);
    // A change it only queued (D1): where its task is followed (its Location header the body's first line, where it has
    // more than one), and what a look says of the task, at a Task's place and at a monitor's.
    const qsizetype firstLine = body.indexOf('\n');
    const QString task = redfish::taskPlace(firstLine < 0 ? QByteArray() : body.first(firstLine), body);
    if (!task.isEmpty()) {
        promise(task.startsWith(QLatin1StringView("/redfish/v1/TaskService/")), "a task followed outside its service");
        redfishs::place(task);
    }
    using State = redfish::TaskSaid::State;
    for (const QString& at : {task, QStringLiteral("/redfish/v1/TaskService/Tasks/1"),
                              QStringLiteral("/redfish/v1/TaskService/Tasks/1/Monitor")}) {
        const redfish::TaskSaid said = redfish::taskOf(at, status, body, QStringLiteral("/redfish/v1/Systems/1"));
        promise(said.words.size() <= 200, "a controller's words of its task are longer than their line");
        promise(said.words.isEmpty() || said.state == State::Failed || said.state == State::Refused,
                "a task's words where it failed not");
        // (A Task's own place answers with the Task: an answer with nothing says no end there, outside review of D1.)
        promise(at != QStringLiteral("/redfish/v1/TaskService/Tasks/1") || !body.isEmpty() ||
                    (said.state != State::Done && said.state != State::Failed),
                "a Task's own place said ended by an answer with nothing");
    }
    const base::Result<QJsonObject> resource =
        redfish::resourceFrom(QStringLiteral("/redfish/v1/Systems/1"), status, body);
    if (!resource.ok()) {
        return;
    }
    const QJsonObject& said = resource.value();
    static_cast<void>(redfish::markOf(body.left(64), said));
    redfish::Looked looked;
    looked.system = said;
    looked.temperatures = redfish::temperaturesOf(said);
    looked.fans = redfish::fansOf(said);
    promise(looked.temperatures.size() <= redfish::mostSensors && looked.fans.size() <= redfish::mostSensors,
            "more sensors than are listed");
    const bool hot =
        std::ranges::any_of(looked.temperatures, [](const redfish::Temperature& one) { return one.celsius >= 90; });
    const bool turning =
        std::ranges::any_of(looked.fans, [](const redfish::Fan& one) { return one.reading.has_value(); });
    if (hot && turning) {
        reached("a Thermal taken, a temperature of 90 degrees or more read from it beside a fan and its reading");
    }
    const QList<redfish::Temperature> readings = redfish::readingsOf(said);
    promise(readings.size() <= redfish::mostSensors, "more readings than are listed");
    looked.version = redfish::named({said.value(QLatin1StringView("FirmwareVersion"))});
    static_cast<void>(redfish::statusFrom(looked));
    looked.faultHeld = QStringLiteral(" (held)");
    static_cast<void>(redfish::statusFrom(looked));
    static_cast<void>(redfish::locator(said));
    const QString server = redfish::serverNumber(said);
    promise(server.isEmpty() || (server.startsWith(QLatin1StringView("server:")) && server.size() == 39),
            "a server's number that is no mark");
    const QString controller = redfish::controllerNumber(said);
    promise(controller.isEmpty() || controller.size() == 32, "a controller's number that is no mark");
    for (const QString& number : redfish::managerNumbers(said)) {
        promise(number.size() == 32, "a manager's number that is no mark");
    }
    static_cast<void>(redfish::isOpenBmc(said));
    // (Not a place of Redfish's: bmcweb's own WebSocket, or none.)
    const QString console = redfish::rfbConsole(said);
    promise(console.isEmpty() || console == u"/kvm/0", "a console somewhere else than bmcweb's");
    static_cast<void>(redfish::amiRoot(said));
    const bool running = redfish::runs(said);
    for (const QLatin1StringView action :
         {QLatin1StringView("#ComputerSystem.Reset"), QLatin1StringView("#Manager.Reset")}) {
        const redfish::Offered offered = redfish::offeredKinds(said, action);
        redfishs::place(offered.target);
        redfishs::place(offered.described);
        const QStringList kinds = offered.kinds.value_or(redfish::kindsIn(said));
        for (const char* wanted : {"power_on", "power_off", "atx_power", "reboot"}) {
            static_cast<void>(redfish::kindToSend(redfish::wanted(QString::fromLatin1(wanted), running), kinds));
        }
    }
    static_cast<void>(redfish::bootTargets(said));
    static_cast<void>(redfish::overrideKinds(said));
    static_cast<void>(redfish::bootOverride(said));
    static_cast<void>(redfish::overrideOff(said));
    static_cast<void>(redfish::anotherOverride(said));
    static_cast<void>(redfish::setupForEveryBoot(said));
    // Each member, and each member of its lists (a collection's Members, a Thermal's Fans), as the drivers read them.
    for (auto at = said.begin(); at != said.end(); ++at) {
        redfishs::value(at.value());
        if (at.value().isArray()) {
            const QJsonArray list = at.value().toArray();
            for (qsizetype one = 0; one < list.size() && one < 64; ++one) {
                redfishs::value(list.at(one));
            }
        }
    }
}

} // namespace qkvm::fuzz
