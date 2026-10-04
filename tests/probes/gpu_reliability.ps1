# Reliability history (non-admin) + TDR-related channel probes.
# Usage: pwsh -NoProfile -File tests\probes\gpu_reliability.ps1

$ErrorActionPreference = 'Continue'
function Sect($t) { Write-Output ""; Write-Output ("=== " + $t + " ===") }
function One($s, $n) {
    if ($null -eq $s) { return '' }
    $s = ($s -replace "`r?`n", ' '); $s = ($s -replace '\s+', ' ').Trim()
    if ($s.Length -gt $n) { return $s.Substring(0, $n) }
    return $s
}

Sect "Win32_ReliabilityRecords (GPU / display related, last 14 days)"
try {
    $since = (Get-Date).AddDays(-14)
    $recs = Get-CimInstance -Namespace root\cimv2 -ClassName Win32_ReliabilityRecords -ErrorAction Stop |
        Where-Object { $_.TimeGenerated -and $_.TimeGenerated -gt $since }
    Write-Output ("  total records in window: " + @($recs).Count)
    $recs | Where-Object {
        (One $_.Message 400) -match 'GPU|graphics|display|video|TDR|LiveKernel|141|neo_editor|硬件错误|显卡|显示'
    } | Sort-Object TimeGenerated -Descending | Select-Object -First 40 | ForEach-Object {
        Write-Output ("  " + $_.TimeGenerated.ToString('yyyy-MM-dd HH:mm:ss') + "  src=" + $_.SourceName + "  event=" + $_.EventIdentifier + "  product=" + $_.ProductName)
        Write-Output ("      " + (One $_.Message 300))
    }
} catch { Write-Output ("  ERR: " + $_.Exception.Message) }

Sect "Reliability record sources histogram (last 14 days)"
try {
    $since = (Get-Date).AddDays(-14)
    Get-CimInstance -Namespace root\cimv2 -ClassName Win32_ReliabilityRecords -ErrorAction Stop |
        Where-Object { $_.TimeGenerated -and $_.TimeGenerated -gt $since } |
        Group-Object SourceName | Sort-Object Count -Descending | Select-Object -First 20 |
        ForEach-Object { Write-Output ("  " + $_.Count.ToString().PadLeft(4) + "  " + $_.Name) }
} catch { Write-Output ("  ERR: " + $_.Exception.Message) }

Sect "LiveKernelEvent-ish providers present"
try {
    Get-WinEvent -ListProvider *LiveKernel* -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ("  " + $_.Name) }
} catch { Write-Output ("  none/err: " + $_.Exception.Message) }
foreach ($log in @('Microsoft-Windows-Kernel-LiveDump/Analytic', 'Microsoft-Windows-DxgKrnl/Diagnostic', 'Microsoft-Windows-DxgKrnl/Operational')) {
    try {
        $li = Get-WinEvent -ListLog $log -ErrorAction Stop
        Write-Output ("  log " + $log + "  enabled=" + $li.IsEnabled + "  records=" + $li.RecordCount)
    } catch { Write-Output ("  log " + $log + " : not found") }
}

Sect "DONE"
