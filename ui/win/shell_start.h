#pragma once

namespace qkvm::ui {

// Starts this program again, with no argument, from its own folder, through the person's own shell: the desktop's
// Shell.Application (IShellDispatch2::ShellExecute), so that the new process is a child of the person's Explorer, at
// its integrity, and takes nothing of whoever started this one (Setup's mitigations among it). Whether it was started.
// Never called by a test: a test hands whileSetupWorks a starter of its own.
bool startAnewThroughTheShell();

// Whether the process of this number runs as the person this one runs as (its token's user is this process's), and
// not above them (not elevated, at medium integrity or below): what the shell has to be for this program to be started
// through it (review C1). False where any of that cannot be read.
bool processIsThePersons(unsigned long processId);
// The same of two tokens (HANDLEs, with TOKEN_QUERY): `theirs` is of `ours`'s user, and not above them.
bool tokenIsThePersons(void* theirs, void* ours);

} // namespace qkvm::ui
