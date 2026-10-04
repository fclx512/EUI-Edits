param(
    [string]$Exe = 'D:\ruanjian\NeoEditor\build\Release\neo_editor.exe',
    [int]$Seconds = 6,
    [string]$Label = 'run',
    [string]$OutDir = '',
    [switch]$LiveResize
)

# ICD A/B probe: start the editor briefly and record which OpenGL ICD / GPU the
# process actually uses. No resize, no input injection, no registry changes.
# Safe mode (NEO_LIVE_RESIZE=0) is the default here so the known-dangerous
# modal-resize repaint path is never entered.
# ASCII only on purpose.

$ErrorActionPreference = 'Continue'

function Sect($t) { Write-Output ""; Write-Output ("=== " + $t + " ===") }
function One($s, $n) {
    if ($null -eq $s) { return '' }
    $s = ($s -replace "`r?`n", ' '); $s = ($s -replace '\s+', ' ').Trim()
    if ($s.Length -gt $n) { return $s.Substring(0, $n) }
    return $s
}

$Exe = [IO.Path]::GetFullPath($Exe)
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "exe not found: $Exe" }

if (-not $OutDir) {
    $OutDir = Join-Path (Split-Path -Parent $Exe) '..\p0-icd-ab'
}
$OutDir = [IO.Path]::GetFullPath($OutDir)
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = Join-Path $OutDir ("{0}-{1}-{2}" -f $stamp, $Pid, $Label)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $runDir 'appdata') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $runDir 'localappdata') | Out-Null

Sect "TARGET"
Write-Output ("  exe    : " + $Exe)
$f = Get-Item -LiteralPath $Exe
Write-Output ("  mtime  : " + $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + "  size=" + $f.Length)
Write-Output ("  sha256 : " + (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash)
Write-Output ("  runDir : " + $runDir)
Write-Output ("  mode   : " + ($(if ($LiveResize) { 'live-resize ENABLED (NEO_LIVE_RESIZE unset)' } else { 'safe (NEO_LIVE_RESIZE=0)' })))

Sect "PRE-CHECK"
$foreign = @(Get-Process -Name 'neo_editor' -ErrorAction SilentlyContinue)
if ($foreign.Count -gt 0) {
    foreach ($p in $foreign) { Write-Output ("  FOREIGN pid=" + $p.Id + " start=" + $p.StartTime) }
    Write-Output "  ABORT: another neo_editor is running; single-instance would forward and exit."
    exit 2
}
$mutexHeld = $false
try {
    $m = [System.Threading.Mutex]::OpenExisting('Local\EUI-Edits.SingleInstance')
    $m.Dispose(); $mutexHeld = $true
} catch { $mutexHeld = $false }
$forward = Join-Path $env:TEMP 'EUI-Edits.next-open'
Write-Output ("  mutex held  : " + $mutexHeld)
Write-Output ("  forward file: " + (Test-Path -LiteralPath $forward))
if ($mutexHeld) { Write-Output "  ABORT: single-instance mutex held."; exit 2 }

Sect "LAUNCH"
$stderrFile = Join-Path $runDir 'stderr.txt'
$stdoutFile = Join-Path $runDir 'stdout.txt'
$trailFile = Join-Path $runDir 'gpu_trail.log'
$started = Get-Date
$env:NEO_GPU_STATS = '1'
$env:NEO_GPU_STATS_FILE = $trailFile
$env:NEO_GPU_STATS_INTERVAL_MS = '500'
if (-not $LiveResize) { $env:NEO_LIVE_RESIZE = '0' } else { Remove-Item Env:NEO_LIVE_RESIZE -ErrorAction SilentlyContinue }
$prevAppData = $env:APPDATA
$prevLocalAppData = $env:LOCALAPPDATA
$env:APPDATA = Join-Path $runDir 'appdata'
$env:LOCALAPPDATA = Join-Path $runDir 'localappdata'
$proc = $null
try {
    $proc = Start-Process -FilePath $Exe -WorkingDirectory ([IO.Path]::GetDirectoryName($Exe)) `
        -RedirectStandardError $stderrFile -RedirectStandardOutput $stdoutFile -WindowStyle Normal -PassThru
} finally {
    $env:APPDATA = $prevAppData
    $env:LOCALAPPDATA = $prevLocalAppData
    Remove-Item Env:NEO_GPU_STATS -ErrorAction SilentlyContinue
    Remove-Item Env:NEO_GPU_STATS_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:NEO_GPU_STATS_INTERVAL_MS -ErrorAction SilentlyContinue
    Remove-Item Env:NEO_LIVE_RESIZE -ErrorAction SilentlyContinue
}
Write-Output ("  pid=" + $proc.Id + " started=" + $started.ToString('HH:mm:ss'))
Start-Sleep -Seconds $Seconds

Sect "STATE AFTER $Seconds s"
$alive = -not $proc.HasExited
Write-Output ("  alive=" + $alive + " exited=" + $proc.HasExited)
if ($proc.HasExited) { Write-Output ("  exitCode=" + $proc.ExitCode) }

Sect "LOADED GRAPHICS MODULES"
if ($alive) {
    try {
        $proc.Refresh()
        $proc.Modules | Where-Object { $_.ModuleName -match 'nvoglv|nvwgf2um|nvapi|igxelpgicd|igd[0-9a-z]*|opengl32|d3d11|dxgi|vulkan|libEGL' } |
            Sort-Object ModuleName | ForEach-Object {
                Write-Output ("  " + $_.ModuleName.PadRight(24) + " " + $_.FileVersionInfo.FileVersion)
            }
    } catch { Write-Output ("  ERR: " + $_.Exception.Message) }
} else { Write-Output "  (process gone)" }

Sect "NVIDIA-SMI PROCESS TABLE"
try {
    $smi = & nvidia-smi 2>&1
    $hit = $smi | Select-String -Pattern 'neo_editor|Processes|MiB \|' -SimpleMatch:$false
    if ($hit) { $hit | ForEach-Object { Write-Output ("  " + (One $_ 160)) } } else { Write-Output "  (no matching lines)" }
} catch { Write-Output ("  nvidia-smi ERR: " + $_.Exception.Message) }

Sect "SHUTDOWN"
if ($alive) {
    [void]$proc.CloseMainWindow()
    if (-not $proc.WaitForExit(5000)) {
        Write-Output "  graceful close timed out; stopping process this probe started"
        $proc.Kill()
        [void]$proc.WaitForExit(5000)
    }
    Write-Output ("  exitCode=" + $proc.ExitCode)
}

Sect "GPU TRAIL (last 12 lines)"
if (Test-Path -LiteralPath $trailFile) {
    Get-Content -LiteralPath $trailFile -Tail 12 | ForEach-Object { Write-Output ("  " + $_) }
} else { Write-Output "  (no trail file)" }

Sect "STDERR SUMMARY"
if (Test-Path -LiteralPath $stderrFile) {
    Get-Content -LiteralPath $stderrFile -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ("  " + $_) }
} else { Write-Output "  (none)" }

Sect "NEW LiveKernelEvent SINCE START"
try {
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1001; StartTime = $started.AddMinutes(-1) } -ErrorAction Stop |
        Where-Object { $_.Message -match '141|LiveKernelEvent' } |
        ForEach-Object { Write-Output ("  " + $_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss') + " | " + (One $_.Message 200)) }
} catch { Write-Output "  (none)" }

Sect "DONE"
Write-Output ("  artifacts: " + $runDir)
