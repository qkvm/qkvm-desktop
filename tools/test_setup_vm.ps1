#Requires -Version 5.1
<#
.SYNOPSIS
Tries the setup for real: ONLY inside a throwaway Windows 10 (22H2) or 11 virtual machine, as a standard user.

.DESCRIPTION
    powershell -ExecutionPolicy Bypass -File test_setup_vm.ps1 -Setup <new setup.exe> -OlderSetup <older setup.exe>
        -Out <results folder> -Disposable

NEVER on a PC anyone works on: it installs, upgrades, kills, updates and uninstalls qKVM Desktop for the user who
runs it, writes their registry and Start menu, and starts and ends the program. It refuses to begin unless it is
told -Disposable, finds the file qkvm-disposable-vm.txt in the user's own folder holding this machine's name, both as
Windows says them and never as the environment does (made
by whoever set the machine up for this), runs as a standard user (no administrator, not elevated), and finds no
qKVM Desktop installed yet. Windows PowerShell 5.1 is enough; nothing is fetched, nothing is sent anywhere.

-Setup is the setup to try (qKVM-Desktop-<version>-windows-x64-setup.exe, as tools/build_setup.ps1 makes it);
-OlderSetup one of an earlier version, which is installed first so that the upgrade is a real one. Each step writes
one JSON file into -Out (NN-<step>.json: passed, what was seen, each failure), and the setups' logs beside them;
summary.json at the end. Steps:
  00 preflight             Windows, x64, a standard user, nothing installed; both setups' versions (older first)
  01 first-install         the older setup, silent: no administrator asked for (no consent prompt seen), exit 0,
                           the user's own entry in Settings > Apps (HKCU) and none for the PC (HKLM), exactly the
                           package's files in %LOCALAPPDATA%\Programs\qKVM Desktop with the marker and the
                           uninstaller, a Start menu entry and no desktop one
  02 program-starts        the program starts (its single-instance mutex appears), stays up, and is closed
  03 killed-upgrade        the new setup is killed once it has written Qt6Core.dll: the folder is compared, file by
                           file, with the earlier version's (what is still the earlier one's, what is the new one's
                           or gone or new: what Inno Setup's rollback restores of a killed setup), and the earlier
                           program is found set aside for the copy (in every mode since 0.1.2, review L3), not there to
                           start until the next setup puts it back
  04 upgrade               the new setup over the earlier version: exit 0, its version, exactly its files, the earlier
                           program put back first (03 left it aside) and set aside for this copy; a file
                           and a folder this puts in plugins and qml beforehand are removed after the copy, and a
                           file it puts beside them (outside the two folders) is left
  05 downgrade-refused     the older setup over the new version, silent: it ends with a code that is not 0, and
                           nothing is changed
  06 program-starts        the upgraded program starts
  07 update-mode           the contract's update: the program runs, the setup is started with exactly
                           /SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /CURRENTUSER /UPDATE=1 /DIR=".." /LOG="..",
                           the program ends within 10 s; the setup waits, installs, exits 0 and starts the
                           program again from its folder with the one argument --after-update; nothing takes the
                           program's mutex while the setup still holds its own (Local\qKVM-Desktop-Setup)
  08 update-still-running  the same with the program left running: exit 105 within about 60 s, nothing changed,
                           the program still running, and the setup's log ends with the contract's result line
                           (qKVM-RESULT: 105 ...)
  09 update-file-in-use    the same with the program ended but one of its files held open: the setup refuses
                           before it changes anything (Inno Setup's exit code 7), nothing changed, and its log has
                           the contract's result line (qKVM-RESULT: 7 nothing was changed: ...)
  10 uninstall-refused     the uninstaller while the program runs: refused, nothing removed
  11 uninstall             the uninstaller: the program's files, its Start menu entry and its Settings > Apps entry
                           gone; the program's data (%LOCALAPPDATA%\QKVM Desktop, %LOCALAPPDATA%\qKVM, Windows
                           Credential Manager) left as it was
  12 reinstall             the new setup again: exit 0, its files, the data still there; then the program, started
                           without --after-update while a setup's mutex is held, ends at once without taking its
                           own (the mutex held by this test, as a setup holds it: a reinstall ends a few
                           milliseconds after its files are in, too soon to start the program within it; starts
                           during a real copy are steps 07 and 14)
  13 program-starts        the reinstalled program starts
  14 starts-during-copy    the new setup again, with the program started without an argument every half second while
                           the setup copies its files: every start finds no program (set aside for the copy), the
                           install completes, no start takes the program's mutex while
                           the setup holds its own, none is left running without it (one that comes as the setup
                           ends may find it gone and run as the program does), and then the new version starts
  15 update-across-qt      the older version installed again, then the contract's update to the new one, whose Qt
                           libraries differ, with the program started every half second during the copy: its file is
                           set aside for the copy (so every start finds no program), the update completes, the aside
                           copy is gone, and the new version starts
  16 update-fails-during-copy  the older version again, then the contract's update with a file of the folder held open
                           once the copy has begun: the update fails, and the earlier program is back in its place and
                           starts; the new setup then installs as usual
  17 killed-then-uninstall  the new setup killed once its copy has reached licenses (what it leaves, in any folder, is
                           seen), then the uninstaller: exit 0, and the program's folder gone with nothing left in it;
                           the data left as it was
Steps 18 to 23 (0.1.2): a running copy closed for the person, the orderly way (the program's event, its answer in the
title of its window, ui/win/quit_requests.h); each starts from the new version installed (18 installs it after 17).
  18 tray-only-closed      the program set to stay in the tray (its settings' stay_in_tray), started, and its window
                           closed: it runs with no window to be seen, only its icon's. The new setup, silent: it asks
                           the program to close (its log says so), the program ends by itself (no forced end), the
                           setup installs (exit 0, its files) and starts it again; the setting is put back after
  19 busy-held             first a note beside the settings of a server's BIOS setup boot override whose doubt is
                           long over (a server at a loopback port where nothing answers, on no card, as B12 found
                           on 0.1.0's own profile): it holds nothing, the silent setup closes the program and
                           upgrades it, and the note is byte for byte what it was. Then the program holding
                           something that its end would cut (WallWindows::whatHoldsAnUpdate: a note for the same
                           server whose doubt goes on, a change that may still come): the new setup, silent, changes nothing
                           (exit 7, its files and entry as they were), the program still runs, and the result line
                           carries the program's own sentence; once the program has ended (and the note is gone), the
                           setup installs and does not start the program (it was not running then)
  20 hung                  the program not answering (every thread of it suspended): silent, the setup gives up
                           after about 10 s and changes nothing (exit 7, "did not answer within 10 seconds"); with
                           /FORCECLOSE=1 it ends the program by force (its log says so), installs, and starts it again
  21 zip-copy-beside       a copy of the program from another folder (the installed one copied without the setup's
                           marker and uninstaller: a portable ZIP's) running: the new setup, silent, finds no copy of
                           its own folder, installs (exit 0) and leaves the other copy running, never asked, and
                           starts nothing
  22 hung-end-it-now       interactive, driven as a person would through UI Automation: the program suspended; the
                           first page says what is installed and that it runs; Install asks "Close qKVM and
                           continue"; after about 10 s the setup says it did not answer and offers End it now, which
                           is chosen; the upgrade completes, the last page offers "Start qKVM Desktop" ticked, and
                           Finish starts it
  23 older-running         the older version installed again and running: a version before 0.1.2 cannot be asked,
                           so a silent install changes nothing (exit 7, "cannot be asked to close"), and interactive,
                           the first page says it upgrades the older version to the new one, the setup says how to
                           quit it from its icon and offers End it now, which is chosen, and the upgrade completes; an
                           older version of 0.1.2 or later is asked, and the silent install closes it and upgrades
  24 stalled-given-up      a request the setup gave up on (review M1): the program's interface stalled (its first
                           thread alone suspended) past the setup's wait; silent, the setup changes nothing (exit 7)
                           and withdraws its request; the interface answers again and the program stays, saying the
                           request came too late; then by hand, Close qKVM and continue, the box's Cancel once it says
                           the program did not answer (the setup still runs, on its Ready page), the interface answers
                           again, and the program stays; nothing changed
  25 fresh-install-uninstall-refused  a fresh install made outside update mode (uninstalled first, so that its uninstall
                           log is its own), the program started, the uninstaller: refused (not 0) as it begins, in
                           InitializeUninstall, nothing removed, the Settings > Apps and Start menu entries there
                           (review M2: what the uninstaller checks is the mutex this install kept in that log); then,
                           the program ended, Setup's mutex held as an open setup holds it: refused (not 0) as it
                           begins, nothing removed (review F4)
  26 closed-then-refused   the program running and a file of its package held (one it does not load): the silent setup
                           closes the program, then finds the file in use and changes nothing (exit 7), and starts the
                           program again as it ends, its result line saying so (review L1; room is looked at before
                           anything is closed)
  27 cannot-be-asked       the request's name taken first by another program (this test holds it), then the program
                           started: it has no answer window; the silent setup takes it for one that cannot be asked,
                           at once, never for one that did not answer, and changes nothing (exit 7); with
                           /FORCECLOSE=1 its log says plainly that it ends a copy it could not ask, and it upgrades
                           (review L2)
  28 busy-while-waiting    the program's interface busy for 6 s (its first thread suspended, let go 6 s into the setup)
                           while a silent install told /FORCECLOSE=1 waits its 10 s: the request still stands, the
                           program ends the orderly way, never by force, and the setup installs and starts it again
                           (review N1)
Steps 22 to 24 press the setup's own buttons through UI Automation, as a person would: run the test in the signed-in
session with its desktop there (not in a remote desktop that was closed), and touch nothing while they run. Each step's
JSON keeps what the setup's windows showed before each press, and what they showed last where a press never came.
For step 15 the two setups' Qt libraries must differ: make the newer setup from a copy of its package in which one byte
is appended to Qt6Core.dll (an overlay that Windows' loader leaves alone), with that file's new size and SHA-256 written
into package-manifest.json, by tools/build_setup.ps1. A setup ended from outside (killed) runs no code of its own: a
program set aside for its copy stays aside as qkvm-desktop.exe.old-update until the next setup, which puts it back
first (step 16 makes the update fail inside the setup instead, as a full disk or a file in use does).
The data is told apart by a file this puts in each data folder at the start (qkvm-setup-vm-test.txt) and by the
names of the program's own entries in Windows Credential Manager (cmdkey /list, those named QKVM Desktop: ...),
which no setup and no uninstaller may change. The program adds its own when it starts (its sign-in pacing key at
its first start), so they are listed again after each start of the program, and none it had may be gone then;
the entries Windows keeps there of its own (a Microsoft account's) are not looked at.
A failed install by a full disk is not tried here: it needs a volume made small for it, which needs an
administrator; the setup's own check of free space (in update mode) is read in its script by test_setup.ps1. The
killed setup (03) and the file in use (09) are the failed installs tried.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Setup,
    [Parameter(Mandatory = $true)] [string] $OlderSetup,
    [Parameter(Mandatory = $true)] [string] $Out,
    [switch] $Disposable
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Where this runs, as Windows says it (GetComputerNameExW, SHGetKnownFolderPath), never as the environment does:
# COMPUTERNAME, USERPROFILE and LOCALAPPDATA are whoever starts this to set (review 012, N2; as k012's
# disposable_machine.h asks, and tools/test_upgrade_keeps_vm.ps1).
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class QkvmVmWhere {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool GetComputerNameExW(int kind, StringBuilder name, ref uint size);
    [DllImport("shell32.dll")] static extern int SHGetKnownFolderPath([MarshalAs(UnmanagedType.LPStruct)] Guid id, uint flags, IntPtr token, out IntPtr path);
    // The machine's own NetBIOS name, which the guard file holds ("" where Windows does not say): the physical one
    // (ComputerNamePhysicalNetBIOS, 4), never the cluster's that _CLUSTER_NETWORK_NAME_ gives the other kinds (review R2).
    public static string Machine() {
        StringBuilder name = new StringBuilder(64);
        uint size = (uint) name.Capacity;
        return GetComputerNameExW(4, name, ref size) ? name.ToString() : "";
    }
    // A known folder of whoever runs this ("" where Windows does not say).
    public static string Folder(string id) {
        IntPtr path;
        if (SHGetKnownFolderPath(new Guid(id), 0, IntPtr.Zero, out path) != 0) return "";
        try { return Marshal.PtrToStringUni(path); } finally { Marshal.FreeCoTaskMem(path); }
    }
}
'@
$Machine = [QkvmVmWhere]::Machine()
# (FOLDERID_Profile: Windows has it from the user's own record, whatever USERPROFILE says.)
$UserFolder = [QkvmVmWhere]::Folder('5E6C858F-0E22-4760-9AFE-EA3317B67173')

# --- Never anywhere but on a machine made for it.
if (-not $Disposable) { throw 'This installs and uninstalls qKVM Desktop for real. Run it only in a throwaway virtual machine, with -Disposable.' }
if (-not $Machine -or -not $UserFolder) { throw 'Windows did not say which machine this is, or where this user''s folder is.' }
$guard = Join-Path $UserFolder 'qkvm-disposable-vm.txt'
if (-not (Test-Path -LiteralPath $guard) -or (Get-Content -LiteralPath $guard -Raw).Trim() -ne $Machine) {
    throw "There is no $guard holding this machine's name ($Machine): this is not a machine made for this test."
}
function Get-Hypervisor([string] $Maker, [string] $Product, [string] $Vendor) {
    <# The hypervisor the firmware's own words name (QEMU, VMware, Hyper-V, VirtualBox); '' where they name none. #>
    if ($Maker -match '\bQEMU\b' -or $Product -match '\bQEMU\b') { return 'QEMU' }
    if ($Maker -match '\bVMware\b' -or $Product -match '\bVMware\b') { return 'VMware' }
    if ($Maker -eq 'Microsoft Corporation' -and $Product -eq 'Virtual Machine') { return 'Hyper-V' }
    if ($Product -match '\bVirtualBox\b' -or $Maker -match '^innotek\b') { return 'VirtualBox' }
    return ''
}
function Get-FirmwareWord([string] $Name) {
    <# One of the firmware's words as Windows keeps them for everyone to read (HKLM\HARDWARE\DESCRIPTION\System\BIOS). #>
    $bios = Get-ItemProperty -LiteralPath 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS' -ErrorAction SilentlyContinue
    if (-not $bios -or -not $bios.PSObject.Properties[$Name]) { return '' }
    return ([string] $bios.$Name).Trim()
}
# (And a machine whose firmware says it is a virtual one, as a standard user can read it: a guard file alone can be put
# on any machine, review T5.)
$Hypervisor = Get-Hypervisor (Get-FirmwareWord 'SystemManufacturer') (Get-FirmwareWord 'SystemProductName') (Get-FirmwareWord 'BIOSVendor')
if (-not $Hypervisor) {
    throw "This machine's firmware names no hypervisor (QEMU, VMware, Hyper-V or VirtualBox: it says $(Get-FirmwareWord 'SystemManufacturer'), $(Get-FirmwareWord 'SystemProductName')): this test runs in a virtual machine alone."
}
# (FOLDERID_LocalAppData, as the program itself finds it; only once the guard has passed.)
$Local = [QkvmVmWhere]::Folder('F1B32785-6FBA-4FCF-9D55-7B8E7F157091')
if (-not $Local) { throw 'Windows did not say where this user''s local application data is.' }

$AppId = '{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1'
$ProgramMutex = 'Local\QKVM Desktop'
# The setup's own, held while it runs (SetupMutex=qKVM-Desktop-Setup, in the session's namespace).
$SetupMutex = 'Local\qKVM-Desktop-Setup'
# How long a setup past its time is waited for with no new line in its log (Invoke-Setup, V1b).
$StallSeconds = 300
$App = Join-Path $Local 'Programs\qKVM Desktop'
$Program = Join-Path $App 'qkvm-desktop.exe'
$UserKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$AppId"
$MachineKeys = @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$AppId",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\$AppId")
$StartMenuEntry = Join-Path ([Environment]::GetFolderPath('Programs')) 'qKVM Desktop.lnk'
$DesktopEntry = Join-Path ([Environment]::GetFolderPath('Desktop')) 'qKVM Desktop.lnk'
$DataFolders = @((Join-Path $Local 'QKVM Desktop'), (Join-Path $Local 'qKVM'))
$Sentinel = 'qkvm-setup-vm-test.txt'
$SilentArguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'

$Setup = (Resolve-Path -LiteralPath $Setup).Path
$OlderSetup = (Resolve-Path -LiteralPath $OlderSetup).Path
if (-not (Test-Path -LiteralPath $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }
$Out = (Resolve-Path -LiteralPath $Out).Path
if (@(Get-ChildItem -LiteralPath $Out -Force).Count) { throw "$Out is not empty: each run writes its results into a new folder." }

$results = New-Object System.Collections.ArrayList

function Get-Sha256([string] $File) { return (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant() }

function Write-Step([string] $Number, [string] $Name, [DateTime] $Began, [System.Collections.ArrayList] $Failures, $Seen) {
    $step = [ordered]@{
        step = "$Number-$Name"; passed = ($Failures.Count -eq 0)
        began = $Began.ToUniversalTime().ToString('o'); ended = [DateTime]::UtcNow.ToString('o')
        failures = @($Failures); seen = $Seen
    }
    $step | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Out "$Number-$Name.json") -Encoding UTF8
    [void] $results.Add([ordered]@{ step = $step.step; passed = $step.passed; failures = $Failures.Count })
    Write-Output ("{0} {1}-{2}{3}" -f $(if ($step.passed) { 'passed:' } else { 'FAILED:' }), $Number, $Name, $(if ($Failures.Count) { ': ' + ($Failures -join '; ') } else { '' }))
}

function Test-Mutex([string] $Name) {
    $mutex = $null
    if ([Threading.Mutex]::TryOpenExisting($Name, [ref] $mutex)) { $mutex.Dispose(); return $true }
    return $false
}
function Test-ProgramRunning { return (Test-Mutex $ProgramMutex) }
function Test-SetupRunning { return (Test-Mutex $SetupMutex) }

function Wait-Until([scriptblock] $Condition, [int] $Seconds) {
    $until = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $until) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return [bool] (& $Condition)
}

function Get-ProgramProcesses {
    return @(Get-CimInstance Win32_Process -Filter "Name = 'qkvm-desktop.exe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $Program })
}

function Start-Program {
    <# Starts the installed program as this user; whether its mutex appeared within 30 s, and its process. #>
    $process = Start-Process -FilePath $Program -WorkingDirectory $App -PassThru
    $up = Wait-Until { Test-ProgramRunning } 30
    return [pscustomobject]@{ Process = $process; Up = $up }
}

function Stop-Program([int] $Within = 10) {
    <# Closes every running copy of the installed program, by its window first; how each ended. #>
    $how = New-Object System.Collections.ArrayList
    foreach ($found in Get-ProgramProcesses) {
        $process = Get-Process -Id $found.ProcessId -ErrorAction SilentlyContinue
        if (-not $process) { continue }
        [void] $process.CloseMainWindow()
        if ($process.WaitForExit($Within * 1000)) { [void] $how.Add("closed ($($found.ProcessId))") }
        else { Stop-Process -Id $found.ProcessId -Force; [void] $how.Add("ended ($($found.ProcessId))") }
    }
    [void] (Wait-Until { -not (Test-ProgramRunning) } 10)
    return @($how)
}

function Stop-Tree([int] $Id, [DateTime] $Since = [DateTime]::MinValue, [Nullable[DateTime]] $Listed = $null, [switch] $FromList) {
    <#
    Ends the process and what it started, children first. A child only where it began at or after its parent: Windows
    keeps a process's ParentProcessId after its parent has ended, and the parent's number may since be another's, the
    setup's among them, whose "children" are then older programs of the person's (review T1). And each only while its
    number is still the process it was: one that began before $Since (its parent's start) is another's by now.
    #>
    # (Held open by its handle from here on: while it is held, its number is its own, and it is told and ended by that
    # handle, never by the number again. A child is ended only where the process its number opens began when Windows'
    # list said that child began, $Listed: one that ended between the list and the opening, its number taken by another,
    # is never ended for it; review R7. The same start exactly, at the list's own precision: Windows keeps a start to
    # 100 ns, CIM says it to the microsecond, cut, not rounded; so the opened process's, cut to the microsecond in UTC,
    # must be the listed one, tick for tick. Where either start is not there, the child is left alone; review F6.)
    $process = $null
    $began = $null
    try {
        $process = [Diagnostics.Process]::GetProcessById($Id)
        [void] $process.Handle
        $began = $process.StartTime
    } catch {
        if ($process) { $process.Dispose() }
        return
    }
    try {
        if ($null -eq $began) { return }
        if ($began -lt $Since) { return }
        if ($FromList) {
            if ($null -eq $Listed) { return }
            $opened = $began.ToUniversalTime().Ticks
            if (($opened - ($opened % 10)) -ne ([DateTime] $Listed).ToUniversalTime().Ticks) { return }
        }
        foreach ($child in @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $Id")) { Stop-Tree ([int] $child.ProcessId) $began $child.CreationDate -FromList }
        if (-not $process.HasExited) { $process.Kill() }
    } catch {
        # (Ended meanwhile, or not ours to end: left as it is.)
    } finally {
        $process.Dispose()
    }
}

function Invoke-Setup([string] $File, [string] $Arguments, [int] $Seconds = 300, [scriptblock] $During = $null, [scriptblock] $Patient = $null) {
    <#
    Runs a setup (or the uninstaller) as this user and waits for it: its exit code (or 'killed' / 'timeout'), how long
    it took, and whether Windows' consent prompt for an administrator was seen while it ran. $During is called every
    quarter of a second with the process and the seconds gone; it returns $true to have the setup killed.
    Past $Seconds it is ended only where it is not copying (its log, where /LOG names one, says so: a setup ended as it
    copies leaves the folder half one version, and the steps after it fail for that; review V1), nor $Patient (called
    as $During is) says to wait, and never later than an hour past $Seconds. Nor is it waited for there once its log
    has not grown for 5 minutes: it is ended then, 'stalled', and the step fails and the run goes on (a run of the
    suite waited on one setup for hours; V1b).
    #>
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $File
    $start.Arguments = $Arguments
    $start.UseShellExecute = $false
    $began = [DateTime]::UtcNow
    $process = [Diagnostics.Process]::Start($start)
    $consent = $false
    $ended = $null
    $logged = if ($Arguments -match '/LOG="([^"]+)"') { $Matches[1] } else { '' }
    $length = -1
    $grew = $began
    while (-not $process.HasExited) {
        if (Get-Process -Name consent -ErrorAction SilentlyContinue) { $consent = $true }
        $gone = ([DateTime]::UtcNow - $began).TotalSeconds
        if ($logged) {
            $now = Get-Item -LiteralPath $logged -ErrorAction SilentlyContinue
            $size = if ($now) { $now.Length } else { -1 }
            if ($size -ne $length) { $length = $size; $grew = [DateTime]::UtcNow }
        }
        if ($During -and (& $During $process $gone)) { Stop-Tree $process.Id; $ended = 'killed'; break }
        if ($gone -gt $Seconds) {
            $stalled = $logged -and ([DateTime]::UtcNow - $grew).TotalSeconds -ge $StallSeconds
            $waits = -not $stalled -and $gone -lt $Seconds + 3600 -and (($logged -and (Test-Copying $logged)) -or ($Patient -and (& $Patient $process $gone)))
            if (-not $waits) { Stop-Tree $process.Id; $ended = if ($stalled) { 'stalled' } else { 'timeout' }; break }
        }
        Start-Sleep -Milliseconds 250
    }
    $process.WaitForExit(10000) | Out-Null
    if (-not $ended) { $ended = $process.ExitCode }
    return [pscustomobject]@{ Exit = $ended; Seconds = [Math]::Round(([DateTime]::UtcNow - $began).TotalSeconds, 1); Consent = $consent; Began = $began }
}

function Get-Installed {
    <# The program's folder, file by file with its SHA-256, and its Settings > Apps entries. #>
    $files = [ordered]@{}
    if (Test-Path -LiteralPath $App) {
        foreach ($file in Get-ChildItem -LiteralPath $App -Recurse -File -Force | Sort-Object FullName) {
            $files[$file.FullName.Substring($App.Length + 1).Replace('\', '/')] = [ordered]@{ sha256 = Get-Sha256 $file.FullName; written = $file.LastWriteTimeUtc.ToString('o') }
        }
    }
    $entry = $null
    if (Test-Path -LiteralPath $UserKey) {
        $values = Get-ItemProperty -LiteralPath $UserKey
        $entry = [ordered]@{}
        foreach ($name in 'DisplayName', 'DisplayVersion', 'Publisher', 'InstallLocation', 'UninstallString', 'DisplayIcon') {
            $entry[$name] = $(if ($values.PSObject.Properties[$name]) { [string] $values.$name } else { $null })
        }
    }
    return [pscustomobject]@{ Files = $files; Entry = $entry; Machine = @($MachineKeys | Where-Object { Test-Path -LiteralPath $_ })
        StartMenu = (Test-Path -LiteralPath $StartMenuEntry); Desktop = (Test-Path -LiteralPath $DesktopEntry) }
}

function Test-Installed([string] $Version, [System.Collections.ArrayList] $Failures) {
    <# The installed program as a setup of $Version leaves it: exactly the package's files, the marker, the uninstaller. #>
    $state = Get-Installed
    if (-not $state.Entry) { [void] $Failures.Add('there is no entry in Settings > Apps for the user') }
    else {
        if ($state.Entry.DisplayName -ne 'qKVM Desktop' -or $state.Entry.Publisher -ne 'qKVM') { [void] $Failures.Add("the Settings > Apps entry is $($state.Entry.DisplayName) of $($state.Entry.Publisher)") }
        if ($state.Entry.DisplayVersion -ne $Version) { [void] $Failures.Add("the Settings > Apps entry says version $($state.Entry.DisplayVersion), not $Version") }
    }
    if ($state.Machine.Count) { [void] $Failures.Add("there is an entry for the whole PC: $($state.Machine -join ', ')") }
    if (-not $state.StartMenu) { [void] $Failures.Add('there is no Start menu entry') }
    if ($state.Desktop) { [void] $Failures.Add('there is a desktop entry, which was not asked for') }
    $manifestFile = Join-Path $App 'package-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestFile)) { [void] $Failures.Add('the program''s folder has no package-manifest.json'); return $state }
    $manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
    if ($manifest.version -ne $Version) { [void] $Failures.Add("the installed package is version $($manifest.version), not $Version") }
    $wanted = @{}
    foreach ($entry in $manifest.files) { $wanted[$entry.path] = $entry.sha256 }
    foreach ($path in $wanted.Keys) {
        if (-not $state.Files.Contains($path)) { [void] $Failures.Add("$path is missing") }
        elseif ($state.Files[$path].sha256 -ne $wanted[$path]) { [void] $Failures.Add("$path is not the package's") }
    }
    $own = @('package-manifest.json', 'qkvm-desktop.installed', 'unins000.exe', 'unins000.dat')
    foreach ($path in $state.Files.Keys) {
        if (-not $wanted.ContainsKey($path) -and $own -notcontains $path) { [void] $Failures.Add("$path is in the program's folder, and not in the package") }
    }
    foreach ($path in $own) { if (-not $state.Files.Contains($path)) { [void] $Failures.Add("$path is missing") } }
    $marker = Join-Path $App 'qkvm-desktop.installed'
    if ((Test-Path -LiteralPath $marker) -and -not ([Convert]::ToBase64String([IO.File]::ReadAllBytes($marker)) -ceq 'c2V0dXAK')) {
        [void] $Failures.Add('the marker is not "setup" and a line feed')
    }
    return $state
}

# The names of the program's own entries in Windows Credential Manager (QKVM Desktop: ...), sorted, one per line.
function Get-ProgramCredentials {
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($line in @(& cmdkey.exe /list)) {
        if ([string] $line -match '(QKVM Desktop: .+?)\s*$') { $names.Add($Matches[1]) }
    }
    return ((@($names) | Sort-Object -Unique) -join "`n")
}

# A setup's log as it is now, while the setup may still be writing it: opened so that its writer may go on.
function Read-Shared([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    } catch [IO.IOException] { return '' }
    try { return [IO.StreamReader]::new($stream, $true).ReadToEnd() } finally { $stream.Dispose() }
}

function Test-Copying([string] $Log) {
    <# Whether the setup of this log is copying now: it began installing and has neither ended its copy nor rolled back. #>
    $said = Read-Shared $Log
    return ($said.Contains('Starting the installation process.') -and -not $said.Contains('Installation process succeeded.') -and
        -not $said.Contains('Rolling back changes.') -and -not $said.Contains('Deinitializing Setup.'))
}

function Get-LogSeconds([string] $Name, [string] $From, [string] $To) {
    <# The seconds between the first line of a setup's log that says $From and the first after it that says $To, by the
    times the setup put on them itself; $null where either is not there. What the setup did when, never when a look at
    its windows saw it (review V1). #>
    $at = $null
    foreach ($line in @((Read-Log $Name) -split "`r?`n")) {
        if ($line -notmatch '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) ') { continue }
        $when = [DateTime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss.fff', [Globalization.CultureInfo]::InvariantCulture)
        if ($null -eq $at -and $line.Contains($From)) { $at = $when; continue }
        if ($null -ne $at -and $line.Contains($To)) { return [Math]::Round(($when - $at).TotalSeconds, 1) }
    }
    return $null
}

function Test-Data([System.Collections.ArrayList] $Failures, [switch] $ProgramStarted) {
    foreach ($folder in $DataFolders) {
        $file = Join-Path $folder $Sentinel
        # (Read as it was written, UTF-8: a folder of a name with signs outside ASCII is its own, review T2.)
        if (-not (Test-Path -LiteralPath $file) -or [IO.File]::ReadAllText($file, [Text.Encoding]::UTF8).Trim() -cne $folder) { [void] $Failures.Add("the program's data in $folder was touched") }
    }
    $now = Get-ProgramCredentials
    if ($ProgramStarted) {
        # The program may add its own; none it had may be gone.
        $has = @($now -split "`n" | Where-Object { $_ })
        foreach ($name in @($script:credentials -split "`n" | Where-Object { $_ })) {
            if ($has -cnotcontains $name) { [void] $Failures.Add("the program's entry '$name' in Windows Credential Manager is gone") }
        }
        $script:credentials = $now
    } elseif ($now -cne $script:credentials) {
        [void] $Failures.Add("the program's entries in Windows Credential Manager changed (were: $($script:credentials -replace "`n", ', '); are: $($now -replace "`n", ', '))")
    }
}

function Get-Version([string] $File) {
    # (Nothing where the file is not there, as after a killed setup it may not be.)
    if (-not (Test-Path -LiteralPath $File -PathType Leaf)) { return $null }
    return ([string] [Diagnostics.FileVersionInfo]::GetVersionInfo($File).ProductVersion).Trim()
}
function Get-Log([string] $Name) { return Join-Path $Out "$Name.log" }
function Get-ResultLines([string] $Name) {
    <# The contract's result lines in a setup's log (qKVM-RESULT: ...), each without the time Inno Setup puts before it. #>
    return @((Read-Log $Name) -split "`r?`n" | Where-Object { $_.Contains('qKVM-RESULT: ') } | ForEach-Object { $_.Substring($_.IndexOf('qKVM-RESULT: ')).TrimEnd() })
}
function Read-Log([string] $Name) { $file = Get-Log $Name; if (Test-Path -LiteralPath $file) { return Get-Content -LiteralPath $file -Raw } else { return '' } }

# --- 00 preflight
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$os = [Environment]::OSVersion.Version
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = (New-Object Security.Principal.WindowsPrincipal $identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$adminGroup = @($identity.Groups | Where-Object { $_.Value -eq 'S-1-5-32-544' }).Count -gt 0
$newVersion = Get-Version $Setup
$oldVersion = Get-Version $OlderSetup
if ($os.Major -ne 10 -or $os.Build -lt 19045) { [void] $failures.Add("Windows $os is older than Windows 10 22H2") }
if (-not [Environment]::Is64BitOperatingSystem) { [void] $failures.Add('Windows is not 64-bit') }
if ($admin -or $adminGroup) { [void] $failures.Add('this runs as an administrator: run it as a standard user') }
if ((Test-Path -LiteralPath $UserKey) -or (Test-Path -LiteralPath $App)) { [void] $failures.Add('qKVM Desktop is installed already: start from a clean machine') }
if (-not ([version] $oldVersion -lt [version] $newVersion)) { [void] $failures.Add("the older setup ($oldVersion) is not older than the setup ($newVersion)") }
# (Nothing installed, and so nothing of the program's own may be there either: a profile left, or passwords it saved,
# would be opened by the steps below, which start the program, and it would reach what they hold; review T5. Before
# anything is written here.)
if (-not (Test-Path -LiteralPath $UserKey) -and -not (Test-Path -LiteralPath $App)) {
    $profileLeft = @($DataFolders | Where-Object { (Test-Path -LiteralPath $_) -and @(Get-ChildItem -LiteralPath $_ -Force).Count })
    if ($profileLeft.Count -or (Get-ProgramCredentials)) {
        throw "qKVM Desktop is not installed, but this user has its profile ($($profileLeft -join ', ')) or passwords it saved (QKVM Desktop: in Windows Credential Manager): start from a clean machine."
    }
}
foreach ($folder in $DataFolders) {
    if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder | Out-Null }
    # (Its folder's own name, written as UTF-8 without a mark, so that a name with signs outside ASCII stays itself:
    # ASCII wrote "?" for a letter with a mark on it, and every step then said the data was touched; review T2.)
    [IO.File]::WriteAllText((Join-Path $folder $Sentinel), $folder, (New-Object Text.UTF8Encoding $false))
}
$script:credentials = Get-ProgramCredentials
Write-Step '00' 'preflight' $began $failures ([ordered]@{
    windows = "$os"; user = $identity.Name; administrator = ($admin -or $adminGroup)
    setup = [ordered]@{ file = Split-Path -Leaf $Setup; version = $newVersion; sha256 = Get-Sha256 $Setup; signature = [string] (Get-AuthenticodeSignature -LiteralPath $Setup).Status }
    olderSetup = [ordered]@{ file = Split-Path -Leaf $OlderSetup; version = $oldVersion; sha256 = Get-Sha256 $OlderSetup; signature = [string] (Get-AuthenticodeSignature -LiteralPath $OlderSetup).Status } })
if ($failures.Count) { throw 'The machine is not one this test can run on; see 00-preflight.json.' }

# --- 01 first install, of the older setup
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$run = Invoke-Setup $OlderSetup "$SilentArguments /LOG=`"$(Get-Log '01-first-install')`""
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$state = Test-Installed $oldVersion $failures
$log = Read-Log '01-first-install'
if ($log -match 'Administrative install mode: Yes') { [void] $failures.Add('the setup ran in administrative install mode') }
Write-Step '01' 'first-install' $began $failures ([ordered]@{ run = $run; entry = $state.Entry; files = $state.Files.Count
    nonAdministrativeMode = ($log -match 'Administrative install mode: No') })

# --- 02 the program starts
function Test-Starts([string] $Number) {
    $began = [DateTime]::UtcNow
    $failures = New-Object System.Collections.ArrayList
    $started = Start-Program
    if (-not $started.Up) { [void] $failures.Add('the program did not start (its mutex did not appear within 30 s)') }
    Start-Sleep -Seconds 5
    if (-not (Test-ProgramRunning)) { [void] $failures.Add('the program did not stay up for 5 s') }
    $version = Get-Version $Program
    $how = Stop-Program
    Test-Data $failures -ProgramStarted
    Write-Step $Number 'program-starts' $began $failures ([ordered]@{ version = $version; ended = $how
        programCredentials = @($script:credentials -split "`n" | Where-Object { $_ }) })
}
Test-Starts '02'

# --- 03 a killed upgrade: what is left of the earlier version, and whether it still starts
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '03-killed-upgrade'
$earlier = Get-Installed
# (Killed once Qt6Core.dll is written: the log names the file after it, or after 60 s.)
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 120 {
    param($process, $gone)
    $said = if (Test-Path -LiteralPath $log) { (Read-Shared $log) } else { '' }
    $at = $said.IndexOf('\Qt6Core.dll')
    return ($at -ge 0 -and $said.IndexOf('Dest filename:', $at) -ge 0) -or $gone -gt 60
}
if ($run.Exit -ne 'killed') { [void] $failures.Add("the setup was not killed while it installed (it ended with $($run.Exit))") }
Start-Sleep -Seconds 2
$after = Get-Installed
# What a killed setup leaves: file by file against the earlier version's.
$kept = New-Object System.Collections.ArrayList
$replaced = New-Object System.Collections.ArrayList
$missing = New-Object System.Collections.ArrayList
$added = New-Object System.Collections.ArrayList
foreach ($path in $earlier.Files.Keys) {
    if (-not $after.Files.Contains($path)) { [void] $missing.Add($path) }
    elseif ($after.Files[$path].sha256 -eq $earlier.Files[$path].sha256) { [void] $kept.Add($path) }
    else { [void] $replaced.Add($path) }
}
foreach ($path in $after.Files.Keys) { if (-not $earlier.Files.Contains($path)) { [void] $added.Add($path) } }
# (Set aside for the copy in every mode since 0.1.2, review L3: a setup ended from outside runs no code of its own, so
# the earlier program stays aside, and nothing is there to start, until the next setup puts it back first, step 04.)
$programLeft = Test-Path -LiteralPath $Program
$asideLeft = Test-Path -LiteralPath "$Program.old-update"
if ($programLeft -or -not $asideLeft) { [void] $failures.Add('the earlier program was not set aside for the copy') }
$how = @()
$entryVersion = if ($after.Entry) { $after.Entry.DisplayVersion } else { $null }
Write-Step '03' 'killed-upgrade' $began $failures ([ordered]@{ run = $run; entryVersion = $entryVersion; programVersion = Get-Version $Program
    programSetAside = ($asideLeft -and -not $programLeft)
    stillTheEarlierVersions = $kept.Count; replaced = @($replaced); gone = @($missing); added = @($added); ended = $how
    leftInTemp = @(Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'is-*.tmp' | ForEach-Object Name) })

# --- 04 the upgrade
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
# (What an earlier version may have had and this one not, in the two folders Qt loads whole, and a file beside them.)
$staleFile = Join-Path $App 'plugins\imageformats\qkvm-vm-stale.dll'
$staleFolder = Join-Path $App 'qml\QkvmVmStale'
$outside = Join-Path $App 'qkvm-vm-outside.txt'
foreach ($file in $staleFile, (Join-Path $staleFolder 'qmldir'), $outside) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
    Set-Content -LiteralPath $file -Value 'put here by test_setup_vm.ps1' -Encoding ASCII
}
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '04-upgrade')`""
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
if (Test-Path -LiteralPath $staleFile) { [void] $failures.Add('a file in plugins that this version has not is still there') }
if (Test-Path -LiteralPath $staleFolder) { [void] $failures.Add('a folder in qml that this version has not is still there') }
if (-not (Test-Path -LiteralPath $outside)) { [void] $failures.Add('a file outside plugins and qml was removed') }
Remove-Item -LiteralPath $outside -Force -ErrorAction SilentlyContinue
$state = Test-Installed $newVersion $failures
if ((Get-Version $Program) -ne $newVersion) { [void] $failures.Add("the program is version $(Get-Version $Program), not $newVersion") }
# (The earlier program that 03's killed setup left aside, put back first, then set aside for this copy, outside update
# mode as in it: review L3.)
$said = Read-Log '04-upgrade'
if ($said -notmatch 'Put the earlier program back\.' -or $said -notmatch 'Set the earlier program aside for the copy\.') {
    [void] $failures.Add('the setup did not put the earlier program back first, or did not set it aside for its copy')
}
Test-Data $failures
Write-Step '04' 'upgrade' $began $failures ([ordered]@{ run = $run; entry = $state.Entry; files = $state.Files.Count })

# --- 05 the older setup over the newer version: refused, nothing changed
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$before = Get-Installed
$run = Invoke-Setup $OlderSetup "$SilentArguments /LOG=`"$(Get-Log '05-downgrade-refused')`"" 120
if ($run.Exit -eq 0) { [void] $failures.Add('the older setup ended with 0 over the newer version') }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or ($before.Entry | ConvertTo-Json) -ne ($after.Entry | ConvertTo-Json)) {
    [void] $failures.Add('the older setup changed something')
}
Write-Step '05' 'downgrade-refused' $began $failures ([ordered]@{ run = $run })

Test-Starts '06'

# --- 07 the update mode of the contract: the program ends within 10 s, the setup waits, installs and starts it again
$contract = { param([string] $log) "/SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /CURRENTUSER /UPDATE=1 /DIR=`"$App`" /LOG=`"$log`"" }
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('the program did not start before the update') }
$firstId = $started.Process.Id
$endedAt = $null
$tookEarly = $false
$setupSeen = $false
$run = Invoke-Setup $Setup (& $contract (Get-Log '07-update-mode')) 180 {
    param($process, $gone)
    # (As the program does: it starts the setup, then closes and exits within 10 s; here after 3 s.)
    if (-not $script:endedAt -and $gone -gt 3) { Stop-Process -Id $firstId -Force -ErrorAction SilentlyContinue; $script:endedAt = $gone; return $false }
    # (Once it has ended, nothing may take the program's mutex while the setup still holds its own.)
    if ($script:endedAt -and (Test-SetupRunning)) { if (Test-ProgramRunning) { $script:tookEarly = $true } else { $script:setupSeen = $true } }
    return $false
}
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$again = Wait-Until { Test-ProgramRunning } 30
$copies = @(Get-ProgramProcesses | Where-Object { $_.ProcessId -ne $firstId })
if (-not $again -or -not $copies.Count) { [void] $failures.Add('the setup did not start the program again from its folder') }
# (Started by the setup with the one argument --after-update, as the setup's log says of its [Run] entry: the copy it
# started may already have started itself again through the shell and ended. The command lines seen are kept.)
$commandLines = @(Get-CimInstance Win32_Process -Filter "Name = 'qkvm-desktop.exe'" | Where-Object { $_.ProcessId -ne $firstId } | ForEach-Object { [string] $_.CommandLine })
if ((Read-Log '07-update-mode') -notmatch 'Filename: [^\r\n]*\\qkvm-desktop\.exe[ \t]*\r?\n[^\r\n]*Parameters: --after-update[ \t]*\r?\n') {
    [void] $failures.Add('the setup''s log does not say it started the program with the one argument --after-update')
}
$state = Test-Installed $newVersion $failures
$log = Read-Log '07-update-mode'
if ($log -notmatch 'had ended after \d+ ms') { [void] $failures.Add('the setup''s log does not say it waited for the program') }
if (@(Get-ResultLines '07-update-mode').Count) { [void] $failures.Add('the log of a setup that installed has a result line of the contract') }
if (-not $script:setupSeen) { [void] $failures.Add('the setup''s own mutex was not seen while it ran') }
if ($script:tookEarly) { [void] $failures.Add('the program took its mutex while the setup still held its own') }
$how = Stop-Program
Test-Data $failures
Write-Step '07' 'update-mode' $began $failures ([ordered]@{ run = $run; programEndedAfterSeconds = $script:endedAt
    startedAgain = @($copies | ForEach-Object { $_.ExecutablePath }); commandLines = $commandLines; ended = $how })

# --- 08 the update mode with the program still running: exit code 105, nothing changed
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$before = Get-Installed
$run = Invoke-Setup $Setup (& $contract (Get-Log '08-update-still-running')) 180
if ($run.Exit -ne 105) { [void] $failures.Add("the setup ended with $($run.Exit), not 105") }
if ($run.Seconds -lt 55 -or $run.Seconds -gt 120) { [void] $failures.Add("the setup ended after $($run.Seconds) s, not after its 60 s") }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or ($before.Entry | ConvertTo-Json) -ne ($after.Entry | ConvertTo-Json)) {
    [void] $failures.Add('the setup changed something')
}
if (-not (Get-Process -Id $started.Process.Id -ErrorAction SilentlyContinue)) { [void] $failures.Add('the program is no longer running') }
# (The program reads this one line: it cannot see the exit code. It must be in the log though the setup ended itself.)
$resultLines = @(Get-ResultLines '08-update-still-running')
if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed') {
    [void] $failures.Add("the setup's log does not have the one result line of 105 ($($resultLines -join ' | '))")
}
$how = Stop-Program
Write-Step '08' 'update-still-running' $began $failures ([ordered]@{ run = $run; ended = $how; resultLines = $resultLines })

# --- 09 the update mode with a file of the program held open: refused before anything is changed
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$before = Get-Installed
$held = [IO.File]::Open((Join-Path $App 'Qt6Core.dll'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $run = Invoke-Setup $Setup (& $contract (Get-Log '09-update-file-in-use')) 180
} finally {
    $held.Dispose()
}
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7 (its Preparing to Install refusal)") }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or ($before.Entry | ConvertTo-Json) -ne ($after.Entry | ConvertTo-Json)) {
    [void] $failures.Add('the setup changed something')
}
$resultLines = @(Get-ResultLines '09-update-file-in-use')
if ($resultLines.Count -ne 1 -or -not $resultLines[0].StartsWith('qKVM-RESULT: 7 nothing was changed: a file of qKVM Desktop is in use or cannot be replaced (', [StringComparison]::Ordinal)) {
    [void] $failures.Add("the setup's log does not have the one result line of 7 for a file in use ($($resultLines -join ' | '))")
}
Write-Step '09' 'update-file-in-use' $began $failures ([ordered]@{ run = $run; resultLines = $resultLines })

# --- 10 the uninstaller while the program runs
$uninstaller = Join-Path $App 'unins000.exe'
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$before = Get-Installed
$run = Invoke-Setup $uninstaller "$SilentArguments /LOG=`"$(Get-Log '10-uninstall-refused')`"" 120
if ($run.Exit -eq 0) { [void] $failures.Add('the uninstaller ended with 0 while the program ran') }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or -not $after.Entry) { [void] $failures.Add('the uninstaller removed something while the program ran') }
$how = Stop-Program
Write-Step '10' 'uninstall-refused' $began $failures ([ordered]@{ run = $run; ended = $how })

# --- 11 the uninstall keeps the data
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$run = Invoke-Setup $uninstaller "$SilentArguments /LOG=`"$(Get-Log '11-uninstall')`"" 180
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the uninstaller ended with $($run.Exit)") }
# (Its second part, from a copy of it in Temp, may still be removing its own files for a moment.)
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
$after = Get-Installed
if ($after.Files.Count) { [void] $failures.Add("$($after.Files.Count) file(s) are left in the program's folder: $(@($after.Files.Keys) -join ', ')") }
if ($after.Entry -or $after.Machine.Count) { [void] $failures.Add('the Settings > Apps entry is still there') }
if ($after.StartMenu -or $after.Desktop) { [void] $failures.Add('a Start menu or desktop entry is still there') }
Test-Data $failures
Write-Step '11' 'uninstall' $began $failures ([ordered]@{ run = $run; folderLeft = (Test-Path -LiteralPath $App) })

# --- 12 the reinstall; then a start of the program while a setup's mutex is held
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '12-reinstall'
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`""
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$state = Test-Installed $newVersion $failures
# The program, started without --after-update while a setup holds its mutex, must end at once without taking its
# own. The mutex is held here, under the setup's own name in this session, as a setup holds it while it copies: a
# reinstall's log says it succeeded a few milliseconds before it ends, too soon to start the program within it
# (seen on Windows 10 and 11, 2026-11-12). Starts during a real copy: steps 07 and 14. And with the event a setup makes
# to say it is at work, named after that mutex and the number of a process that runs, this one's: a start takes a
# setup to be at work only so, never by the mutex alone (review R2, ui/win/setup_wait.h's setupRunsName).
$tookEarly = $false
$endedAtOnce = $false
$created = $false
$held = [Threading.Mutex]::new($true, $SetupMutex, [ref] $created)
$atWork = New-Object System.Threading.EventWaitHandle($false, [System.Threading.EventResetMode]::ManualReset, "$SetupMutex-$PID")
try {
    if (-not $created) { [void] $failures.Add("a setup's mutex was there already, held by another") }
    $startedDuring = Start-Process -FilePath $Program -WorkingDirectory $App -PassThru
    [void] $startedDuring.Handle
    $until = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $until) {
        if (Test-ProgramRunning) { $tookEarly = $true }
        if ($startedDuring.HasExited) { $endedAtOnce = $true; break }
        Start-Sleep -Milliseconds 100
    }
} finally {
    if ($created) { $held.ReleaseMutex() }
    $held.Dispose()
    $atWork.Dispose()
}
if (-not $endedAtOnce) { [void] $failures.Add('the program started while a setup held its mutex did not end within 10 s') }
elseif ($startedDuring.ExitCode -ne 0) { [void] $failures.Add("the program started while a setup held its mutex ended with $($startedDuring.ExitCode), not 0") }
if ($tookEarly) { [void] $failures.Add('the program started while a setup held its mutex took its own') }
$how = Stop-Program
Test-Data $failures
Write-Step '12' 'reinstall' $began $failures ([ordered]@{ run = $run; files = $state.Files.Count
    startedWhileSetupMutexHeld = [ordered]@{ endedAtOnce = $endedAtOnce; tookItsMutex = $tookEarly }; ended = $how })
Test-Starts '13'

# --- 14 starts of the program, again and again, while the setup copies its files
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '14-starts-during-copy'
$script:starts = 0
$script:startsRefused = 0
$script:lastStart = [DateTime]::MinValue
$script:tookEarly = $false
# (From the first file the setup writes until it says the copy is done: a start without an argument every half
# second, as a person might double-click it then. Each finds no program: it is set aside for the copy in every mode,
# review L3, as step 15 sees in update mode.)
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
    param($process, $gone)
    $said = if (Test-Path -LiteralPath $log) { (Read-Shared $log) } else { '' }
    # (None once the setup has named the program's own file, the last it writes: a start then may reach the program's
    # check of the setup's mutex only after the setup has ended, and run as the program does.)
    if ($said.Contains('Dest filename:') -and $said -notmatch '(?m)Dest filename: .*\\qkvm-desktop\.exe\s*$' -and
        ([DateTime]::UtcNow - $script:lastStart).TotalMilliseconds -ge 500) {
        $script:lastStart = [DateTime]::UtcNow
        try { Start-Process -FilePath $Program -WorkingDirectory $App | Out-Null; $script:starts++ } catch { $script:startsRefused++ }
    }
    if ((Test-SetupRunning) -and (Test-ProgramRunning)) { $script:tookEarly = $true }
    return $false
}
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit) with the program started during its copy") }
if ($script:starts + $script:startsRefused -eq 0) { [void] $failures.Add('the program was not once started while the setup copied its files') }
if ($script:starts -gt 0) { [void] $failures.Add("$($script:starts) start(s) during the copy found a program: the earlier one was not set aside") }
if ($script:tookEarly) { [void] $failures.Add('a start during the copy took the program''s mutex while the setup held its own') }
# (Every start while the setup holds its mutex ends at once. One that comes as the setup ends may find its mutex gone
# and run as the program does, holding its own: seen on Windows 10, 2026-11-12. That one is left, a single process
# holding the program's mutex; any other still running is a start that neither ended nor ran.)
$settled = Wait-Until { $left = @(Get-ProgramProcesses); $left.Count -eq 0 -or ($left.Count -eq 1 -and (Test-ProgramRunning)) } 15
$leftRunning = @(Get-ProgramProcesses).Count
$lateStart = $leftRunning -eq 1 -and (Test-ProgramRunning)
if (-not $settled) { [void] $failures.Add("a start during the copy is still running after the setup ended, without the program's mutex ($leftRunning process(es))") }
[void] (Stop-Program)
$state = Test-Installed $newVersion $failures
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('after the install, the new version does not start') }
$version = Get-Version $Program
if ($version -ne $newVersion) { [void] $failures.Add("the program is version $version, not $newVersion") }
$how = Stop-Program
Test-Data $failures
Write-Step '14' 'starts-during-copy' $began $failures ([ordered]@{ run = $run; starts = $script:starts; startsRefused = $script:startsRefused
    lateStartRanAsTheProgram = $lateStart; files = $state.Files.Count; version = $version; ended = $how })

# (Steps 15 and 16 start from the older version: uninstalled, then the older setup again, as a machine that has it.)
function Reset-ToOlder([string] $Number, [System.Collections.ArrayList] $Failures) {
    $uninstalled = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log "$Number-uninstall-first")`"" 180
    [void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
    $installed = Invoke-Setup $OlderSetup "$SilentArguments /LOG=`"$(Get-Log "$Number-older-first")`""
    if ($uninstalled.Exit -ne 0 -or $installed.Exit -ne 0) {
        [void] $Failures.Add("the older version could not be installed again first (uninstall $($uninstalled.Exit), install $($installed.Exit))")
    }
    return Get-Installed
}

# --- 15 the contract's update across a change of the Qt libraries, with starts of the program during its copy
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '15' $failures
$olderQt = if ($older.Files.Contains('Qt6Core.dll')) { $older.Files['Qt6Core.dll'].sha256 } else { '' }
$started = Start-Program
$firstId = $started.Process.Id
$log = Get-Log '15-update-across-qt'
$aside = "$Program.old-update"
$script:ended15 = $null
$script:starts15 = 0
$script:refused15 = 0
$script:last15 = [DateTime]::MinValue
$script:absent15 = $false
$script:aside15 = $false
$script:took15 = $false
$run = Invoke-Setup $Setup (& $contract $log) 300 {
    param($process, $gone)
    if (-not $script:ended15 -and $gone -gt 3) { Stop-Process -Id $firstId -Force -ErrorAction SilentlyContinue; $script:ended15 = $gone; return $false }
    $said = if (Test-Path -LiteralPath $log) { (Read-Shared $log) } else { '' }
    if ($said.Contains('Dest filename:') -and -not $said.Contains('Installation process succeeded')) {
        if (-not (Test-Path -LiteralPath $Program)) { $script:absent15 = $true }
        if (Test-Path -LiteralPath $aside) { $script:aside15 = $true }
        if (([DateTime]::UtcNow - $script:last15).TotalMilliseconds -ge 500) {
            $script:last15 = [DateTime]::UtcNow
            try { Start-Process -FilePath $Program -WorkingDirectory $App | Out-Null; $script:starts15++ } catch { $script:refused15++ }
        }
    }
    if ($script:ended15 -and (Test-SetupRunning) -and (Test-ProgramRunning)) { $script:took15 = $true }
    return $false
}
if ($run.Exit -ne 0) { [void] $failures.Add("the update ended with $($run.Exit)") }
$after = Get-Installed
$newQt = if ($after.Files.Contains('Qt6Core.dll')) { $after.Files['Qt6Core.dll'].sha256 } else { '' }
if (-not $olderQt -or $newQt -eq $olderQt) { [void] $failures.Add('the two setups'' Qt6Core.dll are the same: make the newer one''s differ (see the head of this test)') }
if (-not $script:aside15 -or -not $script:absent15) { [void] $failures.Add('the program''s file was not set aside for the copy') }
if (Test-Path -LiteralPath $aside) { [void] $failures.Add('the set-aside copy of the program is still there after the update') }
if ($script:took15) { [void] $failures.Add('a start during the copy took the program''s mutex while the setup held its own') }
if (-not (Wait-Until { Test-ProgramRunning } 30)) { [void] $failures.Add('the setup did not start the program again after the update') }
if ((Read-Log '15-update-across-qt') -notmatch 'Filename: [^\r\n]*\\qkvm-desktop\.exe[ \t]*\r?\n[^\r\n]*Parameters: --after-update[ \t]*\r?\n') {
    [void] $failures.Add('the setup''s log does not say it started the program with the one argument --after-update')
}
$how = Stop-Program
$state = Test-Installed $newVersion $failures
Test-Data $failures
Write-Step '15' 'update-across-qt' $began $failures ([ordered]@{ run = $run; qtChanged = ($olderQt -and $newQt -ne $olderQt)
    startsDuringCopy = $script:starts15; startsFindingNoProgram = $script:refused15; ended = $how; files = $state.Files.Count })

# --- 16 the contract's update failing during its copy: the earlier program back in its place
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '16' $failures
$olderProgram = if ($older.Files.Contains('qkvm-desktop.exe')) { $older.Files['qkvm-desktop.exe'].sha256 } else { '' }
# (The file the setup puts in last before the program, held once the copy has begun: the setup's own check before
# the copy finds it free, and its copy then fails on it, as on a file in use or a full disk.)
$manifest = Get-Content -LiteralPath (Join-Path $App 'package-manifest.json') -Raw | ConvertFrom-Json
$names = [string[]] @($manifest.files | ForEach-Object { [string] $_.path } | Where-Object { $_ -ne 'qkvm-desktop.exe' })
[Array]::Sort($names, [StringComparer]::Ordinal)
$heldName = $names[-1]
$started = Start-Program
$firstId = $started.Process.Id
$log = Get-Log '16-update-fails-during-copy'
$script:ended16 = $null
$script:held16 = $null
try {
    $run = Invoke-Setup $Setup (& $contract $log) 300 {
        param($process, $gone)
        if (-not $script:ended16 -and $gone -gt 3) { Stop-Process -Id $firstId -Force -ErrorAction SilentlyContinue; $script:ended16 = $gone; return $false }
        if (-not $script:held16 -and (Test-Path -LiteralPath $log) -and (Read-Shared $log).Contains('Dest filename:')) {
            try { $script:held16 = [IO.File]::Open((Join-Path $App ($heldName -replace '/', '\')), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None) } catch { }
        }
        return $false
    }
} finally {
    if ($script:held16) { $script:held16.Dispose() }
}
if (-not $script:held16) { [void] $failures.Add("$heldName could not be held during the copy") }
if ($run.Exit -eq 0) { [void] $failures.Add('the update did not fail with a file of the folder held during its copy') }
$programNow = if (Test-Path -LiteralPath $Program) { (Get-FileHash -LiteralPath $Program -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' }
if (-not $programNow) { [void] $failures.Add('after the failed update there is no program') }
elseif ($programNow -ne $olderProgram) { [void] $failures.Add('after the failed update the program is not the earlier one') }
if (Test-Path -LiteralPath "$Program.old-update") { [void] $failures.Add('the set-aside copy of the program is still there after the failed update') }
$restarted = Start-Program
if (-not $restarted.Up) { [void] $failures.Add('after the failed update, the earlier program does not start') }
$how = Stop-Program
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '16-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
Test-Data $failures
Write-Step '16' 'update-fails-during-copy' $began $failures ([ordered]@{ run = $run; held = $heldName; programIsTheEarlier = ($programNow -eq $olderProgram)
    ended = $how; finish = $finish })

# --- 17 a setup ended halfway, then the uninstaller: the program's folder gone, nothing left in it
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '17-killed-then-uninstall'
# (Killed once its copy has reached licenses: what is being written then, in whatever folder, is left as Inno Setup's
# temporary file, which the uninstaller must take with the rest. Seen in the review of 2026-11-12: a cut below the
# top of the folder, and an uninstall with no whole setup between, had never been tried.)
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 120 {
    param($process, $gone)
    $said = Read-Shared $log
    $at = $said.IndexOf('\licenses\')
    return ($at -ge 0 -and $said.IndexOf('Dest filename:', $at) -ge 0) -or $gone -gt 60
}
if ($run.Exit -ne 'killed') { [void] $failures.Add("the setup was not killed while it installed (it ended with $($run.Exit))") }
Start-Sleep -Seconds 2
$leftovers = @(Get-ChildItem -LiteralPath $App -Recurse -File -Force -Filter 'is-*.tmp' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($App.Length + 1) })
$uninstall = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '17-uninstall')`"" 180
if ($uninstall.Exit -ne 0) { [void] $failures.Add("the uninstaller ended with $($uninstall.Exit)") }
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
if (Test-Path -LiteralPath $App) {
    $left = @(Get-ChildItem -LiteralPath $App -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($App.Length + 1) })
    [void] $failures.Add("the program's folder is left after the uninstall, holding: $($left -join ', ')")
}
Test-Data $failures
Write-Step '17' 'killed-then-uninstall' $began $failures ([ordered]@{ run = $run; leftBeforeTheUninstall = $leftovers
    uninstall = $uninstall; folderLeft = (Test-Path -LiteralPath $App) })

# --- Steps 18 to 23 (0.1.2): a running copy closed for the person. What they need: a program held so that it does not
# answer (every thread of it suspended), its windows looked at, and the setup's own windows driven as a person would
# (UI Automation; a press from a thread of its own, as what it starts in the setup, a wait or a box, would otherwise
# hold this script).
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -ReferencedAssemblies UIAutomationClient, UIAutomationTypes, WindowsBase -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Automation;
public static class QkvmVm {
    [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr process);
    [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string kind, string title);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    delegate bool EachWindow(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EachWindow each, IntPtr data);
    public static int VisibleWindowsOf(uint process) {
        int count = 0;
        EnumWindows(delegate (IntPtr window, IntPtr data) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == process && IsWindowVisible(window)) count++;
            return true;
        }, IntPtr.Zero);
        return count;
    }
    // A process's own top-level windows that are shown, from Windows' own list (EnumWindows): no window of any other
    // process is asked anything, a hung program's among them (review V1).
    public static IntPtr[] WindowsOf(uint process) {
        System.Collections.Generic.List<IntPtr> found = new System.Collections.Generic.List<IntPtr>();
        EnumWindows(delegate (IntPtr window, IntPtr data) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == process && IsWindowVisible(window)) found.Add(window);
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }
    public static bool HasWindowOfKind(uint process, string kind) {
        for (IntPtr at = FindWindowEx(IntPtr.Zero, IntPtr.Zero, kind, null); at != IntPtr.Zero; at = FindWindowEx(IntPtr.Zero, at, kind, null)) {
            uint owner;
            GetWindowThreadProcessId(at, out owner);
            if (owner == process) return true;
        }
        return false;
    }
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr window, System.Text.StringBuilder text, int most);
    // The thread a program began with (its interface's, in qKVM Desktop) suspended alone, the rest of it left to run;
    // its handle, for ResumeAndLetGo (zero where it could not be).
    public static IntPtr SuspendFirstThread(int process) {
        System.Diagnostics.ProcessThread first = null;
        foreach (System.Diagnostics.ProcessThread one in System.Diagnostics.Process.GetProcessById(process).Threads) {
            try { if (first == null || one.StartTime < first.StartTime) first = one; } catch (Exception) { }
        }
        if (first == null) return IntPtr.Zero;
        IntPtr thread = OpenThread(0x0002, false, (uint) first.Id);
        if (thread != IntPtr.Zero && SuspendThread(thread) == 0xFFFFFFFF) { CloseHandle(thread); return IntPtr.Zero; }
        return thread;
    }
    public static void ResumeAndLetGo(IntPtr thread) {
        if (thread == IntPtr.Zero) return;
        ResumeThread(thread);
        CloseHandle(thread);
    }
    // The title of a process's window of this class ("" where it has none).
    public static string TitleOf(uint process, string kind) {
        for (IntPtr at = FindWindowEx(IntPtr.Zero, IntPtr.Zero, kind, null); at != IntPtr.Zero; at = FindWindowEx(IntPtr.Zero, at, kind, null)) {
            uint owner;
            GetWindowThreadProcessId(at, out owner);
            if (owner != process) continue;
            System.Text.StringBuilder text = new System.Text.StringBuilder(1024);
            GetWindowText(at, text, text.Capacity);
            return text.ToString();
        }
        return "";
    }
    public static void Press(AutomationElement button) {
        Thread pressing = new Thread(delegate () {
            try { ((InvokePattern) button.GetCurrentPattern(InvokePattern.Pattern)).Invoke(); } catch (Exception) { }
        });
        pressing.IsBackground = true;
        pressing.Start();
    }
    // (Windows' own look at a setup's windows, which never waits on one: V1b. UI Automation named the buttons of Inno
    // Setup's wizard no buttons where it reached them by their window's handle, and a look at the loader's window,
    // whose thread waits for the wizard without pumping a message, can wait for ever.)
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EachWindow each, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern uint RealGetWindowClassW(IntPtr window, System.Text.StringBuilder kind, uint most);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] static extern int GetWindowLongW(IntPtr window, int index);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern IntPtr SendNullTimeout(IntPtr window, uint message, IntPtr w, IntPtr l, uint flags, uint ms, out IntPtr result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern IntPtr SendTextTimeout(IntPtr window, uint message, IntPtr w, System.Text.StringBuilder l, uint flags, uint ms, out IntPtr result);
    // Whether a window's thread answers within a second (WM_NULL, SMTO_ABORTIFHUNG): one that waits without pumping a
    // message, or a hung one, is never asked anything else.
    public static bool Answers(IntPtr window) {
        IntPtr result;
        return SendNullTimeout(window, 0x0000, IntPtr.Zero, IntPtr.Zero, 0x0002, 1000, out result) != IntPtr.Zero;
    }
    // A window's text as it says it itself (WM_GETTEXT, within a second; "" where it does not answer). GetWindowText
    // gives no control's text of another process.
    public static string TextOf(IntPtr window) {
        System.Text.StringBuilder text = new System.Text.StringBuilder(2048);
        IntPtr result;
        if (SendTextTimeout(window, 0x000D, (IntPtr) text.Capacity, text, 0x0002, 1000, out result) == IntPtr.Zero) return "";
        return text.ToString();
    }
    // Every shown window inside a top-level window, at any depth, in Windows' own order (a hidden page's are not shown).
    public static IntPtr[] ShownInside(IntPtr top) {
        System.Collections.Generic.List<IntPtr> found = new System.Collections.Generic.List<IntPtr>();
        EnumChildWindows(top, delegate (IntPtr window, IntPtr data) {
            if (IsWindowVisible(window)) found.Add(window);
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }
    // The push buttons inside a top-level window that are shown and enabled: Windows' own (base class "Button", by
    // RealGetWindowClass: a message box's) and Inno Setup's TNewButton, whose base class Windows says is TNewButton
    // itself and which UI Automation names a pane (the VM suite of 26c4c6e7 pressed none: V1c); and by their kind of
    // button (a push button or a command link, not a check box, a choice or a frame).
    public static IntPtr[] ButtonsOf(IntPtr top) {
        System.Collections.Generic.List<IntPtr> found = new System.Collections.Generic.List<IntPtr>();
        foreach (IntPtr window in ShownInside(top)) {
            System.Text.StringBuilder kind = new System.Text.StringBuilder(64);
            RealGetWindowClassW(window, kind, (uint) kind.Capacity);
            int type = GetWindowLongW(window, -16) & 0x0F;
            if ((kind.ToString() == "Button" || kind.ToString() == "TNewButton") && IsWindowEnabled(window) && (type == 0 || type == 1 || type == 0x0E || type == 0x0F)) found.Add(window);
        }
        return found.ToArray();
    }
    // The items of a list inside the setup's windows, as the list says them itself (LB_GETCOUNT, LB_GETTEXTLEN and
    // LB_GETTEXT, each within a second; at most 32): Inno Setup's TNewCheckListBox, the last page's "Start qKVM
    // Desktop" among them, whose items are no windows of their own and have no name in UI Automation (V1c), and
    // Windows' own list box. None for any other window, or one that does not answer.
    public static string[] ItemsOf(IntPtr window) {
        System.Collections.Generic.List<string> items = new System.Collections.Generic.List<string>();
        System.Text.StringBuilder kind = new System.Text.StringBuilder(64);
        RealGetWindowClassW(window, kind, (uint) kind.Capacity);
        if (kind.ToString() != "TNewCheckListBox" && kind.ToString() != "ListBox") return items.ToArray();
        IntPtr count;
        if (SendNullTimeout(window, 0x018B, IntPtr.Zero, IntPtr.Zero, 0x0002, 1000, out count) == IntPtr.Zero) return items.ToArray();
        for (int at = 0; at < Math.Min(count.ToInt64(), 32); at++) {
            IntPtr length;
            if (SendNullTimeout(window, 0x018A, (IntPtr) at, IntPtr.Zero, 0x0002, 1000, out length) == IntPtr.Zero) break;
            if (length.ToInt64() < 0 || length.ToInt64() > 1024) continue;
            System.Text.StringBuilder item = new System.Text.StringBuilder((int) length.ToInt64() + 1);
            IntPtr said;
            if (SendTextTimeout(window, 0x0189, (IntPtr) at, item, 0x0002, 1000, out said) == IntPtr.Zero) break;
            if (said.ToInt64() >= 0) items.Add(item.ToString());
        }
        return items.ToArray();
    }
    // A press of a window's own button, as a click of it (BM_CLICK, as UI Automation presses one), posted: never
    // waited for, as what it starts (a box, a wait) would hold this script.
    public static void Click(IntPtr button) { PostMessageW(button, 0x00F5, IntPtr.Zero, IntPtr.Zero); }
}
'@

# The window every version of the program keeps for its icon (ui/win/tray.cpp), and the program's settings.
$IconWindow = 'QKVM Desktop notification area'
$SettingsFile = Join-Path $Local 'QKVM Desktop\settings.json'
$OverridesFile = "$SettingsFile.setup-overrides"

function Set-StaysInTray([bool] $On) {
    <# The program's own setting "stay in the tray" (settings.json's stay_in_tray), as it writes it (UTF-8 without a
    byte order mark); whether it was there to be set. Only while the program does not run. #>
    if (-not (Test-Path -LiteralPath $SettingsFile)) { return $false }
    $text = [IO.File]::ReadAllText($SettingsFile)
    $pattern = '"stay_in_tray"(\s*):(\s*)(true|false)'
    if ($text -notmatch $pattern) { return $false }
    $word = if ($On) { 'true' } else { 'false' }
    $text = [regex]::Replace($text, $pattern, { param($found) '"stay_in_tray"' + $found.Groups[1].Value + ':' + $found.Groups[2].Value + $word })
    [IO.File]::WriteAllText($SettingsFile, $text, (New-Object Text.UTF8Encoding $false))
    return $true
}

function Stop-ById([int] $Id) {
    <# A process ended, whatever it is doing (one suspended is let go of first). #>
    $process = Get-Process -Id $Id -ErrorAction SilentlyContinue
    if (-not $process) { return }
    try { [void] [QkvmVm]::NtResumeProcess($process.Handle) } catch { }
    Stop-Process -Id $Id -Force -ErrorAction SilentlyContinue
    [void] $process.WaitForExit(10000)
}

function Test-Unchanged($Before, [System.Collections.ArrayList] $Failures) {
    $after = Get-Installed
    if (($Before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or ($Before.Entry | ConvertTo-Json) -ne ($after.Entry | ConvertTo-Json)) {
        [void] $Failures.Add('the setup changed something')
    }
}

function Get-SetupProcesses([int] $SetupId) {
    <# The setup's process and those it started, and they in turn, by Windows' list (at most 16): a process is taken
    for a child only where it began at or after its parent, as Stop-Tree takes one (Windows keeps a parent's number
    after it has ended, and it may since be another's). Inno Setup runs its wizard in a second process, setup.tmp in
    a folder is-*.tmp, that the setup's own file starts (V1b). #>
    $ids = New-Object System.Collections.ArrayList
    $next = New-Object System.Collections.Queue
    $next.Enqueue($SetupId)
    while ($next.Count -and $ids.Count -lt 16) {
        $id = [int] $next.Dequeue()
        $began = $null
        try { $began = [Diagnostics.Process]::GetProcessById($id).StartTime } catch { }
        if ($null -eq $began) { continue }
        [void] $ids.Add($id)
        foreach ($child in @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $id")) {
            if ($null -ne $child.CreationDate -and $child.CreationDate -ge $began -and -not $ids.Contains([int] $child.ProcessId)) { $next.Enqueue([int] $child.ProcessId) }
        }
    }
    return @($ids)
}

function Get-SetupWindows([int] $SetupId) {
    <# The top-level windows of a running setup's processes (Get-SetupProcesses), boxes included, that are shown: from
    Windows' own list of each process's windows, and only those whose thread answers within a second. The loader's
    window, whose thread waits for the wizard without pumping a message, is never asked anything (a look at it can
    wait for ever: V1b), nor is any window of another process, a hung program's among them (V1). Their handles. #>
    $found = New-Object System.Collections.ArrayList
    foreach ($id in Get-SetupProcesses $SetupId) {
        foreach ($handle in [QkvmVm]::WindowsOf([uint32] $id)) {
            if ([QkvmVm]::Answers($handle)) { [void] $found.Add($handle) }
        }
    }
    return @($found)
}

function Find-SetupButton([int] $SetupId, [string] $Name, $Within = $null) {
    <# An enabled, shown button of the setup's windows (of the one top-level window $Within, where it is given) whose
    name, without its & sign, begins with $Name; $null where none. Windows' own buttons first, by their base class and
    the text they say themselves: the wizard's (Inno Setup's TNewButton) and a message box's (UI Automation names
    Inno Setup's buttons panes: the VM runs of 3f88ccb2 and 26c4c6e7 pressed none, V1b and V1c). Then what UI Automation names a button
    there: a task dialog's, which are no windows of their own. What is found: its top-level window, and the button's
    handle or UI Automation's element (Push-SetupButton presses either). #>
    $tops = if ($Within) { @($Within) } else { @(Get-SetupWindows $SetupId) }
    foreach ($top in $tops) {
        foreach ($button in [QkvmVm]::ButtonsOf($top)) {
            if ([QkvmVm]::TextOf($button).Replace('&', '').StartsWith($Name, [StringComparison]::OrdinalIgnoreCase)) {
                return [pscustomobject]@{ Window = $top; Button = $button; Element = $null }
            }
        }
    }
    $isButton = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
    foreach ($top in $tops) {
        try {
            foreach ($element in [System.Windows.Automation.AutomationElement]::FromHandle($top).FindAll([System.Windows.Automation.TreeScope]::Descendants, $isButton)) {
                if (([string] $element.Current.Name).Replace('&', '').StartsWith($Name, [StringComparison]::OrdinalIgnoreCase) -and $element.Current.IsEnabled) {
                    return [pscustomobject]@{ Window = $top; Button = [IntPtr]::Zero; Element = $element }
                }
            }
        } catch { }
    }
    return $null
}

function Find-ButtonBeside([int] $SetupId, [string] $Beside, [string] $Name) {
    <# A button whose name begins with $Name in the very top-level window (a box) where a button beginning with $Beside
    is: the Cancel of a box, and not the wizard's. $null where none. #>
    $known = Find-SetupButton $SetupId $Beside
    if (-not $known) { return $null }
    return Find-SetupButton $SetupId $Name $known.Window
}

function Push-SetupButton($Found) {
    <# A press of what Find-SetupButton found: a window's own button clicked by a message posted to it, and a task
    dialog's by UI Automation from a thread of its own; neither waited for, as what it starts (a box, a wait) would
    hold this script. #>
    if ($Found.Element) { [QkvmVm]::Press($Found.Element) } else { [QkvmVm]::Click($Found.Button) }
}

function Get-SetupText([int] $SetupId) {
    <# Every text the setup's windows show, one a line, what a person reads there: each top-level window's title and
    the text of each shown window in it, as each says it itself (a hidden page's are not shown), and the items of
    each list in it (the last page's "Start qKVM Desktop", V1c), then what UI Automation names in it (a task dialog's
    words, which are in no window of their own). #>
    $said = New-Object System.Collections.ArrayList
    foreach ($top in Get-SetupWindows $SetupId) {
        [void] $said.Add([QkvmVm]::TextOf($top))
        foreach ($inside in [QkvmVm]::ShownInside($top)) {
            [void] $said.Add([QkvmVm]::TextOf($inside))
            foreach ($item in [QkvmVm]::ItemsOf($inside)) { [void] $said.Add($item) }
        }
        try {
            foreach ($one in [System.Windows.Automation.AutomationElement]::FromHandle($top).FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) { [void] $said.Add([string] $one.Current.Name) }
        } catch { }
    }
    return ($said -join "`n")
}
function Invoke-SetupByHand([string] $Log, [object[]] $Presses, [int] $Seconds = 300) {
    <#
    The new setup run as a person runs it (no switch but its log): each of $Presses (the start of a button's name) is
    pressed once it is there, in that order, and what the setup's windows showed just before each press is kept
    (Shown, one for each press). Whether every press was made, and how long after the one before each came (Gaps);
    what the windows showed last while a press was waited for (LastSeen, looked at every 10 s), for a run that stuck.
    A press may also be @{ Press = <name>; Beside = <name> } (the button of the box where the other is: a box's own
    Cancel), or a script block, run once when its turn comes (what a person does elsewhere meanwhile).
    #>
    $script:byHand = [ordered]@{ At = 0; Shown = (New-Object System.Collections.ArrayList); Gaps = (New-Object System.Collections.ArrayList); Last = 0.0
        LastSeen = ''; Looked = 0.0 }
    $run = Invoke-Setup $Setup "/LOG=`"$Log`"" $Seconds {
        param($process, $gone)
        # (Nothing pressed in the first 3 s: the wizard is given the time a person's eyes take to find it.)
        if ($script:byHand.At -ge $Presses.Count -or $gone -lt 3) { return $false }
        if ($gone - $script:byHand.Looked -ge 10) { $script:byHand.LastSeen = Get-SetupText $process.Id; $script:byHand.Looked = $gone }
        $step = $Presses[$script:byHand.At]
        if ($step -is [scriptblock]) {
            [void] $script:byHand.Shown.Add((Get-SetupText $process.Id))
            [void] $script:byHand.Gaps.Add([Math]::Round($gone - $script:byHand.Last, 1))
            [void] (& $step)
            $script:byHand.Last = $gone
            $script:byHand.At++
            return $false
        }
        $button = if ($step -is [hashtable]) { Find-ButtonBeside $process.Id ([string] $step.Beside) ([string] $step.Press) } else { Find-SetupButton $process.Id ([string] $step) }
        if ($button) {
            [void] $script:byHand.Shown.Add((Get-SetupText $process.Id))
            [void] $script:byHand.Gaps.Add([Math]::Round($gone - $script:byHand.Last, 1))
            $script:byHand.Last = $gone
            Push-SetupButton $button
            $script:byHand.At++
        }
        return $false
    } {
        # (Past $Seconds: once every press is made, the setup is given 15 minutes more after the last one, to end of
        # itself; a copy under way is waited for in any case, review V1.)
        param($process, $gone)
        return $script:byHand.At -ge $Presses.Count -and ($gone - $script:byHand.Last) -lt 900
    }
    return [pscustomobject]@{ Run = $run; Pressed = $script:byHand.At; Shown = @($script:byHand.Shown); Gaps = @($script:byHand.Gaps)
        LastSeen = $(if ($script:byHand.At -lt $Presses.Count) { $script:byHand.LastSeen } else { '' }) }
}

function Test-StartedAgain([string] $LogName, [System.Collections.ArrayList] $Failures, [bool] $Expected) {
    <# Whether the setup started the program at its end (its log, and the program's mutex), as $Expected says. #>
    $said = (Read-Log $LogName) -match 'Filename: [^\r\n]*\\qkvm-desktop\.exe[ \t]*\r?\n[^\r\n]*Parameters: --after-update[ \t]*\r?\n'
    $running = Wait-Until { Test-ProgramRunning } $(if ($Expected) { 30 } else { 5 })
    if ($Expected -and (-not $said -or -not $running)) { [void] $Failures.Add('the setup did not start the program again') }
    if (-not $Expected -and ($said -or $running)) { [void] $Failures.Add('the setup started the program, which was not running before it') }
}

# (The new version first: 17 left nothing installed.)
$installed = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '18-install-first')`""
$firstFailures = New-Object System.Collections.ArrayList
if ($installed.Exit -ne 0) { [void] $firstFailures.Add("the new version could not be installed first ($($installed.Exit))") }

# --- 18 the program only in the tray: closed by the setup, the orderly way, and started again
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
foreach ($failure in $firstFailures) { [void] $failures.Add($failure) }
$log = Get-Log '18-tray-only-closed'
if (-not (Set-StaysInTray $true)) { [void] $failures.Add('the program''s settings have no stay_in_tray to set') }
$started = Start-Program
$trayId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 3
[void] $started.Process.CloseMainWindow()
Start-Sleep -Seconds 3
$onlyTray = (Get-Process -Id $trayId -ErrorAction SilentlyContinue) -and [QkvmVm]::VisibleWindowsOf($trayId) -eq 0 -and [QkvmVm]::HasWindowOfKind($trayId, $IconWindow)
if (-not $onlyTray) { [void] $failures.Add('the program did not go on in the tray alone, with no window to be seen') }
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 180
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$said = Read-Log '18-tray-only-closed'
if ($said -notmatch 'Asked qKVM Desktop to close, the orderly way\.') { [void] $failures.Add('the setup''s log does not say it asked the program to close') }
if ($said -notmatch 'qKVM Desktop closed after \d+ ms\.') { [void] $failures.Add('the setup''s log does not say the program closed') }
if ($said -match 'ended by force') { [void] $failures.Add('the program was ended by force') }
if (Get-Process -Id $trayId -ErrorAction SilentlyContinue) { [void] $failures.Add('the program that ran in the tray is still running') }
if (@(Get-ResultLines '18-tray-only-closed').Count) { [void] $failures.Add('the log of a setup that installed has a result line of the contract') }
$state = Test-Installed $newVersion $failures
Test-StartedAgain '18-tray-only-closed' $failures $true
$how = Stop-Program
if (-not (Set-StaysInTray $false)) { [void] $failures.Add('the program''s setting stay_in_tray could not be put back') }
Test-Data $failures -ProgramStarted
Write-Step '18' 'tray-only-closed' $began $failures ([ordered]@{ run = $run; firstInstall = $installed; onlyInTheTray = [bool] $onlyTray; files = $state.Files.Count; ended = $how })

# --- 19 the program holding what its end would cut: nothing changed, its sentence in the result line; and a record left
# from before whose doubt is over holds nothing (B12)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '19-busy-held'
# (The notes' times as the program writes them: UTC, to the millisecond, whatever this machine's culture.)
function Get-NoteTime([DateTime] $At) { $At.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture) }
# (First, as B12 found it on 0.1.0's own profile: a server's BIOS setup boot override noted beside the settings, for a
# server at a loopback port where nothing answers and on no card, its doubt over long ago: a day ago, by this machine's
# own clock, so that a machine whose clock is behind takes it as over too. It holds nothing: the silent setup closes the
# program and upgrades it, and the note is byte for byte what it was.)
$dayAgo = [DateTime]::UtcNow.AddDays(-1)
$left = '{"version":1,"servers":[{"address":"https://127.0.0.1:9/","machine":"","since":"' + (Get-NoteTime $dayAgo) + '","doubtUntil":"' + (Get-NoteTime $dayAgo.AddMinutes(2)) + '"}]}'
[IO.File]::WriteAllText($OverridesFile, $left, (New-Object Text.UTF8Encoding $false))
$leftBytes = [IO.File]::ReadAllBytes($OverridesFile)
$started = Start-Program
$overId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$over = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '19-doubt-over')`"" 180
if ($over.Exit -ne 0) { [void] $failures.Add("with a note whose doubt is over, the setup ended with $($over.Exit), not 0") }
if (Get-Process -Id $overId -ErrorAction SilentlyContinue) { [void] $failures.Add('with a note whose doubt is over, the program was not closed') }
if ((Read-Log '19-doubt-over') -match 'stays open|ended by force') { [void] $failures.Add('a note whose doubt is over held the program, or it was ended by force') }
Test-StartedAgain '19-doubt-over' $failures $true
[void] (Stop-Program)
$nowBytes = if (Test-Path -LiteralPath $OverridesFile) { [IO.File]::ReadAllBytes($OverridesFile) } else { [byte[]] @() }
if ([Convert]::ToBase64String($nowBytes) -cne [Convert]::ToBase64String($leftBytes)) { [void] $failures.Add('the note whose doubt was over is not what it was') }
# (Then a change that may still come: a note whose doubt goes on for eight minutes more, as a start into BIOS setup
# leaves it, for the same server, within the longest doubt a press notes (SetupOverrides::longestDoubtMs, ten minutes:
# one that reaches further was noted by a clock that ran ahead, and holds nothing); held until its doubt is over.)
$notedAt = [DateTime]::UtcNow
$noted = '{"version":1,"servers":[{"address":"https://127.0.0.1:9/","machine":"","since":"' + (Get-NoteTime $notedAt) + '","doubtUntil":"' + (Get-NoteTime $notedAt.AddMinutes(8)) + '"}]}'
[IO.File]::WriteAllText($OverridesFile, $noted, (New-Object Text.UTF8Encoding $false))
$started = Start-Program
$busyId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 180
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
Test-Unchanged $before $failures
if (-not (Get-Process -Id $busyId -ErrorAction SilentlyContinue)) { [void] $failures.Add('the program is no longer running') }
$resultLines = @(Get-ResultLines '19-busy-held')
$heldLine = 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running and stays open: A start into BIOS setup of a server is under way. Install once it has ended; /FORCECLOSE=1 ends it by force'
if ($resultLines.Count -ne 1 -or $resultLines[0] -cne $heldLine) {
    [void] $failures.Add("the setup's log does not have the one result line with the program's sentence ($($resultLines -join ' | '))")
}
if ((Read-Log '19-busy-held') -match 'ended by force') { [void] $failures.Add('the program was ended by force') }
# (Once it has ended, nothing holds the setup: it installs, and starts nothing, as nothing ran then.)
$how = Stop-Program
Remove-Item -LiteralPath $OverridesFile -Force -ErrorAction SilentlyContinue
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '19-after-it-ended')`"" 180
if ($finish.Exit -ne 0) { [void] $failures.Add("once the program had ended, the setup ended with $($finish.Exit)") }
$state = Test-Installed $newVersion $failures
Test-StartedAgain '19-after-it-ended' $failures $false
[void] (Stop-Program)
Test-Data $failures -ProgramStarted
Write-Step '19' 'busy-held' $began $failures ([ordered]@{ doubtOver = $over; run = $run; resultLines = $resultLines; ended = $how; finish = $finish })

# --- 20 the program not answering: nothing changed, and with /FORCECLOSE=1 ended by force and upgraded
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$hungId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$suspended = [QkvmVm]::NtSuspendProcess($started.Process.Handle) -eq 0
if (-not $suspended) { [void] $failures.Add('the program could not be suspended') }
try {
    $before = Get-Installed
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '20-hung')`"" 180
    if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
    if ($run.Seconds -lt 9 -or $run.Seconds -gt 60) { [void] $failures.Add("the setup gave up after $($run.Seconds) s, not after about 10 s") }
    Test-Unchanged $before $failures
    if (-not (Get-Process -Id $hungId -ErrorAction SilentlyContinue)) { [void] $failures.Add('the program was ended without /FORCECLOSE=1') }
    $resultLines = @(Get-ResultLines '20-hung')
    if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running and did not answer within 10 seconds; /FORCECLOSE=1 ends it by force') {
        [void] $failures.Add("the setup's log does not have the one result line of a program that does not answer ($($resultLines -join ' | '))")
    }
    $forced = Invoke-Setup $Setup "$SilentArguments /FORCECLOSE=1 /LOG=`"$(Get-Log '20-hung-forced')`"" 180
    if ($forced.Exit -ne 0) { [void] $failures.Add("with /FORCECLOSE=1 the setup ended with $($forced.Exit)") }
    if ((Read-Log '20-hung-forced') -notmatch 'qKVM Desktop is ended by force, as chosen\.') { [void] $failures.Add('the setup''s log does not say it ended the program by force') }
    if (Get-Process -Id $hungId -ErrorAction SilentlyContinue) { [void] $failures.Add('with /FORCECLOSE=1 the program is still there') }
    $state = Test-Installed $newVersion $failures
    Test-StartedAgain '20-hung-forced' $failures $true
} finally {
    Stop-ById $hungId
}
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '20' 'hung' $began $failures ([ordered]@{ run = $run; resultLines = $resultLines; forced = $forced; ended = $how })

# --- 21 a copy from another folder running beside the installed one: left alone
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$zip = Join-Path $UserFolder 'qkvm-vm-zip-copy'
Remove-Item -LiteralPath $zip -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $App -Destination $zip -Recurse
foreach ($own in 'qkvm-desktop.installed', 'unins000.exe', 'unins000.dat') { Remove-Item -LiteralPath (Join-Path $zip $own) -Force -ErrorAction SilentlyContinue }
$other = Start-Process -FilePath (Join-Path $zip 'qkvm-desktop.exe') -WorkingDirectory $zip -PassThru
if (-not (Wait-Until { Test-ProgramRunning } 30)) { [void] $failures.Add('the copy from another folder did not start') }
Start-Sleep -Seconds 3
if (@(Get-ProgramProcesses).Count) { [void] $failures.Add('the installed program runs beside the other copy') }
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '21-zip-copy-beside')`"" 180
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$said = Read-Log '21-zip-copy-beside'
if ($said -match 'runs from its folder|Asked qKVM Desktop to close|ended by force') { [void] $failures.Add('the setup took the copy from another folder for its own') }
if (-not (Get-Process -Id $other.Id -ErrorAction SilentlyContinue)) { [void] $failures.Add('the copy from another folder is no longer running') }
$state = Test-Installed $newVersion $failures
Start-Sleep -Seconds 3
if (@(Get-ProgramProcesses).Count) { [void] $failures.Add('the setup started the installed program') }
[void] $other.CloseMainWindow()
if (-not $other.WaitForExit(10000)) { Stop-ById $other.Id }
[void] (Wait-Until { -not (Test-ProgramRunning) } 10)
Remove-Item -LiteralPath $zip -Recurse -Force -ErrorAction SilentlyContinue
Test-Data $failures -ProgramStarted
Write-Step '21' 'zip-copy-beside' $began $failures ([ordered]@{ run = $run; otherStillRan = $true; files = $state.Files.Count })

# --- 22 interactive: the program not answering, End it now, the upgrade, and Start qKVM Desktop ticked
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$hungId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
if ([QkvmVm]::NtSuspendProcess($started.Process.Handle) -ne 0) { [void] $failures.Add('the program could not be suspended') }
$waitedForIt = $null
try {
    $hand = Invoke-SetupByHand (Get-Log '22-hung-end-it-now') @('Next', 'Install', 'Close qKVM and continue', 'End it now', 'Finish')
    if ($hand.Pressed -ne 5) { [void] $failures.Add("the setup was not seen through by hand: $($hand.Pressed) of 5 presses made (it ended with $($hand.Run.Exit))") }
    elseif ($hand.Run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($hand.Run.Exit)") }
    if ($hand.Shown.Count -ge 1 -and ($hand.Shown[0] -notmatch "qKVM Desktop $([regex]::Escape($newVersion)) is installed\. This installs it again and keeps your wall, settings and passwords\." -or $hand.Shown[0] -notmatch 'qKVM Desktop is running\. When you choose Install, Setup offers to close it for you\.')) {
        [void] $failures.Add('the first page does not say what is installed and that it runs')
    }
    if ($hand.Shown.Count -ge 4 -and $hand.Shown[3] -notmatch 'did not answer within 10 seconds') { [void] $failures.Add('the setup did not say that the program did not answer') }
    # (How long the setup waited, by the times on its own log: the request, then its withdrawal as it offers End it now;
    # never by when a look at its windows saw the box, review V1.)
    $waitedForIt = Get-LogSeconds '22-hung-end-it-now' 'Asked qKVM Desktop to close, the orderly way.' 'Withdrew the request to close.'
    if ($null -eq $waitedForIt) { [void] $failures.Add('the setup''s log has no request to close followed by its withdrawal (it never came to ask)') }
    elseif ($waitedForIt -lt 9 -or $waitedForIt -gt 13) { [void] $failures.Add("the setup gave up on its request $waitedForIt s after it asked, by its own log, not after about 10 s") }
    if ($hand.Shown.Count -ge 5 -and $hand.Shown[4] -notmatch 'Start qKVM Desktop') { [void] $failures.Add('the last page does not offer Start qKVM Desktop') }
    if ((Read-Log '22-hung-end-it-now') -notmatch 'qKVM Desktop is ended by force, as chosen\.') { [void] $failures.Add('the setup''s log does not say it ended the program by force') }
    if (Get-Process -Id $hungId -ErrorAction SilentlyContinue) { [void] $failures.Add('after End it now the program is still there') }
    $state = Test-Installed $newVersion $failures
    # (Finish with the tick as the page had it: ticked, as the program ran before, so it is started.)
    Test-StartedAgain '22-hung-end-it-now' $failures $true
} finally {
    Stop-ById $hungId
}
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '22' 'hung-end-it-now' $began $failures ([ordered]@{ run = $hand.Run; pressed = $hand.Pressed; secondsBetweenPresses = $hand.Gaps; secondsAskedToWithdrawn = $waitedForIt
    shown = $hand.Shown; lastSeen = $hand.LastSeen; ended = $how })

# --- 23 an older version running: one before 0.1.2 cannot be asked
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '23' $failures
$asksIt = [version] $oldVersion -ge [version] '0.1.2'
$started = Start-Program
$olderId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the older program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
$hand = $null
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '23-older-running')`"" 180
    $resultLines = @(Get-ResultLines '23-older-running')
    if ($asksIt) {
        # (An older version that listens is asked, and closes: the silent install upgrades it.)
        if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
        if ((Read-Log '23-older-running') -match 'ended by force') { [void] $failures.Add('the older program was ended by force') }
        Test-StartedAgain '23-older-running' $failures $true
    } else {
        if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
        Test-Unchanged $before $failures
        if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running, and this copy of it cannot be asked to close; /FORCECLOSE=1 ends it by force') {
            [void] $failures.Add("the setup's log does not have the one result line of a version that cannot be asked ($($resultLines -join ' | '))")
        }
        if (-not (Get-Process -Id $olderId -ErrorAction SilentlyContinue)) { [void] $failures.Add('the older program is no longer running') }
        $hand = Invoke-SetupByHand (Get-Log '23-older-by-hand') @('Next', 'Install', 'Close qKVM and continue', 'End it now', 'Finish')
        if ($hand.Pressed -ne 5) { [void] $failures.Add("the setup was not seen through by hand: $($hand.Pressed) of 5 presses made (it ended with $($hand.Run.Exit))") }
        elseif ($hand.Run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($hand.Run.Exit)") }
        if ($hand.Shown.Count -ge 1 -and $hand.Shown[0] -notmatch "qKVM Desktop $([regex]::Escape($oldVersion)) is installed\. This upgrades it to $([regex]::Escape($newVersion)) and keeps your wall, settings and passwords\.") {
            [void] $failures.Add('the first page does not name the installed version and the new one')
        }
        if ($hand.Shown.Count -ge 4 -and $hand.Shown[3] -notmatch 'cannot be asked to close by Setup: .*Quit it from its icon in the notification area') {
            [void] $failures.Add('the setup did not say how to quit a version that cannot be asked')
        }
        if (Get-Process -Id $olderId -ErrorAction SilentlyContinue) { [void] $failures.Add('after End it now the older program is still there') }
        Test-StartedAgain '23-older-by-hand' $failures $true
    }
    $state = Test-Installed $newVersion $failures
} finally {
    Stop-ById $olderId
}
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '23' 'older-running' $began $failures ([ordered]@{ olderAsked = $asksIt; run = $run; resultLines = $resultLines
    byHand = $(if ($hand) { [ordered]@{ run = $hand.Run; pressed = $hand.Pressed; shown = $hand.Shown; lastSeen = $hand.LastSeen } } else { $null }); ended = $how })

# --- 24 a request Setup gave up on: the program's interface stalled past Setup's wait, then answering again (review M1)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$AnswerWindow = 'qKVM Desktop quit answer'
$started = Start-Program
$stallId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
# Silent: the interface stalls (its thread alone suspended, the rest of the program running), the setup gives up and
# changes nothing; the interface answers again, and the program, hearing the request now, stays.
$script:stalled24 = [QkvmVm]::SuspendFirstThread($stallId)
if ($script:stalled24 -eq [IntPtr]::Zero) { [void] $failures.Add('the program''s interface could not be stalled') }
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '24-stalled-given-up')`"" 180
} finally {
    [QkvmVm]::ResumeAndLetGo($script:stalled24)
    $script:stalled24 = [IntPtr]::Zero
}
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
$resultLines = @(Get-ResultLines '24-stalled-given-up')
if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running and did not answer within 10 seconds; /FORCECLOSE=1 ends it by force') {
    [void] $failures.Add("the setup's log does not have the one result line of a program that does not answer ($($resultLines -join ' | '))")
}
if ((Read-Log '24-stalled-given-up') -notmatch 'Withdrew the request to close\.') { [void] $failures.Add('the setup''s log does not say it withdrew its request') }
Test-Unchanged $before $failures
Start-Sleep -Seconds 15
$silentAlive = [bool] (Get-Process -Id $stallId -ErrorAction SilentlyContinue)
if (-not $silentAlive) { [void] $failures.Add('once its interface answered again, the program closed for a setup that had given up') }
$silentAnswer = [QkvmVm]::TitleOf([uint32] $stallId, $AnswerWindow)
if ($silentAnswer -notmatch '^1\* .*too late') { [void] $failures.Add("the program did not say that the request came too late ($silentAnswer)") }
# By hand: Close qKVM and continue, the interface stalled; when the setup says it did not answer, Cancel (back to the
# Ready page: the setup still runs, and holds its mutex); the interface answers again; the program stays. Then the
# setup is left (Cancel, Yes).
$script:stalled24 = [QkvmVm]::SuspendFirstThread($stallId)
$script:alive24 = $false
$script:answer24 = ''
try {
    $hand = Invoke-SetupByHand (Get-Log '24-by-hand-cancel') @('Next', 'Install', 'Close qKVM and continue', @{ Press = 'Cancel'; Beside = 'End it now' }, {
            Start-Sleep -Seconds 2
            [QkvmVm]::ResumeAndLetGo($script:stalled24)
            $script:stalled24 = [IntPtr]::Zero
            Start-Sleep -Seconds 15
            $script:alive24 = [bool] (Get-Process -Id $stallId -ErrorAction SilentlyContinue)
            $script:answer24 = [QkvmVm]::TitleOf([uint32] $stallId, $AnswerWindow)
        }, 'Cancel', 'Yes')
} finally {
    [QkvmVm]::ResumeAndLetGo($script:stalled24)
    $script:stalled24 = [IntPtr]::Zero
}
if ($hand.Pressed -ne 7) { [void] $failures.Add("the setup was not seen through by hand: $($hand.Pressed) of 7 steps made (it ended with $($hand.Run.Exit))") }
if ($hand.Shown.Count -ge 4 -and $hand.Shown[3] -notmatch 'did not answer within 10 seconds') { [void] $failures.Add('the setup did not say that the program did not answer') }
if (-not $script:alive24) { [void] $failures.Add('after Cancel, once its interface answered again, the program closed while the setup waited on its Ready page') }
if ($script:answer24 -notmatch '^2\* .*too late') { [void] $failures.Add("after Cancel the program did not say that the request came too late ($($script:answer24))") }
if ((Read-Log '24-by-hand-cancel') -match 'ended by force') { [void] $failures.Add('the program was ended by force') }
Test-Unchanged $before $failures
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '24' 'stalled-given-up' $began $failures ([ordered]@{ run = $run; resultLines = $resultLines; silentAnswer = $silentAnswer
    byHand = [ordered]@{ run = $hand.Run; pressed = $hand.Pressed; shown = $hand.Shown; lastSeen = $hand.LastSeen; answer = $script:answer24 }; ended = $how })

# --- 25 a fresh install made outside update mode, the program running, the uninstaller: refused (review M2)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
# (Uninstalled first, so that the install below writes an uninstall log of its own, with no record of an earlier one:
# what the uninstaller checks is what this install kept there.)
$uninstalled = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '25-uninstall-first')`"" 180
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
if ($uninstalled.Exit -ne 0 -or (Test-Path -LiteralPath $App)) { [void] $failures.Add("the program could not be uninstalled first ($($uninstalled.Exit))") }
$fresh = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '25-fresh-install')`""
if ($fresh.Exit -ne 0) { [void] $failures.Add("the fresh install ended with $($fresh.Exit)") }
$state = Test-Installed $newVersion $failures
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
$run = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '25-uninstall-refused')`"" 120
if ($run.Exit -eq 0) { [void] $failures.Add('the uninstaller ended with 0 while the program ran') }
# (Refused by its own look at the program's mutex, review V2: Inno Setup keeps nothing of AppMutex for it here. And
# refused as it begins, in InitializeUninstall, where the uninstall is decided, never only at its look again before
# the removal, review F4.)
if ((Read-Log '25-uninstall-refused') -notmatch 'qKVM Desktop is running: it is not uninstalled\.') { [void] $failures.Add('the uninstaller''s log does not say it refused while the program ran') }
if ((Read-Log '25-uninstall-refused') -match 'started while it was being uninstalled') { [void] $failures.Add('the uninstaller refused only at its look again before the removal, not as it began') }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or -not $after.Entry -or -not $after.StartMenu) {
    [void] $failures.Add('the uninstaller removed something while the program ran')
}
if (-not (Get-Process -Id $started.Process.Id -ErrorAction SilentlyContinue)) { [void] $failures.Add('the program is no longer running') }
$how = Stop-Program
# (And while a setup is open, the program not running: Setup's mutex held here, under the setup's own name in this
# session, as an open setup holds it. The uninstaller finds it there already as it begins, and refuses, review F4.)
$created = $false
$open = [Threading.Mutex]::new($false, $SetupMutex, [ref] $created)
try {
    if (-not $created) { [void] $failures.Add("a setup's mutex was there already, held by another") }
    $beforeOpen = Get-Installed
    $whileOpen = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '25-uninstall-while-setup')`"" 120
    if ($whileOpen.Exit -eq 0) { [void] $failures.Add('the uninstaller ended with 0 while a setup was open') }
    if ((Read-Log '25-uninstall-while-setup') -notmatch 'A setup of qKVM Desktop, or another uninstall of it, is at work: it is not uninstalled\.') {
        [void] $failures.Add('the uninstaller''s log does not say it refused while a setup was open')
    }
    $afterOpen = Get-Installed
    if (($beforeOpen.Files | ConvertTo-Json -Depth 4) -ne ($afterOpen.Files | ConvertTo-Json -Depth 4) -or -not $afterOpen.Entry -or -not $afterOpen.StartMenu) {
        [void] $failures.Add('the uninstaller removed something while a setup was open')
    }
} finally {
    $open.Dispose()
}
Test-Data $failures -ProgramStarted
Write-Step '25' 'fresh-install-uninstall-refused' $began $failures ([ordered]@{ uninstalledFirst = $uninstalled; fresh = $fresh; run = $run; ended = $how; whileSetupOpen = $whileOpen })

# --- 26 a silent install that closed the program and then changes nothing starts it again (review L1)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$closedId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
# (A file of the package that the program does not load, held so that it cannot be replaced: the setup finds it only
# once it has closed the program, which holds the program's own files.)
$manifest = Get-Content -LiteralPath (Join-Path $App 'package-manifest.json') -Raw | ConvertFrom-Json
$heldName = @($manifest.files | ForEach-Object { [string] $_.path } | Where-Object { $_ -like 'licenses/*' } | Sort-Object)[0]
$held = [IO.File]::Open((Join-Path $App ($heldName -replace '/', '\')), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '26-closed-then-refused')`"" 180
} finally {
    $held.Dispose()
}
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
Test-Unchanged $before $failures
$said = Read-Log '26-closed-then-refused'
if ($said -notmatch 'qKVM Desktop closed after \d+ ms\.') { [void] $failures.Add('the setup did not close the program first') }
if (Get-Process -Id $closedId -ErrorAction SilentlyContinue) { [void] $failures.Add('the program that ran was not closed') }
$resultLines = @(Get-ResultLines '26-closed-then-refused')
if ($resultLines.Count -ne 1 -or -not $resultLines[0].StartsWith('qKVM-RESULT: 7 nothing was changed: a file of qKVM Desktop is in use or cannot be replaced (', [StringComparison]::Ordinal) -or
    -not $resultLines[0].EndsWith('; qKVM Desktop, which this setup had closed, is started again', [StringComparison]::Ordinal)) {
    [void] $failures.Add("the setup's log does not have the one result line of a refusal after it closed the program ($($resultLines -join ' | '))")
}
if (-not (Wait-Until { Test-ProgramRunning } 30)) { [void] $failures.Add('the program that the setup closed was not started again') }
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '26' 'closed-then-refused' $began $failures ([ordered]@{ run = $run; held = $heldName; resultLines = $resultLines; ended = $how })

# --- 27 a copy that does not hear the setup (the request's name taken first by another program): cannot be asked, never
# "did not answer"; a silent install changes nothing, and /FORCECLOSE=1 ends it only saying so plainly (review L2)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$squatter = New-Object System.Threading.EventWaitHandle($false, [System.Threading.EventResetMode]::AutoReset, 'Local\qKVM-Desktop-Quit')
try {
    $started = Start-Program
    $deafId = $started.Process.Id
    if (-not $started.Up) { [void] $failures.Add('the program did not start') }
    Start-Sleep -Seconds 5
    if ([QkvmVm]::TitleOf([uint32] $deafId, 'qKVM Desktop quit answer')) { [void] $failures.Add('a copy whose request was taken first has an answer window') }
    $before = Get-Installed
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '27-cannot-be-asked')`"" 180
    if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
    if ($run.Seconds -gt 8) { [void] $failures.Add("the setup waited $($run.Seconds) s for a copy that cannot be asked") }
    Test-Unchanged $before $failures
    $resultLines = @(Get-ResultLines '27-cannot-be-asked')
    if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running, and this copy of it cannot be asked to close; /FORCECLOSE=1 ends it by force') {
        [void] $failures.Add("the setup's log does not have the one result line of a copy that cannot be asked ($($resultLines -join ' | '))")
    }
    if ((Read-Log '27-cannot-be-asked') -match 'Asked qKVM Desktop to close') { [void] $failures.Add('the setup asked a copy that has no answer window') }
    $forced = Invoke-Setup $Setup "$SilentArguments /FORCECLOSE=1 /LOG=`"$(Get-Log '27-cannot-be-asked-forced')`"" 180
    if ($forced.Exit -ne 0) { [void] $failures.Add("with /FORCECLOSE=1 the setup ended with $($forced.Exit)") }
    $said = Read-Log '27-cannot-be-asked-forced'
    if ($said -notmatch 'could not be asked to close, so what it is doing is not known; it is ended by force all the same, as /FORCECLOSE=1 says\.') {
        [void] $failures.Add('the setup ended a copy it could not ask without saying so plainly')
    }
    if (Get-Process -Id $deafId -ErrorAction SilentlyContinue) { [void] $failures.Add('with /FORCECLOSE=1 the program is still there') }
    $state = Test-Installed $newVersion $failures
    Test-StartedAgain '27-cannot-be-asked-forced' $failures $true
    $how = Stop-Program
} finally {
    $squatter.Dispose()
}
Test-Data $failures -ProgramStarted
Write-Step '27' 'cannot-be-asked' $began $failures ([ordered]@{ run = $run; resultLines = $resultLines; forced = $forced; ended = $how })
# --- 28 the interface busy for 6 s while the setup still waits, on a silent install told /FORCECLOSE=1: the request
# still stands, and the program ends the orderly way, never by force (review N1)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$busyId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$script:stalled28 = [QkvmVm]::SuspendFirstThread($busyId)
if ($script:stalled28 -eq [IntPtr]::Zero) { [void] $failures.Add('the program''s interface could not be stalled') }
$script:resumedAt28 = $null
try {
    # (Its interface answers again 6 s into the setup: within the setup's 10 s.)
    $run = Invoke-Setup $Setup "$SilentArguments /FORCECLOSE=1 /LOG=`"$(Get-Log '28-busy-while-waiting')`"" 180 {
        param($process, $gone)
        if (-not $script:resumedAt28 -and $gone -ge 6) {
            [QkvmVm]::ResumeAndLetGo($script:stalled28)
            $script:stalled28 = [IntPtr]::Zero
            $script:resumedAt28 = $gone
        }
        return $false
    }
} finally {
    [QkvmVm]::ResumeAndLetGo($script:stalled28)
    $script:stalled28 = [IntPtr]::Zero
}
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$said = Read-Log '28-busy-while-waiting'
if ($said -match 'ended by force') { [void] $failures.Add('the program was ended by force, though it would have ended the orderly way') }
if ($said -match 'stays open|too late') { [void] $failures.Add('the setup took the request for refused') }
if ($said -notmatch 'qKVM Desktop closed after \d+ ms\.') { [void] $failures.Add('the setup''s log does not say the program closed') }
$state = Test-Installed $newVersion $failures
Test-StartedAgain '28-busy-while-waiting' $failures $true
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '28' 'busy-while-waiting' $began $failures ([ordered]@{ run = $run; resumedAfterSeconds = $script:resumedAt28; ended = $how })
$summary = [ordered]@{ machine = $Machine; windows = "$os"; setup = $newVersion; olderSetup = $oldVersion
    passed = (@($results | Where-Object { -not $_.passed }).Count -eq 0); steps = @($results) }
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out 'summary.json') -Encoding UTF8
if ($summary.passed) { Write-Output "All $($results.Count) steps passed: $Out" } else { Write-Output "Not all steps passed: $Out"; exit 1 }
