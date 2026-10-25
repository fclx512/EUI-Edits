# EUI-Edits 构建、检查与打包

更新：2026-10-12。文件名保留旧内部标识；当前产品版本源为 `cmake/NeoEditorVersion.cmake`，内部目标为 `neo_editor`。Windows/PowerShell、MSVC x64 Release、Win32/Direct2D、bundled依赖；正式包使用静态框架与CRT、内嵌资源。

每次发版前的候选冻结、版本同步、发布说明、验收状态和授权顺序见[发版流程](发版流程.md)。本文只保留构建、正确性检查和独立打包的执行细节。

## 统一准备入口

正常发版使用 `scripts/prepare-neoeditor-release.ps1 -Version <已确认版本> -Python D:/Python312/python.exe -Parallel 2`，先加 `-PreflightOnly` 检查，冻结后再去掉。完整入口串行执行正式门禁和独立包，默认输出 `out/stable-candidate/`；创建唯一证据目录，检测源码漂移，保存失败日志/退出码。成功仅为 `READY_FOR_GUI`，不提交或发布。完整路线见[发版流程](发版流程.md)。以下底层入口用于分阶段排查，不是另一套正式发版流程。

两个底层脚本也支持 `-PreflightOnly`，只检查工具和目录，不运行翻译/CTest或编译。打包默认版本读取 CMake 版本源；正式准备要求显式版本与源一致。所有发布脚本要求 PowerShell 7；并发默认 2，显式 `-Parallel` 必须带数值，不使用裸 `--parallel`。

## 构建与正确性检查

普通开发配置见[README](../README.md#开发)。CMake可通过PATH/vswhere定位，本机已使用：

```powershell
$EuiBuildCMake = 'D:/ruanjian/Microsoft Visual Studio/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
./scripts/check-neoeditor.ps1 -BuildDirectory ./out/check-next-unique/build `
  -CMake $EuiBuildCMake -Python D:/Python312/python.exe -Parallel 2
git -c core.whitespace=cr-at-eol diff --check
```

示例目录必须不存在；重跑换新唯一目录。测试集合与数量以 `scripts/check-neoeditor.ps1` 当前 `$targets` 及语言案例为准，不能沿用旧轮次的固定数字。性能、真实窗口和发布授权分别验收；本轮日志和结果保存在对应的 `out/` 与 `local-docs/` 证据位置。

## 独立正式包

确定版本与变更范围后，使用新构建/输出目录；下例0.2.2与当前源码默认版本一致，但不要求发布同版本或移动已有标签。

```powershell
./scripts/package-neoeditor.ps1 -Version 0.2.2 `
  -BuildDirectory ./out/package-next-unique/build `
  -OutputDirectory ./out/package-next-unique/artifacts `
  -CMake $EuiBuildCMake -Parallel 2
```

脚本校验缓存配置、PE版本/x64、系统DLL依赖、开发路径、内嵌许可及旁置SHA256；关闭测试夹具、可选模块和ambient CURL。构建目录与输出目录都必须在当前仓库内且互不嵌套，输出空检查在编译前执行。`-SkipBuild`额外核对缓存来源目录，但不证明源码身份；只用于排查，正式发行必须重新构建。不要复用其它checkout缓存或覆盖旧out。SHA256供维护者在上传前后核验最终资产；发布说明和仓库文档不维护哈希文件表。

新包实机验收须绑定最终EXE哈希和独立配置，记录所属窗口、实际操作、保存/撤销等结果与正常退出。正确性通过不自动代表性能、真实IME、硬件GPU、低配或多屏DPI通过。

提交、推送、打标签和发布需要本轮明确授权。上述脚本含中文注释，请用`pwsh`（PowerShell 7）运行，`powershell.exe`（5.1）会按ANSI代码页解析并报语法错误。
