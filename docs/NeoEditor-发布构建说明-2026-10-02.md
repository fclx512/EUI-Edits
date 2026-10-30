# EUI-Edits 构建参数与脚本

本文维护工具与命令参数。验证范围见[开发与维护规范](开发流程与交接规范.md)，正式候选统一按[发版流程](发版流程.md)准备。文件名沿用原路径。

## 工具

使用 Windows、PowerShell 7、MSVC x64 Release 与 Windows SDK；依赖源码随仓库提供。脚本通过 PATH / vswhere 定位 CMake，默认并发 2。当前开发机可用：

```powershell
$EuiBuildCMake = 'D:/ruanjian/Microsoft Visual Studio/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$EuiPython = 'D:/Python312/python.exe'
```

换机后先检查路径，或让脚本自动定位。产品版本读取 `cmake/NeoEditorVersion.cmake`。

## 日常检查与增量构建

```powershell
& $EuiPython scripts/check-docs.py --base HEAD
& $EuiPython tests/tools/check_i18n.py
& $EuiBuildCMake --build ./build-eui-tests --config Release --target neo_editor --parallel 2
git -c core.whitespace=cr-at-eol diff --check
```

文档检查默认比较 HEAD，包含源码同步记录；生成草稿和填写核对结论按开发规范第 3 节执行。增量命令要求该目录已由本仓库配置；定向测试按改动选择对应 target / CTest。预览复制、版本核对和首次建树按开发规范执行。 菜单改动的定向目标包括 `context_menu`、`menu_commands`、`menu_motion`、`menu_style_benchmark`、`settings_reveal`、`decoration_cadence` 和 `d2d_render_probe`（真实像素与裁剪状态回归）；性能采样命令与口径见[性能测试方法](性能测试方法.md#菜单分风格动画)。

## 完整正确性检查

```powershell
pwsh -NoProfile -File ./scripts/check-neoeditor.ps1 `
  -BuildDirectory ./out/check-<唯一名称>/build `
  -CMake $EuiBuildCMake -Python $EuiPython -Parallel 2
```

构建目录必须不存在。脚本核对文档与回归、翻译、配置、构建和实际 CTest 名单；集合以当前 `$targets` 及衍生案例为准。`-PreflightOnly` 只定位工具和核对路径，统一发版入口的预检另含翻译及文档检查。

## 独立包的底层入口

以下用于分阶段排查；正式发版使用统一准备入口。

```powershell
pwsh -NoProfile -File ./scripts/package-neoeditor.ps1 `
  -BuildDirectory ./out/package-<唯一名称>/build `
  -OutputDirectory ./out/package-<唯一名称>/artifacts `
  -CMake $EuiBuildCMake -Parallel 2
```

默认版本来自版本源，指定版本用 `-Version`。构建与输出目录须在当前仓库内、彼此独立，输出为空或不存在。`-SkipBuild` 复查同仓库现有缓存，仅用于排查；`-PreflightOnly` 检查工具和目录。PE、依赖、嵌入资源与许可检查由脚本执行，最终资产验收按发版流程进行。
