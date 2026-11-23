#pragma once

#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace qkvm::core::testing {

// What Qt says, from any thread, of an object used from another thread than its own ("Timers cannot be stopped from
// another thread", "Cannot create children for a parent that is in a different thread"): kept while one of these is
// there, and handed on to nobody, so that a case says it and goes on (timers_on_their_thread.cpp would end the program
// there). The rest goes where it went. One at a time.
class HeardOfThreads {
public:
    HeardOfThreads()
    {
        const QMutexLocker locked(&mutex());
        kept().clear();
        before() = qInstallMessageHandler(heard);
    }
    ~HeardOfThreads()
    {
        const QMutexLocker locked(&mutex());
        qInstallMessageHandler(before());
    }
    HeardOfThreads(const HeardOfThreads&) = delete;
    HeardOfThreads& operator=(const HeardOfThreads&) = delete;
    HeardOfThreads(HeardOfThreads&&) = delete;
    HeardOfThreads& operator=(HeardOfThreads&&) = delete;

    static QStringList said()
    {
        const QMutexLocker locked(&mutex());
        return kept();
    }

private:
    static void heard(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        const QMutexLocker locked(&mutex());
        if (message.contains(QLatin1StringView("another thread")) ||
            message.contains(QLatin1StringView("different thread"))) {
            kept().append(message);
            return;
        }
        if (before() != nullptr) {
            before()(type, context, message);
        }
    }
    static QMutex& mutex()
    {
        static QMutex one;
        return one;
    }
    static QtMessageHandler& before()
    {
        static QtMessageHandler one = nullptr;
        return one;
    }
    static QStringList& kept()
    {
        static QStringList one;
        return one;
    }
};

} // namespace qkvm::core::testing
