# NeoEditor / EUI-Edits 宣传视频录制与复刻指南

维护日期：2026-10-04。面向接手视频制作的 AI agent，也可供人工操作。

这份文档记录展示需求、可执行的录制方法和检查标准。镜头顺序、具体文案、文稿内容、卡片样式、停留时间和音乐都允许重新设计。本次测试片的分镜不作为后续制作模板。

## 1. 要做出什么效果

产品定位是轻量文本编辑器，支持常用 Markdown。宣传应让观众看到真实操作，并理解工具给日常写作带来的便利。

| 需要展现的内容 | 观众应该看见什么 | 制作要求 |
|---|---|---|
| Markdown 实时渲染与编辑 | 输入文字和 Markdown 标记时，版式随之变化；可以直接继续修改正文 | 用脚本快速输入常见格式，但保留看清变化的时间；不能只展示预先排好的静态页面 |
| 低内存与轻量运行 | 清楚、可信的程序内存值，以及对应的使用场景 | 必须使用当前候选 EXE 实测；主画面使用易懂的 MB 数字，补充指标名称和条件；架构比较必须有同条件数据 |
| 流畅易用的交互 | 在文稿之间切换，继续阅读或编辑，操作有连贯性 | 标签切换是明确的候选内容；可以选择其他已验证的常用交互，避免只有机械重复点击 |

Markdown 候选格式包括标题、列表、任务、引用、强调、行内代码、代码块和表格。按当前版本已经实现并检查过的能力选择，不要求为了覆盖格式把全部内容塞入一个画面。输入内容本身不是重点，应服务于画面清晰和操作连贯。

已有风格要求：

- 用于 B 站常规视频，默认横屏 1920×1080、30fps；总时长暂未固定。
- 使用已构建的当前交付版本，通常从 `out/` 中选择；不能仅凭文件名认为某个 EXE 是最新版本。
- 无配音；介绍字幕配无人声背景音乐，有简单过渡效果。
- 节奏以观众能看清操作为准，支持脚本快速输入；不为了速度牺牲可读性。
- 音乐、字幕、配色和取景可以由制作 agent 调整。浅色背景、合成试听轨、当前示例卡片都不是用户锁定的风格。
- 先做可修改的测试版，由用户比较效果后继续细化。

## 2. 接手时先核对这些文件

从仓库根目录 `D:\ruanjian\NeoEditor` 开始。先读适用的 `AGENTS.md`，再执行 `git status --short`。保留已有修改，不要重置、清理、广泛暂存或覆盖其他 agent 正在编辑的文件。视频制作本身不要求提交、推送、打标签或发布。

| 文件 | 作用 | 复用边界 |
|---|---|---|
| `scripts/recording/mcp_client.py` | 启动 Snow Shot stdio MCP 桥，完成初始化、查询工具和调用工具 | 通用通信基础；桥路径可通过环境变量覆盖 |
| `scripts/recording/record_trial.py` | 隔离配置启动演示实例，准备画面、等待开始信号、输入/编辑/切页、录屏、采样、收尾 | 已跑通的执行骨架；演示内容与动作是样例，允许重新编排 |
| `scripts/recording/edit_trial.py` | 依据录制区间裁切，生成字幕/卡片/音乐、编码、拼接和检查 | 已跑通的剪辑骨架；默认镜头集合、顺序和画面布局是样例 |
| `scripts/recording/prepare.py` | 旧冒烟路径：准备隔离演示实例并写 `active.json` | 仅用于旧冒烟链路，不是 `record_trial.py` 的前置步骤 |
| `scripts/recording/record_smoke.py` | 旧冒烟路径：检查开始、暂停、恢复、结束、关闭和退出 | 使用 `prepare.py` 的输出；报告格式不同，不能直接喂给 `edit_trial.py` |
| `tests/probes/win_capture.py` | DPI、前台/窗口检查、截图、鼠标键盘、进程查询 | 录制脚本的本地依赖 |
| `tests/probes/capture_markdown.py` | 找到指定 PID 的可见窗口 | 录制脚本的本地依赖 |
| `tests/probes/memory_save.py` | 定义 `PROCESS_MEMORY_COUNTERS_EX2` 对应结构 | 当前内存采样的本地依赖；只导入结构，不运行其主流程 |

只有 Markdown 指南而没有上述脚本及依赖，不足以复刻。接手一个新 clone/worktree 时应先检查它们是否存在；文件在其他会话中创建不代表已经提交。需要传递未提交脚本时使用明确的文件副本，不能用 `git clean` 或重置来“同步”。

原始需求背景记录位于：

`C:\Users\123\.zcode\cli\memories\projects\neoeditor-e6ec80206f7a27aa\memory\neoeditor-promo-recording-workflow.md`

这是早期工具链摘要。当前操作以本指南和实际脚本为准；不需要接收方拥有那个个人目录，也不依赖其中的双链文件。

## 3. 已验证的本机工具

以下路径在维护日期已核对。迁移到其他机器时重新确认，不要假设工具在 PATH 上。

| 工具 | 本机位置 | 用途 |
|---|---|---|
| Python | `D:\Python312\python.exe` | 执行自动化脚本；本轮使用 Python 3.12 |
| Pillow | 上述 Python 的 `PIL` 包 | 生成可读的中文字幕与卡片；本轮版本 12.3.0 |
| Snow Shot 主程序 | `D:\ruanjian\SnowShot\bin\snow_shot.exe` | 实际执行录屏；本轮使用 1.2.1 |
| Snow Shot MCP 桥 | `D:\ruanjian\SnowShot\bin\snow-shot-mcp.exe` | stdio JSON-RPC 控制入口 |
| FFmpeg | `D:\ruanjian\qwenasr\ffmpeg\ffmpeg.exe` | 裁切、转场、缩放、拼接、音轨和解码检查 |
| FFprobe | `D:\ruanjian\qwenasr\ffmpeg\ffprobe.exe` | 检查分辨率、帧率、时长、帧数与音轨 |
| 中文字体 | `C:\Windows\Fonts\msyh.ttc` 等 | 剪辑脚本优先使用微软雅黑，并有黑体候选 |

另一个本机 FFmpeg/FFprobe 位置是 `D:\ruanjian\MSST-GUI-1.4.0\env\`。优先固定使用一套配对工具，并记录路径、版本或 SHA256。现有简单制作无需额外安装大型剪辑软件；如果制作方案需要人工时间线编辑，可以另选工具，但须保留原片和可解释的操作速度。

最小检查命令：

```powershell
Set-Location -LiteralPath 'D:\ruanjian\NeoEditor'
git status --short
& 'D:\Python312\python.exe' -c 'import sys, PIL; print(sys.version); print(PIL.__version__)'
& 'D:\ruanjian\qwenasr\ffmpeg\ffmpeg.exe' -version
& 'D:\ruanjian\qwenasr\ffmpeg\ffprobe.exe' -version
& 'D:\Python312\python.exe' 'scripts\recording\record_trial.py' --help
& 'D:\Python312\python.exe' 'scripts\recording\edit_trial.py' --help
Get-ChildItem -LiteralPath 'out' -Recurse -Filter '*.exe' |
    Select-Object FullName, Length, LastWriteTime
```

不要使用 `python -O`，当前录制骨架使用 `assert` 执行关键窗口与状态检查。

## 4. GUI 环境与录制所有权

真实录制需要 Windows 可交互桌面、解锁状态和可见窗口。Linux/headless、锁屏、已断开的远程桌面、不可用的显示器，都不能当作等价录制环境。

每次执行前：

1. 确认 Snow Shot 正在运行，且 MCP 可达。
2. 查询录屏状态，确认没有他人的活动录制。`open=false`、`busy=false`、`state=idle` 是当前骨架的开始条件。状态中仍有上次的 `recording_id` 或 `finalized=true` 不代表必须清理它。
3. 确认此次使用的演示 EXE、PID、HWND、隔离配置目录和输出目录。
4. 提前告知用户录制期间会控制鼠标键盘，避免同时操作。录完及时告知可以正常使用电脑。
5. 只有一个 agent 可以操作本机鼠标键盘或录屏。其他 agent 可以并行做只读调查、文稿准备或剪辑，但不能争用 GUI，也不能同时改同一个脚本。

Snow Shot 没运行时，可以用已有程序启动；在 agent 的 shell 中启动后台程序时使用隐藏方式，避免弹出无关窗口：

```powershell
Start-Process -FilePath 'D:\ruanjian\SnowShot\bin\snow_shot.exe' -WindowStyle Hidden
```

然后再次检查可达性。如果 MCP 未启用，应按当前 Snow Shot UI/工具说明处理；不能因为 EXE 存在就声称录屏能力可用。

### 查询 MCP，而不启动录屏

下面的代码只读服务状态，并关闭自己创建的桥进程。若桥位置不同，先设置 `$env:SNOW_SHOT_MCP_EXE`。不要把这个变量误当成 Snow Shot 主程序路径。

```powershell
Set-Location -LiteralPath 'D:\ruanjian\NeoEditor'
$promoProbeCode = @'
import json, sys
sys.path.insert(0, 'scripts/recording')
from mcp_client import Client
c = Client()
try:
    print(json.dumps(c.tool('snow_shot_mcp_status'), ensure_ascii=False))
    print(json.dumps(c.tool('snow_shot_app_displays'), ensure_ascii=False))
    print(json.dumps(c.tool('snow_shot_recording_state'), ensure_ascii=False))
    for tool in c.tools:
        if tool['name'].startswith('snow_shot_recording_'):
            print(json.dumps(tool, ensure_ascii=False))
finally:
    print('bridge_exit', c.close())
'@
& 'D:\Python312\python.exe' -c $promoProbeCode
```

工具 schema 以当前 `tools/list` 返回值为准。`mcp_status` 的能力摘要不一定列出所有录屏工具，不能只看摘要就认定录屏不存在。直接执行 `mcp_client.py` 的主入口还会创建检查输出目录并写 `inspect.json`，与上面的只读状态查询不同。

## 5. 隔离演示配置

`record_trial.py` 在新的 `--run` 目录下建立：

```text
<run>/
  profile/EUI-Edits/settings.ini
  temp/
  demo/
```

只给演示子进程重定向 `APPDATA`、`TEMP`、`TMP`，不要修改用户原来的配置、恢复会话或文稿。当前子进程还设置：

| 环境项 | 当前骨架值 | 作用与限制 |
|---|---|---|
| `NEO_SINGLE_INSTANCE` | `0` | 避免将打开请求转发给用户已有实例 |
| `NEO_D2D_SOFTWARE` | `1` | 使用软件渲染，便于可重复的 GUI 录制；不能冒充默认 GPU 渲染的性能结果 |
| `NEO_WIN32_DC` | `1` | 使用当前已验证的 Win32 DC 路径 |
| `NEO_LIVE_RESIZE` | `0` | 演示采用固定窗口，不展示持续拖拽缩放 |

当前样例窗口是 1600×1000，位置为物理屏幕坐标 `(200,120)`。脚本先设置 DPI 感知，再查询实际窗口矩形作为 Snow Shot 的 `region`。这些值是录制参数，不是宣传需求。

换屏幕/DPI或取景时必须重新检查：

- 窗口完整位于可见显示器内，未被其他窗口盖住。
- `prepared.png` 是真实演示窗口，字号、主题、侧栏和编辑区可读。
- 点击坐标仍落在编辑区，而非侧栏、标签或菜单。当前输入起点使用客户区 `(600,200)`。
- 不能把客户区截图大小与包含标题栏的录屏矩形直接要求为同一尺寸。
- 不要仅凭 `theme=1` 等枚举值推断主题外观，应看实际截图。

前台检查并不能独自证明屏幕未锁定；当前 `assert_unlocked` 是启发式检查，仍需要真实预览。每次输入前检查窗口归属和前台；活动录制中失去前台应停止，而不是把动作发送到另一个应用。

## 6. 跑通一轮录制

这一节复用已经跑通的技术链路，不规定创作分镜。直接运行当前样例会使用其示例文稿与动作；需要新的演示方案时先按第 9 节修改执行骨架。

### 6.1 指定 EXE 与一个全新输出目录

下面的 EXE 是已验证的路径样例，不保证以后仍是最新交付包。应从当前 `out/` 清单选择，先核对文件，再记录哈希。

```powershell
Set-Location -LiteralPath 'D:\ruanjian\NeoEditor'
$promoExe = 'D:\ruanjian\NeoEditor\out\euiedits-0.1.0-ui-details-r3-20261004\EUI-Edits-0.1.0-windows-x64.exe'
$promoRun = Join-Path 'D:\ruanjian\NeoEditor' ('build-promo-agent-a-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
Get-FileHash -LiteralPath $promoExe -Algorithm SHA256
if (Test-Path -LiteralPath $promoRun) { throw '换一个全新的输出目录，不能覆盖旧录制' }
Write-Output $promoRun
& 'D:\Python312\python.exe' 'scripts\recording\record_trial.py' --exe $promoExe --run $promoRun
```

`record_trial.py` 要求 `--run` 尚不存在，它会自己创建目录。不要提前创建这个目录，也不要把上次失败的目录删掉后复用。每个 agent 使用不同名称；脚本的 `--run`/`--exe` 是命令行参数，不读取旧冒烟的 `NEO_RECORDING_OUT`/`NEO_RECORDING_EXE`。

### 6.2 在 READY 后查看画面，再发开始信号

脚本会打印 `READY <run>`，生成 `ready.json` 和 `prepared.png`，并等待 `<run>/GO` 文件，当前等待上限为 120 秒。

必须使用能返回运行 session 的执行工具，或者后台启动录制进程。不能让第一条命令阻塞到进程退出后，才准备创建 GO；那会必然超时。不同 agent 的执行工具不同，但需要保留这三个动作：启动并继续运行 → 检查预览 → 另一次调用创建 GO。

模型能够查看本地图片时，直接打开 `prepared.png`。预览合格后，在第二次 shell 调用中写入实际 READY 路径：

```powershell
# 把路径替换成上一条命令真正打印的 READY 目录；不要重新生成时间戳。
$promoRun = 'D:\ruanjian\NeoEditor\build-promo-agent-a-实际时间戳'
New-Item -ItemType File -Path (Join-Path $promoRun 'GO') -ErrorAction Stop
```

GO 是 agent 检查预览后发出的执行信号，不是要求用户再次批准创作。已获用户授权的任务可在预览合格后继续。

如果执行工具不会返回运行 session，可在单次 PowerShell 调用中后台启动并保留日志：

```powershell
Set-Location -LiteralPath 'D:\ruanjian\NeoEditor'
$promoExe = 'D:\ruanjian\NeoEditor\out\euiedits-0.1.0-ui-details-r3-20261004\EUI-Edits-0.1.0-windows-x64.exe'
$promoRun = Join-Path 'D:\ruanjian\NeoEditor' ('build-promo-agent-a-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$promoLogRoot = 'D:\ruanjian\NeoEditor\build-promo-controller-logs'
New-Item -ItemType Directory -Path $promoLogRoot -Force | Out-Null
$promoLogStem = Join-Path $promoLogRoot (Split-Path -Leaf $promoRun)
$promoRecorderArgs = @(
    '"D:\ruanjian\NeoEditor\scripts\recording\record_trial.py"',
    '--exe', ('"{0}"' -f $promoExe),
    '--run', ('"{0}"' -f $promoRun)
)
$promoRecorder = Start-Process -FilePath 'D:\Python312\python.exe' `
    -ArgumentList $promoRecorderArgs -WorkingDirectory 'D:\ruanjian\NeoEditor' `
    -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput ($promoLogStem + '.stdout.log') `
    -RedirectStandardError ($promoLogStem + '.stderr.log')
[pscustomobject]@{ Run = $promoRun; RecorderPID = $promoRecorder.Id; LogStem = $promoLogStem }
```

后续调用仍然从打印的实际 Run 路径检查 `ready.json`/`prepared.png`，再创建 GO。shell 变量不一定跨工具调用保留，应明确重设实际路径。轮询文件或进程时使用短等待，并继续给用户必要的进度更新。

### 6.3 确认录制真正完成

轮询原执行 session，或读取后台标准输出/错误日志，直到出现 `RESULT` 和 `report.json`。GO 不等于已完成。

先检查报告再剪辑：

```powershell
$promoRun = 'D:\ruanjian\NeoEditor\build-promo-agent-a-实际时间戳'
$promoReport = Get-Content -LiteralPath (Join-Path $promoRun 'report.json') -Raw | ConvertFrom-Json
$promoReport.checks
$promoReport.demo_exit
$promoReport.bridge_exit
$promoReport.error
Get-Item -LiteralPath (Join-Path $promoRun 'raw.mp4')
```

有效的一轮必须满足：录制结束并完成写盘、所有 checks 为 true、演示进程正常退出、桥正常退出、没有未解决的 error/cleanup_error/exit_error/memory_error。也要目视实际录屏，而非仅检查 PNG 或文件存在。

默认骨架会停止并关闭自己创建的录屏，然后给自己拥有的演示窗口发 `WM_CLOSE`。它不会按名称批量结束用户的编辑器；若正常关闭超时，会留下诊断信息，不应把“杀掉进程”当作正常退出通过。

## 7. Snow Shot 控制协议要点

`Client` 已实现逐行 JSON-RPC stdio、`initialize`、`notifications/initialized` 和 `tools/list`，优先复用，不必重新实现通信。直接使用原协议的 agent 应注意：

- 响应以 `structuredContent` 为优先来源，必要时从 text content 中解析 JSON；桥还包装了 `ok`、`result` 等字段，不能把 MCP 外层与应用状态混为一层。
- 开始：`snow_shot_recording_start`。记录本次返回的 `recording_id`；不能使用查询状态中残留的旧 ID。
- 状态：`snow_shot_recording_state`。轮询到 `state=recording` 且 `busy=false` 再输入。Snow Shot 启动录制可能改变前台，开始后需要把自有演示窗口恢复到前台一次，随后再严格检查。
- 控制：`snow_shot_recording_control`。每次先查询最新状态，确认录制归属，再带 `recording_id`、`expected_revision` 和 `action` 调用，不能复用旧 revision。
- 当前已经验证的 action 包括 `pause`、`resume`、`stop`、`close`。暂停/恢复可用于场景准备，但必须等状态转换完成；最终 stop 后须等 `finalized=true` 且 `busy=false`，再检查文件并 close。
- 清理只针对自己成功 start 获得的录制 ID。发现别人的录制状态、revision 冲突或归属变更时，不要抢占或关闭。

当前可复用的录制选项为 MP4/H.264、30fps、质量 90、veryfast，关闭工具栏录入、按键提示、麦克风和系统音频；保留鼠标光标。`clarity=1080p` 是输出上限/策略，原始文件仍可能跟随选区尺寸；最终 1920×1080 由剪辑明确输出。选项 schema 改变时按服务返回值调整。

时间标记优先使用录屏状态的 `duration_ms / 1000`，而不是只用墙钟时间。暂停期间墙钟继续增加，不能直接拿它做成片切点。录制状态存在刷新粒度，必须对照原视频帧检查最终切点，必要时另存修正后的剪辑计划。

### 自写执行器时保留的报告结构

其他 agent 可以使用不同实现语言，但若要复用当前 Python 剪辑骨架，`report.json` 至少需要下面的字段。完整状态调用与样本仍建议保存；不要只为了让剪辑接受输入拼出一个“成功报告”。

| 字段 | 结构/单位 | 来源 |
|---|---|---|
| `exe` / `sha256` | EXE 绝对路径 / 64 位十六进制 SHA256 字符串 | 本次实际启动的二进制 |
| `pid` / `hwnd` / `region` | 进程 ID / 窗口句柄 / `[x,y,width,height]` | 自有演示实例与实际物理矩形 |
| `scenes` | 数组，每项为 `name`、`start_s`、`end_s` | 名称自定；起止是对应原片中的录屏秒数，且 `end_s > start_s >= 0` |
| `render_environment` | 对象，含真实的 `software_rendering` 布尔值与环境记录 | 实际子进程环境，不是希望展示的性能口径 |
| `memory_samples` | 数组，每项至少有 `t_s`、`pid` 和三个 `*_mib` 指标 | 连续采样，时间轴相对起点须记录清楚 |
| `memory_summary` | `scenario`、`pid`、`samples`、`document_bytes`、`document_lines`、`process_count`；三项指标各含 `median`/`min`/`max` | 固定采样区间汇总；当前 `samples` 为该区间的样本数 |
| 三项指标名称 | `private_ws_mib`、`working_set_mib`、`private_commit_mib` | 单位为 MiB；每项的统计方法相同 |
| `checks` | 非空对象，逐项记录实际检查的布尔结果 | 当前剪辑入口要求全部为 true |
| `demo_exit` / `bridge_exit` | 数字退出码 | 当前成功入口要求均为 0，不能把缺失退出结果写成 0 |
| `mcp_calls` 与错误字段 | 调用记录；发生问题时保留 `error` 等信息 | 用于诊断，不能为了通过剪辑而删掉错误 |

原片文件名为 `<run>/raw.mp4`。当前剪辑骨架还有软件渲染、单进程等样例限制，结构相同不代表其他条件会被自动接受；修改限制时同步修改真实条件说明和卡片。

## 8. 剪辑与重新导出

当前剪辑骨架依赖 `raw.mp4` 和 `report.json`：

```powershell
Set-Location -LiteralPath 'D:\ruanjian\NeoEditor'
$promoRun = 'D:\ruanjian\NeoEditor\build-promo-agent-a-实际时间戳'
& 'D:\Python312\python.exe' 'scripts\recording\edit_trial.py' `
    --run $promoRun --ffmpeg 'D:\ruanjian\qwenasr\ffmpeg\ffmpeg.exe'
```

它会校验录制检查与退出码、当前 EXE 哈希、内存字段和镜头区间；随后生成卡片、字幕、分段视频，配合合成无人声试听音乐导出 MP4。操作区间完整使用原速，不默认加速或延长静帧。

| 修改目标 | 当前代码位置/参数 | 是否需要重录 |
|---|---|---|
| 演示文稿、输入内容、操作顺序、输入速度 | `record_trial.py` 的文稿生成、动作段、`text()`、停留时间 | 需要 |
| 新增或删除要演示的动作 | 录制动作及对应 `scenes`；同步剪辑映射 | 需要新动作素材 |
| 剪辑中操作区间的顺序 | `edit_trial.py` 的 `SCENE_SPEC` | 可复用已有素材 |
| 精修切点与操作区间长度 | `match_scenes()` 当前完整使用报告区间；扩展为明确的剪辑参数/独立计划 | 可复用已有素材；不能把创意试验写回原始采样证据 |
| 字幕与卡片文案 | `SUBTITLES`、`make_card()` 及其调用 | 不需要 |
| 卡片停留与过渡 | `CARD_DURATIONS`、`FADE_S` | 不需要 |
| 画面位置、字号、背景、字体 | `APP_BOX`、`make_subtitle()`、`make_card()`、`font()` | 不需要；须重新目视检查 |
| 背景音乐 | `make_music()`、拼接阶段音轨输入和音量 | 不需要；改为外部音乐时另记录来源与许可 |
| 导出位置 | `OUT_DIR_NAME` | 不需要 |

默认 `SCENE_SPEC` 只认识示例脚本中的几类动作。新方案不能只写一个新的场景名称就期待自动出片：要同步修改名称匹配、字幕、场景选择和拼接逻辑。当前匹配要求每个类别恰好匹配一项，模糊词重复会报错。

当前默认卡片文字和内存逻辑还假定软件渲染、单进程和三个小文稿。因此它不是任意测试数据的通用剪辑器。改变场景规模、进程数量或渲染模式时，必须同步改校验与卡片文字，不能为了通过检查把 false 改成 true 或保留失实的文案。

导出会覆盖同一 `<run>/edit-trial-v1/` 内的生成物。比较多个 agent 或方案时，先改 `OUT_DIR_NAME` 为不同目录，或者在独立副本/checkout 中编辑；保留已被用户看过的版本，不要无提示覆盖。录制脚本的副本应仍放在 `scripts/recording/` 下，否则 `parents[2]` 和本地导入路径需要调整。

当前取景会裁去左右各 10px、底部 12px 的 DWM 外框，避免桌面从透明边框漏进画面。这是已验证窗口的技术修正，不适用于所有尺寸/DPI；更换环境后看实际帧再调整，不能裁掉菜单、文字或操作对象。

## 9. 给其他 agent 的创作自由与复刻边界

可以重新设计镜头，不需要照搬现有视频。保留第 1 节展示需求，以及可复核的真实操作和数据：

1. 先确定自己的演示文稿与操作意图，检查本版本确实支持这些操作。
2. 使用同一候选 EXE、明确的 profile 和输出目录，保证可比条件。不同渲染环境须分开标注。
3. 从现有执行骨架复用所有权、前台、状态等待、采样、异常收尾逻辑，只改演示动作和表现。
4. 每个动作区间记录语义名称与录屏起止时间。名称用于定位素材，不代表固定的成片顺序。
5. 原始操作速度与后期速度要能区分。若采用加速、慢放或静帧，保存该修改并说明，不能用后期速度证明响应性能。
6. 用自己的剪辑目录导出，并提交可改的脚本/参数。独立方案不共用可写输出目录。

文字输入不是 IME 验收：当前骨架对普通字符发送 `WM_CHAR`，换行/Tab 使用实际按键；Win32 后端会忽略控制字符形式的 WM_CHAR。不能仅通过发送 `\n` 来代替 Enter。当前示例包含 BMP 范围的中文；如果加入 emoji 等补充平面字符，需要正确的 UTF-16 surrogate 或后端支持的 `WM_UNICHAR` 路径，先核对保存内容，不能直接沿用 `ord(ch)` 的 WM_CHAR 发送。

打开第二份文稿时，当前骨架将 UTF-8 路径原子写入隔离 `TEMP` 下的 `EUI-Edits.next-open`，再通过按键唤醒处理，并等待文件被消费。这是本应用的打开请求入口，不是“把窗口换成截图”。只能使用此次演示的 TEMP；不能写用户实例的转发文件。

## 10. 内存与性能如何展示才可信

### 10.1 数字口径

| 数据 | 当前字段 | 含义与展示用途 |
|---|---|---|
| 专用工作集 | `private_ws_mib` / `PrivateWorkingSetSize` | 当前驻留 RAM 中属于该进程的专用部分；适合主画面的易懂读数，但不等于系统全部占用 |
| 工作集 | `working_set_mib` / `WorkingSetSize` | 含专用与共享页的工作集；作为补充证据，不能与另一软件的专用工作集混比 |
| 私有提交 | `private_commit_mib` / `PrivateUsage` | 进程的私有提交量；作为独立指标保留，不应叫成当前驻留 RAM |

当前报告以 MiB 保存，换算为画面十进制 MB 时乘以 `1.048576`。MB 是 1,000,000 字节，MiB 是 1,048,576 字节；使用哪种都可以，但标记与计算必须一致。

字段依据：[Microsoft PROCESS_MEMORY_COUNTERS_EX2](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex2)；任务管理器中私有工作集与提交的解释可参考 [Microsoft Edge 内存调查说明](https://blogs.windows.com/msedgedev/2021/01/13/investigate-microsoft-edge-memory-usage/)。不要把“单进程提交”“应用所属进程合计提交”“系统提交总量”混为同一个数字。

主画面可以突出“本场景约 X MB”，附近写清软件名称、指标、文档规模和进程范围；详细值放证据文件。过滤后的真实任务管理器画面可增加直观可信度，但不能暴露无关窗口/文稿，也不能为了更好看的值清空工作集、关闭必要进程或只挑最低的一帧。

### 10.2 当前采样骨架能证明什么

当前骨架从自有 PID 每约 0.2 秒采样一次，保留连续样本；动作后安排一个稳态停留，跳过其起始约一秒，计算剩余样本的 median/min/max，要求至少 20 个有效样本。报告记录文档字节数、行数和进程数。

该骨架使用软件渲染、小文稿和动作后的空闲采样。它没有 CPU/GPU 负载、响应延迟、持续交互平均值、峰值压力或跨软件测试结果。不能用这组读数得出“大文档也只有这些内存”“GPU/CPU 超低”“比某软件快 N 倍”等结论。

当前 `process_count` 只统计演示 PID 加直接子进程，也没有对子进程采样求和。本轮无子进程的情形可作为单进程范围使用；未来用于多进程应用时应改为可靠的递归进程归属和逐进程采样，不能把这个字段当作完整进程树证据。

### 10.3 正式比较还需要补什么

- 从当前交付包在正常默认渲染环境另做测量，记录实际环境。软件渲染录屏与正常环境的测量不能混用；修改环境后同步修改报告标记和剪辑校验。
- 同一机器、相同文稿内容/字节数/标签数量，固定初始化、预热、采样时段；记录软件版本、插件、主题、渲染设置和录屏是否运行。
- 常规文稿与大文档分别测，不把小文稿结果外推。大文档规模应明确给出文件字节数和行数，不能只写“大”。
- 跨浏览器架构应用比较时，把实际属于该应用的进程计入，报告应用合计的专用工作集和私有提交。不要只取一个 renderer，也不要把无关浏览器窗口的进程加进去。
- 共享工作集可能跨进程重复，不能简单累加工作集后声称得到无重复的物理总内存。
- 确需性能数字时另做有明确定义的测试；录屏观感是交互展示，不能代替计时与 CPU/GPU 测量。

## 11. 输出与检查标准

每个方案至少交付以下材料，保证其他 agent 能定位问题并重新制作：

| 材料 | 要求 |
|---|---|
| 成片 MP4 | 清楚的版本名，不覆盖其他方案；记录 SHA256 |
| 原始录屏 | 保留未剪素材、尺寸/帧率与录制条件 |
| 录制报告 | EXE 路径/哈希、PID/HWND、region、环境、动作区间、采样、状态调用与退出检查 |
| 剪辑报告/计划 | 输入哈希、素材区间、裁边/缩放、操作速度、字幕与音乐参数、导出结果 |
| 脚本与依赖说明 | 保存实际使用的脚本副本/版本以及工具路径，不能只交一个视频 |
| 审片图与必要截图 | 原始录屏和最终视频的关键帧，覆盖开始、中间、结尾和场景过渡 |
| 未完成项 | 明确哪些是环境受限、尚未测量或需要用户审片，不写成全部通过 |

当前 `edit_trial.py` 自动生成视频、字幕/卡片 PNG、分段 MP4、音轨、逐秒 contact sheet、`edit-report.json` 和一次输出的 `storyboard.md`。自动生成的 storyboard 是该方案的历史记录，不是本指南的创作要求；不要把它复制为长期固定分镜。

技术检查命令示例：

```powershell
$promoVideo = 'D:\ruanjian\NeoEditor\build-promo-agent-a-实际时间戳\你的剪辑输出目录\trial-v1.mp4'
& 'D:\ruanjian\qwenasr\ffmpeg\ffprobe.exe' -v error -count_frames `
    -show_entries 'stream=codec_type,codec_name,width,height,avg_frame_rate,nb_read_frames,duration' `
    -show_entries 'format=duration,size' -of json $promoVideo
& 'D:\ruanjian\qwenasr\ffmpeg\ffmpeg.exe' -v error -i $promoVideo -f null NUL
& 'D:\ruanjian\qwenasr\ffmpeg\ffmpeg.exe' -hide_banner -i $promoVideo `
    -map '0:a:0' -af volumedetect -f null NUL
Get-FileHash -LiteralPath $promoVideo -Algorithm SHA256
```

完整解码命令退出码应为 0。默认目标为 1920×1080、30fps，有无人声音乐音轨，时长符合当前方案；它不是要求复刻某次试片的总秒数。音量峰值检查能发现静音/削波问题，但不能代替试听。

视觉与听觉检查：

- 真实录屏中能看到输入、渲染、编辑和交互变化；保存的文稿与演示内容一致。
- 字幕不遮挡操作对象，主文字在正常播放尺寸可读；没有错误字体、乱码、UI 被裁断或长路径喧宾夺主的问题。
- 没有被误录的其他窗口、录屏工具栏、锁屏、黑帧、不可解释的跳切；DWM 外框中的背景需要处理。
- 转场不会掩盖关键操作结果；光标和变化有足够停留时间。
- 内存卡片与真实采样/环境一致；过渡截图不能冒充完整交互验收。
- 音乐没有人声、爆音、明显切换噪声或压过观感；试听占位轨可替换。
- 最终 EXE 哈希、原片哈希、导出哈希能对应，演示正常退出。

## 12. 常见失败与处理

| 现象 | 检查与处理 |
|---|---|
| `FileExistsError` / 输出目录已存在 | 换新的 run 名称，不删旧证据来复用 |
| 桥不可达或工具不存在 | 检查主程序是否运行、MCP 是否启用、桥路径与 tools/list；不要反复启动多个桥/录屏来碰运气 |
| `foreign recorder active` | 停止本轮输入，不关闭他人的录制；等待可用状态或明确协调 |
| GO 等待超时 | 执行工具可能同步阻塞；改为保留运行 session 或后台启动，在第二次调用使用真实 READY 路径发 GO |
| 开始录屏后前台断言失败 | Snow Shot 启动可能切换前台；检查是否只需恢复自有窗口一次。活动录制持续丢前台则查用户操作/窗口遮挡，不删除检查 |
| 输入跑到侧栏、菜单或另一个窗口 | 核对 HWND 所有权、前台、DPI 和客户区坐标；预览不正确应在 GO 前解决 |
| 换行、Tab 或 emoji 不正确 | 按第 9 节检查输入路由，并检查保存内容；不要把错误文本当作渲染问题 |
| 打开文稿没有新增标签 | 检查隔离 TEMP 的 next-open 是否被消费，以及会话/快捷键是否仍适用于当前版本 |
| `GetProcessMemoryInfo` 失败或采样为零 | 查看系统/API支持、结构大小、PID/进程是否仍存活；保留 error，不以旧结果填空 |
| 剪辑提示 EXE 哈希不一致 | 被测 EXE 已被替换；找回此次录制对应的二进制或重录，不改报告哈希来绕过 |
| 剪辑提示进程/软件渲染条件不符合 | 骨架有样例范围限制；按真实条件改代码与文案，不能伪造条件通过校验 |
| 镜头匹配零项或多项 | 同步录制语义名称与 SCENE_SPEC，避免模糊词重叠；不需要保持旧顺序 |
| FFmpeg 找不到 FFprobe | 指定同目录配套 FFmpeg/FFprobe；仅有 ffmpeg.exe 不满足当前剪辑骨架 |
| 中文字幕乱码/缺字 | 检查当前 Python 的 Pillow、字体路径、UTF-8 文件和字形支持 |
| 画面边缘漏出桌面/黑边 | 检查原录屏真实矩形与 DWM 裁边；不能只看客户区 prepared.png |
| stop 后文件仍未完成 | 等待 finalized 与 busy 状态，不提前读取/close；只针对本次拥有的 recording_id 清理 |
| 正常关闭超时 | 保留 PID、HWND、日志和错误；只处理已确认属于本轮的实例，不按进程名批量结束 |

失败目录保留用于诊断，下一次采用新目录。不要为了出片放宽到误录其他应用、清除用户设置、结束用户程序、修改系统 GPU/TDR 设置或伪造验收。环境无法提供真实 GUI 时应说明缺少什么，并继续可独立完成的文稿/剪辑准备。

## 13. 可直接交给新 agent 的任务说明

以下是需求与执行边界，不包含固定分镜：

> 请先阅读 `docs/NeoEditor-宣传视频录制与复刻指南.md`，核对当前脚本、交付 EXE 和 Windows GUI 条件，制作一个可修改的宣传视频测试版。需要展现 Markdown 输入时的实时渲染与编辑、可信易懂的内存表现、流畅易用的文稿交互。默认 B 站横屏 1080p30，无配音，中文介绍字幕、无人声音乐与简单过渡；内容以观众看清为准。你可以自行设计文稿、镜头顺序、节奏和视觉表达，不需要照搬已有试片。保留真实操作原片、测量条件与样本、实际脚本/参数、哈希和导出检查。软件渲染的小文稿测量不能当作正常渲染、大文档或跨软件性能结论。使用独立输出目录，保留已有修改，只操控自己启动的隔离实例，不争用本机鼠标键盘。完成后交付视频、可复刻材料和仍待验证的项目，供用户比较。
