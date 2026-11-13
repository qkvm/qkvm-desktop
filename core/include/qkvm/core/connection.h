#pragma once

// Where the program is with one QKVM server, and what follows what (DESIGN.md section 4): a state,
// the things that can happen to it, and one function from the two to the next state. Whoever speaks
// to the server asks this function what an answer means, and keeps no flags of its own beside it.
namespace qkvm::core {

enum class Connection {
    None,        // no server is named
    Connecting,  // the server was asked who is signed in, and has not answered
    WantsTrust,  // it showed a certificate that whoever sits here has to say yes to first
    WantsSignIn, // it answered: nobody is
    SigningIn,   // what was typed is with the server
    SignedIn,    // the wall
    Away,        // signed in, and the server does not answer: it is asked again and again
};

enum class ConnectionEvent {
    Named,            // an address was given that can be a server's
    NobodySignedIn,   // the server said so
    SomebodySignedIn, // the server said the session is somebody's already
    NoAnswer,         // the server was not there, or is not one this program can use
    CertificateShown, // it showed a certificate that is not known for it, or not the one that is
    Trusted,          // whoever sits here said yes to the certificate that was shown
    SignInSent,
    SignInTaken,
    SignInRefused, // also where the server wants a name or a code as well
    SignedOut,     // by the server, or by whoever sits at the screen
    ContactLost,
    ContactBack,
    Left, // another server is to be named
};

// The state after `event` happened in `state`. What means nothing in a state leaves it as it is.
Connection after(Connection state, ConnectionEvent event);

} // namespace qkvm::core
