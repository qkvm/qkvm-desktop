#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <QElapsedTimer>
#include <QIODevice>
#include <QObject>
#include <QString>
#include <QTimer>

// For the tests of an image carried through a server from a file that answers slowly, or stops answering: the file,
// and a watch on the thread the program draws its windows on.
namespace qkvm::core::testing {

// The byte at a place of the pattern the upload tests write their files of (a piece of a megabyte, again and again).
inline char patternAt(qint64 at)
{
    return static_cast<char>(((at % (qint64{1} << 20)) * 7 + 3) & 0xFF);
}

// A file on a share far away: each read waits this long before it answers (a NAS over a VPN). From a place on, a read
// fails as one does where the share went (`failAt`), or stands still until it is let go (`stopAt`: the share stopped
// answering). Of the pattern, read at a place. `opensInMs`: how long it takes to be opened, where a test makes it so.
class FarAwayFile final : public QIODevice {
public:
    struct Gate {
        std::mutex lock;
        std::condition_variable opened;
        bool open = false;
        void letGo()
        {
            {
                const std::lock_guard held(lock);
                open = true;
            }
            opened.notify_all();
        }
        void waitHere()
        {
            std::unique_lock held(lock);
            opened.wait(held, [this] { return open; });
        }
    };

    FarAwayFile(qint64 size, int waitMs, qint64 failAt = -1, qint64 stopAt = -1, std::shared_ptr<Gate> gate = {},
                int opensInMs = 0)
        : m_size(size), m_waitMs(waitMs), m_failAt(failAt), m_stopAt(stopAt), m_gate(std::move(gate))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(opensInMs));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    qint64 size() const override { return m_size; }
    bool isSequential() const override { return false; }

protected:
    qint64 readData(char* data, qint64 most) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_waitMs));
        if (m_gate && m_stopAt >= 0 && pos() + most > m_stopAt) {
            m_gate->waitHere();
        }
        if (m_failAt >= 0 && pos() + most > m_failAt) {
            setErrorString(QStringLiteral("The specified network name is no longer available."));
            return -1;
        }
        const qint64 some = std::min(most, m_size - pos());
        for (qint64 at = 0; at < some; ++at) {
            data[at] = patternAt(pos() + at);
        }
        return some;
    }
    qint64 writeData(const char* /*data*/, qint64 /*length*/) override { return -1; }

private:
    qint64 m_size;
    int m_waitMs;
    qint64 m_failAt;
    qint64 m_stopAt;
    std::shared_ptr<Gate> m_gate;
};

// How long the thread this is made on stood still at the most while it was watched: a beat every 10 ms.
struct ThreadWatch {
    QElapsedTimer clock;
    qint64 last = 0;
    qint64 longest = 0;
    QTimer beat;

    ThreadWatch()
    {
        clock.start();
        QObject::connect(&beat, &QTimer::timeout, &beat, [this] {
            longest = std::max(longest, clock.elapsed() - last);
            last = clock.elapsed();
        });
        beat.start(10);
    }
};

} // namespace qkvm::core::testing
