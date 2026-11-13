#pragma once

#include "file_door.h"

class QWindow;

namespace qkvm::ui {

// Windows' own boxes for where a file of the wall is saved and which one is opened, over one of
// the program's windows, which they hold until they are closed (over none where that window is
// gone). The box that opens a file shows the two kinds of file QKVM writes: .qkvm (encrypted)
// and .json (plain). The box that saves one offers only the kind of the file that is saved,
// which the name that is suggested for it says by its ending. On the interface thread; each
// comes back when its box is closed, with the file's whole name, with nothing where it was
// closed without one, and with why where Windows would not open it. The program goes on behind
// the box meanwhile.
FileDoor windowsFileDoor(QWindow* over);
// And Windows' own box for an image to carry to a machine's drive through its qKVM (MediaView): disk images (.iso,
// .img) first, and any file for whoever names one otherwise. The same in all else.
std::function<base::Result<QString>()> windowsImageDoor(QWindow* over);

} // namespace qkvm::ui
