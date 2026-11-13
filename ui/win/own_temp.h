#pragma once

#include <filesystem>
#include <string>

namespace qkvm::ui {

// The person's own Temp for `name` (TEMP or TMP): as their profile names it (HKCU\Environment, expanded), else the
// Temp of their local application data.
std::wstring ownTemp(const wchar_t* name);

// A program that qKVM's Setup started at its end has Setup's own TEMP and TMP (a "setup-temp-" folder of the update
// cache's Downloads, which Updates::runSetup gave Setup); at the start they are put back to the person's own (ownTemp),
// before anything is unpacked or written there. Only a TEMP or TMP that names such a folder of `downloads` is changed.
// Whether one was.
bool takeBackOwnTemp(const std::filesystem::path& downloads);
// The same for the program's own Downloads of its cache (storage::cache); false where there is none.
bool takeBackOwnTemp();

} // namespace qkvm::ui
