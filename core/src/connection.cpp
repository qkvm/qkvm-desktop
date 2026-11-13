#include <qkvm/core/connection.h>

namespace qkvm::core {

Connection after(Connection state, ConnectionEvent event)
{
    using enum Connection;
    using Event = ConnectionEvent;
    // From anywhere: another server is to be named, and whatever was under way with this one is over.
    if (event == Event::Left) {
        return None;
    }
    // Wherever a server is being spoken to: a certificate that is not the one it is known by ends
    // that, at once, until somebody has looked at it.
    if (event == Event::CertificateShown) {
        return state == None ? None : WantsTrust;
    }
    switch (state) {
    case None:
        return event == Event::Named ? Connecting : state;
    case Connecting:
        switch (event) {
        case Event::NobodySignedIn:
            return WantsSignIn;
        case Event::SomebodySignedIn:
            return SignedIn;
        case Event::NoAnswer:
            return None;
        default:
            return state;
        }
    case WantsTrust:
        // The server is asked again, from the beginning, now that it is known by what it showed.
        return event == Event::Trusted ? Connecting : state;
    case WantsSignIn:
        return event == Event::SignInSent ? SigningIn : state;
    case SigningIn:
        switch (event) {
        case Event::SignInTaken:
            return SignedIn;
        // A server that fell silent with the password is asked again by whoever typed it.
        case Event::SignInRefused:
        case Event::NoAnswer:
            return WantsSignIn;
        default:
            return state;
        }
    case SignedIn:
        switch (event) {
        case Event::ContactLost:
            return Away;
        case Event::SignedOut:
            return WantsSignIn;
        default:
            return state;
        }
    case Away:
        switch (event) {
        case Event::ContactBack:
            return SignedIn;
        case Event::SignedOut:
            return WantsSignIn;
        default:
            return state;
        }
    }
    return state;
}

} // namespace qkvm::core
