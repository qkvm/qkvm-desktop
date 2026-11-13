#pragma once

#include <functional>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <qkvm/base/result.h>
#include <qkvm/core/server_types.h>

#include "file_door.h"

namespace qkvm::ui {

// The wall as a file, as the page's "Export…" and "Import…" have it, and by the server's own
// calls for it: the server makes the file and the server reads it, and this only writes it out
// and hands it in. What the two boxes of one window show and ask goes through here. It decides
// nothing about the server: what a box asks goes out as a signal, and whoever speaks to the
// server says back what came of it.
//
// Everything that is asked has a number, and an answer is taken only for what is waited for just
// now: what was asked before a box was closed, before the wall was left, or while one of Windows'
// own boxes was open and something else happened behind it, is nobody's when it comes.
//
// The sentences are the page's (app/qkvm/static/app.js, exportDialog and importDialog). A
// password and a passphrase pass through and are kept nowhere. The text of a file that was chosen,
// and what the server said is in it, are kept while its box is open and no longer. What a file
// says is shown so that nothing of it goes unseen: a character that draws nothing, or turns a
// line around, is written out as its number, and a console link is said by where a browser would
// go with it, or as something that is no plain address.
//
// Made in C++, one for each window, and handed to its screens; on the interface thread.
class FilesView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    // Something was asked, of the server or of whoever sits there (where a file is to be kept,
    // which is to be opened), and has not been answered: nothing else is asked meanwhile.
    Q_PROPERTY(bool busy READ isBusy NOTIFY changed)
    // The server is taking a file in. That cannot be given up: its box stays until it is over.
    Q_PROPERTY(bool taking READ isTaking NOTIFY changed)
    // What went wrong with what was asked last, as a sentence to show; empty: nothing.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // The file that was chosen to be taken in, by its name; empty: none yet.
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    // It says of itself that it is encrypted: there is a passphrase to ask for.
    Q_PROPERTY(bool sealed READ isSealed NOTIFY changed)
    // The file was read by the server: what is in it can be shown, and taken.
    Q_PROPERTY(bool wasRead READ wasRead NOTIFY changed)
    // What it is: "An encrypted file, made by QKVM 0.1.0, on 10/11/2026."
    Q_PROPERTY(QString about READ about NOTIFY changed)
    // It is plain and holds passwords all the same: said, so that it is deleted afterwards.
    Q_PROPERTY(bool plainSecrets READ hasPlainSecrets NOTIFY changed)
    // Its parts, each {key, label, hint, can, ticked, lines: [{text, typed}]}: whether it can be
    // taken at all, whether it is ticked to begin with, and what would be added, line by line.
    // (They say for themselves when they are others: a list that is made anew each time something
    // else changes would have the screen forget what was ticked in it.)
    Q_PROPERTY(QVariantList parts READ parts NOTIFY partsChanged)
    // There is a part that can be taken.
    Q_PROPERTY(bool canTake READ canTake NOTIFY changed)
    // What the server left out of it, as one sentence; empty: nothing.
    Q_PROPERTY(QString leftOut READ leftOut NOTIFY changed)

public:
    static QString shownTypedText(const QString& text);
    explicit FilesView(QObject* parent = nullptr);
    // How a file is chosen on this PC (file_door.h): Windows' own boxes in the program, a test's
    // own in a test.
    void setDoor(FileDoor door) { m_door = std::move(door); }

    bool isBusy() const { return m_waits != Waits::ForNothing; }
    // Its window is not to be closed just now: something is under way, or one of Windows' own
    // boxes stands over it, whatever has become of the question that was put in it meanwhile.
    bool holdsItsWindow() const { return isBusy() || m_atTheDoor; }
    bool isTaking() const { return m_waits == Waits::ForTheTaking; }
    QString problem() const { return m_problem; }
    QString fileName() const { return m_fileName; }
    bool isSealed() const { return m_sealed; }
    bool wasRead() const { return m_wasRead; }
    QString about() const { return m_about; }
    bool hasPlainSecrets() const { return m_plainSecrets; }
    QVariantList parts() const { return m_parts; }
    bool canTake() const { return m_canTake; }
    QString leftOut() const { return m_leftOut; }

    // A box is opened, or closed, or the wall is left: nothing of before is shown or kept, and
    // what was under way is given up.
    Q_INVOKABLE void reset();
    // What the page lists for a file to hold: [{key, label}], in the page's order.
    Q_INVOKABLE static QVariantList partsOfAFile();

    // "Export": the parts by the server's words, the passphrase for the file (empty: a plain file
    // without passwords), and the password of whoever asks, typed again.
    Q_INVOKABLE void exportWall(const QStringList& parts, const QString& passphrase, const QString& password);
    // "Import", step by step: a file is chosen, read by the server, and its ticked parts taken in.
    Q_INVOKABLE void chooseFile();
    Q_INVOKABLE void readFile(const QString& passphrase);
    // The passphrase was changed after the file was read: it is to be read again, with that one.
    Q_INVOKABLE void readAnew();
    Q_INVOKABLE void takeIn(const QStringList& parts, const QString& passphrase);

    // What came of what was asked under this number: said by whoever speaks to the server.
    void exported(quint64 turn, const base::Result<core::WallFile>& file);
    bool awaitsExport(quint64 turn) const { return turn == m_turn && m_waits == Waits::ForTheFile; }
    void fileWasRead(quint64 turn, const base::Result<core::FileContents>& contents);
    void fileWasTaken(quint64 turn, const base::Result<core::Taken>& taken);
    // The code whoever speaks to the server gives where what was asked was never sent to it at
    // all, with the sentence for why. Of a taking that leaves nothing open: no file was taken in.
    // (The program's own, with a colon: no server says it.)
    static constexpr const char* nothingWasSent = "desktop:not_sent";

signals:
    void changed();
    void partsChanged();
    // It is done: the box closes, and this is told where it was (`good`: as something that was
    // done; otherwise as something to be looked at).
    void done(const QString& said, bool good);
    // A taking under way was given up here, its box gone with its wall or its right to it: the
    // server may take the file in all the same, and this says so, to be told where it is seen.
    void cutOff(const QString& said);
    // For whoever speaks to the server.
    void exportAsked(quint64 turn, const QStringList& parts, const QString& passphrase, const QString& password);
    void readAsked(quint64 turn, const QByteArray& file, const QString& passphrase);
    void takeAsked(quint64 turn, const QByteArray& file, const QString& passphrase, const QStringList& parts);

private:
    enum class Waits { ForNothing, ForAPlace, ForTheFile, ForAChoice, ForTheReading, ForTheTaking };

    // Waits for this from now on, under a number of its own.
    quint64 waitFor(Waits what);
    // Whether this is the answer that is waited for; and if so, nothing is waited for any more.
    bool isAnswer(quint64 turn, Waits to);
    void fail(const QString& problem);
    void forgetTheFile();
    void forgetWhatWasRead();

    FileDoor m_door;
    Waits m_waits = Waits::ForNothing;
    quint64 m_turn = 0;
    bool m_atTheDoor = false;
    QString m_problem;
    // Where the file that is being made will be written, and whether it has a passphrase.
    QString m_savesTo;
    bool m_savesSealed = false;
    QByteArray m_file;
    QString m_fileName;
    bool m_sealed = false;
    bool m_wasRead = false;
    QString m_about;
    bool m_plainSecrets = false;
    QVariantList m_parts;
    bool m_canTake = false;
    QString m_leftOut;
};

} // namespace qkvm::ui
