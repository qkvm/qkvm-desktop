#pragma once

#include <filesystem>
#include <memory>

namespace qkvm::ui {
// Best-effort summaries only: no process memory, stack contents or application
// strings are collected. A normal exit removes its unfinished-session record.
class CrashReport {
public:
    explicit CrashReport(const std::filesystem::path& directory);
    ~CrashReport();
    CrashReport(const CrashReport&) = delete;
    CrashReport& operator=(const CrashReport&) = delete;

    // What is done as well where a crash is recorded: once, after its summary is written, on the thread
    // that crashed (main: the video's diagnostics lines that wait, VideoDiagnostics::writeWhatWaitsAtACrash).
    // It is held to the crash path's rules: no lock that may be held, nothing made, no Qt, and back within
    // a few seconds.
    using AtACrash = void (*)() noexcept;
    static void alsoAtACrash(AtACrash what);

private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace qkvm::ui
