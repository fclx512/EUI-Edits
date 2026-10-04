# EUI-NEO 特性与个人开发参考

> 个人笔记，不是上游文档。原存放在 `参考/` 的本地采集资料已于 2026-10-04 发版清理时删除（该目录原本被 `.gitignore` 忽略，从未入库）。
>
> 调研时间：2026-09-21　代码基线：`main` @ `b9032a8`（v0.6.0 发布提交）
> 上游：`origin` = https://atomgit.com/sudoevolve/EUI-NEO.git（GitHub 同名仓库，AtomGit 是国内镜像）
> 许可：原创源码 Apache 2.0（个人改动若要回馈上游没有障碍）

---

## 0. 这个框架是什么

C++17 的声明式桌面 UI 框架。窗口后端 GLFW 或 SDL2，渲染后端 OpenGL 或 Vulkan，两者的组合都可选。

应用侧只写两个函数：

```cpp
const DslAppConfig& dslAppConfig();                          // 窗口/字体/调试配置
void compose(eui::Ui& ui, const eui::Screen& screen);       // 声明式描述当前这一帧的 UI
```

`include/eui/dsl_app.h` 已经包好 initialize / update / isAnimating / render / shutdown、后台异步任务、按刷新率节流，以及**无动画时等待事件休眠**。也就是说"闲着就真的不跑"是框架自带行为，不是需要自己写的优化。

---

## 1. 本项目的取向（我自己的定位，写下来免得跑偏）

- **极致低占用**：空闲零重绘、输入按需重绘，是选它的主要理由。
- **低负载动画**：只在状态变化时做短过渡（换色、位移、透明度、圆角），不做常驻动效。干净直接。
- **实用主义视觉**：不需要复杂排版、字距、字重体系、设计感堆料。控件长得像控件、点了有反应、看一眼就懂，就够。
- **娱乐向**：小工具、实验、可视化练手。不必背商业产品的排版/无障碍/品牌包袱。

这份取向直接决定了后面的"该做/不该做"：凡是需要**每帧重算**或**常驻动画**才能成立的设计，一律放弃；凡是 Runtime 内部就能插值完成的视觉变化，随便用。

---

## 2. 核心心智模型（理解这四条，写法就不会歪）

### 2.1 声明式重 compose，Runtime 全接管

`compose()` 每次被调用都会重新声明整棵 DSL 树。应用不创建 primitive、不碰 GPU、不读 GLFW/SDL 状态、不手动标脏区。布局（measure→layout）、命中测试、事件派发、动画插值、脏区计算、保留层缓存，全在 Runtime 内。

### 2.2 id 是身份，不是标签

`ui.rect("card.bg")` 里的 id 是 Runtime 复用实例和缓存层的 key。

- **id 稳定** → primitive 实例复用 + retained layer 缓存命中 + 动画状态延续。
- **id 变化**（例如把状态拼进 id、或用循环下标当 id）→ 每帧新建/销毁实例，缓存全部失效，CPU 和显存一起抖。

子元素一律 `父id + ".part"`。这条是"低占用"的第一前提。

### 2.3 受控组件 + 页面持有状态

组件自己不存业务状态：页面传当前值（`value`/`checked`/`selected`/`open`/`visible`），组件通过 `onChange` / `onOpenChange` / `onDismiss` 回传下一个值。组件内部的纯交互态（拖拽中的中间量、动画计时）才用 `ui.state<T>(id)`，生命周期跟着 `pageId` 和 loader scope 走。

想省掉样板可以用 `eui::Signal<T>`，但注意 `.bind(signal)` 是 **builder 上的方法**，`Signal` 本身没有 `bind`。

### 2.4 视觉变化不 compose，状态变化才 compose

- 颜色/透明度/圆角/阴影/变换/模糊的过渡，由 Runtime 逐帧插值，**不触发重 compose**。
- 点击回调改了业务状态 → Runtime 标记 `composeRequested()` → 下一页重 compose + 保守 full paint。

所以"动画掉帧"和"重 compose"是两回事：前者看属性能否被 Runtime 插值，后者看你在回调里改了多少状态。

---

## 3. 能力速查

### 3.1 元素只有 10 种

容器 4 种：`ui.row` / `ui.column` / `ui.stack` / `ui.flow`（`ui.loader` 是生命周期 scope，不绘制）。
叶子 6 种：`ui.rect` / `ui.polygon` / `ui.text` / `ui.image` / `ui.svg` / `ui.shadertoy`。

任何视觉效果最终都落到这 10 种上，所以别指望"某种现成的样式系统"。

**布局语义**（`core/layout.h`）：

| 项 | 支持情况 |
| --- | --- |
| 尺寸 | `SizeValue{Fixed, WrapContent, Fill}`；`width/height/size/fill()/wrapContent()` |
| 间距 | `gap`、`lineGap`（Flow 换行用）、`padding`（1/2/4 值）、`margin` |
| 对齐 | `justifyContent` / `alignItems` / `align`，值 `Align{START, CENTER, END}` |
| flex | `flexGrow` / `flexShrink`（**默认 shrink = 0**，固定尺寸控件不会被压扁） |
| 约束 | `minSize/maxSize/minWidth/...` |
| 层叠 | `zIndex`（只影响绘制与命中，不参与布局） |
| 特殊 | `ignoreLayout()`（装饰层）、`clip()`（矩形裁剪） |
| **没有** | Grid、百分比尺寸、aspect-ratio、媒体查询、order/basis/wrap |

响应式只能手写阈值分支：`const bool wide = width >= 820.0f;`，然后两个分支各写一套结构。

### 3.2 内置组件（约 35 个，`components/components.h` 是权威清单）

| 类别 | 组件 |
| --- | --- |
| 命令/选择 | `button`、`checkbox`、`radio`、`toggleSwitch`、`segmented`、`tabs` |
| 数值/文本 | `progress`、`slider`、`stepper`、`input`（单行/多行、IME、撤销） |
| 菜单/弹层 | `dropdown`、`contextMenu`、`dialog`、`sidebar`、`toast`、`tooltip` |
| Picker | `datePicker`、`timePicker`、`colorPicker`（都是 dialog 式 overlay，draft + Done 提交） |
| 数据/图表 | `dataTable`、`lineChart`、`barChart`、`pieChart`、`markdown` |
| 滚动/大集合 | `scroll`（底层滚动条）、`scrollView`（自动测高）、`virtualList`（定高虚拟）、`virtualMasonry`（变高虚拟网格） |
| 导航 | `navbar`、`tabs`、`sidebar` |
| 展示 | `carousel`、`image`（主题包装） |
| 容器/排版 | `card`、`panel`、`text`（主题包装）、`layoutDebugOverlay` |
| 输入热区 | `mouseArea`（tap/hover/drag/scroll/右键 + 局部坐标） |
| 主题 | `theme`（token 与派生色） |
| 工坊 | `workshop::neumorphicButton`、`workshop::heartSwitch`、`workshop::tiltCard`、`workshop::cardSlider` |

`navbar` 和 `virtualMasonry` **没有进** EUI-NEO 克隆（`D:\编译开发\EUI-NEO`）`docs/组件.md` 的表格，但确实存在并导出。`virtualMasonry` 对"变高卡片墙"很有用，值得记住。

### 3.3 视觉属性

**Rect 专有**：`.color` `.gradient` `.radius` `.border(w,color)` `.shadow(...)` `.insetShadow(...)` `.blur(...)` `.opacity` `.states(normal,hover,pressed)` `.hoverColor/.pressedColor/.smoothStates/.instantStates`。

**Text**：`.text` `.icon(codepoint)` `.fontFamily` `.fontSize` `.fontWeight` `.color` `.opacity` `.maxWidth` `.wrap` `.horizontalAlign` `.verticalAlign` `.lineHeight`。

**Image / Svg**：`.source` `.bingDaily(idx,mkt)` `.stream`（CPU 帧）`.texture`（外部 GPU 纹理）`.tint` `.radius` `.blur` `.opacity` `.cover/.contain/.stretch` `.coverViewport` `.flipVertically`。

**Image 独有福利**：`eui::image::themeColor(source, fallback)` 从图片采样主色——想让 UI 跟着封面图变色，一个调用就够。

**Polygon**：`.points({...})` `.point(x,y)` `.radius(...)`（顶点圆润），单独走 polygon shader 抗锯齿。折线/饼图/tooltip 指针都是它。

**Transform（渲染期能力，不影响布局）**：`.translate` `.translate3d` `.scale` `.rotate/.rotateZ` `.rotateX/.rotateY` `.perspective` `.transformOrigin`。容器上的 transform 会继承到子树。**没有真实 depth buffer**，绘制顺序仍由树序 + zIndex 决定。

**颜色**：`eui::Color` 支持浮点 RGBA，也支持 `"#RGB" "#RGBA" "#RRGGBB" "#RRGGBBAA"` 字符串直传。外部输入用 `Color::tryFromHex` 校验。

### 3.4 主题

`components::theme::{light, dark}()` 返回 `ThemeColorTokens`，里面挂四组 metric：

| 组 | 访问 | 管什么 |
| --- | --- | --- |
| `TypographyTokens` | `tokens.metrics.typography` | 正文字号、行距档位 |
| `SpacingTokens` | `tokens.metrics.spacing` | 内容 inset、gap、页面边距 |
| `RadiusTokens` | `tokens.metrics.radius` | 控件/卡片/弹层圆角（`full = 999` 做胶囊） |
| `ControlSizeTokens` | `tokens.metrics.control` | indicator/field/menu item/switch/滚动条尺寸 |

派生助手：`pageVisuals()`、`fieldVisuals()`、`withAlpha()`、`withOpacity()`、`buttonHover/Pressed`、`border`、`shadow/buttonShadow/panelShadow/popupShadow`。

约定：`.theme(tokens)` 按主题重建样式（显式 `.size()/.fontSize()` 优先，不会被主题覆盖）；`.style(struct)` 完整覆盖样式结构。想换肤就在 app 里改一个 `themeColors()` 函数返回的 token，属于"改代码级"换肤。

**没有样式表、没有 CSS/QSS、没有热重载**。改视觉 = 改 C++ + 重编译。这是这个框架最硬的一条现实。

### 3.5 动画

可动画属性按图元分：

- Rect：frame、color、opacity、radius、border、shadow、blur、transform
- Text：frame、text color、opacity、transform
- Image/Svg：frame、tint、opacity、radius、transform
- Polygon：frame、color、opacity、transform

`Frame` 动画必须显式 `.animate(eui::AnimProperty::Frame)` 才会开（普通窗口缩放/布局变化不会自己插值）。

Ease：`Linear … OutBack`（`core/animation.h:12`）。写法两种：`.transition(0.2f, eui::Ease::OutCubic)` 或 `.transition(eui::Transition::make(...))`。

**Frame 动画的坑（重要）**：`Row/Column/Stack/Flow` 是布局容器，它们自己不做长宽插值，而且一旦祖先容器目标 frame 改变，整棵子树的叶子 frame 会被强制同步到新布局结果——所以"外层容器.height(展开 ? 大 : 小) + 叶子 animate(Frame)"**不会**平滑展开。正确做法是外层尺寸固定，让内部叶子自己插值（`examples/animated_card.cpp` 是这个模式的完整参考）。

### 3.6 输入与事件

- 通用交互：`.onClick/.onPress/.onRelease/.onMove/.onContextMenu/.onHover/.onDrag/.onScroll/.onTimer/.onFrame/.onKeyEvent/.onTextInput/.onFocusChanged`。
- 命中：按绘制顺序从最上层往下，**同一位置只有最上层命中**；`zIndex` 越高越靠上，同 z 时后声明的在上。
- 默认命中用布局矩形；旋转/缩放/透视后要跟随视觉区域，加 `.transformedHitTest()`。
- 交互**默认只接受左键**，中/右/X1/X2 要 `.acceptedButtons(...)` 显式开。
- `onKeyEvent(const eui::KeyEvent&)` 和 `onTextInput(const eui::TextInputEvent&)` 相互独立，返回 `bool` 表示是否消费。
- 弹层/抽屉/遮罩要用 `.blockPointer()` 或透明 hit rect 吃掉下层 hover/click/focus，**不要写空的 `.onClick([] {})`**。
- **没有事件冒泡**（EUI-NEO 克隆的 `docs/DSL.md` 明说"还没有"）。

### 3.7 媒体

本地文件、`http(s)` 网络图、`bing://daily` 占位图、本地 SVG、**内联 SVG**（`ui.svg(...).source(markup)`，改顺序无关，空串清除）、GIF、`.stream()` CPU 动态帧、`.texture()` 外部 GPU 纹理（已有 FFmpeg 视频播放器示例）。

内联 SVG 是精确图形/品牌 icon 的正解，不要用 polygon 去近似。Font Awesome 图标用 `.icon(0xF0C7)`，字形来自 `assets/Font Awesome 7 Free-Solid-900.otf`。

### 3.8 平台能力（`eui::platform::`）

`openUrl`、`openFileDialog/chooseFile/chooseFiles`（只支持打开，不支持存目录/保存）。窗口侧：`.windowSize/.windowPosition/.minWindowSize/.maxWindowSize/.resizable/.highDpi/.decorated(false)/.alwaysOnTop/.maximized/.uiScale/.fps`，托盘 `.tray(true)`（关窗即隐藏并释放图形资源，`Exit` 才真退），多窗口用 `DslWindowConfig` + `openWindow(...)`。

全屏、透明窗口、VSync **不在** `DslAppConfig` 里，属于平台/渲染后端生命周期。无边框（`.decorated(false)`）能做，但**没有内置自绘标题栏组件**。

可选模块只有 `modules/keyboard` 和 `modules/serial`（`EUI_ENABLE_MODULES`）。

### 3.9 后端与构建开关

| CMake 变量 | 作用 |
| --- | --- |
| `EUI_BUILD_APPS` | 编译 `examples/`（默认顶层 ON） |
| `EUI_BUILD_USER_APPS` | 编译 `apps/` |
| `EUI_BUILD_TEST_FIXTURES` | 编译 `tests/fixtures/`（默认 OFF） |
| `EUI_BUILD_FFMPEG_VIDEO_EXAMPLE` | 那个视频示例（默认 OFF） |
| `EUI_WINDOW_BACKEND` | `glfw`（默认）或 `sdl2` |
| `EUI_RENDER_BACKEND` | `auto` / `opengl`（默认）/ `vulkan` |
| `EUI_VULKAN_LOW_LATENCY_PRESENT` | Vulkan 低延迟呈现 |
| `EUI_ENABLE_TRAY` | 托盘（Linux 需要依赖，装不上就关） |
| `EUI_ENABLE_MODULES` | 可选模块 |
| `EUI_BUILD_SHARED` | 编成动态库 |

顺手注意：上游分支除了 `main`/`dev` 还有 `plot`（科学绘图）、`Android`、`sdl3`——说明项目正在往科学绘图和 Android/SDL3 走，但**这些都不在当前 main 的能力范围内**。

---

## 4. 低占用 / 低负载的实操规则

这一节是我最该记的，直接决定写出来的东西轻不轻。

**1. 每帧在动的属性，问它能不能被 Runtime 插值。**
能（color / opacity / radius / shadow / blur / transform / frame 叶子）→ 只是 `.transition(...)`，零 compose。
不能（任何需要重新声明树结构的变化）→ 每帧 compose，成本按整棵树算。**别做这种设计。**

**2. 高频指针跟随，用 Runtime binding，不要 `onMove` 回调。**
`.runtimePointerTransformFrom(...)` / `.runtimePointerTiltFrom(...)` 直接驱动 transform，不写状态、不 compose。`workshop::tiltCard` 就是这么做的；只有 Runtime 表达不了时才退回 `mouseArea.onMove`。

**3. 长列表用虚拟化，不要用 `clip` 挡。**
`clip()` **只裁像素，不省 CPU**：子树照样 compose、layout、提交。定高长列表（日志、波形、寄存器表）用 `virtualList`（百万行量级，行回调拿真实 index + 可复用 slot id）；变高卡片墙用 `virtualMasonry`；只有内容确实小才用 `scrollView`。

**4. 不要自己实现虚拟列表。** 把 offset 抄进页面 state 再从滚动回调 `requestUpdate()`，会把滚轮/拖拽变成整页 compose。`virtualList` 已经接管 Runtime→compose 的交接。

**5. 滚动本身不 compose。** 滚轮走 Runtime 惯性衰减，滚动条拖动直接跟手，两者只改滚动 transform + viewport 脏区。

**6. hover/pressed 不要增删图层。** 保持图层常在，只动颜色/透明度/变换——这样 retained layer 缓存才命中。多层阴影要"多张透明 rect 层 + 稳定 id"，而不是按状态切层。

**7. 别用"关缓存"绕问题。** 项目明确禁止为规避渲染/resize 行为去关 retained layer caching。

**8. 测量入口是现成的**：`DslAppConfig{}.showDebugStatsInTitle(true)` 把每帧统计打到窗口标题（`core::render::RenderFrameStats`：rectDraws / textDraws / imageDraws / polygonDraws / shadertoyDraws / retainedLayerHits / Misses / Rebuilds / clearCalls）。判断标准很简单：

- **静置时**：无输入、无动画 → CPU/GPU 应该回落到接近 0（框架会休眠等事件）。
- **交互时**：draw 数不应随数据总量增长，只随可见区域增长。
- **拖拽时**：不应出现每帧重 compose。

**9. 圆角容器不要装需要裁剪的内容。** `clip` 是矩形 scissor，**圆角不裁剪子内容**——圆角卡片里塞滚动列表，内容会从圆角外冒出来。

---

## 5. 动画规范（我采用的预算）

- 常规交互过渡：**0.12–0.32s**。
- 弹层/抽屉/页面级切换：最多约 **0.45s**。
- 缓动默认 `OutCubic`（干净利落）；要"啪"的手感用 `OutBack`/`OutExpo`。
- **不做**：长时、循环、脉冲、跑马灯、无限 spinner。需要"加载中"就用有限的状态变化表达（换文案/进度值/一次性动画）。
- 页面切换用 `ui.loader(id).active(...)` + `.keepAlive()`（保状态）或 `.destroyOnHide()`（重置状态），在页面容器上挂短 opacity/translate 过渡。
- 用 primitives 手搓的组件，**必须**在会变色/位移/改圆角的层上显式挂 `.transition(...)`，否则会"跳"。

这套预算不是保守，是框架的设计取向：Runtime 的脏区 + 保留层缓存假设了"大部分时间画面是静的"。

---

## 6. 已知坑与能力缺口（都核实过代码/文档，不是猜的）

| # | 坑 | 影响与绕法 |
| --- | --- | --- |
| 1 | **圆角不裁剪**：`clip` = 矩形 scissor | 圆角容器 + 滚动/图片 → 内容溢出圆角。改结构或把内容自己留白 |
| 2 | **无事件冒泡**；嵌套滚动支持有限 | 别设计深层嵌套交互；滚动交互尽量扁平 |
| 3 | **`fontWeight` 实际无效** | `text.cpp:1850` 各分支最终返回同一个默认字体路径；且默认 UI 字体就是 Bold 的 `JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf`（`text.cpp:37`）。想要正常正文观感要用 `.textFont("xxx.ttf")` 或 `.fontFamily("字体文件路径")` 显式换字体 |
| 4 | 无字距、无文字阴影、无渐变文字、无省略号截断 | 文本块留足尺寸；长文本自己截 |
| 5 | 渐变只有 2 段，只能水平/垂直 | 斜向/多色渐变要靠 Shadertoy 或叠层 |
| 6 | 只有**单**外阴影 + **单**内阴影 | 多层阴影 = 多张透明 rect 层（稳定 id） |
| 7 | 无 Grid / 百分比 / aspect-ratio / 媒体查询 | 响应式手写 `if (width >= N)` 分支 |
| 8 | 无样式表、无热重载 | 改视觉 = 改代码重编译。请把颜色/尺寸集中到一个 `themeColors()` 函数里，改起来只动一处 |
| 9 | 透明窗口、VSync 不在 `DslAppConfig` | 需要的话走平台/渲染后端层 |
| 10 | 无边框窗口没有内置自绘标题栏 | `.decorated(false)` 后要自己画标题栏 + 拖拽 + 按钮 |
| 11 | 双后端能力要分别验证 | 用 `.blur()`（backdrop 采样）、retained layer、Shadertoy 时，OpenGL 和 Vulkan 都要看过 |
| 12 | 图标字体依赖 `assets/` | 发布时带 assets 或显式 `.iconFont(...)`；Font Awesome codepoint 系统字体不一定有 |
| 13 | Shadertoy 通道受限 | 支持 image / buffer / self / none 四路 sampler + 多 pass feedback；**不支持** keyboard / video / sound / cubemap / volume |
| 14 | Polygon 单条边数上限 128 | 波形/密集折线别拼成一条自相交多边形，用独立凸段或折线图的分段画法 |
| 15 | 图表是轻量展示件 | `lineChart/barChart/pieChart` **没有科学坐标轴**，值按归一化百分比显示；真正的科学绘图还在 EUI-NEO 克隆的 `docs/科学绘图TODO.md` 规划里（全未勾选） |
| 16 | `markdown` 不执行 HTML/CSS | 图片显示为占位文本，不适合当富文本渲染器 |
| 17 | `scroll` 只是滚动条 | 内容和滚动条必须绑同一个 Runtime scroll state；自动测高用它上层的 `scrollView` |

---

## 7. assets 与资源部署

- 仓库自带 `assets/`：字体（含默认 UI 字体、优设标题黑、Font Awesome、JingNanJunJunTi Bold）、图标、`mona-loading-default.gif`、`music/`、`shaders/`。
- `eui_neo_configure_app(target)` 负责把 assets 部署到可执行文件旁（查找顺序：exe 旁 `assets/` → 工作目录 `assets/` → 上级目录 `assets/`）。
- 独立 app 目录要放自己的资源，用 `eui_neo_copy_app_assets(app_name, "app_dir/assets")`（`apps/<name>/assets` 的规则就是它）。
- 字体加载失败会回退系统字体，但要精确字形（图标、标题体）就老老实实带文件。

---

## 8. 起步方式（两条路线，工作目录还没定就先放着）

### 路线 A：直接在本仓库 `apps/` 里写（零配置，先摸手感）

CMake 的自动发现规则（`CMakeLists.txt:587-620`）：

| 放法 | 生成的目标名 |
| --- | --- |
| `apps/my_probe.cpp` | `my_probe` |
| `apps/my_app/app.cpp` | `my_app`（目录名） |

`apps/*/` 形式会自动 `GLOB_RECURSE` 该目录下所有 `.cpp`（`pages/`、`components/` 里的 `.h` 为主，`.cpp` 也会被收集），并把 `app_dir` 加进 include 路径。**重复目标名会直接 `FATAL_ERROR`**（所以别和新目录撞名）。

```sh
cmake -S . -B build -DEUI_BUILD_USER_APPS=ON
cmake --build build --config Release --target my_app --parallel
```

注意两点：
- 若已有 build 缓存里 `EUI_BUILD_USER_APPS` 是关的，必须重新 configure 一次。
- 本仓库是 upstream 克隆，自己加的 app 会一直显示为未跟踪文件，`git pull` 时心里要有数。

### 路线 B：独立项目，把 EUI-NEO 当 SDK（干净，适合长期做）

```sh
git clone https://atomgit.com/sudoevolve/EUI-NEO.git 3rd/EUI-NEO
```

```cmake
cmake_minimum_required(VERSION 3.14)
project(MyProject LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(3rd/EUI-NEO)
add_executable(my_app main.cpp)
eui_neo_configure_app(my_app)
```

Release SDK 也可以用 `find_package(EuiNeo CONFIG REQUIRED)` + `eui_neo_configure_app(my_app)`。`FetchContent` 也能用，`site/llms.txt` 里的官方快速开始就是 FetchContent 版。

**我的倾向**：先用 A 验证手感（改一行就能跑），确认要继续做长期项目时在 B 建独立目录（这样 upstream 更新和自己代码不打架，也不受"重复目标名"约束）。反正两者的 app 代码是同一套，迁移成本只是挪文件 + 建立 CMakeLists。

### 目录与命名约定（跟着项目 skill 走，改起来省事）

```text
my_app/
  app.cpp            # dslAppConfig + compose 根壳层 + 全局弹层 + 页面分发
  pages/             # 每页一个 .h；共享状态放 page_context.h
  components/        # 本 app 的自定义组件
  assets/            # 本 app 资源
```

依赖方向固定：`app.cpp -> pages -> app components -> page_context/框架`。app 自己的组件**不要** include 页面实现头。

---

## 9. 常用代码片段

### 9.1 最小可跑

```cpp
#include "eui_neo.h"

namespace app {

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("My Probe")
        .pageId("my_probe")
        .windowSize(960, 640);
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ui.column("root")
        .size(screen.width, screen.height)
        .padding(32.0f)
        .content([&] {
            ui.text("title")
                .text("Hello EUI-NEO")
                .fontSize(28.0f)
                .build();
        })
        .build();
}

} // namespace app
```

### 9.2 把颜色尺寸集中一处（应对"无热重载"）

```cpp
namespace {

components::theme::ThemeColorTokens themeColors() {
    auto tokens = components::theme::dark();
    tokens.primary = {0.28f, 0.48f, 0.92f, 1.0f};
    return tokens;
}

eui::Transition quick() { return eui::Transition::make(0.18f, eui::Ease::OutCubic); }
eui::Transition panel() { return eui::Transition::make(0.30f, eui::Ease::OutCubic); }

} // namespace
```

### 9.3 页面壳（顶栏 + 内容 + 全局弹层）

```cpp
void compose(eui::Ui& ui, const eui::Screen& screen) {
    const auto tokens = themeColors();

    ui.stack("root")
        .size(screen.width, screen.height)
        .content([&] {
            ui.rect("root.bg").fill().ignoreLayout().color(tokens.background).build();

            ui.column("page")
                .size(screen.width, screen.height)
                .content([&] {
                    ui.row("page.topbar")
                        .size(screen.width, 56.0f)
                        .padding(16.0f, 0.0f)
                        .gap(12.0f)
                        .alignItems(eui::Align::CENTER)
                        .content([&] { /* 导航/标题 */ })
                        .build();

                    ui.stack("page.body")           // 内容宿主，按需 loader 切页
                        .size(screen.width, std::max(0.0f, screen.height - 56.0f))
                        .content([&] { /* ... */ })
                        .build();
                })
                .build();

            // 弹层放最后，保证在最上层
            components::dialog(ui, "confirm")
                .open(showDialog)
                .screen(screen.width, screen.height)
                .title("Confirm")
                .message("Apply?")
                .onPrimary([&] { showDialog = false; })
                .onSecondary([&] { showDialog = false; })
                .onOpenChange([&](bool open) { showDialog = open; })
                .build();
        })
        .build();
}
```

### 9.4 受控控件的标准写法

```cpp
static bool remember = false;
static float volume = 0.5f;
static int mode = 0;

components::checkbox(ui, "opt.remember")
    .theme(tokens)
    .checked(remember)
    .text("Remember me")
    .onChange([&](bool v) { remember = v; })
    .build();

components::slider(ui, "opt.volume")
    .theme(tokens)
    .size(320.0f, 34.0f)
    .value(volume)                      // 0.0 - 1.0
    .onChange([&](float v) { volume = v; })
    .build();

components::segmented(ui, "opt.mode")
    .items({"Compact", "Normal", "Comfort"})
    .selected(mode)
    .onChange([&](int i) { mode = i; })
    .build();
```

### 9.5 长列表（虚拟化，别用 clip 硬撑）

```cpp
float listOffset = 0.0f;

components::virtualList(ui, "logs.virtual")
    .theme(tokens)
    .size(520.0f, 300.0f)
    .itemCount(1000000)
    .rowHeight(32.0f)
    .offset(listOffset)
    .step(64.0f)
    .overscanViewports(1.0f)
    .onChange([&](float v) { listOffset = v; })
    .row([&](eui::Ui& rowUi, const std::string& rowId, auto index, float w, float h) {
        rowUi.text(rowId + ".text")
            .size(w, h)
            .text("row " + std::to_string(index))
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
    })
    .build();
```

行回调的 `rowId` 是**可复用 slot id**，业务状态要按真实 `index`/数据 key 存在页面里。

### 9.6 自定义小组件模板（primitives 组合）

```cpp
class BadgeBuilder {
public:
    BadgeBuilder(eui::Ui& ui, std::string id) : ui_(ui), id_(std::move(id)) {}
    BadgeBuilder& size(float w, float h) { w_ = w; h_ = h; return *this; }
    BadgeBuilder& text(std::string v) { text_ = std::move(v); return *this; }
    BadgeBuilder& theme(const components::theme::ThemeColorTokens& t) {
        fill_ = components::theme::withAlpha(t.primary, 0.16f);
        textColor_ = t.primary;
        return *this;
    }

    void build() {
        const auto tr = eui::Transition::make(0.18f, eui::Ease::OutCubic);
        ui_.stack(id_)
            .size(w_, h_)
            .content([&] {
                ui_.rect(id_ + ".bg")
                    .fill()
                    .color(fill_)
                    .radius(h_ * 0.5f)
                    .transition(tr)
                    .build();
                ui_.text(id_ + ".text")
                    .fill()
                    .text(text_)
                    .fontSize(13.0f)
                    .color(textColor_)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .transition(tr)
                    .build();
            })
            .build();
    }

private:
    eui::Ui& ui_;
    std::string id_;
    std::string text_ = "Badge";
    float w_ = 96.0f, h_ = 30.0f;
    eui::Color fill_{0.16f, 0.28f, 0.50f, 1.0f};
    eui::Color textColor_{0.80f, 0.88f, 1.0f, 1.0f};
};

inline BadgeBuilder badge(eui::Ui& ui, const std::string& id) { return BadgeBuilder(ui, id); }
```

要点：工厂函数 lowerCamelCase；组件根 id 由调用方给，内部用 `id + ".part"`；组件不持有业务状态，纯交互态走 `ui.state<T>(id)`；不 new primitive、不绕 Runtime。

### 9.7 折叠/展开动画的正确姿势

外层容器尺寸**固定不变**，让内部叶子自己插值：

```cpp
constexpr float kMaxH = 238.0f;
const float h = expanded ? kMaxH : 148.0f;
const float top = (kMaxH - h) * 0.5f;

ui.stack("card").size(width, kMaxH).content([&] {     // 外层不随状态改尺寸
    ui.rect("card.bg")
        .position(0.0f, top)
        .size(width, h)
        .transition(0.30f, eui::Ease::OutCubic)
        .animate(eui::AnimProperty::Frame)
        .build();
}).build();
```

---

## 10. 边界（哪些别碰）

- 不要绕过 Runtime：不 new backend primitive，不读 GLFW/SDL 状态，不手动算脏区。
- 不要为了某个效果关闭 retained layer 缓存。
- 不要往 `examples/` 加自己的东西——那是内置示例/回归夹具，用户 app 走 `apps/`。
- 想改框架层（`core/`、`components/`）也可以，但改动要按项目标准验证：`git diff --check` + 重新 configure + 编目标；涉及渲染/着色器/保留层时 OpenGL 和 Vulkan 都要过；不要用"最小化窗口"冒充真实负载。
- 不要指望在框架层"补一个 CSS 似的东西"——那等于重写 Locale，方向就不对。

---

## 11. 文档与代码地图（下次直接跳这里）

**官方文档**（2026-10-04 起框架文档已迁出本仓库，统一看 EUI-NEO 独立克隆 `D:\编译开发\EUI-NEO\docs\`）：

| 文件 | 什么时候看 |
| --- | --- |
| `DSL.md` | DSL 元素全貌、颜色、Rect/Text/Image/Polygon/Shadertoy、变换、动画、Runtime 行为、限制 |
| `组件.md` | 35 个组件的参数表 + 状态归属 + 使用示例（最常翻） |
| `布局.md` | Row/Column/Stack/Flow 语义、SizeValue、flex、ignoreLayout、限制 |
| `状态.md` | 受控组件、`ui.state<T>`、Signal 绑定 |
| `事件.md` | 事件模型、热区选型、命中与 focus |
| `动画.md` | 过渡与可动画属性 |
| `图片.md` / `动态纹理.md` | 图片源、SVG、GIF、stream、外部 GPU 纹理 |
| `Shadertoy.md` | 多 pass graph、uniform、双后端矩阵 |
| `渲染后端架构.md` / `retained_layer_cache.md` | 想理解脏区/保留层/双后端时 |
| `异步.md` | `app::async` 后台任务 |
| `网络.md` / `audio.md` / `平台能力.md` | 网络、miniaudio 播放、平台集成 |
| `科学绘图TODO.md` | 规划中，**不是现状** |
| `开发与发布.md` / `集成指南.md` | 构建、安装、FetchContent、SDL2/Vulkan 选择 |

**设计方法论（项目给 AI 代理写的 skill，其实也是最好的写法指南）**：

- `D:\编译开发\EUI-NEO\docs\skills\eui-neo-ui-replicator\SKILL.md` —— 复刻/设计流程、决策顺序、布局规则、性能规则、动效预算、CSS→DSL 映射表。
- `components/workshop/SKILL.md` —— 把网页/CSS 组件移植成 workshop 组件的完整流程（状态清单、图层映射、阴影处理、id 规则）。

**关键代码位置**：

| 位置 | 内容 |
| --- | --- |
| `core/dsl.h:1396-1438` | `Ui` 的图元工厂（10 个入口） |
| `core/dsl.h:302-833` | 通用布局/交互/回调/绑定方法（BuilderBase） |
| `core/dsl.h:836-1034` | 形状与样式方法（ShapeBuilderBase：渐变/圆角/边框/阴影/模糊/变换/states） |
| `components/components.h` | 组件导出总清单（权威） |
| `components/theme.h` | token 定义与派生色助手 |
| `core/layout.h` | 布局纯计算（Row/Column/Stack/Flow + flex） |
| `core/render/render_types.h:29-130` | `Color` / `Gradient` / `Border` / `Shadow` / `Transform` |
| `core/animation.h` | Ease / AnimProperty / Transition |
| `core/runtime/*.h` | layout→update→hit-test→render 全链路（出问题才需要下去看） |
| `apps/gallery/pages/*.h` | 多页应用的官方写法参考（Controls/Style/Animation/Layout/Bing/Settings/About 七页） |
| `examples/animated_card.cpp` | frame/transform/颜色/圆角/边框/阴影/透明度动画的完整范例 |
| `tests/fixtures/*_viewer.cpp` | 布局回归、文本对齐、千万行虚拟列表、markdown、滚动压力等可视化夹具 |

**可以直接当参考的上游 app**：`apps/calculator`、`apps/clock`、`apps/gallery`、`apps/sidebar_starter`（弹层/侧栏状态模式起手）、`apps/serial_tool`（托盘 + 配置面板 + 日志）、`apps/vcd_viewer`（波形 + 虚拟化滚动）、`apps/card_slider`、`apps/promo`。

---

## 12. 下次开工前要定的三件事

1. **工作目录**：本仓库 `apps/`（零配置、和 upstream 混在一起）还是独立项目 + `3rd/EUI-NEO`（干净、长期）。app 代码是同一套，先做再搬也行。
2. **后端**：默认 GLFW + OpenGL 就够（也是上游主验证路径）；真要试 Vulkan 得单独建目录/目标验证。
3. **是否 fork**：如果打算改 `core/`/`components/` 并希望自己攒着，fork 一份比在克隆里改舒服；Apache 2.0，想回馈上游也没限制。

在没定之前，**路线 A + 默认后端** 是阻力最小的验证路径。
