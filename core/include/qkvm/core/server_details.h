#pragma once

#include <optional>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <qkvm/base/result.h>

namespace qkvm::core {

struct HistorySummary {
    double since = 0;
    double until = 0;
    double watched = 0;
    double online = 0;
    std::optional<double> share;
    std::optional<double> firstSeen;
    std::optional<double> lastSeen;
    QString state;
    int changes = 0;
    int events = 0;
    QStringList sensors;
};

struct HistoryPoint {
    double at = 0;
    double value = 0;
    double low = 0;
    double high = 0;
};

struct HistorySeries {
    QString sensor;
    QString unit;
    QList<HistoryPoint> points;
};

struct HistorySamples {
    double since = 0;
    double until = 0;
    QList<HistorySeries> series;
    bool more = false;
};

struct ActivityEntry {
    double at = 0;
    QString user;
    QString event;
    QString name;
    QString action;
    QString state;
    QString was;
    std::optional<bool> on;
    QString from;
    QString by;
    QString why;
    // An alert of a machine's screen watch (b7): which ("no_signal", "unchanged", or a word this program
    // does not know, one added since, as the server said it; empty: none), after how many minutes, and
    // whether it is over ("back").
    QString watch;
    std::optional<int> minutes;
    bool back = false;
    // The picture that set it off, by its stamp, where one was kept (GET .../alert-picture/{stamp},
    // for an admin); and when the wall saw it, in seconds since 1970. Empty: none (also where the
    // stamp is not of the server's shape: it goes into an address).
    QString picture;
    std::optional<double> pictureAt;
    // The sentence the server wrote of it, where it wrote one: its `error`, else its `note`, as the page
    // shows them. (Read only where it is text; nothing else of the entry hangs on it.)
    QString said;
};

struct DeviceActivity {
    QList<ActivityEntry> entries;
    bool more = false;
    int missing = 0;
};

// The existing server history/audit contracts, bounded before use by the UI.
// Unknown fields are ignored; arbitrary audit fields never enter the view model.
base::Result<HistorySummary> historySummaryFrom(const QByteArray& bytes);
base::Result<HistorySamples> historySamplesFrom(const QByteArray& bytes);
base::Result<DeviceActivity> deviceActivityFrom(const QByteArray& bytes);

} // namespace qkvm::core
