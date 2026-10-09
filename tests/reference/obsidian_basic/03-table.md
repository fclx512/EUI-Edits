TABLE-BEGIN

| 文件 | 说明 |
| --- | --- |
| `ui/batch_ops.py` | 批量操作的写回范式（五步顺序＋事务外壳，细则见模块 docstring 与设计 §14）；验收判据＝完成后 `utils/block_actions.py::page_data_needs_sync` 为假。中文末尾🙂 |
| 短列 | **粗体中文**与 `inline中🙂` 混排，隐藏标记与可见文字分别测试。长内容用于产生多个视觉段并检查每段背景高度。 |
|  | 空列不能产生假的文字选择背景。 |

TABLE-END
