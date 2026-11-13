#include <qkvm/core/jetkvm_viewer.h>

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonValue>

#include <qkvm/base/clock.h>
#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The JetKVM's own words where they are the web's (jetkvm.py).
base::Error inUse()
{
    return {"in_use", "Someone is using this JetKVM's own console. It shows its picture to one viewer at a time."};
}
base::Error notConnected()
{
    return {"not_connected", "The JetKVM is taking too long to connect"};
}
base::Error notThroughError()
{
    return {JetKvmViewer::notThroughCode, JetKvmViewer::notThroughSentence};
}

QString plainOf(const json::Value* value, qsizetype limit)
{
    return value != nullptr && value->isText() ? kvmd::plain(QJsonValue(value->text), limit) : QString();
}

// A size it says, within reason (the web's 1 to 100000), or nothing.
int sizeOf(const json::Value* value)
{
    if (value == nullptr || !value->isNumber() || !(value->number >= 1) || value->number > 100'000) {
        return 0;
    }
    return static_cast<int>(value->number);
}

} // namespace

JetKvmViewer::JetKvmViewer(JetKvmDevice& device, std::shared_ptr<JetKvmKept> kept, Pace pace, QObject* parent)
    : QObject(parent), m_device(&device), m_kept(kept ? std::move(kept) : std::make_shared<JetKvmKept>()), m_pace(pace)
{
    m_clock.start();
    m_tick.setInterval(m_pace.unwatchedEveryMs);
    connect(&m_tick, &QTimer::timeout, this, &JetKvmViewer::tick);
    m_tick.start();
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &JetKvmViewer::want);
    m_endingWait.setSingleShot(true);
    connect(&m_endingWait, &QTimer::timeout, this, [this] {
        // The one before is not gone in time: this asking fails with what it ended in, or as taking too long
        // (which was not sent: JetKvmDevice::mayHaveBeenDone), and is asked again later.
        disconnect(m_waitingForEnding);
        const base::Error why = m_lastError.value_or(notConnected());
        m_lastError = why;
        m_retryAt = now() + m_pace.retryPauseMs;
        tellResumed(why);
        if (m_windows > 0) {
            m_retry.start(m_pace.retryPauseMs);
        }
    });
    m_inputWithin.setSingleShot(true);
    connect(&m_inputWithin, &QTimer::timeout, this,
            [this] { tellInputReady(nullptr, base::Error{"timeout", "The JetKVM is taking too long to connect"}); });
}

JetKvmViewer::~JetKvmViewer()
{
    // The driver goes (the KVM changed, or taken off): its connection is given back, and the next driver's
    // waits until it is gone.
    disconnect(m_waitingForEnding);
    // And whoever pressed "Show here" is told it was not, once (from the reading of 2g/2h, M2).
    tellResumed(base::Error{"stopped", "It was not shown here: the connection to the JetKVM was let go of first. "
                                       "Show it here again."});
    tellInputReady(nullptr, base::Error{"dropped", "The connection to the JetKVM was let go of"});
    m_going = true;
    letGo();
}

bool JetKvmViewer::isOpen() const
{
    return m_connection && m_connection->isOpen() && !m_connection->taken();
}

PictureBox::Counts JetKvmViewer::counts() const
{
    return m_connection ? m_connection->counts() : PictureBox::Counts{};
}

void JetKvmViewer::want()
{
    if (m_kept->aside || m_device == nullptr || m_connection || m_endingWait.isActive()) {
        return;
    }
    // An offer came to nothing: no card makes another, each of which would take the JetKVM's one viewer from whoever
    // has it; a window that shows it does, at the hold's pace (from the second reading of c7ec58c, B).
    if (m_kept->notThrough && m_windows == 0) {
        return;
    }
    if (now() < m_retryAt) {
        if (m_windows > 0 && !m_retry.isActive()) {
            m_retry.start(static_cast<int>(m_retryAt - now()));
        }
        return;
    }
    // Found away just now (by a connection of this viewer's, or anything else on its driver's road that connects; a
    // look keeps a count of its own): left alone
    // while the driver's growing hold stands (the web's _check_backoff on the road that connects), and asked again as
    // it ends, by a card that asks or, for a window, by itself.
    if (const qint64 heldMs = m_device->heldBackForMs(); heldMs > 0) {
        if (m_windows > 0 && !m_retry.isActive()) {
            m_retry.start(static_cast<int>(heldMs));
        }
        return;
    }
    if (m_kept->ending != nullptr) {
        // One connection at a time for the KVM (design 3.4): the newest, of whichever driver, is waited for
        // until it is gone.
        m_waitingForEnding = connect(m_kept->ending.data(), &QObject::destroyed, this, [this] {
            disconnect(m_waitingForEnding);
            m_endingWait.stop();
            QTimer::singleShot(0, this, &JetKvmViewer::want);
        });
        m_endingWait.start(m_pace.endingWaitMs);
        return;
    }
    begin();
}

void JetKvmViewer::begin()
{
    const quint64 serial = ++m_kept->serials;
    m_kept->newest = serial;
    m_kept->newestOfferWent = false;
    m_kept->newestOver = false;
    m_connection = std::make_unique<JetKvmConnection>(*m_device, serial, m_pace.connection, m_kept);
    m_cameAbout = false;
    JetKvmConnection* made = m_connection.get();
    m_kept->ending = made;
    connect(made, &JetKvmConnection::offerWent, this, [this, serial] {
        if (m_kept->newest == serial) {
            m_kept->newestOfferWent = true;
        }
    });
    connect(made, &JetKvmConnection::opened, this, [this, made] {
        // Key frames only while no window shows it (design 4); a window that is there has every picture.
        made->keyFramesOnly(m_windows == 0);
        m_lastError.reset();
        m_cameAbout = true;
        m_kept->notThrough = false;
        tellResumed(Done{});
        tellInputReady(made, std::nullopt, false);
    });
    connect(made, &JetKvmConnection::pictureCame, this, [this](const QByteArray& annexB, bool key, qint64 arrivedUs) {
        if (key) {
            m_newestKey = annexB;
            m_newestKeyUs = arrivedUs;
        }
        if (m_windows > 0) {
            emit pictureCame(annexB, key, arrivedUs);
        }
    });
    connect(made, &JetKvmConnection::videoSaid, this, [this, made] {
        if (made->video().ready) {
            m_told = made->video().ready;
        }
    });
    connect(made, &JetKvmConnection::takenByAnother, this, [this, serial] { taken(serial); });
    connect(made, &JetKvmConnection::keysReadied, this, [this, made] { tellInputReady(made, std::nullopt, true); });
    connect(made, &JetKvmConnection::ended, this,
            [this, serial](const QString& code, const QString& message) { ended(serial, code, message); });
    made->start();
}

void JetKvmViewer::inputReady(std::function<void(base::Result<QPointer<JetKvmConnection>>)> done)
{
    // A person asked for keyboard and mouse: as for "Show here", tried now, whatever hold stands (the web's
    // input_ready, urgent).
    asked(true, std::move(done));
}

void JetKvmViewer::connected(std::function<void(base::Result<QPointer<JetKvmConnection>>)> done)
{
    asked(false, std::move(done));
}

void JetKvmViewer::asked(bool keys, std::function<void(base::Result<QPointer<JetKvmConnection>>)> done)
{
    if (m_kept->aside) {
        QTimer::singleShot(0, this, [done = std::move(done)] { done(inUse()); });
        return;
    }
    if (isOpen() && (!keys || m_connection->keysReady())) {
        QTimer::singleShot(0, this,
                           [done = std::move(done), keys, on = QPointer<JetKvmConnection>(m_connection.get())] {
                               if (on == nullptr || !on->isOpen() || (keys && !on->keysReady())) {
                                   done(base::Error{"dropped", "The connection to the JetKVM ended"});
                                   return;
                               }
                               done(on);
                           });
        return;
    }
    m_wantedAt = now();
    m_inputWaiting.append(Asking{keys, std::move(done)});
    if (!m_inputWithin.isActive()) {
        m_inputWithin.start(m_pace.inputReadyWithinMs);
    }
    want();
}

void JetKvmViewer::tellInputReady(JetKvmConnection* on, const std::optional<base::Error>& why, bool keys)
{
    if (m_inputWaiting.isEmpty()) {
        return;
    }
    // (Open, and not its keys yet: only those that wait for nothing more are told.)
    QList<Asking> told;
    QList<Asking> left;
    for (Asking& one : m_inputWaiting) {
        (why || keys || !one.keys ? told : left).append(std::move(one));
    }
    m_inputWaiting = std::move(left);
    if (m_inputWaiting.isEmpty()) {
        m_inputWithin.stop();
    }
    const QPointer<JetKvmConnection> connection(on);
    for (const Asking& one : told) {
        // NOLINTNEXTLINE(bugprone-exception-escape): (the words of an answer out of memory: nothing of ours throws)
        QTimer::singleShot(0, QCoreApplication::instance(), [done = one.done, wantsKeys = one.keys, connection, why] {
            if (why) {
                done(*why);
            } else if (connection == nullptr || !connection->isOpen() || (wantsKeys && !connection->keysReady())) {
                done(base::Error{"dropped", "The connection to the JetKVM ended"});
            } else {
                done(connection);
            }
        });
    }
}

void JetKvmViewer::taken(quint64 serial)
{
    if (m_kept->judge(serial) == JetKvmKept::Word::OwnNewOne) {
        // About the program's own new connection, heard over an older one: that one goes, quietly.
        if (m_connection && m_connection->serial() == serial) {
            letGo();
        }
        return;
    }
    // Another viewer took it: the program steps aside until "Show here", and says so once.
    const bool already = std::exchange(m_kept->aside, true);
    letGo();
    tellResumed(inUse());
    tellInputReady(nullptr, inUse());
    if (!already) {
        emit steppedAside();
    }
}

void JetKvmViewer::ended(quint64 serial, const QString& code, const QString& message)
{
    if (!m_connection || m_connection->serial() != serial) {
        return;
    }
    if (code == u"in_use"_s) {
        taken(serial);
        return;
    }
    tellInputReady(nullptr, base::Error{code.toStdString(), message.toStdString()});
    if (m_connection->madeAnew()) {
        // Ended by this program to make a new one (keys that could be done late and out of turn): the JetKVM is there,
        // and not away for it; the next is made at once for whoever watches (the web's _make_anew).
        letGo();
        m_lastError.reset();
        m_retryAt = 0;
        if (m_windows > 0) {
            m_retry.start(0);
        }
        return;
    }
    m_lastError = base::Error{code.toStdString(), message.toStdString()};
    // Its offer went, and it never came about (the JetKVM hears offers, and its video does not come through from this
    // PC): that offer took the JetKVM's one viewer from whoever had it, and the program never heard of them. No card
    // makes another (want); a window that is opened, and "Show here", do (from the second reading of c7ec58c, B).
    if (m_kept->newest == serial && m_kept->newestOfferWent && !m_cameAbout) {
        m_kept->notThrough = true;
    }
    letGo();
    // An end that found it away (unreachable, timeout, dropped, a certificate not its own) is an attempt that found
    // it so, and the driver's growing hold says when it is asked again; any other end waits the pause (the web's
    // _run: RETRY_PAUSE but for DOWN_CODES, which "have their own, growing, back-off").
    qint64 waitMs = m_pace.retryPauseMs;
    if (m_device != nullptr && JetKvmDevice::isAway(m_lastError->code)) {
        m_device->failed(*m_lastError);
        m_retryAt = 0;
        waitMs = std::max<qint64>(1, m_device->heldBackForMs());
    } else if (m_lastError->code == "another_machine" && m_device != nullptr) {
        // Another JetKVM answers at its address: a card asks again which it is only now and then, not at every turn
        // (the web's SAME_JETKVM_EVERY); "Show here" and a window that is opened ask at once. Every window is told so,
        // for good: there is nothing of this JetKVM's to show.
        waitMs = m_device->m_pace.sameJetKvmEveryMs;
        m_retryAt = now() + waitMs;
        tellResumed(*m_lastError);
        emit anotherAnswers(message);
        return;
    } else {
        m_retryAt = now() + m_pace.retryPauseMs;
    }
    tellResumed(*m_lastError);
    if (m_windows > 0) {
        m_retry.start(static_cast<int>(waitMs));
    }
}

void JetKvmViewer::letGo()
{
    if (!m_connection) {
        return;
    }
    if (m_kept->newest == m_connection->serial()) {
        m_kept->newestOver = true;
    }
    const quint64 serial = m_connection->serial();
    // Nothing more it says is heard here. What it has down is let go of and given its time to leave
    // (JetKvmConnection::letGo), and it is deleted then, never from inside its own signals; the next connection waits
    // until it is gone (JetKvmKept::ending names it until then).
    disconnect(m_connection.get(), nullptr, this, nullptr);
    JetKvmConnection::letGo(std::move(m_connection));
    m_cameAbout = false;
    if (!m_going) {
        emit connectionOver(serial);
    }
}

void JetKvmViewer::tick()
{
    if (m_connection && m_windows == 0 && now() - m_wantedAt > m_pace.unwatchedMs) {
        // UNWATCHED: no window, and no card asked for so long: the JetKVM's one viewer is given back.
        letGo();
    }
}

base::Result<Snapshot> JetKvmViewer::stillForACard()
{
    // A card that asks is somebody watching; a look is not (askStatus).
    m_wantedAt = now();
    // (Another JetKVM at its address: said by every card, until the one it is known by answers again, J5.)
    const auto another = [this] { return m_device != nullptr ? m_device->another() : std::nullopt; };
    if (m_kept->aside) {
        return another().value_or(inUse());
    }
    if (!isOpen()) {
        want();
        if (const std::optional<base::Error> other = another()) {
            return *other;
        }
        // (Its offer came to nothing, and no card makes another: said so, whatever stands besides.)
        if (m_kept->notThrough && m_windows == 0) {
            return notThroughError();
        }
        // (Away: said so while it is left alone, as the web's snapshot does.)
        if (std::optional<base::Error> held = m_device != nullptr ? m_device->heldBack() : std::nullopt) {
            return *held;
        }
        if (m_lastError && now() < m_retryAt) {
            return *m_lastError;
        }
        return base::Error{"no_stream", "Connecting to the JetKVM"};
    }
    const JetKvmConnection::Video& video = m_connection->video();
    if (video.ready == std::optional<bool>(false)) {
        return Snapshot{m_newestKey, false};
    }
    // The screen: a key frame at most FRAME_STALE old, and not from before its input came back (one that
    // arrived within the margin after the word may be from before).
    const qint64 marginUs = qint64{m_pace.connection.afterSignalMarginMs} * 1000;
    const bool fresh = !m_newestKey.isEmpty() &&
                       base::steadyNowUs() - m_newestKeyUs <= qint64{m_pace.connection.frameStaleMs} * 1000 &&
                       (video.signalAtUs == 0 || m_newestKeyUs >= video.signalAtUs + marginUs);
    if (fresh) {
        return Snapshot{m_newestKey, true};
    }
    return base::Error{"no_stream", "Waiting for the JetKVM's video"};
}

void JetKvmViewer::keyGoes()
{
    m_wantedAt = now();
}

void JetKvmViewer::windowCame()
{
    m_wantedAt = now();
    // A person opens it while another JetKVM answers at its address: asked again at once, whichever answers now (J5).
    if (m_device != nullptr && m_device->another()) {
        m_retryAt = 0;
    }
    if (++m_windows == 1 && m_connection) {
        // From the next key frame that arrives after this moment, never one held before it (F2).
        m_connection->keyFramesOnly(false);
        m_connection->fromTheNextKeyFrame();
    }
    want();
}

void JetKvmViewer::windowLeft()
{
    m_wantedAt = now();
    if (m_windows > 0) {
        --m_windows;
    }
    if (m_windows == 0 && m_connection) {
        m_connection->keyFramesOnly(true);
    }
}

void JetKvmViewer::askForAKeyFrame()
{
    // (At the box's pace, once a second at most, and nothing held is thrown away: WebRtcOffer::requestKey.)
    if (m_connection) {
        m_connection->requestKey();
    }
}

bool JetKvmViewer::askStatus(std::function<void(const std::optional<Status>&)> done)
{
    if (!isOpen()) {
        return false;
    }
    // The four at once, and the power board's only where its extension is on (the web's _read_status).
    struct Asking {
        Said said;
        std::function<void(const std::optional<Status>&)> done;
        int events = 0; // what it had said of its input in events when this was asked (10f)
    };
    auto asking = std::make_shared<Asking>();
    asking->events = m_connection->videoEvents();
    asking->done = std::move(done);
    JetKvmConnection* on = m_connection.get();
    const QPointer<JetKvmViewer> here(this);
    const auto whenAll = [here, asking, on = QPointer<JetKvmConnection>(on)] {
        if (!std::ranges::all_of(asking->said, [](const auto& one) { return one.has_value(); })) {
            return;
        }
        const std::optional<base::Result<json::Value>>& extension = asking->said[2];
        const bool atx = *extension && extension->value().isText() && extension->value().text == u"atx-power"_s;
        if (here == nullptr) {
            return;
        }
        if (!atx || on == nullptr) {
            asking->done(here->made(asking->said, std::nullopt, on, asking->events));
            return;
        }
        on->call(u"getATXState"_s, {}, [here, asking, on](const base::Result<json::Value>& power) {
            if (here != nullptr) {
                asking->done(here->made(asking->said, power, on, asking->events));
            }
        });
    };
    const std::array<QString, 4> methods{u"getVideoState"_s, u"getUSBState"_s, u"getActiveExtension"_s,
                                         u"getLocalVersion"_s};
    for (std::size_t which = 0; which < methods.size(); ++which) {
        on->call(methods[which], {}, [asking, which, whenAll](const base::Result<json::Value>& said) {
            asking->said[which] = said;
            whenAll();
        });
    }
    return true;
}

std::optional<Status> JetKvmViewer::made(const Said& said, const std::optional<base::Result<json::Value>>& atx,
                                         const QPointer<JetKvmConnection>& askedOver, int eventsAtAsking)
{
    Status status;
    status.model = u"JetKVM"_s;
    status.hasPicture = true;
    const auto failedAs = [](const std::optional<base::Result<json::Value>>& one, std::string_view code) {
        return one && !*one && one->error().code == code;
    };
    const auto& [video, usb, extension, version] = said;
    if (m_kept->aside || std::ranges::any_of(said, [&](const auto& one) { return failedAs(one, "in_use"); })) {
        status.state = u"busy"_s;
        status.errorCode = u"in_use"_s;
        status.errorMessage = QString::fromStdString(inUse().message);
        return status;
    }
    if (video && !*video && (video->error().code == "timeout" || video->error().code == "dropped")) {
        if (askedOver == nullptr || !askedOver->isOpen()) {
            // The connection went meanwhile (given back by this program, or ended): nothing was heard of the
            // JetKVM, and its driver looks the plain way (the web's RETIRED, jetkvm.py _read_status).
            return std::nullopt;
        }
        // No answer on a connection that is meant to be open: it is dead, and what was heard over it says
        // nothing of now.
        status.state = u"offline"_s;
        status.errorCode = QString::fromStdString(video->error().code);
        status.errorMessage = QString::fromStdString(video->error().message);
        return status;
    }
    // (An answer asked before an event of its input that came since is older than the event: not taken, and the
    // event's word stands, as the web's _read_status has it. 10f)
    const bool older = askedOver != nullptr && askedOver->videoEvents() != eventsAtAsking;
    const json::Value* state = !older && video && *video && video->value().isObject() ? &video->value() : nullptr;
    const json::Value* ready = state != nullptr ? state->find(u"ready") : nullptr;
    // Where it does not say: believed by its picture (the web's).
    bool signal = !m_newestKey.isEmpty();
    if (ready != nullptr && ready->isFlag()) {
        signal = ready->flag;
    } else if (m_connection && m_connection->video().ready) {
        signal = *m_connection->video().ready;
    }
    m_told = signal;
    status.state = signal ? u"online"_s : u"no_signal"_s;
    status.signal = signal;
    if (state != nullptr) {
        status.width = sizeOf(state->find(u"width"));
        status.height = sizeOf(state->find(u"height"));
    } else if (older) {
        status.width = askedOver->video().width;
        status.height = askedOver->video().height;
    }
    if (usb && *usb && usb->value().isText()) {
        status.keyboard = usb->value().text == u"configured"_s;
    }
    if (version && *version && version->value().isObject()) {
        status.version = plainOf(version->value().find(u"appVersion"), 40);
    }
    if (atx) {
        status.powerIsKnown = true;
        const json::Value* power = *atx && atx->value().isObject() ? atx->value().find(u"power") : nullptr;
        if (power != nullptr && power->isFlag()) {
            status.power = power->flag;
        }
    }
    return status;
}

void JetKvmViewer::resume(std::function<void(base::Result<Done>)> done)
{
    // "Show here": whoever has its own console open loses it (the web's act("resume")).
    if (m_resuming.isEmpty()) {
        m_asideBeforeShowHere = m_kept->aside;
        m_notThroughBeforeShowHere = m_kept->notThrough;
    }
    m_kept->aside = false;
    m_kept->notThrough = false;
    m_lastError.reset();
    m_retryAt = 0;
    // (A person is waiting: tried now, whatever hold stands.)
    if (m_device != nullptr) {
        m_device->tryNow();
    }
    m_wantedAt = now();
    if (isOpen()) {
        QTimer::singleShot(0, this, [done = std::move(done)] { done(Done{}); });
        return;
    }
    m_resuming.append(std::move(done));
    want();
}

void JetKvmViewer::tellResumed(const base::Result<Done>& what)
{
    const QList<std::function<void(base::Result<Done>)>> waiting = std::exchange(m_resuming, {});
    // A "Show here" that did not come to that leaves the program aside as it stood before the press: nothing of a
    // driver of it connects by itself to take another viewer's picture later (from the third reading of the twins, L3).
    if (!waiting.isEmpty() && std::exchange(m_asideBeforeShowHere, false) && !what) {
        m_kept->aside = true;
    }
    // (And where an offer had come to nothing before it, no card makes one again: only a window, or another press.)
    if (!waiting.isEmpty() && std::exchange(m_notThroughBeforeShowHere, false) && !what) {
        m_kept->notThrough = true;
    }
    for (const auto& done : waiting) {
        tellOne(done, what);
    }
}

void JetKvmViewer::tellOne(std::function<void(base::Result<Done>)> done, base::Result<Done> what)
{
    // A turn of the loop later, and the application's: whoever waited is told also where this viewer is gone
    // by then (as JetKvmConnection tells its calls).
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [done = std::move(done), what = std::move(what)] { done(what); });
}

} // namespace qkvm::core
