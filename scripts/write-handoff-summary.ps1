[CmdletBinding()]
param(
    [string]$Repository = (Get-Location).Path,
    [string]$OutputDirectory = '',
    [string]$ExePath = '',
    [string]$GateLog = '',
    [int]$RecentOutCount = 8,
    [string]$Python = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path -LiteralPath $Repository).Path
if (-not $Python) {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) { $Python = $pythonCommand.Source }
}
if (-not $Python -or -not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    throw 'Python 3 not found. Pass -Python with an absolute executable path.'
}
$docsOutput = @(& $Python -B (Join-Path $repo 'scripts/check-docs.py') --root $repo --base HEAD --json)
if ($LASTEXITCODE -ne 0) { throw "Documentation checks failed before handoff.`n$($docsOutput -join "`n")" }
$docsCheck = ($docsOutput -join "`n") | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repo 'local-docs/repo-docs/handoffs'
} elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $repo $OutputDirectory
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

function Invoke-GitText {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $result = & git -C $repo @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE`n$($result -join "`n")"
    }
    return ($result -join "`n").TrimEnd()
}

function Invoke-GitLines {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $result = @(& git -C $repo @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE`n$($result -join "`n")"
    }
    return @($result | ForEach-Object { [string]$_ } | Where-Object { $_ -ne '' })
}

function Format-CodeBlock {
    param([AllowEmptyString()][string]$Text)
    if ([string]::IsNullOrEmpty($Text)) { return '(none)' }
    return $Text
}

$now = Get-Date
$stamp = $now.ToString('yyyyMMdd-HHmmss')
$outputPath = Join-Path $OutputDirectory "handoff-$stamp.md"
while (Test-Path -LiteralPath $outputPath) {
    Start-Sleep -Milliseconds 20
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss-fff')
    $outputPath = Join-Path $OutputDirectory "handoff-$stamp.md"
}

$branch = Invoke-GitText @('branch', '--show-current')
$head = Invoke-GitText @('rev-parse', 'HEAD')
$status = Invoke-GitText @('status', '--short', '--untracked-files=all')
$trackedFiles = @(Invoke-GitLines @('ls-files'))
$trackedCount = $trackedFiles.Count
$changedFiles = @($status -split "`r?`n" | Where-Object { $_ -and $_.Length -ge 4 } | ForEach-Object { $_.Substring(3) })
$versionFile = Join-Path $repo 'cmake/NeoEditorVersion.cmake'
$version = 'NOT_FOUND'
if (Test-Path -LiteralPath $versionFile) {
    $versionMatch = Select-String -LiteralPath $versionFile -Pattern 'NEO_EDITOR_VERSION\s+"?([^"\s\)]+)' | Select-Object -First 1
    if ($versionMatch) { $version = $versionMatch.Matches[0].Groups[1].Value }
}

$exeLine = 'NOT_RUN'
$exeHash = 'NOT_RUN'
if ($ExePath) {
    $resolvedExe = if ([System.IO.Path]::IsPathRooted($ExePath)) { $ExePath } else { Join-Path $repo $ExePath }
    if (Test-Path -LiteralPath $resolvedExe -PathType Leaf) {
        $resolvedExe = (Resolve-Path -LiteralPath $resolvedExe).Path
        $exeLine = $resolvedExe
        $exeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $resolvedExe).Hash
    } else {
        $exeLine = "NOT_FOUND: $resolvedExe"
        $exeHash = 'NOT_FOUND'
    }
}

$gateLine = 'NOT_RUN'
if ($GateLog) {
    $resolvedGate = if ([System.IO.Path]::IsPathRooted($GateLog)) { $GateLog } else { Join-Path $repo $GateLog }
    if (Test-Path -LiteralPath $resolvedGate -PathType Leaf) {
        $gateLine = (Resolve-Path -LiteralPath $resolvedGate).Path
    } else {
        $gateLine = "NOT_FOUND: $resolvedGate"
    }
}

$outEntries = @()
$outRoot = Join-Path $repo 'out'
if (Test-Path -LiteralPath $outRoot -PathType Container) {
    $outEntries = @(Get-ChildItem -LiteralPath $outRoot -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First ([Math]::Max(0, $RecentOutCount)) | ForEach-Object {
        "- $($_.Name) — $($_.LastWriteTime.ToString('s'))"
    })
}

$sourceFiles = @(Invoke-GitLines @('ls-files', '--cached', '--others', '--exclude-standard', '*.cpp', '*.h', '*.hpp', '*.c', '*.cmake', 'CMakeLists.txt', '*.ps1', '*.py') | Sort-Object -Unique)
$sourceHash = 'NOT_RUN'
if ($sourceFiles.Count -gt 0) {
    $hashLines = foreach ($relative in $sourceFiles) {
        $full = Join-Path $repo $relative
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $full).Hash
            "$relative`t$hash"
        }
    }
    $sourceHash = (Get-FileHash -Algorithm SHA256 -InputStream ([IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes(($hashLines -join "`n"))))).Hash
}

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# EUI-Edits 本地交接快照')
$lines.Add('')
$lines.Add(('> 生成时间：{0}。本文件位于 ''local-docs/''，不纳入 Git；它只记录生成时状态，不替代新一轮构建和验收。' -f $now.ToString('yyyy-MM-dd HH:mm:ss zzz')))
$lines.Add('')
$lines.Add('## 身份')
$lines.Add('')
$lines.Add(('- 分支：''{0}''' -f $branch))
$lines.Add(('- HEAD：''{0}''' -f $head))
$lines.Add(('- 产品版本：''{0}''' -f $version))
$lines.Add(('- 跟踪文件数：{0}' -f $trackedCount))
$lines.Add(('- dirty/untracked 条目数：{0}' -f $changedFiles.Count))
$lines.Add(('- 当前源码文件清单摘要 SHA256：''{0}''' -f $sourceHash))
$lines.Add('')
$lines.Add('## 当前工作树')
$lines.Add('')
$lines.Add('```text')
$lines.Add((Format-CodeBlock $status))
$lines.Add('```')
$lines.Add('')
$lines.Add('## 验证与候选包')
$lines.Add('')
$lines.Add(('- 门禁日志：{0}' -f $gateLine))
$lines.Add(('- EXE：{0}' -f $exeLine))
$lines.Add(('- EXE SHA256：{0}' -f $exeHash))
$lines.Add('- GUI/性能/低配/多屏/DPI/IME 覆盖：由本轮记录填写；缺失项写 `NOT_RUN` 或 `SKIP`。')
$lines.Add('')
$lines.Add('## 最近 `out/` 目录')
$lines.Add('')
if ($outEntries.Count -gt 0) { $outEntries | ForEach-Object { $lines.Add($_) } } else { $lines.Add('- NOT_FOUND') }
$lines.Add('')
$lines.Add('## 本轮交接待填写')
$lines.Add('')
$lines.Add('- 目标与完成条件：')
$lines.Add('- 实际修改与未采用功能：')
$lines.Add('- 文档更新与无需修改的核对结论：')
$lines.Add('- 定向测试与正式门禁结果：')
$lines.Add('- 真实窗口/性能状态：PASS / FAIL / OBSERVED / SKIP / NOT_RUN')
$lines.Add('- 覆盖缺口与失败原因：')
$lines.Add('- 下一步：一个具体动作、文件或脚本、通过条件。')
$lines.Add('- 提交/推送/标签/发布授权：未授权 / 已明确授权（按实际填写）')
$lines.Add('')
$lines.Add('## 文档维护检查')
$lines.Add('')
$lines.Add('- 文档检查：PASS（清单、导航、链接、版本简介与源码同步核对）。')
$lines.Add(('- 比较基准：{0}' -f $docsCheck.base))
foreach ($impact in $docsCheck.impacts) {
    $lines.Add(('- 已核对 {0}：{1}' -f $impact.topic, ($impact.review_docs -join '、')))
}
foreach ($decision in $docsCheck.document_reviews.PSObject.Properties) {
    $lines.Add(('- {0}：{1}' -f $decision.Name, $decision.Value))
}

[IO.File]::WriteAllLines($outputPath, $lines, [Text.UTF8Encoding]::new($false))
Write-Output $outputPath
