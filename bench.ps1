# Test build: unattended benchmark on the last save. A virtual SDL pad opens the title menu (LT+RT), picks Load, the
# save, confirms (Left, A) and closes the two pop-ups after loading (Start twice); the player then stands still.
# Reports the fps from 76 to 116 s after the first frame line (gameplay starts at about 68 s), CPU % per thread (guest
# threads as tN) and whether the game had the foreground. Saves are not written (AC_SAVE_SNAPSHOTS=0; nothing is
# played). Do not touch the controller or use the PC while it runs: the handheld's own pad still moves the player.
# Logs: build\bench\<name>.log.
#   powershell -File bench.ps1 -Name fix1                      the active build
#   powershell -File bench.ps1 -Name base1 -Build build\game-old -ExtraEnv "AC_SLEEP_SPIN=1"
param([string]$Name='run', [int]$Seconds=135, [string]$ExtraEnv='', [string]$Build='build\game',
      [string]$Presses='22-22.6:114,22-22.6:115,26-26.3:1,30-30.3:1,34-34.3:1,38-38.3:13,42-42.3:1,65-65.3:6,69-69.3:6')
$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
if (Get-Process trace -ErrorAction SilentlyContinue) { throw 'The game is already running.' }
$out=Join-Path $PSScriptRoot 'build\bench'; New-Item -ItemType Directory -Force $out | Out-Null
$env0="AC_PARALLEL=1;AC_IGNORE_FOCUS=1;AC_FULLSCREEN=1;AC_ASYNC_SUBMIT=1;AC_UNCAPPED=1;AC_RESOLUTION=1920x1080;AC_SAVE_SNAPSHOTS=0;AC_SDL_TEST=$Presses"
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
$log=Join-Path $out "$Name.log"; Copy-Item "$Build\round-00\stderr.log" $log -Force
$rows=Select-String -Path $log -Pattern 't=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms.*\((\d+) draws' | ForEach-Object { $m=$_.Matches[0].Groups; [pscustomobject]@{t=[double]$m[1].Value; fps=[double]$m[3].Value; dpf=[int]([int]$m[5].Value/[math]::Max([double]$m[3].Value*2,1))} }
$late=@($rows | Where-Object { $_.t - $rows[0].t -ge 76 -and $_.t - $rows[0].t -le 116 })
$s=$late | Measure-Object fps -Average -Minimum -Maximum
'{0}: fps avg {1:N1} min {2:N1} max {3:N1}; draws/frame {4:N0}; foreground {5}' -f $Name,$s.Average,$s.Minimum,$s.Maximum,(($late | Measure-Object dpf -Average).Average),$(if ($fg -eq $p.Id) { 'the game' } else { 'NOT the game' })
'cpu%: ' + (($top | ForEach-Object { "$($_.n)=$($_.pct)" }) -join ' ')
'fps (2 s samples): ' + (($late | ForEach-Object { [int]$_.fps }) -join ' ')
