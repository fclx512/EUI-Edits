# 自动配色参考数据

此 CSV 来自用户已确认的离线交互预览算法：Material Color Utilities JavaScript 0.3.0 + EUI 语义色映射 1。

- 8 种起始色、3 个方案、7 个色相轮次、浅深两侧、26 个字段，共 8736 个 RGBA8 值。
- 本轮原始适配器：`out/theme-auto-research-20261018/engine/palette.mjs`；输入集合见 CSV。
- C++ 对应实现：`apps/neo_editor/model/theme_auto_palette.cpp`。
- `theme_workbench` 单测逐字段精确核对，同时独立检查指定前景/背景对比；参考值不得由 C++ 实现重新生成。
- JavaScript 包 SHA256：2f0729537d5f0f0f1e1663a12cce1e52b75a1c0fef26ba60789e5b01954bdfa8。
- MCU C++ 固定源版本与本地适配见 `3rd/material-color-utilities/UPSTREAM.md`。

CSV SHA256: 8a7c9cdec81a79cd7b18cc8926dfcc0bde3182f85e333c8edbcb5be77b2727cf
