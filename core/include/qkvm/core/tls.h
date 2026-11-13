#pragma once

#include <QUrl>

#include <qkvm/base/result.h>

namespace qkvm::core {

// Chooses Windows' own TLS (Schannel) for everything the program says over TLS, once, and says
// whether that is what is in use. Qt would take OpenSSL where it happens to find one, and the
// program would then trust and refuse differently from one PC to the next (DESIGN.md section 4).
//
// Whatever speaks over TLS calls this before it does, and before it makes anything of Qt's that
// could speak TLS: a socket that is made takes Qt's own pick there and then, and the choice
// comes too late for it. So a speaker whose socket is a member has the answer as a member
// before it. It is chosen at the first call and only there: Qt takes a choice once, and answers
// every later one with a warning, also where it is the same choice.
//
// False: what this program speaks over TLS is not Windows' own. One of two things is so. TLS
// was in use before anybody called this, with something else: a socket was made first, or
// another part of the process chose. Or the part of Qt that speaks Windows' own is missing from
// this copy of the program (the plugin "qschannelbackend", in the folder "tls" beside it): Qt
// has then picked for itself what it found, an OpenSSL that happens to be on this PC, or a part
// that reads certificates and speaks nothing, or it has found none. In none of these is a
// server's or a KVM's certificate judged as Windows judges it, so the program then speaks to
// nothing over HTTPS (refusedForItsTls): each speaker asks, and refuses such an address before
// anything goes out. Plain HTTP is none of this.
//
// From any thread.
bool useWindowsOwnTls();

// Whether an address is one the program does not speak to for how its TLS stands: one that is
// spoken to over TLS ("https"; "wss", which is what the line of a machine live at an HTTPS
// server is), where TLS is not Windows' own. A plain function of the two, the same for every
// speaker. Whether something is an address at all is each speaker's own to say.
bool refusedForItsTls(bool windowsOwnTls, const QUrl& address);

// What a speaker says of an address it refuses for that. The code is the program's own: written
// with a colon, as ServerClient's own are, so that no server and no KVM can say it. One
// sentence, for whoever sits at the program: there is nothing to be done about it but to install
// the program anew.
inline constexpr const char* tlsDamaged = "desktop:tls_damaged";
inline constexpr const char* tlsDamagedSentence =
    "This copy of qKVM is damaged and speaks to nothing over HTTPS: install qKVM anew.";
base::Error damagedTls();

} // namespace qkvm::core
