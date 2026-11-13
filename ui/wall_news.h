#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QString>

#include "wall_model.h"

namespace qkvm::ui {

// What is known of one machine of the wall: what the bar counted it under when that was last
// known ("online", "no_signal" or "attention"), its name, and how it was in a word ("Offline",
// "Login failed").
struct Looked {
    QString as;
    QString name;
    QString label;

    bool operator==(const Looked&) const = default;
};

// What is known of the wall's machines, by which machine. A machine that is only being connected
// to is not known to be any way: it keeps what was known of it before, or is not in here yet.
using WallLook = QHash<QString, Looked>;

// Takes the wall as it is now into what is known: every machine that is some way, as it is; and
// nothing more of a machine that is no longer on the wall.
void remember(WallLook& known, const WallModel& now);

// A machine there is news of: which, its name, and how it is now.
struct Told {
    QString key;
    QString name;
    QString label;

    bool operator==(const Told&) const = default;
};

// What became of the wall's machines, for somebody who is not looking at the wall: which went
// away (they want attention now, and did not when they were last known to be some way) and which
// came back (they wanted attention, and do not any more).
//
// No news: a machine that is new on the wall or gone from it; a machine of which nothing was
// known yet; a machine that is being connected to, as every one is for a while after QKVM was
// started again; and a computer that went to sleep or woke up, which is every evening and
// every morning.
struct WallNews {
    QList<Told> away;
    QList<Told> back;

    bool isEmpty() const { return away.isEmpty() && back.isEmpty(); }
    bool operator==(const WallNews&) const = default;
};

// What is news between what was known and the wall as it is now, in the wall's order.
WallNews newsBetween(const WallLook& known, const WallModel& now);

// Puts later news to earlier news that nobody was told yet. A machine that went away and is back
// before anybody was told, or the other way round, is no news any more.
void add(WallNews& soFar, const WallNews& later);

// News as Windows shows it: a heading, a line under it, and whether it is a warning.
struct Message {
    QString title;
    QString text;
    bool isWarning = false;

    bool operator==(const Message&) const = default;
};

// Nothing where there is no news. One machine is named in the heading with how it is under it;
// several are counted in the heading and named under it, as many as there is room for.
std::optional<Message> messageOf(const WallNews& news);

} // namespace qkvm::ui
