#pragma once

#include <chrono>
#include <functional>

#include <QString>

namespace qkvm::ui {

// The name of the mutex qKVM's Setup holds while it runs (its SetupMutex), in the session of whoever is signed in.
inline constexpr char setupMutexName[] = "Local\\qKVM-Desktop-Setup";
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
};

// Not `afterUpdate`: with no mutex of `name` there, go on; with one there, being updated, at once. `afterUpdate`
// (Setup's own start, while it may still hold the mutex): the mutex waited for, looking every 200 ms (it is opened and
// let go of at once, never held), at most `most`; then the program started anew without the argument (`startAnew`,
// through the person's own shell, so that it is no child of Setup's and takes none of its limits: Inno Setup's
// RedirectionGuard, inherited, keeps a program from crossing a junction the person made), and this start ends. Where
// that could not be done (false), this start goes on.
WhileSetupWorks whileSetupWorks(const QString& name, bool afterUpdate, std::chrono::milliseconds most,
                                const std::function<bool()>& startAnew);

} // namespace qkvm::ui
