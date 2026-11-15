#Requires -Version 5.1
<#
.SYNOPSIS
The VM script's helpers, as a VM runs them: in Windows PowerShell 5.1 under strict mode 2.0, on none, one and several
of what each finds. Nothing of the script's guard or steps runs here, nothing is installed, and nothing is written
outside a scratch folder: CTest runs it in every preset (CMakeLists.txt, test_vm_scripts).

.DESCRIPTION
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\test_vm_scripts.ps1 [-Script <a VM script>] [-LintOnly]

What a PowerShell function returns is unrolled where it is taken: none comes as nothing and one as itself, and under
strict mode neither has a Count. test_upgrade_keeps_vm.ps1 stopped at step 04 on its first VM run for that: one
program running, and $ids.Count of a single number (2026-11-14). So:
  * the lint: every variable set from what a command, a pipeline or a statement gives, and then counted (.Count) or
    indexed in the same function, is one that is taken in @() (or made by New-Object, one object). (A .Length is the
    length of one string, which a command such as Join-Path gives, and is let be.);
  * the helpers: the script's own functions, taken from its syntax tree alone and run here with stand-ins for what
    they ask of Windows (its processes, their windows' titles, cmdkey, a tool), on none, one and two of each; and
    only once every command and method they call is on a list: by bare name, a command of PowerShell's that reaches
    nothing outside the scratch folder, a stand-in or one of them; a method by its name, or a type's by both (review
    012 hold, LOW 5: a list of names alone let cmdkey without .exe, a path, a module's name or .Kill() past it);
    and of those, New-Object only of a type listed, and a command that writes only to a place made of the scratch
    folder, both by their arguments as PowerShell binds them, and again at run time, where every place written to must
    resolve into the scratch folder (review 012 final, LOW 2: a name alone let a listed command make any object or write
    anywhere);
  * the titles of a profile's own views, as step 04 and step 06 expect them, from the fixtures of 0.1.0 and 0.1.1 and
    from profiles made up here;
  * the program ended by force, with none of its windows closed first, by no way at all: the program forgets a window
    a person closed, and the second VM run lost the profile's second window when one was (2026-11-14).
-LintOnly: the lint alone (for a VM script whose helpers are not these).
#>
param(
    [string] $Script = (Join-Path $PSScriptRoot 'test_upgrade_keeps_vm.ps1'),
    [string] $Fixtures = (Join-Path (Split-Path -Parent $PSScriptRoot) 'ui\tests\fixtures\profiles'),
    [switch] $LintOnly
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$failed = New-Object System.Collections.Generic.List[string]
$passedCount = 0
function Check([string] $Name, [scriptblock] $Test) {
    try {
        $said = @(& $Test)
        if ($said.Count) {
            $failed.Add("${Name}: $($said -join '; ')")
            Write-Output "FAILED: ${Name}: $($said -join '; ')"
        } else {
            $script:passedCount++
            Write-Output "ok: $Name"
        }
    } catch {
        $failed.Add("${Name}: threw: $($_.Exception.Message) ($($_.FullyQualifiedErrorId))")
        Write-Output "FAILED: ${Name}: threw: $($_.Exception.Message) ($($_.FullyQualifiedErrorId))"
    }
}

if ($PSVersionTable.PSVersion.Major -ne 5) { throw "Run this in Windows PowerShell 5.1, where the VMs run the script (this is $($PSVersionTable.PSVersion))." }
$Script = (Resolve-Path -LiteralPath $Script).Path
$errors = $null
$tokens = $null
$tree = [System.Management.Automation.Language.Parser]::ParseFile($Script, [ref] $tokens, [ref] $errors)
Check 'the script parses in Windows PowerShell' { @($errors) | ForEach-Object { "line $($_.Extent.StartLineNumber): $($_.Message)" } }

# --- the lint
# What a statement gives is unrolled where it is taken, unless it is a single expression (a variable, a call of a
# method, @(...)): a command, a pipeline of more than one part, an if, a foreach, a switch.
function Test-Unrolled($Statement) {
    if ($Statement -is [System.Management.Automation.Language.PipelineAst]) {
        if ($Statement.PipelineElements.Count -ne 1) { return $true }
        $only = $Statement.PipelineElements[0]
        if ($only -is [System.Management.Automation.Language.CommandAst]) {
            return [string] $only.GetCommandName() -ne 'New-Object'
        }
        return $false
    }
    return $Statement -isnot [System.Management.Automation.Language.CommandExpressionAst]
}
function Get-FunctionOf($Ast) {
    for ($at = $Ast.Parent; $null -ne $at; $at = $at.Parent) {
        if ($at -is [System.Management.Automation.Language.FunctionDefinitionAst]) { return $at }
    }
    return $null
}
function Find-BareCounts($Tree) {
    $assigned = @($Tree.FindAll({ param($node) $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                $node.Operator -eq 'Equals' -and $node.Left -is [System.Management.Automation.Language.VariableExpressionAst] }, $true))
    $counted = @($Tree.FindAll({ param($node)
                ($node -is [System.Management.Automation.Language.MemberExpressionAst] -and
                 $node.Expression -is [System.Management.Automation.Language.VariableExpressionAst] -and
                 $node.Member -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
                 $node.Member.Value -eq 'Count') -or
                ($node -is [System.Management.Automation.Language.IndexExpressionAst] -and
                 $node.Target -is [System.Management.Automation.Language.VariableExpressionAst]) }, $true))
    foreach ($set in $assigned) {
        if (-not (Test-Unrolled $set.Right)) { continue }
        $name = $set.Left.VariablePath.UserPath
        $function = Get-FunctionOf $set
        foreach ($use in $counted) {
            $variable = if ($use -is [System.Management.Automation.Language.MemberExpressionAst]) { $use.Expression } else { $use.Target }
            if ($variable.VariablePath.UserPath -ne $name -or -not [object]::ReferenceEquals((Get-FunctionOf $use), $function)) { continue }
            "line $($set.Extent.StartLineNumber): `$$name is set from what is unrolled, and counted or indexed at line $($use.Extent.StartLineNumber) ($($use.Extent.Text)): take it in @()"
        }
    }
}
Check 'nothing unrolled is counted or indexed bare' { Find-BareCounts $tree }

# (And the lint itself: it sees each way, and lets each safe one be.)
Check 'the lint sees a function''s result counted bare, and lets @() and New-Object be' {
    $sample = @'
function Get-Some { return @(1) }
function One { $ids = Get-Some; if (-not $ids.Count) { return } }
function Two { $ids = @(Get-Some); $ids.Count; $list = New-Object System.Collections.ArrayList; $list.Count }
function Three { $first = Get-Some | Select-Object -First 1; $first[0] }
function Four { $chosen = if ($true) { Get-Some }; $chosen.Count }
function Five { $where = Join-Path 'C:\' 'here'; $where.Length; $text = 'abc'; $text.Count }
'@
    $sampleTree = [System.Management.Automation.Language.Parser]::ParseInput($sample, [ref] $null, [ref] $null)
    $found = @(Find-BareCounts $sampleTree)
    $lines = @($found | ForEach-Object { [int] ($_ -replace '^line (\d+):.*$', '$1') })
    if (($lines -join ',') -ne '2,4,5') { "the lint found lines $($lines -join ',') of the sample, not 2,4,5: $($found -join ' / ')" }
}

if ($LintOnly) {
    Write-Output ''
    if ($failed.Count) { Write-Output "$($failed.Count) of $($failed.Count + $passedCount) checks FAILED"; exit 1 }
    Write-Output "All $passedCount checks passed"; exit 0
}

# --- what the driven functions may call, before any of them is driven
# The functions driven here, and those of the script's own that they call: only these are taken from it. Each may call
# only what is listed: by bare name, PowerShell's own commands that reach nothing outside the scratch folder here, the
# stand-ins below, and these functions; methods by name, and of a type by the type and the name; a command that is
# not named only where listed, in its own function; nothing dot-sourced. Anything else (a program by a path or
# without its .exe, a command named with its module, .Kill(), .CloseMainWindow()) and nothing is driven at all.
$driven = 'Get-ProgramIds', 'Get-Titles', 'Stop-Program', 'Get-ProgramCredentials', 'Invoke-Tool', 'Write-Step', 'Wait-Until',
    'Get-ProfileTitles', 'Test-ProfileViews', 'Get-Hypervisor'
$standInNames = 'Get-CimInstance', 'cmdkey.exe', 'Get-Process', 'Stop-Process', 'taskkill.exe', 'taskkill'
$commandsListed = @('ConvertFrom-Json', 'ConvertTo-Json', 'Copy-Item', 'ForEach-Object', 'Get-ChildItem', 'Get-Content', 'Join-Path',
    'New-Item', 'New-Object', 'Out-Null', 'Set-Content', 'Sort-Object', 'Split-Path', 'Start-Sleep', 'Where-Object', 'Write-Output') +
    @($standInNames | Where-Object { $_ -notlike 'taskkill*' })
$methodsListed = 'Add', 'AddSeconds', 'Dispose', 'ToString', 'ToUniversalTime', 'WaitForExit'
$staticListed = 'Math::Max', 'Math::Min', 'QkvmVmWindows::Titles', 'Threading.Mutex::TryOpenExisting'
$unnamedListed = @{ 'Wait-Until' = @('$Condition'); 'Invoke-Tool' = @('(Join-Path $at (Split-Path -Leaf $Tool))') }
# And what those listed commands may be given (review 012 final, LOW 2: a name alone let New-Object make any object, and
# a file command write anywhere): New-Object only of a type listed (never a COM object); a command that writes only
# to a place made of the scratch folders this sets ($Out, $App), as one of
# them, a Join-Path of one with what is no absolute place, no drive, no "..", no $env:, or a variable of the function's
# own set once from one such; and no argument of either with any of those. The arguments are bound as PowerShell binds
# them (StaticParameterBinder), positional ones too. (At run time the same commands are held to the scratch folder
# again: below.)
$typesListed = 'System.Collections.ArrayList', 'System.Collections.Generic.List[string]'
$scratchRoots = 'Out', 'App'
$writesTo = @{ 'New-Item' = @('Path'); 'Set-Content' = @('Path', 'LiteralPath'); 'Copy-Item' = @('Destination') }
$writersTake = @{ 'New-Item' = @('ItemType', 'Path'); 'Set-Content' = @('Path', 'LiteralPath', 'Value', 'Encoding')
    'Copy-Item' = @('LiteralPath', 'Path', 'Destination') }
function Find-Elsewhere($Ast) {
    $parts = @($Ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.StringConstantExpressionAst] -or
                $node -is [System.Management.Automation.Language.ExpandableStringExpressionAst] -or
                $node -is [System.Management.Automation.Language.VariableExpressionAst] }, $true))
    foreach ($part in $parts) {
        if ($part -is [System.Management.Automation.Language.VariableExpressionAst]) {
            if ($part.VariablePath.IsDriveQualified) { "a drive's variable, $($part.Extent.Text)" }
        } elseif ([string] $part.Value -match '^[A-Za-z]:|^[\\/]|^[A-Za-z][\w.]*::|^~|(^|[\\/])\.\.([\\/]|$)') {
            "an absolute place, a drive's or a way up, $($part.Extent.Text)"
        }
    }
}
function Test-ScratchMade($Ast, $Function, [int] $Depth = 0) {
    if ($Depth -gt 4 -or $null -eq $Ast) { return $false }
    if ($Ast -is [System.Management.Automation.Language.ParenExpressionAst]) { return Test-ScratchMade $Ast.Pipeline $Function ($Depth + 1) }
    if ($Ast -is [System.Management.Automation.Language.PipelineAst]) {
        if ($Ast.PipelineElements.Count -ne 1) { return $false }
        return Test-ScratchMade $Ast.PipelineElements[0] $Function ($Depth + 1)
    }
    if ($Ast -is [System.Management.Automation.Language.CommandExpressionAst]) { return Test-ScratchMade $Ast.Expression $Function ($Depth + 1) }
    if ($Ast -is [System.Management.Automation.Language.VariableExpressionAst]) {
        if ($Ast.VariablePath.IsDriveQualified) { return $false }
        $name = $Ast.VariablePath.UserPath
        # (No parameter of the function, which its caller sets, whatever its name; then the scratch folders; then a
        # variable of the function's own, set once, from a place made so.)
        $parameters = @(@($Function.Parameters) + @(if ($Function.Body.ParamBlock) { $Function.Body.ParamBlock.Parameters }) | Where-Object { $_ })
        if (@($parameters | Where-Object { $_.Name.VariablePath.UserPath -eq $name }).Count) { return $false }
        if ($scratchRoots -contains $name) { return $true }
        $sets = @($Function.Body.FindAll({ param($node) $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                    $node.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
                    $node.Left.VariablePath.UserPath -eq $name }, $true))
        if ($sets.Count -ne 1 -or $sets[0].Operator -ne 'Equals') { return $false }
        return Test-ScratchMade $sets[0].Right $Function ($Depth + 1)
    }
    if ($Ast -is [System.Management.Automation.Language.CommandAst] -and [string] $Ast.GetCommandName() -eq 'Join-Path') {
        $bound = [System.Management.Automation.Language.StaticParameterBinder]::BindCommand($Ast, $true)
        $keys = @($bound.BoundParameters.Keys)
        if ($bound.BindingExceptions.Count -or @($keys | Where-Object { @('Path', 'ChildPath') -notcontains $_ }).Count -or -not ($keys -contains 'Path')) { return $false }
        if ($keys -contains 'ChildPath' -and @(Find-Elsewhere $bound.BoundParameters['ChildPath'].Value).Count) { return $false }
        return Test-ScratchMade $bound.BoundParameters['Path'].Value $Function ($Depth + 1)
    }
    return $false
}
function Find-ArgumentsUnlisted($Call, $Function, [string] $Name) {
    $bound = [System.Management.Automation.Language.StaticParameterBinder]::BindCommand($Call, $true)
    if ($bound.BindingExceptions.Count) {
        # (Not bound, so not looked at as bound: refused, and what its words hold said besides. A place under a drive
        # of no file system, $env: among them, leaves a file system's own parameters, -Encoding, without a meaning.)
        "$Name with arguments that do not bind ($(@($bound.BindingExceptions.Keys) -join ', '))"
        foreach ($element in @($Call.CommandElements | Select-Object -Skip 1)) { foreach ($word in @(Find-Elsewhere $element)) { "$Name is given $word" } }
        return
    }
    $keys = @($bound.BoundParameters.Keys)
    if ($Name -eq 'New-Object') {
        # (A COM object has no type name, and so none listed.)
        $type = if ($keys -contains 'TypeName') { $bound.BoundParameters['TypeName'].Value } else { $null }
        if ($type -isnot [System.Management.Automation.Language.StringConstantExpressionAst] -or -not ($typesListed -contains [string] $type.Value)) {
            "New-Object of $(if ($type) { $type.Extent.Text } else { 'no type' }), which is not listed"
        }
    } else {
        foreach ($key in @($keys | Where-Object { $writersTake[$Name] -notcontains $_ })) { "$Name with -$key" }
        $targets = @($writesTo[$Name] | Where-Object { $keys -contains $_ })
        if (-not $targets.Count) { "$Name with no place it writes to" }
        foreach ($target in $targets) {
            $place = $bound.BoundParameters[$target].Value
            if (-not (Test-ScratchMade $place $Function)) { "$Name writes to $($place.Extent.Text), which is not made of the scratch folder" }
        }
    }
    foreach ($key in $keys) { foreach ($word in @(Find-Elsewhere $bound.BoundParameters[$key].Value)) { "$Name is given $word" } }
}
# What a driven function may not do to the harness that drives it (outside review of 0.1.2, T4: one that set
# $script:scratch made the harness's cleanup remove another folder): set a variable of the script's or the global scope,
# or of a drive ($env: among them), as the harness's own are (by =, by ++ or --); set a part of one that is neither its
# parameter nor its own (an index, a property); or define a function, which would come before the harness's commands.
function Find-HarnessReaches([object[]] $Functions) {
    foreach ($function in $Functions) {
        $parameters = @(@($function.Parameters) + @(if ($function.Body.ParamBlock) { $function.Body.ParamBlock.Parameters }) |
                Where-Object { $_ } | ForEach-Object { $_.Name.VariablePath.UserPath })
        $own = @($function.Body.FindAll({ param($node) $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                    $node.Left -is [System.Management.Automation.Language.VariableExpressionAst] }, $true) |
                ForEach-Object { $_.Left.VariablePath.UserPath })
        $sets = @($function.Body.FindAll({ param($node) $node -is [System.Management.Automation.Language.AssignmentStatementAst] -or
                    ($node -is [System.Management.Automation.Language.UnaryExpressionAst] -and
                     @('PlusPlus', 'MinusMinus', 'PostfixPlusPlus', 'PostfixMinusMinus') -contains [string] $node.TokenKind) }, $true))
        foreach ($set in $sets) {
            $where = "$($function.Name), line $($set.Extent.StartLineNumber)"
            $target = if ($set -is [System.Management.Automation.Language.AssignmentStatementAst]) { $set.Left } else { $set.Child }
            $variables = @($target.FindAll({ param($node) $node -is [System.Management.Automation.Language.VariableExpressionAst] }, $true))
            foreach ($variable in $variables) {
                $path = $variable.VariablePath
                if ($path.IsScript -or $path.IsGlobal -or $path.IsDriveQualified) { "${where}: sets $($variable.Extent.Text), which is not its own" }
            }
            if ($target -isnot [System.Management.Automation.Language.VariableExpressionAst] -and $variables.Count) {
                $base = $variables[0].VariablePath.UserPath
                if (-not ($parameters -contains $base) -and -not ($own -contains $base)) { "${where}: sets a part of `$$base, which is not its own" }
            }
        }
        foreach ($inner in @($function.Body.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true))) {
            "$($function.Name), line $($inner.Extent.StartLineNumber): defines a function of its own, $($inner.Name)"
        }
    }
}
function Find-UnlistedCalls([object[]] $Functions, [string[]] $Commands) {
    foreach ($function in $Functions) {
        $calls = @($function.Body.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -or
                    $node -is [System.Management.Automation.Language.InvokeMemberExpressionAst] }, $true))
        foreach ($call in $calls) {
            $where = "$($function.Name), line $($call.Extent.StartLineNumber)"
            if ($call -is [System.Management.Automation.Language.CommandAst]) {
                $first = $call.CommandElements[0]
                # (A name with a path, or with its module before it, is never one of the bare names listed; nor is a
                # file that is dot-sourced one that is named here.)
                if ($first -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
                    $name = [string] $first.Value
                    if (-not ($Commands -contains $name)) { "${where}: calls $name, which is not listed" }
                    elseif ($name -eq 'New-Object' -or $writesTo.ContainsKey($name)) {
                        foreach ($said in @(Find-ArgumentsUnlisted $call $function $name)) { "${where}: $said" }
                    }
                } elseif ($first -isnot [System.Management.Automation.Language.ScriptBlockExpressionAst]) {
                    $allowed = @(if ($unnamedListed.ContainsKey($function.Name)) { $unnamedListed[$function.Name] })
                    if (-not ($allowed -ccontains $first.Extent.Text)) { "${where}: calls $($first.Extent.Text), which is not named" }
                }
            } else {
                if ($call.Member -isnot [System.Management.Automation.Language.StringConstantExpressionAst]) { "${where}: calls a method that is not named: $($call.Extent.Text)"; continue }
                $method = [string] $call.Member.Value
                if ($call.Static -and $call.Expression -is [System.Management.Automation.Language.TypeExpressionAst]) {
                    $key = "$($call.Expression.TypeName.FullName)::$method"
                    if (-not ($staticListed -contains $key)) { "${where}: calls $key, which is not listed" }
                } elseif (-not ($methodsListed -contains $method)) { "${where}: calls .$method(), which is not listed" }
            }
        }
    }
}
$defined = @{}
foreach ($one in @($tree.EndBlock.Statements | Where-Object { $_ -is [System.Management.Automation.Language.FunctionDefinitionAst] })) { $defined[$one.Name] = $one }
$closure = New-Object System.Collections.Generic.List[string]
$toTake = New-Object System.Collections.Generic.Queue[string]
foreach ($name in $driven) { $toTake.Enqueue($name) }
while ($toTake.Count) {
    $name = $toTake.Dequeue()
    if ($closure.Contains($name)) { continue }
    if (-not $defined.ContainsKey($name)) { continue }
    $closure.Add($name)
    foreach ($call in @($defined[$name].Body.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] }, $true))) {
        $called = [string] $call.GetCommandName()
        if ($called -and $defined.ContainsKey($called)) { $toTake.Enqueue($called) }
    }
}
$functions = @($closure | ForEach-Object { $defined[$_] })
$unlisted = @(Find-UnlistedCalls $functions (@($commandsListed) + @($closure)))
$reaching = @(Find-HarnessReaches $functions)
Check 'the script defines every function driven here' { @($driven | Where-Object { -not $defined.ContainsKey($_) } | ForEach-Object { "$_ is not in the script" }) }
Check 'the functions driven here call only what is listed' { $unlisted }
Check 'the functions driven here leave the harness''s own alone' { $reaching }
# (And that guard itself: it sees each way at the harness's own, and lets a function's own and its parameters be.)
Check 'the guard sees every way at the harness''s own' {
    $sample = @'
function A { $script:scratch = 'elsewhere' }
function B { $global:scratch = 'elsewhere' }
function C { $env:TEMP = 'elsewhere' }
function D { $script:count++ }
function E { function Remove-Item { } }
function F { $failed[0] = 'x' }
function G { $results.Count = 0 }
function H { [string] $script:scratch = 'elsewhere' }
function I { param([System.Collections.ArrayList] $List) $mine = @(1); $mine[0] = 2; $List[0] = 3; $n = 1; $n++; $t = @{}; $t.k = 1 }
'@
    $sampleTree = [System.Management.Automation.Language.Parser]::ParseInput($sample, [ref] $null, [ref] $null)
    $sampleFunctions = @($sampleTree.EndBlock.Statements | Where-Object { $_ -is [System.Management.Automation.Language.FunctionDefinitionAst] })
    $found = @(Find-HarnessReaches $sampleFunctions)
    $names = @($found | ForEach-Object { ($_ -split ',')[0] } | Select-Object -Unique)
    if (($names -join '') -cne 'ABCDEFGH') { "the guard found $($names -join ''), not ABCDEFGH: $($found -join ' / ')" }
}
# (And the guard itself: it sees each way past a list of names, and lets each listed one be.)
Check 'the guard sees every call that is not listed' {
    $sample = @'
function A { & cmdkey /list }
function B { & 'C:\Windows\System32\cmdkey.exe' /list }
function C { Microsoft.PowerShell.Management\Stop-Process -Id 1 -Force }
function D { $p = Get-Process -Id 1; $p.Kill() }
function E { [Diagnostics.Process]::GetProcessById(1) }
function F { & $anything }
function G { . $PSScriptRoot\other.ps1 }
function H { $p = Get-Process -Id 1; [void] $p.CloseMainWindow(); [void] $p.WaitForExit(1); Get-ChildItem | Where-Object { $_ } }
function I { $p = Get-Process -Id 1; $p.$which() }
function J { $list = New-Object System.Collections.ArrayList; [void] $list.Add(1); [Math]::Max(1, 2); & { Start-Sleep -Milliseconds 1 } }
function K { taskkill /PID 1 }
function L { $held = New-Object Threading.Mutex($true, 'Local\QKVM Desktop') }
function M { 'x' | Set-Content -LiteralPath "$env:LOCALAPPDATA\QKVM Desktop\settings.json" }
function N { New-Item -ItemType Directory -Path 'Z:\elsewhere\x' }
function O { Copy-Item -LiteralPath a -Destination (Join-Path $Out '..\..\x') }
function P { Copy-Item -LiteralPath a }
function Q { $shell = New-Object -ComObject WScript.Shell }
function R { param([string] $Folder) $at = Join-Path $Out $Folder; New-Item -ItemType Directory -Path $at | Out-Null; Copy-Item -LiteralPath $Folder -Destination $at; 'a' | Set-Content -LiteralPath (Join-Path $App 'b.txt') -Encoding UTF8; $list = New-Object System.Collections.ArrayList; $names = New-Object System.Collections.Generic.List[string] }
function S { $where = Join-Path $HOME 'x'; New-Item -ItemType Directory -Path $where }
function T { $at = Join-Path $Out 'x'; $at = 'elsewhere'; New-Item -ItemType Directory -Path $at }
function U { param([string] $Out) New-Item -ItemType Directory -Path $Out }
function V { New-Item -ItemType SymbolicLink -Path (Join-Path $Out 'x') -Target $Elsewhere }
function W { New-Item -ItemType Directory -Path (Join-Path $Out $env:USERNAME) }
'@
    $sampleTree = [System.Management.Automation.Language.Parser]::ParseInput($sample, [ref] $null, [ref] $null)
    $sampleFunctions = @($sampleTree.EndBlock.Statements | Where-Object { $_ -is [System.Management.Automation.Language.FunctionDefinitionAst] })
    $found = @(Find-UnlistedCalls $sampleFunctions $commandsListed)
    $names = @($found | ForEach-Object { ($_ -split ',')[0] } | Select-Object -Unique)
    if (($names -join '') -cne 'ABCDEFGHIKLMNOPQSTUVW') { "the guard found $($names -join ''), not ABCDEFGHIKLMNOPQSTUVW: $($found -join ' / ')" }
}
if ($unlisted.Count -or $reaching.Count -or @($driven | Where-Object { -not $defined.ContainsKey($_) }).Count) {
    Write-Output ''
    Write-Output "Nothing was driven: the functions call what is not listed. $($failed.Count) of $($failed.Count + $passedCount) checks FAILED"
    exit 1
}

# --- the helpers, with stand-ins for what they ask of Windows
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("qkvm-test-vm-scripts-{0}-{1}" -f $PID, [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $scratch | Out-Null
# The one folder this removes at its end, fixed now that it is made in Temp, as constants nothing in this run can set
# again: where it is, and a mark of its own written in it (outside review of 0.1.2, T4).
New-Variable -Name madeScratch -Value ([IO.Path]::GetFullPath($scratch)) -Option Constant -Scope Script
New-Variable -Name madeMark -Value ([Guid]::NewGuid().ToString('N')) -Option Constant -Scope Script
[IO.File]::WriteAllText((Join-Path $madeScratch 'qkvm-test-vm-scripts.mark'), $madeMark)
# Whether `Folder` is that one still: the folder made (and so in Temp, of this test's name), with its own mark in it,
# which another folder put in its place has not.
function Test-MadeScratch([string] $Folder) {
    $full = [IO.Path]::GetFullPath($Folder)
    $mark = Join-Path $full 'qkvm-test-vm-scripts.mark'
    return $full -ceq $madeScratch -and (Test-Path -LiteralPath $mark -PathType Leaf) -and
        ([IO.File]::ReadAllText($mark) -ceq $madeMark)
}
# At run time too, every place a command writes to is the scratch folder's, or it is refused before anything is written
# (review 012 final, LOW 2): these stand in for New-Item, Set-Content and Copy-Item from here on, for the driven
# functions and for this test alike, resolve each place in full ("..", a relative place and all), and only then hand on
# to PowerShell's own. Every refusal is kept, so that one a function swallows is seen at the end.
$script:scratchRoot = [IO.Path]::GetFullPath($scratch).TrimEnd('\') + '\'
$script:outside = New-Object System.Collections.ArrayList
function Assert-InScratch([object[]] $Places, [string] $By) {
    $given = @($Places | Where-Object { $_ })
    if (-not $given.Count) { [void] $script:outside.Add("$By with no place"); throw "$By with no place it writes to: refused" }
    foreach ($place in $given) {
        $full = [IO.Path]::GetFullPath([string] $place)
        if (-not $full.StartsWith($script:scratchRoot, [StringComparison]::OrdinalIgnoreCase)) {
            [void] $script:outside.Add("$By $full")
            throw "$By would write to $full, which is outside the scratch folder: refused"
        }
    }
}
function New-Item([string] $ItemType, [string[]] $Path) {
    Assert-InScratch @($Path) 'New-Item'
    Microsoft.PowerShell.Management\New-Item @PSBoundParameters
}
function Copy-Item([string[]] $LiteralPath, [string[]] $Path, [string] $Destination) {
    Assert-InScratch @($Destination) 'Copy-Item'
    Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
}
function Set-Content {
    param([Parameter(ValueFromPipeline = $true)] [object[]] $Value, [string[]] $LiteralPath, [string[]] $Path, [string] $Encoding)
    begin { Assert-InScratch (@($LiteralPath) + @($Path)) 'Set-Content'; $all = New-Object System.Collections.ArrayList }
    process { foreach ($one in @($Value)) { [void] $all.Add($one) } }
    end {
        $given = @{ Value = @($all) }
        foreach ($key in 'LiteralPath', 'Path', 'Encoding') { if ($PSBoundParameters.ContainsKey($key)) { $given[$key] = $PSBoundParameters[$key] } }
        Microsoft.PowerShell.Management\Set-Content @given
    }
}
try {
    Add-Type -TypeDefinition @'
public static class QkvmVmWindows {
    public static string[] Next = new string[0];
    public static uint[] Asked = new uint[0];
    public static string[] Titles(uint[] processes) { Asked = processes; return Next; }
}
'@
    # What the functions read of the script's own: where the program is, its mutex (one that is nowhere), the folders.
    $Program = Join-Path $scratch 'Programs\qKVM Desktop\qkvm-desktop.exe'
    $ProgramMutex = "Local\qkvm-test-vm-scripts-$([Guid]::NewGuid().ToString('N'))"
    $App = Join-Path $scratch 'app'
    $Out = Join-Path $scratch 'out'
    New-Item -ItemType Directory -Path $App, $Out | Out-Null
    Set-Content -LiteralPath (Join-Path $App 'a.dll') -Value 'not a library' -Encoding ASCII
    $results = New-Object System.Collections.ArrayList

    # The driven functions alone, as the guard above saw them: the script's guard, its steps, its own Add-Type and every
    # other function of it never run.
    . ([scriptblock]::Create((@($functions | ForEach-Object { $_.Extent.Text }) -join "`n`n")))

    # Stand-ins, found before the commands of the same names (a function comes before a cmdlet or a program).
    $script:processes = @()
    $script:credentialLines = @()
    # (What was ended, by id, and whether by force; and every window closed, by any way: a close is what a person does.)
    $script:stopped = New-Object System.Collections.ArrayList
    $script:closed = New-Object System.Collections.ArrayList
    # (Whether a program that was ended is gone within the wait: as Windows does it, unless a case says otherwise.)
    $script:endsWhenEnded = $true
    function Get-CimInstance { $script:processes }
    function cmdkey.exe { $script:credentialLines }
    function Get-Process([int] $Id) {
        $one = New-Object psobject
        $one | Add-Member -MemberType NoteProperty -Name Id -Value $Id
        $one | Add-Member -MemberType ScriptMethod -Name CloseMainWindow -Value { [void] $script:closed.Add("CloseMainWindow ($($this.Id))"); $true }
        $one | Add-Member -MemberType ScriptMethod -Name Close -Value { [void] $script:closed.Add("Close ($($this.Id))") }
        $one | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value { param([int] $Ms)
            $script:endsWhenEnded -and @($script:stopped | Where-Object { $_.Id -eq $this.Id }).Count -gt 0 }
        return $one
    }
    function Stop-Process([int] $Id, [switch] $Force) { [void] $script:stopped.Add([pscustomobject] @{ Id = $Id; Force = [bool] $Force }) }
    function taskkill.exe { [void] $script:closed.Add("taskkill $($args -join ' ')") }
    function taskkill { [void] $script:closed.Add("taskkill $($args -join ' ')") }
    # (Odd ids: Windows gives every process a multiple of four, so none of these is any real one's.)
    function Set-Programs([int] $Count) {
        $made = @(for ($at = 1; $at -le $Count; $at++) { [pscustomobject] @{ ExecutablePath = $Program; ProcessId = 4001 + 2 * $at } })
        # (And one of another program of the same name, which is never the program's.)
        $script:processes = @($made) + @([pscustomobject] @{ ExecutablePath = 'C:\elsewhere\qkvm-desktop.exe'; ProcessId = 9 })
    }
    # Only with every stand-in the one found: a real Stop-Process would end whatever runs under a made-up id, a real
    # taskkill would ask it to close, and a real cmdkey would list this PC's own Credential Manager.
    $standIns = @(@($standInNames) + @('New-Item', 'Set-Content', 'Copy-Item') | Where-Object { (Get-Command $_).CommandType -ne 'Function' })
    Check 'the stand-ins are the commands found' { @($standIns | ForEach-Object { "$_ is not the stand-in here" }) }
    if ($standIns.Count) { throw "The stand-ins are not the commands found: $($standIns -join ', '). Nothing was driven." }

    foreach ($count in 0, 1, 2) {
        Check "Get-ProgramIds with $count of the program running" {
            Set-Programs $count
            $ids = @(Get-ProgramIds)
            if ($ids.Count -ne $count) { "$($ids.Count) ids, not $count" }
        }
        Check "Get-Titles with $count of the program running" {
            Set-Programs $count
            [QkvmVmWindows]::Next = [string[]] @(for ($at = 1; $at -le $count; $at++) { "window $at - qKVM" })
            $titles = @(Get-Titles)
            if ($titles.Count -ne $count) { "$($titles.Count) titles, not $count" }
        }
        # The program is ended as a person never ends it, by force, and none of its windows is closed first: the program
        # forgets a window a person closed, and the second VM run lost the profile's second window so (2026-11-14).
        Check "Stop-Program ends each of $count by force, and closes no window" {
            Set-Programs $count
            $script:stopped.Clear()
            $script:closed.Clear()
            $how = @(Stop-Program)
            if ($script:closed.Count) { "it closed: $($script:closed -join ', ')" }
            $forced = @($script:stopped | Where-Object { $_.Force })
            if ($script:stopped.Count -ne $count -or $forced.Count -ne $count) { "$($script:stopped.Count) ended, $($forced.Count) by force, not $count" }
            if ($how.Count -ne $count -or @($how | Where-Object { $_ -notlike 'ended by force, no window closed first (*)' }).Count) { "it said: $($how -join ' / ')" }
        }
        Check "Get-ProgramCredentials with $count of the program's" {
            $script:credentialLines = @('', 'Currently stored credentials:', '    Target: LegacyGeneric:target=somebody else') +
                @(for ($at = 1; $at -le $count; $at++) { "    Target: LegacyGeneric:target=QKVM Desktop: kvm:$at"; "    Target: LegacyGeneric:target=QKVM Desktop: kvm:$at" })
            $names = @(Get-ProgramCredentials)
            if ($names.Count -ne $count) { "$($names.Count) names, not ${count}: $($names -join ', ')" }
        }
        Check "Invoke-Tool with a tool that says $count lines" {
            $tool = Join-Path $scratch "tool-$count.cmd"
            $lines = @('@echo off') + @(for ($at = 1; $at -le $count; $at++) { "echo line $at" }) + @('exit /b 3')
            Set-Content -LiteralPath $tool -Value $lines -Encoding ASCII
            $ran = Invoke-Tool $tool "tool-$count" @('--disposable')
            $said = @($ran.Said)
            if ($ran.Exit -ne 3) { "it ended with $($ran.Exit), not 3" }
            if ($said.Count -ne $count) { "$($said.Count) lines said, not $count" }
            if (-not (Test-Path -LiteralPath (Join-Path $Out "tool-$count\a.dll"))) { "the program's libraries were not put beside it" }
        }
        Check "Write-Step with $count failures" {
            $failures = New-Object System.Collections.ArrayList
            for ($at = 1; $at -le $count; $at++) { [void] $failures.Add("failure $at") }
            $line = @(Write-Step "9$count" 'step' ([DateTime]::UtcNow) $failures @{ seen = 1 })
            $written = Get-Content -LiteralPath (Join-Path $Out "9$count-step.json") -Raw | ConvertFrom-Json
            if ([bool] $written.passed -ne ($count -eq 0) -or @($written.failures).Count -ne $count) { "it wrote passed=$($written.passed) with $(@($written.failures).Count) failures" }
            if ($line.Count -ne 1) { "it said $($line.Count) lines" }
        }
    }
    Check 'Invoke-Tool with a tool that writes to its error stream' {
        $tool = Join-Path $scratch 'tool-error.cmd'
        Set-Content -LiteralPath $tool -Value @('@echo off', 'echo on the output', 'echo on the error stream 1>&2', 'exit /b 4') -Encoding ASCII
        $ran = Invoke-Tool $tool 'tool-error' @('--disposable')
        $said = @($ran.Said)
        if ($ran.Exit -ne 4) { "it ended with $($ran.Exit), not 4" }
        if (($said -join ' / ') -notlike '*on the output*' -or ($said -join ' / ') -notlike '*on the error stream*') { "it said: $($said -join ' / ')" }
    }
    Check 'Stop-Program says so where the program is not gone after it was ended' {
        Set-Programs 1
        $script:stopped.Clear()
        $script:closed.Clear()
        $script:endsWhenEnded = $false
        try { $how = @(Stop-Program) } finally { $script:endsWhenEnded = $true }
        if ($script:closed.Count) { "it closed: $($script:closed -join ', ')" }
        if ($how.Count -ne 1 -or $how[0] -notlike 'ended by force, and still running * s later (*)') { "it said: $($how -join ' / ')" }
    }
    # (And by its text: no way of closing a window is anywhere in the script but its comments.)
    Check 'nothing in the script closes a window of the program' {
        $closing = 'CloseMainWindow', 'taskkill', 'WM_CLOSE', 'PostMessage', 'SendMessage', '.Close('
        $text = (@($tokens | Where-Object { $_.Kind -ne 'Comment' } | ForEach-Object { $_.Text }) -join ' ')
        @($closing | Where-Object { $text.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 } | ForEach-Object { "the script holds $_" })
    }
    Check 'Wait-Until, on what comes and on what never does' {
        if (-not (Wait-Until { $true } 1)) { 'it did not see what is so' }
        if (Wait-Until { $false } 1) { 'it saw what is not so' }
    }

    # --- the titles of a profile's own views (steps 04 and 06)
    foreach ($version in '0.1.0', '0.1.1') {
        Check "the titles of $version's views are the program's" {
            $titles = @(Get-ProfileTitles (Join-Path $Fixtures "$version\profile\settings.json"))
            $expected = @('Rack A - This PC - qKVM', '127.0.0.1:47391 - qKVM')
            if (($titles -join ' | ') -cne ($expected -join ' | ')) { "Get-ProfileTitles says $($titles -join ' | '), not $($expected -join ' | ')" }
        }
    }
    function Write-Profile([string] $Name, [string] $Json) {
        $file = Join-Path $scratch "$Name.json"
        Set-Content -LiteralPath $file -Value $Json -Encoding UTF8
        return $file
    }
    Check 'the titles of profiles made up here' {
        $cases = @(
            @('no windows, This PC', '{"own_wall": true, "server": "http://127.0.0.1:47391/"}', 'This PC - qKVM'),
            @('no windows, a server alone', '{"own_wall": false, "server": "https://198.51.100.40:8443/"}', '198.51.100.40:8443 - qKVM'),
            @('a window that remembers no source', '{"own_wall": false, "server": "http://127.0.0.1:47391/", "windows": [{"group": "Rack A", "source": ""}]}', '127.0.0.1:47391 - qKVM'),
            @('a group no KVM has', '{"kvms": [{"group": "Desk"}], "windows": [{"group": "Gone", "source": "pc"}]}', 'This PC - qKVM'),
            @('a group a KVM has', '{"kvms": [{"group": "Desk"}, {"name": "no group"}], "windows": [{"group": "Desk", "source": "pc"}]}', 'Desk - This PC - qKVM'),
            @('one window', '{"windows": [{"group": "", "source": "http://127.0.0.1:47391/"}]}', '127.0.0.1:47391 - qKVM'))
        foreach ($case in $cases) {
            $titles = @(Get-ProfileTitles (Write-Profile 'made-up' $case[1]))
            if (($titles -join ' | ') -cne $case[2]) { "$($case[0]): $($titles -join ' | '), not $($case[2])" }
        }
        $many = '{"windows": [' + ((1..10 | ForEach-Object { '{"group": "", "source": "pc"}' }) -join ', ') + ']}'
        if (@(Get-ProfileTitles (Write-Profile 'many' $many)).Count -ne 8) { 'more than eight windows were foretold' }
        $odd = @(Get-ProfileTitles (Write-Profile 'odd' '{"windows": [{"group": "", "source": "connect"}]}'))
        if ($odd.Count -ne 1 -or $odd[0] -like '* - qKVM') { "a window on ""connect"" was foretold: $($odd -join ' | ')" }
    }
    # Step 04 as the first VM run saw the program (2026-11-14: the window on the stand-in server in front, This PC's
    # behind it), and as it must not pass.
    # (The steps themselves are not run here: what they ask is read from the syntax tree. Steps 04 and 06 each ask for
    # the fixture's views, and step 00 has them from the fixture's own settings.)
    Check 'steps 04 and 06 end the program, and fail where it still runs' {
        $ends = @($tree.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and
                    [string] $node.GetCommandName() -eq 'Stop-Program' -and $null -eq (Get-FunctionOf $node) }, $true))
        $told = @($tree.FindAll({ param($node) $node -is [System.Management.Automation.Language.IfStatementAst] -and $null -eq (Get-FunctionOf $node) -and
                    $node.Clauses[0].Item1.Extent.Text -eq 'Test-ProgramRunning' -and $node.Clauses[0].Item2.Extent.Text -like '*$failures.Add(*still runs after it was ended*' }, $true))
        if ($ends.Count -ne 2 -or $told.Count -ne 2) { "the steps end it $($ends.Count) times and fail for it still running $($told.Count) times, not 2 and 2" }
    }
    Check 'steps 04 and 06 expect the fixture''s own views' {
        $asked = @($tree.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and
                    @('Test-ProfileViews', 'Test-AtTheWall') -contains [string] $node.GetCommandName() -and $null -eq (Get-FunctionOf $node) }, $true))
        $right = @($asked | Where-Object { [string] $_.GetCommandName() -eq 'Test-ProfileViews' -and $_.CommandElements.Count -ge 3 -and
                    $_.CommandElements[2].Extent.Text -eq '$viewTitles' })
        if ($asked.Count -ne 2 -or $right.Count -ne 2) { "the steps ask $(@($asked | ForEach-Object { $_.Extent.Text }) -join ' / ')" }
        $made = @($tree.FindAll({ param($node) $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                    $node.Left.Extent.Text -eq '$viewTitles' }, $true))
        if ($made.Count -ne 1 -or $made[0].Right.Extent.Text -notmatch "Get-ProfileTitles \(Join-Path \`$Fixture 'profile\\settings\.json'\)") { "the views are not the fixture's: $(@($made | ForEach-Object { $_.Extent.Text }) -join ' / ')" }
    }
    function Get-Views { return @(Get-ProfileTitles (Join-Path $Fixtures '0.1.1\profile\settings.json')) }
    Set-Programs 1
    Check 'step 04 passes the program at its views' {
        $views = @(Get-Views)
        [QkvmVmWindows]::Next = [string[]] @('127.0.0.1:47391 - qKVM', 'Rack A - This PC - qKVM')
        $failures = New-Object System.Collections.ArrayList
        $titles = @(Test-ProfileViews $failures $views 1)
        if ($failures.Count) { "it failed: $($failures -join '; ')" }
        if ($titles.Count -ne 2) { "it gave $($titles.Count) titles" }
    }
    Check 'step 04 fails where a view is missing' {
        $views = @(Get-Views)
        [QkvmVmWindows]::Next = [string[]] @('127.0.0.1:47391 - qKVM')
        $failures = New-Object System.Collections.ArrayList
        [void] @(Test-ProfileViews $failures $views 1)
        if ($failures.Count -ne 1 -or $failures[0] -notlike '*"Rack A - This PC - qKVM"*') { "it said: $($failures -join '; ')" }
    }
    Check 'step 04 fails where a box asks for the app password or a reset' {
        $views = @(Get-Views)
        [QkvmVmWindows]::Next = [string[]] @('qKVM')
        $failures = New-Object System.Collections.ArrayList
        [void] @(Test-ProfileViews $failures $views 1)
        if (@($failures | Where-Object { $_ -like '*titled "qKVM" alone*' }).Count -ne 1) { "it said: $($failures -join '; ')" }
    }
    Check 'step 04 fails where the program has no window, and where it is not running' {
        $views = @(Get-Views)
        foreach ($count in 0, 1) {
            Set-Programs $count
            [QkvmVmWindows]::Next = [string[]] @()
            $failures = New-Object System.Collections.ArrayList
            $titles = @(Test-ProfileViews $failures $views 1)
            if ($failures.Count -ne 1 -or $titles.Count) { "with $count running it said: $($failures -join '; ')" }
        }
    }
    # A virtual machine is one whose firmware says so (outside review of 0.1.2, T5: the marker alone proves none): QEMU,
    # VMware, Hyper-V, VirtualBox, and no PC, of the same maker as a hypervisor or not.
    Check 'only firmware that names a hypervisor makes a virtual machine' {
        $cases = @(
            @('QEMU', 'Standard PC (Q35 + ICH9, 2009)', 'QEMU'),
            @('VMware, Inc.', 'VMware Virtual Platform', 'VMware'),
            @('Microsoft Corporation', 'Virtual Machine', 'Hyper-V'),
            @('innotek GmbH', 'VirtualBox', 'VirtualBox'),
            @('Microsoft Corporation', 'A laptop', ''),
            @('A maker of PCs', 'A desk PC', ''),
            @('', '', ''))
        foreach ($case in $cases) {
            $named = [string] (Get-Hypervisor $case[0] $case[1])
            if ($named -cne $case[2]) { "$($case[0]) / $($case[1]): '$named', not '$($case[2])'" }
        }
    }
    # (And the script asks it of Windows' own key, and refuses before it does anything else: read from its syntax tree.)
    Check 'the script refuses a machine whose firmware names no hypervisor, before it does anything' {
        $statements = @($tree.EndBlock.Statements)
        $readAt = -1; $refusedAt = -1; $doneAt = $statements.Count
        for ($at = 0; $at -lt $statements.Count; $at++) {
            $one = $statements[$at]
            if ($readAt -lt 0 -and $one -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                $one.Left.Extent.Text -eq '$hypervisor' -and $one.Right.Extent.Text -like 'Get-Hypervisor *') { $readAt = $at }
            if ($refusedAt -lt 0 -and $one -is [System.Management.Automation.Language.IfStatementAst] -and
                $one.Clauses[0].Item1.Extent.Text -eq '-not $hypervisor' -and $one.Clauses[0].Item2.Extent.Text -like '*throw *') { $refusedAt = $at }
            $does = @($one.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and
                        @('Invoke-Setup', 'Invoke-Tool', 'Start-Process', 'New-Item', 'Write-Step') -contains [string] $node.GetCommandName() }, $true))
            if ($does.Count -and $one -isnot [System.Management.Automation.Language.FunctionDefinitionAst] -and $at -lt $doneAt) { $doneAt = $at }
        }
        if ($readAt -lt 0 -or $refusedAt -lt $readAt -or $refusedAt -ge $doneAt) { "the firmware is read at $readAt, the refusal at $refusedAt, the first thing done at $doneAt" }
        if ($tree.Extent.Text.IndexOf("Get-ItemProperty -LiteralPath 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS'", [StringComparison]::Ordinal) -lt 0) { 'the firmware is not read from Windows'' own key' }
    }
    # The folder removed at the end is the one made and no other: fixed when it was made, and asked again before.
    Check 'the end removes only the folder made, as it was made' {
        if (-not (Test-MadeScratch $madeScratch)) { 'the folder made is not taken for itself' }
        if (-not ((Get-Variable -Name madeScratch -Scope Script).Options -band [System.Management.Automation.ScopedItemOptions]::Constant)) { 'where it is can be set again' }
        # (Its mark gone, as from a folder put in its place: not.)
        $mark = Join-Path $madeScratch 'qkvm-test-vm-scripts.mark'
        Microsoft.PowerShell.Management\Rename-Item -LiteralPath $mark -NewName 'qkvm-test-vm-scripts.mark.away'
        try {
            if (Test-MadeScratch $madeScratch) { 'the folder made, its mark gone, was taken for itself' }
        } finally {
            Microsoft.PowerShell.Management\Rename-Item -LiteralPath "$mark.away" -NewName 'qkvm-test-vm-scripts.mark'
        }
        $beside = Join-Path ([IO.Path]::GetTempPath()) "qkvm-test-vm-scripts-beside-$([Guid]::NewGuid().ToString('N'))"
        Microsoft.PowerShell.Management\New-Item -ItemType Directory -Path $beside | Out-Null
        try {
            if (Test-MadeScratch $beside) { 'a folder beside it, with no mark, was taken for it' }
            [IO.File]::WriteAllText((Join-Path $beside 'qkvm-test-vm-scripts.mark'), 'another mark')
            if (Test-MadeScratch $beside) { 'a folder with another mark was taken for it' }
            [IO.File]::WriteAllText((Join-Path $beside 'qkvm-test-vm-scripts.mark'), $madeMark)
            if (Test-MadeScratch $beside) { 'a folder with its mark, but another, was taken for it' }
        } finally {
            Microsoft.PowerShell.Management\Remove-Item -LiteralPath $beside -Recurse -Force
        }
        if (Test-MadeScratch (Join-Path $madeScratch 'app')) { 'a folder in it was taken for it' }
    }
    # Everything the driven functions wrote, they wrote in the scratch folder (a refusal they swallowed is kept).
    Check 'nothing was written outside the scratch folder' { @($script:outside | ForEach-Object { "refused: $_" }) }
    # (And the stand-ins themselves refuse every way out: a place beside it, one by "..", none at all; nothing is made.)
    Check 'the file commands refuse every place outside the scratch folder' {
        $away = Join-Path ([IO.Path]::GetTempPath()) "qkvm-test-vm-scripts-outside-$([Guid]::NewGuid().ToString('N'))"
        $up = Join-Path $Out "..\..\qkvm-test-vm-scripts-up-$([Guid]::NewGuid().ToString('N'))"
        $tries = @(
            { New-Item -ItemType Directory -Path $away },
            { New-Item -ItemType Directory -Path $up },
            { 'x' | Set-Content -LiteralPath $away },
            { Set-Content -Value 'x' },
            { Copy-Item -LiteralPath (Join-Path $App 'a.dll') -Destination $away },
            { Copy-Item -LiteralPath (Join-Path $App 'a.dll') })
        foreach ($try in $tries) {
            $refused = $false
            try { & $try | Out-Null } catch { $refused = $true }
            if (-not $refused) { "it let { $($try.ToString().Trim()) } be" }
        }
        foreach ($made in $away, [IO.Path]::GetFullPath($up)) {
            if (Test-Path -LiteralPath $made) { "it made $made"; Microsoft.PowerShell.Management\Remove-Item -LiteralPath $made -Recurse -Force }
        }
        if ($script:outside.Count -ne $tries.Count) { "it kept $($script:outside.Count) refusals, not $($tries.Count)" }
        $script:outside.Clear()
    }
} finally {
    # Only the folder made above, and only where it is that still (Test-MadeScratch). (PowerShell's own command, by its
    # module: no function of this run comes before it.)
    if (Test-MadeScratch $madeScratch) {
        Microsoft.PowerShell.Management\Remove-Item -LiteralPath $madeScratch -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        $failed.Add("the scratch folder was not removed: $madeScratch is not as this test made it")
        Write-Output "NOT REMOVED: $madeScratch is not as this test made it"
    }
}

Write-Output ''
if ($failed.Count) { Write-Output "$($failed.Count) of $($failed.Count + $passedCount) checks FAILED"; exit 1 }
Write-Output "All $passedCount checks passed"
exit 0
