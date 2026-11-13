#pragma once

#include <cstdint>
#include <set>

#include <QObject>
#include <QTimer>

namespace qkvm::ui {

// The keys that were kept from Windows while a machine was controlled and were not heard to go up
// since: fingers may still be on them, Windows does not know that they are down, and a listening
// that begins anew did not hear them go down. What is physically down is one thing for the whole
// program, whichever window's keys heard of it: one of these is shared by the controls of all the
// windows (WallWindows owns it, and hands it to each MachineControl). A key swallowed in one
// window is known in the next one that takes keyboard and mouse, and its going up, heard by
// whichever window's keys, is known to all.
//
// A key stays here until it was heard to go up, or seen to go down anew, however long that is
// while a machine is controlled. While none is, no longer than a minute: no finger stays on a key
// for a minute, the listening itself ends by then, and nothing of the keyboard is kept by a
// program that controls nothing.
//
// By their USB numbers: which keys, and nothing else. On the interface thread.
class KeysKept : public QObject {
    Q_OBJECT

public:
    // `forgetAfterMs`: for how long a key is remembered while no machine is controlled.
    explicit KeysKept(int forgetAfterMs = 60'000, QObject* parent = nullptr);

    // A key went down and was kept from Windows.
    void keep(std::uint16_t usage);
    // It was heard to go up, by whoever; or it was seen to go down anew, so it had gone up.
    void letGo(std::uint16_t usage);
    bool isIn(std::uint16_t usage) const { return m_down.contains(usage); }
    bool isEmpty() const { return m_down.empty(); }
    const std::set<std::uint16_t>& all() const { return m_down; }

    // A window holds keyboard and mouse of a machine from now on, and no longer: while one does,
    // nothing is forgotten for the time it took.
    void controlBegan();
    void controlEnded();

signals:
    void changed();

private:
    void forgetLater();

    std::set<std::uint16_t> m_down;
    int m_controlling = 0;
    QTimer m_forget;
};

} // namespace qkvm::ui
