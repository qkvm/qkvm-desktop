#include "live_measure.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QTimer>
#include <QVariantList>

#include <qkvm/base/clock.h>
#include <qkvm/engine/types.h>

namespace qkvm::bench {

namespace {

using namespace Qt::StringLiterals;

// For so long a first picture is waited for before the report says that none came; and for so long
// after the measuring was to end, for the numbers to come that end it.
constexpr int waitForAPictureMs = 60'000;
constexpr int waitAfterTheEndMs = 30'000;

// How much a count grew between two samples. A count that fell is of another of the engine's
// streams, which counts from nought again.
qint64 grew(const QVariantMap& now, const QVariantMap& before, const QString& key)
{
    const qint64 is = now.value(key).toLongLong();
    const qint64 was = before.value(key).toLongLong();
    return is >= was ? is - was : is;
}

// The same for each place of a list of counts.
QJsonArray grewEach(const QVariantMap& now, const QVariantMap& before, const QString& key)
{
    const QVariantList is = now.value(key).toList();
    const QVariantList was = before.value(key).toList();
    QJsonArray grown;
    for (qsizetype place = 0; place < is.size(); ++place) {
        const qint64 count = is[place].toLongLong();
        const qint64 earlier = place < was.size() ? was[place].toLongLong() : 0;
        grown.append(count >= earlier ? count - earlier : count);
    }
    return grown;
}

// How many were counted over one of the steps.
qint64 countedOver(const QJsonArray& counts, std::span<const double> steps, double step)
{
    const auto from = static_cast<qsizetype>(std::ranges::find(steps, step) - steps.begin()) + 1;
    qint64 over = 0;
    for (qsizetype place = from; place < counts.size(); ++place) {
        over += counts[place].toInteger();
    }
    return over;
}

// Steps longer than two refreshes of a 60 Hz screen; pictures that took more than 12 ms (most of a
// refresh of such a screen) longer or shorter to it than the one before. As LivePicture counts them.
qint64 late(const QJsonArray& steps)
{
    return countedOver(steps, engine::gapStepsMs, 38);
}
qint64 uneven(const QJsonArray& changes)
{
    return countedOver(changes, engine::unevenStepsMs, 12);
}

// The steps between the window's frames: half of them, nine in ten and 99 in 100 are within these,
// and the longest.
QJsonObject stepsBetween(const std::vector<std::int64_t>& framesUs)
{
    std::vector<double> steps;
    for (std::size_t at = 1; at < framesUs.size(); ++at) {
        steps.push_back(static_cast<double>(framesUs[at] - framesUs[at - 1]) / 1000.0);
    }
    if (steps.empty()) {
        return {};
    }
    std::ranges::sort(steps);
    const auto within = [&steps](std::size_t ofHundred) { return steps.at(steps.size() * ofHundred / 100); };
    return {{u"half"_s, within(50)},
            {u"nineInTen"_s, within(90)},
            {u"ninetyNineInHundred"_s, within(99)},
            {u"longest"_s, steps.back()}};
}

QJsonArray listed(const QVariantList& values)
{
    return QJsonArray::fromVariantList(values);
}

} // namespace

LiveMeasure::LiveMeasure(QQuickWindow& window, ui::WallView& wall, QString file, int settleSeconds, int seconds,
                         std::function<void()> done, QObject* parent)
    : QObject(parent), m_window(window), m_wall(wall), m_file(std::move(file)),
      m_settleSeconds(std::max(0, settleSeconds)), m_seconds(std::max(1, seconds)), m_done(std::move(done))
{
    // Said by the thread that draws, once for every frame the window shows.
    connect(
        &m_window, &QQuickWindow::frameSwapped, this,
        [frames = m_frames] {
            const std::scoped_lock held(frames->guard);
            if (frames->measuring) {
                frames->us.push_back(base::steadyNowUs());
            }
        },
        Qt::DirectConnection);
    connect(&m_wall, &ui::WallView::liveDiagnosticsChanged, this, [this] {
        if (m_begun) {
            sample();
        } else if (m_settled) {
            begin();
        } else if (!m_settling && m_wall.liveDiagnostics().value(u"shown"_s).toLongLong() > 0) {
            m_settling = true;
            QTimer::singleShot(m_settleSeconds * 1000, this, [this] { m_settled = true; });
        }
    });
    QTimer::singleShot(waitForAPictureMs, this, [this] {
        if (!m_settling) {
            end();
        }
    });
}

// At the first numbers told after the settling: what is measured begins with them, and so do the
// frames counted.
void LiveMeasure::begin()
{
    m_begun = true;
    m_first = m_last = m_wall.liveDiagnostics();
    m_stream = m_wall.liveStream();
    // (The first asking gives nothing: what is measured is from here.)
    static_cast<void>(m_meter.ask());
    m_since.start();
    QTimer::singleShot(m_seconds * 1000 + waitAfterTheEndMs, this, &LiveMeasure::end);
    const std::scoped_lock held(m_frames->guard);
    m_frames->measuring = true;
}

void LiveMeasure::sample()
{
    if (m_ended) {
        return;
    }
    const QVariantMap now = m_wall.liveDiagnostics();
    if (m_wall.liveStream() != m_stream) {
        m_streamChanged = true;
    }
    const qint64 atMs = m_since.elapsed();
    m_samples.append(QJsonObject{
        {u"atMs"_s, atMs},
        {u"forMs"_s, atMs - m_lastAtMs},
        {u"received"_s, grew(now, m_last, u"receivedPictures"_s)},
        {u"shown"_s, grew(now, m_last, u"shown"_s)},
        {u"delayMs"_s, now.value(u"arrivedToShownMs"_s).toDouble()},
        {u"waitedMs"_s, now.value(u"waitedMs"_s).toDouble()},
        {u"decodeMs"_s, now.value(u"decodeMs"_s).toDouble()},
        {u"readyToDrawnMs"_s, now.value(u"readyToDrawnMs"_s).toDouble()},
        {u"drawnToShownMs"_s, now.value(u"drawnToShownMs"_s).toDouble()},
        {u"lateOnScreen"_s, late(grewEach(now, m_last, u"shownGaps"_s))},
        {u"lateArriving"_s, late(grewEach(now, m_last, u"arrivalGaps"_s))},
        {u"uneven"_s, uneven(grewEach(now, m_last, u"uneven"_s))},
        {u"decoding"_s, now.value(u"decoding"_s).toString()},
    });
    m_last = now;
    m_lastAtMs = atMs;
    if (atMs >= qint64{m_seconds} * 1000) {
        end();
    }
}

void LiveMeasure::end()
{
    if (m_ended) {
        return;
    }
    m_ended = true;
    std::vector<std::int64_t> frames;
    {
        const std::scoped_lock held(m_frames->guard);
        m_frames->measuring = false;
        frames = std::move(m_frames->us);
    }
    QJsonObject report;
    const QVariantMap& last = m_last;
    if (!m_begun) {
        report.insert(u"problem"_s, u"no live picture reached the screen"_s);
        report.insert(u"diagnostics"_s, QJsonObject::fromVariantMap(m_wall.liveDiagnostics()));
    } else {
        const double seconds = std::max(0.001, static_cast<double>(m_lastAtMs) / 1000.0);
        const QJsonArray shownSteps = grewEach(last, m_first, u"shownGaps"_s);
        const QJsonArray arrivalSteps = grewEach(last, m_first, u"arrivalGaps"_s);
        const QJsonArray changes = grewEach(last, m_first, u"uneven"_s);
        const qint64 shown = grew(last, m_first, u"shown"_s);
        const qint64 received = grew(last, m_first, u"receivedPictures"_s);
        // The means of what each second said (each itself a running mean of the last ten or so).
        const auto meanOf = [this](const QString& key) {
            double all = 0;
            for (const auto& one : std::as_const(m_samples)) {
                all += one.toObject().value(key).toDouble();
            }
            return m_samples.isEmpty() ? 0.0 : all / static_cast<double>(m_samples.size());
        };
        const QScreen* screen = m_window.screen();
        const Cost cost = m_meter.ask();
        report = QJsonObject{
            {u"seconds"_s, seconds},
            {u"codec"_s, last.value(u"codec"_s).toString()},
            {u"decoding"_s, last.value(u"decoding"_s).toString()},
            {u"picture"_s, QJsonArray{last.value(u"width"_s).toInt(), last.value(u"height"_s).toInt()}},
            {u"drawnPixelsWide"_s, m_wall.enlargedPixels()},
            {u"screenHz"_s, screen != nullptr ? screen->refreshRate() : 0.0},
            {u"windowActive"_s, m_window.isActive()},
            {u"received"_s, received},
            {u"shown"_s, shown},
            {u"receivedPerSecond"_s, static_cast<double>(received) / seconds},
            {u"shownPerSecond"_s, static_cast<double>(shown) / seconds},
            {u"replaced"_s, grew(last, m_first, u"replaced"_s)},
            {u"droppedBehind"_s, grew(last, m_first, u"dropped"_s)},
            {u"refused"_s, grew(last, m_first, u"refused"_s)},
            {u"gapStepsMs"_s, listed(last.value(u"gapStepsMs"_s).toList())},
            {u"arrivalSteps"_s, arrivalSteps},
            {u"shownSteps"_s, shownSteps},
            {u"lateArriving"_s, late(arrivalSteps)},
            {u"lateOnScreen"_s, late(shownSteps)},
            {u"unevenStepsMs"_s, listed(last.value(u"unevenStepsMs"_s).toList())},
            {u"unevenCounts"_s, changes},
            {u"uneven"_s, uneven(changes)},
            {u"arrivedToShownStepsMs"_s, listed(last.value(u"arrivedToShownStepsMs"_s).toList())},
            {u"arrivedToShown"_s, grewEach(last, m_first, u"arrivedToShown"_s)},
            {u"delayMeanMs"_s, meanOf(u"delayMs"_s)},
            {u"waitedMeanMs"_s, meanOf(u"waitedMs"_s)},
            {u"decodeMeanMs"_s, meanOf(u"decodeMs"_s)},
            {u"readyToDrawnMeanMs"_s, meanOf(u"readyToDrawnMs"_s)},
            {u"drawnToShownMeanMs"_s, meanOf(u"drawnToShownMs"_s)},
            {u"readyToDrawn"_s, grewEach(last, m_first, u"readyToDrawn"_s)},
            {u"drawnToShown"_s, grewEach(last, m_first, u"drawnToShown"_s)},
            {u"longestDelayEverMs"_s, last.value(u"arrivedToShownMaxMs"_s).toDouble()},
            {u"longestStepOnScreenEverMs"_s, last.value(u"shownGapMaxMs"_s).toDouble()},
            {u"windowFrames"_s, static_cast<qint64>(frames.size())},
            {u"windowFrameStepsMs"_s, stepsBetween(frames)},
            {u"processorPercent"_s, cost.processorPercent},
            {u"cardDecodingPercent"_s, cost.decodingPercent},
            {u"cardDrawingPercent"_s, cost.drawingPercent},
            {u"committedMb"_s, static_cast<double>(cost.committedBytes) / (1024.0 * 1024.0)},
            {u"videoMb"_s, static_cast<double>(cost.videoBytes) / (1024.0 * 1024.0)},
            {u"streamChanged"_s, m_streamChanged},
            {u"endedEarly"_s, m_lastAtMs < qint64{m_seconds} * 1000},
            {u"samples"_s, m_samples},
        };
    }
    if (QFile out(m_file); out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    }
    if (m_done) {
        m_done();
    }
}

} // namespace qkvm::bench
