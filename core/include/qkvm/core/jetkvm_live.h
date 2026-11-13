#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QPointer>
#include <QString>

#include <qkvm/base/result.h>
#include <qkvm/core/jetkvm_connection.h>
#include <qkvm/core/own_device.h>
#include <qkvm/core/typed_keys.h>

namespace qkvm::core {

class JetKvmViewer;

// A JetKVM live, for all who show it large (M6 2g; plan 1.8): its viewer's one connection, shared, as a
// kvmd's KvmdLive shares its own (without the JPEG half: a JetKVM sends H.264 only). Each window has every
// picture from the next key frame after it came. Where the program stands aside (another viewer took the
// JetKVM), each window is told `closed("in_use")` once, which ends it for good until "Show here"; one that
// asks meanwhile is told the same a turn after it holds it.
//
// Keyboard and mouse (the web's live.JetHid): one window holds them at a time (another that asks is told
// "in_use"), on the connection that was up with its keys ready when they were given (JetKvmViewer::inputReady),
// never carried over to the next: where that one is over, the holder is told `dropped`. The keyboard is kept here
// as the whole report the JetKVM takes (modifiers as bits, six keys, a seventh lets go of the first), the mouse
// with its buttons and its last place (a place, or steps), and each change goes to the connection as a report
// (JetKvmConnection::say), which sends it the JetKVM's way. The keyboard's Power key (0x66) is never sent. Given
// back, closed, or the live over: everything down is let go of first, behind what was said.
//
// Its size (Live::Video) is what its key frames' SPS say is shown, the frame's cropping taken off, read as the
// engine's SPS gate reads it (base/h264.h): said to each window with its first key frame, and to every window again
// when a key frame says another; 0 by 0 where none has said one yet. A window's pointer is placed over that (the
// review of the fitted view, 2026-11-06: with 0 by 0 it was placed nowhere, and no pointer went).
//
// On the interface thread.
class JetKvmLive : public OwnLive {
    Q_OBJECT

public:
    explicit JetKvmLive(JetKvmViewer& viewer, QObject* parent = nullptr);
    ~JetKvmLive() override;
    JetKvmLive(const JetKvmLive&) = delete;
    JetKvmLive& operator=(const JetKvmLive&) = delete;
    JetKvmLive(JetKvmLive&&) = delete;
    JetKvmLive& operator=(JetKvmLive&&) = delete;

    std::unique_ptr<Live> open(Live::Need need) override;
    void end(const base::Error& why) override;

private:
    class Handle;
    struct Watcher {
        Handle* handle = nullptr;
        bool told = false; // its video was said (at its first key frame)
        quint32 counter = 0;
    };
    friend class Handle;
    void leave(Handle* handle);
    void handOn(const QByteArray& annexB, bool key, qint64 arrivedUs);
    // Every window is told, a turn later, that it ended so.
    void closeAll(const QString& code, const QString& message);

    // Keyboard and mouse.
    void askControl(Handle* handle, bool on);
    bool holds(const Handle* handle) const;
    void key(int usage, bool down);
    void pointerAt(int x, int y);
    void pointerBy(int dx, int dy);
    void button(int number, bool down);
    void wheel(int dx, int dy);
    void releaseAll();
    void sayMouse(bool lettingGo);
    JetKvmConnection::Keyboard keyboardNow() const;
    void say(const JetKvmConnection::Report& report, bool fold = false, bool lettingGo = false);
    // Whoever held them holds nothing from here on, and is told why (a turn later; `leftDown` first where something
    // was down).
    void loseControl(const base::Error& why);
    void dropHolder();
    static void tellLater(Handle* handle, const QString& code, const QString& message, bool leftDown = false);

    QPointer<JetKvmViewer> m_viewer;
    std::vector<Watcher> m_watchers;
    std::pair<int, int> m_size; // what the last key frame said is shown
    bool m_over = false;
    QString m_overCode;
    QString m_overMessage;

    Handle* m_holder = nullptr;
    bool m_given = false;
    quint64 m_controlTurn = 0; // another whenever they are given back, so that an answer for an asking before is none
    QPointer<JetKvmConnection> m_on;
    quint64 m_onSerial = 0;
    quint64 m_holders = 0;      // a number for each holding, which the connection's reports are told apart by
    quint64 m_holderNumber = 0; // this holding's
    typed::Keyboard m_keyboard;
    int m_buttons = 0;
    bool m_placed = false; // put at a place (m_x, m_y), or moved by steps
    int m_x = 0;
    int m_y = 0;
};

} // namespace qkvm::core
