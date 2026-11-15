#pragma once

#include <optional>

#include <QString>

#include <qkvm/base/result.h>

namespace qkvm::core::profile {

// The files the program keeps beside its settings (SPEC.md section 11.2), each named after the settings' own file
// (Settings::usualFile()). Every name here is part of every profile from 0.1.0 on: one that is to change is read under
// its old name and written under the new (a migration, section 11.1), never renamed here.

// The wrong-password counts of the sign-ins (PacingFile).
QString signInsFile(const QString& settings);
// The app lock's second record (AppLock).
QString lockFile(const QString& settings);
// The servers whose BIOS setup boot override may still be set (SetupOverrides).
QString setupOverridesFile(const QString& settings);
// The alerts' history (Incidents).
QString incidentsFile(const QString& settings);
// The screen watches' pictures (ScreenWatch::keepIn).
QString alertPicturesFolder(const QString& settings);

// --- The way back (SPEC.md section 11.2, from 0.1.2 on) --------------------------------------------------------------

// The version that last started with this profile, the one before it, and when the settings were last written as
// that start found them: `{"version", "previous", "settings_time"}` (`previous` empty where none was recorded,
// `settings_time` in milliseconds since 1970, UTC). Nothing secret is in it.
QString versionFile(const QString& settings);
struct Record {
    QString version;
    QString previous;
    std::optional<qint64> settingsTime;
};
// What is recorded; nothing where nothing is, or what is there is no record of this shape.
std::optional<Record> recorded(const QString& settings);
// Whether this is a version, or a name of a copy, as files are named by it: 1 to 32 of letters, digits and ".+-".
bool namesAVersion(const QString& version);

// The two kinds of copy, kept apart, each to its newest few (copiesKept): at a start of another version, named by the
// version that left the settings ("<settings>.<name>.bak"), and before a reset of the saved secrets, named by the
// version that resets ("<settings>.reset-<name>.bak"). A name taken is numbered ("-2" to "-999"), and past those, by
// the time of the copy: a full count never keeps a copy from being made.
enum class CopyKind { VersionChange, Reset };
inline constexpr int copiesKept = 3;
// The file the next copy of this kind and name goes to, as keepCopy names it; empty where `name` names nothing.
QString nextCopy(const QString& settings, CopyKind kind, const QString& name);
// Keeps a copy of the settings, byte for byte, beside them, at `as` where that is given and free and of this kind and
// name (what nextCopy said a moment before, which a box has shown), else at nextCopy; never over another. Then the
// oldest copies of this kind beyond copiesKept are taken away (never the new one; one that cannot be is left). Where it
// went; empty where there are no settings. An error where the copy could not be made whole, and then nothing of it is
// there. The settings hold no secret, and neither does the copy.
base::Result<QString> keepCopy(const QString& settings, CopyKind kind, const QString& name, const QString& as = {});

// The code of recordStart's error where it could not keep the settings' copy (and so recorded nothing).
inline constexpr const char* copyNotKept = "profile-copy";

// Records `version` as the one that started with this profile, with the one before it (the version recorded, where
// that is another) and the settings' time now: what recordStart records once the settings' copy is kept, and what
// Settings records once it has kept a copy it owed (Settings::owesACopy).
base::Result<void> recordVersion(const QString& settings, const QString& version);

// What a start found.
struct Started {
    // The version recorded before this start; empty where none was.
    QString previous;
    // Another version than this one, or one that kept no record, left the settings.
    bool another = false;
    // Where the settings it found were kept; empty: no copy was made (the same version, or no settings yet).
    QString keptAs;
};
// At every start, before anything of the profile is read or written. Whoever wrote the settings last: the version they
// say wrote them ("written_by", from 0.1.2 on), or, where they say none, the version recorded if they were not written
// since it recorded them (their time), else one from before 0.1.2. Where that is not `version` (newer, older, or one
// that kept no record), the settings are kept as they were found (keepCopy, VersionChange, named by that version, or
// "before-<version>" for one before 0.1.2), and only then is `version` recorded, with the one before it and the
// settings' time. Where the copy cannot be made, nothing is recorded and the error is handed back (copyNotKept): the
// program starts all the same, tells the person, and its settings owe the copy (Settings::owesACopy), which is kept
// before anything of them is written; nothing is written while it cannot be, so the settings stay as the version
// before left them, and the next start owes it still (outside review of 0.1.2, C2 and C9).
base::Result<Started> recordStart(const QString& settings, const QString& version);

// What a start of the program does with that (main.cpp, before the settings are read; review 012, L4 c): recordStart,
// and where its copy could not be kept, what the person is told once, in plain words (`said`), and the name the copy
// is owed under (`owedCopy`: Settings::owesACopy). Both empty where the copy was kept, or none was needed.
struct AtStart {
    QString said;
    QString owedCopy;
};
AtStart atStart(const QString& settings, const QString& version);
// The same for what recordStart came to (started).
AtStart afterRecording(const QString& settings, const QString& version, const base::Result<Started>& started);

} // namespace qkvm::core::profile
