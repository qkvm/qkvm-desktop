#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <qkvm/core/device_line.h>

namespace qkvm::core {

// Keyboard and mouse to a KVM that runs kvmd (PiKVM, GL.iNet Comet): a line of its own to the KVM
// (DeviceLine, which see for how the KVM's certificate is looked at before the pass goes out),
// on which keys, buttons and the pointer are said in kvmd's words (kvmd_input). Its calls take what
// LiveLink's of the same names take, so that one screen can drive a QKVM server or a KVM.
//
// The line is opened when this is made, and never a second time. Nothing is said on it before
// the KVM has let it in (`opened`), and what was asked for before that is dropped, not kept: a
// key pressed before the line stands does not arrive later by surprise. What comes from the KVM
// on it is thrown away unread.
//
// Only a press that begins while the line can say it is said. It remembers which keys and buttons
// it said are down: one that is down is not said down a second time (the machine repeats by
// itself), and one that never went down is not said up. And it remembers which it was told are
// down while it could not say so (the line not open yet, or the key let go by releaseAll() while
// a finger was still on it): for such a one nothing is said, also not when Windows repeats it,
// until it was told that it is up. A key held across the opening is not pressed on the machine.
// Nor is a key or a button of the mouse said that goes down while one of the eight modifiers
// (Ctrl, Shift, Alt and the Windows key, left and right) is held here in that way: on the machine
// that modifier is up, and what was pressed would arrive bare, a "c" typed into a shell where
// Ctrl+C was pressed, a click where Ctrl and a click was. It is held too, until it was let go
// here. Where the pointer is, how far it moved and the wheel are said all the same.
//
// What is said is to go to the machine now or not at all. The KVM is asked every so often whether
// it is there, and a line on which nothing at all has come for too long is cut. And nothing is
// said behind what has waited in this program to leave for too long: the line ends instead, and
// what waited here is thrown away. Either way the line is reset, and what Windows had taken for
// it and not delivered goes with it (DeviceLine::say). Not covered by either: a way that stalls
// for less long than the KVM may be silent for plays to the machine what was pressed
// meanwhile, late; and what the KVM's own system had taken when the line ended is the KVM's.
//
// Before the line is ended from here everything that is down is let go. Where the line ends by
// itself (`closed`) that cannot be said any more. What this line said is down is forgotten here,
// and the machine may still hold those keys and buttons then: it is the KVM's to let go of them
// when the line is gone, and nothing from here can make it. So may it hold what was said to be
// up last: that may have gone with the line.
//
// On the thread it was made on. What it says, it says from the middle of its own work, also from
// inside the calls below, close() excepted: it may be told to close() from there, and is not
// thrown away from there (deleteLater() is).
class KvmdConsoleLine : public QObject {
    Q_OBJECT

public:
    // Where on a KVM the line is. What stands behind its question mark is the caller's to choose
    // (`Where::query`): with "stream=1" the KVM counts this line as somebody who watches, and
    // keeps its video awake for it; with "stream=0" it does not.
    static constexpr const char* path = "/api/ws";

    // Where the KVM is, who asks, and what it is known by: as for the line that keeps it awake.
    // The query and the cookie are held to what DeviceLine::Where holds them to, and a line for
    // anything else is not asked for ("bad_response").
    struct Where {
        QUrl address;      // the KVM's: http or https, its host, its port where it has one of its own
        QByteArray query;  // what stands behind the question mark of `path`; none: no question mark
        QByteArray cookie; // what the asking carries to say who asks
        bool knownByCertificate = false;
        QByteArray certificate; // its SHA-256
        // How keys and the mouse are told (kvmd_input.h): in the short messages of bytes kvmd's
        // own pages send, or in the texts of JSON an older kvmd takes. Bytes unless whoever makes
        // the line knows the KVM to be one that takes only texts.
        bool inTexts = false;
    };

    // The times it keeps, and how much may wait to go to the KVM: one that takes nothing off the
    // line is gone. (A key is some sixty bytes: this is some thousands of them, which is more
    // than a hand says in seconds.) A test makes them short and small.
    struct Pace {
        int letInWithinMs = 4500; // to be let in at all
        int askEveryMs = 1000;    // how often the KVM is asked whether it is there
        // Nothing at all has come from the KVM for this long, three askings: it is taken to be
        // gone, or the way to it, and the line is cut ("timeout").
        int silentAfterMs = 3000;
        // What was said has waited in this program to leave for longer than this: nothing more
        // is said behind it, and the line ends ("timeout").
        int lateAfterMs = 250;
        int goodbyeWithinMs = 2000; // how long a line that was closed from here may take to go
        qint64 mostUnsentBytes = qint64{256} * 1024;
    };

    explicit KvmdConsoleLine(Where where, QObject* parent = nullptr);
    KvmdConsoleLine(Where where, Pace pace, QObject* parent = nullptr);
    ~KvmdConsoleLine() override;
    KvmdConsoleLine(const KvmdConsoleLine&) = delete;
    KvmdConsoleLine& operator=(const KvmdConsoleLine&) = delete;
    KvmdConsoleLine(KvmdConsoleLine&&) = delete;
    KvmdConsoleLine& operator=(KvmdConsoleLine&&) = delete;

    // The KVM has let the line in, and it has not ended since.
    bool isOpen() const { return m_open; }
    // The pass the line was asked for with: what stands behind the "=" of its cookie. For
    // whoever holds a line that ended "auth_failed", to tell the driver that made it which pass
    // the KVM refused (KvmdDevice::passWasRefused).
    QByteArray pass() const { return m_pass; }
    // A key, by its USB HID usage (page 7); the pointer, absolute (each 0 to 32767 over the
    // picture) or by how far it moved; a button, 1 to 5; the wheel. Nothing is sent while the
    // line is not open, and nothing for a key or a button kvmd has no name for.
    // A key that is down when this is made, and of which the first "down" told here is Windows'
    // repeat, is said as a press: whoever feeds this keeps back what is down when control is taken.
    void key(int usage, bool down);
    void pointerAt(int x, int y);
    void pointerBy(int dx, int dy);
    void button(int number, bool down);
    void wheel(int dx, int dy);
    // Lets go of everything that is down on the machine: the buttons first, then the keys, each
    // the last that went down first. (Ctrl let go before the key it was held with leaves that key
    // bare, and before a button turns what is being dragged into something else.) Whatever of it
    // a finger is still on stays unsaid until it was told to be up.
    void releaseAll();
    // Ends the line from here, letting go first of what is down. `closed` is never said for that:
    // not from inside this call, and not later. That holds where the line takes no more while it
    // is being let go of: it is cut there, and what could not be let go stays down on the machine
    // until the KVM lets go of it. Whoever closes it is told that by what this gives back.
    //
    // True: everything that was down was said to be up, and that was offered to Windows with
    // the goodbye, on a line that stood. (So far this program sees: whether it got there, only
    // the KVM's own goodbye says, and a line the KVM does not end in its time is reset with
    // what had not left. Whoever needs to know keeps the line until it has gone: letGo.) False:
    // something may still be down on the machine. A key or a button could not be said up any more; or
    // what was said up since the KVM was last heard from went with a line that was reset in
    // here, and not pushed out. Of a line that had ended before, by itself or from here, nothing
    // is left to say: true. What that one left down it said when it ended (`closed`).
    bool close();
    // Closes a line and keeps it until it has gone (DeviceLine::close, `gone`): the KVM ends a
    // line after the goodbye, and one that it has not ended within the line's time is reset then,
    // with what Windows still held for it, so that a key that waited is not played later. Whoever
    // is done with a line hands it over here rather than throwing it away in the same breath. It
    // ends with the program at the latest. What it gives back is what close() says.
    // `stillDown`: for what only shows later. Where the closing said that all was let go, on
    // the strength of an "up" that was offered to Windows and that the KVM was not heard from
    // after, and the line then goes without the KVM's own goodbye (it fell, or the KVM did not
    // end it in its time and it was reset, the "up" with it), this is called, once, when the
    // line has gone: something may still be down on the machine after all, late as that is to
    // say. Never from inside this call. Not called where the KVM's goodbye came, which says that
    // it got what was said; nor where the closing itself said no; nor for a line that had
    // ended before, which said what it left down when it ended (`closed`).
    // `gone`: called once, when the line has gone, after `stillDown` where that is called: what
    // was let go of has landed by then (the KVM's goodbye came behind it), or went with the line.
    // For whoever sends the machine something next, which is not to land beside a key that is
    // still on its way up (KvmdLive's word for a giving back). Never from inside this call: for
    // no line, or one that had ended before, a turn of the loop later.
    static bool letGo(std::unique_ptr<KvmdConsoleLine> line, std::function<void()> stillDown = {},
                      std::function<void()> gone = {});

signals:
    void opened();
    // The line ended by itself, once. A code of the driver's (KvmdDevice) for why: "unreachable",
    // "timeout" (it did not answer the asking, fell silent, or did not take what was said in
    // time), "cert_mismatch", "cert_invalid", "auth_failed" (the KVM does not know the pass:
    // whoever made the line signs in anew), "bad_response", "dropped"; and the code of tls.h
    // where the KVM's address is an HTTPS one and TLS is not Windows' own, in which case nothing
    // went anywhere. The sentence is this program's own: nothing a KVM sent is in it.
    // `leftDown`: this line had said keys or buttons down that it could not let go of any more.
    // They are forgotten here, and the machine may still hold them: the KVM's to let go. Or it
    // had said some up since the KVM was last heard from: that may have gone with the line, and
    // counts as possibly still down.
    void closed(const QString& code, const QString& message, bool leftDown);

private:
    bool say(const QByteArray& word);
    // A key, a button, a place, a move or a turn, said in the wording the KVM takes.
    bool sayOfInput(const QByteArray& word);
    void over();
    // The line ended by itself: what is down is forgotten, and `closed` says so.
    void gone(const char* code, const char* message);

    // (Before the line, which is made of what is left of `where`.)
    bool m_inTexts;
    QByteArray m_pass;
    DeviceLine m_line;
    QTimer m_asking;
    QTimer m_silence;
    bool m_open = false;
    bool m_ended = false;
    // What this line said is down on the machine, in the order it went down; and what it was
    // told is down here while it could not say so. Never more of either than there are keys with
    // a name, and buttons.
    QList<int> m_keysDown;
    QList<int> m_buttonsDown;
    QList<int> m_keysHeld;
    QList<int> m_buttonsHeld;
    // A key or a button was said to be up, and nothing has come from the KVM since: where the
    // line ends now, that word may have gone with it. (Heard from since is taken to mean that it
    // got there. What came may have been on its way before the word was said: then that is a
    // guess.)
    bool m_saidUpUnheard = false;
    // close() said that all was let go, and that rests on an "up" that was offered to Windows
    // and that the KVM was not heard from after: whether it arrived, only the KVM's goodbye
    // says (letGo).
    bool m_restsOnAnUp = false;
};

} // namespace qkvm::core
