#pragma once

#include <utility>

#include <QObject>

namespace qkvm::core {

namespace detail {

// What keptWith keeps: a child of its owner, and nothing else.
template <typename T>
class Kept final : public QObject {
public:
    Kept(QObject& owner, T held) : QObject(&owner), value(std::move(held)) {}
    T value;
};

} // namespace detail

// As keptWith (below), and gives the keeper itself: a child of `owner`'s that holds `value` in its `value`, and lets go
// of it as it ends. For a call of Qt's that hears its answer on a context object, and may let go of the functor that
// hears it on another thread: QHostInfo::lookupHost, whose functor is its lookup's own until the answer is handed to an
// object of the context's thread, and is let go of on Qt's lookup thread where the context has ended by then. Given the
// keeper as that context, the functor holds only the keeper's pointer: it is not called once the keeper has ended,
// with `owner` or before, and what it was to call ends with the keeper, on `owner`'s thread.
template <typename T>
detail::Kept<T>& keeperOf(QObject& owner, T value)
{
    return *new detail::Kept<T>(owner, std::move(value)); // NOLINT(cppcoreguidelines-owning-memory): owner's child
}

// Keeps `value` for as long as `owner` lives, and lets go of it as `owner` ends, on `owner`'s own thread: it is a child
// of `owner`'s. Gives where it is kept, good until then.
//
// For what a slot connected to a signal of one of Qt's network replies would hold otherwise. Qt lets go of a
// connection's slot on whatever thread cleans up after the connection, and a reply's connections are cleaned up on Qt's
// own network thread too: there it ends what it had of the reply (its delegate) as the reply ends here, and with that
// lets go of the slots of the reply's connections that have ended by then. Seen: a whole call's limit, a QTimer of the
// interface thread, ended there ("QObject::killTimer: Timers cannot be stopped from another thread"; and whatever heard
// the answer with it). So what such a slot captures may be let go of on any thread: plain values and pointers only, and
// what is to go on this thread is kept here, with the reply.
template <typename T>
T& keptWith(QObject& owner, T value)
{
    return keeperOf(owner, std::move(value)).value;
}

} // namespace qkvm::core
