#Requires -Version 7.2
<#
The setup's static test: what tools/build_setup.ps1 made, read and never run.

    pwsh -File tools\test_setup.ps1 -SetupDirectory <what build_setup.ps1 made> -PackageDirectory <its package>
    pwsh -File tools\test_setup.ps1                  (its own cases alone)

First its own cases, on made-up packages in a folder of its own, compiled with the Inno Setup of .deps\inno
(tools/fetch_inno.ps1; -Inno names another folder of it): a package of a few files is made into a setup that
passes, and then each way of being wrong is seen to fail, one at a time, so that a check that refuses nothing is
caught. build_setup.ps1 refuses a package with a file its manifest does not name, with a file of other bytes or
without one it names, with a name Inno Setup would read as something else, an output folder that is not new, and
an Inno Setup that is not the pinned one. The checks below fail a setup with a file more or less than the package,
one of other bytes or put elsewhere, one that asks for more than the user, a script other than the one committed,
another mutex, no marker or another one, other compression, something else run, the network asked, and a setup
whose own manifest asks for an administrator.

Then, with -SetupDirectory, the setup made from the package named:
- the script compiled, and the script is packaging/qkvm-desktop.iss as committed;
- the compiler's own list of what it put in (setup-files.txt) is the package's manifest, file for file and by
  SHA-256, with the manifest itself and the marker and nothing else, and files.iss puts each at its own place;
- PrivilegesRequired=lowest, with nothing that overrides it, and the setup's own manifest asks for nothing more
  (asInvoker);
- AppMutex is the program's single-instance mutex, as ui/main.cpp and ui/win/single_instance.cpp make it, and the
  update mode of the contract is there: /UPDATE=1, 60 s, exit code 105, the program started again; and the setup holds
  its own mutex, qKVM-Desktop-Setup (the program waits for it as Local\qKVM-Desktop-Setup); in update mode, before
  anything is changed, the room for every file and every file of the folder free to be replaced (Preparing to
  Install, exit code 7); and an older version over a newer one refused (the installed DisplayVersion, compared);
- the marker is "setup" and a line feed, put at {app}\qkvm-desktop.installed;
- the fixed AppId, publisher qKVM, the package's version, x64, Windows 10 22H2, lzma2 and solid, no closing of
  programs, the user's own Programs folder;
- Windows' junction guard left on (RedirectionGuard is never turned off); before an install nothing deleted but
  {app}\plugins and {app}\qml, which a later version may hold less of and Qt loads whole; the folder never chosen
  in the wizard (DisableDirPage=yes); one call outside Pascal Script, ExitProcess, and no other;
- the program's own file the last one put in (files.iss's last line, after the marker); in update mode it is set
  aside before any file is written, deleted after a whole copy, put back when the copy did not complete;
- after a whole copy only what the package's own list (kept.iss) has not is removed from plugins and qml: neither
  folder when it is itself a redirection, no redirection in them, no name ending in a dot or a blank; after a whole
  copy and at an uninstall, what a setup ended halfway left in the package's own folders (by its list, folders.iss,
  made at build time and never read from the folder; Inno Setup's temporary is-XXXXXXXXXX.tmp files: that very
  form, files only, no folder that is or is reached through a redirection), and at an uninstall the program an
  update ended halfway set aside; nothing else;
- nothing is run but the program itself, and nothing asks the network;
- where build_setup.ps1 made the uncompressed twin for the byte scan: it holds the same files, it is readable (the
  mutex's name is seen in it), and neither it nor the setup names the folders the setup was made from.
Every failed check is a "FAIL:" line, and the script then fails.
#>
param(
    [string] $SetupDirectory = '',
    [string] $PackageDirectory = '',
    [string] $Inno = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

$desktop = Split-Path -Parent $PSScriptRoot
if (-not $Inno) { $Inno = Join-Path $desktop ".deps\inno\$InnoVersion" }
$failures = 0
function Check([bool] $Condition, [string] $Message) {
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}

# The program's mutex, as the program makes it: "Local\" and the name main.cpp gives SingleInstance.
$main = [IO.File]::ReadAllText((Join-Path $desktop 'ui/main.cpp'))
$single = [IO.File]::ReadAllText((Join-Path $desktop 'ui/win/single_instance.cpp'))
$named = [regex]::Match($main, 'SingleInstance\s+\w+\(u"([^"]+)"_s\)')
Check ($named.Success -and $single.Contains('(u"Local\\"_s + name)')) 'the program''s mutex is read from ui/main.cpp and ui/win/single_instance.cpp'
$ProgramMutex = "Local\$($named.Groups[1].Value)"
$MarkerBytes = [byte[]] @(0x73, 0x65, 0x74, 0x75, 0x70, 0x0A)
$Committed = Join-Path $desktop 'packaging/qkvm-desktop.iss'

function Get-Sections([string] $Text) {
    <# The script's lines, by the section they are in (its head before the first section is ''). #>
    $sections = @{ '' = [Collections.Generic.List[string]]::new() }
    $at = ''
    foreach ($line in ($Text -replace "`r`n", "`n").Split("`n")) {
        if ($line -match '^\[(\w+)\]\s*$') { $at = $Matches[1]; if (-not $sections.ContainsKey($at)) { $sections[$at] = [Collections.Generic.List[string]]::new() }; continue }
        $sections[$at].Add($line)
    }
    return $sections
}

function Get-SetupFailures([string] $Setup, [string] $Package) {
    <# What is wrong with the setup build_setup.ps1 made in $Setup from $Package: nothing, or one line for each. #>
    $said = [Collections.Generic.List[string]]::new()
    $manifest = Get-Content -LiteralPath (Join-Path $Package 'package-manifest.json') -Raw | ConvertFrom-Json
    $version = [string] $manifest.version
    $name = "qKVM-Desktop-$version-windows-x64-setup.exe"
    $exe = Join-Path $Setup $name
    $inputs = Join-Path $Setup 'inputs'
    $log = Join-Path $Setup 'compile.txt'

    # --- It compiled, from the script as committed.
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf) -or -not (Test-Path -LiteralPath $log) -or
        -not (Select-String -LiteralPath $log -Pattern '^Successful compile' -Quiet)) {
        $said.Add("the script did not compile into $name")
        return $said
    }
    $script = [IO.File]::ReadAllText((Join-Path $inputs 'qkvm-desktop.iss'))
    if ((Get-Sha256 (Join-Path $inputs 'qkvm-desktop.iss')) -cne (Get-Sha256 $Committed)) { $said.Add('the script compiled is not packaging/qkvm-desktop.iss as committed') }
    $sections = Get-Sections $script
    $setupLines = @($sections['Setup'] | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith(';') })

    # --- What the compiler put in: the package, file for file, the manifest and the marker; nothing else.
    $expected = @{}
    foreach ($entry in $manifest.files) {
        $expected[(Join-Path $Package ($entry.path -replace '/', '\')).ToLowerInvariant()] = [ordered]@{ path = $entry.path; sha256 = $entry.sha256; bytes = [long] $entry.bytes }
    }
    $manifestFile = Join-Path $Package 'package-manifest.json'
    $expected[$manifestFile.ToLowerInvariant()] = [ordered]@{ path = 'package-manifest.json'; sha256 = Get-Sha256 $manifestFile; bytes = (Get-Item -LiteralPath $manifestFile).Length }
    $markerSource = Join-Path $inputs 'qkvm-desktop.installed'
    $expected[$markerSource.ToLowerInvariant()] = [ordered]@{ path = 'qkvm-desktop.installed'
        sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($MarkerBytes)).ToLowerInvariant(); bytes = $MarkerBytes.Length }
    $listed = @(Get-Content -LiteralPath (Join-Path $Setup 'setup-files.txt') | ConvertFrom-Csv -Delimiter "`t")
    $seen = @{}
    foreach ($row in $listed) {
        $source = ([string] $row.SourceFilename).ToLowerInvariant()
        if (-not $expected.ContainsKey($source)) { $said.Add("the setup holds a file the package does not have: $($row.SourceFilename)"); continue }
        if ($seen.ContainsKey($source)) { $said.Add("the setup holds $($expected[$source].path) twice"); continue }
        $seen[$source] = $true
        if ([string] $row.SHA256Sum -cne $expected[$source].sha256 -or [long] $row.OriginalSize -ne $expected[$source].bytes) {
            $said.Add("the setup holds other bytes than the package's manifest names for $($expected[$source].path)")
        }
        if ([string] $row.Encrypted -cne 'no') { $said.Add("the setup holds $($expected[$source].path) encrypted") }
    }
    foreach ($source in $expected.Keys | Where-Object { -not $seen.ContainsKey($_) }) {
        $said.Add("the setup lacks $($expected[$source].path), which the package has")
    }
    # files.iss: each of the package's files at its own place, and nothing more.
    $lines = @(Get-Content -LiteralPath (Join-Path $inputs 'files.iss') | Where-Object { $_.Trim() })
    $placed = @{}
    foreach ($line in $lines) {
        if ($line -cnotmatch '\ASource: "([^"{}]+)"; DestDir: "\{app\}((?:\\[^"\\{}]+)*)"; Flags: ignoreversion\z') { $said.Add("files.iss has a line that is not one file at its own place: $line"); continue }
        $relative = $Matches[1]
        $folder = $Matches[2]
        $wanted = Split-Path -Parent $relative
        if ($folder -cne $(if ($wanted) { "\$wanted" } else { '' })) { $said.Add("files.iss puts $relative elsewhere than its own place ({app}$folder)") }
        $placed[(Join-Path $Package $relative).ToLowerInvariant()] = $true
    }
    foreach ($source in $expected.Keys | Where-Object { $_ -cne $markerSource.ToLowerInvariant() }) {
        if (-not $placed.ContainsKey($source)) { $said.Add("files.iss does not put $($expected[$source].path) in the program's folder") }
    }
    if ($lines.Count -ne $expected.Count - 1) { $said.Add("files.iss has $($lines.Count) lines for $($expected.Count - 1) files") }
    # (The program's own file last: whatever starts it during the copy finds it beside the libraries it was built with.)
    if ($lines.Count -and $lines[-1] -cne 'Source: "qkvm-desktop.exe"; DestDir: "{app}"; Flags: ignoreversion') { $said.Add("the program's file is not the last one files.iss puts in") }

    # --- For the user alone.
    if ($setupLines -cnotcontains 'PrivilegesRequired=lowest') { $said.Add('PrivilegesRequired is not lowest') }
    if (@($setupLines | Where-Object { $_ -match '^PrivilegesRequiredOverridesAllowed' }).Count) { $said.Add('PrivilegesRequired may be overridden') }
    $level = [regex]::Match((Get-PeManifest $exe), 'requestedExecutionLevel\s+level="([^"]+)"\s+uiAccess="([^"]+)"')
    if (-not $level.Success -or $level.Groups[1].Value -cne 'asInvoker' -or $level.Groups[2].Value -cne 'false') {
        $said.Add("the setup's own manifest asks for more than the user ($($level.Value))")
    }
    # --- The mutex, the update mode and the marker of the contract.
    if ($setupLines -cnotcontains "AppMutex={code:ProgramMutex|$ProgramMutex}") { $said.Add("AppMutex is not the program's mutex ($ProgramMutex)") }
    $code = ($sections['Code'] -join "`n")
    foreach ($part in @("ExpandConstant('{param:UPDATE|0}') = '1'", 'UpdateWaitMs = 60000;', 'StillRunningExitCode = 105;',
        'ExitProcess(StillRunningExitCode);', "external 'ExitProcess@kernel32.dll stdcall';", 'Result := Param;',
        # (In update mode, before anything is changed: room for every file, and every file of the folder free to be
        # replaced; else Preparing to Install refuses, Inno Setup's exit code 7.)
        'function PrepareToInstall(var NeedsRestart: Boolean): String;', 'GetSpaceOnDisk64(', '{#PackageBytes}',
        'OpenReadWriteExclusive = $12;', 'TFileStream.Create(', '  else if Free < Needed then', "    if Busy <> '' then",
        '        if (Found.Attributes and ReparsePointAttribute) <> 0 then', "    until (Result <> '') or not FindNext(Found);",
        # (Never an older version over a newer one.)
        'function InitializeSetup: Boolean;', 'ComparePackedVersion(', '  if ComparePackedVersion(Have, Offered) > 0 then begin',
        "RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'DisplayVersion', Installed)")) {
        if (-not $code.Contains($part)) { $said.Add("the update mode of the contract lacks: $part") }
    }
    $files = @($sections['Files'] | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith(';') })
    if ($files -cnotcontains 'Source: "{#SourcePath}qkvm-desktop.installed"; DestDir: "{app}"; Flags: ignoreversion' -or
        $files -cnotcontains '#include "files.iss"' -or $files.Count -ne 2 -or $files[-1] -cne '#include "files.iss"') {
        $said.Add('the [Files] are not the package and the marker at {app}\qkvm-desktop.installed')
    }
    if (-not (Test-Path -LiteralPath $markerSource) -or (Get-Sha256 $markerSource) -cne $expected[$markerSource.ToLowerInvariant()].sha256) {
        $said.Add('the marker is not "setup" and a line feed')
    }
    # --- The rest of what it is.
    foreach ($line in "AppId={{A964EFA6-87C6-4A23-A9CC-B71F87441433}", 'AppPublisher=qKVM', 'AppVersion={#AppVersion}',
        'ArchitecturesAllowed=x64os', 'ArchitecturesInstallIn64BitMode=x64os', 'MinVersion=10.0.19045', 'CloseApplications=no', 'SetupMutex=qKVM-Desktop-Setup', 'DisableDirPage=yes',
        'DefaultDirName={userpf}\qKVM Desktop', 'UninstallDisplayIcon={app}\qkvm-desktop.exe',
        'OutputBaseFilename=qKVM-Desktop-{#AppVersion}-windows-x64-setup', 'SetupIconFile={#SourcePath}qkvm.ico') {
        if ($setupLines -cnotcontains $line) { $said.Add("the [Setup] section does not say $line") }
    }
    if (-not ($script -replace "`r`n", "`n").Contains("#else`nCompression=lzma2`nSolidCompression=yes`n#endif")) { $said.Add('the setup is not compressed with lzma2, solid') }
    # Its version, as the program's updates read it (prepare_update_payload.ps1 -Setup): the fixed file and product
    # versions exactly X.Y.Z.0, and the texts X.Y.Z (Inno Setup pads its version texts with blanks).
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
    $fixed = '{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
    $product = '{0}.{1}.{2}.{3}' -f $info.ProductMajorPart, $info.ProductMinorPart, $info.ProductBuildPart, $info.ProductPrivatePart
    if ($fixed -cne "$version.0" -or $product -cne "$version.0" -or "$($info.FileVersion)".Trim() -cne $version -or "$($info.ProductVersion)".Trim() -cne $version) {
        $said.Add("the setup does not say the package's version ($version): file $fixed '$($info.FileVersion)', product $product '$($info.ProductVersion)'")
    }
    if ((Get-Sha256 (Join-Path $inputs 'qkvm.ico')) -cne (Get-Sha256 (Join-Path $desktop 'ui/brand/qkvm.ico'))) { $said.Add('the setup''s icon is not the program''s') }
    # --- Nothing run but the program itself, nothing asked of the network.
    $run = @($sections['Run'] | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith(';') })
    # (Both with the one argument --after-update: the program then starts itself again through the shell, a copy that
    # inherits nothing of the setup, Windows' junction guard included.)
    $wantedRun = @('Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "{cm:LaunchProgram,qKVM Desktop}"; Flags: nowait postinstall skipifsilent; Check: not UpdateMode',
        'Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Flags: nowait runasoriginaluser; Check: UpdateMode')
    if (($run -join "`n") -cne ($wantedRun -join "`n")) { $said.Add('something else than the program is run, or the program otherwise') }
    # Windows' junction guard stays on for the setup and the uninstall, as Inno Setup has it unless told otherwise.
    if (@($setupLines | Where-Object { $_ -imatch '^RedirectionGuard\s*=\s*no\b' }).Count) { $said.Add('the setup turns RedirectionGuard off') }
    # Nothing deleted before the copy; after a whole copy only what an earlier version left in the plugin and QML
    # folders (Qt loads them whole) and this package has not: by the package's own list, made into the setup at build
    # time (kept.iss, from package-manifest.json), never read from the folder; nothing outside the two folders.
    $kept = @(Get-Content -LiteralPath (Join-Path $inputs 'kept.iss') | ForEach-Object {
        if ($_ -cmatch "\A  Result := Result \+ '([^']+)\|';\z") { $Matches[1] } })
    $packaged = @($manifest.files | ForEach-Object { [string] $_.path } | Where-Object { $_ -cmatch '\A(plugins|qml)/' } |
        ForEach-Object { ($_ -replace '/', '\').ToLowerInvariant() })
    if ((@($kept | Sort-Object -CaseSensitive) -join "`n") -cne (@($packaged | Sort-Object -CaseSensitive) -join "`n")) {
        $said.Add("the setup's list of what it keeps in plugins and qml is not the package's ($($kept.Count) for $($packaged.Count))")
    }
    $keptText = [IO.File]::ReadAllText((Join-Path $inputs 'kept.iss')) -replace "`r`n", "`n"
    if (-not $keptText.Contains("function KeptFiles: String;`nbegin`n  Result := '|';`n") -or -not $keptText.TrimEnd().EndsWith("`nend;") -or
        @(($keptText -split "`n") | Where-Object { $_ -and -not $_.StartsWith('//') -and $_ -cnotmatch "\A(  Result := Result \+ '[^']+\|';|function KeptFiles: String;|begin|  Result := '\|';|end;)\z" }).Count) {
        $said.Add('kept.iss is not the one function that says the kept files and nothing else')
    }
    # What a setup ended halfway left is looked for only in the package's own folders: by the package's own list, made
    # into the setup at build time (folders.iss, from package-manifest.json), never read from the folder.
    $foldersFile = Join-Path $inputs 'folders.iss'
    $listedFolders = @(if (Test-Path -LiteralPath $foldersFile) { Get-Content -LiteralPath $foldersFile | ForEach-Object {
        if ($_ -cmatch "\A  Result := Result \+ '([^']*)\|';\z") { $Matches[1] } } })
    $ownFolders = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    [void] $ownFolders.Add('')
    foreach ($path in @($manifest.files | ForEach-Object { [string] $_.path })) { [void] $ownFolders.Add((Split-Path -Parent ($path -replace '/', '\'))) }
    if (($listedFolders -join "`n") -cne (@($ownFolders) -join "`n")) {
        $said.Add("the setup's list of the package's folders is not the package's ($($listedFolders.Count) for $($ownFolders.Count))")
    }
    $foldersText = if (Test-Path -LiteralPath $foldersFile) { [IO.File]::ReadAllText($foldersFile) -replace "`r`n", "`n" } else { '' }
    if (-not $foldersText.Contains("function PackageFolders: String;`nbegin`n  Result := '|';`n") -or -not $foldersText.TrimEnd().EndsWith("`nend;") -or
        @(($foldersText -split "`n") | Where-Object { $_ -and -not $_.StartsWith('//') -and $_ -cnotmatch "\A(  Result := Result \+ '[^']*\|';|function PackageFolders: String;|begin|  Result := '\|';|end;)\z" }).Count) {
        $said.Add('folders.iss is not the one function that says the package''s folders and nothing else')
    }
    foreach ($part in @('#include "kept.iss"', 'procedure RemoveStale(Folder, Relative, Kept: String);',
        "        end else if Pos('|' + Lowercase(Name) + '|', Kept) = 0 then begin", '          if DeleteFile(Path) then',
        '          if RemoveDir(Path) then', '  if CurStep = ssPostInstall then begin',
        "        else if (Found.Attributes and ReparsePointAttribute) <> 0 then`n          Log('Left as it is, a redirection: ' + Name)",
        # (A name ending in a dot or a blank is left alone: Windows reads it as the name without them, a kept file.)
        "        if (Copy(Found.Name, Length(Found.Name), 1) = '.') or (Copy(Found.Name, Length(Found.Name), 1) = ' ') then`n          Log('Left as it is, a name Windows reads as another: ' + Name)",
        # (Either folder, when it is itself a redirection, is not looked at at all.)
        'procedure RemoveStaleIn(Relative: String);', '  Redirected := (Found.Attributes and ReparsePointAttribute) <> 0;',
        "  if Redirected then`n    Log('Left as it is, a redirection: ' + Relative)`n  else`n    RemoveStale(Folder, Relative, KeptFiles);",
        "    RemoveStaleIn('plugins');", "    RemoveStaleIn('qml');",
        # In update mode the program's file is set aside before any file is written, so that a start during the copy finds
        # no program beside half-copied libraries; deleted after a whole copy, put back when the copy did not complete.
        "  Result := ExpandConstant('{app}\qkvm-desktop.exe.old-update');", '  if CurStep = ssInstall then begin',
        "    Installing := True;`n    SetProgramAside;", "    PostInstalled := True;",
        "  if UpdateMode and FileExists(ProgramPath) then`n    SetAside := RenameFile(ProgramPath, ProgramAside);",
        "    if SetAside then`n      DropAside;", 'procedure DeinitializeSetup;',
        # What a setup ended halfway left in the folder and every folder in it (Inno Setup's temporary files, is- and ten
        # letters or digits and .tmp, files only, no redirection followed), removed after a whole copy and at an uninstall,
        # with the program an update ended halfway set aside; nothing else.
        'function SetupLeftover(Name: String): Boolean;',
        "  Result := (Length(Name) = 17) and (Lowercase(Copy(Name, 1, 3)) = 'is-') and (Lowercase(Copy(Name, 14, 4)) = '.tmp');",
        "  for I := 4 to 13 do`n    if Result and (Pos(Uppercase(Copy(Name, I, 1)), 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') = 0) then`n      Result := False;",
        '#include "folders.iss"', 'procedure RemoveSetupLeftoversIn(Relative: String);',
        "  if ThroughRedirection(Relative) then begin`n    Log('Left as it is, a redirection: ' + Relative);`n    Exit;`n  end;",
        "  Folder := ExpandConstant('{app}');`n  if Relative <> '' then`n    Folder := Folder + '\' + Relative;`n  if not FindFirst(Folder + '\is-*.tmp', Found) then",
        "      if ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) = 0) and SetupLeftover(Found.Name) then begin`n        Entry := Folder + '\' + Found.Name;`n        if DeleteFile(Entry) then",
        "          Log('Could not remove what a setup ended halfway left: ' + Entry);`n      end;`n    until not FindNext(Found);",
        'function ThroughRedirection(Relative: String): Boolean;',
        "  Path := ExpandConstant('{app}');`n  Rest := Relative;`n  while (Rest <> '') and not Result do begin",
        "    Path := Path + '\' + Copy(Rest, 1, At - 1);`n    Rest := Copy(Rest, At + 1, Length(Rest));",
        "    if FindFirst(Path, Found) then begin`n      Result := (Found.Attributes and ReparsePointAttribute) <> 0;",
        "  Rest := PackageFolders;`n  Rest := Copy(Rest, 2, Length(Rest));`n  At := Pos('|', Rest);`n  while At > 0 do begin`n    RemoveSetupLeftoversIn(Copy(Rest, 1, At - 1));`n    Rest := Copy(Rest, At + 1, Length(Rest));`n    At := Pos('|', Rest);`n  end;",
        # (The install step's and the stale walk's own lines: misnamed, or cut after the first entry, they would
        # silently do nothing.)
        'procedure CurStepChanged(CurStep: TSetupStep);',
        "            Log('Could not remove what this version does not have: ' + Name);`n        end;`n      end;`n    until not FindNext(Found);",
        "    RemoveStaleIn('qml');`n    RemoveSetupLeftovers;",
        'procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);',
        "  if CurUninstallStep = usUninstall then begin`n    RemoveSetupLeftovers;`n    DropAside;`n  end;",
        "  if SetAside and not PostInstalled and not FileExists(ProgramPath) then`n    PutProgramBack;")) {
        if (-not $code.Contains($part)) { $said.Add("the removal after the copy lacks: $part") }
    }
    # (Deleted: a stale file, what a setup ended halfway left, and the program's own copy set aside; renamed: that copy,
    # aside and back; nothing else.)
    $deletes = (@([regex]::Matches($code, 'DeleteFile\([^)]*\)') | ForEach-Object Value) | Sort-Object -CaseSensitive) -join '|'
    $renames = (@([regex]::Matches($code, 'RenameFile\([^)]*\)') | ForEach-Object Value) | Sort-Object -CaseSensitive) -join '|'
    if ($deletes -cne 'DeleteFile(Entry)|DeleteFile(Path)|DeleteFile(ProgramAside)' -or
        $renames -cne 'RenameFile(ProgramAside, ProgramPath)|RenameFile(ProgramPath, ProgramAside)' -or
        ([regex]::Matches($code, 'RemoveDir\(')).Count -ne 1) {
        $said.Add("the [Code] section deletes or renames otherwise than the removal after the copy, what a setup ended halfway left and the program set aside ($deletes; $renames)")
    }
    # The one result line the program reads in the log when the setup ends with 105 or 7 (it cannot see the exit code):
    # the 105 line right before ExitProcess, so it is in the log before the process ends.
    $codeLines = @($sections['Code'] | ForEach-Object { $_.TrimEnd() } | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('//') })
    $at = [Array]::IndexOf($codeLines, "    Log('qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed');")
    if ($at -lt 0 -or $codeLines[$at + 1] -cne '    ExitProcess(StillRunningExitCode);') {
        $said.Add('the result line of exit code 105 is not logged right before ExitProcess')
    }
    if ($codeLines -cnotcontains "    Log('qKVM-RESULT: 7 nothing was changed: ' + Reason);") { $said.Add('the result line of exit code 7 is not logged') }
    if (([regex]::Matches($code, 'qKVM-RESULT: ')).Count -ne 2) { $said.Add('the [Code] section logs a result line otherwise than the two of the contract') }
    # One call outside Pascal Script and only one: ExitProcess.
    $outside = @($sections['Code'] | Where-Object { $_ -match '\bexternal\b' })
    if ($outside.Count -ne 1 -or $outside[0] -cne "  external 'ExitProcess@kernel32.dll stdcall';") { $said.Add("the [Code] section calls outside Pascal Script otherwise than ExitProcess: $($outside -join ' | ')") }
    foreach ($section in 'InstallDelete', 'UninstallRun', 'Registry', 'INI', 'UninstallDelete', 'Dirs', 'Components') {
        if ($sections.ContainsKey($section)) { $said.Add("the script has a [$section] section") }
    }
    foreach ($word in 'Exec', 'Download', 'CreateOleObject', 'CreateComObject', 'ExtractArchive', 'http', 'RegWrite', 'SaveStringToFile', 'DelTree') {
        if ($code -match "(?i)$word") { $said.Add("the [Code] section uses $word") }
    }
    # --- The twin of the byte scan, where it was made.
    $twin = Join-Path $Setup "scan\$name"
    if (Test-Path -LiteralPath $twin) {
        $twinRows = @(Get-Content -LiteralPath (Join-Path $Setup 'scan\setup-files.txt') | ConvertFrom-Csv -Delimiter "`t" | ForEach-Object { "$($_.SourceFilename)|$($_.SHA256Sum)" } | Sort-Object)
        $rows = @($listed | ForEach-Object { "$($_.SourceFilename)|$($_.SHA256Sum)" } | Sort-Object)
        if (($twinRows -join "`n") -cne ($rows -join "`n")) { $said.Add('the twin for the byte scan does not hold the same files') }
        $bytes = [IO.File]::ReadAllBytes($twin)
        $wide = [Text.Encoding]::Unicode
        if (-not ($wide.GetString($bytes).Contains($ProgramMutex) -or $wide.GetString($bytes, 1, $bytes.Length - 1).Contains($ProgramMutex))) {
            $said.Add('the twin for the byte scan is not readable (the mutex''s name is not seen in it)')
        }
        foreach ($folder in $inputs, $Package) {
            foreach ($file in $exe, $twin) {
                if (Test-NamesFolder $file $folder) { $said.Add("$(Split-Path -Leaf (Split-Path -Parent $file))\$(Split-Path -Leaf $file) names the folder $folder") }
            }
        }
    }
    return $said
}

# --- Its own cases first.
if (-not (Test-Path -LiteralPath (Join-Path $Inno 'ISCC.exe'))) { throw "There is no Inno Setup in ${Inno}: run tools\fetch_inno.ps1 first." }
$work = Join-Path $desktop "build/setup-tests/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
function New-FixturePackage([string] $Name, [hashtable] $Files) {
    $package = Join-Path $work "$Name/qKVM-Desktop-0.1.0-windows-x64"
    foreach ($file in $Files.Keys) {
        $path = Join-Path $package $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path, $Files[$file])
    }
    $entries = @(Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/'); bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } })
    [ordered]@{ product = 'qKVM'; version = '0.1.0'; architecture = 'x64'; files = $entries } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $package 'package-manifest.json') -Encoding utf8
    return $package
}
$fixture = @{ 'qkvm-desktop.exe' = 'a made-up program, never run'; 'Qt6Core.dll' = 'made-up'; 'plugins/platforms/qwindows.dll' = 'made-up'
    'qml/QtQuick/Shapes/qmldir' = 'module QtQuick.Shapes'; 'licenses/AGPL-3.0.txt' = 'licence'; 'START-HERE.txt' = 'start here' }
function Invoke-Build([string] $Package, [string] $Out, [string] $With = $Inno, [switch] $ScanTwin) {
    $arguments = @('-NoProfile', '-NonInteractive', '-File', "$PSScriptRoot/build_setup.ps1", '-PackageDirectory', $Package, '-Inno', $With, '-OutputDirectory', $Out)
    if ($ScanTwin) { $arguments += '-ScanTwin' }
    $said = & (Get-Process -Id $PID).Path @arguments 2>&1
    return [pscustomobject]@{ Ended = $LASTEXITCODE; Said = ($said | ForEach-Object { "$_" }) -join "`n" }
}
function Expect-Refusal([string] $Package, [string] $Expected, [string] $Message, [string] $Out = '', [string] $With = $Inno) {
    if (-not $Out) { $Out = Join-Path (Split-Path -Parent $Package) 'out' }
    $result = Invoke-Build $Package $Out $With
    Check ($result.Ended -ne 0 -and $result.Said -match $Expected) "$Message (exit $($result.Ended): $($result.Said))"
}

$good = New-FixturePackage 'good' $fixture
$goodOut = Join-Path $work 'good/out'
$result = Invoke-Build $good $goodOut -ScanTwin
Check ($result.Ended -eq 0) "a package is made into a setup ($($result.Said))"
if ($result.Ended -eq 0) {
    $said = @(Get-SetupFailures $goodOut $good)
    Check ($said.Count -eq 0) "the setup of a package passes ($($said -join '; '))"
    $record = Get-Content -LiteralPath (Join-Path $goodOut 'setup-record.json') -Raw | ConvertFrom-Json
    Check ($record.setup.name -ceq 'qKVM-Desktop-0.1.0-windows-x64-setup.exe' -and $record.setup.sha256 -ceq (Get-Sha256 (Join-Path $goodOut $record.setup.name))) 'the record names the setup by its SHA-256'
    Check ($record.inno.version -ceq $InnoVersion -and $record.package.manifestSha256 -ceq (Get-Sha256 (Join-Path $good 'package-manifest.json'))) 'the record names the Inno Setup and the package it was made with'

    # Each way of being wrong, alone, on a copy of what was made.
    function Expect-Failure([string] $Name, [scriptblock] $Change, [string] $Expected) {
        $copy = Join-Path $work "wrong-$Name"
        Copy-Item -LiteralPath $goodOut -Destination $copy -Recurse
        & $Change $copy
        $said = @(Get-SetupFailures $copy $good)
        Check (@($said | Where-Object { $_ -match $Expected }).Count -gt 0) "a setup $Name fails ($($said -join '; '))"
    }
    function Edit-Script([string] $Copy, [string] $From, [string] $To) {
        $file = Join-Path $Copy 'inputs/qkvm-desktop.iss'
        $text = [IO.File]::ReadAllText($file)
        # (The script is kept with Unix line ends: .gitattributes.)
        $From = $From.Replace("`r`n", "`n")
        $To = $To.Replace("`r`n", "`n")
        if (-not $text.Contains($From)) { throw "The script has no $From to change." }
        [IO.File]::WriteAllText($file, $text.Replace($From, $To))
    }
    function Edit-List([string] $Copy, [scriptblock] $Rows) {
        $file = Join-Path $Copy 'setup-files.txt'
        $lines = [Collections.Generic.List[string]]::new([string[]] @(Get-Content -LiteralPath $file))
        & $Rows $lines
        Set-Content -LiteralPath $file -Value $lines -Encoding utf8
    }
    $sha = { param([string] $text) [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text))).ToLowerInvariant() }
    Expect-Failure 'with a file more' { param($c) Edit-List $c { param($l) $l.Add("99`t$good\extra.dll`t2026-01-01 00:00:00.000`t`t$(& $sha 'x')`t1`t1`t1`t0`t0`t1`tno`t") } } 'a file the package does not have'
    Expect-Failure 'with a file less' { param($c) Edit-List $c { param($l) $l.RemoveAt(@($l | Select-String -SimpleMatch 'Qt6Core.dll')[0].LineNumber - 1) } } 'lacks Qt6Core.dll'
    Expect-Failure 'with other bytes' { param($c) Edit-List $c { param($l) $at = @($l | Select-String -SimpleMatch 'Qt6Core.dll')[0].LineNumber - 1; $l[$at] = $l[$at] -replace '\t[0-9a-f]{64}\t', "`t$('0' * 64)`t" } } 'other bytes .* Qt6Core\.dll'
    Expect-Failure 'with a file put elsewhere' { param($c) $f = Join-Path $c 'inputs/files.iss'; [IO.File]::WriteAllText($f, [IO.File]::ReadAllText($f).Replace('DestDir: "{app}\plugins\platforms"', 'DestDir: "{app}"')) } 'elsewhere than its own place'
    Expect-Failure 'with a file at a place of its own choosing' { param($c) $f = Join-Path $c 'inputs/files.iss'; [IO.File]::AppendAllText($f, "Source: `"Qt6Core.dll`"; DestDir: `"{sys}`"; Flags: ignoreversion`r`n") } 'not one file at its own place'
    Expect-Failure 'from another script' { param($c) Edit-Script $c 'AppPublisherURL=https://qkvm.com/' 'AppPublisherURL=https://qkvm.com/x' } 'not packaging/qkvm-desktop\.iss as committed'
    Expect-Failure 'for an administrator' { param($c) Edit-Script $c 'PrivilegesRequired=lowest' 'PrivilegesRequired=admin' } 'PrivilegesRequired is not lowest'
    Expect-Failure 'that may be overridden' { param($c) Edit-Script $c 'PrivilegesRequired=lowest' "PrivilegesRequired=lowest`r`nPrivilegesRequiredOverridesAllowed=commandline" } 'may be overridden'
    Expect-Failure 'with another mutex' { param($c) Edit-Script $c "|$ProgramMutex}" '|Global\QKVM Desktop}' } 'AppMutex is not'
    Expect-Failure 'that never waits' { param($c) Edit-Script $c 'UpdateWaitMs = 60000;' 'UpdateWaitMs = 0;' } 'lacks: UpdateWaitMs = 60000'
    Expect-Failure 'that ends with Inno Setup''s own 5' { param($c) Edit-Script $c 'StillRunningExitCode = 105;' 'StillRunningExitCode = 5;' } 'lacks: StillRunningExitCode = 105'
    Expect-Failure 'without a mutex of its own' { param($c) Edit-Script $c 'SetupMutex=qKVM-Desktop-Setup' '' } 'SetupMutex=qKVM-Desktop-Setup'
    Expect-Failure 'with its mutex for the whole PC' { param($c) Edit-Script $c 'SetupMutex=qKVM-Desktop-Setup' 'SetupMutex=Global\qKVM-Desktop-Setup' } 'SetupMutex=qKVM-Desktop-Setup'
    Expect-Failure 'without the marker' { param($c) Edit-Script $c 'Source: "{#SourcePath}qkvm-desktop.installed"; DestDir: "{app}"; Flags: ignoreversion' '' } 'the marker at'
    Expect-Failure 'with another marker' { param($c) [IO.File]::WriteAllText((Join-Path $c 'inputs/qkvm-desktop.installed'), 'setup') } 'marker is not'
    Expect-Failure 'compressed otherwise' { param($c) Edit-Script $c "#else`r`nCompression=lzma2" "#else`r`nCompression=zip" } 'lzma2, solid'
    Expect-Failure 'for another architecture' { param($c) Edit-Script $c 'ArchitecturesAllowed=x64os' 'ArchitecturesAllowed=x64compatible' } 'ArchitecturesAllowed=x64os'
    Expect-Failure 'that closes programs' { param($c) Edit-Script $c 'CloseApplications=no' 'CloseApplications=yes' } 'CloseApplications=no'
    Expect-Failure 'that turns the junction guard off' { param($c) Edit-Script $c 'CloseApplications=no' "CloseApplications=no`r`nRedirectionGuard=no" } 'turns RedirectionGuard off'
    Expect-Failure 'that deletes before it installs' { param($c) Edit-Script $c '[Files]' "[InstallDelete]`r`nType: filesandordirs; Name: `"{app}\plugins`"`r`n`r`n[Files]" } '\[InstallDelete\] section'
    Expect-Failure 'that keeps a file the package does not have' { param($c) $k = Join-Path $c 'inputs/kept.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("  Result := '|';", "  Result := '|';`n  Result := Result + 'plugins\stale.dll|';")) } 'list of what it keeps'
    Expect-Failure 'that keeps less than the package' { param($c) $k = Join-Path $c 'inputs/kept.iss'; [IO.File]::WriteAllText($k, (@([IO.File]::ReadAllLines($k)) | Where-Object { $_ -notmatch 'qwindows' }) -join "`n") } 'list of what it keeps'
    Expect-Failure 'whose list does more than say the files' { param($c) $k = Join-Path $c 'inputs/kept.iss'; [IO.File]::AppendAllText($k, "procedure Other;`nbegin`nend;`n") } 'one function that says the kept files'
    Expect-Failure 'that removes nothing after the copy' { param($c) Edit-Script $c "    RemoveStaleIn('qml');" '' } 'removal after the copy lacks'
    Expect-Failure 'that walks a redirected folder' { param($c) Edit-Script $c "  if Redirected then`r`n    Log('Left as it is, a redirection: ' + Relative)`r`n  else`r`n    RemoveStale" '    RemoveStale' } 'removal after the copy lacks'
    Expect-Failure 'that deletes a name ending in a dot' { param($c) Edit-Script $c "(Copy(Found.Name, Length(Found.Name), 1) = '.') or " '' } 'removal after the copy lacks'
    Expect-Failure 'that sets the program aside in every mode' { param($c) Edit-Script $c 'if UpdateMode and FileExists(ProgramPath) then' 'if FileExists(ProgramPath) then' } 'removal after the copy lacks'
    Expect-Failure 'that never puts the program back' { param($c) Edit-Script $c "    PutProgramBack;`r`nend;" "    Log('');`r`nend;" } 'removal after the copy lacks'
    Expect-Failure 'that leaves the aside copy' { param($c) Edit-Script $c "    if SetAside then`r`n      DropAside;" '' } 'removal after the copy lacks'
    Expect-Failure 'that leaves what a setup ended halfway left' { param($c) Edit-Script $c "    RemoveStaleIn('qml');`r`n    RemoveSetupLeftovers;" "    RemoveStaleIn('qml');" } 'removal after the copy lacks'
    Expect-Failure 'whose uninstaller leaves what a setup ended halfway left' { param($c) Edit-Script $c "    RemoveSetupLeftovers;`r`n    DropAside;" '    DropAside;' } 'removal after the copy lacks'
    Expect-Failure 'whose uninstaller leaves the program set aside' { param($c) Edit-Script $c "    RemoveSetupLeftovers;`r`n    DropAside;" '    RemoveSetupLeftovers;' } 'removal after the copy lacks'
    Expect-Failure 'whose uninstall step is never called' { param($c) Edit-Script $c 'procedure CurUninstallStepChanged(' 'procedure CurUninstallStepChange(' } 'removal after the copy lacks'
    Expect-Failure 'that takes any .tmp for what a setup left' { param($c) Edit-Script $c '(Length(Name) = 17) and ' '' } 'removal after the copy lacks'
    Expect-Failure 'that takes any letters for what a setup left' { param($c) Edit-Script $c '  for I := 4 to 13 do' '  for I := 4 to 4 do' } 'removal after the copy lacks'
    Expect-Failure 'that follows a redirection for what a setup left' { param($c) Edit-Script $c '  if ThroughRedirection(Relative) then begin' '  if False then begin' } 'removal after the copy lacks'
    Expect-Failure 'that looks through a redirected folder on the way' { param($c) Edit-Script $c "  while (Rest <> '') and not Result do begin" '  while False do begin' } 'removal after the copy lacks'
    Expect-Failure 'that looks at no folder on the way' { param($c) Edit-Script $c '  Rest := Relative;' "  Rest := '';" } 'removal after the copy lacks'
    Expect-Failure 'that looks at another path on the way' { param($c) Edit-Script $c "    Path := Path + '\' + Copy(Rest, 1, At - 1);" "    Path := ExpandConstant('{userdocs}');" } 'removal after the copy lacks'
    Expect-Failure 'that removes more than what a setup left' { param($c) Edit-Script $c '= 0) and SetupLeftover(Found.Name) then begin' '= 0) then begin' } 'removal after the copy lacks'
    Expect-Failure 'that looks only in the first of its folders' { param($c) Edit-Script $c '  while At > 0 do begin' '  if At > 0 then begin' } 'removal after the copy lacks'
    Expect-Failure 'that stops at the first leftover' { param($c) Edit-Script $c "+ Entry);`r`n      end;`r`n    until not FindNext(Found);" "+ Entry);`r`n      end;`r`n    until True;" } 'removal after the copy lacks'
    Expect-Failure 'that stops at the first stale file' { param($c) Edit-Script $c "+ Name);`r`n        end;`r`n      end;`r`n    until not FindNext(Found);" "+ Name);`r`n        end;`r`n      end;`r`n    until True;" } 'removal after the copy lacks'
    Expect-Failure 'whose install step is never called' { param($c) Edit-Script $c 'procedure CurStepChanged(' 'procedure CurStepChange(' } 'removal after the copy lacks'
    Expect-Failure 'whose list of folders lacks one of the package''s' { param($c) $k = Join-Path $c 'inputs/folders.iss'; [IO.File]::WriteAllText($k, (@([IO.File]::ReadAllLines($k)) | Where-Object { $_ -notmatch 'platforms' }) -join "`n") } 'list of the package''s folders'
    Expect-Failure 'whose list of folders has one the package has not' { param($c) $k = Join-Path $c 'inputs/folders.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("  Result := '|';", "  Result := '|';`n  Result := Result + '..|';")) } 'list of the package''s folders'
    Expect-Failure 'whose list of folders does more than say them' { param($c) $k = Join-Path $c 'inputs/folders.iss'; [IO.File]::AppendAllText($k, "procedure Other;`nbegin`nend;`n") } 'one function that says the package''s folders'
    Expect-Failure 'that removes what a setup left elsewhere' { param($c) Edit-Script $c "        Entry := Folder + '\' + Found.Name;" "        Entry := ExpandConstant('{userdocs}') + '\' + Found.Name;" } 'removal after the copy lacks'
    Expect-Failure 'whose program is not written last' { param($c) $l = Join-Path $c 'inputs/files.iss'; $lines = @([IO.File]::ReadAllLines($l)); [IO.File]::WriteAllLines($l, [string[]] (@($lines[-1]) + $lines[0..($lines.Count - 2)])) } 'program''s file is not the last'
    Expect-Failure 'whose marker comes after the program' { param($c) Edit-Script $c "Source: `"{#SourcePath}qkvm-desktop.installed`"; DestDir: `"{app}`"; Flags: ignoreversion`r`n" ''; Edit-Script $c '#include "files.iss"' "#include `"files.iss`"`r`nSource: `"{#SourcePath}qkvm-desktop.installed`"; DestDir: `"{app}`"; Flags: ignoreversion" } 'the package and the marker'
    Expect-Failure 'that deletes something else' { param($c) Edit-Script $c '    Installing := True;' "    Installing := True;`r`n  DeleteFile(ExpandConstant('{userdocs}\x'));" } 'deletes or renames otherwise'
    Expect-Failure 'that says another 105' { param($c) Edit-Script $c 'qKVM-RESULT: 105 the program was still running after 60 seconds' 'qKVM-RESULT: 105 still running' } 'result line of exit code 105'
    Expect-Failure 'that says 105 after it has ended' { param($c) Edit-Script $c "    Log('qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed');`r`n" '' } 'result line of exit code 105'
    Expect-Failure 'that says nothing of 7' { param($c) Edit-Script $c "    Log('qKVM-RESULT: 7 nothing was changed: ' + Reason);" "    Log('7');" } 'result line of exit code 7'
    Expect-Failure 'that lets its folder be chosen' { param($c) Edit-Script $c 'DisableDirPage=yes' 'DisableDirPage=auto' } 'DisableDirPage=yes'
    Expect-Failure 'that calls something else outside' { param($c) Edit-Script $c 'procedure ExitProcess(ExitCode: Cardinal);' "function MessageBeep(Kind: Cardinal): Boolean;`r`n  external 'MessageBeep@user32.dll stdcall';`r`nprocedure ExitProcess(ExitCode: Cardinal);" } 'calls outside Pascal Script otherwise'
    Expect-Failure 'with another AppId' { param($c) Edit-Script $c 'AppId={{A964EFA6' 'AppId={{B964EFA6' } 'AppId='
    Expect-Failure 'that runs something else' { param($c) Edit-Script $c "[Run]`r`n" "[Run]`r`nFilename: `"{sys}\cmd.exe`"; Flags: nowait`r`n" } 'something else than the program is run'
    Expect-Failure 'with registry entries' { param($c) Edit-Script $c '[Icons]' "[Registry]`r`nRoot: HKCU; Subkey: `"Software\qKVM`"`r`n`r`n[Icons]" } '\[Registry\] section'
    Expect-Failure 'that asks the network' { param($c) Edit-Script $c "[Code]`r`n" "[Code]`r`n// DownloadTemporaryFile`r`n" } 'uses Download'
    # (Its version texts made 0.1.1 in place: they are UTF-16 in the setup's version resource, which is not compressed.)
    Expect-Failure 'of another version' { param($c)
        $file = Join-Path $c 'qKVM-Desktop-0.1.0-windows-x64-setup.exe'
        $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($file))
        $from = [Text.Encoding]::Latin1.GetString([Text.Encoding]::Unicode.GetBytes('0.1.0 '))
        if (-not $text.Contains($from)) { throw 'The setup has no version text to change.' }
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::Latin1.GetBytes($text.Replace($from, [Text.Encoding]::Latin1.GetString([Text.Encoding]::Unicode.GetBytes('0.1.1 '))))) } 'does not say the package''s version'
    Expect-Failure 'that launches the program without --after-update' { param($c) Edit-Script $c 'Parameters: "--after-update"; Description:' 'Description:' } 'something else than the program is run'
    Expect-Failure 'that restarts the program without --after-update' { param($c) Edit-Script $c 'Parameters: "--after-update"; Flags: nowait runasoriginaluser' 'Flags: nowait runasoriginaluser' } 'something else than the program is run'
    Expect-Failure 'that does not look at the room for an update' { param($c) Edit-Script $c 'GetSpaceOnDisk64(' 'GetSpaceOnDisk(' } 'lacks: GetSpaceOnDisk64'
    Expect-Failure 'that does not look for files in use' { param($c) Edit-Script $c 'TFileStream.Create(' 'TStringStream.Create(' } 'lacks: TFileStream'
    Expect-Failure 'that takes an older version' { param($c) Edit-Script $c 'ComparePackedVersion(' 'Max(' } 'lacks: ComparePackedVersion'
    Expect-Failure 'not compiled' { param($c) Remove-Item -LiteralPath (Join-Path $c 'qKVM-Desktop-0.1.0-windows-x64-setup.exe') } 'did not compile'
    # (Inno Setup leaves room in the setup's manifest for the level it writes: another of no more letters takes its place.)
    Expect-Failure 'whose manifest asks for an administrator' { param($c)
        $file = Join-Path $c 'qKVM-Desktop-0.1.0-windows-x64-setup.exe'
        $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($file))
        $from = [regex]::Match($text, 'level="asInvoker" +uiAccess').Value
        if (-not $from) { throw 'The setup''s manifest has no level to change.' }
        $to = 'level="requireAdministrator"'.PadRight($from.Length - 'uiAccess'.Length) + 'uiAccess'
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::Latin1.GetBytes($text.Replace($from, $to))) } 'asks for more than the user'
    Expect-Failure 'whose twin is compressed' { param($c) Copy-Item -LiteralPath (Join-Path $c 'qKVM-Desktop-0.1.0-windows-x64-setup.exe') -Destination (Join-Path $c 'scan/qKVM-Desktop-0.1.0-windows-x64-setup.exe') -Force } 'twin for the byte scan is not readable'
}

# build_setup.ps1's refusals.
$extra = New-FixturePackage 'unrecorded' $fixture
[IO.File]::WriteAllText((Join-Path $extra 'stray.dll'), 'not in the manifest')
Expect-Refusal $extra 'not in its manifest: stray\.dll' 'a package with a file its manifest does not name is refused'
$changed = New-FixturePackage 'changed' $fixture
[IO.File]::AppendAllText((Join-Path $changed 'Qt6Core.dll'), 'x')
Expect-Refusal $changed 'not the bytes its manifest names: Qt6Core\.dll' 'a package with a file of other bytes is refused'
$missing = New-FixturePackage 'missing' $fixture
Remove-Item -LiteralPath (Join-Path $missing 'Qt6Core.dll')
Expect-Refusal $missing 'lacks Qt6Core\.dll' 'a package without a file its manifest names is refused'
$braces = New-FixturePackage 'braces' ($fixture + @{ 'qml/{app}/x.qml' = 'a name Inno Setup reads as a constant' })
Expect-Refusal $braces 'cannot be named to Inno Setup' 'a name Inno Setup would read as something else is refused'
$quote = New-FixturePackage 'quote' ($fixture + @{ "qml/it's.qml" = 'a quote' })
Expect-Refusal $quote 'cannot be named to Inno Setup' 'a name with a quote is refused'
$used = New-FixturePackage 'used' $fixture
New-Item -ItemType Directory -Path (Join-Path $work 'used/out') | Out-Null
[IO.File]::WriteAllText((Join-Path $work 'used/out/old.txt'), 'old')
Expect-Refusal $used 'is not empty' 'an output folder with something in it is refused'
$otherInno = Join-Path $work 'other-inno'
Copy-Item -LiteralPath $Inno -Destination $otherInno -Recurse
[IO.File]::AppendAllText((Join-Path $otherInno 'Default.isl'), "`r`n; changed")
Expect-Refusal (New-FixturePackage 'other-inno' $fixture) 'not the pinned Inno Setup' 'an Inno Setup other than the pinned one is refused' -With $otherInno

# --- Then the setup named.
if ($SetupDirectory) {
    $setup = (Resolve-Path -LiteralPath $SetupDirectory).Path
    $package = (Resolve-Path -LiteralPath $PackageDirectory).Path
    $said = @(Get-SetupFailures $setup $package)
    foreach ($line in $said) { Check $false $line }
    if (-not $said.Count) { Write-Output "The setup in $setup is the package in $package, for the user alone, with the program's mutex and the marker." }
}

if ($failures) { Write-Host "$failures setup check(s) failed."; exit 1 }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Setup checks passed: a package made into a setup passes, and every way of being wrong that is tried fails.'
