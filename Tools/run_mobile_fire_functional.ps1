param(
    [ValidateRange(1, 5000)]
    [int]$Units = 64,

    [ValidateRange(2.0, 600.0)]
    [double]$TestSeconds = 8.0,

    [string]$UnrealEditorCmd = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [string]$Project,
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
if (-not $Project) {
    $Project = [System.IO.Path]::GetFullPath((Join-Path $PluginRoot '..\..\Winyunq.uproject'))
}
if (-not $LogPath) {
    $ProjectRoot = Split-Path -Parent $Project
    $LogPath = Join-Path $ProjectRoot "Saved\Logs\MBST_MobileFire_Functional_${Units}.log"
}

if (-not (Test-Path -LiteralPath $UnrealEditorCmd)) {
    throw "UnrealEditor-Cmd was not found: $UnrealEditorCmd"
}
if (-not (Test-Path -LiteralPath $Project)) {
    throw "Project was not found: $Project"
}

$Arguments = @(
    $Project,
    '/MassBattleSingleTurret/Demo/MobileFire/Map_MBST_MobileFire',
    '-game',
    '-unattended',
    '-nullrhi',
    '-DisablePlugins=MassBattleEditorMCP',
    '-MBSTMobileFireAutoExit',
    '-MBSTMobileFireNoRender',
    "-MBSTMobileFireUnits=$Units",
    "-MBSTMobileFireTestSeconds=$TestSeconds",
    "-abslog=$LogPath"
)

& $UnrealEditorCmd @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "Unreal exited with code $LASTEXITCODE. See $LogPath"
}

$ResultLine = Select-String -LiteralPath $LogPath -Pattern 'MBST_MOBILE_FIRE_FUNCTIONAL_RESULT:' |
    Select-Object -Last 1
if (-not $ResultLine -or $ResultLine.Line -notmatch 'RESULT:\s+PASS') {
    throw "Mobile-fire functional test did not pass. See $LogPath"
}

Write-Output $ResultLine.Line.Trim()
Write-Output "PASS log: $LogPath"
