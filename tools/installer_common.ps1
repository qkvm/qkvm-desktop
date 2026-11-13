function Invoke-InstallerCheck {
    param([string] $Helper, [ValidateSet('--inspect','--verify','--check-helper')][string] $Mode, [string] $Package = '')
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = (Resolve-Path -LiteralPath $Helper).Path
    $start.ArgumentList.Add($Mode)
    if ($Package) { $start.ArgumentList.Add((Resolve-Path -LiteralPath $Package).Path) }
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.WorkingDirectory = $env:SystemRoot
    $start.Environment['PATH'] = "$env:SystemRoot/System32;$env:SystemRoot"
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'The installer check could not start.' }
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw 'The installer check timed out.'
        }
        $said = $output.GetAwaiter().GetResult() + $errors.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "Installer check failed: $($said.Trim())" }
        return $said.Trim()
    } finally { $process.Dispose() }
}
