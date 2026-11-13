#pragma once

#include <QPointer>
#include <QtGlobal>

namespace qkvm::core {

class JetKvmConnection;

// What outlives a JetKVM's driver (M6 2g; design 3.4): kept in the wall's record of the KVM (LocalWall's
// Held) and handed to each driver it has, so that one made anew (its address, password or certificate
// changed) knows what the one before knew.
//
// * Whether the program stepped aside (another viewer took the JetKVM's one picture): written only where
//   judge() says so, and read by every road that would connect. Only "Show here" (act "resume") clears it.
// * Whether an offer of the program's came to nothing (it went, and its connection ended without coming
//   about): from then on no card makes an offer, each of which would take the JetKVM's one viewer from
//   whoever has it; a window that shows it does, and "Show here". Written by the viewer, and cleared where
//   a connection comes about, or by "Show here" (from the second reading of c7ec58c, B).
// * Which connection is the program's newest, by its serial, whether its offer went, and whether it is
//   over: what judge() reads, at the moment a word is read.
// * The newest connection itself, of whichever driver, until it is gone: a new one is begun only then (one
//   connection at a time for the KVM), waited for 8 s at most.
// * What the program's connections may have left down on it (the web's _Leftover, jetkvm.py): a JetKVM lets go of
//   its keyboard as a connection ends, never of a mouse button. Where one ended with a key or a button of the
//   program's down, or maybe down, the next begins with "nothing down" (JetKvmConnection), the absolute mouse let go
//   of where it was last put, over any connection.
//
// On the interface thread.
struct JetKvmKept {
    bool aside = false;
    bool notThrough = false;
    quint64 serials = 0; // the last serial handed out, for every driver of the KVM
    quint64 newest = 0;
    bool newestOfferWent = false;
    bool newestOver = true;
    QPointer<JetKvmConnection> ending; // the newest connection, until it is gone (being ended, or alive)
    struct Leftover {
        bool down = false;
        bool placed = false; // where the absolute mouse was last put, over any connection
        int x = 0;
        int y = 0;
    };
    Leftover leftover;

    enum class Word { StepAside, OwnNewOne };
    // Whose another viewer's word is, heard over the connection of `serial` (design 3.4's table): over the
    // newest, it steps aside; over an older one, once the newest's offer went and while the newest is not
    // over, it is about the program's own new one; over an older one otherwise (the newest still signing in
    // or asking, or over), it steps aside.
    Word judge(quint64 serial) const
    {
        if (serial != newest && newestOfferWent && !newestOver) {
            return Word::OwnNewOne;
        }
        return Word::StepAside;
    }
};

} // namespace qkvm::core
