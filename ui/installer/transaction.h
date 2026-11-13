#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace qkvm::installer {

struct Package {
    std::wstring file;
    std::wstring family;
    std::uint64_t version = 0;
    std::uint32_t profileSchema = 0;
    bool operator==(const Package&) const = default;
};

// A pending candidate is journalled before Windows is asked to change anything. Current and
// previous remain the last health-checked versions until a successful commit.
struct History {
    std::optional<Package> current;
    std::optional<Package> previous;
    std::optional<Package> pending;
    bool operator==(const History&) const = default;
};

struct Backend {
    std::function<bool(const Package&)> verify;
    std::function<std::optional<Package>()> installed;
    std::function<bool(const History&)> save;
    std::function<bool(const Package&, bool)> deploy;
    std::function<bool(const Package&)> healthy;
};

// Installed: the newer version asked for runs. RolledBack: the older version asked for runs. PutBack:
// the version asked for failed its startup check and the version the user had was put back and runs
// (L-2: a failed rollback is never told as a rollback). KeptCurrent: nothing changed.
enum class Result { Installed, RolledBack, PutBack, KeptCurrent, Refused, RecoveryNeeded };

// `running`: the version that runs now, as Setup writes versions.
std::wstring resultMessage(Result result, const std::wstring& running, const std::wstring& deployProblem,
                           const std::wstring& healthProblem);
// Setup's exit code for a result; `repair`: Setup was asked only to recover an interrupted installation.
int setupExitCode(Result result, bool repair);
// Whether the result is told with the information icon (what was asked was done) or a warning.
bool informational(Result result);

bool sameVersion(const std::optional<Package>& left, const std::optional<Package>& right);
bool compatible(const Package& left, const Package& right);
Result recover(History& history, const Backend& backend);
Result apply(History& history, const Package& candidate, bool rollback, const Backend& backend);

} // namespace qkvm::installer
