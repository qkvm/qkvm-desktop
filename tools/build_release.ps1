#Requires -Version 7.2
<#
.SYNOPSIS
Builds qKVM Desktop as it is handed out, for x64 and for Windows on ARM: from the exported public tree, in a folder that
says nothing of the PC, in two parts on two PCs.

.DESCRIPTION
    On a PC with WSL (the FFmpeg part):
    pwsh -File tools\build_release.ps1 -Part ffmpeg -Export <the export's folder> -Root <build folder> [-Records <folder>]
    On the PC that builds the release (the build part: Visual Studio's x64 and ARM64 C++ tools, no WSL):
    pwsh -File tools\build_release.ps1 -Part build -Export <folder> -Root <folder> -FFmpeg <the FFmpeg part's folder>
    pwsh -File tools\build_release.ps1 -Part build -Export <folder> -Root <folder> -FFmpeg <folder> -Trial
    Either with -CheckOnly says what it would build, once everything it is given has been checked.

The release is built in two parts because what FFmpeg's build runs (its pinned compilers, in WSL) and what the program's
build runs for both kinds of Windows (Visual Studio's x64 and ARM64 compilers) are not on one PC. Both parts start from
the same export at the same commit; the FFmpeg part's folder is copied from the one PC to the other, and the build part
takes it only as its record names it. The owner signs the release on the PC with WSL, as before (sign_release.ps1).

The export is the git repository tools/export_public.py --part desktop made: its last commit is the public
commit. Only that commit's tree is built, taken out by git archive into <Root>\src; nothing of this checkout,
of its folder or of its history goes in. <Root> is a folder of its own (new, or made by this script before)
whose name says nothing private and holds no comma, on a disk with room (about 6 GB for the FFmpeg part, 12 GB for the
build part): the release plan names it. The libraries built there keep their source paths from below it, and nothing
that ships may name it (checked here).

The FFmpeg part (-Part ffmpeg) builds FFmpeg for x64 and for ARM64 with tools/build_ffmpeg.ps1, both anew from the
sources and toolchains pinned there (the archives are kept in <Root>\downloads\ffmpeg and checked against their pins
again at every run; nothing built before is taken, and the work folders are made anew), without the import libraries
(lib stamps those with the time: the build part makes them). It writes <Root>\ffmpeg-runs\<time>\part: each kind's
FFmpeg, FFmpeg's and zlib's source archives, and its record (ffmpeg-record.json): the commit and tree, the pins of each
kind's sources and toolchain and of the two scripts that build it, FFmpeg's configuration, and every file, each DLL
among them, by name, size and SHA-256. -Records is a folder kept from release to release: every record in it of a build
of the same pins (by their digest, build_common.ps1) must have made the same files byte for byte, or this stops and the
release stops with it (the new build is kept beside the part, not as one, and names what differs); a build that matches
says which records it reproduces. Its record is then kept there too. -Records is, unless told another,
qkvm-desktop-records\ffmpeg beside <Root> (build_common.ps1's Get-FFmpegRecordsFolder): in the folder of the owner's
release roots, which are kept there and never cleaned, and each release's record is kept in the private repository too, at
desktop-notes/releases/<version>/ffmpeg-record.json, committed with the release, so that it outlives any disk (the
coordinator, 2026-11-22). Nothing of it is signed or handed out. A release
runs this part twice, each anew, with the same -Records, and takes the second run's part: the second must reproduce the
first byte for byte (for 0.2.0, whose pins no earlier record holds, the first run has nothing to compare with).
-Records is never cleared: tools/release_commands.ps1 holds what ships to the records kept there.

The build part (-Part build) refuses an FFmpeg part whose record is of another commit or tree, or of no build of one
kind, or that holds any file the record does not name, or one of another size or SHA-256, or lacks one, or that
reproduced no earlier build of its pins for each kind (unless -Trial); and after it is copied into the tree, its copy is
checked against the record once more. Then:
- Qt for both kinds is fetched by the export's own fetch script, each archive checked against the size and SHA-256
  written there. The archives are kept in <Root>\downloads, and a later run takes them from there after checking them
  again; they are unpacked anew each time.
- FFmpeg's import libraries are made from the part's export lists (build_ffmpeg.ps1 -ImportLibrariesOnly).
- The WebRTC stack is fetched by its pinned commits and hash and built for both kinds in <Root>\src, as the export says.
- The program is configured with the publish preset of the export's CMakePresets.json (x64) and its arm64-publish
  preset (ARM64, which takes the same feed and key and nothing of its own): the public update feed and the release
  key's public half. Until the owner has put that key there this refuses. -Trial builds with a key made here and dropped
  at once (nobody can sign for it): such a build is never handed out, and its record says so. The ARM64 build is a
  cross-build: the tool it runs while it builds is the x64 build's (QKVM_HOST_TOOLS).
- Release builds link with /PDBALTPATH:%_PDB% (CMakeLists.txt): this checks that every program names its symbols by
  their file name alone.
- tools/package.ps1 makes each kind's folder, ZIP, audit and private symbols. Everything lands in <Root>\runs\<time>,
  with the build record (build-record.json): the commit, the tools' versions, the FFmpeg part's record and the hashes
  of what went in and what came out. Symbols stay in that run's symbols folder and are never a release file. The ARM64
  package's startup checks cannot run on an x64 PC: tools/test_package_archive.ps1 -Evidence runs them on its ZIP on
  Windows on Arm, and tools/release_files.ps1 (-Arm64Startup) and release_commands.ps1 refuse the run until that record
  says they passed there, for that very ZIP (review armp-1 M3).
- The setup is made from that run's own two package folders, once their checks below have passed: Inno Setup is
  unpacked anew in <Root>\inno by tools/fetch_inno.ps1 (in its portable mode, at low integrity, the registry read
  before and after; never installed), tools/build_setup.ps1 makes the setup, which carries both and installs the PC's
  own, and its uncompressed twin for the byte scan in the run's setup folder, and tools/test_setup.ps1 holds it to its
  static test. Neither may name <Root>. The setup is never run.
- Each kind's folder and ZIP, and the setup, are then audited as a beta (audit_release.ps1 -Beta -SetupDirectory, with
  the FFmpeg part's record: every FFmpeg DLL packaged is one it names) and looked through byte for byte, the setup's
  twin too, by the private repository's tools/scan_private.py (found beside the desktop folder there, or named with
  -Scanner; run from anywhere else, the record says it was not run). The record says what both found, and this ends
  with a failure when either refuses: tools/release_files.ps1 takes only a run whose record says both passed.

Nothing is installed, signed, uploaded or published. The children run with a cleaned environment: no
Qt, QML, QKVM or compiler setting of this shell reaches them, and their PATH is Windows', PowerShell's and
git's alone until Visual Studio's own setup adds its tools.
#>
param(
    [Parameter(Mandatory)] [ValidateSet('ffmpeg', 'build')] [string] $Part,
    [Parameter(Mandatory)] [string] $Export,
    [Parameter(Mandatory)] [string] $Root,
    # The FFmpeg part's: the folder its records are kept in from release to release (beside -Root unless told).
    [string] $Records = '',
    # The build part's: the FFmpeg part's folder (<Root of the FFmpeg part>\ffmpeg-runs\<time>\part, copied here).
    [string] $FFmpeg = '',
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

if ($Part -ceq 'ffmpeg') {
    if ($Trial) { throw '-Trial is the build part''s: the FFmpeg part is the same for a trial and for a release.' }
    if ($FFmpeg) { throw '-FFmpeg names the FFmpeg part for the build part: the FFmpeg part makes it.' }
    # (It keeps its record for the next release's check, and checks it against the earlier ones, beside the build
    # folder, in the folder of the owner's release roots, unless told another.)
    if (-not $Records) { $Records = Get-FFmpegRecordsFolder $Root }
} else {
    if ($PSBoundParameters.ContainsKey('Records')) { throw '-Records is the FFmpeg part''s.' }
    if (-not $FFmpeg) { throw 'The build part takes FFmpeg from the FFmpeg part (build_release.ps1 -Part ffmpeg on a PC with WSL): name its folder with -FFmpeg.' }
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

# Its version, and the update channel its publish presets carry (the build part's: FFmpeg's builds carry no channel).
$tempTree = Join-Path ([IO.Path]::GetTempPath()) "qkvm-release-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $tempTree | Out-Null
try {
    foreach ($name in 'CMakeLists.txt', 'CMakePresets.json') {
        $text = Invoke-Git $exportPath @('show', "HEAD:$name")
        Set-Content -LiteralPath (Join-Path $tempTree $name) -Value $text -Encoding utf8
    }
    $version = Get-ProjectVersion $tempTree
    $preset = if ($Part -ceq 'build') { Get-PublishPreset $tempTree } else { $null }
} finally {
    Remove-Item -LiteralPath $tempTree -Recurse -Force
}
$publicKey = ''
if ($Part -ceq 'build') {
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
}

# The neutral folder: made here, or one this script made before (it says so in a file of its own).
$Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
$marker = Join-Path $Root '.qkvm-release-root'
if ((Test-Path -LiteralPath $Root) -and -not (Test-Path -LiteralPath $marker) -and @(Get-ChildItem -LiteralPath $Root -Force).Count) {
    throw "$Root holds something this script did not make. Name an empty or new folder."
}
if ($Root.StartsWith($exportPath + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $exportPath.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) -or $Root -ieq $exportPath) {
    throw 'The build folder and the export must be apart.'
}
# (Its folders are handed to the setup's tools as one word, a comma between them.)
if ($Root.Contains(',')) { throw "The build folder's path holds a comma: name another ($Root)." }

# The FFmpeg part's earlier records, every one of them read now: a folder of them holds nothing else.
$earlier = @()
if ($Part -ceq 'ffmpeg') {
    $Records = [IO.Path]::GetFullPath($Records).TrimEnd('\')
    if (Test-Path -LiteralPath $Records) {
        foreach ($item in Get-ChildItem -LiteralPath $Records -Force) {
            $was = $null
            if ($item.Extension -ieq '.json' -and -not $item.PSIsContainer) {
                try { $was = Get-Content -LiteralPath $item.FullName -Raw | ConvertFrom-Json } catch { $was = $null }
            }
            if ($null -eq $was -or $was.PSObject.Properties['part'] -eq $null -or $was.part -cne 'ffmpeg' -or -not $was.PSObject.Properties['builds']) {
                throw "$($item.FullName) is not the record of an FFmpeg part: the folder of records holds them alone. Move it out of that folder, or name another with -Records."
            }
            $earlier += [pscustomobject]@{ Name = $item.Name; Record = $was }
        }
    }
}
# The FFmpeg part the build part is given: of this commit and tree, and exactly what its record names.
$ffmpegPart = $null
if ($Part -ceq 'build') {
    $FFmpeg = (Resolve-Path -LiteralPath $FFmpeg).Path.TrimEnd('\')
    foreach ($anew in 'src', 'qt', 'temp', 'inno') {
        $folder = Join-Path $Root $anew
        if ($FFmpeg -ieq $folder -or $FFmpeg.StartsWith($folder + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "The FFmpeg part is in $folder, which every run makes anew: copy it elsewhere."
        }
    }
    $ffmpegPart = Get-FFmpegPartRefusals -Folder $FFmpeg -Commit $commit -Tree $tree
    if ($ffmpegPart.Refusals.Count) { throw "The FFmpeg part in $FFmpeg is not taken: $($ffmpegPart.Refusals -join '; '). Copy the part again, as it is, from the FFmpeg part's run of the same export (<its Root>\ffmpeg-runs\<time>\part, of its second run), and name that copy with -FFmpeg." }
    # (The first build of its pins, of either kind, is not a release's: the release builds FFmpeg twice and takes the
    # part of the second run, which reproduced the first byte for byte (review armp-1 M1). This is the part's own word,
    # an early stop; what binds the FFmpeg that ships is the owner's PC's check against its kept records,
    # release_commands.ps1 -Records. A trial, never handed out, takes any part of its commit.)
    $partReproduces = @(if ($ffmpegPart.Record.PSObject.Properties['reproduces']) { $ffmpegPart.Record.reproduces })
    $unproved = @($FFmpegKinds | Where-Object { $kind = $_; -not @($partReproduces | Where-Object { "$_" -clike "* ($kind)" }).Count })
    if ($unproved.Count -and -not $Trial) {
        throw "The FFmpeg part in $FFmpeg reproduces no earlier build of its pins for $($unproved -join ', '): the release builds FFmpeg twice (build_release.ps1 -Part ffmpeg run twice with the same -Records) and takes the part of the second run, which reproduced the first byte for byte."
    }
}
if ($CheckOnly) {
    if ($Part -ceq 'ffmpeg') {
        Write-Output "Would build FFmpeg for x64 and arm64 of qKVM Desktop $version from $commit in $Root, against $($earlier.Count) earlier record(s) in $Records."
    } else {
        Write-Output "Would build qKVM Desktop $version for x64 and arm64 from $commit in $Root, with the FFmpeg part in $FFmpeg$(if ($Trial) { ' (TRIAL)' })."
    }
    return
}
New-Item -ItemType Directory -Force -Path $Root | Out-Null
Set-Content -LiteralPath $marker -Value 'Made by tools/build_release.ps1. Its src and qt folders are made anew at every run.' -Encoding utf8

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$run = if ($Part -ceq 'ffmpeg') { Join-Path $Root "ffmpeg-runs\$stamp" } else { Join-Path $Root "runs\$stamp" }
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
        'This run was built with -Trial: its programs pin a key that was made and dropped during the build.'
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

# Where each kind's FFmpeg is in the tree (common.ps1's Get-DepsRoot, for a tree whose local.cmake names no other place).
$ffmpegIn = [ordered]@{ x64 = Join-Path $src '.deps\ffmpeg'; arm64 = Join-Path $src '.deps\arm64\ffmpeg' }

if ($Part -ceq 'ffmpeg') {
    Write-Output "Building FFmpeg for x64 and arm64 of qKVM Desktop $version from $commit in $src"
    $partFolder = Join-Path $run 'part'
    $step = 0
    foreach ($kind in $FFmpegKinds) {
        # (Anew: nothing of an earlier build is taken, its work folder neither.)
        $work = Join-Path $Root "ffmpeg-work-$kind"
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
        ++$step
        Invoke-Clean "$step-ffmpeg-$kind" @('-File', "$src\tools\build_ffmpeg.ps1", '-Arch', $kind, '-Downloads', "$downloads\ffmpeg", '-Work', $work, '-NoImportLibraries')
        Copy-Item -LiteralPath $ffmpegIn[$kind] -Destination (Join-Path $partFolder $kind) -Recurse
    }
    # FFmpeg's and zlib's archives, the same for both kinds: the source offered with the release (release_files.ps1).
    $made = [ordered]@{}
    foreach ($kind in $FFmpegKinds) { $made[$kind] = Get-Content -LiteralPath (Join-Path $partFolder "$kind\build.json") -Raw | ConvertFrom-Json }
    New-Item -ItemType Directory -Path (Join-Path $partFolder 'sources') | Out-Null
    foreach ($source in 'ffmpeg', 'zlib') {
        $one = $made['x64'].$source
        if ("$($made['arm64'].$source.archive) $($made['arm64'].$source.sha256)" -cne "$($one.archive) $($one.sha256)") {
            throw "The two kinds were built from other $source sources."
        }
        $file = Join-Path $downloads "ffmpeg\$($one.archive)"
        if ((Get-Sha256 $file) -cne $one.sha256) { throw "$($one.archive) is not the archive the build names." }
        Copy-Item -LiteralPath $file -Destination (Join-Path $partFolder 'sources')
    }
    foreach ($file in Get-ChildItem -LiteralPath $partFolder -Recurse -File) {
        if (Test-NamesFolder $file.FullName $Root) { throw "$([IO.Path]::GetRelativePath($partFolder, $file.FullName)) names the folder it was built in." }
    }
    $scripts = @('build_ffmpeg.ps1', 'build_ffmpeg.sh' | ForEach-Object { "$_ $(Get-Sha256 (Join-Path $src "tools\$_"))" })
    # (The Ubuntu of WSL that built it, as it says itself: one that cannot say so ends the part, which would not say
    # what built it; outside review 2, 5.1.)
    $release = @(& wsl.exe -d Ubuntu -e cat /etc/os-release 2>$null)
    if ($LASTEXITCODE -ne 0) { throw "WSL's Ubuntu, which built FFmpeg, did not say what it is (wsl.exe exit $LASTEXITCODE): the part's record would not say what built it. Find why wsl.exe -d Ubuntu -e cat /etc/os-release fails, and build the part again." }
    $system = @($release | Where-Object { $_ -match '^PRETTY_NAME=' } | ForEach-Object { ($_ -split '=', 2)[1].Trim('"') })
    $record = New-FFmpegRecord -Part $partFolder -Version $version -Commit $commit -Tree $tree -Scripts $scripts -Wsl "Ubuntu$(if ($system.Count) { " ($($system[0]))" })"
    # The same bytes as every earlier build of the same pins, or the release stops here.
    $reproduced = Get-FFmpegReproductionRefusals -Record $record -Earlier $earlier
    if ($reproduced.Refusals.Count) {
        $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'ffmpeg-record-NOT-REPRODUCED.json') -Encoding utf8
        Set-Content -LiteralPath (Join-Path $run 'NOT-REPRODUCED.txt') -Encoding utf8 -Value (@(
            'This FFmpeg part is not what an earlier build of the same pins made (or, for x64, the DLLs 0.1.2''s release published from the same sources and toolchain), and is no part of any release:') + $reproduced.Refusals)
        Rename-Item -LiteralPath $partFolder -NewName 'not-reproduced'
        throw "FFmpeg was not built as before from the same pins, and the release stops here: $($reproduced.Refusals -join '; '). NOT-REPRODUCED.txt in $run says what differs, and the build's record is kept beside it; nothing of it is a part of any release. Find why the same sources, toolchain and scripts made other bytes before a release is built."
    }
    $record.reproduces = @($reproduced.Matched)
    $recordFile = Join-Path $partFolder $FFmpegRecordName
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $recordFile -Encoding utf8
    # (What the build part will hold it to, held to it here first.)
    $self = Get-FFmpegPartRefusals -Folder $partFolder -Commit $commit -Tree $tree
    if ($self.Refusals.Count) { throw "The FFmpeg part just made is not what its record names: $($self.Refusals -join '; ')." }
    New-Item -ItemType Directory -Force -Path $Records | Out-Null
    $kept = Join-Path $Records "ffmpeg-$version-$stamp-$($commit.Substring(0, 12)).json"
    if (Test-Path -LiteralPath $kept) { throw "$kept is there already." }
    Copy-Item -LiteralPath $recordFile -Destination $kept
    Write-Output "FFmpeg part: $partFolder" "Record: $recordFile" "SHA-256: $(Get-Sha256 $recordFile)"
    Write-Output $(if ($reproduced.Matched.Count) { "Byte for byte the builds of the same pins in $($reproduced.Matched -join ', ')." } else { 'The first build of these pins in the records: nothing to compare it with yet.' })
    # (And x64's, of 0.1.2's sources and toolchain, the DLLs that release published: held above with the earlier records,
    # review r2 of arm and armp, L4.)
    Write-Output $(if ($reproduced.Published) { "x64's FFmpeg DLLs are byte for byte the ones $($FFmpegPublished.release)'s release published from the same sources and toolchain." } else { "x64's FFmpeg is not of $($FFmpegPublished.release)'s sources and toolchain: it was not compared with what that release published." })
    Write-Output 'Copy that folder to the PC that builds the release, and name it there with -Part build -FFmpeg.'
    Write-Output "With the release, keep the record of the part it takes in the private repository too: desktop-notes/releases/$version/ffmpeg-record.json, committed with the release."
    return
}

Write-Output "Building qKVM Desktop $version for x64 and arm64 from $commit in $src$(if ($Trial) { ' (TRIAL)' })"
# The FFmpeg part, as its record names it (checked above), copied into the tree; and the copies checked once more.
foreach ($kind in $FFmpegKinds) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ffmpegIn[$kind]) | Out-Null
    Copy-Item -LiteralPath (Join-Path $FFmpeg $kind) -Destination $ffmpegIn[$kind] -Recurse
    $build = @($ffmpegPart.Record.builds | Where-Object { $_.architecture -ceq $kind })[0]
    $again = @(Compare-FolderToFiles $ffmpegIn[$kind] @($build.files) $kind)
    if ($again.Count) { throw "The FFmpeg part's copy is not what its record names: $($again -join '; ')." }
}
foreach ($source in @($ffmpegPart.Record.sources)) {
    $to = Join-Path $downloads "ffmpeg\$($source.archive)"
    Copy-Item -LiteralPath (Join-Path $FFmpeg "sources\$($source.archive)") -Destination $to -Force
    if ((Get-Item -LiteralPath $to).Length -ne [long] $source.bytes -or (Get-Sha256 $to) -cne $source.sha256) { throw "The copy of $($source.archive) is not the archive the FFmpeg part's record names." }
}
$ffmpegRecordFile = Join-Path $run $FFmpegRecordName
Copy-Item -LiteralPath (Join-Path $FFmpeg $FFmpegRecordName) -Destination $ffmpegRecordFile
if ((Get-Sha256 $ffmpegRecordFile) -cne (Get-Sha256 (Join-Path $FFmpeg $FFmpegRecordName))) { throw 'The copy of the FFmpeg part''s record is not its record.' }

Invoke-Clean '1-qt' @('-File', "$src\tools\fetch_qt.ps1", '-Into', $qt, '-Downloads', "$downloads\qt")
Invoke-Clean '1-qt-arm64' @('-File', "$src\tools\fetch_qt.ps1", '-Into', $qt, '-Downloads', "$downloads\qt", '-Arch', 'arm64')
foreach ($kind in $FFmpegKinds) {
    Invoke-Clean "2-ffmpeg-libraries-$kind" @('-File', "$src\tools\build_ffmpeg.ps1", '-Arch', $kind, '-ImportLibrariesOnly')
}
Invoke-Clean '3-webrtc' @('-File', "$src\tools\fetch_webrtc.ps1")
Invoke-Clean '3-webrtc-arm64' @('-File', "$src\tools\fetch_webrtc.ps1", '-Arch', 'arm64')

# Configured with the publish presets (the key on the command line only for a trial), built, and the tools asked what
# they are; x64's first, whose tool the ARM64 cross-build runs.
$keyArgument = if ($Trial) { "'-DQKVM_UPDATE_PUBLIC_KEY=$publicKey'" } else { '' }
$presetOf = [ordered]@{ x64 = 'publish'; arm64 = 'arm64-publish' }
foreach ($kind in $FFmpegKinds) {
    $buildPreset = $presetOf[$kind]
    $versions = Join-Path $run "tools-$kind.json"
    $hostTools = if ($kind -ceq 'arm64') { "'-DQKVM_HOST_TOOLS=$src\build\publish\bin'" } else { '' }
    $build = @"
`$ErrorActionPreference = 'Stop'
. '$src\tools\common.ps1'
Enter-BuildEnvironment -Arch $kind
Set-Location '$src'
cmake --preset $buildPreset $keyArgument $hostTools
if (`$LASTEXITCODE -ne 0) { throw 'The $buildPreset configuration failed.' }
cmake --build --preset $buildPreset --target qkvm-desktop qkvm-setup qkvm-update-check
if (`$LASTEXITCODE -ne 0) { throw 'The $buildPreset build failed.' }
. '$src\tools\build_common.ps1'
Get-BuildToolVersions | ConvertTo-Json | Set-Content -LiteralPath '$versions' -Encoding utf8
"@
    Invoke-Clean "4-build-$kind" @('-Command', $build)
}
Invoke-Clean '5-package-x64' @('-File', "$src\tools\package.ps1", '-Preset', 'publish', '-SkipBuild')
Invoke-Clean '5-package-arm64' @('-File', "$src\tools\package.ps1", '-Arch', 'arm64', '-Preset', 'publish', '-SkipBuild')

# What was made, moved out of src (which the next run makes anew) into this run's folder; and each kind's checks.
$packages = [ordered]@{}
$zips = [ordered]@{}
$updateChecks = [ordered]@{}
$imagesOf = [ordered]@{}
New-Item -ItemType Directory -Path (Join-Path $run 'symbols') | Out-Null
foreach ($kind in $FFmpegKinds) {
    $latest = if ($kind -ceq 'x64') { 'latest.json' } else { "latest-$kind.json" }
    $made = Get-Content -LiteralPath "$src\build\packages\$latest" -Raw | ConvertFrom-Json
    $package = Join-Path $run (Split-Path -Leaf $made.folder)
    Move-Item -LiteralPath $made.folder -Destination $package
    $zip = Join-Path $run (Split-Path -Leaf $made.archive)
    Move-Item -LiteralPath $made.archive -Destination $zip
    Move-Item -LiteralPath "$($made.archive).sha256" -Destination "$zip.sha256"
    Move-Item -LiteralPath $made.audit -Destination (Join-Path $run "audit-$kind.json")
    Move-Item -LiteralPath $made.symbols -Destination (Join-Path $run "symbols\$kind")
    if ((Split-Path -Leaf $zip) -cne "qKVM-Desktop-$version-windows-$kind.zip") { throw "The $kind package is not named for its version and kind: $(Split-Path -Leaf $zip)" }
    $packages[$kind] = $package
    $zips[$kind] = $zip

    # The program names its symbols by file name alone, and is the one this tree's version says...
    $program = Join-Path $package 'qkvm-desktop.exe'
    if (-not (Test-BareDebugPath $program)) {
        throw "The $kind qkvm-desktop.exe names its symbols with a folder: $(@(Get-PeDebugPaths $program) -join ', ')"
    }
    if ((Get-Item -LiteralPath $program).VersionInfo.ProductVersion -cne $version) { throw "The $kind program is not the version the tree says." }
    # ...and carries the feed and the key it was configured with.
    $cache = Get-Content -LiteralPath "$src\build\$($presetOf[$kind])\CMakeCache.txt"
    $builtKey = @($cache | Where-Object { $_.StartsWith('QKVM_UPDATE_PUBLIC_KEY:STRING=') } | ForEach-Object { ($_ -split '=', 2)[1] })
    if ($builtKey.Count -ne 1 -or $builtKey[0] -ne $publicKey) { throw "The $kind build was not configured with the public key it was given." }
    $programBytes = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($program))
    foreach ($carried in $PublicUpdateFeed, $builtKey[0]) {
        if (-not $programBytes.Contains("$carried`0")) { throw "The $kind program does not carry $carried." }
    }
    # Nothing that ships names the folder it was built in (the libraries built here keep their source paths from below
    # it: experiments/webrtc/build-cpp-dependencies.ps1).
    foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File) {
        if (Test-NamesFolder $file.FullName $Root) {
            throw "$kind $([IO.Path]::GetRelativePath($package, $file.FullName)) names the folder it was built in."
        }
    }
    $imagesOf[$kind] = @(Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object Extension -in '.exe', '.dll' |
        Sort-Object FullName | ForEach-Object {
            [ordered]@{
                path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/')
                sha256 = Get-Sha256 $_.FullName
                symbols = @(Get-PeDebugPaths $_.FullName)
            }
        })
    # (qKVM Setup's update check of this kind, the publish build's own, which the setup carries and puts at the program's
    # name while the earlier version is set aside: it names its symbols by file name alone, as the program does.)
    $updateCheck = "$src\build\$($presetOf[$kind])\bin\qkvm-update-check.exe"
    if (-not (Test-BareDebugPath $updateCheck)) {
        throw "The $kind qkvm-update-check.exe names its symbols with a folder: $(@(Get-PeDebugPaths $updateCheck) -join ', ')"
    }
    $updateChecks[$kind] = $updateCheck
}
# Both packages made: the ARM64 one holds the x64 one's files path for path, but what only one kind has (x64's
# vcruntime140_1.dll; build_common.ps1's Get-PackagePairRefusals, review arm 1, L3): a plugin windeployqt left out of it,
# or any file of x64's it lacks, stops the run here, before the setup, rather than at its start on Windows on Arm.
function Read-PackageManifest([string] $Package) { return Get-Content -LiteralPath (Join-Path $Package 'package-manifest.json') -Raw | ConvertFrom-Json }
$pair = @(Get-PackagePairRefusals -X64 (Read-PackageManifest $packages['x64']) -Arm64 (Read-PackageManifest $packages['arm64']))
if ($pair.Count) { throw "The ARM64 package is not the x64 one's files of its kind: $($pair -join '; ')." }

# The setup, from this run's own two packages as they were checked above: Inno Setup unpacked here, never installed; the
# setup, which carries both and installs the PC's own, and its uncompressed twin for the byte scan; its static test.
# Neither names the build folder (inside the setup only what is not compressed can be looked at; in the twin all of it).
$setupWork = Join-Path $run 'setup'
$bothPackages = "$($packages['x64']),$($packages['arm64'])"
$bothChecks = "$($updateChecks['x64']),$($updateChecks['arm64'])"
Invoke-Clean '6-inno' @('-File', "$src\tools\fetch_inno.ps1", '-Into', $inno, '-Downloads', "$downloads\inno")
Invoke-Clean '7-setup' @('-File', "$src\tools\build_setup.ps1", '-PackageDirectory', $bothPackages, '-Inno', (Join-Path $inno $InnoVersion), '-OutputDirectory', $setupWork, '-UpdateCheck', $bothChecks, '-ScanTwin')
Invoke-Clean '8-setup-test' @('-File', "$src\tools\test_setup.ps1", '-SetupDirectory', $setupWork, '-PackageDirectory', $bothPackages, '-Inno', (Join-Path $inno $InnoVersion))
$setupName = Get-SetupName $version
$setupFile = Join-Path $setupWork $setupName
$twinFile = Join-Path $setupWork "scan\$setupName"
foreach ($file in $setupFile, $twinFile) {
    if (Test-NamesFolder $file $Root) { throw "$([IO.Path]::GetRelativePath($run, $file)) names the folder it was built in." }
}

# Each kind's beta audit, the setup and the FFmpeg part's record with it, and the byte scan for what must not get out.
$betaRefused = @()
$betaOpen = @()
foreach ($kind in $FFmpegKinds) {
    $auditFile = Join-Path $run "audit-beta-$kind.json"
    $auditLog = Join-Path $logs "9-audit-beta-$kind.txt"
    try {
        & "$src\tools\audit_release.ps1" -PackageDirectory $packages[$kind] -OutputFile $auditFile -Beta -SetupDirectory $setupWork -SetupPackageDirectory $bothPackages -FFmpegRecord $ffmpegRecordFile *>&1 |
            Tee-Object -FilePath $auditLog
    } catch {
        "$_" | Tee-Object -FilePath $auditLog -Append
    }
    if (-not (Test-Path -LiteralPath $auditFile)) { $betaRefused += "${kind}: the audit wrote no record"; continue }
    $betaAudit = Get-Content -LiteralPath $auditFile -Raw | ConvertFrom-Json
    $betaRefused += @($betaAudit.gates | Where-Object { $_.open -and -not $_.beta } | ForEach-Object { "${kind}: $($_.id)" })
    $betaOpen += @($betaAudit.gates | Where-Object open | ForEach-Object { [ordered]@{ kind = $kind; id = $_.id; accepted = $_.beta } })
}
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
    & $python -I (Resolve-Path -LiteralPath $Scanner).Path @theirs $packages['x64'] $zips['x64'] $packages['arm64'] $zips['arm64'] $setupFile $twinFile *>&1 | Tee-Object -FilePath (Join-Path $logs '10-scan.txt')
    $scan = switch ($LASTEXITCODE) { 0 { 'clean' } 1 { 'findings' } default { 'failed' } }
} else {
    Write-Output 'The private byte scan is not beside this script, and was not run (-Scanner names it).'
}

# The record: enough to say later, of any file, whether it is the one built here.
$webrtc = Join-Path $src '.deps\webrtc-trial'
$record = [ordered]@{
    schema = 2
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
    tools = [ordered]@{ x64 = Get-Content -LiteralPath (Join-Path $run 'tools-x64.json') -Raw | ConvertFrom-Json
        arm64 = Get-Content -LiteralPath (Join-Path $run 'tools-arm64.json') -Raw | ConvertFrom-Json }
    dependencies = [ordered]@{
        qt = @(Get-ChildItem -LiteralPath "$downloads\qt" -File | Sort-Object Name | ForEach-Object {
            [ordered]@{ archive = $_.Name; bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } })
        # (The FFmpeg part's record, as it was taken: its sources, toolchains, configurations and files; and the import
        # libraries made here from its export lists.)
        ffmpeg = [ordered]@{
            record = $ffmpegPart.Record
            recordSha256 = Get-Sha256 $ffmpegRecordFile
            importLibraries = @($FFmpegKinds | ForEach-Object {
                $kind = $_
                Get-ChildItem -LiteralPath (Join-Path $ffmpegIn[$kind] 'lib') -File -Filter '*.lib' | Sort-Object Name | ForEach-Object {
                    [ordered]@{ architecture = $kind; file = $_.Name; bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } } })
        }
        webrtc = [ordered]@{
            libdatachannel = [string] (Invoke-Git "$webrtc\libdatachannel" @('rev-parse', 'HEAD'))
            submodules = @(Invoke-Git "$webrtc\libdatachannel" @('submodule', 'status') | ForEach-Object { "$_".Trim() })
            mbedtls = Get-Sha256 "$webrtc\mbedtls-3.6.7.tar.bz2"
        }
        # (What tools/fetch_inno.ps1 says of its unpacking: the installer, how it was run, the registry before and after.)
        inno = Get-Content -LiteralPath (Join-Path $inno "$InnoVersion-unpacked.json") -Raw | ConvertFrom-Json
    }
    packages = @($FFmpegKinds | ForEach-Object {
        $kind = $_
        [ordered]@{
            architecture = $kind
            archive = Split-Path -Leaf $zips[$kind]
            bytes = (Get-Item -LiteralPath $zips[$kind]).Length
            sha256 = Get-Sha256 $zips[$kind]
            folder = Split-Path -Leaf $packages[$kind]
            manifestSha256 = Get-Sha256 (Join-Path $packages[$kind] 'package-manifest.json')
            files = @(Get-ChildItem -LiteralPath $packages[$kind] -Recurse -File).Count
            images = $imagesOf[$kind]
        } })
    setup = [ordered]@{ name = $setupName; file = "setup/$setupName"; bytes = (Get-Item -LiteralPath $setupFile).Length; sha256 = Get-Sha256 $setupFile
        scanTwin = [ordered]@{ file = "setup/scan/$setupName"; bytes = (Get-Item -LiteralPath $twinFile).Length; sha256 = Get-Sha256 $twinFile }
        made = Get-Content -LiteralPath (Join-Path $setupWork 'setup-record.json') -Raw | ConvertFrom-Json }
    symbols = [ordered]@{ x64 = Get-Content -LiteralPath (Join-Path $run 'symbols\x64\manifest.json') -Raw | ConvertFrom-Json
        arm64 = Get-Content -LiteralPath (Join-Path $run 'symbols\arm64\manifest.json') -Raw | ConvertFrom-Json }
    checks = [ordered]@{
        betaAudit = $(if ($betaRefused.Count) { 'refused' } else { 'accepted' })
        betaRefused = $betaRefused
        betaOpen = $betaOpen
        privateScan = $scan
        # (Invoke-Clean stops the run where the setup's static test fails: a run that got here passed it.)
        setupTest = 'passed'
        # (An x64 PC cannot start ARM64 programs: tools/test_package_archive.ps1 -Evidence on the ARM64 ZIP, on Windows on
        # Arm; release_files.ps1 -Arm64Startup keeps its record with the run, as arm64-startup.json, and refuses without.)
        arm64Startup = 'not run here'
        # (The tools those checks are to be run with, of the tree built here, by SHA-256: release_files.ps1 and
        # release_commands.ps1 take a record of them made by these alone; review r2 of arm and armp, L2.)
        arm64StartupTools = Get-StartupCheckTools "$src\tools"
    }
}
$record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $run 'build-record.json') -Encoding utf8
foreach ($kind in $FFmpegKinds) { Write-Output "Built: $($zips[$kind])" "SHA-256: $(Get-Sha256 $zips[$kind])" }
Write-Output "Setup: $setupFile" "SHA-256: $($record.setup.sha256)" "Record: $run\build-record.json"
Write-Output 'The ARM64 package''s startup checks are run on Windows on Arm next: the export''s tools folder and that ZIP copied there, tools\test_package_archive.ps1 -Archive <its ZIP> -Evidence <a new file> from that copy; release_files.ps1 -Arm64Startup takes that record.'
if ($Trial) { Write-Output 'TRIAL: built with a throwaway key; never hand any of it out.' }
$against = @()
if ($betaRefused.Count) { $against += "the beta audit refused it ($($betaRefused -join ', '))" }
if ($scan -ne 'clean') { $against += "the private byte scan: $scan" }
if ($against.Count) { throw "Built, and NOT FOR RELEASE: $($against -join '; '). See $logs." }
