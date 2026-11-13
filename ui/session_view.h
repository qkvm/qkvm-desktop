#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace qkvm::ui {

// What the screens are given of the program's dealings with a QKVM server: where it is with them,
// and what went wrong in words to show. And what the screens ask for in return: whoever speaks to
// the server hears it, and writes here what came of it. Nothing here speaks to a server itself.
//
// Made in C++ and handed to the screens; lives on the interface thread.
class SessionView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    // Where the program is: "address" (which server is still asked), "trust" (the server showed a
    // certificate that has to be looked at first), "signIn", or "wall".
    Q_PROPERTY(QString stage READ stage NOTIFY changed)
    // Something was asked of the server and has not been answered yet.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // What went wrong, as a sentence to show; empty when nothing did.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // Which server, as it is shown: its name or number, and its port.
    Q_PROPERTY(QString address READ address NOTIFY changed)
    // What the address's line begins with: the server that was used last, as it would be typed.
    Q_PROPERTY(QString suggested READ suggested NOTIFY changed)
    // The certificate the server showed, as its SHA-256 the way QKVM prints it; and the one it was
    // known by before, where that is another. For the stage "trust".
    Q_PROPERTY(QString certificate READ certificate NOTIFY changed)
    Q_PROPERTY(QString certificateBefore READ certificateBefore NOTIFY changed)
    // Whether what goes to that server is encrypted on the way.
    Q_PROPERTY(bool encrypted READ encrypted NOTIFY changed)
    // Whether the server wants a name beside the password, and a code from an authenticator app.
    Q_PROPERTY(bool asksName READ asksName NOTIFY changed)
    Q_PROPERTY(bool asksCode READ asksCode NOTIFY changed)
    // Who is signed in, and what the server lets them do: "admin", "operator" or "viewer", or
    // "display" for a screen opened with a display link, which only looks; empty: nobody, or this
    // PC's own wall.
    Q_PROPERTY(QString account READ account NOTIFY changed)
    Q_PROPERTY(QString role READ role NOTIFY changed)
    // False while the server does not answer: the wall shows what it last knew.
    Q_PROPERTY(bool inContact READ inContact NOTIFY changed)
    // Since when it does not answer, the day and the time as this PC writes them; empty while it does.
    Q_PROPERTY(QString awaySince READ awaySince NOTIFY changed)
    // Whoever is signed in manages this QKVM (an admin): what is managed on its page is offered.
    Q_PROPERTY(bool manages READ manages NOTIFY changed)
    // The wall is this PC's own: KVMs that were added in the program, which it speaks to itself.
    // No server, nobody signed in, and whoever sits here manages it.
    Q_PROPERTY(bool ownWall READ ownWall NOTIFY changed)
    // The QKVM said that it was restarted locked and that nobody has signed in since: it reaches no
    // machine until somebody does. For the stage "signIn".
    Q_PROPERTY(bool locked READ locked NOTIFY changed)
    // The server that is named did not answer as a QKVM, and is being asked again by itself: a
    // screen that opens by its display link, which nobody sits at to name its server again. For
    // the stage "address", where it is not busy meanwhile: another address can be typed.
    Q_PROPERTY(bool tryingAgain READ tryingAgain NOTIFY changed)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList sourceTree READ sourceTree NOTIFY sourceTreeChanged)
    Q_PROPERTY(QString source READ source NOTIFY sourcesChanged)
    Q_PROPERTY(bool canChooseSource READ canChooseSource NOTIFY sourcesChanged)

public:
    struct Shown {
        QString stage = QStringLiteral("address");
        bool busy = false;
        QString problem;
        QString address;
        QString suggested;
        QString certificate;
        QString certificateBefore;
        bool encrypted = false;
        bool asksName = false;
        bool asksCode = false;
        QString account;
        QString role;
        bool inContact = true;
        QString awaySince;
        bool manages = false;
        bool ownWall = false;
        bool locked = false;
        bool tryingAgain = false;

        bool operator==(const Shown&) const = default;
    };

    explicit SessionView(QObject* parent = nullptr);

    const Shown& shown() const { return m_shown; }
    void show(const Shown& shown);
    void showSources(const QVariantList& sources, const QString& selected, bool allowed);
    QVariantList sources() const { return m_sources; }
    QVariantList sourceTree() const { return m_sourceTree; }
    void showSourceTree(const QVariantList& rows);
    Q_INVOKABLE void openMachine(const QString& scope, const QString& machine) { emit machineAsked(scope, machine); }
    QString source() const { return m_source; }
    bool canChooseSource() const { return m_canChooseSource; }
    Q_INVOKABLE void chooseSource(const QString& source) { emit sourceAsked(source); }

    QString stage() const { return m_shown.stage; }
    bool busy() const { return m_shown.busy; }
    QString problem() const { return m_shown.problem; }
    QString address() const { return m_shown.address; }
    QString suggested() const { return m_shown.suggested; }
    QString certificate() const { return m_shown.certificate; }
    QString certificateBefore() const { return m_shown.certificateBefore; }
    bool encrypted() const { return m_shown.encrypted; }
    bool asksName() const { return m_shown.asksName; }
    bool asksCode() const { return m_shown.asksCode; }
    QString account() const { return m_shown.account; }
    QString role() const { return m_shown.role; }
    bool inContact() const { return m_shown.inContact; }
    QString awaySince() const { return m_shown.awaySince; }
    bool manages() const { return m_shown.manages; }
    bool ownWall() const { return m_shown.ownWall; }
    bool locked() const { return m_shown.locked; }
    bool tryingAgain() const { return m_shown.tryingAgain; }

    // What the screens ask for. Each only passes the wish on.
    Q_INVOKABLE void connectTo(const QString& address);
    // `keepSignedIn`: for thirty days, and over the program's end.
    Q_INVOKABLE void signIn(const QString& name, const QString& password, const QString& code, bool keepSignedIn);
    // Yes to the certificate that is shown: this server is known by it from now on.
    Q_INVOKABLE void trustCertificate();
    Q_INVOKABLE void signOut();
    // Back to saying which server: also from this PC's own wall, to connect to one.
    Q_INVOKABLE void anotherServer();
    // To this PC's own wall, from wherever the program is with a server.
    Q_INVOKABLE void goToOwnWall();
    // The QKVM's own page, in the browser: machines and accounts are managed there.
    Q_INVOKABLE void openPage();
    // Somebody types where a server is named: one that was being asked again by itself
    // (`tryingAgain`) is asked no more, and nothing comes up under what is being typed.
    Q_INVOKABLE void stopTrying();

signals:
    void changed();
    void sourcesChanged();
    void sourceTreeChanged();
    void machineAsked(const QString& scope, const QString& machine);
    void sourceAsked(const QString& source);
    void connectAsked(const QString& address);
    void signInAsked(const QString& name, const QString& password, const QString& code, bool keepSignedIn);
    void trustAsked();
    void signOutAsked();
    void anotherServerAsked();
    void ownWallAsked();
    void pageAsked();
    void stopTryingAsked();

private:
    Shown m_shown;
    QVariantList m_sources;
    QVariantList m_sourceTree;
    QString m_source;
    bool m_canChooseSource = false;
};

} // namespace qkvm::ui
