#Requires -Version 5.1
<#
.SYNOPSIS
The VM step "upgrade keeps everything": ONLY inside a throwaway Windows 10 (22H2) or 11 virtual machine, as a
standard user, on a machine where qKVM Desktop was never installed (a fresh snapshot). It refuses before it does
anything unless it is told -Disposable, the machine's firmware names a hypervisor (QEMU, VMware, Hyper-V, VirtualBox:
HKLM\HARDWARE\DESCRIPTION\System\BIOS, which a standard user may read), and qkvm-disposable-vm.txt in the profile
Windows names holds the machine's name; its writer and its checker refuse the same way.

.DESCRIPTION
    powershell -ExecutionPolicy Bypass -File test_upgrade_keeps_vm.ps1 -Setup <new setup.exe> -OlderSetup <older
        setup.exe> -Fixture <ui\tests\fixtures\profiles\<older version>> -Writer <the older version's
        qkvm-test-write-profile.exe> -Checker <the new version's qkvm-test-check-profile.exe>
        -Out <empty results folder> -Disposable

The companion of test_setup_vm.ps1 (the same guards, the same results), for the owner's word of 2026-11-14: "the
upgrade should be seamless: everything stays the same, the settings, preferences, username, password". A published
version's own profile (made by that version's code: the fixtures' README) is put where the older version keeps it,
and the newer setup upgrades over it. Steps, one JSON file each in -Out, summary.json at the end:
  00 preflight        Windows, x64, a standard user, qKVM Desktop not installed and nothing of its data here (no
                      settings.json, no "QKVM Desktop: " entry in Windows Credential Manager); the older setup older
                      than the new one; the fixture whole and of the older setup's version
  01 older-install    the older setup, silent: exit 0, its version
  02 load-profile     the fixture written by qkvm-test-write-profile built at the OLDER version's own commit (a test
                      program, never in a package): its files into %LOCALAPPDATA%\QKVM Desktop, and each secret into
                      Credential Manager by that version's own Windows code, the bytes exactly as the fixture holds
                      them, each read back; cmdkey then lists exactly the names that version's code gave them
                      (credentials.json)
  03 upgrade          the new setup over it, silent: exit 0, the new version
  04 starts-at-its-views  the upgraded program starts at the profile's own views, with no box for the app password
                      and none for "Reset saved secrets" (each shows a window titled "qKVM" alone): a window for each
                      one the fixture's settings.json remembers, titled as the program titles it ("Rack A - This PC -
                      qKVM" on This PC's wall, "127.0.0.1:47391 - qKVM" on its stand-in server, which nothing answers;
                      Get-ProfileTitles), and none titled "qKVM" alone; it is left 35 s, for what it writes a while
                      after it starts, then ended by force with none of its windows closed first (a window a person
                      closes is one the program forgets; its files, each written whole, bear an end by force), and
                      seen to be gone
  05 profile-kept     qkvm-test-check-profile of the NEW version, file by file, its Windows code reading every credential,
                      byte for byte; settings.json says
                      all the old one said, the same (a window that Windows moved onto the screen is said, and is no
                      loss); the sign-ins' counts (no refusal forgotten), the alert and the setup override still there;
                      the settings as they were found kept as settings.json.before-<new version>.bak, byte for byte;
                      the new version recorded; no lock marker
  06 second-start     started again: the same views again, and no second copy of the settings (one at the version's
                      first start, and only then)
What the program does with the made-up KVMs of the fixture (192.0.2.0/24, 198.51.100.0/24, set aside for examples) and
its stand-in server (127.0.0.1:47391): it tries them, and nothing answers. It looks for an update (the release feed,
on the internet) only where the profile has it do so: the fixtures turn the daily look off, and their hand-over to
Setup is to the version now installed ("Updated to") or to an older one, which a newer install has gone past and the
program lets go of untold. A hand-over that did not install is told, and the update looked for anew: that moves
settings.json's last_update_check later, which step 05 lets be (the check: profile_loading.h). Nothing else is sent
anywhere but those addresses.

Before: the two tools, each built in release, as test programs (never in a package):
  - the older version's writer, qkvm-test-write-profile, built at that version's commit
    (ui\tests\fixtures\profiles\README.md says how; the builder of 0.1.2 handed both over with their SHA-256);
  - the new version's checker, from the same tree as the new setup (it names the new version):
        cmake --preset release ; cmake --build --preset release --target qkvm-test-check-profile
Copy both, and the older version's fixture folder (ui\tests\fixtures\profiles\0.1.1 with 0.1.1's setup, or 0.1.0 with
0.1.0's), into the VM. Each runs with the libraries of the program installed at its step: this copies every DLL of
the installed folder beside it (-Out\writer after step 01, -Out\checker after step 03). Run it from a fresh snapshot,
not after test_setup_vm.ps1 (that one leaves data behind).#>
param(
    [Parameter(Mandatory = $true)] [string] $Setup,
    [Parameter(Mandatory = $true)] [string] $OlderSetup,
    [Parameter(Mandatory = $true)] [string] $Fixture,
    [Parameter(Mandatory = $true)] [string] $Writer,
    [Parameter(Mandatory = $true)] [string] $Checker,
    [Parameter(Mandatory = $true)] [string] $Out,
    [switch] $Disposable
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$UserKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1'
$ProgramMutex = 'Local\QKVM Desktop'
# (Each as Windows says it, never as the environment does: whoever starts this sets the environment. The machine's
# name is the physical NetBIOS one, kind 4: [Environment]::MachineName and ComputerNameNetBIOS give
# _CLUSTER_NETWORK_NAME_ where that is set, as qkvm-test-write-profile's own guard knows; review 012 last, R2.)
Add-Type -Namespace QkvmVm -Name Names -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool GetComputerNameExW(int kind, System.Text.StringBuilder name, ref uint size);
'@
$NameOfWindows = New-Object System.Text.StringBuilder 64
$NameSize = [uint32] 64
if (-not [QkvmVm.Names]::GetComputerNameExW(4, $NameOfWindows, [ref] $NameSize)) { throw 'Windows did not say the name of this machine.' }
$Machine = $NameOfWindows.ToString()
$UserFolder = [Environment]::GetFolderPath('UserProfile')
$Local = [Environment]::GetFolderPath('LocalApplicationData')
$App = Join-Path $Local 'Programs\qKVM Desktop'
$Program = Join-Path $App 'qkvm-desktop.exe'
$Data = Join-Path $Local 'QKVM Desktop'
$SilentArguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'

if (-not $Disposable) { throw 'This installs qKVM Desktop and writes Windows Credential Manager for real. Run it only in a throwaway virtual machine, with -Disposable.' }
$guard = Join-Path $UserFolder 'qkvm-disposable-vm.txt'
if (-not (Test-Path -LiteralPath $guard) -or (Get-Content -LiteralPath $guard -Raw).Trim() -ne $Machine) {
    throw "There is no $guard holding this machine's name ($Machine): this is not a machine made for this test."
}
# The hypervisor a machine's firmware names (its maker and product, as Windows keeps them where a standard user may
# read them), if it names one: QEMU (as Proxmox's guests are), VMware, Hyper-V ("Microsoft Corporation" and "Virtual
# Machine"), VirtualBox ("innotek GmbH"); empty where it names none, as a PC's does.
function Get-Hypervisor([string] $Maker, [string] $Product) {
    if ($Maker -match 'QEMU' -or $Product -match 'QEMU') { return 'QEMU' }
    if ($Maker -like 'VMware*' -or $Product -like 'VMware*') { return 'VMware' }
    if ($Maker -eq 'Microsoft Corporation' -and $Product -eq 'Virtual Machine') { return 'Hyper-V' }
    if ($Maker -eq 'innotek GmbH' -or $Product -eq 'VirtualBox') { return 'VirtualBox' }
    return ''
}
# (And a virtual machine, as its firmware says: the marker alone proves none. outside review of 0.1.2, T5.)
$firmware = Get-ItemProperty -LiteralPath 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS' -ErrorAction SilentlyContinue
$firmwareSays = @(if ($firmware) { $firmware.PSObject.Properties | ForEach-Object Name })
$maker = if ($firmwareSays -contains 'SystemManufacturer') { ([string] $firmware.SystemManufacturer).Trim() } else { '' }
$product = if ($firmwareSays -contains 'SystemProductName') { ([string] $firmware.SystemProductName).Trim() } else { '' }
$hypervisor = Get-Hypervisor $maker $product
if (-not $hypervisor) {
    throw "This machine's firmware names no hypervisor (QEMU, VMware, Hyper-V, VirtualBox): it is no virtual machine, and this test runs only in one."
}
$Setup = (Resolve-Path -LiteralPath $Setup).Path
$OlderSetup = (Resolve-Path -LiteralPath $OlderSetup).Path
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path
$Writer = (Resolve-Path -LiteralPath $Writer).Path
$Checker = (Resolve-Path -LiteralPath $Checker).Path
if (-not (Test-Path -LiteralPath $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }
$Out = (Resolve-Path -LiteralPath $Out).Path
if (@(Get-ChildItem -LiteralPath $Out -Force).Count) { throw "$Out is not empty: each run writes its results into a new folder." }

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class QkvmVmWindows {
    delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int most);
    // The titles of the visible top-level windows of these processes.
    public static string[] Titles(uint[] processes) {
        var titles = new List<string>();
        var wanted = new HashSet<uint>(processes);
        EnumWindows((window, data) => {
            uint process;
            GetWindowThreadProcessId(window, out process);
            if (wanted.Contains(process) && IsWindowVisible(window)) {
                var text = new StringBuilder(512);
                GetWindowText(window, text, text.Capacity);
                if (text.Length > 0) titles.Add(text.ToString());
            }
            return true;
        }, IntPtr.Zero);
        return titles.ToArray();
    }
}
'@

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
function Test-ProgramRunning { $mutex = $null; if ([Threading.Mutex]::TryOpenExisting($ProgramMutex, [ref] $mutex)) { $mutex.Dispose(); return $true }; return $false }
function Wait-Until([scriptblock] $Condition, [int] $Seconds) {
    $until = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $until) { if (& $Condition) { return $true }; Start-Sleep -Milliseconds 250 }
    return [bool] (& $Condition)
}
# (What a function returns is unrolled where it is taken: none comes as nothing, one as itself, and under strict mode
# neither has a Count. So each is taken in @() wherever it is counted or indexed; tools/test_vm_scripts.ps1 drives these
# helpers in Windows PowerShell on none, one and several, and refuses a bare one.)
function Get-ProgramIds { return @(Get-CimInstance Win32_Process -Filter "Name = 'qkvm-desktop.exe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $Program } | ForEach-Object { [uint32] $_.ProcessId }) }
function Get-Titles { $ids = @(Get-ProgramIds); if (-not $ids.Count) { return @() }; return @([QkvmVmWindows]::Titles([uint32[]] $ids)) }
# Every copy of the program ended by force, with none of its windows closed first: a window closed is what a person
# does, and the program rightly forgets a window a person closed (the second VM run lost the profile's second window
# so, 2026-11-14). Its files, each written whole, bear an end by force. What was done, and how it went, for each.
function Stop-Program([int] $Seconds = 10) {
    $how = New-Object System.Collections.ArrayList
    foreach ($id in @(Get-ProgramIds)) {
        $process = Get-Process -Id $id -ErrorAction SilentlyContinue
        if (-not $process) { continue }
        Stop-Process -Id $id -Force
        if ($process.WaitForExit($Seconds * 1000)) { [void] $how.Add("ended by force, no window closed first ($id)") }
        else { [void] $how.Add("ended by force, and still running $Seconds s later ($id)") }
    }
    [void] (Wait-Until { -not (Test-ProgramRunning) } $Seconds)
    return @($how)
}
function Invoke-Setup([string] $File, [string] $Arguments, [int] $Seconds = 300) {
    $began = [DateTime]::UtcNow
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -PassThru
    if (-not $process.WaitForExit($Seconds * 1000)) { Stop-Process -Id $process.Id -Force; return [pscustomobject]@{ Exit = 'timeout'; Seconds = $Seconds } }
    return [pscustomobject]@{ Exit = $process.ExitCode; Seconds = [Math]::Round(([DateTime]::UtcNow - $began).TotalSeconds, 1) }
}
function Get-Version([string] $File) { if (-not (Test-Path -LiteralPath $File -PathType Leaf)) { return $null }; return ([string] [Diagnostics.FileVersionInfo]::GetVersionInfo($File).ProductVersion).Trim() }
function Get-ProgramCredentials {
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($line in @(& cmdkey.exe /list)) { if ([string] $line -match '(QKVM Desktop: .+?)\s*$') { $names.Add($Matches[1]) } }
    return @($names | Sort-Object -Unique)
}
# A tool beside the libraries of the program installed now, in a folder of its own under -Out; what it said.
function Invoke-Tool([string] $Tool, [string] $Folder, [string[]] $Arguments) {
    $at = Join-Path $Out $Folder
    New-Item -ItemType Directory -Path $at | Out-Null
    Copy-Item -LiteralPath $Tool -Destination $at
    foreach ($dll in Get-ChildItem -LiteralPath $App -Filter '*.dll' -File) { Copy-Item -LiteralPath $dll.FullName -Destination $at }
    # (What it writes to its error stream is taken as said: Windows PowerShell would stop this script for its first line
    # of it, under 'Stop', in place of the step's saying what went wrong.)
    $output = & { $ErrorActionPreference = 'Continue'; & (Join-Path $at (Split-Path -Leaf $Tool)) @Arguments 2>&1 }
    return [pscustomobject]@{ Exit = $LASTEXITCODE; Said = @($output | ForEach-Object { [string] $_ }) }
}
# The titles of the windows the program opens on a profile's own views: one for each window its settings.json
# remembers (at most eight; one where it remembers none), each as ui/qml/Main.qml makes it: the group the window shows
# where it is at a wall that has that group, then its source ("This PC", or the server's host and port), then " - qKVM".
# A window on This PC's wall is at its wall; one on a server is not, as nothing answers here, and shows no group. What
# is not one of these ways cannot be foretold: a word of why, in place of a title.
function Get-ProfileTitles([string] $Settings) {
    $said = Get-Content -LiteralPath $Settings -Raw | ConvertFrom-Json
    $has = @($said.PSObject.Properties | ForEach-Object Name)
    $kvms = @(if ($has -contains 'kvms') { $said.kvms })
    $groups = @($kvms | ForEach-Object { if (@($_.PSObject.Properties | ForEach-Object Name) -contains 'group') { [string] $_.group } } | Where-Object { $_ })
    $windows = @(if ($has -contains 'windows') { $said.windows })
    $server = if ($has -contains 'server') { [string] $said.server } else { '' }
    $ownWall = ($has -contains 'own_wall') -and [bool] $said.own_wall
    $titles = New-Object System.Collections.Generic.List[string]
    $count = [Math]::Max(1, [Math]::Min($windows.Count, 8))
    for ($at = 0; $at -lt $count; $at++) {
        $window = if ($at -lt $windows.Count) { $windows[$at] } else { [pscustomobject] @{ group = ''; source = '' } }
        $fields = @($window.PSObject.Properties | ForEach-Object Name)
        $group = if ($fields -contains 'group') { [string] $window.group } else { '' }
        $source = if ($fields -contains 'source') { [string] $window.source } else { '' }
        # (A window that remembers no source takes the profile's: This PC's wall, unless only a server is remembered.)
        if (-not $source) { $source = if ($ownWall -or -not $server) { 'pc' } else { $server } }
        if ($source -ceq 'pc') {
            $parts = @(@($(if ($groups -ccontains $group) { $group } else { '' }), 'This PC') | Where-Object { $_ })
        } elseif ($source -match '^[A-Za-z][A-Za-z0-9+.-]*://([^/?#@]+)') {
            $parts = @($Matches[1])
        } else {
            $titles.Add("(a window on ""$source"", whose title is not foretold)")
            continue
        }
        $titles.Add(($parts -join ' - ') + ' - qKVM')
    }
    return @($titles)
}
# The program at its profile's own views: a window titled as each is (Get-ProfileTitles), and none titled "qKVM" alone
# (a box for the app password, or "Reset saved secrets", shows only that). The titles it has.
function Test-ProfileViews([System.Collections.ArrayList] $Failures, [string[]] $Expected, [int] $Seconds = 60) {
    [void] (Wait-Until { $now = @(Get-Titles); @($Expected | Where-Object { $now -cnotcontains $_ }).Count -eq 0 } $Seconds)
    $titles = @(Get-Titles)
    $missing = @($Expected | Where-Object { $titles -cnotcontains $_ })
    if ($missing.Count) { [void] $Failures.Add("no window titled ""$($missing -join '" or "')"" within $Seconds s (titles: $($titles -join ' | '))") }
    if (@($titles | Where-Object { $_ -ceq 'qKVM' }).Count) { [void] $Failures.Add('a window is titled "qKVM" alone: a box for the app password or a reset') }
    return @($titles)
}

# --- 00 preflight
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$os = [Environment]::OSVersion.Version
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = (New-Object Security.Principal.WindowsPrincipal $identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$newVersion = Get-Version $Setup
$oldVersion = Get-Version $OlderSetup
$made = Get-Content -LiteralPath (Join-Path $Fixture 'made.json') -Raw | ConvertFrom-Json
# (The names the older version's own Windows code gave its records: credentials.json, made by that code.)
$shapes = Get-Content -LiteralPath (Join-Path $Fixture 'credentials.json') -Raw | ConvertFrom-Json
$fixtureNames = @($shapes.credentials.PSObject.Properties | ForEach-Object { [string] $_.Value.target } | Sort-Object -Unique)
# (The windows the new program opens on that profile, by their titles: the profile's own views, whatever they show.)
$viewTitles = @(if (Test-Path -LiteralPath (Join-Path $Fixture 'profile\settings.json')) { Get-ProfileTitles (Join-Path $Fixture 'profile\settings.json') })
if ($os.Major -ne 10 -or $os.Build -lt 19045) { [void] $failures.Add("Windows $os is older than Windows 10 22H2") }
if ($admin) { [void] $failures.Add('this runs as an administrator: run it as a standard user') }
if ((Test-Path -LiteralPath $UserKey) -or (Test-Path -LiteralPath $App)) { [void] $failures.Add('qKVM Desktop is installed already: start from a fresh snapshot') }
if (Test-Path -LiteralPath (Join-Path $Data 'settings.json')) { [void] $failures.Add("there is a profile in $Data already: start from a fresh snapshot") }
if (@(Get-ProgramCredentials).Count) { [void] $failures.Add('Windows Credential Manager holds entries of qKVM already: start from a fresh snapshot') }
if (-not ([version] $oldVersion -lt [version] $newVersion)) { [void] $failures.Add("the older setup ($oldVersion) is not older than the setup ($newVersion)") }
if ([string] $made.made_by.kind -ne 'person') { [void] $failures.Add('the fixture is not of a person at the PC (a wall screen''s is for test_ui_upgrade_continuity)') }
if ([string] $made.made_by.version -ne $oldVersion) { [void] $failures.Add("the fixture is of $($made.made_by.version), the older setup of $oldVersion") }
if (-not $fixtureNames.Count -or -not (Test-Path -LiteralPath (Join-Path $Fixture 'profile\settings.json'))) { [void] $failures.Add('the fixture is not whole') }
if (-not $viewTitles.Count -or @($viewTitles | Where-Object { $_ -notlike '* - qKVM' }).Count) { [void] $failures.Add("the titles of the fixture's windows cannot all be foretold: $($viewTitles -join ' | ')") }
Write-Step '00' 'preflight' $began $failures ([ordered]@{ windows = "$os"; user = $identity.Name; hypervisor = $hypervisor
    setup = [ordered]@{ file = Split-Path -Leaf $Setup; version = $newVersion; sha256 = Get-Sha256 $Setup }
    olderSetup = [ordered]@{ file = Split-Path -Leaf $OlderSetup; version = $oldVersion; sha256 = Get-Sha256 $OlderSetup }
    fixture = [ordered]@{ version = $made.made_by.version; commit = $made.made_by.commit; secrets = $fixtureNames.Count; windows = $viewTitles }
    writer = [ordered]@{ file = Split-Path -Leaf $Writer; sha256 = Get-Sha256 $Writer }
    checker = [ordered]@{ file = Split-Path -Leaf $Checker; sha256 = Get-Sha256 $Checker } })
if ($failures.Count) { throw 'The machine is not one this test can run on; see 00-preflight.json.' }

# --- 01 the older version installed
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$run = Invoke-Setup $OlderSetup "$SilentArguments /LOG=`"$(Join-Path $Out '01-older-install.log')`""
if ($run.Exit -ne 0) { [void] $failures.Add("the older setup ended with $($run.Exit)") }
if ((Get-Version $Program) -ne $oldVersion) { [void] $failures.Add("the program is version $(Get-Version $Program), not $oldVersion") }
Write-Step '01' 'older-install' $began $failures ([ordered]@{ run = $run; version = Get-Version $Program })
if ($failures.Count) { throw 'The older version is not installed; see 01-older-install.json.' }

# --- 02 the profile of the older version put in place
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$load = Invoke-Tool $Writer 'writer' @('--disposable', $Fixture)
if ($load.Exit -ne 0) { [void] $failures.Add("the writer ended with $($load.Exit): $($load.Said -join ' ')") }
$listed = @(Get-ProgramCredentials)
if (($listed -join "`n") -cne ($fixtureNames -join "`n")) { [void] $failures.Add("Credential Manager lists $($listed -join ', '), not the fixture's $($fixtureNames -join ', ')") }
$put = @(Get-ChildItem -LiteralPath $Data -File | ForEach-Object { [ordered]@{ name = $_.Name; sha256 = Get-Sha256 $_.FullName } })
Write-Step '02' 'load-profile' $began $failures ([ordered]@{ writer = $load; credentials = $listed; files = $put })
if ($failures.Count) { throw 'The profile was not put in place; see 02-load-profile.json.' }

# --- 03 the upgrade
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Join-Path $Out '03-upgrade.log')`""
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
if ((Get-Version $Program) -ne $newVersion) { [void] $failures.Add("the program is version $(Get-Version $Program), not $newVersion") }
Write-Step '03' 'upgrade' $began $failures ([ordered]@{ run = $run; version = Get-Version $Program })

# --- 04 the upgraded program starts at its profile's own views
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$process = Start-Process -FilePath $Program -WorkingDirectory $App -PassThru
if (-not (Wait-Until { Test-ProgramRunning } 30)) { [void] $failures.Add('the program did not start (its mutex did not appear within 30 s)') }
$titles = @(Test-ProfileViews $failures $viewTitles)
Start-Sleep -Seconds 35
$titlesLater = @(Get-Titles)
if (@($titlesLater | Where-Object { $_ -ceq 'qKVM' }).Count) { [void] $failures.Add('35 s later, a window is titled "qKVM" alone') }
$how = @(Stop-Program)
if (Test-ProgramRunning) { [void] $failures.Add("the program still runs after it was ended: $($how -join '; ')") }
Write-Step '04' 'starts-at-its-views' $began $failures ([ordered]@{ expected = $viewTitles; titles = $titles; titlesLater = $titlesLater; ended = $how })

# --- 05 the profile, file by file
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$report = Join-Path $Out '05-report.json'
$check = Invoke-Tool $Checker 'checker' @('--disposable', $Fixture, $report)
if ($check.Exit -ne 0) { [void] $failures.Add("the check ended with $($check.Exit): $($check.Said -join ' ')") }
$after = @(Get-ChildItem -LiteralPath $Data -File | ForEach-Object { [ordered]@{ name = $_.Name; sha256 = Get-Sha256 $_.FullName } })
Write-Step '05' 'profile-kept' $began $failures ([ordered]@{ check = $check; files = $after; credentials = @(Get-ProgramCredentials) })

# --- 06 a second start: the same views again, and no second copy of the settings
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$copiesBefore = @(Get-ChildItem -LiteralPath $Data -File -Filter 'settings.json.*.bak' | ForEach-Object Name)
$process = Start-Process -FilePath $Program -WorkingDirectory $App -PassThru
if (-not (Wait-Until { Test-ProgramRunning } 30)) { [void] $failures.Add('the program did not start again') }
$titles = @(Test-ProfileViews $failures $viewTitles)
$how = @(Stop-Program)
if (Test-ProgramRunning) { [void] $failures.Add("the program still runs after it was ended: $($how -join '; ')") }
$copiesAfter = @(Get-ChildItem -LiteralPath $Data -File -Filter 'settings.json.*.bak' | ForEach-Object Name)
if (($copiesAfter -join ',') -ne ($copiesBefore -join ',')) { [void] $failures.Add("the second start kept another copy: $($copiesAfter -join ', ')") }
if ($copiesAfter.Count -ne 1) { [void] $failures.Add("there are $($copiesAfter.Count) copies of the settings, not one") }
Write-Step '06' 'second-start' $began $failures ([ordered]@{ titles = $titles; copies = $copiesAfter; ended = $how })

$summary = [ordered]@{ machine = $Machine; windows = "$os"; setup = $newVersion; olderSetup = $oldVersion
    fixture = [string] $made.made_by.version; passed = (@($results | Where-Object { -not $_.passed }).Count -eq 0); steps = @($results) }
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out 'summary.json') -Encoding UTF8
if ($summary.passed) { Write-Output "All $($results.Count) steps passed: $Out" } else { Write-Output "Not all steps passed: $Out"; exit 1 }
