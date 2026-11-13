#Requires -Version 7.2
<#
.SYNOPSIS
Builds qKVM Desktop as it is handed out: from the exported public tree, in a folder that says nothing of the PC.

.DESCRIPTION
    pwsh -File tools\build_release.ps1 -Export <the export's folder> -Root <build folder>
    pwsh -File tools\build_release.ps1 -Export <folder> -Root <folder> -Trial
    pwsh -File tools\build_release.ps1 -Export <folder> -Root <folder> -CheckOnly   (says what it would build)

The export is the git repository tools/export_public.py --part desktop made: its last commit is the public
commit. Only that commit's tree is built, taken out by git archive into <Root>\src; nothing of this checkout,
of its folder or of its history goes in. <Root> is a folder of its own (new, or made by this script before)
whose name says nothing private, on a disk with room (about 6 GB): the release plan names it. The libraries
built there keep their source paths from below it, and nothing that ships may name it (checked here).

- Qt and FFmpeg are fetched by the export's own fetch scripts, each archive checked against the size and
  SHA-256 written in them. The archives are kept in <Root>\downloads, and a later run takes them from there
  after checking them again; they are unpacked anew each time.
- The WebRTC stack is fetched by its pinned commits and hash and built in <Root>\src, as the export says.
- The program is configured with the publish preset of the export's CMakePresets.json: the public update
  feed and the release key's public half. Until the owner has put that key there this refuses. -Trial
  builds with a key made here and dropped at once (nobody can sign for it): such a build is never handed
  out, and its record says so.
- Release builds link with /PDBALTPATH:%_PDB% (CMakeLists.txt): this checks that the program names its
  symbols by their file name alone.
- tools/package.ps1 makes the folder, the ZIP, its audit and the private symbols. Everything lands in
  <Root>\runs\<time>, with the build record (build-record.json): the commit, the tools' versions and the
  hashes of what went in and what came out. Symbols stay in that run's symbols folder and are never a
  release file.
- The setup is made from that run's own package folder, once its checks below have passed: Inno Setup is
  unpacked anew in <Root>\inno by tools/fetch_inno.ps1 (in its portable mode, at low integrity, the registry
  read before and after; never installed), tools/build_setup.ps1 makes the setup and its uncompressed twin
  for the byte scan in the run's setup folder, and tools/test_setup.ps1 holds it to its static test. Neither
  may name <Root>. The setup is never run.
- The folder, the ZIP and the setup are then audited as a beta (audit_release.ps1 -Beta -SetupDirectory) and
  looked through byte for byte, the setup's twin too, by the private repository's tools/scan_private.py
  (found beside the desktop folder there, or named with -Scanner; run from anywhere else, the record says it
  was not run). The record says what both found, and this ends with a failure when either refuses:
  tools/release_files.ps1 takes only a run whose record says both passed.

Nothing is installed, signed, uploaded or published. The children run with a cleaned environment: no
Qt, QML, QKVM or compiler setting of this shell reaches them, and their PATH is Windows', PowerShell's and
git's alone until Visual Studio's own setup adds its tools.
#>
param(
    [Parameter(Mandatory)] [string] $Export,
    [Parameter(Mandatory)] [string] $Root,
    [switch] $Trial,
    [switch] $CheckOnly,
    # The private repository's byte scan (tools/scan_private.py beside this desktop/ folder in the monorepo).
    [string] $Scanner = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. "$PSScriptRoot\build_common.ps1"

function Get-Git {
    $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $git) { throw 'git is not on the PATH.' }
    return $git.Source
}

function Invoke-Git {
    param([string] $Repository, [string[]] $Arguments)
    $said = & $script:git -C $Repository @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed in ${Repository}: $said" }
    return $said
}

$git = Get-Git
$exportPath = (Resolve-Path -LiteralPath $Export).Path.TrimEnd('\')

# The export: a repository of its own, nothing changed in it, with the desktop's tree at its top.
$top = (Invoke-Git $exportPath @('rev-parse', '--show-toplevel')) -replace '/', '\'
if (-not [string]::Equals($top.TrimEnd('\'), $exportPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "$exportPath is not the top of a git repository of its own (the export's folder is)."
}
$changed = @(Invoke-Git $exportPath @('status', '--porcelain', '--untracked-files=all'))
if ($changed.Count) { throw "The export has changes that are not committed: only a commit is built." }
$commit = [string] (Invoke-Git $exportPath @('rev-parse', 'HEAD'))
$tree = [string] (Invoke-Git $exportPath @('rev-parse', 'HEAD^{tree}'))
$committedBy = [string] (Invoke-Git $exportPath @('log', '-1', '--format=%an <%ae> %cI'))
$files = @(Invoke-Git $exportPath @('ls-tree', '-r', '--name-only', 'HEAD'))
foreach ($needed in 'CMakeLists.txt', 'CMakePresets.json', 'tools/build_release.ps1', 'tools/package.ps1') {
    if ($files -cnotcontains $needed) { throw "The export has no $needed at its top: it is not the desktop's tree." }
}
foreach ($file in $files) {
    if ($file -match '^(\.deps|build)/' -or $file -ceq 'local.cmake') {
        throw "The export holds $file, which is this PC's and never committed."
    }
}

# Its version, and the update channel its publish preset carries.
$tempTree = Join-Path ([IO.Path]::GetTempPath()) "qkvm-release-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $tempTree | Out-Null
try {
    foreach ($name in 'CMakeLists.txt', 'CMakePresets.json') {
        $text = Invoke-Git $exportPath @('show', "HEAD:$name")
        Set-Content -LiteralPath (Join-Path $tempTree $name) -Value $text -Encoding utf8
    }
    $version = Get-ProjectVersion $tempTree
    $preset = Get-PublishPreset $tempTree
} finally {
    Remove-Item -LiteralPath $tempTree -Recurse -Force
}
if ($preset.Feed -cne $PublicUpdateFeed) { throw "The publish preset's feed is not $PublicUpdateFeed." }
if ($Trial) {
    $publicKey = New-ThrowawayPublicKey
} else {
    if (-not $preset.PublicKey) {
        throw 'The publish preset holds no public key yet. The owner makes the release key with tools/release_key.ps1, puts the 128 hexadecimal digits it prints into CMakePresets.json (publish, QKVM_UPDATE_PUBLIC_KEY), commits and exports again.'
    }
    if (-not (Test-ReleasePublicKey $preset.PublicKey)) {
        throw 'The publish preset''s public key is not 128 hexadecimal digits of a point on the P-256 curve.'
    }
    $publicKey = $preset.PublicKey.ToLowerInvariant()
}

# The neutral folder: made here, or one this script made before (it says so in a file of its own).
$Root =[IO.Path]::GetFullPath($Root).TrimEnd('\')
$marker = Join-Path $Root '.qkvm-release-root'
if ((Test-Path -LiteralPath $Root) -and -not (Test-Path -LiteralPath $marker) -and @(Get-ChildItem -LiteralPath $Root -Force).Count) {
    throw "$Root holds something this script did not make. Name an empty or new folder."
}
if ($Root.StartsWith($exportPath + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $exportPath.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) -or $Root -ieq $exportPath) {
    throw 'The build folder and the export must be apart.'
}
if ($CheckOnly) {
    Write-Output "Would build qKVM Desktop $version from $commit in $Root$(if ($Trial) { ' (TRIAL)' })."
    return
}
New-Item -ItemType Directory -Force -Path $Root | Out-Null
Set-Content -LiteralPath $marker -Value 'Made by tools/build_release.ps1. Its src and qt folders are made anew at every run.' -Encoding utf8

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$run = Join-Path $Root "runs\$stamp"
if (Test-Path -LiteralPath $run) { throw "$run is there already." }
$logs = (New-Item -ItemType Directory -Path "$run\logs").FullName
$src = Join-Path $Root 'src'
$qt = Join-Path $Root 'qt'
$downloads = Join-Path $Root 'downloads'
$temp = Join-Path $Root 'temp'
$inno = Join-Path $Root 'inno'
foreach ($old in $src, $qt, $temp, $inno) {
    if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force }
}
New-Item -ItemType Directory -Path $src, $temp | Out-Null
New-Item -ItemType Directory -Force -Path "$downloads\qt", "$downloads\ffmpeg", "$downloads\inno" | Out-Null
if ($Trial) {
    Set-Content -LiteralPath "$run\TRIAL-NOT-FOR-RELEASE.txt" -Encoding utf8 -Value @(
        'This run was built with -Trial: its program pins a key that was made and dropped during the build.'
        'Nothing of it is ever handed out; tools/release_files.ps1 refuses it.'
    )
}

# The tree of the public commit, and nothing else.
$archive = Join-Path $Root 'src.tar'
# (As committed, byte for byte: a git that turns line ends to Windows' on the way out, as this PC's is set to,
# would hand WSL a shell script it cannot read, and the tree's digest would be of other bytes than the commit's.)
Invoke-Git $exportPath @('-c', 'core.autocrlf=false', '-c', 'core.eol=lf', 'archive', '--format=tar', "--output=$archive", 'HEAD') | Out-Null
& "$env:SystemRoot\System32\tar.exe" -xf $archive -C $src
if ($LASTEXITCODE -ne 0) { throw 'The export''s tree could not be unpacked.' }
Remove-Item -LiteralPath $archive
$sourceDigest = Get-TreeDigest $src
Write-Output "Building qKVM Desktop $version from $commit in $src$(if ($Trial) { ' (TRIAL)' })"

# What every child is started with: Windows', PowerShell's and git's folders, and temporary files in the build
# folder (they are never in this shell's own Temp, on a disk that may be full).
$keep = @('SystemRoot', 'windir', 'SystemDrive', 'ComSpec', 'PATHEXT', 'OS', 'NUMBER_OF_PROCESSORS',
    'PROCESSOR_ARCHITECTURE', 'PROCESSOR_IDENTIFIER', 'PROCESSOR_LEVEL', 'PROCESSOR_REVISION', 'ProgramFiles',
    'ProgramFiles(x86)', 'ProgramW6432', 'ProgramData', 'CommonProgramFiles', 'CommonProgramFiles(x86)',
    'CommonProgramW6432', 'ALLUSERSPROFILE', 'APPDATA', 'LOCALAPPDATA', 'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH',
    'USERNAME', 'USERDOMAIN', 'COMPUTERNAME', 'PUBLIC', 'DriverData')
$pwsh = (Get-Process -Id $PID).Path
$cleanPath = @("$env:SystemRoot\System32", $env:SystemRoot, "$env:SystemRoot\System32\Wbem",
    "$env:SystemRoot\System32\WindowsPowerShell\v1.0", (Split-Path -Parent $pwsh), (Split-Path -Parent $git)) -join ';'

function Invoke-Clean {
    <# Runs pwsh with $Arguments in the cleaned environment, its words into $Log as well, and stops on a failure. #>
    param([string] $Step, [string[]] $Arguments)
    $saved = [Environment]::GetEnvironmentVariables()
    try {
        foreach ($name in @($saved.Keys)) {
            if ($keep -notcontains $name) { [Environment]::SetEnvironmentVariable($name, $null) }
        }
        $env:PATH = $cleanPath
        $env:TEMP = $temp
        $env:TMP = $temp
        $log = Join-Path $logs "$Step.txt"
        & $pwsh -NoProfile -NonInteractive @Arguments *>&1 | Tee-Object -FilePath $log
        $ended = $LASTEXITCODE
    } finally {
        foreach ($name in @([Environment]::GetEnvironmentVariables().Keys)) {
            if (-not $saved.Contains($name)) { [Environment]::SetEnvironmentVariable($name, $null) }
        }
        foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    }
    if ($ended -ne 0) { throw "The step $Step failed (exit $ended); its words are in $log." }
}

Invoke-Clean '1-qt' @('-File', "$src\tools\fetch_qt.ps1", '-Into', $qt, '-Downloads', "$downloads\qt")
Invoke-Clean '2-ffmpeg' @('-File', "$src\tools\build_ffmpeg.ps1", '-Downloads', "$downloads\ffmpeg", '-Work', (Join-Path $Root 'ffmpeg-work'))
Invoke-Clean '3-webrtc' @('-File', "$src\tools\fetch_webrtc.ps1")

# Configured with the publish preset (the key on the command line only for a trial), built, and the tools
# asked what they are.
$keyArgument = if ($Trial) { "'-DQKVM_UPDATE_PUBLIC_KEY=$publicKey'" } else { '' }
$versions = Join-Path $run 'tools.json'
$build = @"
`$ErrorActionPreference = 'Stop'
. '$src\tools\common.ps1'
Enter-BuildEnvironment
Set-Location '$src'
cmake --preset publish $keyArgument
if (`$LASTEXITCODE -ne 0) { throw 'The publish configuration failed.' }
cmake --build --preset publish --target qkvm-desktop qkvm-setup
if (`$LASTEXITCODE -ne 0) { throw 'The publish build failed.' }
function First([scriptblock] `$what, [string] `$like = '.') {
    (& `$what 2>&1 | ForEach-Object { "`$_".Trim() } | Where-Object { `$_ -match `$like } | Select-Object -First 1)
}
`$vswhere = Join-Path `${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
[ordered]@{
    visualStudio = First { & `$vswhere -path `$env:QKVM_BUILD_ENVIRONMENT -property installationVersion }
    msvcTools = `$env:VCToolsVersion
    compiler = First { cl } 'Version'
    linker = First { link } 'Version'
    windowsSdk = `$env:WindowsSDKVersion.TrimEnd('\')
    vcRedist = Split-Path -Leaf `$env:VCToolsRedistDir.TrimEnd('\')
    cmake = First { cmake --version }
    ninja = First { ninja --version }
    git = First { git --version }
    powershell = `$PSVersionTable.PSVersion.ToString()
} | ConvertTo-Json | Set-Content -LiteralPath '$versions' -Encoding utf8
"@
Invoke-Clean '4-build' @('-Command', $build)
Invoke-Clean '5-package' @('-File', "$src\tools\package.ps1", '-Preset', 'publish', '-SkipBuild')

# What was made, moved out of src (which the next run makes anew) into this run's folder.
$made = Get-Content -LiteralPath "$src\build\packages\latest.json" -Raw | ConvertFrom-Json
$package = Join-Path $run (Split-Path -Leaf $made.folder)
Move-Item -LiteralPath $made.folder -Destination $package
$zip = Join-Path $run (Split-Path -Leaf $made.archive)
Move-Item -LiteralPath $made.archive -Destination $zip
Move-Item -LiteralPath "$($made.archive).sha256" -Destination "$zip.sha256"
Move-Item -LiteralPath $made.audit -Destination (Join-Path $run 'audit.json')
Move-Item -LiteralPath $made.symbols -Destination (Join-Path $run 'symbols')

# The program names its symbols by file name alone, and is the one this tree's version says.
$program = Join-Path $package 'qkvm-desktop.exe'
if (-not (Test-BareDebugPath $program)) {
    throw "qkvm-desktop.exe names its symbols with a folder: $(@(Get-PeDebugPaths $program) -join ', ')"
}
if ((Get-Item -LiteralPath $program).VersionInfo.ProductVersion -cne $version) { throw 'The program is not the version the tree says.' }
# ...and carries the feed and the key it was configured with.
$cache = Get-Content -LiteralPath "$src\build\publish\CMakeCache.txt"
$builtKey = @($cache | Where-Object { $_.StartsWith('QKVM_UPDATE_PUBLIC_KEY:STRING=') } | ForEach-Object { ($_ -split '=', 2)[1] })
if ($builtKey.Count -ne 1 -or $builtKey[0] -ne $publicKey) { throw 'The build was not configured with the public key it was given.' }
$programBytes = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($program))
foreach ($carried in $PublicUpdateFeed, $builtKey[0]) {
    if (-not $programBytes.Contains("$carried`0")) { throw "The program does not carry $carried." }
}
# Nothing that ships names the folder it was built in (the libraries built here keep their source paths from below
# it: experiments/webrtc/build-cpp-dependencies.ps1).
foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File) {
    if (Test-NamesFolder $file.FullName $Root) {
        throw "$([IO.Path]::GetRelativePath($package, $file.FullName)) names the folder it was built in."
    }
}
$images = @(Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object Extension -in '.exe', '.dll' |
    Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/')
            sha256 = Get-Sha256 $_.FullName
            symbols = @(Get-PeDebugPaths $_.FullName)
        }
    })

# The setup, from this run's own package as it was checked above: Inno Setup unpacked here, never installed; the
# setup and its uncompressed twin for the byte scan; its static test. Neither names the build folder (inside the
# setup only what is not compressed can be looked at; in the twin all of it).
$setupWork = Join-Path $run 'setup'
Invoke-Clean '6-inno' @('-File', "$src\tools\fetch_inno.ps1", '-Into', $inno, '-Downloads', "$downloads\inno")
Invoke-Clean '7-setup' @('-File', "$src\tools\build_setup.ps1", '-PackageDirectory', $package, '-Inno', (Join-Path $inno $InnoVersion), '-OutputDirectory', $setupWork, '-ScanTwin')
Invoke-Clean '8-setup-test' @('-File', "$src\tools\test_setup.ps1", '-SetupDirectory', $setupWork, '-PackageDirectory', $package, '-Inno', (Join-Path $inno $InnoVersion))
$setupName = Get-SetupName $version
$setupFile = Join-Path $setupWork $setupName
$twinFile = Join-Path $setupWork "scan\$setupName"
foreach ($file in $setupFile, $twinFile) {
    if (Test-NamesFolder $file $Root) { throw "$([IO.Path]::GetRelativePath($run, $file)) names the folder it was built in." }
}

# The beta's audit, the setup with it, and the byte scan for what must not get out.
$betaRefused = @()
try {
    & "$src\tools\audit_release.ps1" -PackageDirectory $package -OutputFile (Join-Path $run 'audit-beta.json') -Beta -SetupDirectory $setupWork *>&1 |
        Tee-Object -FilePath (Join-Path $logs '9-audit-beta.txt')
} catch {
    "$_" | Tee-Object -FilePath (Join-Path $logs '9-audit-beta.txt') -Append
}
$betaAudit = Get-Content -LiteralPath (Join-Path $run 'audit-beta.json') -Raw | ConvertFrom-Json
$betaRefused = @($betaAudit.gates | Where-Object { $_.open -and -not $_.beta } | ForEach-Object { $_.id })
if (-not $Scanner) { $Scanner = Join-Path $PSScriptRoot '..\..\tools\scan_private.py' }
$scan = 'not run'
if (Test-Path -LiteralPath $Scanner -PathType Leaf) {
    $python = (Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    # Inno Setup's own programs, as fetch_inno.ps1 checked them (its installer's SHA-256 and its publisher's signature):
    # the setup's twin holds most of the two it is made of as they are, and a short word turns up in their bytes by
    # chance. Only such a word, standing in their own bytes, is theirs (tools/scan_private.py --public). The tree they
    # are in is checked once more first: still the one pinned (build_common.ps1), nothing of this run written into it.
    $innoTree = Join-Path $inno $InnoVersion
    if ((Get-TreeDigest $innoTree) -cne $InnoTreeDigest -or @(Get-ChildItem -LiteralPath $innoTree -Recurse -File -Force).Count -ne $InnoTreeFiles) {
        throw "Inno Setup in $innoTree is no longer the tree that was pinned: its programs are not taken as theirs."
    }
    $theirs = @('--public', (Join-Path $innoTree 'Setup.e32'), '--public', (Join-Path $innoTree 'SetupLdr.e32'))
    & $python -I (Resolve-Path -LiteralPath $Scanner).Path @theirs $package $zip $setupFile $twinFile *>&1 | Tee-Object -FilePath (Join-Path $logs '10-scan.txt')
    $scan = switch ($LASTEXITCODE) { 0 { 'clean' } 1 { 'findings' } default { 'failed' } }
} else {
    Write-Output 'The private byte scan is not beside this script, and was not run (-Scanner names it).'
}

# The record: enough to say later, of any file, whether it is the one built here.
$webrtc = Join-Path $src '.deps\webrtc-trial'
$record = [ordered]@{
    schema = 1
    product = 'qKVM Desktop'
    version = $version
    trial = [bool] $Trial
    builtAt = [DateTime]::UtcNow.ToString('o')
    source = [ordered]@{
        commit = $commit
        tree = $tree
        committedBy = $committedBy
        digest = $sourceDigest
    }
    updates = [ordered]@{ feed = $PublicUpdateFeed; publicKey = $publicKey; keyFromPreset = -not $Trial }
    tools = Get-Content -LiteralPath $versions -Raw | ConvertFrom-Json
    dependencies = [ordered]@{
        qt = @(Get-ChildItem -LiteralPath "$downloads\qt" -File | Sort-Object Name | ForEach-Object {
            [ordered]@{ archive = $_.Name; bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } })
        # (What tools/build_ffmpeg.ps1 says it built from and made: the sources, the toolchain, the configuration.)
        ffmpeg = Get-Content -LiteralPath "$src\.deps\ffmpeg\build.json" -Raw | ConvertFrom-Json
        webrtc = [ordered]@{
            libdatachannel = [string] (Invoke-Git "$webrtc\libdatachannel" @('rev-parse', 'HEAD'))
            submodules = @(Invoke-Git "$webrtc\libdatachannel" @('submodule', 'status') | ForEach-Object { "$_".Trim() })
            mbedtls = Get-Sha256 "$webrtc\mbedtls-3.6.7.tar.bz2"
        }
        # (What tools/fetch_inno.ps1 says of its unpacking: the installer, how it was run, the registry before and after.)
        inno = Get-Content -LiteralPath (Join-Path $inno "$InnoVersion-unpacked.json") -Raw | ConvertFrom-Json
    }
    package = [ordered]@{
        archive = Split-Path -Leaf $zip
        bytes = (Get-Item -LiteralPath $zip).Length
        sha256 = Get-Sha256 $zip
        folder = Split-Path -Leaf $package
        manifestSha256 = Get-Sha256 (Join-Path $package 'package-manifest.json')
        files = @(Get-ChildItem -LiteralPath $package -Recurse -File).Count
        images = $images
    }
    setup = [ordered]@{ name = $setupName; file = "setup/$setupName"; bytes = (Get-Item -LiteralPath $setupFile).Length; sha256 = Get-Sha256 $setupFile
        scanTwin = [ordered]@{ file = "setup/scan/$setupName"; bytes = (Get-Item -LiteralPath $twinFile).Length; sha256 = Get-Sha256 $twinFile }
        made = Get-Content -LiteralPath (Join-Path $setupWork 'setup-record.json') -Raw | ConvertFrom-Json }
    symbols = Get-Content -LiteralPath (Join-Path $run 'symbols\manifest.json') -Raw | ConvertFrom-Json
    checks = [ordered]@{
        betaAudit = $(if ($betaRefused.Count) { 'refused' } else { 'accepted' })
        betaRefused = $betaRefused
        betaOpen = @($betaAudit.gates | Where-Object open | ForEach-Object { [ordered]@{ id = $_.id; accepted = $_.beta } })
        privateScan = $scan
        # (Invoke-Clean stops the run where the setup's static test fails: a run that got here passed it.)
        setupTest = 'passed'
    }
}
$record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'build-record.json') -Encoding utf8
Write-Output "Built: $zip" "SHA-256: $($record.package.sha256)" "Setup: $setupFile" "SHA-256: $($record.setup.sha256)" "Record: $run\build-record.json"
if ($Trial) { Write-Output 'TRIAL: built with a throwaway key; never hand any of it out.' }
$against = @()
if ($betaRefused.Count) { $against += "the beta audit refused it ($($betaRefused -join ', '))" }
if ($scan -ne 'clean') { $against += "the private byte scan: $scan" }
if ($against.Count) { throw "Built, and NOT FOR RELEASE: $($against -join '; '). See $logs." }
