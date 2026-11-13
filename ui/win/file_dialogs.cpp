#include "win/file_dialogs.h"

#include <array>
#include <span>

#include <QPointer>
#include <QWindow>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace qkvm::ui {

namespace {

// Where a file is opened: the two kinds of file QKVM writes, and all files for whoever named one
// otherwise.
const std::array<COMDLG_FILTERSPEC, 2> kindsToOpen = {
    {{L"qKVM files (*.qkvm; *.json)", L"*.qkvm;*.json"}, {L"All files", L"*.*"}}};
// Where one is saved: the one kind that this file is, and no other. An encrypted file is not
// offered a plain one's name, nor a plain one an encrypted one's. (Whatever name comes back all
// the same is what it is here. The box for this PC's own wall puts it right before it writes
// (namedAsWhatItIs, file_door.h); the server's boxes write it as it comes, files_view.cpp.)
const std::array<COMDLG_FILTERSPEC, 1> encryptedKind = {{{L"qKVM file, encrypted (*.qkvm)", L"*.qkvm"}}};
const std::array<COMDLG_FILTERSPEC, 1> plainKind = {{{L"qKVM file, plain (*.json)", L"*.json"}}};
const std::array<COMDLG_FILTERSPEC, 1> pictureKind = {{{L"PNG image (*.png)", L"*.png"}}};
// An image to carry to a machine's drive: a disk image, and any file for whoever names one otherwise.
const std::array<COMDLG_FILTERSPEC, 2> imageKinds = {
    {{L"Disk images (*.iso; *.img)", L"*.iso;*.img"}, {L"All files", L"*.*"}}};

base::Error notOpened()
{
    return {"box", "Windows did not open its box for choosing a file."};
}

// What was chosen in a box that was closed with a file; nothing where it was closed without; and
// that it did not open, where it did not.
base::Result<QString> chosenIn(IFileDialog& box, const QPointer<QWindow>& over,
                               std::span<const COMDLG_FILTERSPEC> kinds)
{
    // (The folder a file is chosen in is that box's affair, not the program's from then on.)
    FILEOPENDIALOGOPTIONS options = 0;
    if (FAILED(box.GetOptions(&options)) || FAILED(box.SetOptions(options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR)) ||
        FAILED(box.SetFileTypes(static_cast<UINT>(kinds.size()), kinds.data()))) {
        return notOpened();
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr): a window's number is what Windows calls it by
    const HWND owner = over != nullptr ? reinterpret_cast<HWND>(over->winId()) : nullptr;
    const HRESULT shown = box.Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return QString();
    }
    if (FAILED(shown)) {
        return notOpened();
    }
    ComPtr<IShellItem> item;
    PWSTR name = nullptr;
    if (FAILED(box.GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || name == nullptr) {
        return notOpened();
    }
    const QString chosen = QString::fromWCharArray(name);
    CoTaskMemFree(name);
    return chosen;
}

base::Result<QString> whereToSave(const QPointer<QWindow>& over, const QString& suggested)
{
    ComPtr<IFileSaveDialog> box;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&box)))) {
        return notOpened();
    }
    // (The name's ending is the file's kind: whoever types a name without one gets it. Windows
    // asks before a file that is there is written over.)
    if (FAILED(box->SetFileName(suggested.toStdWString().c_str())) ||
        FAILED(box->SetDefaultExtension(suggested.section(u'.', -1).toStdWString().c_str()))) {
        return notOpened();
    }
    // Only the kind this file is, which the name that is suggested says by its ending.
    const bool encrypted = suggested.endsWith(QLatin1StringView(".qkvm"), Qt::CaseInsensitive);
    if (suggested.endsWith(QLatin1StringView(".png"), Qt::CaseInsensitive)) {
        return chosenIn(*box.Get(), over, pictureKind);
    }
    return chosenIn(*box.Get(), over,
                    encrypted ? std::span<const COMDLG_FILTERSPEC>(encryptedKind)
                              : std::span<const COMDLG_FILTERSPEC>(plainKind));
}

base::Result<QString> whichToOpen(const QPointer<QWindow>& over)
{
    ComPtr<IFileOpenDialog> box;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&box)))) {
        return notOpened();
    }
    return chosenIn(*box.Get(), over, kindsToOpen);
}

base::Result<QString> whichImageToOpen(const QPointer<QWindow>& over)
{
    ComPtr<IFileOpenDialog> box;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&box)))) {
        return notOpened();
    }
    return chosenIn(*box.Get(), over, imageKinds);
}

} // namespace

FileDoor windowsFileDoor(QWindow* over)
{
    const QPointer<QWindow> window(over);
    return {[window](const QString& suggested) { return whereToSave(window, suggested); },
            [window] { return whichToOpen(window); }};
}

std::function<base::Result<QString>()> windowsImageDoor(QWindow* over)
{
    const QPointer<QWindow> window(over);
    return [window] { return whichImageToOpen(window); };
}

} // namespace qkvm::ui
