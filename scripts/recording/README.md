# 宣传视频自动化入口

本目录提供 EUI-Edits 宣传视频的录制与剪辑脚本。以下列出现有入口；操作参数以各脚本的 `--help` 和实现为准，演示分镜可以按需求调整。

- [`mcp_client.py`](mcp_client.py)：Snow Shot stdio MCP 通信基础。
- [`record_trial.py`](record_trial.py)：隔离录制与采样骨架，`--exe` 和 `--run` 指定被测 EXE 与全新输出目录。打印 READY 后先检查 `prepared.png`，再通过另一次调用创建 `<run>/GO`。
- [`edit_trial.py`](edit_trial.py)：依据 `raw.mp4` 和 `report.json` 导出视频，`--run` 指定录制目录，`--ffmpeg` 可覆盖工具路径。演示范围、字幕、卡片和布局属于可改的样例实现。
- [`prepare.py`](prepare.py) / [`record_smoke.py`](record_smoke.py)：旧冒烟链路；与 trial 链路的准备方式和报告格式不同，不要混用。

`record_trial.py` 使用软件渲染、隔离配置与临时目录，并在 `report.json` 中保留 EXE 哈希、场景、采样值和退出检查。录屏样本应连同文档大小、标签数、渲染条件和采样时段一起解释。无需录屏的 README 内存复测可使用 [`readme_memory.py`](../../tests/probes/readme_memory.py)，该探针不强制覆盖渲染选项，保留当前 Win32 版的默认路径。

不同方案使用独立录制与剪辑输出目录。当前脚本并非通用分镜引擎；改动演示内容时同时检查场景映射、文案、采样范围和保存验证。录制开始会操作鼠标键盘，只有一个 agent 可以占用本机 GUI。
