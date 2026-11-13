#include "transaction.h"

namespace qkvm::installer {

int setupExitCode(Result result, bool repair)
{
    // 0 only where what was asked is what runs now. A recovery with nothing to finish is that too; an
    // update or a rollback that left the existing version is not.
    return result == Result::Installed || result == Result::RolledBack || (repair && result == Result::KeptCurrent) ? 0
                                                                                                                    : 1;
}

bool informational(Result result)
{
    return result == Result::Installed || result == Result::RolledBack;
}

std::wstring resultMessage(Result result, const std::wstring& running, const std::wstring& deployProblem,
                           const std::wstring& healthProblem)
{
    const std::wstring version = running.empty() ? std::wstring(L"qKVM") : L"qKVM " + running;
    std::wstring detail;
    switch (result) {
    case Result::Installed:
        detail = version + L" was installed and its startup check passed. Open qKVM from the Start menu.";
        break;
    case Result::RolledBack:
        detail = version + L" runs again, as asked, and its startup check passed. Open qKVM from the Start menu.";
        break;
    case Result::PutBack:
        detail = L"The version that was to be installed did not pass its startup check. " + version +
                 L", the version you had, was put back and passed its check. Open qKVM from the Start menu.";
        break;
    case Result::KeptCurrent:
        detail = running.empty() ? std::wstring(L"The existing version was kept. No update was committed.")
                                 : version + L", the existing version, was kept. No update was committed.";
        break;
    case Result::Refused:
        detail = L"Installation was refused. The package may not match the installed version, or its recovery copy "
                 L"could not be verified. Settings were not changed.";
        break;
    case Result::RecoveryNeeded:
        detail = L"Installation needs recovery. The saved installation record and packages were kept. Run Recover "
                 L"installation before trying another update. Settings were not changed.";
        break;
    }
    if (result != Result::Installed && result != Result::RolledBack) {
        // A version put back: what Windows said of the failed one's deployment is stale (N-2).
        if (result != Result::PutBack && !deployProblem.empty())
            detail += L"\n\nWindows: " + deployProblem;
        // The startup check names itself.
        if (!healthProblem.empty())
            detail += L"\n\n" + healthProblem;
    }
    return detail;
}

bool sameVersion(const std::optional<Package>& left, const std::optional<Package>& right)
{
    return (!left && !right) || (left && right && left->family == right->family && left->version == right->version);
}

bool compatible(const Package& left, const Package& right)
{
    return left.family == right.family && left.profileSchema != 0 && left.profileSchema == right.profileSchema;
}

namespace {
bool remember(History& history, const History& next, const Backend& backend)
{
    if (!backend.save(next)) {
        return false;
    }
    history = next;
    return true;
}

Result commit(History& history, const Backend& backend)
{
    // Older than what it replaces: a rollback, whether apply() or a recovery finishes it.
    const bool older = history.pending && history.current && history.pending->version < history.current->version;
    History next{history.pending, history.current, std::nullopt};
    if (!remember(history, next, backend)) {
        return Result::RecoveryNeeded;
    }
    return older ? Result::RolledBack : Result::Installed;
}
} // namespace

Result recover(History& history, const Backend& backend)
try {
    if (!history.pending) {
        return Result::KeptCurrent;
    }
    const auto actual = backend.installed();
    if (sameVersion(actual, history.current)) {
        History next = history;
        next.pending.reset();
        return remember(history, next, backend) ? Result::KeptCurrent : Result::RecoveryNeeded;
    }
    if (!sameVersion(actual, history.pending)) {
        return Result::RecoveryNeeded;
    }
    if (backend.verify(*history.pending) && backend.healthy(*history.pending)) {
        return commit(history, backend);
    }
    if (!history.current || !compatible(*history.pending, *history.current) || !backend.verify(*history.current) ||
        !backend.deploy(*history.current, true) || !sameVersion(backend.installed(), history.current) ||
        !backend.healthy(*history.current)) {
        return Result::RecoveryNeeded;
    }
    History next = history;
    next.pending.reset();
    return remember(history, next, backend) ? Result::PutBack : Result::RecoveryNeeded;
}

catch (...) {
    // Registration/deployment queries may throw. Keep the pending journal and use
    // the recovery result; no exception may bypass Setup's recovery instructions.
    return Result::RecoveryNeeded;
}

Result apply(History& history, const Package& candidate, bool rollback, const Backend& backend)
{
    if (history.pending || candidate.family.empty() || candidate.version == 0 || candidate.profileSchema == 0 ||
        !backend.verify(candidate) || !sameVersion(backend.installed(), history.current)) {
        return Result::Refused;
    }
    if (history.current && (!compatible(candidate, *history.current) || !backend.verify(*history.current))) {
        return Result::Refused;
    }
    if (rollback) {
        if (!history.current || !history.previous || candidate != *history.previous ||
            candidate.version >= history.current->version) {
            return Result::Refused;
        }
    } else if (history.current && candidate.version <= history.current->version) {
        return Result::Refused;
    }
    History next = history;
    next.pending = candidate;
    if (!remember(history, next, backend)) {
        return Result::Refused;
    }
    // A failed deployment may still have reached Windows' commit. Inspect actual state before
    // clearing the journal; never claim the old version survived just from an error code.
    static_cast<void>(backend.deploy(candidate, rollback));
    return recover(history, backend);
}

} // namespace qkvm::installer
