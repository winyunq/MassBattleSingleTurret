param(
    [ValidateRange(1, 1000000)]
    [int]$Units = 500,

    [ValidateRange(1, 21)]
    [int]$Repetitions = 3,

    [ValidateRange(0.0, 3600.0)]
    [double]$WarmupSeconds = 10.0,

    [ValidateRange(1.0, 3600.0)]
    [double]$SampleSeconds = 20.0,

    [ValidateRange(100.0, 10000000.0)]
    [double]$TargetRadius = 10000.0,

    [ValidateRange(1.0, 3600.0)]
    [double]$TargetPeriodSeconds = 12.0,

    [ValidateRange(1.0, 60.0)]
    [double]$LogicHz = 15.0,

    [ValidateRange(1, 1000000)]
    [int]$BatchSize = 10000,

    [ValidateSet('actor', 'mass', 'turret')]
    [string[]]$Scenarios = @('mass', 'turret'),

    [string]$DisablePlugins = 'FogOfWar',

    [string]$UnrealEditor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe',
    [string]$Project,
    [string]$OutputRoot,
    [switch]$CaptureScreenshots,
    [switch]$SummarizeOnly
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
if (-not $Project) {
    $Project = [System.IO.Path]::GetFullPath((Join-Path $PluginRoot '..\..\Winyunq.uproject'))
}
if (-not $OutputRoot) {
    $ProjectRoot = Split-Path -Parent $Project
    $OutputRoot = Join-Path $ProjectRoot "Saved\MassBattleSingleTurret\NativeTrackingBenchmark_$Units"
}

if (-not (Test-Path -LiteralPath $UnrealEditor)) {
    throw "UnrealEditor was not found: $UnrealEditor"
}
if (-not (Test-Path -LiteralPath $Project)) {
    throw "Project was not found: $Project"
}

$Map = '/MassBattleSingleTurret/Demo/Benchmark/Map_MBST_NativeTrackingBenchmark'
$ScenarioFiles = @{
    actor = 'Actor'
    mass = 'Mass'
    turret = 'TurretMass'
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

if (-not $SummarizeOnly) {
for ($RunIndex = 1; $RunIndex -le $Repetitions; ++$RunIndex) {
    $RunDirectory = Join-Path $OutputRoot "Run$RunIndex"
    New-Item -ItemType Directory -Force -Path $RunDirectory | Out-Null
    $Order = if (($RunIndex % 2) -eq 1) {
        @($Scenarios)
    }
    else {
        @($Scenarios[($Scenarios.Count - 1)..0])
    }

    foreach ($Scenario in $Order) {
        $LogPath = Join-Path (Split-Path -Parent $Project) "Saved\Logs\MBST_Native_${Units}_Run${RunIndex}_${Scenario}.log"
        $Arguments = @(
            $Project,
            $Map,
            '-game',
            '-windowed',
            '-ResX=1280',
            '-ResY=720',
            '-dx12',
            '-NoVSync',
            '-NoSplash',
            '-NoLoadingScreen',
            '-NoSound',
            '-unattended',
            '-ExecCmds=t.MaxFPS 0',
            "-MBSTScenario=$Scenario",
            "-MBSTUnits=$Units",
            "-MBSTWarmup=$WarmupSeconds",
            "-MBSTSample=$SampleSeconds",
            "-MBSTLogicHz=$LogicHz",
            "-MBSTBatchSize=$BatchSize",
            "-MBSTTargetRadius=$TargetRadius",
            "-MBSTTargetPeriod=$TargetPeriodSeconds",
            "-MBSTOutputDir=$RunDirectory",
            "-abslog=$LogPath"
        )
        if ($DisablePlugins) {
            $Arguments += "-DisablePlugins=$DisablePlugins"
        }
        if (-not $CaptureScreenshots) {
            $Arguments += '-MBSTSkipScreenshots'
        }

        Write-Host "Run $RunIndex/${Repetitions}: $Scenario ($Units units)"
        # A visible, non-minimized window is intentional: occluded/minimized
        # rendering can invalidate UE's RHI/GPU comparison.
        $Process = Start-Process `
            -FilePath $UnrealEditor `
            -ArgumentList $Arguments `
            -PassThru `
            -WindowStyle Normal
        try {
            $Process.PriorityClass = 'High'
        }
        catch {
            Write-Warning "Could not raise benchmark process priority: $($_.Exception.Message)"
        }
        $Process.WaitForExit()
        if ($Process.ExitCode -ne 0) {
            throw "Scenario '$Scenario' exited with code $($Process.ExitCode). Log: $LogPath"
        }

        $ExpectedResult = Join-Path $RunDirectory "$($ScenarioFiles[$Scenario])_${Units}_units.json"
        if (-not (Test-Path -LiteralPath $ExpectedResult)) {
            throw "Scenario '$Scenario' did not produce $ExpectedResult. Log: $LogPath"
        }
    }
}
}

function Get-Median([double[]]$Values) {
    $Sorted = @($Values | Sort-Object)
    if ($Sorted.Count -eq 0) { return 0.0 }
    $Middle = [int][math]::Floor($Sorted.Count / 2)
    if (($Sorted.Count % 2) -eq 1) { return [double]$Sorted[$Middle] }
    return ([double]$Sorted[$Middle - 1] + [double]$Sorted[$Middle]) * 0.5
}

$Rows = @()
Get-ChildItem -LiteralPath $OutputRoot -Filter '*.json' -Recurse |
    Where-Object { $_.Directory.Name -like 'Run*' } |
    ForEach-Object {
        $Json = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
        $Rows += [pscustomobject]@{
            Scenario = [string]$Json.scenario_token
            FrameAverageMs = [double]$Json.timings.frame_wall_ms.average
            FrameP95Ms = [double]$Json.timings.frame_wall_ms.p95
            GameThreadAverageMs = [double]$Json.timings.game_thread_ms.average
            RenderThreadAverageMs = [double]$Json.timings.render_thread_ms.average
            GPUAverageMs = [double]$Json.timings.gpu_frame_ms.average
            Samples = [int]$Json.timings.frame_wall_ms.sample_count
            Source = $_.FullName
        }
    }

$SummaryRows = @()
foreach ($ScenarioName in $Scenarios) {
    $Scenario = $ScenarioFiles[$ScenarioName]
    $ScenarioRows = @($Rows | Where-Object Scenario -eq $Scenario)
    if ($ScenarioRows.Count -ne $Repetitions) {
        throw "Expected $Repetitions '$Scenario' results, found $($ScenarioRows.Count)."
    }
    $MedianFrame = Get-Median @($ScenarioRows.FrameAverageMs)
    $SummaryRows += [pscustomobject]@{
        scenario = $Scenario
        repetitions = $ScenarioRows.Count
        median_frame_average_ms = $MedianFrame
        fps_from_median_frame = if ($MedianFrame -gt 0.0) { 1000.0 / $MedianFrame } else { 0.0 }
        median_frame_p95_ms = Get-Median @($ScenarioRows.FrameP95Ms)
        median_game_thread_average_ms = Get-Median @($ScenarioRows.GameThreadAverageMs)
        median_render_thread_average_ms = Get-Median @($ScenarioRows.RenderThreadAverageMs)
        median_gpu_average_ms = Get-Median @($ScenarioRows.GPUAverageMs)
        raw_results = @($ScenarioRows.Source)
    }
}

$Summary = [ordered]@{
    benchmark_schema_version = 3
    aggregation = 'median of independent UE process runs'
    comparison = 'identical turret-capable MassBattleFrame AgentConfig, renderer, mesh, material, Niagara, and entity archetype'
    feature_off_path = 'turret pack processor unregistered; whole root rotation tracks the target'
    feature_on_path = 'fixed body root; turret state is packed to Style for GPU local transform'
    units = $Units
    repetitions = $Repetitions
    warmup_seconds = $WarmupSeconds
    sample_seconds = $SampleSeconds
    logic_hz = $LogicHz
    frame_spreading = $true
    render_batch_size = $BatchSize
    resolution = '1280x720'
    rhi = 'DX12'
    vsync = $false
    map = $Map
    scenarios = $SummaryRows
}
$SummaryPath = Join-Path $OutputRoot "Summary_${Units}_units.json"
$Summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $SummaryPath -Encoding UTF8

$SummaryRows |
    Select-Object scenario,
        @{Name='FrameAvgMs';Expression={[math]::Round($_.median_frame_average_ms, 4)}},
        @{Name='FPS';Expression={[math]::Round($_.fps_from_median_frame, 2)}},
        @{Name='FrameP95Ms';Expression={[math]::Round($_.median_frame_p95_ms, 4)}},
        @{Name='GameMs';Expression={[math]::Round($_.median_game_thread_average_ms, 4)}},
        @{Name='RenderMs';Expression={[math]::Round($_.median_render_thread_average_ms, 4)}},
        @{Name='GPUMs';Expression={[math]::Round($_.median_gpu_average_ms, 4)}} |
    Format-Table -AutoSize
Write-Host "Summary: $SummaryPath"
