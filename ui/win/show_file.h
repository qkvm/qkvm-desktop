#pragma once

#include <QString>

namespace qkvm::ui {

// A window of Windows' Explorer on the folder a file is in, with that file chosen (Windows' own
// SHOpenFolderAndSelectItems). The file itself is never opened, run or unpacked here. True once Windows took it.
bool showFileInItsFolder(const QString& file);

} // namespace qkvm::ui
