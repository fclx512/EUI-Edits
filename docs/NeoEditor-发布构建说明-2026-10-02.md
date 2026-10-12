# EUI-Edits 构建、检查与打包

更新：2026-10-11。文件名保留旧内部标识；当前产品版本源为 `cmake/NeoEditorVersion.cmake`，内部目标为 `neo_editor`。Windows/PowerShell、MSVC x64 Release、Win32/Direct2D、bundled依赖；正式包使用静态框架与CRT、内嵌资源。

每次发版前的候选冻结、版本同步、发布说明、验收状态和授权顺序见[发版流程](发版流程.md)。本文只保留构建、正确性检查和独立打包的执行细节。

## 构建与正确性检查

普通开发配置见[README](../README.md#开发)。CMake可通过PATH/vswhere定位，本机已使用：

```powershell
$EuiBuildCMake = 'D:/ruanjian/Microsoft Visual Studio/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
./scripts/check-neoeditor.ps1 -BuildDirectory ./out/check-next-unique/build `
  -CMake $EuiBuildCMake -Python D:/Python312/python.exe -Parallel 6
git -c core.whitespace=cr-at-eol diff --check
```

示例目录必须不存在；重跑换新唯一目录。测试集合与数量以 `scripts/check-neoeditor.ps1` 当前 `$targets` 及语言案例为准，不能沿用旧轮次的固定数字。性能、真实窗口和发布授权分别验收；本轮日志和结果保存在对应的 `out/` 与 `local-docs/` 证据位置。

## 独立正式包

确定版本与变更范围后，使用新构建/输出目录；下例0.1.3与当前源码默认版本一致，但不要求发布同版本或移动已有标签。

```powershell
./scripts/package-neoeditor.ps1 -Version 0.1.3 `
  -BuildDirectory ./out/package-next-unique/build `
  -OutputDirectory ./out/package-next-unique/artifacts `
  -CMake $EuiBuildCMake
```

脚本校验缓存配置、PE版本/x64、系统DLL依赖、开发路径、内嵌许可及旁置SHA256；关闭测试夹具、可选模块和ambient CURL。`-SkipBuild`仍校验正式缓存和资产，不允许借用开发EXE冒充独立包。不要复用其它checkout缓存或覆盖旧out。SHA256供维护者在上传前后核验最终资产；发布说明和仓库文档不维护哈希文件表。

新包实机验收须绑定最终EXE哈希和独立配置，记录所属窗口、实际操作、保存/撤销等结果与正常退出。正确性通过不自动代表性能、真实IME、硬件GPU、低配或多屏DPI通过。

提交、推送、打标签和发布需要本轮明确授权。上述脚本含中文注释，请用`pwsh`（PowerShell 7）运行，`powershell.exe`（5.1）会按ANSI代码页解析并报语法错误。
