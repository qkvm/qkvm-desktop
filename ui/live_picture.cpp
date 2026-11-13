#include "live_picture.h"
#include <qkvm/core/video_diagnostics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include <QPointer>

#include <qkvm/base/clock.h>
#include <qkvm/core/tls.h>
#include <qkvm/engine/types.h>

namespace qkvm::ui {

namespace {

// As many pictures a second as a machine's screen is worth looking at with.
constexpr int picturesASecond = 30;

// For so long the engine is asked, picture or no picture, whether a stream's first one is made;
// after that only when the next one is handed in.
constexpr int looksInARow = 200;

// The words with which the place a wall's machines come from, or this program for it, says that
// there is no live picture to be had here: of this machine, by whoever asks, or in a way this
// program knows; or that this copy of the program speaks to nothing over HTTPS (tls.h), which
// is so for as long as it runs. Asking again would get the same answer. And a server's goodbyes
// that end a line for good until a person does something (signed out, not confirmed by the
// identity proxy, the machine changed, a line that broke the rules: Part D of 2026-10-26; a word
// this program does not know is the line's own to tell, Live::endsForGood). And a JetKVM whose one
// viewer somebody else took ("in_use", M6 2h): only "Show here" takes it back (openAgain).
bool isANo(std::string_view code)
{
    constexpr std::array<std::string_view, 12> words = {"no_live",   "not_found",      "not_allowed", "forbidden",
                                                        "live_form", core::tlsDamaged, "signed_out",  "unconfirmed",
                                                        "gone",      "flooding",       "bad_message", "in_use"};
    return std::ranges::find(words, code) != words.end();
}

// The engine's counts of steps between pictures, and the steps they are counted in, for QML.
QVariantList countsOf(std::span<const std::uint64_t> counts)
{
    QVariantList listed;
    for (const std::uint64_t count : counts) {
        listed.append(QVariant::fromValue(static_cast<quint64>(count)));
    }
    return listed;
}

QVariantList stepsOf(std::span<const double> steps)
{
    QVariantList listed;
    for (const double step : steps) {
        listed.append(step);
    }
    return listed;
}

// How many were counted over one of the steps.
qint64 countedOver(std::span<const std::uint64_t> counts, std::span<const double> steps, double step)
{
    const auto from = static_cast<std::size_t>(std::ranges::find(steps, step) - steps.begin()) + 1;
    std::uint64_t over = 0;
    for (std::size_t place = from; place < counts.size(); ++place) {
        over += counts[place];
    }
    return static_cast<qint64>(over);
}

} // namespace

LivePicture::LivePicture(Around around, Pace pace, QObject* parent)
    : QObject(parent), m_around(std::move(around)), m_pace(pace), m_waitMs(pace.againAfterMs)
{
    m_statistics.setInterval(std::max(1, m_pace.statisticsEveryMs));
    connect(&m_statistics, &QTimer::timeout, this, [this] {
        passOnAWish();
        publishDiagnostics();
    });
    m_again.setSingleShot(true);
    connect(&m_again, &QTimer::timeout, this, &LivePicture::ask);
    m_looking.setSingleShot(true);
    m_looking.setInterval(m_pace.lookEveryMs);
    connect(&m_looking, &QTimer::timeout, this, &LivePicture::look);
    m_makingWay.setSingleShot(true);
    m_makingWay.setInterval(m_pace.makeWayAfterMs);
    connect(&m_makingWay, &QTimer::timeout, this, [this] {
        drop(m_held);
        emit changed();
    });
}

LivePicture::~LivePicture()
{
    // The line first: what it still says finds nobody.
    m_link.reset();
    drop(m_fed);
    drop(m_held);
}

core::Live::Need LivePicture::need() const
{
    return {std::max(1, m_pixels), picturesASecond};
}

engine::Stream* LivePicture::streamOf(const Kept& kept) const
{
    // (An engine that has ended took its streams with it.)
    return kept.stream != nullptr && kept.scene == m_around.scene() ? kept.stream : nullptr;
}

void LivePicture::tellTheSize()
{
    for (const Kept* kept : {&m_fed, &m_held}) {
        if (engine::Stream* stream = streamOf(*kept); stream != nullptr) {
            stream->shownAt({m_pixels, m_pixels * 9 / 16}, engine::Importance::ShownLarge);
        }
    }
}

void LivePicture::show(const QString& machine, int pixels, bool live)
{
    m_pixels = pixels;
    if (machine != m_machine) {
        // Another machine, or none: nothing of the one before stays up. And where this one is
        // had live from is what its key reaches now, for as long as it stays shown.
        m_machine = machine;
        m_at = m_machine.isEmpty() ? Reached{} : m_around.place(m_machine);
        m_stoppedOnANo = false;
        m_letGoEarlier = 0;
        m_letGoOfLine = 0;
        m_makingWay.stop();
        drop(m_held);
        holdWhatWasFed(false);
        resetDiagnostics();
    }
    if (live && !machine.isEmpty()) {
        if (!m_statistics.isActive()) {
            m_statistics.start();
        }
    } else {
        m_statistics.stop();
    }
    const QString lineFor = live ? machine : QString();
    if (lineFor != m_lineFor) {
        core::VideoDiagnostics::record(
            "live_requested",
            {{"enabled", live}, {"has_machine", !machine.isEmpty()}, {"has_source", static_cast<bool>(m_at)}});
        ++m_turn;
        endLine();
        m_lineFor = lineFor;
        m_stoppedOnANo = false;
        m_waitMs = m_pace.againAfterMs;
        m_again.stop();
        if (!m_lineFor.isEmpty()) {
            m_again.start(m_pace.beginAfterMs);
        }
        publishDiagnostics();
        return;
    }
    if (m_link != nullptr) {
        m_link->setNeed(need());
    }
    tellTheSize();
    publishDiagnostics();
}

void LivePicture::ask()
{
    // (The line is for the machine that is shown. One whose key reached nothing has no place
    // that could have it live.)
    if (!m_at) {
        return;
    }
    const auto open = m_around.open ? m_around.open : OpenLive([](const Reached& at, core::Live::Need need, auto done) {
        at.place->openLive(at.machine, need, std::move(done));
    });
    open(m_at, need(),
         [self = QPointer<LivePicture>(this), turn = m_turn](base::Result<std::unique_ptr<core::Live>> live) {
             // (A line for a machine that is asked for no more is nobody's: it ends
             // here, with what was handed over, before it has said anything.)
             if (self == nullptr || turn != self->m_turn) {
                 return;
             }
             if (live) {
                 self->hold(std::move(live).value());
             } else if (!isANo(live.error().code)) {
                 // Not reached, or refused without a word that says there is none to be had.
                 self->tryAgainLater();
             } else {
                 self->m_stoppedOnANo = true;
             }
         });
}

void LivePicture::openAgain(const QString& machine)
{
    if (machine.isEmpty() || machine != m_lineFor || !m_stoppedOnANo || m_link != nullptr) {
        return;
    }
    m_stoppedOnANo = false;
    m_waitMs = m_pace.againAfterMs;
    m_again.start(0);
}

void LivePicture::hold(std::unique_ptr<core::Live> live)
{
    m_link = std::move(live);
    resetDiagnostics();
    connect(m_link.get(), &core::Live::opened, this, &LivePicture::publishDiagnostics);
    connect(m_link.get(), &core::Live::transportChanged, this, &LivePicture::publishDiagnostics);
    connect(m_link.get(), &core::Live::videoChanged, this, [this](const core::Live::Video& video) { m_video = video; });
    connect(m_link.get(), &core::Live::pictureCame, this, &LivePicture::take);
    connect(m_link.get(), &core::Live::closed, this,
            [this](const QString& code, const QString& message) { lineEnded(code, message); });
    emit liveChanged();
    publishDiagnostics();
}

void LivePicture::take(const core::Live::Picture& picture)
{
    const bool first = !m_hasReceived;
    m_hasReceived = true;
    m_receivedH264 = picture.h264;
    m_receivedChanges = picture.changes;
    ++m_receivedPictures;
    m_receivedBytes += static_cast<quint64>(picture.data.size());
    m_lastReceived.start();
    // The first picture, or the first of another kind than the ones before: a stream for it.
    // What was drawn until now stays up until the engine has made a picture of this one.
    if (m_fed.number == 0 || picture.h264 != m_fedIsH264 || picture.changes != m_fedIsChanges) {
        holdWhatWasFed(true);
        m_fedIsH264 = picture.h264;
        m_fedIsChanges = picture.changes;
        m_fed.number = m_around.number();
        m_fed.scene = m_around.scene();
        if (m_fed.scene != nullptr) {
            // (A server's screen comes as what changed of it, which the engine applies to the screen as it stands.)
            m_fed.stream = &m_fed.scene->add(m_fed.number, picture.changes ? engine::Codec::Rgb
                                                           : picture.h264  ? engine::Codec::H264
                                                                           : engine::Codec::Still);
            m_fed.stream->shownAt({m_pixels, m_pixels * 9 / 16}, engine::Importance::ShownLarge);
        }
        m_looks = 0;
        m_lastDecoded = 0;
        m_reportedDecodeFailure = false;
        m_decodeProgress.start();
    }
    if (engine::Stream* stream = streamOf(m_fed); stream != nullptr) {
        // Arrived when its giver stamped it as it became whole, where one did (a WebRTC receiver, before
        // the picture waited for this thread); otherwise now (core::Live::Picture::arrivedUs).
        const qint64 arrived = picture.arrivedUs != 0 ? picture.arrivedUs : base::steadyNowUs();
        stream->push({std::as_bytes(std::span(picture.data.constData(), static_cast<std::size_t>(picture.data.size()))),
                      arrived, picture.keyFrame});
        // Whoever decodes lost its place: the server is asked for a picture to begin at.
        if (stream->takeKeyFrameWish()) {
            m_link->askForAKeyFrame();
        }
    }
    if (!m_fedIsDrawn) {
        look();
    }
    if (first) {
        publishDiagnostics();
    }
}

void LivePicture::look()
{
    // (A window without an engine draws nothing: what was fed counts as drawn.)
    const engine::Stream* stream = streamOf(m_fed);
    if (stream != nullptr && stream->stats().decoded == 0) {
        if (++m_looks <= looksInARow) {
            m_looking.start();
        } else if (m_fedIsH264 && m_link != nullptr) {
            m_link->videoFailed();
        }
        return;
    }
    m_fedIsDrawn = true;
    m_drawnSince.start();
    m_looking.stop();
    m_makingWay.stop();
    drop(m_held);
    emit changed();
}

void LivePicture::holdWhatWasFed(bool keep)
{
    m_looking.stop();
    if (keep && m_fedIsDrawn) {
        m_held = m_fed;
        m_fed = {};
    } else {
        drop(m_fed);
    }
    m_fedIsDrawn = false;
}

void LivePicture::lineEnded(const QString& code, const QString& message)
{
    // endLine/destruction may already own the line when a backend says its final word.
    if (!m_link) {
        return;
    }
    // (Not thrown away from inside what it says. Whoever sent keys through it hears that it is
    // none of this one's any more; and first why it ended: this one hears of the end before they
    // do, and with the next word they stop listening to it.)
    std::unique_ptr<core::Live> gone = std::move(m_link);
    keepWhatItLetGo(*gone);
    const bool forGood = isANo(code.toStdString()) || gone->endsForGood(code);
    emit lineEndedBecause(message);
    emit liveChanged();
    gone.release()->deleteLater();
    const bool wanted = wantsSnapshots();
    // A line that held for long was a good one: whatever went wrong before it is over.
    if (m_fedIsDrawn && m_drawnSince.elapsed() >= m_pace.longestMs) {
        m_waitMs = m_pace.againAfterMs;
    }
    holdWhatWasFed(true);
    if (!forGood) {
        tryAgainLater();
    } else {
        m_stoppedOnANo = true;
    }
    if (wantsSnapshots() != wanted) {
        emit changed();
    }
    publishDiagnostics();
}

void LivePicture::keepWhatItLetGo(const core::Live& line)
{
    // (A line of the machine shown before, ended as another is shown, counts for nothing here.)
    if (m_lineFor == m_machine) {
        m_letGoEarlier += std::max(m_letGoOfLine, line.picturesLetGo());
    }
    m_letGoOfLine = 0;
}

void LivePicture::endLine()
{
    if (m_link != nullptr) {
        // Whoever sends keys through it lets go of them there, while it stands: then it is ended.
        std::unique_ptr<core::Live> going = std::move(m_link);
        keepWhatItLetGo(*going);
        emit liveChanged();
        going->close();
        going.release()->deleteLater();
    }
    holdWhatWasFed(!m_machine.isEmpty());
    publishDiagnostics();
}

void LivePicture::snapshotCame()
{
    // Given a moment to be drawn, so that what it takes the place of is never what stood in its
    // card before it.
    if (m_held.number != 0 && !m_fedIsDrawn && !m_makingWay.isActive()) {
        m_makingWay.start();
    }
}

void LivePicture::drop(Kept& kept)
{
    if (streamOf(kept) != nullptr) {
        kept.scene->remove(kept.number);
    }
    kept = {};
}

void LivePicture::passOnAWish()
{
    // A wish that came up while decoding, after the picture it was decoding was handed in, also
    // where no picture comes after it (a screen that does not change): asked for now.
    if (engine::Stream* stream = streamOf(m_fed);
        stream != nullptr && m_link != nullptr && stream->takeKeyFrameWish()) {
        m_link->askForAKeyFrame();
    }
}

void LivePicture::tryAgainLater()
{
    m_again.start(m_waitMs);
    m_waitMs = std::min(m_waitMs * 2, m_pace.longestMs);
}

void LivePicture::resetDiagnostics()
{
    m_video = {};
    m_hasReceived = false;
    m_receivedH264 = false;
    m_receivedChanges = false;
    m_receivedPictures = 0;
    m_receivedBytes = 0;
    m_ratePictures = 0;
    m_rateBytes = 0;
    m_receivedFps = 0;
    m_receivedMbps = 0;
    m_rateNumber = 0;
    m_rateShown = 0;
    m_shownFps = 0;
    m_shownRateKnown = false;
    m_rateKnown = false;
    m_lastReceived.invalidate();
    m_rateSince.start();
}

void LivePicture::publishDiagnostics()
{
    using namespace Qt::StringLiterals;
    QVariantMap next;
    if (!m_machine.isEmpty()) {
        const qint64 elapsed = m_rateSince.isValid() ? m_rateSince.elapsed() : 0;
        const bool rateNow = elapsed >= std::max(1, m_pace.statisticsEveryMs);
        if (rateNow) {
            m_receivedFps =
                static_cast<double>(m_receivedPictures - m_ratePictures) * 1000.0 / static_cast<double>(elapsed);
            m_receivedMbps =
                static_cast<double>(m_receivedBytes - m_rateBytes) * 8.0 / (static_cast<double>(elapsed) * 1000.0);
            m_rateKnown = true;
            m_ratePictures = m_receivedPictures;
            m_rateBytes = m_receivedBytes;
            m_rateSince.restart();
        }
        next.insert(u"transport"_s, m_link != nullptr ? m_link->transport() : QString{});
        next.insert(u"fallbackReason"_s, m_link != nullptr ? m_link->fallbackReason() : QString{});
        next.insert(u"connected"_s, m_link != nullptr && m_link->isOpen());
        next.insert(u"hasPicture"_s, m_hasReceived);
        next.insert(u"codec"_s, !m_hasReceived      ? u"unknown"_s
                                : m_receivedChanges ? u"rgb"_s
                                : m_receivedH264    ? u"h264"_s
                                                    : u"jpeg"_s);
        next.insert(u"receivedPictures"_s, m_receivedPictures);
        next.insert(u"receivedBytes"_s, m_receivedBytes);
        next.insert(u"rateKnown"_s, m_rateKnown);
        next.insert(u"receivedFps"_s, m_receivedFps);
        next.insert(u"receivedMbps"_s, m_receivedMbps);
        next.insert(u"lastReceivedMs"_s, m_lastReceived.isValid() ? m_lastReceived.elapsed() : qint64{-1});
        // The engine can end before this object. Never use the old stream pointer then, and
        // never infer hardware decoding from the codec or the user's preference.
        const Kept& counted = m_fed.number != 0 ? m_fed : m_held;
        const engine::Stream* decoded = m_hasReceived ? streamOf(counted) : nullptr;
        const quint32 number = decoded != nullptr ? counted.number : 0;
        const engine::Stats stats = decoded != nullptr ? decoded->stats() : engine::Stats{};
        if (rateNow) {
            // Of another stream than the count before, or of one that counts from nought again:
            // no rate for this while, and counted from here. A still let go as the same as the one on
            // the screen counts as shown: the screen shows it (a still screen's JPEGs, since M6 2c).
            const std::uint64_t onTheScreen = stats.shown + stats.stillsSame;
            const bool same = number != 0 && number == m_rateNumber && onTheScreen >= m_rateShown;
            m_shownFps =
                same ? static_cast<double>(onTheScreen - m_rateShown) * 1000.0 / static_cast<double>(elapsed) : 0;
            m_shownRateKnown = same;
            m_rateNumber = number;
            m_rateShown = onTheScreen;
        }
        if (stats.decoded != m_lastDecoded) {
            m_lastDecoded = stats.decoded;
            m_decodeProgress.start();
        }
        if (!m_reportedDecodeFailure && m_receivedH264 && m_link != nullptr && m_link->transport() == u"webrtc"_s &&
            m_decodeProgress.isValid() && m_decodeProgress.elapsed() > 5000 && m_lastReceived.isValid() &&
            m_lastReceived.elapsed() < 2000) {
            m_reportedDecodeFailure = true;
            // This may change transport and publish diagnostics again. Avoid overwriting
            // that newer state with the values sampled before the fallback.
            m_link->videoFailed();
            return;
        }
        const bool knownSize = m_hasReceived && m_video.h264 == m_receivedH264 && m_video.changes == m_receivedChanges;
        next.insert(u"width"_s, stats.picture.width > 0 ? stats.picture.width : (knownSize ? m_video.width : 0));
        next.insert(u"height"_s, stats.picture.height > 0 ? stats.picture.height : (knownSize ? m_video.height : 0));
        next.insert(u"decoding"_s, stats.decoding == engine::Decoding::OnTheCard    ? u"gpu"_s
                                   : stats.decoding == engine::Decoding::InSoftware ? u"software"_s
                                                                                    : u"unknown"_s);
        next.insert(u"engineKnown"_s, decoded != nullptr);
        next.insert(u"decoded"_s, QVariant::fromValue(static_cast<quint64>(stats.decoded)));
        // Dropped behind: by the engine, and before it by the live's own mailbox (Live::picturesLetGo), of
        // every line of this machine since it was shown here, never less than said before.
        if (m_link != nullptr) {
            m_letGoOfLine = std::max(m_letGoOfLine, m_link->picturesLetGo());
        }
        const quint64 letGo = m_letGoEarlier + m_letGoOfLine;
        next.insert(u"dropped"_s, QVariant::fromValue(static_cast<quint64>(stats.droppedBehind) + letGo));
        next.insert(u"refused"_s, QVariant::fromValue(static_cast<quint64>(stats.refused)));
        next.insert(u"replaced"_s, QVariant::fromValue(static_cast<quint64>(stats.replaced)));
        next.insert(u"decodeMs"_s, stats.decodeMs);
        // What reached the screen, and how evenly (engine::Stats says what each count is).
        next.insert(u"shown"_s, QVariant::fromValue(static_cast<quint64>(stats.shown)));
        next.insert(u"shownFps"_s, m_shownFps);
        // Known once a while has passed with one stream and one count: before that, and for the
        // while after another stream came, the rate said is no measurement.
        next.insert(u"shownRateKnown"_s, m_shownRateKnown && number != 0 && number == m_rateNumber);
        next.insert(u"arrivedToShownMs"_s, stats.arrivedToShownMs);
        next.insert(u"arrivedToShownMaxMs"_s, stats.arrivedToShownMaxMs);
        next.insert(u"shownGapMaxMs"_s, stats.shownGapMaxMs);
        next.insert(u"shownGaps"_s, countsOf(stats.shownGapCounts));
        next.insert(u"arrivalGaps"_s, countsOf(stats.arrivalGapCounts));
        next.insert(u"gapStepsMs"_s, stepsOf(engine::gapStepsMs));
        next.insert(u"arrivedToShown"_s, countsOf(stats.arrivedToShownCounts));
        next.insert(u"arrivedToShownStepsMs"_s, stepsOf(engine::arrivedToShownStepsMs));
        next.insert(u"uneven"_s, countsOf(stats.unevenCounts));
        // Where the time from arriving to the screen goes (engine::Stats says what each part is).
        next.insert(u"waitedMs"_s, stats.waitedMs);
        next.insert(u"readyToDrawnMs"_s, stats.readyToDrawnMs);
        next.insert(u"drawnToShownMs"_s, stats.drawnToShownMs);
        next.insert(u"readyToDrawn"_s, countsOf(stats.readyToDrawnCounts));
        next.insert(u"drawnToShown"_s, countsOf(stats.drawnToShownCounts));
        next.insert(u"unevenStepsMs"_s, stepsOf(engine::unevenStepsMs));
        // Steps longer than two refreshes of a 60 Hz screen, on it and as they arrived; and pictures
        // that took more than 12 ms (most of a refresh of such a screen) longer or shorter to it
        // than the one before.
        next.insert(u"lateSteps"_s, countedOver(stats.shownGapCounts, engine::gapStepsMs, 38));
        next.insert(u"sourceLateSteps"_s, countedOver(stats.arrivalGapCounts, engine::gapStepsMs, 38));
        next.insert(u"unevenSteps"_s, countedOver(stats.unevenCounts, engine::unevenStepsMs, 12));
    }
    if (!m_machine.isEmpty() && (!m_diagnosticSince.isValid() || m_diagnosticSince.elapsed() >= 1000)) {
        m_diagnosticSince.start();
        core::VideoDiagnostics::record(
            "video_sample",
            {{"transport",
              QStringList{u"connecting"_s, u"webrtc"_s, u"mjpeg"_s, u"snapshots"_s, u"reconnecting"_s}.indexOf(
                  next.value(u"transport"_s).toString())},
             {"h264", next.value(u"codec"_s).toString() == u"h264"_s},
             {"decoder",
              QStringList{u"unknown"_s, u"gpu"_s, u"software"_s}.indexOf(next.value(u"decoding"_s).toString())},
             {"received", next.value(u"receivedPictures"_s).toLongLong()},
             {"decoded", next.value(u"decoded"_s).toLongLong()},
             {"age_ms", next.value(u"lastReceivedMs"_s).toLongLong()},
             {"width", next.value(u"width"_s).toLongLong()},
             {"height", next.value(u"height"_s).toLongLong()},
             {"dropped", next.value(u"dropped"_s).toLongLong()},
             {"refused", next.value(u"refused"_s).toLongLong()},
             {"shown", next.value(u"shown"_s).toLongLong()},
             {"delay_ms", std::llround(next.value(u"arrivedToShownMs"_s).toDouble())},
             {"late_steps", next.value(u"lateSteps"_s).toLongLong()},
             {"source_late_steps", next.value(u"sourceLateSteps"_s).toLongLong()},
             {"uneven_steps", next.value(u"unevenSteps"_s).toLongLong()}});
    }
    if (m_diagnostics != next) {
        m_diagnostics = std::move(next);
        emit diagnosticsChanged();
    }
}

} // namespace qkvm::ui
