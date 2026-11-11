# Test build: unattended benchmark. Do not touch the controller or use the PC while it runs: the handheld's own pad
# still moves the player.
#
# Without -Scene: on the last save. A virtual SDL pad opens the title menu (LT+RT), picks Load, the save, confirms
# (Left, A) and closes the two pop-ups after loading (Start twice); the player then stands still. Gameplay starts at
# about 68 s.
# With -Scene <name>: replays the input recording in scenes\<name> (input.txt and save\data, made with the launcher's
# Troubleshooting > Record controller input, AC_INPUT_RECORD) from its own save, in place of all input
# (AC_INPUT_REPLAY). scenes\<name>\scene.txt may set Seconds=, From= and To= (the measured window) and Env= (variables
# for the scene, before -ExtraEnv: the settings it was recorded with, and AC_INPUT_REPLAY_CLOCK=seconds, which the
# menus need when the recording ran at another frame rate).
#
# Your save is protected either way: it is copied to build\bench\save-backups\<time> before the run, and if any of
# its files changed afterwards they are put back from that copy (checked file by file). The last 10 copies are kept.
#
# Reports the fps from -From to -To s after the first frame line, CPU % per thread (guest threads as tN) and whether
# the game had the foreground. Logs: build\bench\<name>.log.
#   powershell -File bench.ps1 -Name fix1                      the active build
#   powershell -File bench.ps1 -Name c1 -Scene cutscene        a recorded scene
#   powershell -File bench.ps1 -Name base1 -Build build\game-old -ExtraEnv "AC_SLEEP_SPIN=1"
param([string]$Name='run', [int]$Seconds=135, [string]$ExtraEnv='', [string]$Build='build\game', [string]$Scene='',
      [int]$From=76, [int]$To=116,
      [string]$Presses='22-22.6:114,22-22.6:115,26-26.3:1,30-30.3:1,34-34.3:1,38-38.3:13,42-42.3:1,65-65.3:6,69-69.3:6')
$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
if (Get-Process trace -ErrorAction SilentlyContinue) { throw 'The game is already running.' }
$out=Join-Path $PSScriptRoot 'build\bench'; New-Item -ItemType Directory -Force $out | Out-Null

$input0="AC_SDL_TEST=$Presses"
if ($Scene) {
  $sceneDir=Join-Path $PSScriptRoot "scenes\$Scene"
  if (!(Test-Path (Join-Path $sceneDir 'input.txt'))) { throw "No recording in $sceneDir (input.txt)." }
  $conf=Join-Path $sceneDir 'scene.txt'
  if (Test-Path $conf) {
    foreach ($line in Get-Content $conf) {
      if ($line -match '^\s*(Seconds|From|To)\s*=\s*(\d+)') {
        $k=$Matches[1]; if (!$PSBoundParameters.ContainsKey($k)) { Set-Variable -Name $k -Value ([int]$Matches[2]) }
      } elseif ($line -match '^\s*Env\s*=\s*(.+?)\s*$') { $sceneEnv=$Matches[1] }
    }
  }
  $input0="AC_INPUT_REPLAY=$sceneDir"
  if ($sceneEnv) { $input0+=";$sceneEnv" }
}

# ---- your save: copied before, put back after if anything in it changed
$saveUser=Get-ChildItem (Join-Path $PSScriptRoot 'userdata\saves\*\*') -Directory -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -notmatch '-(snapshots|recordings)$' -and (Test-Path (Join-Path $_.FullName 'data')) } | Select-Object -First 1
$saveData=if ($saveUser) { Join-Path $saveUser.FullName 'data' } else { $null }
function SaveHashes([string]$dir) {
  @(Get-ChildItem $dir -Recurse -File | Sort-Object FullName | ForEach-Object {
    $_.FullName.Substring($dir.Length) + ' ' + (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join "`n"
}
$backup=$null
if ($saveData) {
  $backups=Join-Path $out 'save-backups'
  $backup=Join-Path $backups ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Name")
  New-Item -ItemType Directory -Force $backup | Out-Null
  Copy-Item $saveData (Join-Path $backup 'data') -Recurse
  $before=SaveHashes $saveData
  if ((SaveHashes (Join-Path $backup 'data')) -ne $before) { throw "The save copy in $backup does not match the save: nothing was run." }
  Get-ChildItem $backups -Directory | Sort-Object Name -Descending | Select-Object -Skip 10 | Remove-Item -Recurse -Force
}

try {
  $env0="AC_PARALLEL=1;AC_IGNORE_FOCUS=1;AC_FULLSCREEN=1;AC_ASYNC_SUBMIT=1;AC_UNCAPPED=1;AC_RESOLUTION=1920x1080;AC_SAVE_SNAPSHOTS=0;$input0"
  if ($ExtraEnv) { $env0+=";$ExtraEnv" }
  $job=Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File','astral_chain\run_guarded.ps1','-Run',$Build,'-Seconds',"$Seconds",'-NoConsole','-Env',"`"$env0`"")
  $p=$null; for ($i=0; $i -lt 60 -and !$p; $i++) { Start-Sleep -Milliseconds 500; $p=Get-Process trace -ErrorAction SilentlyContinue | Select-Object -First 1 }
  if (!$p) { throw 'The game did not start.' }
  function GuestMap { $m=@{}; Get-Content "$Build\round-00\trace.log" -ErrorAction SilentlyContinue | Select-String -Pattern 'PARALLEL guest thread (\d+) on host thread (\d+)' | ForEach-Object { $m[[int]$_.Matches[0].Groups[2].Value]='t'+$_.Matches[0].Groups[1].Value }; $m }
  Start-Sleep -Seconds ([math]::Max(10, $Seconds-45))
  $snap={ $h=@{}; foreach ($t in (Get-Process -Id $p.Id).Threads) { $h[[int]$t.Id]=$t.TotalProcessorTime.TotalMilliseconds }; $h }
  if (-not ('BenchForeground' -as [type])) { Add-Type 'using System; using System.Runtime.InteropServices; public class BenchForeground { [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow(); [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid); public static uint Pid() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; } }' }
  $fg=[BenchForeground]::Pid()
  $a=& $snap; $t0=Get-Date; Start-Sleep -Seconds 20; $b=& $snap; $dt=((Get-Date)-$t0).TotalMilliseconds
  $map=GuestMap
  $top=$b.Keys | Where-Object { $a.ContainsKey($_) } | ForEach-Object { $n=$map[$_]; if (!$n) { $n="h$_" }; [pscustomobject]@{n=$n; pct=[math]::Round(100*($b[$_]-$a[$_])/$dt,0)} } | Sort-Object pct -Descending | Select-Object -First 10
  $job.WaitForExit()
} finally {
  if (Get-Process trace -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 3; Get-Process trace -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Seconds 1 }
  if ($saveData) {
    if ((SaveHashes $saveData) -eq $before) { 'save: unchanged' }
    else {
      robocopy (Join-Path $backup 'data') $saveData /MIR /R:3 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
      if ((SaveHashes $saveData) -eq $before) { "save: changed during the run, put back from $backup" }
      else { Write-Warning "save: changed during the run and NOT fully put back: copy $backup\data over $saveData" }
    }
  }
}

$log=Join-Path $out "$Name.log"; Copy-Item "$Build\round-00\stderr.log" $log -Force
Select-String -Path $log -Pattern '^\[input\]' | ForEach-Object { $_.Line }
$rows=Select-String -Path $log -Pattern 't=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms.*\((\d+) draws' | ForEach-Object { $m=$_.Matches[0].Groups; [pscustomobject]@{t=[double]$m[1].Value; fps=[double]$m[3].Value; ms=[int]$m[4].Value; dpf=[int]([int]$m[5].Value/[math]::Max([double]$m[3].Value*2,1))} }
$late=@($rows | Where-Object { $_.t - $rows[0].t -ge $From -and $_.t - $rows[0].t -le $To })
$s=$late | Measure-Object fps -Average -Minimum -Maximum
'{0}: fps avg {1:N1} min {2:N1} max {3:N1} ({6}-{7} s); draws/frame {4:N0}; foreground {5}' -f $Name,$s.Average,$s.Minimum,$s.Maximum,(($late | Measure-Object dpf -Average).Average),$(if ($fg -eq $p.Id) { 'the game' } else { 'NOT the game' }),$From,$To
'cpu%: ' + (($top | ForEach-Object { "$($_.n)=$($_.pct)" }) -join ' ')
'fps (2 s samples): ' + (($late | ForEach-Object { [int]$_.fps }) -join ' ')
'longest frame per sample (ms): ' + (($late | ForEach-Object { $_.ms }) -join ' ')
