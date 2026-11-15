#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <QString>

namespace qkvm::ui {

// The name of the mutex qKVM's Setup holds while it runs (its SetupMutex), in the session of whoever is signed in.
inline constexpr char setupMutexName[] = "Local\\qKVM-Desktop-Setup";
// The event a qKVM Setup at work makes and keeps for as long as it runs, named after its mutex and its process's
// number ("Local\qKVM-Desktop-Setup-<n>"), so that the program can tell a Setup that runs from a mutex kept there by
// a handle elsewhere (review R2). packaging/qkvm-desktop.iss makes it, and tools/test_setup.ps1 holds the two alike.
std::wstring setupRunsName(const std::wstring& mutex, unsigned long process);
// The one argument Setup starts the program with (the contract with Setup: at the end of an update, and its Launch at
// the end of a setup). It means only that the program waits for Setup to be over and then starts itself anew; it
// skips nothing.
inline constexpr char afterUpdateArgument[] = "--after-update";

// What a start does while qKVM's Setup may be at work, before any window and before the single-instance mutex (a
// program that runs while Setup replaces its files keeps them from being replaced, and Setup gives up halfway).
enum class WhileSetupWorks {
    GoOn,          // no Setup (any more)
    BeingUpdated,  // Setup is at work, and this start is not Setup's: it ends at once, saying so
    StillUpdating, // Setup's own start, and Setup was still there after `most`: it ends, saying so
    StartedAnew,   // Setup's own start, Setup over, and the program started anew through the person's shell: it ends
    // Setup's own start that may not start the program: it runs above the person (elevated, or at an integrity above
    // medium), or the person's shell could not start it as the person this install is of. It ends, and starts nothing:
    // the person starts qKVM from the Start menu (review C1).
    NotStartedAgain,
};

// Whether a token of these two words runs above the person: elevated, or at an integrity whose number is above
// medium's (SECURITY_MANDATORY_MEDIUM_RID).
bool aboveThePerson(bool elevated, unsigned long integrity);
// Whether a token (a HANDLE, with TOKEN_QUERY) runs above the person (where that cannot be read: it does).
bool tokenRunsAboveThePerson(void* token);
// Whether this process runs above the person, by its own token.
bool runsAboveThePerson();

// Not `afterUpdate`: with no Setup at work, go on; with one at work, being updated, at once. A Setup is at work where
// its mutex `name` is there and a process that runs keeps its event setupRunsName(name, its number): a mutex alone,
// kept there by a handle elsewhere after Setup has ended, is none (review R2). `afterUpdate` (Setup's own start, while
// it may still hold the mutex): never goes on itself, whatever comes. Where it runs above the person
// (`aboveThePersonNow`), it ends at once, starting nothing. Otherwise the mutex is waited for, looking every 200 ms (it
// is opened and let go of at once, never held), at most `most`; then the program started anew without the argument
// (`startAnew`, through the person's own shell, so that it is no child of Setup's and takes none of its limits: Inno
// Setup's RedirectionGuard, inherited, keeps a program from crossing a junction the person made; and only as the person
// this install is of), and this start ends; where that could not be done (false), it ends all the same, starting
// nothing.
//
// `setup`, where given (a HANDLE that is signalled once Setup is over: its process, openTheStarter), is what Setup's
// own start waits for, at most `most`, in place of the mutex: the mutex stays there while any handle to it is kept, by
// anyone (review C4), Setup's process ends with Setup.
WhileSetupWorks whileSetupWorks(const QString& name, bool afterUpdate, std::chrono::milliseconds most,
                                const std::function<bool()>& startAnew,
                                const std::function<bool()>& aboveThePersonNow = &runsAboveThePerson,
                                void* setup = nullptr);

// The process that started this one (for Setup's own start: Setup's), where Windows still has it and it began before
// this one did, so that its number is still its own; nothing otherwise. A HANDLE that may be waited on, the caller's to
// close.
void* openTheStarter();

} // namespace qkvm::ui
