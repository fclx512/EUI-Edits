---
title: Manual clean AI comparison
keep_marker: [cite_start]
---

# 清除 AI 格式：手工对照样本

运行菜单「清除 AI 格式」后观察正文与保护结构。这里只展示当前保守扫描器的重点样本，不代表完整 CommonMark 解析。

## 正文中应清除

这句有引用[cite_start]，还有 [CITE: 1, 2]、[CiTe:1,2]、[cite_end]、【1†source】、【1:3†title】和【oai_citation:1|title】。

零宽​字符；不间断 空格；文首标记：﻿内容。

期望：正文引用标记删除，U+200B/U+FEFF 删除，NBSP 变成普通空格。frontmatter 中的 `[cite_start]` 应保留。

## 正常文字与反例应保留

普通数字 [1]、脚注[^1]，弯引号“内容”、破折号——和省略号……都保留。

两个空格  和行尾硬换行保留  
ZWNJ‌、ZWJ‍ emoji 👩‍💻 与双向控制 ‮方向‬ 保留。

[cite:x]、[cite: 1, x]、【abc†source】、[cite:1 2] 都不是完整已知标记，应保留。

[ref]: https://example.test/[cite_start]

引用链接 [引用正文][ref] 的定义目标保持原样。

## Markdown 结构应保留

- > ```text
- > [cite_start]
- > ```

    [cite_start]

>     [cite_start]

-     [cite_start]

行内代码 `[cite_start]` 和链接目标 [链接](https://example.test/[cite_start]) 保留。

图片目标 ![图片](assets/[cite_start].png) 保留。

[[目标[cite_start]]] 与 [[含 NBSP 的目标]] 保留。

[reference][cite_start] 保留为引用链接。

[a](url "标题里有右括号 [cite_start])") 的链接目标和标题保持原样。

## HTML 块应保留

<div data-reference="[cite_start]">
[cite_start]
</div>

<!--

[cite_start]
-->

<![CDATA[

[cite_start]
]]>

<?xml-stylesheet

[cite_start]
?>

<script>
const marker = "[cite_start]";

still code [cite_start]
</script>

## 选区上下文

只选中行内代码 `` `[cite_start]` `` 内部的标记并清理：不得变化。只选中最上方真实正文中的一个标记并清理：只改选中的标记，其他相同文本保持原样。

## 未完成语法的保守处理（放在最后）

未闭合目标 [未闭合](url [cite_start] 保守保留其余部分。
