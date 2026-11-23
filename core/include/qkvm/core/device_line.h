#pragma once

#include <memory>
#include <optional>

#include <QByteArray>
#include <QByteArrayView>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <qkvm/core/tls.h>

class QThread;

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
//
// Its socket is not on that thread (outside review 2.2). Windows judges a TLS server's certificate on the thread that
// speaks TLS, at the handshake and again after it: Qt judges anew at every session ticket of TLS 1.3, every key update
// and every renegotiation (measured: twice on every line to a server of Windows' own, four times on OpenSSL's after a
// key update). Building the certificate's chain, Windows fetches a signer it names from where it names (an AIA fetch),
// and on a PC without internet waits there for as long as it gives that: 17 s, measured, on the interface thread of a
// vncs:// session. So the socket, its TLS, and every look at a certificate it is shown, the driver's rule's among them,
// are on a thread of the line's own for its whole life (line_threads.h), and what they come to is handed here. The
// line's own work stays here, as its user sees it: what it says and when, the times it is held to, its ends. Nothing
// here waits for that thread: a line whose thread Windows holds still ends in its time. One that ends, is cut, or is
// thrown away neither closed nor cut goes at once, and what was said on it and waits goes with it, never handed on
// once the line has ended, but for the one message that thread may be handing over at that instant (SPEC.md section
// 8); one closed goes at once too, behind what was said before its close and its goodbye (close()). What it held goes
// once Windows lets the thread go. Review 2.6's rule holds there too (what has waited longer than
// Limits::lateAfterMs is never handed on). Qt hands what a socket holds to Windows by itself the moment Windows has
// room, at whatever turn of that thread's loop comes first, with no look at how long it waited: so every message that
// loop takes from Windows, and every call posted to the socket, is looked at first (what has waited too long ends the
// line there, Late, and it is reset; a line reset from here is reset there); the socket is given what was said one
// message at a time, and only while it holds nothing, so that where the thread is held or put aside between that look
// and Qt's own work no more than that one message can leave late; and what leaves is looked at as it leaves, and ends
// the line where it had waited too long. The asking alone is not held to that limit: it is no input, it may wait
// behind a judgement of the certificate after the handshake there, and Limits::letInWithinMs bounds it. Whatever a
// line reads, its socket holds no more of what came than a message's worth (more waits with Windows, which holds the
// device back), and its socket's thread takes no more off it than the line has room for. Where Windows still holds
// lines::mostHeldToOnePlace threads of earlier lines to the same place, a line to it is not reached (End::NotReached)
// until Windows lets go: each try would leave another thread (line_threads.h).
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
        // Who asks said as a Redfish session's token (X-Auth-Token), in place of a cookie: OpenBMC's console,
        // whose upgrade is signed in to as any request of its web server's. `cookie` is empty then, and the token is
        // what a controller may hand out to be sent back in a header (visible ASCII but for the quote, the comma, the
        // semicolon and the backslash, 512 at the most: the web's HANDED); a line with any other is not asked for.
        QByteArray authToken;
        // Who asks said in an Authorization header, in place of a cookie: a Proxmox guest's console, whose upgrade is
        // signed in to with the API token as every question of its API is (`PVEAPIToken=<id>=<secret>`). `cookie` and
        // `authToken` are empty then, and this is visible ASCII with no blank, 512 at the most; a line with any other
        // is not asked for. (Never in the asking's address: the ticket that goes there is not the token.) The line
        // holds it for its upgrade alone: it is written over once the upgrade went, and when the line goes without
        // one; whoever hands one in hands in the one copy of it there is (RfbScreen::entered).
        QByteArray authorization;
    };

    // What a line is held to. A test makes the times short.
    struct Limits {
        int letInWithinMs = 4500; // from open() to "you are in"
        // The most that may wait to go to the KVM: one that takes nothing off the line is gone,
        // and what is said to it is not kept on this PC without end.
        qint64 mostUnsentBytes = qint64{64} * 1024;
        int goodbyeWithinMs = 2000; // how long a line that was closed from here may take to go
        // The longest that anything said on it (not its asking) may wait in this program to leave for the KVM: where
        // something has waited longer, the line ends there and then (`ended`, Late), whether or
        // not anything is said after it, and what waited goes with it, never handed on later (a
        // key from then pressed now: outside review 2.6). And a text said behind it is not said.
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
        SignedOut,                  // it answered that whoever asks is not signed in (HTTP 401)
        Refused,                    // it answered that whoever asks may not have this line (HTTP 403)
        NotLetIn,                   // it answered anything else that is not "you are in"
        Fell,                       // the line fell, or the KVM said goodbye
        Misbehaved,                 // it sent what a KVM has no reason to send
        TakesNothing,               // more waits to go to it than may
        Late,                       // what was said to it has waited to leave for longer than may
    };
    Q_ENUM(End)

    DeviceLine(Where where, Limits limits, QObject* parent = nullptr);
    // Thrown away (deleted, or deleteLater) neither closed nor cut, a line goes as a cut one does: what was said and
    // not handed to Windows goes with it, its asking with who asks in it among that, and it is reset, at once or the
    // moment Windows lets its socket's thread go (outside review 2, 2.2). One closed first goes behind what was said
    // before its close and its goodbye (close() says what that leaves open).
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
    // Before open: the same with no subprotocol (OpenBMC's console asks for none). It is then held to what a
    // server's live line is held to: "you are in" in WebSocket's own words, and no option nobody asked for.
    void receiveBytes();
    // Before open: on a line that otherwise throws away what comes, texts of `mostBytes` or fewer that come in one
    // part are handed out whole after all (textCame): what a KVM says of itself on its line for events, its power
    // board's state among it (kvmd's /api/ws: some ninety bytes on a real Comet). Longer texts, bytes and messages in
    // parts still go unread, and nothing else the line is held to changes. Not on a line that reads texts or bytes.
    void hearShortTexts(qint64 mostBytes);
    // Before open: no WebSocket at all. The line is RFB's own TCP connection to a VNC server (`vnc://` in the clear,
    // `vncs://` TLS from the first byte: `address` is http or https, its host and its port), in once it is up (over
    // TLS once its certificate was looked at as every line's is), with nothing asked and nothing said for it, and
    // nothing of Where but the address taken. What comes is handed out as it comes (bytesCame), in pieces of
    // Limits::largestFrameBytes at the most, each counted as a message of the second it came in; what is said goes as
    // it is, held to everything else a line is held to. There is no "are you there" (ping says nothing). Closed from
    // here, what was said is pushed out and the connection ended, and `gone` said once it has gone, or after
    // Limits::goodbyeWithinMs (`arrived` is false: RFB has no goodbye to answer one).
    void receiveRaw();
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
    // waits, as it waited while the screen was read on the line's own thread.) What waits in this program is what the
    // socket's thread took off the line and has not handed here, and what it handed here: each no more than the line
    // takes of one message.
    void holdBack(bool held);
    // The KVM has let it in, and the line has not ended since.
    bool isIn() const { return m_in; }
    // The KVM said WebSocket's goodbye on it (of its own accord, or in answer to one from here): a line that ended so
    // (End::Fell) was closed by the KVM, and did not fall without a word. For whoever tells the two apart (a Proxmox
    // guest's console, whose line that falls is a failure of contact, and whose goodbye is none: review cx3-3, L3).
    bool saidGoodbye() const { return m_saidGoodbye; }
    // Says one text, as one message, and has the socket's thread hand it to Windows at once. Only on a line the
    // KVM has let in: before that and after its end nothing is sent, and the answer is false.
    // So it is where the text would be more than may wait to go, and where something written
    // before it (a text, an answer to "are you there") has waited in this program for longer than
    // Limits::lateAfterMs: the line ends there and then (`ended`, from inside this call), and
    // what waited is thrown away with it, never offered to Windows first. A text that cannot go
    // now does not go later. (Where nothing is said after it, the line ends by itself all the same,
    // at the moment what waits has waited too long.)
    //
    // That is as far as this program sees, and it is not far. What Windows has taken for the
    // line and not yet got to the KVM is Windows': tens of kilobytes on a line that stalls, which
    // is hundreds of keys, and nothing waits in this program before Windows has that much. It is
    // not counted here and its age is not known here. While the line stands it is not taken
    // back: a way that comes back delivers it, late. It goes with the line where the line is
    // cut() or ends by itself (`ended`): the line is reset then (by its socket's thread, at once,
    // or the moment Windows lets that thread go: nothing more is handed to Windows meanwhile),
    // and Windows throws away what it held for it (but where that thread learns of it in Windows' judgement of the
    // certificate after the handshake: Qt closes the line there itself, and what Windows held still goes; SPEC 8).
    // Only after close(), which pushes out what was said, Windows goes on delivering. That nothing came for too long
    // is noticed by whoever watches the line (`heard`).
    bool say(const QByteArray& text);
    // The same for a message of bytes, which a KVM tells from a text by its kind.
    bool sayBytes(const QByteArray& bytes);
    // How long the oldest of what was said has waited in this program to leave for the KVM, in milliseconds: 0 where
    // nothing waits (what Windows took left at once, as far as this program sees). For whoever says some things
    // within a shorter time than Limits::lateAfterMs and others within that (a server's console: keys and the
    // pointer are not said behind what has waited two seconds, while the console goes on).
    qint64 waitedMs();
    // Where all that was said so far ends, as a mark in all that is ever said on this line; and whether what was said
    // up to a mark has left this program for the KVM (as far as this program sees: Windows took it). For whoever has to
    // know of one thing that it left, whatever was said after it (a server's console: a key let go of).
    qint64 mark() const noexcept { return m_written; }
    bool hasLeft(qint64 mark) const;
    // Ends the line from here: goodbye in WebSocket's word where the KVM had let it in, behind
    // what was said before that, all of it handed to the line's socket's thread, which offers it to Windows at once (a
    // raw line has no goodbye: what was said goes, and its connection is ended behind it there). Then the KVM
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
    // True: goodbye was said behind what was said before it, all of it was handed to the socket's thread, and the line
    // stood as that thread last saw it, before it was told (that thread may find it late, or fallen, a moment later:
    // the goodbye then never goes, and `gone` says false). That is not "it has left": what Windows did not take at
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
    // Over TLS, the certificate the line was let in by (DER), once its look at it passed: for whoever keeps the
    // certificate itself beside its fingerprint (a VNC server's driver, for the wall as a file). Just before letIn.
    void certificateShown(const QByteArray& der);
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
    // ...on a raw line: what came, handed out as it came.
    std::optional<End> takeRaw();
    // Counts into the second that is now what came in it: so many messages, so many bytes. Whether
    // that is more of either than a KVM has reason to send.
    bool tooMuch(int messages, qint64 bytes);
    // Hands bytes to the socket's thread, and notes when: every byte goes through here. (Qt may keep `bytes`
    // themselves, shared, until they have left; of `copied`, a copy is made for that thread, which writes it over once
    // Qt has made its own of it, and whoever made them may write them over at once: the asking, which says who asks.)
    void write(const QByteArray& bytes);
    void writeCopied(QByteArrayView copied);
    void hand(QByteArray bytes, bool wiped);
    void noteWritten();
    // Lets go of the notes of what has left since.
    void forgetWhatLeft();
    // Whether something that was written has waited to leave for longer than may.
    bool late();
    // The watch on what waits (m_lateWatch's): a line on which something has waited too long ends then, also where
    // nothing more is said on it.
    void lookWhetherLate();
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

    // What the socket's thread comes to, handed here (each a turn of this thread's loop after it, in its order).
    void wireConnected();
    void wireEncrypted(const QByteArray& der, bool changed);
    void wireCame();
    void wireLate();
    // What came is taken from the socket's thread into m_came; and how much of it waits here is told back, with room
    // asked for where that thread waits for it.
    void takeWhatCame();
    void gaveRoom();
    // What this line has come to read: what came and was not read yet.
    qint64 available() const { return m_came.size() - m_cameAt; }
    QByteArray peekCame(qint64 most) const;
    QByteArray readCame(qint64 most);
    qint64 skipCame(qint64 most);
    // What the socket's thread found wrong with the certificate it was shown, where it found anything.
    std::optional<End> objection() const;
    // How the socket stood when its thread last looked.
    bool connectedNow() const;

    // The socket's thread's side (device_line.cpp): the socket, on that thread alone; and what the two share.
    class Wire;
    struct Shared;

    Where m_where;
    Limits m_limits;
    // Windows' own TLS, as the driver of the same KVM has it: the same trust on both lines.
    // Chosen here, before the socket is made: a socket takes Qt's own pick of TLS when it
    // is made (OpenSSL, where one happens to be on the PC), and the choice would come too late
    // where a line is the first thing the program makes that speaks. Where it is not what is in
    // use, a line to an HTTPS address is not asked for (End::TlsDamaged).
    const bool m_windowsOwnTls = useWindowsOwnTls();
    // Made at open(), with the socket's thread: what the two share; the Wire, until this line tells it to end (its
    // end, on its own thread, ends that thread).
    std::shared_ptr<Shared> m_shared;
    Wire* m_wire = nullptr;
    QThread* m_thread = nullptr; // the Wire's, until this line lets go of it (lines::letGo)
    bool m_reset = false;        // reset from here: what still comes is nobody's
    // What came, from m_cameAt on: handed here by the socket's thread, not read yet.
    QByteArray m_came;
    qsizetype m_cameAt = 0;
    QElapsedTimer m_clock; // a clock that only goes forward, for what is counted and timed in here
    QByteArray m_key;
    QByteArray m_protocol;
    QByteArray m_text;
    int m_fragmentedKind = 0;
    bool m_live = false;
    bool m_receivesText = false;  // receiveText, with a subprotocol or without
    bool m_receivesBytes = false; // receiveBytes, and receiveRaw
    bool m_raw = false;           // receiveRaw
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
    bool m_saidGoodbye = false; // saidGoodbye
    qint64 m_toSkip = 0;        // what is left of the message under way, which is thrown away unread
    bool m_heldBack = false;    // holdBack
    // The second that is being counted in, and the messages and bytes that came in it.
    qint64 m_second = -1;
    int m_heard = 0;
    qint64 m_heardBytes = 0;
    // How many bytes were ever written; and of what may not have left yet, up to where it was
    // written when (one note for each millisecond in which something was). Never more notes than
    // there are bytes that may wait: what has left is forgotten with every writing.
    qint64 m_written = 0;
    QList<Written> m_waiting;
    // Set while anything written may still be waiting: at the moment the oldest of it would be late.
    QTimer m_lateWatch;
};

} // namespace qkvm::core
