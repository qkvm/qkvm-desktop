#pragma once

#include <functional>

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

namespace qkvm::core {

// Local observations, not the server's audit log. No address fields, credentials or server-provided
// error text are retained. Display names can contain a host when no name was given. Only fresh
// completed status observations count toward settling.
class Incidents final : public QObject {
    Q_OBJECT
public:
    struct Observation {
        QString key;
        QString name;
        QString state;
    };
    struct Entry {
        QString id;
        QString scope;
        QString key;
        QString name;
        QString state;
        QDateTime opened;
        QDateTime changed;
        QDateTime closed;
        bool acknowledged = false;
        bool removed = false;
        // Of a screen watch's alert (screenAlert): what the watch waits, in minutes.
        int minutes = 0;
    };
    struct Change {
        QString key;
        QString name;
        QString state;
    };
    static constexpr int mostTracked = 10000;
    static constexpr int mostClosed = 1000;
    static constexpr qint64 mostBytes = 16LL * 1024 * 1024;

    explicit Incidents(QString file, std::function<QDateTime()> clock = QDateTime::currentDateTimeUtc,
                       QObject* parent = nullptr);
    ~Incidents() override;
    // Restored observations get the web watcher's two-minute startup/reconnect grace.
    void begin(const QString& scope);
    QList<Change> observe(const QString& scope, const QList<Observation>& observations);
    // A fresh authoritative inventory: missing devices cease monitoring, without pretending recovery.
    void reconcile(const QString& scope, const QSet<QString>& keys);
    QList<Entry> entries(const QString& scope) const;
    bool acknowledge(const QString& scope, const QString& id);
    void pause(const QString& scope, const QString& key, bool paused);
    QDateTime pausedUntil(const QString& scope, const QString& key = {}) const;
    bool flush();
    bool repair();
    bool damaged() const { return m_damaged; }
    QString problem() const { return m_problem; }
    QDateTime now() const { return m_clock().toUTC(); }
    static bool knownState(const QString& state);
    // A screen watch of this PC's own wall went off, or is over (`back`; screen_watch.h): an entry of its own, under
    // the KVM's key, a "#" and the watch ("no_signal", "unchanged"), in the state "screen-" and the watch; told at
    // once, with no settling (the watch waited its minutes). Whether it is to be told now: not while it, its KVM or the
    // scope are paused.
    bool screenAlert(const QString& scope, const QString& key, const QString& name, const QString& watch, bool back,
                     int minutes);
    // The key of a screen watch's entries, and whether an entry's state is a screen watch's.
    static QString screenKey(const QString& key, const QString& watch) { return key + u'#' + watch; }
    static bool isScreenState(const QString& state);

signals:
    void changed();

private:
    struct Watch {
        QString scope;
        QString key;
        QString name;
        QString stable;
        QString pending;
        QString held;
        int count = 0;
        bool restored = true;
        QDateTime muted;
    };
    void load();
    void dirty();
    void transition(const Watch& watch);
    void trim();
    QString m_file;
    std::function<QDateTime()> m_clock;
    QHash<QString, Watch> m_watches;
    QHash<QString, QDateTime> m_pauses;
    QHash<QString, QDateTime> m_grace;
    QList<Entry> m_entries;
    QTimer m_write;
    QString m_problem;
    bool m_dirty = false;
    bool m_damaged = false;
};

} // namespace qkvm::core
