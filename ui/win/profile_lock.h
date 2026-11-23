#pragma once

#include <QString>

namespace qkvm::ui {

// The one copy of the program that writes a profile, in whichever Windows session of its account it runs (outside
// review of 0.1.2, C3): a lock that Windows holds on a file beside the settings (core::profile::runningFile) for as
// long as this copy keeps it, and lets go of when the copy ends, however it ends. A mutex of "Local\" is one session's
// alone, and an account signed in twice (at the PC and by Remote Desktop) has one profile: SingleInstance settles which
// copy of this account is the first in this session, and this which one writes the profile.
//
// The file holds nothing (SPEC.md section 11.2): it is there to be locked, and stays when the copy ends. Another copy
// may open it, as it must to ask for the lock; nobody may delete it while it is held.
class ProfileLock {
public:
    // `file`: the file beside the settings; its folder is made where it is not there yet (a first start).
    explicit ProfileLock(const QString& file);
    ~ProfileLock();

    ProfileLock(const ProfileLock&) = delete;
    ProfileLock& operator=(const ProfileLock&) = delete;
    ProfileLock(ProfileLock&&) = delete;
    ProfileLock& operator=(ProfileLock&&) = delete;

    // This copy holds the lock: it is the one that writes the profile.
    bool taken() const { return m_file != nullptr; }
    // Another copy holds it: one of this account in another Windows session (or a setup at work that holds it).
    bool heldElsewhere() const { return m_held; }
    // Where it is not taken, why, in plain words (with Windows' number where Windows refused); empty where it is.
    QString whyNot() const;

private:
    // Windows' own handle of the file, kept open with the lock; nothing where it is not taken.
    void* m_file = nullptr;
    bool m_held = false;
    unsigned long m_error = 0;
    // The folder the file is in, as Windows writes it: what is said where it could not be taken.
    QString m_folder;
};

} // namespace qkvm::ui
