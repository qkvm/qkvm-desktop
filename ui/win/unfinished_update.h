#pragma once

#include <string>

namespace qkvm::ui {

// An update of an installed copy that did not finish (packaging/qkvm-desktop.iss, its staged install, i020). Before
// the setup sets aside the earlier version's files for its copy (each renamed to its own name and an ending of the
// setup's, in its own folder), it writes this marker beside the program's file, whose record says which files it
// renames and with which ending; it takes the marker away once the earlier version is whole again or the copy is (then
// it first renames the marker to qkvm-desktop.update-finishing, which the program does not look at).
// tools/test_setup.ps1 holds the setup and these names alike.
inline constexpr char unfinishedUpdateMarker[] = "qkvm-desktop.update-unfinished";
// The first of the endings a set-aside gives (then .2.old-update to .9.old-update: the first that no file of the
// package's has beside it, so that a file of that name and ending which is not the setup's is never in its way).
inline constexpr char setAsideEnding[] = ".old-update";
// The record in the marker: its first line says what it is in words, the second is this kind, then "ending <e>",
// one "aside <name>" for each file renamed (as the package names it under the program's folder, with backslashes),
// lines of other kinds this does not look at, and this last line, which says it was written whole.
inline constexpr char setAsideRecordKind[] = "qKVM Desktop set-aside record 1";
inline constexpr char setAsideRecordEnd[] = "end of the record";

// The gate qKVM's Setup and its uninstaller hold for as long as they work, one for each Windows account in whatever
// Windows session: a mutex of the whole PC's names, this and the account's SID after it (packaging/qkvm-desktop.iss,
// AccountGatePrefix; tools/test_setup.ps1 holds the two alike). A start takes a stale marker away only while it holds
// that gate itself, so never while a setup works, not even in the instant between the marker's writing and the setup's
// hold of it (outside review of 0.2.0, 1.4).
inline constexpr wchar_t setupAccountGatePrefix[] = L"Global\\qKVM-Desktop-Setup-Account-";

// The gate of this process's Windows account (the prefix and its SID); empty where Windows does not say who that is.
std::wstring setupAccountGate();

// What a start finds beside the program's own file.
enum class UpdateLeft {
    Nothing,     // no marker: the program starts
    StaleMarker, // a marker whose record names nothing that is still set aside: taken away, and the program starts
    // the marker and a file its record set aside, or one only its copy wrote (no "aside" line of its name: the earlier
    // version had none), or a record that cannot be read: said, nothing started
    Unfinished,
};

struct UpdateLook {
    UpdateLeft found = UpdateLeft::Nothing;
    // (StaleMarker) whether the marker was taken away: the program starts whether or not, so that a marker left
    // behind never keeps it from starting.
    bool markerRemoved = false;
    // (StaleMarker) where it was not: whether that is for a setup or an uninstall at work for this account, which holds
    // its gate (or a gate that cannot be told free, as one another account made under its name).
    bool setupAtWork = false;
    // (Unfinished) the first file the record names that is set aside, or else that only the copy wrote and is still
    // there, as the record names it ("plugins\platforms\..."); empty where the record could not be read.
    std::wstring setAside;
};

// The program's folder looked at, first in ui/main.cpp, before the single-instance mutex and before anything of the
// program starts (Setup's own start has waited there for Setup to be over); only where the contract's marker of a copy
// the setup installed (qkvm-desktop.installed) is there, never in a copy from the ZIP. Windows' own calls alone,
// nothing of Qt's: the marker's record read (up to 1 MiB), and each file it names as set aside looked for by its name
// and the record's own ending, and each it names as what the copy writes and not as set aside, by its name (outside
// review of 0.2.0, 1.5); nothing else, and the folder is never walked. A file of a set-aside ending that the
// record does not name is not the setup's, and is never looked at. Only names a package of ours can have are taken
// (each part letters, digits, blanks and . _ + -, beginning with a letter, a digit or _, ending in neither a dot nor a
// blank: tools/build_setup.ps1's rule); a record that names another, or is not whole, or of another kind (a later
// version's), is not read, and keeps the program shut: the setup is the one to look at it. Nothing is changed but a
// stale marker, which is taken away only while this start holds `gate` (the account's, setupAccountGate; a test names
// its own), looked at again then: a marker a setup wrote meanwhile is judged anew.
UpdateLook lookForUnfinishedUpdate(const std::wstring& folder, const std::wstring& gate = setupAccountGate());

// What the program says where an update did not finish, and qKVM Setup's update check alike (ui/win/update_check.cpp,
// which the setup puts at the program's own name while the earlier version is set aside), in a box of Windows' own.
// "Choose OK" first: the box keeps the program's file in use while it is open, and a setup run meanwhile could not
// put it right (review i020-1, L6).
inline constexpr wchar_t unfinishedUpdateText[] =
    L"An update of qKVM did not finish. Choose OK, then run the qKVM setup "
    L"again: it puts the earlier version back, or finishes the update.";

// unfinishedUpdateText, for the program's box.
const wchar_t* unfinishedUpdateWords();

// The folder of this program's own file, as Windows names it (GetModuleFileNameW); empty where it cannot tell.
std::wstring ownFolder();

} // namespace qkvm::ui
