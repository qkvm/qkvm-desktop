#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "decode_pool.h"

namespace qkvm::engine {

// How many stills of a wall's cards that are key frames of H.264 (Codec::KeyFrameStill: a JetKVM's card) are decoded
// at once (PERF-2). Each such decoding makes a decoder of its own, some megabytes, and frees it after (decoder.cpp,
// keyFrameByItself); a wall of 64 cards decoded up to eleven at once, all in the same moment of each round of
// pictures, on all the decoding threads previews may have. (It was thought that what they left in the heaps stayed
// committed; measured with two, four, six and no limit, the totals lie within one spread, and the heaps kept the least
// beyond what is in use with two; how much of that is theirs was not shown: SPEC.md section 9.) The limit is held where
// a card's still is handed in to the decoding pool, and only there: the pool, its order and every other stream's work
// (a machine live or shown large, whatever its kind) are as they were, and never wait for this.
//
// A card that has a still to decode hands it in here (handIn) instead of waking the pool itself: it is woken now
// where it holds a place, or a place is free, and otherwise waits in line, in the order handed in; a card waits in
// line once, however often it hands in (the newest still it has is decoded when its turn comes). It holds its place
// until it lets go of its decoding (leave), and then the first in line has it and is woken. A card that comes to be
// no card (its machine shown large: setCard) is out of the line in that same step, under the line's lock, and what it
// hands in from then on is woken at once, as any other stream's. A stream that goes is forgotten first (forget): it is
// out of the line and its place, and refused from then on, so that whatever its last decoding hands in as it ends puts
// nothing of it back, and nothing of the line wakes it once it is freed. Any thread. The lock it has is taken after
// the device's and a stream's inbox and before the pool's, and no other lock is asked for while it is held
// (gpu_core.h).
class StillsGate {
public:
    // A stream as the line knows it, kept by the stream itself: the work the pool runs for it; whether it is a card of
    // a wall now (Stream::shownAsACard), which is changed only here, under the line's lock, and read by the stream as
    // it likes; and whether it goes (forget), which only the line's lock guards. A stream that goes is refused by
    // everything here from then on: the mark is the stream's own and goes with it, so that no stream made later where
    // it was is taken for it.
    struct Member {
        explicit Member(DecodePool::Work& work) noexcept : work(work) {}
        DecodePool::Work& work;
        std::atomic<bool> card{false};
        bool gone = false;
    };

    // `atOnce`: so many places; 0 or less: as many as stills are handed in (no limit, as before PERF-2).
    StillsGate(DecodePool& pool, int atOnce);
    StillsGate(const StillsGate&) = delete;
    StillsGate& operator=(const StillsGate&) = delete;

    // `fresh`: a still that came (and not the last one again, or what was left when it let go): one that a card in
    // line is handed takes the place of the one that waited, and is counted for that (Seen::replacedWaiting). A stream
    // that goes is refused: nothing is woken, and nothing waits. One that is no card (any more, as it was handed in) is
    // woken at once, and is not counted.
    void handIn(Member& stream, bool fresh);
    // Whether it is a card of a wall from now on; and whether that changed (not where it goes). No card any more, it
    // is out of the line in the same step; a place it holds is given back as it lets go (leave).
    bool setCard(Member& stream, bool card);
    bool holds(const Member& stream) const;
    void leave(Member& stream);
    // It goes (taken away, or its scene ends): out of the line, its place to the next, and refused from then on.
    void forget(Member& stream);

    // A still's decoding begins, and is over: counted, whatever the places, for what is said of them (seen).
    void decodingBegins();
    void decodingEnds();

    // Since the last time this was asked: the most stills that were decoded at once, and the longest one waited in
    // line, in milliseconds; how many waited in line at all, and how many a newer still of the same stream took the
    // place of while they waited; how many stills were handed in, and decoded. And as it is asked: how many places
    // are held, and how many wait in line.
    struct Seen {
        int mostAtOnce = 0;
        double longestWaitMs = 0;
        std::uint64_t waited = 0;
        std::uint64_t replacedWaiting = 0;
        std::uint64_t handedIn = 0;
        std::uint64_t decoded = 0;
        int holdingNow = 0;
        int inLineNow = 0;
    };
    Seen seen();

private:
    void wakeTheNext(); // with the lock held

    DecodePool& m_pool;
    const int m_atOnce;
    mutable std::mutex m_lock;
    // Neither ever holds a stream that goes (forget takes it out, and handIn refuses it after), and the line holds
    // only cards (setCard takes one out that is no card any more, and handIn wakes such a one at once).
    std::vector<const Member*> m_holding;
    struct InLine {
        Member* stream = nullptr;
        std::int64_t sinceUs = 0;
    };
    std::deque<InLine> m_line;
    int m_decoding = 0;
    Seen m_seen;
};

} // namespace qkvm::engine
