#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

// M-D4. libdatachannel calls a mailbox on its own thread while it holds a lock of its own, so a call
// into the library while a mailbox lock is held can leave the two threads waiting for each other; the
// registry's lock too, which those calls take to find their mailbox. Every lock of the files that speak
// to the library (webrtc_receiver.cpp, and the offering side's) is taken through Held, and every call
// into the library is preceded by intoLibrary(), which counts the calls made with one held (the test
// seam callsUnderOwnLock()). tools/test_review_guards.ps1 holds those files to it, and holds every other
// file of the program off the library. Moved here from webrtc_receiver.cpp as it was (M6 2b).
namespace qkvm::core::rtcseam {

inline thread_local int heldHere = 0;
inline std::atomic<int> callsUnderLock{0};

class Held {
public:
    explicit Held(std::mutex& mutex) : m_lock(mutex) { ++heldHere; }
    ~Held() { --heldHere; }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;

private:
    std::lock_guard<std::mutex> m_lock;
};

inline void intoLibrary()
{
    if (heldHere > 0) {
        callsUnderLock.fetch_add(1, std::memory_order_relaxed);
    }
}

// How often this program called into the library while it held a lock of these files, on any thread.
inline int callsUnderOwnLock()
{
    return callsUnderLock.load(std::memory_order_relaxed);
}

// The mailboxes of one kind, by the library's id of the peer or track they are for: what the library's
// callbacks find their mailbox by.
template <class Box>
class Registry {
public:
    static std::shared_ptr<Box> find(int id)
    {
        const Held lock(mutex());
        const auto found = boxes().find(id);
        return found == boxes().end() ? nullptr : found->second;
    }
    static void remember(int id, const std::shared_ptr<Box>& box)
    {
        const Held lock(mutex());
        boxes()[id] = box;
    }
    static void forget(int id)
    {
        const Held lock(mutex());
        boxes().erase(id);
    }

private:
    static std::mutex& mutex()
    {
        static std::mutex value;
        return value;
    }
    static std::map<int, std::shared_ptr<Box>>& boxes()
    {
        static std::map<int, std::shared_ptr<Box>> value;
        return value;
    }
};

} // namespace qkvm::core::rtcseam
