#pragma once

#include <optional>

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSslSocket>
#include <QString>
#include <QUrl>

#include <qkvm/core/tls.h>

namespace qkvm::core {

// A line to a KVM: a WebSocket on which short texts can be said, and on which what comes is thrown
// away unread unless bounded text reception is explicitly enabled. Janus signaling opts in
// to that mode, with a required WebSocket subprotocol, and a JetKVM's signalling line without one; a management
// controller's console reads bytes (receiveBytes), with a subprotocol it requires too. Other users:
// the line that keeps a KVM's video awake (AwakeLine), which hears short texts (hearShortTexts), the line that carries
// keyboard and mouse to it (KvmdConsoleLine), and a qKVM server's live line (LiveLink, receiveLive).
//
// It speaks WebSocket itself, the little that takes, and not through Qt's own: asked to sign on
// in some ways (NTLM, Negotiate), Qt's answers by itself with the account of whoever is signed in
// to Windows, names this PC in doing so, and asks again for as long as it is asked, and offers no
// way to be told not to (tried: core/tests/test_kvmd_device.cpp). Here the asking for the line is
// answered by "you are in" or the line is over.
//
// The pass goes out with the asking. So over TLS the certificate is looked at first, when the two
// have agreed on how to speak and before a byte is written: trusted by Windows for the KVM's
// name, or exactly the one the KVM is known by. That is the driver's rule, by the driver's own
// code for it (kvm_certificate.h): on the line of a KVM that is known by a certificate Windows is
// given nobody to vouch for anybody, what it objects to is passed over only where every objection
// names that certificate itself, and a KVM that sends its signer's certificate along is refused as
// the driver refuses it. Straight to the KVM, whatever proxy this PC uses.
// TLS is Windows' own, chosen before the line's socket is made (tls.h); where that is not what
// the program speaks, a line to an HTTPS address ends before anything goes out.
//
// On the thread it was made on. What it says, it says from the middle of its own work, also from
// inside say(): it may be told to close() from there, and is not thrown away from there
// (deleteLater() is).
class DeviceLine : public QObject {
    Q_OBJECT

public:
    // `asking` and `cookie` go into the asking as they are, so they are held to what cannot split
    // it: visible ASCII and no blank in either; the asking a path (it begins with "/"); the cookie
    // one name (letters, digits, "_", "-"), "=", and what kvmd::isToken takes as a pass. A line
    // for anything else is not asked for at all (End::NotAsked). (No cookie: a server's live line,
    // whose first message says who asks, and a line asked for withoutCookie.)
    struct Where {
        QUrl address;      // the KVM's: http or https, its host, its port where it has one of its own
        QByteArray asking; // the path of the line, with what stands behind its question mark
        QByteArray cookie; // what the asking carries to say who asks
        bool knownByCertificate = false;
        QByteArray certificate; // its SHA-256
        // Asked for with nothing to say who asks (a JetKVM that runs without a password): `cookie` is
        // empty, and the asking carries none. Otherwise a line with no cookie is not asked for.
        bool withoutCookie = false;
    };

    // What a line is held to. A test makes the times short.
    struct Limits {
        int letInWithinMs = 4500; // from open() to "you are in"
        // The most that may wait to go to the KVM: one that takes nothing off the line is gone,
        // and what is said to it is not kept on this PC without end.
        qint64 mostUnsentBytes = qint64{64} * 1024;
        int goodbyeWithinMs = 2000; // how long a line that was closed from here may take to go
        // The longest that anything written may wait in this program to leave for the KVM before
        // the next text is said: where something has waited longer, that text is not said, and
        // the line ends (say()).
        int lateAfterMs = 250;
        // The longest message, or part of one, that is taken from a KVM, and the most bytes that
        // are taken and thrown away in a second: a KVM says a few kilobytes on this line when it
        // is let in, and little after that. More ends the line, as more messages in a second
        // than a KVM has reason to send do.
        qint64 largestFrameBytes = qint64{64} * 1024;
        qint64 mostBytesASecond = qint64{1024} * 1024;
        int mostMessagesASecond = 2000;
    };

    // Why a line ended by itself.
    enum class End {
        NotAsked,                   // what the asking was to carry is not fit to be sent: nothing went anywhere
        TlsDamaged,                 // the KVM's address is an HTTPS one, and TLS is not Windows' own: nothing went
        NotReached,                 // no line to the KVM came about
        CertificateUnread,          // what is saved as its certificate is none
        CertificateChanged,         // it is known by a certificate, and showed another
        CertificateUntrusted,       // it is known by none, and Windows does not trust what it showed
        CertificateOfUnknownSigner, // it showed the one it is known by, with a signer's that Windows does not know
        Silent,                     // it took the asking for the line and did not answer in time
        SignedOut,                  // it answered that whoever asks is not signed in
        NotLetIn,                   // it answered anything else that is not "you are in"
        Fell,                       // the line fell, or the KVM said goodbye
        Misbehaved,                 // it sent what a KVM has no reason to send
        TakesNothing,               // more waits to go to it than may
        Late,                       // what was said to it has waited to leave for longer than may
    };
    Q_ENUM(End)

    DeviceLine(Where where, Limits limits, QObject* parent = nullptr);
    ~DeviceLine() override;
    DeviceLine(const DeviceLine&) = delete;
    DeviceLine& operator=(const DeviceLine&) = delete;
    DeviceLine(DeviceLine&&) = delete;
    DeviceLine& operator=(DeviceLine&&) = delete;

    // Once: a second call does nothing, and neither does one after close() or cut(). `ended` is
    // never said from inside it.
    void open();
    // Before open: opt in to bounded UTF-8 text reception and require this subprotocol.
    void receiveText(QByteArray protocol);
    // Before open: the same on a line that has no subprotocol (a JetKVM's signalling). It is then held
    // to what a server's live line is held to: "you are in" in WebSocket's own words, and no option
    // (a subprotocol, an extension) that nobody asked for.
    void receiveText();
    // Before open: read messages of bytes (handed out whole by bytesCame, each no longer than
    // Limits::largestFrameBytes, in one part or in several), and require this subprotocol in the
    // answer. A text ends the line (Misbehaved). The asking says it comes from a page of the KVM's own
    // (Origin: its address), as a browser that has that page open would: a management controller's
    // console is asked for so.
    void receiveBytes(QByteArray protocol);
    // Before open: on a line that otherwise throws away what comes, texts of `mostBytes` or fewer that come in one
    // part are handed out whole after all (textCame): what a KVM says of itself on its line for events, its power
    // board's state among it (kvmd's /api/ws: some ninety bytes on a real Comet). Longer texts, bytes and messages in
    // parts still go unread, and nothing else the line is held to changes. Not on a line that reads texts or bytes.
    void hearShortTexts(qint64 mostBytes);
    // Server live v2: no cookie, X-QKVM only in the upgrade; bounded text and binary
    // messages, including fragmentation. The one-use ticket is sent after letIn.
    void receiveLive();
    bool ping();
    // Takes nothing more off the line while `held`: a reader that has its fill holds the KVM back so (a server's
    // console whose frames wait to be read on a thread of their own, as they waited while the interface thread read
    // them). Said from inside bytesCame, the message under way is the last handed out; what comes after it waits, in
    // this program no more than the line takes of one message (Limits::largestFrameBytes), then with Windows, then
    // with the KVM, which is held back by that. Let go, what waited is handed out, a turn of the loop later and never
    // from inside this. Nothing else of the line changes: its times go on, what is said goes out, and a line closed
    // from here is read for its goodbye whatever this says. (Nor is "are you there" answered meanwhile: a KVM that asks
    // waits, as it waited while the screen was read on the line's own thread.)
    void holdBack(bool held);
    // The KVM has let it in, and the line has not ended since.
    bool isIn() const { return m_in; }
    // Says one text, as one message, and hands it to Windows there and then. Only on a line the
    // KVM has let in: before that and after its end nothing is sent, and the answer is false.
    // So it is where the text would be more than may wait to go, and where something written
    // before it (a text, an answer to "are you there") has waited in this program for longer than
    // Limits::lateAfterMs: the line ends there and then (`ended`, from inside this call), and
    // what waited is thrown away with it. A text that cannot go now does not go later.
    //
    // That is as far as this program sees, and it is not far. What Windows has taken for the
    // line and not yet got to the KVM is Windows': tens of kilobytes on a line that stalls, which
    // is hundreds of keys, and nothing waits in this program before Windows has that much. It is
    // not counted here and its age is not known here. While the line stands it is not taken
    // back: a way that comes back delivers it, late. It goes with the line where the line is
    // cut() or ends by itself (`ended`): the line is reset then, and Windows throws away what
    // it held for it. Only after close(), which pushes out what was said, Windows goes on
    // delivering. That nothing came for too long is noticed by whoever watches the line (`heard`).
    bool say(const QByteArray& text);
    // The same for a message of bytes, which a KVM tells from a text by its kind.
    bool sayBytes(const QByteArray& bytes);
    // How long the oldest of what was said has waited in this program to leave for the KVM, in milliseconds: 0 where
    // nothing waits (what Windows took left at once, as far as this program sees). For whoever says some things
    // within a shorter time than Limits::lateAfterMs and others within that (a server's console: keys and the
    // pointer are not said behind what has waited two seconds, while the console goes on).
    qint64 waitedMs();
    // Ends the line from here: goodbye in WebSocket's word where the KVM had let it in, behind
    // what was said before that, and all of it offered to Windows there and then. Then the KVM
    // answers with its own goodbye and ends the line, as the book has it, and that is waited
    // for, for Limits::goodbyeWithinMs: a line the KVM has not ended by then is reset as a cut
    // one is, and what Windows still held for it goes with it, as does what Windows had not
    // taken and still waited in this program. Where something has waited to leave for longer
    // than may, nothing is pushed out and no goodbye said: cut(). `ended` is not said for any of
    // it; `gone` is, when it is over, with whether the KVM's goodbye came: the one sign there is
    // that what was said arrived.
    //
    // That is for a line that is kept until then. One that is thrown away in the same breath is
    // neither waited for nor reset: Windows goes on delivering what it took, the last that was
    // said and whatever was said before it, for as long as the KVM takes it. What had come from
    // the KVM and was not read is taken off such a line before it goes: Windows resets a line
    // that is closed over what was not read from it, and a far end may then lose what it had not
    // taken yet (seen: a far end on this PC lost the last that was said, every time). Set, and
    // not shown by a test: what the KVM says after that finds no line, and is answered with a
    // reset all the same. So whoever is done with a line keeps it until it has gone.
    //
    // True: goodbye was said behind what was said before it, all of it was offered to Windows,
    // and the line stood after that. That is not "it has left": what Windows did not take at
    // once waits in this program, goes on leaving while the line is kept, and goes with the line
    // where that is reset; and of what Windows took nobody here knows how far it got. Whether
    // it arrived, `gone` says. False: no goodbye was said and nothing offered; the line was
    // reset in here, or had ended, or was closed before.
    bool close();
    // Ends the line from here without a word: no goodbye, and what waits to leave is thrown
    // away, in this program and with Windows: the line is reset. For a line that is taken to be
    // dead. `ended` is not said for that.
    void cut();

signals:
    void letIn();
    void textCame(const QByteArray& text);
    void bytesCame(const QByteArray& bytes);
    // Something came from the KVM on a line it has let in, a message or a part of one, and was
    // thrown away: said once for all that was read in one go, and nothing of what came is handed
    // out. For whoever watches a line for silence. (Not said where what came ended the line.)
    void heard();
    // Once, and not after close() or cut(). The line is reset by then, as one that was cut.
    void ended(qkvm::core::DeviceLine::End why);
    // Once, after close() and never from inside it: nothing more comes of the line. The KVM has
    // ended it, or its time was up and it was reset, or it had ended before it was closed. For
    // whoever keeps a closed line until then; it is not thrown away from inside this
    // (deleteLater() is).
    // `arrived`: the KVM said its own goodbye after the one from here, which is its answer to
    // that one: it got the goodbye, and with it all that was said before. (A goodbye of its own
    // accord that crossed the one from here on the way is taken for the answer: a guess.) False:
    // the line fell without that, or was reset when its time was up, or was reset in the
    // closing, or had ended before: what was said last may not have arrived.
    void gone(bool arrived);

private:
    // What was written up to a place in all that was ever written, and when.
    struct Written {
        qint64 upTo = 0;
        qint64 atMs = 0;
    };

    bool sayAs(quint8 kind, const QByteArray& what);
    void ask();
    void read();
    // What the answer to the asking says: nothing where it is "you are in".
    std::optional<End> refusedBy(const QByteArray& head) const;
    // Works through what has come; says why where the line is over for it.
    std::optional<End> throwAway();
    // Counts into the second that is now what came in it: so many messages, so many bytes. Whether
    // that is more of either than a KVM has reason to send.
    bool tooMuch(int messages, qint64 bytes);
    // Hands bytes to the socket, and notes when: every byte goes through here.
    void write(const QByteArray& bytes);
    // Lets go of the notes of what has left since.
    void forgetWhatLeft();
    // Whether something that was written has waited to leave for longer than may.
    bool late();
    // What waits to go to the KVM, in the clear and, over TLS, hidden already.
    qint64 unsent() const;
    // Why a line that fell or failed did.
    End fallen() const;
    void end(End why);
    // The socket says that the line is no more: its end, or, on a line that was closed from
    // here and waits for the KVM to end it, what was waited for.
    void fell();
    void sayGone();
    // Lets go of the line at once, and has Windows throw away what it holds for it: how this
    // program ends every line that it does not close with a goodbye.
    void reset();

    Where m_where;
    Limits m_limits;
    // Windows' own TLS, as the driver of the same KVM has it: the same trust on both lines.
    // Chosen here, before the socket below is made: a socket takes Qt's own pick of TLS when it
    // is made (OpenSSL, where one happens to be on the PC), and the choice would come too late
    // where a line is the first thing the program makes that speaks. Where it is not what is in
    // use, a line to an HTTPS address is not asked for (End::TlsDamaged).
    const bool m_windowsOwnTls = useWindowsOwnTls();
    QSslSocket m_socket;
    QElapsedTimer m_clock; // a clock that only goes forward, for what is counted and timed in here
    QByteArray m_key;
    QByteArray m_protocol;
    QByteArray m_text;
    int m_fragmentedKind = 0;
    bool m_live = false;
    bool m_receivesText = false;  // receiveText, with a subprotocol or without
    bool m_receivesBytes = false; // receiveBytes
    qint64 m_hearsUpTo = 0;       // hearShortTexts; 0: none are
    bool m_opened = false;
    bool m_asked = false;
    bool m_in = false;
    bool m_over = false;
    // close() was called; the goodbye went out and the KVM's ending of the line is waited for;
    // what comes meanwhile is still read for the KVM's own goodbye; that came; and `gone` was
    // said.
    bool m_closed = false;
    bool m_leaving = false;
    bool m_awaitsGoodbye = false;
    bool m_answered = false;
    bool m_gone = false;
    // What was found wrong with its certificate, where the line has not ended for it yet.
    std::optional<End> m_objection;
    qint64 m_toSkip = 0;     // what is left of the message under way, which is thrown away unread
    bool m_heldBack = false; // holdBack
    // The second that is being counted in, and the messages and bytes that came in it.
    qint64 m_second = -1;
    int m_heard = 0;
    qint64 m_heardBytes = 0;
    // How many bytes were ever written; and of what may not have left yet, up to where it was
    // written when (one note for each millisecond in which something was). Never more notes than
    // there are bytes that may wait: what has left is forgotten with every writing.
    qint64 m_written = 0;
    QList<Written> m_waiting;
};

} // namespace qkvm::core
