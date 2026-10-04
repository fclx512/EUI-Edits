<#
Small-text memory diagnostic, Windows 10/11 with PROCESS_MEMORY_COUNTERS_EX2 support.
Reads total/private working set and private commit separately; never trims a working set.
Uses visible isolated windows, disables live resizing, and closes only its own processes.
Run with PowerShell 7. No existing EUI-Edits instance or forwarding file may be present.
Normal samples use repeated Markdown to expose per-line overhead; -StressCases adds
many short lines and varied text. These are startup probes, not editing/visual acceptance.
Examples: pwsh -File tests/probes/memory_small_text.ps1
          pwsh -File tests/probes/memory_small_text.ps1 -StressCases
          pwsh -File tests/probes/memory_small_text.ps1 -TextCacheOff
#>
param([string]$Exe = '', [string]$OutputRoot = '', [switch]$TextCacheOff, [switch]$StressCases)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 is required.' }
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $Exe) { $Exe = Join-Path $repoRoot 'build\Release\neo_editor.exe' }
$Exe = (Get-Item -LiteralPath $Exe).FullName
if (-not $OutputRoot) { $OutputRoot = Join-Path $repoRoot 'build\p0-small-memory' }
if (@(Get-Process neo_editor -ErrorAction SilentlyContinue).Count) { throw 'Existing EUI-Edits instance; do not touch it.' }
if (Test-Path -LiteralPath (Join-Path $env:TEMP 'EUI-Edits.next-open')) { throw 'Pending single-instance forwarding file.' }
try { $mutex = [Threading.Mutex]::OpenExisting('Local\EUI-Edits.SingleInstance'); $mutex.Dispose(); throw 'EUI-Edits mutex exists.' } catch [Threading.WaitHandleCannotBeOpenedException] {}
$taskRoot = Join-Path $OutputRoot ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID)
New-Item -ItemType Directory -Path $taskRoot -Force | Out-Null
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class SmallMemory {
 [StructLayout(LayoutKind.Sequential)] public struct Counters {
  public uint cb, PageFaultCount;
  public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
   QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage,
   PrivateUsage, PrivateWorkingSetSize;
  public ulong SharedCommitUsage;
 }
 [DllImport("psapi.dll", SetLastError=true)] public static extern bool GetProcessMemoryInfo(IntPtr h, ref Counters c, uint cb);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
}
'@
$utf8 = [Text.UTF8Encoding]::new($false)
$block = "## Heading 标题`r`n`r`n这是中文正文 English paragraph with **bold**, [link](https://example.invalid) and ``code``.`r`n`r`n- list item`r`n- 第二项`r`n`r`n"
$cases = @(
 @{Name='empty'; Bytes=0; Ext='md'},
 @{Name='md-64k'; Bytes=64KB; Ext='md'},
 @{Name='md-256k'; Bytes=256KB; Ext='md'},
 @{Name='md-768k'; Bytes=768KB; Ext='md'},
 @{Name='txt-256k'; Bytes=256KB; Ext='txt'}
)
if ($StressCases) {
 $cases = @(
  @{Name='md-short-128k'; Bytes=128KB; Ext='md'; Block="a`r`n"},
  @{Name='txt-short-128k'; Bytes=128KB; Ext='txt'; Block="a`r`n"},
  @{Name='md-varied-768k'; Bytes=768KB; Ext='md'; Varied=$true}
 )
}
$rows = @()
$conditions = [ordered]@{ Date=(Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz'); Exe=$Exe; SHA256=(Get-FileHash -LiteralPath $Exe).Hash; SourceHead=(git -C $repoRoot rev-parse HEAD); SourceStatus=@(git -C $repoRoot status --short); Resize='disabled; no resizing'; TextCacheOff=[bool]$TextCacheOff; StressCases=[bool]$StressCases; Samples='visible window at 6, 12 and 20 seconds; isolated APPDATA; no editing'; Units='MiB (bytes / 1048576)'; Root=$taskRoot }
$conditions['D2DSoftwareRequested'] = $env:NEO_D2D_SOFTWARE
$conditions | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskRoot 'conditions.json') -Encoding utf8
foreach ($case in $cases) {
 if (@(Get-Process neo_editor -ErrorAction SilentlyContinue).Count) { throw 'Another EUI-Edits appeared between scenarios.' }
 $scenario = Join-Path $taskRoot $case.Name
 $appdata = Join-Path $scenario 'appdata'
 New-Item -ItemType Directory -Path $appdata -Force | Out-Null
 $doc = $null; $bytes=0; $lines=0
 if ($case.Bytes) {
  $doc = Join-Path $scenario ('sample.' + $case.Ext)
  $caseBlock = if ($case.Block) {$case.Block} else {$block}
  $count = [int][Math]::Ceiling($case.Bytes / $utf8.GetByteCount($caseBlock))
  if ($case.Varied) {
   $builder=[Text.StringBuilder]::new()
   for ($i=0;$i -lt $count;$i++) { [void]$builder.Append($caseBlock.Replace('English paragraph',("English paragraph number $i"))) }
   $content=$builder.ToString()
  } else { $content = $caseBlock * $count }
  [IO.File]::WriteAllText($doc, $content, $utf8)
  $bytes=(Get-Item -LiteralPath $doc).Length; $lines=($content.Split("`n")).Count
 }
 $psi=[Diagnostics.ProcessStartInfo]::new()
 $psi.FileName=$Exe; $psi.WorkingDirectory=$repoRoot; $psi.UseShellExecute=$false
 if ($doc) { $psi.ArgumentList.Add($doc) }
 $psi.Environment['APPDATA']=$appdata
 $psi.Environment['NEO_LIVE_RESIZE']='0'
 $psi.Environment['NEO_TEXT_CACHE_OFF']= $(if ($TextCacheOff) {'1'} else {'0'})
 $psi.Environment.Remove('NEO_GPU_STATS') | Out-Null
 $p=[Diagnostics.Process]::Start($psi)
 try {
  $deadline=[DateTime]::UtcNow.AddSeconds(20)
  do { Start-Sleep -Milliseconds 200; $p.Refresh(); if ($p.HasExited) { throw 'Probe process exited early.' } } while ($p.MainWindowHandle -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
  if ($p.MainWindowHandle -eq [IntPtr]::Zero) { throw 'No visible main window.' }
  $owner=[uint32]0; [SmallMemory]::GetWindowThreadProcessId($p.MainWindowHandle,[ref]$owner) | Out-Null
  if ($owner -ne $p.Id) { throw 'Window PID mismatch.' }
  $previousSample=0
  foreach ($at in @(6,12,20)) {
   Start-Sleep -Seconds ($at - $previousSample)
   $previousSample=$at
   $p.Refresh(); if ($p.HasExited) { throw 'Probe process exited during measurement.' }
   $c=[SmallMemory+Counters]::new(); $c.cb=[uint32][Runtime.InteropServices.Marshal]::SizeOf([type][SmallMemory+Counters])
   if (-not [SmallMemory]::GetProcessMemoryInfo($p.Handle,[ref]$c,$c.cb)) { throw ('GetProcessMemoryInfo EX2 failed: ' + [Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
   if (-not $c.WorkingSetSize.ToUInt64()) { throw 'Zero memory sample.' }
   $row=[pscustomobject]@{ Case=$case.Name; Bytes=$bytes; Lines=$lines; Seconds=$at; PID=$p.Id; WorkingSetMiB=[Math]::Round($c.WorkingSetSize.ToUInt64()/1MB,2); PrivateWorkingSetMiB=[Math]::Round($c.PrivateWorkingSetSize.ToUInt64()/1MB,2); PrivateCommitMiB=[Math]::Round($c.PrivateUsage.ToUInt64()/1MB,2); ManagedWorkingSetMiB=[Math]::Round($p.WorkingSet64/1MB,2); PeakWorkingSetMiB=[Math]::Round($c.PeakWorkingSetSize.ToUInt64()/1MB,2); PeakCommitMiB=[Math]::Round($c.PeakPagefileUsage.ToUInt64()/1MB,2) }
   $rows += $row
   $row | ConvertTo-Json -Compress | Write-Output
   $rows | Export-Csv -LiteralPath (Join-Path $taskRoot 'samples.csv') -NoTypeInformation -Encoding utf8
  }
  $p.Modules | Where-Object { $_.ModuleName -match '^(ig|nv)|opengl|d3d|dxgi' } | Select-Object ModuleName,FileName | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $scenario 'graphics-modules.json') -Encoding utf8
  if ($case.Bytes -and -not (Select-String -LiteralPath (Join-Path $appdata 'EUI-Edits\settings.ini') -Pattern ([regex]::Escape($doc)) -Quiet)) { Write-Warning 'Settings do not yet contain requested document; check after graceful exit.' }
 } finally {
  if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(8000)) { throw ('Probe process did not close; leave PID ' + $p.Id + ' for inspection.') } }
  $p.Dispose()
 }
 if ($doc) {
  $settings=Join-Path $appdata 'EUI-Edits\settings.ini'
  if (-not (Select-String -LiteralPath $settings -Pattern ([regex]::Escape($doc.Replace('\','/'))) -Quiet) -and -not (Select-String -LiteralPath $settings -Pattern ([regex]::Escape($doc)) -Quiet)) { throw 'Requested file absent from settings after graceful close; sample invalid.' }
 }
}
$conditions | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskRoot 'conditions.json') -Encoding utf8
Write-Output ('REPORT_ROOT=' + $taskRoot)
