#pragma once

#include <functional>
#include <optional>

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <qkvm/base/result.h>
#include <qkvm/core/server_types.h>

#include "machine_menu.h"

namespace qkvm::ui {

// One thing done to several machines, as the page does it: a few at a time, or one after the other
// with a pause between them (machines that are switched on together draw their starting current
// together, and a storage server wants to be up first), and how each one went.
//
// What is on its way cannot be called back; what has not been sent yet is not sent once it is
// stopped. A machine that did not answer may have done it all the same: it is counted as that, and
// is not among those that are offered again. Offered again are those that were refused and those
// that were never sent.
//
// It sends nothing itself and keeps no list of what is under way where: whoever owns it hands it
// the ways to do both (Ways), so that what goes to a machine from here and what goes to it from its
// own menu know of each other. On the interface thread.
class BulkRun : public QObject {
    Q_OBJECT

public:
    struct Ways {
        // What is under way on this machine just now, by what it is called; empty: nothing is.
        std::function<QString(const QString& machine)> underWay;
        // Something of this name goes out to this machine now; and it has been answered, or not.
        std::function<void(const QString& machine, const QString& title)> began;
        std::function<void(const QString& machine)> ended;
        // Sends it, and says later how it went: exactly once, unless the sender is gone by then.
        std::function<void(const core::Machine&, const core::Action&, std::function<void(base::Result<core::Done>)>)>
            send;
        // Whether an error of what was sent to this machine leaves open that it did it all the
        // same: the word of the place it was sent to (core::WallSource), since a server and a KVM
        // do not mean the same by it.
        std::function<bool(const core::Machine&, const base::Error&)> mayHaveBeenDone;
        // It was done on this machine.
        std::function<void(const core::Machine&, const Ask&)> done;
    };

    // A test lets its seconds pass sooner: `msPerSecond` of them in a second.
    explicit BulkRun(Ways ways, int msPerSecond = 1000, QObject* parent = nullptr);

    // Runs `ask` on these machines, in this order. `gapSeconds`: nothing for a few at a time; a
    // number for one after the other, that many seconds apart. `batch` ties what is written down of
    // this run together on the server. Nothing while a run is under way.
    void start(const QList<core::Machine>& targets, const Ask& ask, std::optional<int> gapSeconds,
               const QString& batch);
    // What has not been sent yet is not sent.
    void stop();
    // Those that were refused, and those that were never sent, once more. Nothing while a run is
    // under way, or where there are none.
    void retry();
    // Forgets the run: for when nobody is there any more whose run it was. What is on its way is
    // let run, and is nobody's.
    void forget();

    bool isRunning() const { return m_running; }
    // There is a run to show: under way, or over and not forgotten.
    bool isThere() const { return !m_rows.isEmpty(); }
    // How it ended, in a sentence; empty while it runs. And whether every machine did it.
    QString told() const { return m_running ? QString() : m_summary; }
    bool allDone() const { return !m_running && m_ok == m_total; }
    QString title() const { return m_ask.title; }

    // The run as the screen takes it: {title, summary, running, stopping, again,
    //  rows: [{name, state, text}]}; empty where there is none. A row's state is the page's word for
    // it: "" (waiting or being sent), "ok", "bad", "unknown" or "skipped". `again` is what the
    // button says that sends once more what can be sent once more; empty: nothing can.
    QVariantMap shown() const;

signals:
    void changed();
    // The run is over, by itself or stopped.
    void finished();

private:
    struct Row {
        core::Machine machine;
        QString state;
        QString text;
    };

    void run(const QList<core::Machine>& list);
    // One more goes out, where one is waiting and may; where none is and none is out, the run is over.
    void next();
    void answered(const core::Machine& machine, const base::Result<core::Done>& answer);
    void mark(const QString& machine, const QString& state, const QString& text);
    void count();
    void finish();

    Ways m_ways;
    int m_msPerSecond;
    Ask m_ask;
    QString m_batch;
    std::optional<int> m_gapSeconds;
    QList<Row> m_rows;
    QList<core::Machine> m_queue;
    QList<core::Machine> m_failed;
    QList<core::Machine> m_skipped;
    int m_total = 0;
    int m_ok = 0;
    int m_unknown = 0;
    int m_out = 0;
    bool m_running = false;
    bool m_stopping = false;
    QString m_summary;
    // Raised with every run and when the run is forgotten: an answer to what was sent before that
    // only says that the machine is free again.
    quint64 m_run = 0;
    QTimer m_pause;
};

} // namespace qkvm::ui
