#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <qkvm/core/secrets.h>

namespace qkvm::ui {

// Whether this PC is a wall screen: a screen nobody sits at, which only shows. It becomes one the
// moment a display link signs a QKVM server's wall in here (whoever is at that wall is a display
// link's: core::Account::isDisplay), and stays one, for that server, until a person signs that
// server's screen out at this PC. That goes through one function, ServerSource::signOut (the
// screens' "Sign out"), which alone ends a screen here (signedOutAt): what is to be asked before a
// screen is signed out is to be asked in front of it. By nothing else: not by the server taking
// the link back, not by contact lost, not by a QKVM of another version, not by a certificate that
// has to be looked at, not by a start while the server is away, and not by this program ending and
// starting again. While it is one, the whole program only shows (MergedWall::onlyShows): this PC's
// own KVMs are shown, and nobody acts on any machine.
//
// What is kept over the program's end is kept where the display links are kept: with the secrets
// (core::SecretStore, Windows Credential Manager), under one name (keptAs()), as the addresses of
// those servers (as their clients write them), one to a line. It is read once, when this is made.
// What forgets it, all of it there is:
// - a person signing out the screen of each of those servers here (signedOutAt), which takes that
//   server's line out, and the whole entry with the last one;
// - Windows that does not keep the entry when it is written: the program holds it all the same
//   while it runs, and the next start does not know of it;
// - Windows that cannot read the entry at a start (nothing is known then);
// - whoever deletes the entry in Windows' Credential Manager by hand.
// Nothing else of the program writes it or takes it away.
//
// On the interface thread. The secrets outlive it.
class WallScreen final : public QObject {
    Q_OBJECT

public:
    explicit WallScreen(core::SecretStore& secrets, QObject* parent = nullptr);

    // Under which name it is kept with the secrets.
    static QString keptAs();

    // Whether this PC is a wall screen just now: of one server at least.
    bool isOne() const { return !m_servers.isEmpty(); }
    // A display link signed in at the wall of the server at this address. Nothing where it is one
    // for that server already.
    void becameOneFor(const QString& server);
    // A person at this PC signed out the screen of the server at this address: called by
    // ServerSource::signOut, and by nothing else. Nothing where it was none for that server.
    void signedOutAt(const QString& server);
    base::Result<void> clearLocally();

signals:
    // It is one where it was none, or none where it was one.
    void changed();

private:
    void keep();

    core::SecretStore& m_secrets;
    // The servers it is a screen of, as their clients write their addresses.
    QStringList m_servers;
};

} // namespace qkvm::ui
