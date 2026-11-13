#include "show_file.h"

#include <QDir>
#include <QFileInfo>

// Windows' own headers.
#include <windows.h>

#include <objbase.h>
#include <shlobj.h>

namespace qkvm::ui {

bool showFileInItsFolder(const QString& file)
{
    const QFileInfo info(file);
    if (!info.isAbsolute() || !info.isFile()) {
        return false;
    }
    // (Qt has the interface thread in COM's single-threaded apartment already; this only counts one more use.)
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const auto native = QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString();
    bool shown = false;
    if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(native.c_str()); item != nullptr) {
        shown = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
        ILFree(item);
    }
    if (SUCCEEDED(apartment)) {
        CoUninitialize();
    }
    return shown;
}

} // namespace qkvm::ui
