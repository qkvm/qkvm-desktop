#pragma once

namespace qkvm::ui {

// Starts this program again, with no argument, from its own folder, through the person's own shell: the desktop's
// Shell.Application (IShellDispatch2::ShellExecute), so that the new process is a child of the person's Explorer, at
// its integrity, and takes nothing of whoever started this one (Setup's mitigations among it). Whether it was started.
// Never called by a test: a test hands whileSetupWorks a starter of its own.
bool startAnewThroughTheShell();

} // namespace qkvm::ui
