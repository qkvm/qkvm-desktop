#Requires -Version 5.1
<#
.SYNOPSIS
Tries the setup for real: ONLY inside a throwaway Windows 10 (22H2) or 11 virtual machine, as a standard user.

.DESCRIPTION
    powershell -ExecutionPolicy Bypass -File test_setup_vm.ps1 -Setup <new setup.exe> -OlderSetup <older setup.exe>
        -Out <results folder> -Disposable

NEVER on a PC anyone works on: it installs, upgrades, kills, updates and uninstalls qKVM Desktop for the user who
runs it, writes their registry and Start menu, and starts and ends the program. It refuses to begin unless it is
told -Disposable, finds the file qkvm-disposable-vm.txt in the user's own folder holding this machine's name (made
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
                           or gone or new: what Inno Setup's rollback restores of a killed setup), and whether the
                           earlier version still starts
  04 upgrade               the new setup over the earlier version: exit 0, its version, exactly its files; a file
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
                           the setup copies its files: the install completes, no start takes the program's mutex while
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

$AppId = '{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1'
$ProgramMutex = 'Local\QKVM Desktop'
# The setup's own, held while it runs (SetupMutex=qKVM-Desktop-Setup, in the session's namespace).
$SetupMutex = 'Local\qKVM-Desktop-Setup'
$App = Join-Path $env:LOCALAPPDATA 'Programs\qKVM Desktop'
$Program = Join-Path $App 'qkvm-desktop.exe'
$UserKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$AppId"
$MachineKeys = @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$AppId",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\$AppId")
$StartMenuEntry = Join-Path ([Environment]::GetFolderPath('Programs')) 'qKVM Desktop.lnk'
$DesktopEntry = Join-Path ([Environment]::GetFolderPath('Desktop')) 'qKVM Desktop.lnk'
$DataFolders = @((Join-Path $env:LOCALAPPDATA 'QKVM Desktop'), (Join-Path $env:LOCALAPPDATA 'qKVM'))
$Sentinel = 'qkvm-setup-vm-test.txt'
$SilentArguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'

# --- Never anywhere but on a machine made for it.
if (-not $Disposable) { throw 'This installs and uninstalls qKVM Desktop for real. Run it only in a throwaway virtual machine, with -Disposable.' }
$guard = Join-Path $env:USERPROFILE 'qkvm-disposable-vm.txt'
if (-not (Test-Path -LiteralPath $guard) -or (Get-Content -LiteralPath $guard -Raw).Trim() -ne $env:COMPUTERNAME) {
    throw "There is no $guard holding this machine's name ($env:COMPUTERNAME): this is not a machine made for this test."
}
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

function Stop-Tree([int] $Id) {
    foreach ($child in @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $Id")) { Stop-Tree $child.ProcessId }
    Stop-Process -Id $Id -Force -ErrorAction SilentlyContinue
}

function Invoke-Setup([string] $File, [string] $Arguments, [int] $Seconds = 300, [scriptblock] $During = $null) {
    <#
    Runs a setup (or the uninstaller) as this user and waits for it: its exit code (or 'killed' / 'timeout'), how long
    it took, and whether Windows' consent prompt for an administrator was seen while it ran. $During is called every
    quarter of a second with the process and the seconds gone; it returns $true to have the setup killed.
    #>
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $File
    $start.Arguments = $Arguments
    $start.UseShellExecute = $false
    $began = [DateTime]::UtcNow
    $process = [Diagnostics.Process]::Start($start)
    $consent = $false
    $ended = $null
    while (-not $process.HasExited) {
        if (Get-Process -Name consent -ErrorAction SilentlyContinue) { $consent = $true }
        $gone = ([DateTime]::UtcNow - $began).TotalSeconds
        if ($During -and (& $During $process $gone)) { Stop-Tree $process.Id; $ended = 'killed'; break }
        if ($gone -gt $Seconds) { Stop-Tree $process.Id; $ended = 'timeout'; break }
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

function Test-Data([System.Collections.ArrayList] $Failures, [switch] $ProgramStarted) {
    foreach ($folder in $DataFolders) {
        $file = Join-Path $folder $Sentinel
        if (-not (Test-Path -LiteralPath $file) -or (Get-Content -LiteralPath $file -Raw).Trim() -ne $folder) { [void] $Failures.Add("the program's data in $folder was touched") }
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
foreach ($folder in $DataFolders) {
    if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder | Out-Null }
    Set-Content -LiteralPath (Join-Path $folder $Sentinel) -Value $folder -Encoding ASCII
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
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('after the killed setup, the program does not start') }
Start-Sleep -Seconds 5
if ($started.Up -and -not (Test-ProgramRunning)) { [void] $failures.Add('after the killed setup, the program does not stay up') }
$how = Stop-Program
$entryVersion = if ($after.Entry) { $after.Entry.DisplayVersion } else { $null }
Write-Step '03' 'killed-upgrade' $began $failures ([ordered]@{ run = $run; entryVersion = $entryVersion; programVersion = Get-Version $Program
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
# (seen on Windows 10 and 11, 2026-11-12). Starts during a real copy: steps 07 and 14.
$tookEarly = $false
$endedAtOnce = $false
$created = $false
$held = [Threading.Mutex]::new($true, $SetupMutex, [ref] $created)
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
# second, as a person might double-click it then. Each must end at once and leave the setup to finish.)
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
if ($script:starts -eq 0) { [void] $failures.Add('the program was not once started while the setup copied its files') }
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

$summary = [ordered]@{ machine = $env:COMPUTERNAME; windows = "$os"; setup = $newVersion; olderSetup = $oldVersion
    passed = (@($results | Where-Object { -not $_.passed }).Count -eq 0); steps = @($results) }
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out 'summary.json') -Encoding UTF8
if ($summary.passed) { Write-Output "All $($results.Count) steps passed: $Out" } else { Write-Output "Not all steps passed: $Out"; exit 1 }
