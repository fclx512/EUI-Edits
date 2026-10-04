# GPU / TDR forensics - read only, non destructive.
# ASCII only on purpose (no BOM dependency).
# Usage: pwsh -NoProfile -File tests\probes\gpu_tdr_forensics.ps1

$ErrorActionPreference = 'Continue'

function Sect($t) { Write-Output ""; Write-Output ("=== " + $t + " ===") }
function One($s, $n) {
    if ($null -eq $s) { return '' }
    $s = ($s -replace "`r?`n", ' ')
    $s = ($s -replace '\s+', ' ').Trim()
    if ($s.Length -gt $n) { return $s.Substring(0, $n) }
    return $s
}

Sect "CLOCK / BOOT"
Write-Output ("now          : " + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))
$os = Get-CimInstance Win32_OperatingSystem
Write-Output ("last boot    : " + $os.LastBootUpTime.ToString('yyyy-MM-dd HH:mm:ss'))
Write-Output ("uptime       : " + ((Get-Date) - $os.LastBootUpTime).ToString())
Write-Output ("OS           : " + $os.Caption + " build " + $os.BuildNumber)

Sect "WER ROOTS (newest 12 each)"
$roots = @(
    (Join-Path $env:ProgramData 'Microsoft\Windows\WER\ReportArchive'),
    (Join-Path $env:ProgramData 'Microsoft\Windows\WER\ReportQueue'),
    (Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\WER\ReportArchive'),
    (Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\WER\ReportQueue')
)
$cands141 = @()
$candsApp = @()
foreach ($r in $roots) {
    if (Test-Path -LiteralPath $r) {
        Write-Output ("root: " + $r)
        try {
            Get-ChildItem -LiteralPath $r -Directory -ErrorAction Stop |
                Sort-Object LastWriteTime -Descending | Select-Object -First 12 |
                ForEach-Object {
                    Write-Output ("  " + $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + "  " + $_.Name)
                    if ($_.Name -like 'Kernel_141*') { $script:cands141 += $_ }
                    if ($_.Name -like '*neo_editor*') { $script:candsApp += $_ }
                }
        } catch {
            Write-Output ("  DENIED/ERR: " + $_.Exception.Message)
        }
    } else {
        Write-Output ("missing: " + $r)
    }
}

function DumpWer($dir, $label) {
    Write-Output ("--- " + $label + "  " + $dir.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + "  " + $dir.FullName)
    $wer = Join-Path $dir.FullName 'Report.wer'
    if (-not (Test-Path -LiteralPath $wer)) { Write-Output "    (no Report.wer)"; return }
    $lines = $null
    foreach ($enc in @('Unicode', 'Default', 'OEM')) {
        try { $lines = Get-Content -LiteralPath $wer -Encoding $enc -ErrorAction Stop; break } catch { $lines = $null }
    }
    if ($null -eq $lines) {
        try { $lines = Get-Content -LiteralPath $wer -ErrorAction Stop } catch {
            Write-Output ("    READ FAILED: " + $_.Exception.Message); return
        }
    }
    Write-Output ("    (lines=" + $lines.Count + ")")
    $lines | Where-Object { $_ -match '^(Sig|EventType|AppName|AppPath|AppCompany|DumpFile|FriendlyEventName|NsPartner|Response|EventTime|ReportIdentifier|DynamicSig|OsInfo|DriverName|DeviceName|Hardware)' } |
        ForEach-Object { Write-Output ("    " + $_) }
}

Sect "KERNEL_141 REPORTS (newest 3)"
$cands141 = $cands141 | Sort-Object LastWriteTime -Descending | Select-Object -First 3
if (@($cands141).Count -eq 0) { Write-Output "none in WER roots (may be admin-only)" }
foreach ($c in $cands141) { DumpWer $c "KERNEL_141" }

Sect "NEO_EDITOR APPCRASH REPORTS (newest 3)"
$candsApp = $candsApp | Sort-Object LastWriteTime -Descending | Select-Object -First 3
if (@($candsApp).Count -eq 0) { Write-Output "none" }
foreach ($c in $candsApp) { DumpWer $c "APP" }

Sect "LIVE KERNEL REPORTS / MINIDUMPS"
foreach ($p in @('C:\Windows\LiveKernelReports', 'C:\Windows\Minidump', 'C:\Windows\MEMORY.DMP')) {
    if (Test-Path -LiteralPath $p) {
        try {
            $it = Get-Item -LiteralPath $p -ErrorAction Stop
            if ($it.PSIsContainer) {
                Get-ChildItem -LiteralPath $p -Recurse -File -ErrorAction Stop |
                    Sort-Object LastWriteTime -Descending | Select-Object -First 15 |
                    ForEach-Object { Write-Output ("  " + $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + "  " + [int]($_.Length / 1MB) + "MB  " + $_.FullName) }
            } else {
                Write-Output ("  " + $it.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + "  " + [int]($it.Length / 1MB) + "MB  " + $it.FullName)
            }
        } catch { Write-Output ("  DENIED/ERR " + $p + " : " + $_.Exception.Message) }
    } else {
        Write-Output ("  missing: " + $p)
    }
}

Sect "SYSTEM LOG (last 4 days, interesting providers/ids)"
$since = (Get-Date).AddDays(-4)
try {
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction Stop |
        Where-Object {
            $_.Id -in 41, 1001, 4101, 6008, 219, 20, 17 -or
            $_.ProviderName -match 'Kernel-Power|BugCheck|Display|Kernel-PnP|WHEA|LiveKernel|nvlddmkm|igfx'
        } |
        Sort-Object TimeCreated -Descending | Select-Object -First 80 |
        ForEach-Object {
            Write-Output ($_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss') + "  id=" + $_.Id + "  " + $_.ProviderName + "  | " + (One $_.Message 150))
        }
} catch { Write-Output ("  ERR: " + $_.Exception.Message) }

Sect "APPLICATION LOG (WER / faults, last 4 days)"
try {
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $since; Id = 1000, 1001, 1002 } -ErrorAction Stop |
        Sort-Object TimeCreated -Descending | Select-Object -First 25 |
        ForEach-Object {
            Write-Output ($_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss') + "  id=" + $_.Id + "  " + $_.ProviderName + "  | " + (One $_.Message 220))
        }
} catch { Write-Output ("  ERR: " + $_.Exception.Message) }

Sect "TDR / GRAPHICS REGISTRY"
$gd = 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers'
try {
    $p = Get-ItemProperty -Path $gd -ErrorAction Stop
    foreach ($k in @('TdrDelay', 'TdrDdiDelay', 'TdrLevel', 'TdrLimitCount', 'TdrLimitTime', 'PlatformSupportMiracast')) {
        Write-Output ("  " + $k + " = " + $p.$k)
    }
} catch { Write-Output ("  ERR: " + $_.Exception.Message) }
foreach ($k2 in @('HKLM:\SOFTWARE\Microsoft\Windows\Dwm', 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Winevt')) {
    Write-Output ("  [" + $k2 + "] exists=" + (Test-Path $k2))
}

Sect "GPU ADAPTERS / STATUS"
try {
    Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object {
        Write-Output ("  " + $_.Name)
        Write-Output ("    driver " + $_.DriverVersion + "  date " + $_.DriverDate + "  status " + $_.Status + "  cfgmgr " + $_.ConfigManagerErrorCode)
        Write-Output ("    mode " + $_.VideoModeDescription + "  cur " + $_.CurrentHorizontalResolution + "x" + $_.CurrentVerticalResolution + " @ " + $_.CurrentRefreshRate + "Hz")
        Write-Output ("    pnp " + $_.PNPDeviceID)
    }
} catch { Write-Output ("  ERR: " + $_.Exception.Message) }

Sect "USER GPU PREFERENCES (HKCU per-app)"
foreach ($k3 in @('HKCU:\SOFTWARE\Microsoft\DirectX\UserGpuPreferences',
                  'HKLM:\SOFTWARE\Microsoft\DirectX\UserGpuPreferences')) {
    if (Test-Path $k3) {
        Write-Output ("  [" + $k3 + "]")
        try {
            $item = Get-Item -LiteralPath $k3
            $item.GetValueNames() | ForEach-Object { Write-Output ("    " + $_ + "  =  " + $item.GetValue($_)) }
        } catch { Write-Output ("    ERR " + $_.Exception.Message) }
    } else {
        Write-Output ("  missing: " + $k3)
    }
}

Sect "NEO_EDITOR EXE / BUILD"
$exe = 'D:\ruanjian\NeoEditor\build\Release\neo_editor.exe'
if (Test-Path -LiteralPath $exe) {
    $f = Get-Item -LiteralPath $exe
    Write-Output ("  exe   : " + $f.FullName)
    Write-Output ("  mtime : " + $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + "   size: " + $f.Length)
    Write-Output ("  sha256: " + (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash)
} else {
    Write-Output ("  missing: " + $exe)
}
$alt = 'D:\ruanjian\NeoEditor\参考\dist\neo_editor.exe'
if (Test-Path -LiteralPath $alt) {
    $f2 = Get-Item -LiteralPath $alt
    Write-Output ("  dist  : " + $f2.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + "   size: " + $f2.Length)
    Write-Output ("  sha256: " + (Get-FileHash -LiteralPath $alt -Algorithm SHA256).Hash)
}

Sect "RUNNING NEO_EDITOR / SESSIONS"
try {
    Get-Process -Name 'neo_editor' -ErrorAction Stop | ForEach-Object {
        Write-Output ("  pid=" + $_.Id + " start=" + $_.StartTime.ToString('yyyy-MM-dd HH:mm:ss') + " ws=" + [int]($_.WorkingSet64 / 1MB) + "MB")
    }
} catch { Write-Output "  none running" }

Sect "DONE"
