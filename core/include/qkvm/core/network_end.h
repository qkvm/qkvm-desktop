#pragma once

#include <memory>

class QNetworkAccessManager;

namespace qkvm::core {

// Ends one of Qt's managers of HTTP (a QNetworkAccessManager) without holding up the thread that ends it.
//
// Every such manager speaks on a thread of its own, which it makes for its first question, and its end
// waits for that thread to end, for up to five seconds. That thread may be held by Windows: a TLS line's
// certificate is judged there, and building its chain can fetch a signer's certificate from the address the
// certificate names (an AIA fetch), which on a PC without internet takes as long as Windows gives it. A
// question given up meanwhile ends at once; the manager's end waits. On the interface thread, that is the
// window held still for up to five seconds: never.
//
// So its questions (its replies) end here and now, on the thread they live on, as its own end would end them;
// and the rest of it, the wait among it, on a thread kept for that alone, which ends with the application (its
// post routine), once everything handed to it has ended. Where there is no application, or it is ending, or the
// manager is not this thread's, it ends here as before.
//
// From the thread the manager lives on. Not from inside a signal of one of its replies (its own end does
// not take that either).
void endOffThisThread(QNetworkAccessManager* network);

// A manager owned so: ended through endOffThisThread.
struct EndedOffThisThread {
    void operator()(QNetworkAccessManager* network) const { endOffThisThread(network); }
};
using OwnNetwork = std::unique_ptr<QNetworkAccessManager, EndedOffThisThread>;

// One made for its owner (with no parent: it is the owner's by this alone).
OwnNetwork ownNetwork();

} // namespace qkvm::core
