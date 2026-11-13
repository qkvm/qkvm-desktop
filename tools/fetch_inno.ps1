#Requires -Version 7.2
<#
.SYNOPSIS
Fetches Inno Setup, which makes the program's setup, and unpacks it without installing it.

.DESCRIPTION
    tools\fetch_inno.ps1                                        into .deps\inno (the gate's)
    tools\fetch_inno.ps1 -Into <folder> -Downloads <folder>     (tools/build_release.ps1, in its own folder)

Inno Setup 6.7.4, the last release of Inno Setup 6 (the owner, 2026-11-08: a tool of the build; nothing of it
is installed on the PC that builds). Its own installer is taken from Inno Setup's official release on GitHub
(the download its site points to), checked against the size and SHA-256 written below (those of the
release's immutable asset) and against its Authenticode signature by its publisher, Pyrsys B.V.

That installer is then asked to unpack itself in its portable mode (/PORTABLE=1: no uninstaller, no registry
entry, no Start menu entry, no administrator) into a folder of its own. So that it cannot do otherwise, it is
started from a folder marked low integrity, and so runs at low integrity: Windows lets such a program write
into no folder but those marked for it and into no part of the registry but HKCU\Software\AppDataLow. The
uninstall entries of the user and of the PC (as reg query lists them), and the keys Inno Setup would use,
are read before and after, and must be the same: else this stops, and says so. The unpacked files are then
copied into <Into>\6.7.4 (where they have the ordinary integrity of the folder again), and must be byte for
byte the tree written below (its digest, as build_common.ps1's Get-TreeDigest makes it). The low folder is
removed. What was found is kept beside, in <Into>\6.7.4-unpacked.json.

A folder that is already the pinned tree is used as it is: nothing is fetched and nothing is run.
#>
param(
    [string] $Into = '',
    [string] $Downloads = ''
)

. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\build_common.ps1"

# (The version and the unpacked tree are build_common.ps1's: tools/build_setup.ps1 holds the compiler to them too.)
$Version = $InnoVersion
$Name = "innosetup-$Version.exe"
$Url = "https://github.com/jrsoftware/issrc/releases/download/is-6_7_4/$Name"
$Bytes = 10604576
$Sha256 = 'a549929319f9120716677c685535ca4671e4b937b5fdc87e2585f32d8eb30e8c'
# Who signs Inno Setup's installers (its site's verification page; before 6.4.2 it was another name).
$Publisher = 'CN=Pyrsys B.V., O=Pyrsys B.V., S=Noord-Holland, C=NL'
$TreeDigest = $InnoTreeDigest
$TreeFiles = $InnoTreeFiles

if (-not $Into) { $Into = Join-Path $DesktopRoot '.deps\inno' }
$Into = [IO.Path]::GetFullPath($Into)
$Root = Join-Path $Into $Version
$Record = Join-Path $Into "$Version-unpacked.json"

function Test-PinnedTree([string] $Folder) {
    if (-not $TreeDigest -or -not (Test-Path -LiteralPath (Join-Path $Folder 'ISCC.exe') -PathType Leaf)) { return $false }
    return (Get-TreeDigest $Folder) -ceq $TreeDigest -and @(Get-ChildItem -LiteralPath $Folder -Recurse -File -Force).Count -eq $TreeFiles
}

if (Test-PinnedTree $Root) {
    Write-Host "Inno Setup $Version is in $Root, as pinned."
    return
}

# What Inno Setup, installed, would leave in the registry: the uninstall entries of the user and of the PC (both
# views), its file kind and its own settings. Read as reg query says them.
$Watched = @(
    'HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKCU\Software\Classes\.iss',
    'HKCU\Software\Classes\InnoSetupScriptFile',
    'HKCU\Software\Jordan Russell'
)
function Get-RegistryNow {
    $said = [ordered]@{}
    foreach ($key in $Watched) {
        $lines = & "$env:SystemRoot\System32\reg.exe" query $key /s 2>&1 | ForEach-Object { "$_" }
        $said[$key] = ($lines -join "`n")
    }
    # (And the user's Start menu, where its shortcuts would be.)
    $menu = [Environment]::GetFolderPath('Programs')
    $said['Start menu'] = (@(Get-ChildItem -LiteralPath $menu -Recurse -Force -ErrorAction SilentlyContinue |
        ForEach-Object { [IO.Path]::GetRelativePath($menu, $_.FullName) } | Sort-Object) -join "`n")
    return $said
}

New-Item -ItemType Directory -Force -Path $Into | Out-Null
$keep = [bool] $Downloads
if (-not $keep) { $Downloads = Join-Path $Into '.download' }
New-Item -ItemType Directory -Force -Path $Downloads | Out-Null
$installer = Join-Path $Downloads $Name
Write-Host "Inno Setup $Version into $Root"
Get-Checked $Url $installer $Bytes $Sha256
$signature = Get-AuthenticodeSignature -LiteralPath $installer
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -cne $Publisher) {
    throw "$Name is not signed by Inno Setup's publisher ($($signature.Status), $($signature.SignerCertificate.Subject))."
}

# The low folder: whoever runs this is given full control of it (Windows asks for that before it lets a folder's
# integrity be set), and it is marked low, for what is made in it too.
$staging = Join-Path $Into ".unpack-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $staging | Out-Null
$me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$icacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
& $icacls $staging /grant "*${me}:(OI)(CI)F" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "The unpacking folder could not be given to $me." }
& $icacls $staging /setintegritylevel '(OI)(CI)low' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'The unpacking folder could not be marked low integrity.' }
try {
    foreach ($part in 'in', 'temp', 'out') { New-Item -ItemType Directory -Path (Join-Path $staging $part) | Out-Null }
    $copy = Join-Path $staging "in\$Name"
    Copy-Item -LiteralPath $installer -Destination $copy
    if (-not (Test-IsWhatWasRecorded $copy $Bytes $Sha256)) { throw 'The copy of the installer is not the one checked.' }
    $label = (& $icacls $copy) -join "`n"
    if ($label -notmatch 'Mandatory Label\\Low Mandatory Level') { throw "The installer's copy is not marked low integrity: $label" }

    $before = Get-RegistryNow
    $log = Join-Path $staging 'unpack.log'
    $start = [Diagnostics.ProcessStartInfo]::new($copy)
    foreach ($argument in '/PORTABLE=1', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCANCEL', '/CURRENTUSER',
        '/NOICONS', '/NOCLOSEAPPLICATIONS', '/NORESTARTAPPLICATIONS', "/DIR=$(Join-Path $staging 'out')", "/LOG=$log") {
        $start.ArgumentList.Add($argument)
    }
    $start.UseShellExecute = $false
    $start.WorkingDirectory = Join-Path $staging 'temp'
    $start.Environment['TEMP'] = Join-Path $staging 'temp'
    $start.Environment['TMP'] = Join-Path $staging 'temp'
    $process = [Diagnostics.Process]::Start($start)
    if (-not $process.WaitForExit(300000)) {
        $process.Kill($true)
        throw "Inno Setup's installer did not end within 5 minutes, and was ended."
    }
    $ended = $process.ExitCode
    $after = Get-RegistryNow
    $changed = @($Watched + 'Start menu' | Where-Object { $before[$_] -cne $after[$_] })
    $unpacked = [ordered]@{
        version = $Version
        installer = [ordered]@{ name = $Name; bytes = $Bytes; sha256 = $Sha256; signer = $signature.SignerCertificate.Subject }
        unpackedAt = [DateTime]::UtcNow.ToString('o')
        how = 'its own installer, /PORTABLE=1 /VERYSILENT /CURRENTUSER, run at low integrity from a low folder'
        exitCode = $ended
        # (Whether each was the same, and nothing of what it holds: what is installed on the PC that builds is its own.)
        registry = @($Watched + 'Start menu' | ForEach-Object { [ordered]@{ key = $_; same = $before[$_] -ceq $after[$_] } })
    }
    if ($changed.Count) {
        $unpacked | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Record -Encoding utf8
        throw "Unpacking Inno Setup changed what it must not: $($changed -join ', '). Nothing more was done; see $Record."
    }
    if ($ended -ne 0) {
        $tail = if (Test-Path -LiteralPath $log) { (Get-Content -LiteralPath $log -Tail 15) -join "`n" } else { '(no log)' }
        throw "Inno Setup's installer ended with $ended in portable mode. The end of its log:`n$tail"
    }
    $made = Join-Path $staging 'out'
    if (-not (Test-Path -LiteralPath (Join-Path $made 'ISCC.exe') -PathType Leaf)) { throw 'The portable unpacking made no ISCC.exe.' }

    # Out of the low folder: a copy takes the ordinary integrity of where it is put.
    if (Test-Path -LiteralPath $Root) { Remove-Item -LiteralPath $Root -Recurse -Force }
    Copy-Item -LiteralPath $made -Destination $Root -Recurse
    $low = @(Get-ChildItem -LiteralPath $Root -Recurse -Force | Where-Object { ((& $icacls $_.FullName) -join "`n") -match 'Low Mandatory Level' })
    if ($low.Count) { throw "Copied out, $($low.Count) file(s) are still marked low integrity." }
    $digest = Get-TreeDigest $Root
    $count = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force).Count
    $unpacked['tree'] = [ordered]@{ digest = $digest; files = $count }
    $unpacked | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Record -Encoding utf8
    if (-not $TreeDigest) {
        throw "No tree is pinned yet. This one: digest $digest, $count files ($Root). Pin both, after a look at them."
    }
    if (-not (Test-PinnedTree $Root)) {
        Remove-Item -LiteralPath $Root -Recurse -Force
        throw "The unpacked tree is not the pinned one (digest $digest, $count files): it was removed."
    }
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}
if (-not $keep) { Remove-Item -LiteralPath $Downloads -Recurse -Force }
Write-Host "Inno Setup $Version is in $Root, unpacked in its portable mode; the registry and the Start menu were left as they were."
