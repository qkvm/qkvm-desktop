#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QTimer>
#include <qkvm/core/device_line.h>

#include <qkvm/core/live.h>
#include <qkvm/core/tls.h>

namespace qkvm::core {

// One machine of a QKVM server's wall, live (live.h; SPEC.md section 6.2). A line of its own to
// the server, beside the calls of ServerClient, which is who makes one: with the server's
// address, the pass the server gave for this one line, and the rule for the server's certificate.
//
// The line is opened when this is made. From then on it says what comes (`videoChanged`,
// `pictureCame`, `stateChanged`, `controlChanged`), and at its end, once, why it ended
// (`closed`). It is never opened a second time: whoever wants the machine live again asks the
// client for another.
//
// Pictures are handed over as they come, none kept. Where H.264 begins (at the start, after a
// JPEG) and after a gap in it, nothing is handed over until a key frame, which is asked for: a
// picture that builds on one that never came would be drawn as rubbish.
//
// Keys and the mouse go out only while the server has said that this line holds them. Before
// they are given back, and before the line is ended from here, everything that is down on the
// machine is let go. Where the line ends by itself that cannot be said any more, and is the
// server's to do: it releases whatever the reason (the draft's rule). Whether the server could,
// it has no word for (SPEC.md section 6.2): so this line never says `leftDown`, whatever was
// down when keyboard and mouse went. What may still be down on the machine then, only the
// server knows.
//
// Over TLS the pass is said only to the server the program knows: to the certificate the server
// is known by, or, where it is known by none, to one that Windows trusts. What goes to any other
// before it is found out is the question for the line itself, which has no secret in it.
//
// On the interface thread. What it says, it says from the middle of the socket's own work: it
// may be told to `close()` from there, and is not thrown away from there (`deleteLater()` is).
//
// What it says is what a Live says, of this line. `videoChanged`: what the pictures are, as the
// server says it at the start and whenever it changes. A picture's counter is one higher for each
// picture the server sent on this line, and when it was taken is on a clock of the server's.
// `stateChanged`: as in the server's list of how machines are. `controlChanged`: where the server
// took keyboard and mouse away or did not give them, its code and its sentence for why ("refused"
// for a code that is no server's to say); where they went with the line, why the line ended.
// `closed`: the server's own word for why where it said one before it closed; one of
// ServerClient's own codes where the server was not reached, fell silent, or showed a certificate
// that is not its own; and the code of tls.h where the line would be encrypted and TLS is not
// Windows' own: nothing went anywhere then.
class LiveLink : public Live {
    Q_OBJECT

public:
    // How the line is watched: the server is asked every so often whether it is there, and one
    // of which nothing has come for so long is taken to be gone. So is one that takes nothing
    // off the line, once this much waits to be sent to it. A test waits less.
    struct Watch {
        int openWithinMs = 10'000;
        int askEveryMs = 3000;
        int silentAfterMs = 9000;
        qsizetype mostUnsentBytes = qsizetype{1024} * 1024;
    };

    // `request`: where the line goes, with what every call carries. `pass`: what the server gave
    // for this one line, said as the line's first word. `knownCertificate`: as ServerClient has
    // it, the SHA-256 of the certificate this server is known by; empty: one that Windows trusts.
    LiveLink(const QNetworkRequest& request, QByteArray pass, Need need, const QByteArray& knownCertificate,
             Watch watch, QObject* parent = nullptr);
    ~LiveLink() override;
    LiveLink(const LiveLink&) = delete;
    LiveLink& operator=(const LiveLink&) = delete;
    LiveLink(LiveLink&&) = delete;
    LiveLink& operator=(LiveLink&&) = delete;

    // The server has taken the line, and the first word was said.
    bool isOpen() const override { return m_open; }
    // A goodbye whose word is a server's (no ':' in it) that this program does not know.
    bool endsForGood(const QString& code) const override;
    Pointer pointerMode() const override { return m_pointer; }
    // The tile is another size now: said to the server where it is another need.
    void setNeed(Need need) override;
    // Whoever decodes the pictures has lost its place (a picture was thrown away, the graphics
    // device was lost): the server is asked for a key frame.
    void askForAKeyFrame() override;
    // Keyboard and mouse: asked for, or given back. Whether the line holds them is the server's
    // to say (`controlChanged`); until then it does not.
    void askControl(bool on) override;
    ControlOwner controlOwner() const override { return m_owner; }
    void takeOver(const QString& expectedOwner) override;
    bool hasControl() const override { return m_control; }
    // A key, by its USB HID usage (page 7); the pointer, absolute (each 0 to 32767 over the
    // picture) or by how far it moved; a button, 1 to 5; the wheel; and letting go of everything
    // that is down. Nothing is sent while the line does not hold keyboard and mouse.
    void key(int usage, bool down) override;
    void pointerAt(int x, int y) override;
    void pointerBy(int dx, int dy) override;
    void button(int number, bool down) override;
    void wheel(int dx, int dy) override;
    void releaseAll() override;
    // Ends the line from here, letting go first of what is down: `closed` is not said for that.
    void close() override;

    // How many pictures were not handed over because they built on one that never came.
    quint64 skipped() const { return m_skipped; }

private:
    friend class LiveLinkTestAccess;
    void say(const QJsonObject& what);
    void take(const QString& text);
    void take(const QByteArray& picture);
    // The line ended, or is ended here, for this reason: said once.
    void end(const QString& code, const QString& message);
    DeviceLine m_socket;
    QByteArray m_pass;
    Need m_need;
    Watch m_watch;
    bool m_open = false;
    bool m_ended = false;
    bool m_closing = false;
    bool m_control = false;
    ControlOwner m_owner;
    Pointer m_pointer = Pointer::Unknown;
    // The server's last word before it closed the line.
    QString m_byeCode;
    QString m_byeMessage;
    // The counter of the picture that came last. And whether an H.264 picture that is no key
    // frame can be shown: not at the start, not after a gap and not after a JPEG, until a key
    // frame has come; and whether one was asked for since.
    quint32 m_lastCounter = 0;
    bool m_needsKeyFrame = true;
    bool m_askedForKeyFrame = false;
    quint64 m_skipped = 0;
    QTimer m_opening;
    QTimer m_asking;
    QTimer m_silence;
};

} // namespace qkvm::core
