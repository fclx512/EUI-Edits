# NeoEditor 独立发布构建

> 下文的旧构建结果与哈希为 2026-10-02 的历史记录。当前产品使用系统字体和矢量 UI 图标，旧记录中的 Font Awesome 内嵌字体描述不再适用。

## 当前本地构建（2026-10-04）

最新已验收程序统一放在 `out/latest/EUI-Edits-0.1.0-windows-x64.exe`，版本为 `0.1.0`，SHA256 为 `bed21e8d4e0f52e46b40be9685d397e10b22e1c58538b02d49888bcc13e1c48e`。同目录保留 SHA256 文件、完整许可证和 `latest.json`；当前增量构建目录为 `build-win32/ui-issues-20261004`。验收记录、构建日志及现场整理清单位于 `out/acceptance`，均不入源码库。

该本地构建依赖动态 MSVC 运行库，不能直接充当静态 CRT 的正式单 EXE 发布包。

## 正式静态包（2026-10-04）

README 中英双语定稿（提交 `bf9da23`）后，用 `scripts/package-neoeditor.ps1 -Version 0.1.0` 生成正式单 EXE（静态框架与 MSVC CRT，全新构建目录 `build-neoeditor-package-f6076e66d58a422dbd24cdd251f2018f`）：

- 位置：`out/euiedits-0.1.0-single-exe/EUI-Edits-0.1.0-windows-x64.exe`（3,671,552 字节）+ 同名 `.sha256`
- SHA256：`c3176a0c079fb189015a6efee943e415a2a761d0890e0c6eed5842f3fbf6f19d`（与旁置校验文件一致）
- 打包校验：PE 版本/x64、仅系统 DLL 依赖、内嵌资源与许可导出（空目录，49,649 字节，与 `out/latest` 的许可文本同尺寸）均通过
- 实机冒烟：隔离 APPDATA 启动、argv 打开 Markdown、文档库侧栏与状态栏正常，工作集 57.9 MB / 专用 29.8 MB，与已验收构建口径一致

剩余步骤：远端仓库尚未配置（`git remote` 为空），创建远端、推送与公开发布需用户授权后进行。原 123 云盘自动上传工具（`D:\ruanjian\neo-upload\`）与 123pan 客户端登录态在本机均已不存在，自动上传通道失效。

## 独立发布构建与历史记录

用户已确定 NeoEditor 首发产品版本为 `0.1.0`；它与根 `project(EUI-NEO VERSION 0.6.0)` 独立。根目录英文 README 与 `README.zh-CN.md` 已改为 NeoEditor 产品说明；旧框架说明归档在 `docs/upstream/`，原 EUI-NEO workflows 归档在 `.github/upstream-workflows/`，不再作为活动 workflow 自动触发。

在安装 VS 2022 或 VS 2026 C++ x64 工具的 Windows 环境执行：

```powershell
./scripts/package-neoeditor.ps1 -Version 0.1.0
```

脚本查找 CMake 和 dumpbin；必要时显式传 `-CMake`、`-Dumpbin`、`-Generator`。也可传 `-BuildDirectory`（必须不存在，不会删除旧目录）和 `-OutputDirectory`（必须不存在或为空）。默认使用新 GUID 构建目录，从 VS 安装版本选择 VS 2022/2026 x64 生成器，`EUI_BUILD_NEOEDITOR_ONLY=ON`、Win32/D2D、bundled 依赖、静态框架与 MSVC CRT；禁用可选模块、安装规则、测试夹具和 ambient CURL，只构建 `neo_editor` 及链接依赖。原框架的后端和应用能力仍可按原方式构建。

`-SkipBuild` 可打包已构建候选，但会检查缓存配置（含安装、模块、测试夹具关闭）、PE 版本、x64 和 DLL 导入。仅系统 DLL 允许动态依赖，异常第三方 DLL/动态 MSVC CRT 会拒绝打包。编译调试路径映射为相对路径；独立构建禁用图片资源查找中的源码目录兜底，并扫描最终 EXE，避免携带开发机器路径。现有合并图标 RC 追加 VERSIONINFO，避免拆分 RC 导致图标资源碰撞。

打包前的 Win32/Direct2D 正确性门禁使用独立脚本；`-BuildDirectory` 必须是不存在的新目录，失败即停止且不会清理旧目录。默认从 PATH/VS 查找 CMake 与 Python，也可明确指定：

```powershell
./scripts/check-neoeditor.ps1 -BuildDirectory ./build-neoeditor-check
# 如 Python 不在 PATH：-Python D:/Python312/python.exe
```

最新 Win32/Direct2D fresh check 在独立新目录中通过 **23/23**，包含 `image_viewport`、`vault_rename`、`dsl_main_handle`、`win32_input` 等回归；i18n 静态检查报告 **514 条消息、534 处字面量调用、12 处动态 ID**，动态 ID 已逐项审查。检查脚本不启动 GUI，也不把性能基准作为发布正确性门槛。打包门禁将仅含 EXE 的副本放入新空目录，确认可启动许可导出命令、许可文本完整，且没有 `assets` 回退目录。

默认输出目录仅包含可运行程序与校验文件：

```text
out/neoeditor-<version>-single-exe/
  NeoEditor-<version>-windows-x64.exe
  NeoEditor-<version>-windows-x64.exe.sha256
```

程序图标、Font Awesome UI 图标字体和主许可、完整第三方许可证与 notices 均以 Windows RCDATA 嵌入 EXE。About 页面可“查看并导出完整许可证文本”；也可运行 `NeoEditor-<version>-windows-x64.exe --export-licenses [输出路径]`，省略路径时输出当前目录的 `NeoEditor-LICENSES.txt`。独立发布构建不创建旁置 `assets` 文件夹，也不会因原源码目录、工作目录或 build 目录资源而成功回退；用户设置和恢复副本仍保存在 `%APPDATA%/NeoEditor`。

`.github/workflows/neoeditor-ci.yml` 在 `main`、`master`、`codex/**` 推送、PR 和手动触发时运行 Windows 检查，再构建单 EXE 候选并上传 workflow artifact。`.github/workflows/neoeditor-release.yml` 在手动候选流程或 `neoeditor-v*` 标签流程中先运行同一正确性门禁，再打包；标签流程只创建 **draft** release，release 附件为 EXE 与 `.exe.sha256`。旧 EUI-NEO `ci.yml`、`pages.yml`、`release.yml` 已移至 `.github/upstream-workflows/`，不会因 NeoEditor README/docs 更新、普通 `v*` 标签或主分支推送触发。PowerShell 检查脚本通过 Parser 与本地实际运行，两个活动 workflow 通过 actionlint 1.7.12 校验；当前没有配置 Git remote，因此 GitHub 上的 workflow 尚未实际运行，不能把本地结果表述为远端流水线通过。

启动恢复按未保存草稿优先处理：如果上次异常退出留下恢复文稿，同时用户通过文件关联启动另一份文件，先恢复草稿，再让命令行文件进入正常的保存／不保存／取消保护流程。单实例转发只在当前文档模态事务结束后消费，文件拖放在这些模态状态下不启动新文档，避免覆盖关闭、保存冲突或风险确认中的待处理操作。About 页按当前构建后端显示窗口/渲染技术栈，并在小窗口中提供完整滚动范围和许可证导出入口。

许可资源包含根 `LICENSE`、`apps/neo_editor/THIRD-PARTY-NOTICES.md` 和 13 份 `LICENSES/*.txt`，共 15 个来源；没有独立根 `NOTICE` 文件。About 直接查看并导出入口通过 7 项 GUI 检查，命令行导出也通过实测。EXE 中包含 Windows `ICON/GROUP_ICON` 图标资源以及 Font Awesome 字体 RCDATA；抽取的字体字节与源文件一致。许可涵盖 Apache-2.0、FreeType FTL 及组成部分、zlib、libpng、MD4C、yyjson、tray、stb_image、NanoSVG、miniaudio 和 Font Awesome OFL。vendored FreeType 缺失的 FTL 从官方 `VER-2-13-3` 补入 NeoEditor 专用目录。NeoEditor 使用 `assets/icon.ico` 编译出的应用图标；`assets/icon.png` 不用于该应用。Windows 系统字体不复制进包。

最终单 EXE 位于 `out/neoeditor-0.1.0-single-exe-final/NeoEditor-0.1.0-windows-x64.exe`，大小 **3,819,520 字节**，SHA-256：

```text
9c83011b857a888cb1a80f181a9f7dcac3ae336f9364690aad08addfd7347d77
```

该哈希绑定的实机回归包含图片查看两个主题 22 项、文档库重命名两个主题 28 项、About 许可导出 7 项，共 57 项断言、5 次正常退出；程序也在仅有 EXE 的空目录启动通过。此前 E42… ZIP 候选及 132 项恢复／保存回归属于前批历史，不代表本轮单 EXE 的哈希或覆盖范围。用户已暂停公开发版并继续开发；当前成果未提交、推送或发布，且仓库没有 Git remote。
