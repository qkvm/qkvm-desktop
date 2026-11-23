#pragma once

#include <functional>

#include <QString>

class QThread;

namespace qkvm::core::lines {

// A thread of its own for one line to a device: its socket, its TLS and every judgement of the certificate it is shown
// (outside review 2.2). Windows judges what a TLS server shows on the thread that speaks TLS, at the handshake and
// again after it (each session ticket of TLS 1.3, each key update, each renegotiation: Qt judges anew at every one),
// and where the certificate names a signer to be fetched from an address that does not answer (an AIA fetch on a PC
// without internet), it waits there for as long as it gives that. So each line has a thread of its own: one held by
// Windows holds no other line, and never the thread its user lives on.
//
// Started here, above normal priority (QThread::HighPriority): the watch that keeps a line to its limits runs there,
// and a thread put aside by busier work at normal priority would let it come late. Ended by whoever made something on
// it, which quits it when that is done (QThread::quit, from the thread itself); then it goes by itself (the QThread, a
// turn of the loop of the thread that asked for it later).
// Nothing waits for it, ever, but the application's end, and that for endWithinMs at the most, for all of them
// together: a thread still held by Windows then is left to the end of the process. None where there is no
// application, or it is ending: nothing more is begun then. None either where it could not be started, and none for a
// line to `place` (its device's address: host and port) while mostHeldToOnePlace threads of that place's lines are
// still held: let go of by their lines (letGo) more than heldAfterMs ago, and not ended yet (outside review of tls, L7:
// every attempt at a device whose signer Windows waits for would leave another such thread, and a call of Windows'
// that never returns would leave them without end). Whoever asked tells that as a line not reached.
//
// From any thread that has an event loop.
QThread* aThreadOfItsOwn(const QString& name, const QString& place);

// The line made on `thread` has let go of it: its end is posted to it. From then on it is waited for no more, and
// counts as held where it has not ended heldAfterMs later. (On the thread that asked for it; a thread that has ended
// already is no more here.)
void letGo(const QThread* thread);

// The longest the application's end (its post routine, on the interface thread) waits for lines' threads to end.
inline constexpr int endWithinMs = 300;

// How many threads let go of by their lines may still be held by Windows for one place before its next line is
// refused, and from how long after they were let go of they count so. (A line's thread ends within milliseconds of its
// line when Windows does not hold it: lines that go and come again in a breath, a wall's Proxmox consoles to one host
// among them, never come near this.)
inline constexpr int mostHeldToOnePlace = 4;
inline constexpr int heldAfterMs = 2000;

// How many of those threads run now: for whoever looks that none is left behind once what was made on them has gone
// (a test).
int running();

// Has each of those threads that runs now do `what`, at its next turn (one that Windows holds, once Windows lets it
// go); on how many. For a test that holds a line's thread there, as Windows holds it while it judges a certificate.
int askEach(const std::function<void()>& what);

// For a test, as askEach: the looks on lines' threads before Qt has a message their loops take from Windows
// (device_line.cpp's BeforeQt) call `hook` after a look that found nothing to do, with that message (an MSG of
// Windows'; none where the look is made before a call posted to a line's socket), until it answers true. So a test can
// put a line's thread aside between that look and Qt's own work, as Windows or a busier PC may, which nothing else on
// one PC can make. Set none to take it away.
void afterALook(std::function<bool(const void* message)> hook);
// (Said by a line's thread after such a look.)
void lookedBeforeQt(const void* message);
// For a test as well, as afterALook, but before that look looks at anything (a line reset from its own thread, what
// has waited too long), with each message it is made for on a line whose socket stands: so a test can put a line's
// thread aside at a message of Windows' and end the line from its own thread meanwhile. The call that says so then
// waits behind that message, and only the look can reset the line before Qt has it (review cx2-cxl, L1: without
// this, which of the two came first no case chose).
void beforeALook(std::function<bool(const void* message)> hook);
// (Said by a line's thread before such a look.)
void lookingBeforeQt(const void* message);

// For a test as well, as afterALook: a line's thread, about to take what came off its socket, calls `hook` with the
// room it found for it there (what the line takes, less what it holds and what waits for it to take), once it has let
// go of the lock the two share and before it reads or says anything of that, until it answers true. So a test can put
// that thread aside between its look at the room and what it does with it, while the line gives room back (outside
// review 2, 2.1). Set none to take it away.
void afterTheRoom(std::function<bool(qint64 room)> hook);
// (Said by a line's thread at that place.)
void foundRoom(qint64 room);

// For a test as well: how much of all that was said on a line its thread has handed Windows (all it gave its socket,
// less what the socket still holds), said by that thread to `hook` each time it looks at its socket, which it does
// after each word of Qt's that Windows took something and after each message it gives the socket (Qt hands nothing to
// Windows on that thread without one of those, before its loop turns again), and once more the moment before it lets
// go of the socket (reset, or ended), which throws away what the socket still holds. So a test sees what went to
// Windows, and when, on the thread that handed it on: the far end cannot see it where a reset follows close behind
// (Windows throws away there what came and was not read yet), and its own times are its thread's, taken after it read
// (outside review 2, 2.3). In the clear (over TLS what the socket holds is counted hidden), and on a line only this
// program ends (Qt's own end of a socket after an error throws away what it held with no look). Set none to take it
// away.
void whatWasHanded(std::function<void(qint64 bytes)> hook);
// (Said by a line's thread at those places.)
void handed(qint64 bytes);

// For a test as well: whether lines' threads make that look before Qt at all (always, unless a test turns it off to see
// the guards behind it on their own: what leaves looked at as it leaves, one message at a time to the socket). Qt's
// loop on Windows posts its word that a socket has room again anew each time it listens again, and acts on one of
// them, so a test cannot tell at which word Qt hands on what a socket holds; with the look off, whichever it is, no
// look comes first.
void looksBeforeQt(bool on);
bool looksBeforeQt();

} // namespace qkvm::core::lines
