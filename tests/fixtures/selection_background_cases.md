# 鼠标划选背景复现夹具

在 Live Preview 中缩窄正文，使第二列内容折成至少三条视觉行。从第二列最后一个字符之后反向拖动到第一列第一个字符之前。预期每条被选中的视觉文本行都有等高背景，复制范围覆盖两列实际内容。再用同样范围正向拖选作对照。

## 用户截图近似内容

| 文件 | 说明 |
| --- | --- |
| `ui/batch_ops.py` | 批量操作的写回范式（五步顺序＋事务外壳，细则见模块 docstring 与设计 §14）；验收判断＝完成后 `utils/block_actions.py::page_data_needs_sync` 为假 |

## 纯中文对照

| 文件 | 说明 |
| --- | --- |
| ui/batch_ops.py | 批量操作的写回范式五步顺序事务外壳细则见模块设计验收判断完成后为假 |

## ASCII 对照

| File | Description |
| --- | --- |
| ui/batch_ops.py | Batch operations write back in five steps with transaction scope and validation |

## emoji 末尾对照

| File | Description |
| --- | --- |
| ui/batch_ops.py | Batch operations write back in five steps with transaction scope and validation😀 |

## 两列换行与空列

| 第一列 | 第二列 |
| --- | --- |
| 第一列也有很长的中文内容用于比较不同列的视觉行映射与部分选区 | 第二列包含更多文字并继续换行用于验证选区不能仅依赖起点终点所在视觉行来决定背景范围 |
| | 空的第一列，第二列有中文内容并继续换行 |
| 短 | |

部分选区补测：同列跨段、两列同一视觉行、只选一个中文或 emoji、列边界、源行起止含隐藏管道、跨越表头与下一数据行。此文件只是复现材料，不表示上述 GUI 案例已运行。
