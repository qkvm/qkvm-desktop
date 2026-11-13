#include <qkvm/core/kvmd_live.h>
#include <qkvm/core/video_diagnostics.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <QByteArrayView>
#include <QHashFunctions>
#include <QSize>

#include <qkvm/base/clock.h>
#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// So many failures to get a picture, one after the other, and the live is over.
constexpr int failuresInARow = 3;

// The driver's words (kvmd_stream.h). For a stream that is not there at all: said by the KVM, by
// what stands before it while its video is awake, or by a yes with something of another kind
// than a stream of pictures. For a KVM that answered with something that is no stream and says
// nothing of whether it has one: an error of its own (500), or what calls itself a stream and is
// not read here. That one is asked again. And for a picture that only is not there yet: the
// video rests and is being woken, or the KVM is slow.
constexpr const char* noStreamHere = "no_stream_here";
constexpr const char* answeredOtherwise = "bad_response";
constexpr const char* resting = "no_stream";

} // namespace

// One asker's Live of a KVM. It holds nothing itself: what it is asked goes to the one KvmdLive
// that all who have the KVM live share, for as long as that one knows it.
class KvmdLive::Handle : public Live {
public:
    explicit Handle(KvmdLive* live) : m_live(live), m_letGoAtOpen(live != nullptr ? live->letGo() : 0) {}
    ~Handle() override { leave(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) = delete;
    Handle& operator=(Handle&&) = delete;

    bool isOpen() const override { return m_live != nullptr; }
    void setNeed(Need need) override
    {
        if (m_live != nullptr) {
            m_live->setNeed(this, need);
        }
    }
    QString transport() const override { return m_live != nullptr ? m_live->m_transport : QString{}; }
    QString fallbackReason() const override { return m_live != nullptr ? m_live->m_fallback : QString{}; }
    quint64 picturesLetGo() const override { return m_live != nullptr ? m_live->letGo() - m_letGoAtOpen : 0; }
    void videoFailed() override
    {
        if (m_live != nullptr && m_live->m_nativeActive) {
            m_live->nativeEnded(u"decode_failed"_s,
                                u"Native video could not be decoded; using the alternate picture path."_s);
        }
    }
    void askForAKeyFrame() override
    {
        if (m_live != nullptr && m_live->m_native) {
            m_live->m_native->requestKey();
        }
    }
    void askControl(bool on) override
    {
        if (m_live != nullptr) {
            m_live->askControl(this, on);
        }
    }
    // (Followed as the KVM is set: whoever shows it chooses no other kind, rule 26.)
    Pointer pointerMode() const override { return m_live != nullptr ? m_live->pointer() : Pointer::Unknown; }
    bool hasControl() const override { return m_live != nullptr && m_live->holds(this); }
    void key(int usage, bool down) override
    {
        if (KvmdConsoleLine* line = lineForThis()) {
            line->key(usage, down);
        }
    }
    void pointerAt(int x, int y) override
    {
        if (KvmdConsoleLine* line = lineForThis()) {
            line->pointerAt(x, y);
        }
    }
    void pointerBy(int dx, int dy) override
    {
        if (KvmdConsoleLine* line = lineForThis()) {
            line->pointerBy(dx, dy);
        }
    }
    void button(int number, bool down) override
    {
        if (KvmdConsoleLine* line = lineForThis()) {
            line->button(number, down);
        }
    }
    void wheel(int dx, int dy) override
    {
        if (KvmdConsoleLine* line = lineForThis()) {
            line->wheel(dx, dy);
        }
    }
    void releaseAll() override
    {
        if (KvmdConsoleLine* line = lineForThis()) {
            line->releaseAll();
        }
    }
    void close() override
    {
        m_closed = true;
        leave();
    }

    // It is over by itself: nothing this one is asked goes anywhere from here on.
    void over() { m_live.clear(); }
    // It was ended from here: nothing more is said to whoever holds it.
    bool wasClosed() const { return m_closed; }

private:
    // The line for keyboard and mouse, where this one holds them or is being given them.
    KvmdConsoleLine* lineForThis() const { return m_live != nullptr ? m_live->lineOf(this) : nullptr; }
    void leave()
    {
        KvmdLive* live = m_live.data();
        m_live.clear();
        if (live != nullptr) {
            live->leave(this);
        }
    }

    QPointer<KvmdLive> m_live;
    // What the KVM's native attempts had let go before this one was handed over.
    quint64 m_letGoAtOpen = 0;
    bool m_closed = false;
};

KvmdLive::KvmdLive(KvmdDevice& device, QObject* parent) : KvmdLive(device, Pace{}, parent) {}

KvmdLive::KvmdLive(KvmdDevice& device, Pace pace, QObject* parent)
    : OwnLive(parent), m_device(&device), m_pace(pace), m_streamWaitMs(pace.streamAgainMs)
{
    m_clock.start();
    m_nativeAgain.setSingleShot(true);
    connect(&m_nativeAgain, &QTimer::timeout, this, &KvmdLive::tryNative);
    m_next.setSingleShot(true);
    connect(&m_next, &QTimer::timeout, this, &KvmdLive::ask);
    m_streamAgain.setSingleShot(true);
    connect(&m_streamAgain, &QTimer::timeout, this, &KvmdLive::askForTheStreamAgain);
    // As long as the driver's stream may take over one picture: parts that are whole and no
    // picture keep both of the stream's own limits, and this is the one they do not keep.
    m_noPicture.setSingleShot(true);
    m_noPicture.setInterval(device.pace().streamPictureMs);
    connect(&m_noPicture, &QTimer::timeout, this, &KvmdLive::noPictureInTime);
    const Status status = device.status();
    m_pointer = pointer();
    m_state = status.state;
    m_errorCode = status.errorCode;
    m_errorMessage = status.errorMessage;
    connect(&device, &KvmdDevice::statusChanged, this, &KvmdLive::tellState);
    connect(&device, &KvmdDevice::passGiven, this, &KvmdLive::driverSignedIn);
}

KvmdLive::~KvmdLive()
{
    // Gone under whoever has the KVM live, which is the program ending: nobody is told from in
    // here, and nothing they ask goes anywhere. Keyboard and mouse are let go by their line,
    // which is handed over to be kept until it has gone, as wherever this is done with one: if
    // the program goes on after all, the line is not thrown away in the same breath. What could
    // not be let go there, nobody is told: there is nobody left to tell.
    for (const Watcher& watcher : m_watchers) {
        watcher.handle->over();
    }
    static_cast<void>(KvmdConsoleLine::letGo(std::move(m_console)));
}

std::unique_ptr<Live> KvmdLive::open(Live::Need need)
{
    auto handle = std::make_unique<Handle>(this);
    Handle* made = handle.get();
    if (m_over || m_device == nullptr) {
        // Taken away already. Told as the others were, when whoever asked holds it.
        made->over();
        QTimer::singleShot(0, made, [made, code = m_overCode, message = m_overMessage] {
            if (!made->wasClosed()) {
                emit made->closed(code, message);
            }
        });
        return handle;
    }
    Watcher watcher;
    watcher.handle = made;
    watcher.need = need;
    m_watchers.push_back(watcher);
    QTimer::singleShot(0, made, [made] {
        if (made->isOpen()) {
            emit made->opened();
        }
    });
    if (m_watchers.size() == 1) {
        begin();
    } else {
        if (m_pacing && m_next.isActive()) {
            waitForTheNext();
        }
        if (m_nativeActive && m_native) {
            m_native->requestKey();
        }
    }
    return handle;
}

void KvmdLive::end(const base::Error& why)
{
    if (m_over) {
        return;
    }
    m_over = true;
    m_overCode = QString::fromStdString(why.code);
    m_overMessage = QString::fromStdString(why.message);
    finish(m_overCode, m_overMessage);
    // Nothing more is asked of the driver, and nothing it says is passed on.
    if (m_device != nullptr) {
        disconnect(m_device.data(), nullptr, this, nullptr);
    }
    m_device.clear();
}

Live::Need KvmdLive::need() const
{
    // The widest and the fastest that anybody asked for.
    Live::Need all{0, 0};
    for (const Watcher& watcher : m_watchers) {
        all.maxWidth = std::max(all.maxWidth, watcher.need.maxWidth);
        all.maxFps = std::max(all.maxFps, watcher.need.maxFps);
    }
    return all;
}

KvmdLive::Watcher* KvmdLive::watcherOf(const Handle* handle)
{
    const auto found = std::ranges::find_if(m_watchers, [handle](const Watcher& one) { return one.handle == handle; });
    return found == m_watchers.end() ? nullptr : &*found;
}

QList<QPointer<KvmdLive::Handle>> KvmdLive::handles() const
{
    QList<QPointer<Handle>> all;
    for (const Watcher& watcher : m_watchers) {
        all.append(QPointer<Handle>(watcher.handle));
    }
    return all;
}

void KvmdLive::setNeed(const Handle* handle, Live::Need need)
{
    Watcher* watcher = watcherOf(handle);
    if (watcher == nullptr || watcher->need == need) {
        return;
    }
    watcher->need = need;
    // A faster need is not made to wait out the slower one's pause.
    if (m_pacing && m_next.isActive()) {
        waitForTheNext();
    }
}

void KvmdLive::leave(Handle* handle)
{
    const auto gone = std::ranges::find_if(m_watchers, [handle](const Watcher& one) { return one.handle == handle; });
    if (gone == m_watchers.end()) {
        return;
    }
    m_watchers.erase(gone);
    // Whoever closed it still holds it, and is told what could not be let go. One that is thrown
    // away has nobody left to tell: what is still down there is the KVM's to let go.
    if (m_holder == handle && giveBack() && handle->wasClosed()) {
        QTimer::singleShot(0, handle, [handle] { emit handle->leftDown(); });
    }
    if (m_watchers.empty()) {
        stop();
    }
}

void KvmdLive::begin()
{
    m_fallback.clear();
    m_transport.clear();
    m_nativeWaitMs = m_pace.nativeAgainMs;
    m_nativeAgain.start(0);
    startJpeg();
}

void KvmdLive::startJpeg()
{
    m_failures = 0;
    m_rested = false;
    m_askedAwake = false;
    m_mark.reset();
    m_streamWaitMs = m_pace.streamAgainMs;
    if (servesNoStream()) {
        // Still remembered: its pictures, and its stream again when that has had its time.
        askOneAfterTheOther();
        m_streamAgain.start(static_cast<int>(std::max<qint64>(0, *m_noStreamUntil - m_clock.elapsed())));
    } else {
        m_noStreamUntil.reset();
        watch();
    }
}

void KvmdLive::stop()
{
    // (A question for a picture that is still out is answered to nobody: `answered`.)
    m_asking = false;
    m_nativeAgain.stop();
    m_nativeActive = false;
    if (m_native) {
        m_native->close();
        m_letGoBefore += m_native->picturesLetGo();
        m_native.release()->deleteLater();
    }
    m_next.stop();
    m_streamAgain.stop();
    m_noPicture.stop();
    if (m_stream != nullptr) {
        m_stream->close();
        m_stream.release()->deleteLater();
    }
}

void KvmdLive::watch()
{
    if (m_nativeActive || m_device == nullptr || m_watchers.empty()) {
        return;
    }
    m_streamGave = false;
    m_noPicture.stop();
    m_stream = m_device->watch();
    // (Only the stream that is held is heard: one that was let go of says nothing more anyway.)
    KvmdStream* stream = m_stream.get();
    connect(stream, &KvmdStream::pictureCame, this, [this, stream](const QByteArray& jpeg) {
        if (m_stream.get() != stream) {
            return;
        }
        const QPointer<KvmdLive> self(this);
        const bool isPicture = hand(jpeg, false);
        // (Whoever was handed it may have let go of the KVM there, and the stream with it.)
        if (self == nullptr || m_stream.get() != stream) {
            return;
        }
        if (!isPicture) {
            // A part that is no picture: the stream has given nothing by it, and has from the
            // first such part as long as one picture may take to give one.
            if (!m_noPicture.isActive()) {
                m_noPicture.start();
            }
            return;
        }
        m_noPicture.stop();
        m_streamLastAt = m_clock.elapsed();
        if (!m_streamGave) {
            // Its stream is there: nothing more is asked for one after the other. And what was
            // noted of a stream that only rested is over.
            m_streamGave = true;
            m_streamGaveAt = m_streamLastAt;
            m_asking = false;
            m_next.stop();
            m_rested = false;
            m_askedAwake = false;
        }
    });
    connect(stream, &KvmdStream::ended, this, [this, stream](const QString& code, const QString& message) {
        if (m_stream.get() == stream) {
            streamEnded(code, message);
        }
    });
}

void KvmdLive::streamEnded(const QString& code, const QString& message)
{
    // (Not thrown away from inside what it says.)
    m_stream.release()->deleteLater();
    streamIsOver(code, message);
}

void KvmdLive::noPictureInTime()
{
    if (m_stream == nullptr) {
        return;
    }
    // Parts came all the while, and no picture among them for as long as one may take: whoever
    // looks would keep the last picture up as if it were live. Ended as a stream that answered
    // with something that is none.
    m_stream->close();
    m_stream.release()->deleteLater();
    streamIsOver(QLatin1StringView(answeredOtherwise), u"The device's stream sends no pictures"_s);
}

void KvmdLive::streamIsOver(const QString& code, const QString& message)
{
    m_noPicture.stop();
    if (m_device == nullptr) {
        // Its driver went under it, untold: there is nobody to ask again.
        finish(code, message);
        return;
    }
    const bool rests = code == QLatin1StringView(resting);
    if (m_streamGave) {
        // It ended after pictures had come. One that gave them for long was a good one:
        // whatever went wrong before it is over.
        if (m_streamLastAt - m_streamGaveAt >= m_pace.streamAgainLongestMs) {
            m_streamWaitMs = m_pace.streamAgainMs;
        }
        askForTheStreamLater();
    } else if (code == QLatin1StringView(noStreamHere) || (rests && m_askedAwake)) {
        // It serves none: remembered, for a time and not for good.
        m_noStreamUntil = m_clock.elapsed() + m_pace.noStreamForMs;
        m_streamAgain.start(m_pace.noStreamForMs);
    } else if (rests) {
        m_rested = true;
    } else if (code == QLatin1StringView(answeredOtherwise) || m_asking || m_native) {
        // It answered with something that is no stream, which says nothing of whether it has
        // one. Or it could not be asked, while its pictures come one after the other: whether
        // the live goes on is theirs to say. Native negotiation also gets its full deadline;
        // failure of the alternate path must not cancel a receiver that can still connect.
        askForTheStreamLater();
    } else {
        // The first since somebody came could not be asked at all, or broke off: the KVM is
        // away, or refuses.
        finish(code, message);
        return;
    }
    if (!m_asking) {
        askOneAfterTheOther();
    }
}

void KvmdLive::askForTheStreamLater()
{
    m_streamAgain.start(m_streamWaitMs);
    m_streamWaitMs = static_cast<int>(std::min<qint64>(qint64{m_streamWaitMs} * 2, m_pace.streamAgainLongestMs));
}

void KvmdLive::askForTheStreamAgain()
{
    if (m_watchers.empty() || m_device == nullptr || m_stream != nullptr) {
        return;
    }
    // (Also where "no stream" was remembered and has had its time: asked as if for the first time.)
    m_noStreamUntil.reset();
    m_rested = false;
    m_askedAwake = false;
    watch();
}

void KvmdLive::driverSignedIn()
{
    if (!m_noStreamUntil) {
        return;
    }
    // kvmd was started anew, or its session ran out: what answered for its stream before may
    // be another by now. Its stream is asked for again a moment later, where somebody has it
    // live: the driver says this from the middle of a question.
    m_noStreamUntil.reset();
    if (!m_watchers.empty() && m_stream == nullptr) {
        m_streamWaitMs = m_pace.streamAgainMs;
        askForTheStreamLater();
    }
}

void KvmdLive::askOneAfterTheOther()
{
    m_asking = true;
    m_failures = 0;
    ask();
}

void KvmdLive::ask()
{
    // The next when the last has come: never two at once.
    if (!m_asking || m_out || m_watchers.empty() || m_device == nullptr) {
        return;
    }
    m_out = true;
    m_askedAt = m_clock.elapsed();
    // As it captured it: a width beyond every size the KVM would shrink it to. And a new one
    // each time: a card's asking the driver answers with the picture of a fifth of a second
    // ago, which is no live picture.
    m_device->askNewSnapshot(kvmd::longestPictureSide,
                             [self = QPointer<KvmdLive>(this)](const base::Result<Snapshot>& answer) {
                                 if (self != nullptr) {
                                     self->answered(answer);
                                 }
                             });
}

void KvmdLive::answered(const base::Result<Snapshot>& answer)
{
    m_out = false;
    // Nobody waits for it any more, or the stream has come meanwhile: it is handed to nobody,
    // and no other is asked for.
    if (!m_asking || m_watchers.empty() || m_nativeActive) {
        return;
    }
    if (answer) {
        m_failures = 0;
        const QPointer<KvmdLive> self(this);
        hand(answer.value().picture, true);
        // (Whoever was handed it may have let go of the KVM there.)
        if (self == nullptr || !m_asking || m_watchers.empty()) {
            return;
        }
        if (m_rested && !m_askedAwake && !servesNoStream() && m_stream == nullptr) {
            // Its video is awake now: whether it has a stream is asked once more.
            m_askedAwake = true;
            watch();
        }
        waitForTheNext();
        return;
    }
    const base::Error& why = answer.error();
    m_pacing = false;
    if (why.code == resting) {
        // No failure: asking again soon is the thing to do.
        m_next.start(m_pace.restingAgainMs);
    } else if (++m_failures >= failuresInARow && !m_native) {
        finish(QString::fromStdString(why.code), QString::fromStdString(why.message));
    } else {
        m_next.start(m_pace.failedAgainMs);
    }
}

int KvmdLive::betweenMs() const
{
    return 1000 / std::clamp(need().maxFps, 1, std::max(1, m_pace.mostASecond));
}

void KvmdLive::waitForTheNext()
{
    // Counted from when the last was asked for: a KVM that takes as long to answer as the pace
    // is, is asked again at once.
    m_pacing = true;
    m_next.start(static_cast<int>(std::max<qint64>(0, m_askedAt + betweenMs() - m_clock.elapsed())));
}

bool KvmdLive::hand(const QByteArray& jpeg, bool mayBeTheOneBefore)
{
    if (m_nativeActive) {
        return false;
    }
    // What a KVM sends is a picture only where it says a size that is shown.
    const std::optional<QSize> stated = kvmd::pictureSize(jpeg);
    if (!stated || !kvmd::isShown(*stated)) {
        return false;
    }
    setTransport(mayBeTheOneBefore ? u"snapshots"_s : u"mjpeg"_s);
    bool same = false;
    if (mayBeTheOneBefore) {
        const std::size_t mark = qHash(QByteArrayView(jpeg));
        same = m_mark == std::optional<std::size_t>(mark) && m_markedBytes == jpeg.size();
        m_mark = mark;
        m_markedBytes = jpeg.size();
    }
    const Live::Video video{false, stated->width(), stated->height()};
    Live::Picture picture;
    picture.keyFrame = true;
    picture.takenUs = base::steadyNowUs();
    picture.data = jpeg;
    // Whoever is handed something may let go there, of its own or of somebody else's: each is
    // looked up anew before it is told anything.
    const QPointer<KvmdLive> self(this);
    const QList<QPointer<Handle>> all = handles();
    for (const QPointer<Handle>& handle : all) {
        if (self == nullptr) {
            return true;
        }
        Watcher* watcher = handle != nullptr ? watcherOf(handle) : nullptr;
        if (watcher == nullptr || (same && watcher->counter > 0)) {
            continue;
        }
        if (watcher->told != std::optional<Live::Video>(video)) {
            watcher->told = video;
            emit handle->videoChanged(video);
            if (self == nullptr) {
                return true;
            }
            watcher = handle != nullptr ? watcherOf(handle) : nullptr;
            if (watcher == nullptr) {
                continue;
            }
        }
        picture.counter = ++watcher->counter;
        emit handle->pictureCame(picture);
    }
    return true;
}

void KvmdLive::setTransport(const QString& transport)
{
    if (m_transport == transport) {
        return;
    }
    VideoDiagnostics::record(
        "video_transport",
        {{"value", QStringList{u"connecting"_s, u"webrtc"_s, u"mjpeg"_s, u"snapshots"_s, u"reconnecting"_s}.indexOf(
                       transport)}});
    m_transport = transport;
    const QList<QPointer<Handle>> all = handles();
    for (const QPointer<Handle>& handle : all) {
        if (handle != nullptr && handle->isOpen()) {
            emit handle->transportChanged();
        }
    }
}

void KvmdLive::tryNative()
{
    if (m_native || m_device == nullptr || m_watchers.empty()) {
        return;
    }
    m_native = std::make_unique<KvmdWebRtc>(*m_device);
    KvmdWebRtc* attempt = m_native.get();
    connect(attempt, &KvmdWebRtc::pictureCame, this, [this, attempt](const QByteArray& bytes, bool key, qint64 at) {
        if (m_native.get() != attempt || m_watchers.empty()) {
            return;
        }
        if (!m_nativeActive) {
            if (!key) {
                return;
            }
            m_nativeActive = true;
            m_nativeSince = m_clock.elapsed();
            m_fallback.clear();
            m_asking = false;
            m_next.stop();
            m_streamAgain.stop();
            m_noPicture.stop();
            if (m_stream) {
                m_stream->close();
                m_stream.release()->deleteLater();
            }
            setTransport(u"webrtc"_s);
        }
        handNative(bytes, key, at);
    });
    connect(attempt, &KvmdWebRtc::ended, this, [this, attempt](const QString& code, const QString& reason) {
        if (m_native.get() == attempt) {
            nativeEnded(code, reason);
        }
    });
    attempt->start();
}

void KvmdLive::nativeEnded(const QString& code, const QString& reason)
{
    if (!m_native) {
        return;
    }
    m_native->close();
    m_letGoBefore += m_native->picturesLetGo();
    m_native.release()->deleteLater();
    const bool wasActive = std::exchange(m_nativeActive, false);
    if (wasActive && m_clock.elapsed() - m_nativeSince >= m_pace.nativeAgainLongestMs) {
        m_nativeWaitMs = m_pace.nativeAgainMs;
    }
    m_fallback = reason;
    // Mark the transition even before the first alternate picture; the actual path is
    // named only when that path supplies a picture.
    const QString previous = m_transport;
    m_transport.clear();
    setTransport(wasActive ? u"reconnecting"_s : previous.isEmpty() ? u"connecting"_s : previous);
    if (m_watchers.empty() || m_device == nullptr) {
        return;
    }
    const bool unsupported = code == u"no_native_video"_s || code == u"decode_failed"_s;
    VideoDiagnostics::record("native_retry", {{"delay_ms", unsupported ? m_pace.nativeUnsupportedMs : m_nativeWaitMs},
                                              {"was_active", wasActive},
                                              {"unsupported", unsupported}});
    m_nativeAgain.start(unsupported ? m_pace.nativeUnsupportedMs : m_nativeWaitMs);
    m_nativeWaitMs = static_cast<int>(std::min<qint64>(qint64{m_nativeWaitMs} * 2, m_pace.nativeAgainLongestMs));
    if (wasActive) {
        startJpeg();
    }
}

void KvmdLive::handNative(const QByteArray& bytes, bool key, qint64 arrivedUs)
{
    Live::Picture picture;
    picture.h264 = true;
    picture.keyFrame = key;
    picture.takenUs = base::steadyNowUs();
    picture.data = bytes;
    picture.arrivedUs = arrivedUs;
    // Dimensions come from the decoder, not from the preceding JPEG or a stale device status.
    const Live::Video video{true, 0, 0};
    const QPointer<KvmdLive> self(this);
    const QList<QPointer<Handle>> all = handles();
    for (const QPointer<Handle>& handle : all) {
        if (self == nullptr) {
            return;
        }
        Watcher* watcher = handle != nullptr ? watcherOf(handle) : nullptr;
        if (watcher == nullptr) {
            continue;
        }
        if (watcher->told != std::optional<Live::Video>(video)) {
            if (!key) {
                continue;
            }
            watcher->told = video;
            emit handle->videoChanged(video);
            if (self == nullptr) {
                return;
            }
            watcher = handle != nullptr ? watcherOf(handle) : nullptr;
            if (watcher == nullptr) {
                continue;
            }
        }
        picture.counter = ++watcher->counter;
        emit handle->pictureCame(picture);
    }
}

void KvmdLive::finish(const QString& code, const QString& message)
{
    // Keyboard and mouse first: what is down is let go while their line can still say so.
    const Handle* held = m_holder;
    const bool left = giveBack();
    const std::vector<Watcher> watchers = std::exchange(m_watchers, {});
    stop();
    for (const Watcher& watcher : watchers) {
        Handle* handle = watcher.handle;
        handle->over();
        // Said a turn later: whoever ends a live may be in the middle of its own work (the wall,
        // of taking a KVM off it), and whoever hears of the end asks things there and then.
        QTimer::singleShot(0, handle, [handle, lost = handle == held, left, code, message] {
            const QPointer<Handle> still(handle);
            // (What could not be let go is said also to one who has closed it meanwhile.)
            if (lost && left) {
                emit handle->leftDown();
            }
            if (lost && still != nullptr && !still->wasClosed()) {
                emit still->controlChanged(false, code, message);
            }
            if (still != nullptr && !still->wasClosed()) {
                emit still->closed(code, message);
            }
        });
    }
}

void KvmdLive::askControl(Handle* handle, bool on)
{
    // Every asking and every giving back gets one word of its own, in the order they were
    // said: whoever asks can then tell which word is whose, and need not wait out a time for
    // one that never comes. (A giving back that was answered by nothing, and an asking that was
    // called off unanswered, kept the button for keyboard and mouse inert for seconds.)
    if (!on) {
        if (m_holder == handle) {
            // Given back before they were given: that asking is over with it, and is answered
            // as that, before the word for the giving back.
            if (!m_given) {
                tellNoControlLater(handle, u"given_up"_s,
                                   u"Keyboard and mouse were given back before the KVM had given them."_s);
            }
            // The word for the giving back is said once their line has gone: what it let go of
            // has landed by then (the KVM's goodbye came behind it), or went with the line, which
            // is said first. Whatever is sent to the machine next waits for this word (a typed
            // text, a key combination, a saved action: MachineControl::textAfterRelease), and so
            // does not land beside a key that is still on its way up (rule 27 of the web's
            // FOR-THE-DESKTOP-APP). Within the line's time for a goodbye, at the most.
            const auto left = std::make_shared<bool>(false);
            *left = giveBack([still = QPointer<Handle>(handle), left] {
                if (still != nullptr) {
                    tellNoControlLater(still, {}, {}, *left);
                }
            });
        } else if (watcherOf(handle) != nullptr) {
            // (Nothing was held by this one: its giving back is answered all the same.)
            tellNoControlLater(handle, {}, {});
        }
        return;
    }
    // (An asking by one who holds them already, or is being given them, gets no second word:
    // one asking at a time is the caller's to keep. And nothing is said to one whose live is
    // over: `closed` was its last word.)
    if (m_holder == handle || watcherOf(handle) == nullptr || m_device == nullptr) {
        return;
    }
    if (m_holder != nullptr) {
        tellNoControlLater(handle, u"in_use"_s,
                           u"Keyboard and mouse of this machine are in use in another window. Release them there "
                           "first."_s);
        return;
    }
    m_holder = handle;
    const quint64 turn = ++m_controlTurn;
    const QPointer<KvmdLive> self(this);
    auto made = [self, turn](base::Result<std::unique_ptr<KvmdConsoleLine>> line) {
        // (Given back meanwhile: a line that was made all the same goes here, with what it
        // came in, before it has asked the KVM for anything.)
        if (self != nullptr && turn == self->m_controlTurn) {
            self->consoleMade(std::move(line));
        }
    };
    // (The KVM's mouse is left as it is set: its line takes the kind of move it says, rule 26.)
    m_device->openConsole(std::move(made));
}

void KvmdLive::consoleMade(base::Result<std::unique_ptr<KvmdConsoleLine>> made)
{
    if (!made) {
        // No line: the driver could not sign in, has no password, or hands out none under a pass
        // under which the KVM has just refused the line.
        Handle* asked = std::exchange(m_holder, nullptr);
        m_given = false;
        ++m_controlTurn;
        if (asked != nullptr) {
            emit asked->controlChanged(false, QString::fromStdString(made.error().code),
                                       QString::fromStdString(made.error().message));
        }
        return;
    }
    m_console = std::move(made).value();
    // (Only the line that is held is heard: one that was closed from here says nothing more.)
    KvmdConsoleLine* line = m_console.get();
    connect(line, &KvmdConsoleLine::opened, this, [this, line] {
        if (m_console.get() == line && m_holder != nullptr) {
            m_given = true;
            emit m_holder->controlChanged(true, {}, {});
        }
    });
    connect(line, &KvmdConsoleLine::closed, this,
            [this, line](const QString& code, const QString& message, bool leftDown) {
                if (m_console.get() != line) {
                    return;
                }
                // The line ended by itself, and what was down could not be let go from here any
                // more (the KVM's to do): whoever held keyboard and mouse is told that there
                // was something, before being told that they went. The pictures go on. (The line
                // was reset when it ended, and has nothing to wait for: thrown away a turn
                // later, and not from inside what it says.)
                Handle* held = std::exchange(m_holder, nullptr);
                m_given = false;
                ++m_controlTurn;
                // A pass the KVM refused on this line is none for the next one either: the driver is
                // told, and signs in anew before it hands out another. Unless it finds that it is
                // the line that the KVM refuses, under a pass it has only just given: then the
                // pass is kept, and whoever asked is told that in the driver's words.
                QString why = code;
                QString sentence = message;
                if (code == "auth_failed"_L1 && m_device != nullptr) {
                    if (const std::optional<base::Error> refuses = m_device->passWasRefused(line->pass())) {
                        why = QString::fromStdString(refuses->code);
                        sentence = QString::fromStdString(refuses->message);
                    }
                }
                m_console.release()->deleteLater();
                const QPointer<Handle> still(held);
                if (still != nullptr && leftDown) {
                    emit still->leftDown();
                }
                if (still != nullptr) {
                    emit still->controlChanged(false, why, sentence);
                }
            });
}

bool KvmdLive::giveBack(std::function<void()> whenGone)
{
    ++m_controlTurn;
    // Whoever held them is who is told, should it show only later that something may still be
    // down: the line said that all was let go, and then went without the KVM's goodbye, reset
    // when its time was up or fallen. Told then, late as that is, if that one is still there.
    // Where it is not (thrown away meanwhile, or never there: the program ending), there is
    // nobody to tell, and what is down is the KVM's to let go.
    const QPointer<Handle> held(m_holder);
    m_holder = nullptr;
    m_given = false;
    // Everything that is down is let go first: closing the line says so before it goes. And
    // the line is kept until the KVM has ended it, or its time is up and it is reset.
    return !KvmdConsoleLine::letGo(
        std::move(m_console),
        [held] {
            if (held != nullptr) {
                emit held->leftDown();
            }
        },
        std::move(whenGone));
}

void KvmdLive::tellNoControlLater(Handle* handle, const QString& code, const QString& message, bool left)
{
    QTimer::singleShot(0, handle, [handle, code, message, left] {
        const QPointer<Handle> still(handle);
        if (left) {
            emit handle->leftDown();
        }
        if (still != nullptr && still->isOpen()) {
            emit still->controlChanged(false, code, message);
        }
    });
}

bool KvmdLive::holds(const Handle* handle) const
{
    return m_holder == handle && m_console != nullptr && m_console->isOpen();
}

KvmdConsoleLine* KvmdLive::lineOf(const Handle* handle) const
{
    return m_holder == handle ? m_console.get() : nullptr;
}

Live::Pointer KvmdLive::pointer() const
{
    const std::optional<bool> absolute = m_device != nullptr ? m_device->mouseAbsolute() : std::nullopt;
    if (!absolute) {
        return Live::Pointer::Unknown;
    }
    return *absolute ? Live::Pointer::Absolute : Live::Pointer::Relative;
}

void KvmdLive::tellState()
{
    if (m_device == nullptr) {
        return;
    }
    // How the pointer is moved first, where it is set otherwise now (the KVM's own page, another program): said again,
    // as the web's line says `pointer` again when it changes.
    if (const Live::Pointer now = pointer(); now != m_pointer) {
        m_pointer = now;
        const QPointer<KvmdLive> self(this);
        const QList<QPointer<Handle>> all = handles();
        for (const QPointer<Handle>& handle : all) {
            if (self == nullptr) {
                return;
            }
            if (handle != nullptr && handle->isOpen()) {
                emit handle->pointerModeChanged();
            }
        }
    }
    const Status status = m_device->status();
    if (status.state == m_state && status.errorCode == m_errorCode && status.errorMessage == m_errorMessage) {
        return;
    }
    m_state = status.state;
    m_errorCode = status.errorCode;
    m_errorMessage = status.errorMessage;
    const QPointer<KvmdLive> self(this);
    const QList<QPointer<Handle>> all = handles();
    for (const QPointer<Handle>& handle : all) {
        if (self == nullptr) {
            return;
        }
        if (handle != nullptr && handle->isOpen()) {
            emit handle->stateChanged(m_state, m_errorCode, m_errorMessage);
        }
    }
}

} // namespace qkvm::core
