#include <qkvm/core/network_end.h>

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QThread>

#include <utility>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The thread managers end on: made for the first, ended with the application.
struct Ender {
    QMutex mutex;
    QThread* thread = nullptr;
    bool over = false; // the application is ending, or has ended: nothing more is handed over
};

Ender& ender()
{
    static Ender one;
    return one;
}

// From the application's end (its post routine, on its thread): what was handed over has ended once this has. Each
// manager's end waits five seconds for its own thread at the most, and leaves it to end by itself after that (Qt's
// own rule), so this is never held for ever.
void endTheEnder()
{
    QThread* thread = nullptr;
    {
        const QMutexLocker locked(&ender().mutex);
        ender().over = true;
        thread = std::exchange(ender().thread, nullptr);
    }
    if (thread != nullptr) {
        thread->quit();
        thread->wait();
        delete thread; // NOLINT(cppcoreguidelines-owning-memory): made in theEnder, and ended here alone
    }
}

QThread* theEnder()
{
    const QMutexLocker locked(&ender().mutex);
    if (ender().over || QCoreApplication::instance() == nullptr) {
        return nullptr;
    }
    if (ender().thread == nullptr) {
        auto* thread = new QThread; // NOLINT(cppcoreguidelines-owning-memory): ended by endTheEnder
        thread->setObjectName(u"qKVM network end"_s);
        thread->start();
        ender().thread = thread;
        qAddPostRoutine(endTheEnder);
    }
    return ender().thread;
}

} // namespace

void endOffThisThread(QNetworkAccessManager* network)
{
    if (network == nullptr) {
        return;
    }
    // Its questions first, here, where they live: as its own end deletes them.
    qDeleteAll(network->findChildren<QNetworkReply*>());
    QThread* thread = network->thread() == QThread::currentThread() ? theEnder() : nullptr;
    if (thread == nullptr) {
        delete network; // NOLINT(cppcoreguidelines-owning-memory): handed over to be ended
        return;
    }
    network->setParent(nullptr);
    network->moveToThread(thread);
    network->deleteLater();
}

OwnNetwork ownNetwork()
{
    return OwnNetwork(new QNetworkAccessManager); // NOLINT(cppcoreguidelines-owning-memory): owned from here on
}

} // namespace qkvm::core
