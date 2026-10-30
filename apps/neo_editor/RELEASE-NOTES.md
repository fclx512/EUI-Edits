# EUI-Edits @NEO_EDITOR_VERSION@ — 发布说明

## 相比 0.2.2 的主要更新

0.2.3 完善图片标签、非 Markdown 编辑菜单和两种界面风格的菜单动画，并减少 Direct2D 的重复裁剪操作。

### 图片标签与查看

- 图片在编辑区的独立只读标签中打开，支持切换、关闭、拖拽重排和标签列表；重复打开同一文件会定位到已有页。
- 每页独立保留缩放和平移。底部提供缩小、当前比例、放大和“适应窗口”；放大后左键可平移超出画布的部分。
- 图片不会进入文字保存、编码转换或格式编辑流程；文字页的正文、光标、滚动和撤销记录独立保留。正常退出仍确认其他未保存文稿。
- 与干净文字标签一致，图片页不会在正常退出后自动重开。图片字节不写入正文恢复稿；恢复目标缺失时显示可重试的只读错误页。

### 菜单与绘制

- 非 Markdown 语法模式的编辑区右键菜单只保留剪切、复制、粘贴和全选；手动切换语法模式后菜单同步更新。
- Win10 风格菜单在固定裁剪窗内短滑入，行高亮即时切换，保留亮边；Win11 风格使用短浮入和相邻行高亮过渡。两种风格分别使用固定边框、阴影和高亮几何，关闭动画时直接切换。
- Direct2D 对相同目标、完全相同矩形的连续裁剪请求复用现有状态，不新增图像缓存或改变重绘范围。本机同轨迹测量观察到周期与调用墙钟改善，不能换算为所有设备的 CPU 使用率、能耗或屏幕延迟保证。

### 下载与更新

用新 EXE 替换旧版本。设置和本地主题保存在用户配置目录，替换 EXE 不会清除它们；如果曾登记文件打开方式，在新版本的文件关联设置中应用并修复登记。

目标平台为 Windows 10/11 x64，程序未签名。自绘最大化按钮暂不提供 Windows 11 Snap 悬停布局。主题工作台仍为测试功能，长期使用请将主题草稿保存为文件；CSS 导入只映射支持的配色。

## English

### Changes since 0.2.2

- Images open in independent read-only tabs with switching, closing, drag reordering, a tab list, and per-tab zoom/pan. Reopening the same file activates its existing tab.
- Bottom controls provide zoom out, scale, zoom in, and Fit window. Dragging pans only overflowing areas. Text tabs retain their own contents, cursor, scroll position, and undo history.
- Images cannot enter text save, encoding-conversion, or formatting paths. They do not reopen after a normal exit, matching clean text tabs. Recovery stores image metadata only; unavailable image targets show a retry page.
- Non-Markdown context menus contain only Cut, Copy, Paste, and Select all, following the selected syntax mode.
- Win10-style menus slide inside a fixed clip with immediate row highlights and Reveal; Win11 uses a short floating entrance and adjacent highlight motion. Borders, shadows, and highlight geometry follow each style. Disabling animations applies changes immediately.
- Direct2D reuses consecutive identical clips on the same target without adding image caches or changing redraw bounds. Local measurements observed fewer cycles and shorter timed calls; this is not a universal CPU, power, or display-latency guarantee.

Replace the old EXE to update. Settings and saved local themes remain in the user configuration folder. Reapply file associations in Settings if previously registered. Windows 10/11 x64 is targeted; the app is unsigned. The custom maximize button does not provide Windows 11 Snap hover layouts. The theme workbench remains experimental; save drafts as theme files for long-term use.
