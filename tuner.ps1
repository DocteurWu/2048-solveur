# tuner.ps1 - campagne de tuning heuristique 2048
# Round 2 : moteur final (LUT eval + policy_depth recalibre), --threads 1 par
# job pour isoler les jeux (4 jeux en parallele = 4 coeurs logiques).
# Compile une variante par config (=-DCFG_W_*), joue N seeds en parallele,
# agrège score/tuile max, classe les configs.
param(
    [int]$Parallel = 4,
    [double]$Ms = 400,
    [int]$JobsPerConfig = 4
)
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$gpp = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe'
if (-not (Test-Path $gpp)) { $gpp = 'g++' }
$tune = Join-Path $dir 'tune'
New-Item -ItemType Directory -Force -Path $tune | Out-Null
$progLog = Join-Path $dir 'tuner_progress.log'
$results = Join-Path $dir 'tuning_results.csv'
$summary = Join-Path $dir 'tuning_summary.txt'

function Log([string]$m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Add-Content -Path $progLog -Value $line
    Write-Host $line
}
Log "=== campagne start (parallel=$Parallel ms=$Ms jobs/config=$JobsPerConfig) ==="

$seeds = @(2048, 99991, 777001, 424242) | Select-Object -First $JobsPerConfig
$configs = @(
    @{ n = 'base'; d = @() },
    @{ n = 'mono75'; d = @('CFG_W_MONO=75.0') },
    @{ n = 'corner600'; d = @('CFG_W_CORNER=600.0') },
    @{ n = 'empty160'; d = @('CFG_W_EMPTY_BASE=160.0') },
    @{ n = 'corner250'; d = @('CFG_W_CORNER=250.0') },
    @{ n = 'mono45'; d = @('CFG_W_MONO=45.0') },
    @{ n = 'empty90'; d = @('CFG_W_EMPTY_BASE=90.0') },
    @{ n = 'snake12'; d = @('CFG_W_SNAKE=12.0') }
)

Set-Content -Path $results -Value 'config,seed,score,max_tile,moves'

Log 'compilation des variantes...'
foreach ($c in $configs) {
    $exe = Join-Path $tune "$($c.n).exe"
    $defs = @()
    foreach ($kv in $c.d) { $defs += "-D$kv" }
    $args = @('-O3', '-march=native', '-std=c++20') + $defs + @((Join-Path $dir 'main.cpp'), '-o', $exe)
    & $gpp @args
    if ($LASTEXITCODE -ne 0) { throw "compilation KO: $($c.n)" }
    Log "  compile ok: $($c.n)"
}

$jobs = @()
foreach ($c in $configs) { foreach ($s in $seeds) { $jobs += @{ n = $c.n; s = $s } } }
Log "total jobs: $($jobs.Count)"

function Parse-Result([string]$cfg, [string]$logPath, $seed) {
    if (-not (Test-Path $logPath)) { Log "  LOG MANQUANT $cfg/$seed"; return }
    $txt = Get-Content -Path $logPath -Raw
    $mt = [regex]::Match($txt, 'Tuile max\s*:\s*(\d+)')
    $sc = [regex]::Match($txt, 'Score\s*:\s*(\d+)')
    $mv = [regex]::Match($txt, 'Coups\s*:\s*(\d+)')
    if (-not $mt.Success) { Log "  PARSE KO $cfg/$seed"; return }
    $tile = [int]$mt.Groups[1].Value
    $score = if ($sc.Success) { [int]$sc.Groups[1].Value } else { 0 }
    $moves = if ($mv.Success) { [int]$mv.Groups[1].Value } else { 0 }
    Add-Content -Path $results -Value "$cfg,$seed,$score,$tile,$moves"
    Log "  $cfg seed=$seed -> score=$score tuile=$tile coups=$moves"
}

for ($i = 0; $i -lt $jobs.Count; $i += $Parallel) {
    $end = [Math]::Min($i + $Parallel - 1, $jobs.Count - 1)
    $wave = $jobs[$i..$end]
    Log "--- vague $i..$end ---"
    $procs = @()
    foreach ($j in $wave) {
        $exe = Join-Path $tune "$($j.n).exe"
        $log = Join-Path $tune "$($j.n)_$($j.s).log"
        $p = Start-Process -FilePath $exe -ArgumentList @('--play', '--seed', "$($j.s)", '--ms', "$Ms", '--games', '1', '--threads', '1') `
            -RedirectStandardOutput $log -PassThru -NoNewWindow
        $procs += @{ p = $p; j = $j; log = $log }
    }
    foreach ($pr in $procs) {
        $pr.p.WaitForExit()
        Parse-Result $pr.j.n $pr.log $pr.j.s
    }
}

Log '=== agrégation ==='
$rows = Import-Csv $results
$agg = $rows | Group-Object config | ForEach-Object {
    $t = $_.Group | ForEach-Object { [int]$_.max_tile }
    $s = $_.Group | ForEach-Object { [int]$_.score }
    [pscustomobject]@{
        config    = $_.Name
        avg_tile  = [math]::Round(($t | Measure-Object -Average).Average, 1)
        avg_score = [math]::Round(($s | Measure-Object -Average).Average, 0)
        max_tile  = ($t | Measure-Object -Maximum).Maximum
        max_score = ($s | Measure-Object -Maximum).Maximum
        n4096     = @($t | Where-Object { $_ -ge 4096 }).Count
        n8192     = @($t | Where-Object { $_ -ge 8192 }).Count
    }
}
$ranked = $agg | Sort-Object -Property @{ Expression = 'avg_tile'; Descending = $true },
    @{ Expression = 'avg_score'; Descending = $true }
$ranked | Format-Table -AutoSize | Out-String | Write-Host
$txt = "=== RANKING (ms=$Ms, seeds=$($seeds -join ',')) ===`n" + ($ranked | Format-Table -AutoSize | Out-String)
Add-Content -Path $summary -Value $txt
Log "classement ecrit dans $summary"
Log '=== campagne terminee ==='
