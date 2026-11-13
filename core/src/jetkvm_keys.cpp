// Keys and the mouse over one connection to a JetKVM (JetKvmConnection, its header says what): the road, the
// reports, the keep-alive and the letting go, as the web's jetkvm.py has them (rule 28 of its FOR-THE-DESKTOP-APP).
#include <qkvm/core/jetkvm_connection.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <qkvm/base/clock.h>
#include <qkvm/core/jetkvm_layouts.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

constexpr char hidVersion = 1;
constexpr int mostStep = 127;
constexpr int farthest = 32767;
constexpr int firstModifier = 0xE0;
constexpr int modifierCount = 8;
constexpr int everyLookMs = 10; // how often what is waited for is looked at

const char* const inUseSentence =
    "Someone is using this JetKVM's own console. It shows its picture to one viewer at a time.";

using Report = JetKvmConnection::Report;
using Keyboard = JetKvmConnection::Keyboard;

QByteArray bytesOf(std::initializer_list<int> values)
{
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(values.size()));
    for (const int value : values) {
        bytes.append(static_cast<char>(value & 0xFF));
    }
    return bytes;
}

QByteArray handshake()
{
    return bytesOf({0x01, hidVersion});
}
QByteArray nothingDown()
{
    return bytesOf({0x02, 0x00});
}
QByteArray keypress(int usage, bool press)
{
    return bytesOf({0x05, usage, press ? 1 : 0});
}
QByteArray keepAliveWord()
{
    return bytesOf({0x09});
}
// `03 x4 y4 b`: the place as two four-byte signed numbers, big-endian.
QByteArray placeWord(int x, int y, int buttons)
{
    const auto x32 = static_cast<quint32>(std::clamp(x, 0, farthest));
    const auto y32 = static_cast<quint32>(std::clamp(y, 0, farthest));
    return bytesOf({0x03, static_cast<int>(x32 >> 24), static_cast<int>(x32 >> 16), static_cast<int>(x32 >> 8),
                    static_cast<int>(x32), static_cast<int>(y32 >> 24), static_cast<int>(y32 >> 16),
                    static_cast<int>(y32 >> 8), static_cast<int>(y32), buttons});
}
// `06 dx dy b`: a step as two signed bytes.
QByteArray stepWord(int dx, int dy, int buttons)
{
    return bytesOf({0x06, std::clamp(dx, -mostStep, mostStep), std::clamp(dy, -mostStep, mostStep), buttons});
}

bool isModifier(int usage)
{
    return usage >= firstModifier && usage < firstModifier + modifierCount;
}

QString methodOf(Report::Kind kind)
{
    switch (kind) {
    case Report::Kind::Keyboard:
        return u"keyboardReport"_s;
    case Report::Kind::Place:
        return u"absMouseReport"_s;
    case Report::Kind::Step:
        return u"relMouseReport"_s;
    case Report::Kind::Wheel:
        return u"wheelReport"_s;
    }
    return {};
}

QJsonObject paramsOf(const Report& report)
{
    switch (report.kind) {
    case Report::Kind::Keyboard: {
        QJsonArray keys;
        for (const int key : report.keyboard.keys) {
            keys.append(key);
        }
        return QJsonObject{{u"modifier"_s, report.keyboard.modifiers}, {u"keys"_s, keys}};
    }
    case Report::Kind::Place:
        return QJsonObject{{u"x"_s, report.x}, {u"y"_s, report.y}, {u"buttons"_s, report.buttons}};
    case Report::Kind::Step:
        return QJsonObject{{u"dx"_s, report.x}, {u"dy"_s, report.y}, {u"buttons"_s, report.buttons}};
    case Report::Kind::Wheel:
        return QJsonObject{{u"wheelY"_s, report.y}, {u"wheelX"_s, report.x}};
    }
    return {};
}

// A report folded into the one of its kind that waits last, or none where it cannot be (the web's _folded): a place
// replaces a place, steps and turns add up within a signed byte, and never across another state of the buttons.
std::optional<Report> folded(const Report& waiting, const Report& now)
{
    if (waiting.kind != now.kind || waiting.buttons != now.buttons) {
        return std::nullopt;
    }
    if (now.kind == Report::Kind::Place) {
        return now;
    }
    if (now.kind != Report::Kind::Step && now.kind != Report::Kind::Wheel) {
        return std::nullopt;
    }
    Report sum = now;
    sum.x = waiting.x + now.x;
    sum.y = waiting.y + now.y;
    if (std::abs(sum.x) > mostStep || std::abs(sum.y) > mostStep) {
        return std::nullopt;
    }
    return sum;
}

// Whether a report says a whole state (the keyboard; a place with the buttons; the buttons alone, a step of nothing)
// that is the same as another's.
bool saysAState(const Report& report)
{
    return report.kind == Report::Kind::Keyboard || report.kind == Report::Kind::Place ||
           (report.kind == Report::Kind::Step && report.x == 0 && report.y == 0);
}
bool sameState(const Report& one, const Report& other)
{
    return one.kind == other.kind && one.keyboard == other.keyboard && one.x == other.x && one.y == other.y &&
           one.buttons == other.buttons;
}

} // namespace

// --- the road ------------------------------------------------------------------------------------------------------

void JetKvmConnection::hidOpened()
{
    m_hidOpen = true;
    if (m_open && m_road == Road::Undecided) {
        m_hidWithin.stop();
        beginShaking();
    }
}

void JetKvmConnection::beginShaking()
{
    if (m_shaking || m_road != Road::Undecided) {
        return;
    }
    // (Counted from the channel's opening: what is waited for is the JetKVM's answer.)
    m_shaking = true;
    m_shakeWithin.start(m_pace.shakeWithinMs);
    m_shake.start();
    shake();
}

void JetKvmConnection::shake()
{
    if (m_shaken || m_taken || m_over || m_road != Road::Undecided) {
        m_shake.stop();
        return;
    }
    if (!m_hidOpen || !m_peer || !m_peer->send(hidChannel, handshake())) {
        // (Closed meanwhile: keys go over JSON-RPC.)
        decide(Road::Rpc);
    }
}

void JetKvmConnection::decide(Road road)
{
    if (m_road != Road::Undecided || m_over) {
        return;
    }
    m_shake.stop();
    m_shakeWithin.stop();
    m_hidWithin.stop();
    m_shaking = false;
    m_road = road;
    // What an earlier connection may have left down is let go of before anything else goes; then keys may go.
    settleOwed([here = QPointer<JetKvmConnection>(this)] {
        if (here == nullptr || here->m_over) {
            return;
        }
        here->m_keysReady = true;
        if (here->m_road == Road::Rpc && !here->m_hidOther) {
            // (Asked again, as its own page asks: an answer then moves keys onto hidrpc at a quiet moment.)
            here->m_asksLeft = here->m_pace.asksAgain;
            here->m_lateRoad.start(here->m_pace.askAgainEveryMs);
        }
        emit here->keysReadied();
    });
}

void JetKvmConnection::settleOwed(std::function<void()> then)
{
    JetKvmKept::Leftover& leftover = m_kept->leftover;
    if (!leftover.down || m_taken) {
        then();
        return;
    }
    leftover.down = false;
    if (m_road == Road::Hid) {
        bool all = !sendHid(nothingDown()) && !sendHid(stepWord(0, 0, 0));
        if (all && leftover.placed) {
            all = !sendHid(placeWord(leftover.x, leftover.y, 0));
        }
        if (!all) {
            leftover.down = true; // (said again on the next connection)
        }
        then();
        return;
    }
    // Over JSON-RPC, each once the one before was answered.
    QList<std::pair<QString, QJsonObject>> owed{
        {u"keyboardReport"_s, QJsonObject{{u"modifier"_s, 0}, {u"keys"_s, QJsonArray{}}}},
        {u"relMouseReport"_s, QJsonObject{{u"dx"_s, 0}, {u"dy"_s, 0}, {u"buttons"_s, 0}}},
    };
    if (leftover.placed) {
        owed.append({u"absMouseReport"_s, QJsonObject{{u"x"_s, leftover.x}, {u"y"_s, leftover.y}, {u"buttons"_s, 0}}});
    }
    struct Owing {
        QList<std::pair<QString, QJsonObject>> owed;
        std::function<void()> then;
        std::function<void()> next;
    };
    auto owing = std::make_shared<Owing>(Owing{std::move(owed), std::move(then), {}});
    owing->next = [here = QPointer<JetKvmConnection>(this), weak = std::weak_ptr<Owing>(owing)] {
        const std::shared_ptr<Owing> it = weak.lock();
        if (it == nullptr) {
            return;
        }
        if (here == nullptr) {
            it->next = nullptr;
            return;
        }
        if (it->owed.isEmpty()) {
            const std::function<void()> then = std::move(it->then);
            it->next = nullptr;
            then();
            return;
        }
        const auto [method, params] = it->owed.takeFirst();
        here->call(
            method, params,
            [here, it](const base::Result<json::Value>& said) {
                if (here == nullptr) {
                    it->next = nullptr;
                    return;
                }
                if (!said) {
                    here->m_kept->leftover.down = true;
                    it->owed.clear();
                }
                it->next();
            },
            here->m_pace.takeWithinMs);
    };
    owing->next();
}

void JetKvmConnection::askAgain()
{
    if (m_over || m_closing || m_taken || m_hidOther || m_road != Road::Rpc) {
        m_lateRoad.stop();
        return;
    }
    if (!m_shaken) {
        if (m_asksLeft-- <= 0) {
            m_lateRoad.stop();
            return;
        }
        if (m_hidOpen && m_peer && !m_peer->send(hidChannel, handshake())) {
            m_lateRoad.stop();
        }
        return;
    }
    // It answered late: keys move onto hidrpc at the first moment nothing of the program's is down or on its way over
    // JSON-RPC, so that nothing pressed on one channel is let go of on the other (the JetKVM does each in tasks of its
    // own).
    if (!m_hidOpen) {
        m_lateRoad.stop();
        return;
    }
    if (quiet()) {
        m_road = Road::Hid;
        m_lateRoad.stop();
    }
}

bool JetKvmConnection::quiet() const
{
    return m_kbSent.empty() && (!m_placed || m_absButtons == 0) && m_relButtons == 0 && m_outbox.isEmpty() &&
           !m_sending && m_mouseCalls == 0 && m_unanswered.isEmpty() && m_pressing.isEmpty() && m_givenUp.isEmpty() &&
           !m_typing;
}

// --- what the JetKVM says on hidrpc ----------------------------------------------------------------------------------

void JetKvmConnection::heardHid(const QByteArray& bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    const auto kind = static_cast<unsigned char>(bytes.at(0));
    if (kind == 0x01 && bytes.size() >= 2) {
        if (bytes.at(1) == hidVersion) {
            m_shaken = true;
            if (m_road == Road::Undecided) {
                decide(Road::Hid);
            } else if (m_road == Road::Rpc && m_lateRoad.isActive()) {
                m_lateRoad.setInterval(m_pace.shakeEveryMs);
                askAgain();
            }
            return;
        }
        // Another version: JSON-RPC, and nothing more is asked.
        m_hidOther = true;
        decide(Road::Rpc);
        return;
    }
    if (kind == 0x33 && bytes.size() >= 2 && m_road == Road::Hid) {
        Keyboard said;
        said.modifiers = static_cast<unsigned char>(bytes.at(1));
        for (qsizetype at = 2; at < std::min<qsizetype>(bytes.size(), 8); ++at) {
            if (const auto key = static_cast<unsigned char>(bytes.at(at)); key != 0) {
                said.keys.append(key);
            }
        }
        keysSaid(said);
    }
}

void JetKvmConnection::keysSaid(const Keyboard& said)
{
    m_keyboardSaid = said;
    // The first keyboard of an action's waiting for this word that is the same (its keys as a set: the order a JetKVM
    // keeps them in is its own), and every one before it (it does them in order), are done. One none waits for is one
    // it made itself, or of the live line's.
    const auto asASet = [](Keyboard keyboard) {
        std::ranges::sort(keyboard.keys);
        return keyboard;
    };
    const Keyboard heard = asASet(said);
    for (qsizetype at = 0; at < m_echoes.size(); ++at) {
        if (asASet(m_echoes.at(at).keyboard) == heard) {
            const QList<Echo> done = m_echoes.first(at + 1);
            m_echoes.remove(0, at + 1);
            for (const Echo& echo : done) {
                QTimer::singleShot(0, QCoreApplication::instance(), [heard = echo.heard] { heard(std::nullopt); });
            }
            return;
        }
    }
}

// --- the live line's reports -----------------------------------------------------------------------------------------

bool JetKvmConnection::live() const
{
    return m_open && !m_over && !m_closing && m_anew.isEmpty();
}

base::Error JetKvmConnection::endedError() const
{
    if (!m_anew.isEmpty()) {
        return {"dropped", "The connection to the JetKVM was made anew (" + m_anew.toStdString() + ")"};
    }
    return {"dropped", "The connection to the JetKVM ended"};
}

std::optional<base::Error> JetKvmConnection::say(const Report& report, quint64 holder, bool fold, bool lettingGo)
{
    if (m_taken) {
        // (Its own page has the viewer: these would land on the keyboard of whoever is at that page.)
        return base::Error{"in_use", inUseSentence};
    }
    if (!live()) {
        return endedError();
    }
    if (!m_keysReady) {
        return base::Error{"dropped", "The connection to the JetKVM is not ready for keys yet"};
    }
    // (A drag begun on one of the JetKVM's two mice and ended on the other: the button is let go of on both.)
    const std::optional<Report> other = otherMouse(report);
    if (m_road == Road::Hid && report.kind != Report::Kind::Wheel) {
        if (std::optional<base::Error> failed = hidReport(report)) {
            return failed;
        }
        return other ? hidReport(*other) : std::nullopt;
    }
    queue(Queued{report, holder, fold, lettingGo});
    if (other) {
        queue(Queued{*other, holder, false, lettingGo});
    }
    if (!live()) {
        return endedError();
    }
    sayInTurn();
    return std::nullopt;
}

std::optional<base::Error> JetKvmConnection::sendHid(const QByteArray& bytes)
{
    if (!live()) {
        return endedError();
    }
    if (!m_hidOpen || !m_peer) {
        noMoreHid();
        return base::Error{"dropped", "The JetKVM closed the channel keys go over"};
    }
    if (m_peer->bufferedAmount(hidChannel) > m_pace.mostBufferedBytes) {
        // Far behind: a JetKVM (or a link) that takes them slower than they are sent. The connection is made anew,
        // and with it what waits; the JetKVM lets go of the keyboard as it ends.
        makeAnew(u"it was not keeping up with keys and the mouse"_s);
        return base::Error{"dropped",
                           "The connection to the JetKVM ended: it was not keeping up with keys and the mouse"};
    }
    if (!m_peer->send(hidChannel, bytes)) {
        if (m_taken || m_peer->stopped()) {
            return base::Error{"in_use", inUseSentence};
        }
        noMoreHid();
        return base::Error{"dropped", "The JetKVM closed the channel keys go over"};
    }
    return std::nullopt;
}

std::optional<base::Error> JetKvmConnection::hidReport(const Report& report)
{
    switch (report.kind) {
    case Report::Kind::Keyboard: {
        Keyboard keyboard = report.keyboard;
        keyboard.modifiers &= 0xFF;
        keyboard.keys.removeIf([](int key) { return key <= 0 || key > 0xFF; });
        keyboard.keys = keyboard.keys.first(std::min<qsizetype>(keyboard.keys.size(), 6));
        return hidKeyboard(keyboard);
    }
    case Report::Kind::Place: {
        const int x = std::clamp(report.x, 0, farthest);
        const int y = std::clamp(report.y, 0, farthest);
        const int buttons = report.buttons & 0xFF;
        // (Said before it goes, as all that was and is down: one that could not go may have gone.)
        const int before = m_placed ? m_absButtons : 0;
        m_placed = true;
        m_absX = x;
        m_absY = y;
        m_absButtons = before | buttons;
        m_kept->leftover.placed = true;
        m_kept->leftover.x = x;
        m_kept->leftover.y = y;
        if (std::optional<base::Error> failed = sendHid(placeWord(x, y, buttons))) {
            return failed;
        }
        m_absButtons = buttons;
        return std::nullopt;
    }
    case Report::Kind::Step: {
        const int buttons = report.buttons & 0xFF;
        m_relButtons |= buttons;
        if (std::optional<base::Error> failed = sendHid(stepWord(report.x, report.y, buttons))) {
            return failed;
        }
        m_relButtons = buttons;
        return std::nullopt;
    }
    case Report::Kind::Wheel:
        break;
    }
    return base::Error{"invalid", "A JetKVM takes no turn of the wheel over hidrpc"};
}

std::optional<base::Error> JetKvmConnection::hidKeyboard(const Keyboard& now)
{
    const Keyboard old = m_kbSent;
    if (now == old) {
        return std::nullopt;
    }
    // (Said before they go, as all that was and is down: a message that could not go may have gone.)
    m_kbSent.modifiers = old.modifiers | now.modifiers;
    for (const int key : now.keys) {
        if (!m_kbSent.keys.contains(key)) {
            m_kbSent.keys.append(key);
        }
    }
    if (now.empty()) {
        if (std::optional<base::Error> failed = sendHid(nothingDown())) {
            return failed;
        }
        m_held.clear();
    } else {
        // As the JetKVM's own page says it: a keypress message for each key that changed, those let go of first, then
        // the modifiers, then those pressed.
        for (const int key : old.keys) {
            if (!now.keys.contains(key)) {
                if (std::optional<base::Error> failed = sendHid(keypress(key, false))) {
                    return failed;
                }
                m_held.removeAll(key);
            }
        }
        for (int bit = 0; bit < modifierCount; ++bit) {
            if (((old.modifiers ^ now.modifiers) & (1 << bit)) != 0) {
                if (std::optional<base::Error> failed =
                        sendHid(keypress(firstModifier + bit, (now.modifiers & (1 << bit)) != 0))) {
                    return failed;
                }
            }
        }
        for (const int key : now.keys) {
            if (!old.keys.contains(key)) {
                if (std::optional<base::Error> failed = sendHid(keypress(key, true))) {
                    return failed;
                }
                if (!isModifier(key)) {
                    m_held.append(key);
                }
            }
        }
    }
    m_kbSent = now;
    if (!m_held.isEmpty() && !m_keepAlive.isActive()) {
        m_keepAliveDueUs = base::steadyNowUs() + qint64{m_pace.keepAliveEveryMs} * 1000;
        m_keepAlive.start(m_pace.keepAliveEveryMs);
    }
    return std::nullopt;
}

void JetKvmConnection::keepAlive()
{
    // (Never after the hand-over: it would keep alive the keys of whoever took the viewer. Once the connection is
    // ending, the JetKVM lets go of the key itself within a tenth of a second.)
    if (m_held.isEmpty() || m_taken || !live() || !m_hidOpen || !m_peer || !m_peer->send(hidChannel, keepAliveWord())) {
        return;
    }
    const qint64 step = qint64{m_pace.keepAliveEveryMs} * 1000;
    const qint64 now = base::steadyNowUs();
    m_keepAliveDueUs += step;
    if (m_keepAliveDueUs < now) {
        // (Late, the loop having been busy: the next a whole step from now, not at once.)
        m_keepAliveDueUs = now + step;
    }
    m_keepAlive.start(static_cast<int>(std::max<qint64>(1, (m_keepAliveDueUs - now) / 1000)));
}

std::optional<Report> JetKvmConnection::otherMouse(const Report& report) const
{
    if (report.kind != Report::Kind::Place && report.kind != Report::Kind::Step) {
        return std::nullopt;
    }
    int relButtons = m_relButtons;
    bool placed = m_placed;
    int x = m_absX;
    int y = m_absY;
    int absButtons = m_absButtons;
    // (Over JSON-RPC, what waits to go says it first: the newest of each kind.)
    for (const Queued& waiting : m_outbox) {
        if (waiting.report.kind == Report::Kind::Step) {
            relButtons = waiting.report.buttons;
        } else if (waiting.report.kind == Report::Kind::Place) {
            placed = true;
            x = waiting.report.x;
            y = waiting.report.y;
            absButtons = waiting.report.buttons;
        }
    }
    if (report.kind == Report::Kind::Place && (relButtons & ~report.buttons) != 0) {
        return Report{Report::Kind::Step, {}, 0, 0, relButtons & report.buttons};
    }
    if (report.kind == Report::Kind::Step && placed && (absButtons & ~report.buttons) != 0) {
        return Report{Report::Kind::Place, {}, x, y, absButtons & report.buttons};
    }
    return std::nullopt;
}

// --- the JSON-RPC road: one report at a time ------------------------------------------------------------------------

void JetKvmConnection::queue(const Queued& queued)
{
    // Folded into one of its kind that waits (the web's _fold_into): only into one that came after the last key or
    // button that waits, of the same holder; a place and a step are not folded across each other, and a turn of the
    // wheel may be folded across either.
    if (queued.fold) {
        for (qsizetype at = m_outbox.size() - 1; at >= 0; --at) {
            Queued& waiting = m_outbox[at];
            if (!waiting.fold || waiting.holder != queued.holder || waiting.lettingGo != queued.lettingGo) {
                break;
            }
            if (waiting.report.kind == queued.report.kind) {
                if (const std::optional<Report> sum = folded(waiting.report, queued.report)) {
                    waiting.report = *sum;
                    return;
                }
                break;
            }
            if (waiting.report.kind != Report::Kind::Wheel && queued.report.kind != Report::Kind::Wheel) {
                break;
            }
        }
    }
    // A whole state the same as the last of its kind that waits adds nothing (the web's _same_as_last).
    if (saysAState(queued.report)) {
        for (qsizetype at = m_outbox.size() - 1; at >= 0; --at) {
            const Queued& waiting = m_outbox.at(at);
            if (waiting.report.kind == queued.report.kind) {
                if (sameState(waiting.report, queued.report) && waiting.holder == queued.holder &&
                    waiting.lettingGo == queued.lettingGo) {
                    return;
                }
                break;
            }
        }
    }
    if (m_outbox.size() >= m_pace.mostWaiting) {
        // Far behind: a JetKVM that answers slower than it is sent to. The connection is made anew, and with it what
        // waits (it is lost); the JetKVM lets go of every key when it ends.
        makeAnew(u"it was not keeping up with keys and the mouse"_s);
        return;
    }
    m_outbox.append(queued);
}

void JetKvmConnection::sayInTurn()
{
    while (!m_sending && !m_outbox.isEmpty() && live()) {
        if (m_taken) {
            m_outbox.clear(); // (its own page has it now: nothing more goes to whoever is there)
            return;
        }
        Queued next = m_outbox.takeFirst();
        if (next.lettingGo && next.report.kind == Report::Kind::Place) {
            // A letting go lets go where the pointer is, as last sent, and moves it nowhere; where nothing is down on
            // that mouse, there is nothing to let go of.
            if (!m_placed || m_absButtons == 0) {
                continue;
            }
            next.report.x = m_absX;
            next.report.y = m_absY;
        }
        m_sending = true;
        const bool presses = (next.report.kind == Report::Kind::Keyboard && !next.report.keyboard.empty()) ||
                             ((next.report.kind == Report::Kind::Place || next.report.kind == Report::Kind::Step) &&
                              next.report.buttons != 0);
        sendReport(methodOf(next.report.kind), paramsOf(next.report), m_pace.takeWithinMs,
                   presses ? std::optional<Report>(next.report) : std::nullopt,
                   [here = QPointer<JetKvmConnection>(this)](const base::Result<json::Value>& said) {
                       if (here == nullptr) {
                           return;
                       }
                       here->m_sending = false;
                       if (!said && (said.error().code == "timeout" || said.error().code == "dropped")) {
                           // Not answered in time, or the channel is gone: what waits is not sent after it, out of
                           // turn. The connection is made anew instead (the JetKVM then lets go of every key).
                           here->m_outbox.clear();
                           if (said.error().code == "timeout") {
                               here->makeAnew(u"it stopped answering keys and the mouse"_s);
                           }
                           return;
                       }
                       // (Refused, this one: the next goes all the same.)
                       here->sayInTurn();
                   });
    }
}

void JetKvmConnection::sendReport(const QString& method, const QJsonObject& params, int withinMs,
                                  std::optional<Report> press, std::function<void(base::Result<json::Value>)> done)
{
    Waiting waiting;
    waiting.done = std::move(done);
    waiting.report = true;
    waiting.mouse = method == "absMouseReport"_L1 || method == "relMouseReport"_L1;
    waiting.press = std::move(press);
    const bool mouse = waiting.mouse;
    Report said;
    said.kind = method == "keyboardReport"_L1   ? Report::Kind::Keyboard
                : method == "absMouseReport"_L1 ? Report::Kind::Place
                : method == "relMouseReport"_L1 ? Report::Kind::Step
                                                : Report::Kind::Wheel;
    if (const std::optional<qint64> id = ask(method, params, std::move(waiting), withinMs)) {
        static_cast<void>(id);
        if (mouse) {
            ++m_mouseCalls;
        }
        said.keyboard.modifiers = params.value(u"modifier"_s).toInt();
        for (const auto& key : params.value(u"keys"_s).toArray()) {
            said.keyboard.keys.append(key.toInt());
        }
        said.x = params.value(said.kind == Report::Kind::Step ? u"dx"_s : u"x"_s).toInt();
        said.y = params.value(said.kind == Report::Kind::Step ? u"dy"_s : u"y"_s).toInt();
        said.buttons = params.value(u"buttons"_s).toInt();
        noted(said);
    }
}

void JetKvmConnection::settled(Waiting& call, bool answered)
{
    if (!call.report) {
        return;
    }
    if (call.press) {
        // Answered after the hand-over: done after it, and let go of at once. Not answered (no answer in time, cut
        // off): unknown, and let go of the same way while the connection is open; given up on, it is let go of again
        // should its answer come after all (heard). Answered before the hand-over: the JetKVM let go of it in the
        // hand-over.
        const Report press = *std::exchange(call.press, std::nullopt);
        m_pressing.remove(call.id);
        const bool late = m_afterHandOver.remove(call.id);
        if (late || !answered) {
            letGoOf(press);
        }
        if (!answered && !m_over) {
            m_givenUp.append({call.id, press});
            while (m_givenUp.size() > mostGivenUp) {
                m_givenUp.removeFirst();
            }
        }
    }
    if (call.mouse) {
        call.mouse = false;
        m_mouseCalls = std::max(0, m_mouseCalls - 1);
        if (m_taken && m_mouseCalls == 0) {
            // The last mouse report of the program's the JetKVM had in its hands at the hand-over is through: a button
            // it left down stays down for whoever took the viewer (a JetKVM keeps buttons across a hand-over).
            sendLetGo(false);
        }
    }
}

void JetKvmConnection::noted(const Report& report)
{
    switch (report.kind) {
    case Report::Kind::Keyboard:
        m_kbSent = report.keyboard;
        break;
    case Report::Kind::Place:
        m_placed = true;
        m_absX = report.x;
        m_absY = report.y;
        m_absButtons = report.buttons;
        m_kept->leftover.placed = true;
        m_kept->leftover.x = report.x;
        m_kept->leftover.y = report.y;
        break;
    case Report::Kind::Step:
        m_relButtons = report.buttons;
        break;
    case Report::Kind::Wheel:
        break;
    }
}

void JetKvmConnection::letGoOf(const Report& press)
{
    // Said without waiting for an answer: nothing is down any more of what this report pressed (the web's _let_go_of).
    Report nothing;
    nothing.kind = press.kind;
    if (press.kind == Report::Kind::Place) {
        nothing.x = press.x;
        nothing.y = press.y;
    }
    if (!m_peer || m_over) {
        return;
    }
    const qint64 id = ++m_lastId;
    const QByteArray text = QJsonDocument(QJsonObject{{u"jsonrpc"_s, u"2.0"_s},
                                                      {u"method"_s, methodOf(nothing.kind)},
                                                      {u"params"_s, paramsOf(nothing)},
                                                      {u"id"_s, id}})
                                .toJson(QJsonDocument::Compact);
    if (m_peer->sendLettingGo(rpcChannel, text)) {
        m_unanswered.insert(id);
        noted(nothing);
    }
}

// --- letting go ------------------------------------------------------------------------------------------------------

JetKvmConnection::Went JetKvmConnection::sendLetGo(bool keyboard)
{
    // What this program has down over it, as said last, let go of at once and not waited for (the web's
    // _send_let_go): over hidrpc after everything of the program's before it; over JSON-RPC only the buttons (the
    // JetKVM lets go of the keyboard itself as the connection ends and at a hand-over, and a release sent now could be
    // done before a press still in its hands). Each mouse that has a button down. Where hidrpc has shut and `rpc` has
    // not, the buttons and the keyboard go over that.
    const bool kbDown = keyboard && !m_kbSent.empty();
    const bool absDown = m_placed && m_absButtons != 0;
    const bool relDown = m_relButtons != 0;
    if (!kbDown && !absDown && !relDown) {
        return Went::Nowhere;
    }
    if (!m_taken) {
        m_hadDown = true;
        if (m_over) {
            m_kept->leftover.down = true;
        }
    }
    Went went = Went::Nowhere;
    if (m_road == Road::Hid && m_hidOpen && m_peer) {
        bool sent = true;
        if (kbDown) {
            sent = m_peer->sendLettingGo(hidChannel, nothingDown());
        }
        if (sent && absDown) {
            sent = m_peer->sendLettingGo(hidChannel, placeWord(m_absX, m_absY, 0));
        }
        if (sent && relDown) {
            sent = m_peer->sendLettingGo(hidChannel, stepWord(0, 0, 0));
        }
        if (!sent) {
            // (Closed meanwhile: what the JetKVM keeps is let go of on the next connection, first.)
            m_kept->leftover.down = true;
            return Went::Nowhere;
        }
        m_held.clear();
        m_keepAlive.stop();
        if (kbDown) {
            m_kbSent = {};
        }
        went = Went::Hid;
    } else {
        const bool kbToo = kbDown && m_road == Road::Hid;
        if (!absDown && !relDown && !kbToo) {
            return Went::Nowhere;
        }
        if (!m_peer || m_over) {
            // Nothing can go: the JetKVM lets go of the keyboard as the connection closes, never a mouse button. Owed
            // to the next connection, first.
            m_kept->leftover.down = true;
            return Went::Nowhere;
        }
        if (kbToo) {
            m_held.clear();
            m_keepAlive.stop();
            letGoOf(Report{Report::Kind::Keyboard, {}, 0, 0, 0});
        }
        if (absDown) {
            letGoOf(Report{Report::Kind::Place, {}, m_absX, m_absY, 0});
        }
        if (relDown) {
            letGoOf(Report{Report::Kind::Step, {}, 0, 0, 0});
        }
        went = Went::Rpc;
    }
    if (absDown) {
        m_absButtons = 0;
    }
    if (relDown) {
        m_relButtons = 0;
    }
    if (!live() && !m_closing) {
        // Handed to a connection that is ending: it may never arrive. Owed to the next one too.
        m_kept->leftover.down = true;
    }
    return went;
}

void JetKvmConnection::letGoAtHandOver()
{
    // The JetKVM lets go of the keyboard as it hands the viewer over, never of a mouse button, and goes on doing what
    // the old connection sends for a moment. Over hidrpc the program cannot tell whether its last messages were done
    // before the hand-over or after: one letting go of each, where the last it said had something down, behind
    // everything of its own. Over JSON-RPC the buttons, where no mouse report of the program's is in its hands (one
    // that is lets go as its answer comes). No keep-alive after it, and nothing else, ever.
    m_held.clear();
    m_keepAlive.stop();
    m_outbox.clear();
    if (m_road == Road::Hid) {
        sendLetGo(true);
        echoesFail(base::Error{"in_use", inUseSentence});
    } else if (m_mouseCalls == 0) {
        sendLetGo(false);
    }
}

void JetKvmConnection::noMoreHid()
{
    if (m_road == Road::Undecided && m_shaking) {
        decide(Road::Rpc);
        return;
    }
    // Where keys go over it, the connection is over (as where `rpc` closes), and made anew.
    if (m_road == Road::Hid && m_open && !m_over && !m_closing) {
        finish(u"dropped"_s, u"The JetKVM closed the channel keys go over"_s);
    }
}

void JetKvmConnection::makeAnew(const QString& why)
{
    if (!m_anew.isEmpty() || m_over || m_closing) {
        return;
    }
    m_anew = why;
    m_outbox.clear();
    m_watch.stop();
    // What it has down is let go of now, as the end is decided, and given its time to leave; then it ends, and the
    // viewer makes another for whoever asks (not counted as the JetKVM being away: madeAnew).
    closeAfterLettingGo([here = QPointer<JetKvmConnection>(this), why] {
        if (here != nullptr) {
            here->finish(u"dropped"_s, u"The connection to the JetKVM was made anew: "_s + why);
        }
    });
}

bool JetKvmConnection::drained(Went went) const
{
    if (!m_peer || went == Went::Nowhere) {
        return true;
    }
    return m_peer->bufferedAmount(went == Went::Hid ? hidChannel : rpcChannel) <= 0;
}

void JetKvmConnection::waitFor(std::function<bool()> until, int withinMs, std::function<void()> then)
{
    // (Each look asks for the next while it waits; the last one there is holds what it waits for.)
    struct Waiting {
        std::function<bool()> until;
        std::function<void()> then;
        QElapsedTimer clock;
        int withinMs = 0;
    };
    auto waiting = std::make_shared<Waiting>(Waiting{std::move(until), std::move(then), {}, withinMs});
    waiting->clock.start();
    auto look = std::make_shared<std::function<void()>>();
    *look = [this, waiting, weak = std::weak_ptr<std::function<void()>>(look)] {
        if (!m_over && !waiting->until() && waiting->clock.elapsed() < waiting->withinMs) {
            if (const std::shared_ptr<std::function<void()>> again = weak.lock()) {
                QTimer::singleShot(everyLookMs, this, [again] { (*again)(); });
            }
            return;
        }
        const std::function<void()> then = std::move(waiting->then);
        if (then) {
            then();
        }
    };
    (*look)();
}

void JetKvmConnection::closeAfterLettingGo(std::function<void()> then)
{
    if (m_taken) {
        // After a hand-over nothing more of the program's goes: the answers still due are given their time, so that a
        // letting go of what it pressed after the hand-over comes after its press.
        waitFor([this] { return m_waiting.isEmpty() && m_givenUp.isEmpty(); }, m_pace.handOverGraceMs, std::move(then));
        return;
    }
    if (pressesInHand()) {
        // Over JSON-RPC the presses in the JetKVM's hands are given their answers first (the web's _presses_answered):
        // sent at once, the letting go overtook a press, and the button, which a JetKVM never lets go of by itself,
        // stayed down.
        waitFor([this] { return !pressesInHand() || m_taken; }, m_pace.handOverGraceMs,
                [this, then = std::move(then)]() mutable { letGoAndWait(std::move(then)); });
        return;
    }
    letGoAndWait(std::move(then));
}

void JetKvmConnection::letGoAndWait(std::function<void()> then)
{
    if (m_taken || m_over) {
        then();
        return;
    }
    Went went = sendLetGo();
    if (went == Went::Nowhere && m_hadDown && m_road == Road::Hid) {
        went = Went::Hid; // (let go of already, by an action that stopped: given the same time to leave)
    }
    if (went == Went::Nowhere) {
        then();
        return;
    }
    waitFor([this, went] { return drained(went); }, m_pace.letGoWaitMs,
            [this, then = std::move(then)]() mutable {
                if (m_over) {
                    then();
                    return;
                }
                QTimer::singleShot(m_pace.letGoSettleMs, this, std::move(then));
            });
}

void JetKvmConnection::leftWhatWasDown()
{
    // (Presses with no answer when it ends may be done after it: down, maybe.)
    if (!m_taken && (!m_pressing.isEmpty() || !m_givenUp.isEmpty())) {
        m_hadDown = true;
    }
    if (m_hadDown) {
        m_kept->leftover.down = true; // (the next connection says "nothing down" first)
    }
}

bool JetKvmConnection::pressesInHand() const
{
    return m_road != Road::Hid && (!m_pressing.isEmpty() || !m_givenUp.isEmpty());
}

void JetKvmConnection::letGoOfPressesInHand()
{
    // (While the connection is open: a press the JetKVM does after its end is otherwise down for whoever has its
    // viewer then.)
    for (const Waiting& call : std::as_const(m_waiting)) {
        if (call.press) {
            letGoOf(*call.press);
        }
    }
}

void JetKvmConnection::echoesFail(const base::Error& why)
{
    const QList<Echo> due = std::exchange(m_echoes, {});
    const std::optional<base::Error> said = why;
    for (const Echo& echo : due) {
        // NOLINTNEXTLINE(bugprone-exception-escape): (the words of an answer out of memory: nothing of ours throws)
        QTimer::singleShot(0, QCoreApplication::instance(), [heard = echo.heard, said] { heard(said); });
    }
}

// --- for an action that types ----------------------------------------------------------------------------------------

std::optional<base::Error> JetKvmConnection::typeWhole(const Keyboard& keyboard,
                                                       std::function<void(const std::optional<base::Error>&)> heard)
{
    if (m_taken) {
        return base::Error{"in_use", inUseSentence};
    }
    if (!live()) {
        return endedError();
    }
    Keyboard whole = keyboard;
    whole.keys = whole.keys.first(std::min<qsizetype>(whole.keys.size(), 6));
    m_echoes.append(Echo{whole, std::move(heard)});
    // (Said before it goes, as all that was and is down.)
    m_kbSent.modifiers |= whole.modifiers;
    for (const int key : whole.keys) {
        if (!m_kbSent.keys.contains(key)) {
            m_kbSent.keys.append(key);
        }
    }
    QByteArray bytes = bytesOf({0x02, whole.modifiers});
    for (const int key : whole.keys) {
        bytes.append(static_cast<char>(key & 0xFF));
    }
    if (std::optional<base::Error> failed = sendHid(bytes)) {
        m_echoes.removeLast();
        return failed;
    }
    m_kbSent = whole;
    // (A whole report: nothing pressed by a keypress message is down any more, nor kept alive.)
    m_held.clear();
    return std::nullopt;
}

void JetKvmConnection::press(const Keyboard& keyboard, int withinMs,
                             std::function<void(base::Result<json::Value>)> done)
{
    Report report;
    report.keyboard = keyboard;
    sendReport(methodOf(report.kind), paramsOf(report), withinMs,
               keyboard.empty() ? std::nullopt : std::optional<Report>(report), std::move(done));
}

void JetKvmConnection::letGoNow()
{
    if (!m_taken) {
        sendLetGo();
    }
}

void JetKvmConnection::keyboardLayout(std::function<void(base::Result<QString>)> done)
{
    if (m_layout) {
        QTimer::singleShot(0, QCoreApplication::instance(),
                           [done = std::move(done), layout = *m_layout] { done(layout); });
        return;
    }
    call(u"getKeyboardLayout"_s, {},
         [here = QPointer<JetKvmConnection>(this), done = std::move(done)](const base::Result<json::Value>& said) {
             QString layout;
             if (said) {
                 layout = jetkvm::layoutOf(said.value());
             } else if (said.error().code == "action_failed" && said.error().message == "Method not found") {
                 layout = QString::fromLatin1(jetkvm::usLayout); // (an older one, which typed US keys only)
             } else {
                 done(said.error());
                 return;
             }
             if (here != nullptr) {
                 here->m_layout = layout;
             }
             done(layout);
         });
}

} // namespace qkvm::core
