# EUI-Edits 构建、检查与打包

更新：2026-10-09。文件名保留旧内部标识；当前产品版本源为 `cmake/NeoEditorVersion.cmake`，默认0.1.2，内部目标为 `neo_editor`。Windows/PowerShell、MSVC x64 Release、Win32/Direct2D、bundled依赖；正式包使用静态框架与CRT、内嵌资源。

## 构建与正确性检查

普通开发配置见[README](../README.md#开发)。CMake可通过PATH/vswhere定位，本机已使用：

```powershell
$EuiBuildCMake = 'D:/ruanjian/Microsoft Visual Studio/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
./scripts/check-neoeditor.ps1 -BuildDirectory ./out/check-next-unique/build `
  -CMake $EuiBuildCMake -Python D:/Python312/python.exe -Parallel 6
git -c core.whitespace=cr-at-eol diff --check
```

示例目录必须不存在；重跑换新唯一目录。脚本当前正式选择50个目标和4个语言案例，共54项，性能/GUI分别验收。**2026-10-09已在全新目录完成54/54及翻译检查。** 实际集合以 `scripts/check-neoeditor.ps1` 为准；这次结果绑定本轮源码，不沿用旧23/47/51项数量。逐轮证据保存在维护者本地文档区与 `out/`，不随仓库分发。

## 独立正式包

确定版本与变更范围后，使用新构建/输出目录；下例0.1.2与当前源码默认版本一致，但不要求发布同版本或移动已有标签。

```powershell
./scripts/package-neoeditor.ps1 -Version 0.1.2 `
  -BuildDirectory ./out/package-next-unique/build `
  -OutputDirectory ./out/package-next-unique/artifacts `
  -CMake $EuiBuildCMake
```

脚本校验缓存配置、PE版本/x64、系统DLL依赖、开发路径、内嵌许可及旁置SHA256；关闭测试夹具、可选模块和ambient CURL。`-SkipBuild`仍校验正式缓存和资产，不允许借用开发EXE冒充独立包。不要复用其它checkout缓存或覆盖旧out。

新包实机验收须绑定最终EXE哈希和独立配置，记录所属窗口、实际操作、保存/撤销等结果与正常退出。正确性通过不自动代表性能、真实IME、硬件GPU、低配或多屏DPI通过。

提交、推送、打标签和发布需要明确授权；计划文档中的历史授权不自动用于新版本。0.1.2 已完成版本冻结、fresh54/54门禁、独立打包与最终包窗口生命周期/渲染截图；逐轮记录、候选哈希与未覆盖缺口保存在维护者本地文档区（`local-docs/repo-docs/`），不随仓库分发。上述脚本含中文注释，请用 `pwsh`（PowerShell 7）运行，`powershell.exe`（5.1）会按ANSI代码页解析并报语法错误。
