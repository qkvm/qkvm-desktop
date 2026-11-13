#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

namespace qkvm::core {

// One machine, live: its pictures as they come, and keyboard and mouse to it. Whoever gives it:
// a QKVM server over a line of its own (LiveLink, SPEC.md section 6.2), or a KVM of this PC's own
// (KvmdLive, section 6.3). Whoever shows a machine live asks the place its wall's machines come
// from for one (WallSource::openLive), and cannot tell which of the two it holds.
//
// It is live from when it is handed over. From then on it says what comes (`videoChanged`,
// `pictureCame`, `stateChanged`, `controlChanged`), and at its end, once, why it ended (`closed`).
// It is never live a second time: whoever wants the machine live again asks for another.
//
// Pictures are handed over as they come, none kept.
//
// Keys and the mouse go out only while this one holds them (`controlChanged`). Before they are
// given back, and before it is ended from here, everything that is down on the machine is let go.
// Where that could not be done, it says so (`leftDown`).
//
// On the interface thread. What it says, it says from the middle of its giver's own work: it may
// be told to `close()` from there, and is not thrown away from there (`deleteLater()` is).
class Live : public QObject {
    Q_OBJECT

public:
    enum class Pointer { Unknown, Absolute, Relative, Both };
    // Direct devices prepare the selected output. Servers advertise supported modes.
    virtual Pointer pointerMode() const { return Pointer::Both; }
    // How much of the picture whoever shows it needs: no wider than this, in the monitor's own
    // pixels, and no more pictures a second.
    struct Need {
        int maxWidth = 1920;
        int maxFps = 30;

        bool operator==(const Need&) const = default;
    };
    // What the pictures are: said at the start, and whenever it changes.
    struct Video {
        bool h264 = false; // otherwise JPEGs, each a whole picture
        int width = 0;
        int height = 0;
        // ...or a screen as what changed of it (base/screen_changes.h: a server's, ServerLive), the whole screen
        // first; drawn by the engine as its Codec::Rgb. Then h264 is false. (Last: what is made of the fields in
        // their order stays as it was.)
        bool changes = false;

        bool operator==(const Video&) const = default;
    };
    // One picture as it came.
    struct Picture {
        bool h264 = false;
        bool keyFrame = false;
        // One higher for each picture that was sent to this one.
        quint32 counter = 0;
        // When its giver got it, in microseconds on a clock of the giver's that only goes forward
        // (a server's own; for a KVM of this PC's, this PC's steady clock): for telling how long
        // a picture was on its way, against the one before.
        qint64 takenUs = 0;
        QByteArray data;
        // When it became whole on this PC, on this PC's steady clock, where whoever gave it stamped
        // that as it happened (a WebRTC receiver does, on its library's thread, before the picture
        // waits for the interface thread); 0 where nobody did, and whoever shows it stamps it as it
        // comes. What the engine counts as its arrival.
        qint64 arrivedUs = 0;
        // What changed of a screen (Video::changes), h264 false; a key frame where it is the whole screen.
        bool changes = false;
    };

    ~Live() override = default;
    Live(const Live&) = delete;
    Live& operator=(const Live&) = delete;
    Live(Live&&) = delete;
    Live& operator=(Live&&) = delete;

    // It is there: pictures can come, and what is said to it goes somewhere.
    virtual bool isOpen() const = 0;
    // Observed transport, and why an alternate path is being used. Empty if unknown.
    virtual QString transport() const { return {}; }
    virtual QString fallbackReason() const { return {}; }
    // The renderer could not decode this transport in its allotted time.
    virtual void videoFailed() {}
    // Pictures that came and were let go before they were handed over here, for want of room while the
    // interface thread was busy (a WebRTC mailbox's: PictureBox::Counts::letGoBehind), since this one
    // was handed over: shown beside what the engine dropped behind.
    virtual quint64 picturesLetGo() const { return 0; }
    // Whether a line that ended (`closed`) with this code is opened again only when a person does
    // something: a word of a newer server that this program does not know (Part D of 2026-10-26).
    // The words every kind of line shares are the caller's to know (ui/live_picture.cpp).
    virtual bool endsForGood(const QString& code) const
    {
        static_cast<void>(code);
        return false;
    }
    // The tile is another size now: said where it is another need.
    virtual void setNeed(Need need) = 0;
    // Whoever decodes the pictures has lost its place (a picture was thrown away, the graphics
    // device was lost): a key frame is asked for, where the pictures are not each a whole one.
    virtual void askForAKeyFrame() = 0;
    // Keyboard and mouse: asked for, or given back. Whether this one holds them is its giver's
    // to say (`controlChanged`); until then it does not. Each asking and each giving back is to
    // be answered by one word, in the order they were said: a KVM of this PC's own does so
    // (kvmd_live.h); a server's line passes on what the server says, which may be less. And no
    // word comes of a live that has ended (`closed` was its last). Whoever asks sets its own
    // time for the answer.
    virtual void askControl(bool on) = 0;
    // A server reports the other holder after refusing control. This is its last report,
    // not a claim that the holder is continuously observed. Local shared windows are known.
    struct ControlOwner {
        QString name;
        QString reference;
        bool anotherWindow = false;

        bool operator==(const ControlOwner&) const = default;
    };
    virtual ControlOwner controlOwner() const { return {}; }
    // Only an explicit, confirmed request may use this. Normal requests never take over.
    virtual void takeOver(const QString& /*expectedOwner*/) {}
    // The kind of pointer whoever shows it would use, chosen before asking for control: where both kinds are taken
    // (pointerMode() is Both: a server's line). A KVM of this PC's own takes the kind its mouse is set to, which is
    // followed and never switched (kvmd_live.h), and pays no heed to this.
    virtual void chooseRelativeMouse(bool /*relative*/) {}
    virtual bool hasControl() const = 0;
    // A key, by its USB HID usage (page 7); the pointer, absolute (each 0 to 32767 over the
    // picture) or by how far it moved; a button, 1 to 5; the wheel; and letting go of everything
    // that is down. Nothing is sent while this one does not hold keyboard and mouse.
    virtual void key(int usage, bool down) = 0;
    virtual void pointerAt(int x, int y) = 0;
    virtual void pointerBy(int dx, int dy) = 0;
    virtual void button(int number, bool down) = 0;
    virtual void wheel(int dx, int dy) = 0;
    virtual void releaseAll() = 0;
    // Ends it from here, letting go first of what is down: `closed` is not said for that.
    virtual void close() = 0;

signals:
    void controlOwnerChanged();
    void pointerModeChanged();
    void opened();
    void transportChanged();
    void videoChanged(const qkvm::core::Live::Video& video);
    void pictureCame(const qkvm::core::Live::Picture& picture);
    // How the machine is, as in the list of how a wall's machines are, the moment it changes.
    void stateChanged(const QString& state, const QString& errorCode, const QString& errorMessage);
    // Whether this one holds keyboard and mouse; where they were taken away or not given, a code
    // and a sentence for why; where they went with the live itself, why that ended.
    void controlChanged(bool on, const QString& code, const QString& message);
    // Keyboard and mouse went from this one, and something it had pressed may still be down on
    // the machine: it could not be let go from here any more, or the word for it may have gone
    // with the way it went by. The machine's own end is to let go of it then, and nothing from
    // here can make it. Said whichever way they went: taken away, given back, with the live
    // itself, and also after `close()`, a turn of the loop later, to whoever still holds this
    // then. Where a `controlChanged(false)` is said for the same end, this is said just before
    // it; or seconds after it, where it shows only then that what was let go may not have
    // arrived (a KVM's line that went without the KVM's goodbye: kvmd_live.h). Whoever gives
    // the live says where it can know this at all, and where it never says it.
    void leftDown();
    // It is over, by itself: why, in the words of whoever gave it.
    void closed(const QString& code, const QString& message);

protected:
    explicit Live(QObject* parent = nullptr) : QObject(parent) {}
};

} // namespace qkvm::core
