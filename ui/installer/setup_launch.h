#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>
namespace qkvm::installer {
enum class SetupHop { Relay, Installer };
struct SetupLaunchApi {
    decltype(&CreateProcessW) create = &CreateProcessW;
    decltype(&UpdateProcThreadAttribute) attribute = &UpdateProcThreadAttribute;
};
// The first hop sets the policy for its children; the relay inherits that policy
// and starts the second hop normally. Setup independently refuses package identity.
// Waiting is allowed only for the profile-free --check-helper diagnostic.
std::uint32_t launchSetup(const std::wstring& executable, const std::vector<std::wstring>& arguments, SetupHop hop,
                          bool check = false, const SetupLaunchApi& api = {});
} // namespace qkvm::installer