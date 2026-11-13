#include "bulk_run.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QPointer>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// As many as go out at once where nothing else was asked for.
constexpr qsizetype aFewAtATime = 4;

QString words(const char* text)
{
    return QCoreApplication::translate("Actions", text);
}

} // namespace

BulkRun::BulkRun(Ways ways, int msPerSecond, QObject* parent)
    : QObject(parent), m_ways(std::move(ways)), m_msPerSecond(msPerSecond)
{
    m_pause.setSingleShot(true);
    connect(&m_pause, &QTimer::timeout, this, &BulkRun::next);
}

void BulkRun::start(const QList<core::Machine>& targets, const Ask& ask, std::optional<int> gapSeconds,
                    const QString& batch)
{
    if (m_running || targets.isEmpty()) {
        return;
    }
    m_ask = ask;
    m_batch = batch;
    m_gapSeconds = gapSeconds;
    m_rows.clear();
    for (const core::Machine& machine : targets) {
        m_rows.append(Row{machine, QString(), QString()});
    }
    run(targets);
}

void BulkRun::retry()
{
    if (m_running || (m_failed.isEmpty() && m_skipped.isEmpty())) {
        return;
    }
    // In the order they were in.
    QList<core::Machine> again;
    for (const Row& row : std::as_const(m_rows)) {
        const auto isIt = [&](const core::Machine& one) { return one.id == row.machine.id; };
        if (std::ranges::any_of(m_failed, isIt) || std::ranges::any_of(m_skipped, isIt)) {
            again.append(row.machine);
        }
    }
    run(again);
}

void BulkRun::run(const QList<core::Machine>& list)
{
    ++m_run;
    m_queue = list;
    m_failed.clear();
    m_skipped.clear();
    m_total = static_cast<int>(list.size());
    m_ok = 0;
    m_unknown = 0;
    m_out = 0;
    m_running = true;
    m_stopping = false;
    for (const core::Machine& machine : list) {
        mark(machine.id, QString(), words("Waiting…"));
    }
    m_summary = words("Starting…");
    emit changed();
    // A few at a time; or one after the other.
    const qsizetype workers = m_gapSeconds ? 1 : std::min(aFewAtATime, list.size());
    for (qsizetype worker = 0; worker < workers; ++worker) {
        next();
    }
}

void BulkRun::next()
{
    while (!m_queue.isEmpty()) {
        const core::Machine machine = m_queue.takeFirst();
        if (const QString out = m_ways.underWay(machine.id); !out.isEmpty()) {
            // Something sent from its card or its menu, or by a run before this one, is still out to
            // this machine. Nothing is sent on top of it. Offered again with what failed.
            mark(machine.id, u"bad"_s, words("\"%1\" is still under way here").arg(out));
            m_failed.append(machine);
            count();
            continue;
        }
        mark(machine.id, QString(), words("Sending…"));
        m_ways.began(machine.id, m_ask.title);
        ++m_out;
        emit changed();
        core::Action action = m_ask.action;
        action.batch = m_batch;
        m_ways.send(machine, action,
                    [guard = QPointer<BulkRun>(this), run = m_run, machine](const base::Result<core::Done>& answer) {
                        if (guard == nullptr) {
                            return;
                        }
                        // Whoever's run it was: the machine is free again.
                        guard->m_ways.ended(machine.id);
                        if (run == guard->m_run) {
                            guard->answered(machine, answer);
                        }
                    });
        return;
    }
    if (m_out == 0 && m_running && !m_pause.isActive()) {
        finish();
    }
}

void BulkRun::answered(const core::Machine& machine, const base::Result<core::Done>& answer)
{
    --m_out;
    if (answer) {
        // ("already on": left as it was, and that is no failure.)
        const QString& note = answer.value().note;
        mark(machine.id, u"ok"_s, note.isEmpty() ? m_ask.done : note);
        ++m_ok;
        m_ways.done(machine, m_ask);
    } else if (m_ways.mayHaveBeenDone(machine, answer.error())) {
        mark(machine.id, u"unknown"_s, words("No answer. It may have been done."));
        ++m_unknown;
    } else {
        mark(machine.id, u"bad"_s, QString::fromStdString(answer.error().message));
        m_failed.append(machine);
    }
    count();
    // One after the other, with a pause where one was asked for and another machine waits.
    if (m_gapSeconds && *m_gapSeconds > 0 && !m_queue.isEmpty()) {
        m_summary += words(" Next in %1 s.").arg(*m_gapSeconds);
        emit changed();
        m_pause.start(*m_gapSeconds * m_msPerSecond);
        return;
    }
    emit changed();
    next();
}

void BulkRun::stop()
{
    if (!m_running || m_stopping) {
        return;
    }
    m_stopping = true;
    for (const core::Machine& machine : std::as_const(m_queue)) {
        mark(machine.id, u"skipped"_s, words("Not sent"));
        m_skipped.append(machine);
    }
    m_queue.clear();
    m_pause.stop();
    emit changed();
    // What is on its way is waited for; where nothing is, that was all.
    if (m_out == 0) {
        finish();
    }
}

void BulkRun::forget()
{
    ++m_run;
    m_pause.stop();
    m_queue.clear();
    m_failed.clear();
    m_skipped.clear();
    m_rows.clear();
    m_running = false;
    m_stopping = false;
    m_out = 0;
    m_summary.clear();
    emit changed();
}

void BulkRun::mark(const QString& machine, const QString& state, const QString& text)
{
    for (Row& row : m_rows) {
        if (row.machine.id == machine) {
            row.state = state;
            row.text = text;
        }
    }
}

void BulkRun::count()
{
    m_summary = words("%1 of %2 done…").arg(m_ok + m_failed.size() + m_unknown).arg(m_total);
}

void BulkRun::finish()
{
    m_running = false;
    m_stopping = false;
    if (m_ok == m_total) {
        m_summary = m_ok == 1 ? words("Done on the KVM.") : words("Done on all %1 KVMs.").arg(m_ok);
    } else {
        QStringList parts{words("%1 succeeded").arg(m_ok)};
        if (!m_failed.isEmpty()) {
            parts.append(words("%1 failed").arg(m_failed.size()));
        }
        if (m_unknown > 0) {
            parts.append(words("%1 without an answer").arg(m_unknown));
        }
        if (!m_skipped.isEmpty()) {
            parts.append(words("%1 not sent").arg(m_skipped.size()));
        }
        m_summary = parts.join(u", "_s) + u'.';
    }
    emit changed();
    emit finished();
}

QVariantMap BulkRun::shown() const
{
    if (m_rows.isEmpty()) {
        return {};
    }
    QVariantList rows;
    for (const Row& row : m_rows) {
        rows.append(QVariantMap{{u"name"_s, row.machine.name}, {u"state"_s, row.state}, {u"text"_s, row.text}});
    }
    // Offered again: what was refused, and what was never sent. Not what got no answer, which may
    // have been carried out: pressing a power button a second time is not a retry.
    QString again;
    if (!m_running && (!m_failed.isEmpty() || !m_skipped.isEmpty())) {
        again = m_skipped.isEmpty()  ? words("Retry failed")
                : m_failed.isEmpty() ? words("Send the rest")
                                     : words("Retry failed, send the rest");
    }
    return {
        {u"title"_s, words("%1 · %2 KVMs").arg(m_ask.title).arg(m_rows.size())},
        {u"summary"_s, m_summary},
        {u"running"_s, m_running},
        {u"stopping"_s, m_stopping},
        {u"again"_s, again},
        {u"rows"_s, rows},
    };
}

} // namespace qkvm::ui
