# 宣传视频自动化入口

完整操作方法见 [NeoEditor-宣传视频录制与复刻指南](../../docs/NeoEditor-宣传视频录制与复刻指南.md)。指南记录展示需求、工具与依赖、GUI 条件、录制/剪辑命令、内存口径、检查和故障处理；不固定已有试片的分镜。

- `mcp_client.py`：Snow Shot stdio MCP 通信基础。
- `record_trial.py`：已验证的隔离录制与采样骨架，`--exe` 和 `--run` 指定候选 EXE 与全新输出目录。打印 READY 后先检查 `prepared.png`，再通过另一次调用创建 `<run>/GO`。
- `edit_trial.py`：依据 `raw.mp4` 和 `report.json` 导出视频，`--run` 指定录制目录，`--ffmpeg` 可覆盖工具路径。演示范围、字幕、卡片和布局属于可改的样例实现。
- `prepare.py` / `record_smoke.py`：旧冒烟链路；与 trial 链路的准备方式和报告格式不同，不要混用。

不同方案使用独立录制与剪辑输出目录。当前脚本并非通用分镜引擎；改动演示内容时同时检查场景映射、文案、采样范围和保存验证。录制开始会操作鼠标键盘，只有一个 agent 可以占用本机 GUI。
