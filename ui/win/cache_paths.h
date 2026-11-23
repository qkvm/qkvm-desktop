#pragma once

#include <filesystem>
#include <string>

namespace qkvm::storage {
enum class Area { Downloads, Helpers, Installer, Diagnostics };

// A supplied base is used by disposable tests; ordinary callers use localData().
std::filesystem::path localData();
std::wstring currentFamily();
std::wstring familyFor(std::wstring name, std::wstring publisher);
// The kind of Windows familyFor derives a family as (a PACKAGE_ID's processorArchitecture): this build's own. A family
// has none in it.
unsigned int familyArchitecture();
std::filesystem::path cachePath(const std::filesystem::path& base, const std::wstring& family, Area area);
// Creates only ordinary directories, refusing a redirected component.
void prepareDirectory(const std::filesystem::path& path);
std::filesystem::path cache(Area area);
} // namespace qkvm::storage
