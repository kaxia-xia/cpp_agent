# coding-agent — 自主编程助手

> **适用于 Android Termux、x64/ARM64 Linux 和 Windows x64 的 AI 编程助手**，由 DeepSeek / 智谱 GLM 等大模型驱动，在终端中自主完成代码编写、重构、调试等任务。

本项目是一个运行在终端中的自主软件工程助手（autonomous coding agent）。它使用 C++20 编写，**零第三方运行时依赖**（仅需 libcurl），自带 JSON 解析器、HTTP 客户端和工具调度引擎，可在 Termux、普通 Linux（x64 / ARM64）和 Windows x64 上流畅运行。

---

## 📱 适用环境

| 环境 | 说明 |
|------|------|
| **Android Termux** ✅ | 主要目标平台，已内置 Termux 前缀探测与 libc++ ABI 适配 |
| **ARM64 Linux** ✅ | Ubuntu / Debian / Armbian 等普通 ARM64 发行版，零 Android 依赖 |
| **x64 Linux** ✅ | Ubuntu / Debian / Fedora 等普通 x86_64 发行版，零 Android 依赖 |
| **Windows x64** ✅ | WinLibs (独立 MinGW-w64) 或 Visual Studio (MSVC)，纯 Windows 原生 |

---

## 🚀 三平台快速安装速查

> 三台设备对应三种工具链，各自只需「装依赖 → 编译 → 安装」三步。下方命令是**最小可用集**，完整说明与可选工具依赖见下文各节。

### 📱 Android Termux

```bash
pkg update && pkg install clang cmake libcurl git libandroid-spawn
# 可选：agent 工具依赖（按需）
pkg install python tesseract ffmpeg termux-api

cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake --install build          # 装到 ~/.local/bin
```

### 🐧 ARM64 / x64 Linux（Ubuntu / Debian）

```bash
sudo apt update && sudo apt install cmake g++ libcurl4-openssl-dev git
# ⚠️ g++ 必须 ≥ 13（Ubuntu 22.04 默认是 11，会编译失败），详见「普通 Linux」一节

cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" && cmake --build build  # -O2 替代默认 -O3，避免小内存设备（如香橙派）编译时内存耗尽卡死
cmake --install build          # 装到 ~/.local/bin
```

### 🪟 Windows 10 x64（WinLibs MinGW-w64，推荐）

```cmd
:: 1. 下载 WinLibs GCC x64 UCRT 便携版，解压到 C:\mingw64，把 C:\mingw64\bin 加入 PATH
:: 2. 安装 CMake 与 Ninja 并加入 PATH
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build
```

### 🔑 三平台通用的运行前设置

```bash
export DEEPSEEK_API_KEY="sk-..."     # DeepSeek
# 或 export ZHIPU_API_KEY="..."       # 智谱 GLM

coding-agent                          # 安装后直接运行（或 ./build/coding-agent）
```

---

## ✨ 功能特性

### 🤖 自主 Agent 循环

- 模型可连续调用工具（读文件、写文件、执行命令等），直到完成任务或达到迭代上限
- 支持**流式输出**，实时显示模型回复与工具调用过程
- 内置**重复检测**：连续相同工具调用超过 3 次自动终止，防止死循环

### 🛠️ Agent 工具集（模型可调用的工具）

Agent 模型可通过 Function Calling 机制调用以下 **47 个工具**，自主完成代码编写、文件操作、网络请求、数据解析等任务：

| 工具 | 作用 |
|------|------|
| `read_file` | 读取工作区内文本文件（支持 offset/limit 分段读取大文件） |
| `write_file` | 创建/覆盖文件（自动创建父目录） |
| `list_dir` | 列出目录条目 |
| `run_command` | 通过 `/bin/sh -c` 执行 shell 命令（带超时与退出码） |
| `edit_file` | 替换文件中首次出现的精确文本（精准定位修改） |
| `delete_file` | 删除文件或空目录 |
| `rename_file` | 重命名/移动文件或目录 |
| `copy_file` | 复制文件或目录（自动创建父目录） |
| `append_file` | 追加内容到文件末尾（文件不存在则自动创建） |
| `search_text` | 在文件中搜索文本或正则模式（基于 grep） |
| `find_files` | 按 glob 模式查找文件（如 `*.cpp`、`*.{hpp,h}`） |
| `file_info` | 获取文件/目录元数据（大小、类型、权限、修改时间） |
| `read_multiple_files` | 一次读取多个文件（比多次调用 read_file 更高效） |
| `write_multiple_files` | 一次写入多个文件（比多次调用 write_file 更高效） |
| `fetch_url` | HTTP GET 获取 URL 内容（读取网页、API、文本） |
| `parse_html` | 解析 HTML 内容，提取文本/链接/CSS 选择器匹配 |
| `parse_xml` | 解析 XML 内容，支持 XPath 查询 |
| `parse_json` | 解析 JSON 内容，支持点号路径查询（如 `data.items[0].name`） |
| `render_mermaid` | 将 Mermaid 图表定义渲染为 SVG 图片文件 |
| `image_info` | 获取图片元数据（格式、尺寸、色彩模式） |
| `image_convert` | 转换图片格式或调整尺寸（支持 PNG/JPEG/GIF/BMP/WebP） |
| `image_to_svg` | 将位图嵌入为 base64 SVG（或使用 potrace 矢量化） |
| `clipboard` | 读取/写入 Android 系统剪贴板 |
| `notify` | 发送 Android 通知到通知栏 |
| `vibrate` | 手机震动反馈 |
| `run_python` | 执行 Python 代码片段并返回结果 |
| `ocr` | 图片文字识别（OCR），基于 Tesseract 引擎 |
| `qr_encode` | 生成二维码图片 |
| `qr_decode` | 解码图片中的二维码/条形码 |
| `diff_files` | 对比两个文件的差异（unified diff 格式） |
| `compress` | 创建压缩归档（zip/tar.gz） |
| `decompress` | 解压归档文件（zip/tar.gz/tar.bz2/tar.xz） |
| `system_info` | 获取 Android 设备信息（电池/CPU/内存/存储/网络） |
| `weather` | 查询天气和预报（基于 wttr.in） |
| `get_location` | 获取当前地理位置（经纬度），基于 termux-location |
| `get_datetime` | 获取当前日期和时间，支持自定义格式 |
| `screenshot` | 截取手机屏幕（结合 ocr 可分析屏幕内容） |
| `plot_chart` | 根据数据生成图表（柱状图/折线图/饼图/散点图） |
| `create_image` | 创建任意尺寸的空白图片（支持 RGB/RGBA/灰度模式） |
| `create_video` | 将多张图片组合成视频（支持 mp4/webm/gif 等格式） |
| `read_pixel` | 精确读取图片中某个像素的 RGBA 颜色值 |
| `draw_pixel` | 精确设置图片中某个像素的颜色 |
| `draw_rect` | 在图片上绘制矩形（支持填充和描边） |
| `draw_line` | 在图片上绘制线段 |
| `show_image` | 在 Termux 中显示图片（系统查看器或 ASCII 艺术） |
| `finish` | 标记任务完成并返回最终答复 |

### 🧩 新增工具详解

#### 📱 Termux 特色工具（需安装 termux-api）

以下工具利用 Termux:API 与 Android 系统交互，让 Agent 突破终端限制：

| 工具 | 依赖 | 说明 |
|------|------|------|
| `clipboard` | `termux-clipboard-get/set` | 读写系统剪贴板，Agent 可直接把代码放剪贴板 |
| `notify` | `termux-notification` | 发送通知到通知栏，长时间任务完成时提醒 |
| `vibrate` | `termux-vibrate` | 震动反馈，出错或完成时震动提醒 |
| `screenshot` | `termux-screencap` | 截取屏幕，结合 `ocr` 可分析屏幕内容 |
| `system_info` | `termux-battery-status` | 获取电池/CPU/内存/存储/网络信息 |

> ⚠️ **安装步骤**：
> 1. `pkg install termux-api` — 安装命令行工具
> 2. 下载安装 **Termux:API Android App**（[F-Droid](https://f-droid.org/packages/com.termux.api/) 或 [GitHub](https://github.com/termux/termux-api/releases)）
> 3. 在系统设置中授予 Termux:API **通知、剪贴板**等权限
>
> 缺少第 2 步会导致工具调用卡住或失败！

#### 🔍 OCR 文字识别

基于 **Tesseract OCR** 引擎，支持多语言识别：

```bash
# 安装（已预装）
pkg install tesseract

# 当前支持的语言
tesseract --list-langs
# 输出: chi_sim  eng
```

| 语言参数 | 说明 |
|----------|------|
| `lang="eng"` | 英文识别（默认） |
| `lang="chi_sim"` | 简体中文识别 |
| `lang="eng+chi_sim"` | 中英文混合识别 |

> 如需更多语言，从 [tesseract-ocr/tessdata](https://github.com/tesseract-ocr/tessdata) 下载 `.traineddata` 文件放到 `/data/data/com.termux/files/usr/share/tessdata/` 目录

#### 🐍 run_python — 快速执行 Python

预导入常用库，无需写 import：

```python
# 自动已导入：sys, json, math, random, datetime, os, re, collections, itertools, statistics
result = sum(range(1000))
print(f"计算结果: {result}")
```

#### 📈 plot_chart — 数据可视化

支持四种图表类型，数据以 JSON 格式传入：

```json
// 柱状图/折线图
{"labels": ["A","B","C"], "values": [10, 20, 15], "xlabel": "类别", "ylabel": "数量"}

// 饼图
{"labels": ["苹果","香蕉","橘子"], "values": [30, 20, 15]}

// 散点图
{"x": [1,2,3,4,5], "y": [2,4,1,5,3], "xlabel": "X轴", "ylabel": "Y轴"}
```

#### 🌤️ weather — 天气查询

基于 [wttr.in](https://wttr.in) 服务，无需 API Key：

- 不传 `location` → 自动根据 IP 定位
- `location="Beijing"` → 查询指定城市
- `location="39.9,116.4"` → 查询坐标位置

#### 🎬 视频创建工具

| 工具 | 说明 |
|------|------|
| `create_video` | 将多张图片组合成视频，支持任意数量的图片 |

**参数说明：**
- `output` - 输出视频文件路径，扩展名决定格式（`.mp4`、`.webm`、`.avi`、`.gif` 等）
- `images` - 图片路径数组，按顺序组合成视频
- `fps` - 帧率（默认 24），控制播放速度
- `loop` - 循环次数（默认 1），仅 GIF 有效，0 表示无限循环
- `duration_per_image` - 每张图片显示时长（秒），设置后覆盖 fps

**组合示例：创建图片序列并合成视频**
```
# 创建一系列图片
create_image("frame001.png", width=320, height=240, color="red")
draw_rect("frame001.png", x1=10, y1=10, x2=310, y2=230, outline="white")
draw_text? → 可添加文字

create_image("frame002.png", width=320, height=240, color="blue")
...

# 合成视频
create_video("animation.mp4", 
    images=["frame001.png", "frame002.png", "frame003.png", ...],
    fps=30)
  → [ok: created video 'animation.mp4' (10 images, 30 fps, 123456 bytes)]
```

> 💡 依赖 ffmpeg：`pkg install ffmpeg`

#### 🎨 图片创建与像素级编辑工具

新增 5 个图片工具，支持从零创建图片、精确读写像素、绘制基本图形：

| 工具 | 说明 |
|------|------|
| `create_image` | 创建任意尺寸的空白图片（1x1 ~ 10000x10000），支持 RGB/RGBA/灰度模式，可指定背景色 |
| `read_pixel` | 精确读取图片中某个像素的 RGBA 颜色值（返回十进制和十六进制） |
| `draw_pixel` | 精确设置图片中某个像素的颜色（支持 hex、命名颜色、transparent） |
| `draw_rect` | 在图片上绘制矩形（支持 fill 填充色、outline 描边色、outline_width 描边宽度） |
| `draw_line` | 在图片上绘制线段（支持 color 颜色、line_width 线宽） |

**组合示例：创建并编辑图片**
```
create_image("myart.png", width=200, height=100, color="#4488ff", mode="RGB")
  → [ok: created image 'myart.png' (200x100, RGB mode, ...)]

draw_rect("myart.png", x1=10, y1=10, x2=190, y2=90, fill="#ffcc00", outline="white", outline_width=3)
  → [ok: drew rectangle (10,10)-(190,90) [181x81px]]

draw_line("myart.png", x1=0, y1=0, x2=199, y2=99, color="red", line_width=2)
  → [ok: drew line from (0,0) to (199,99)]

read_pixel("myart.png", x=100, y=50)
  → Pixel at (100,50): RGBA: (255, 204, 0, 255), Hex: #ffcc00
```

> 💡 这些工具依赖 Python Pillow 库：`pip install Pillow`

#### 🎮 组合技示例

```
screenshot → ocr → "屏幕上显示编译错误：..."
                → notify "发现编译错误！"
                → vibrate (震动提醒你)
```

```
clipboard (get) → "获取剪贴板中的代码"
                → run_python "分析这段代码..."
                → clipboard (set) "把优化后的代码放回剪贴板"
```

```
weather → "今天下雨，不适合出门"
        → "正好在家写代码！"
```

#### 🎬 视频 + 图片组合创作示例

```
# 1. 创建一系列帧图片
create_image("frame_001.png", width=640, height=480, color="#1a1a2e", mode="RGB")
draw_rect("frame_001.png", x1=50, y1=50, x2=590, y2=430, fill="#16213e", outline="#0f3460", outline_width=2)
draw_line("frame_001.png", x1=100, y1=240, x2=540, y2=240, color="#e94560", line_width=3)

create_image("frame_002.png", width=640, height=480, color="#1a1a2e", mode="RGB")
draw_rect("frame_002.png", x1=50, y1=50, x2=590, y2=430, fill="#16213e", outline="#0f3460", outline_width=2)
draw_line("frame_002.png", x1=120, y1=240, x2=520, y2=240, color="#e94560", line_width=3)
# ... 创建更多帧 ...

# 2. 读取某个像素确认颜色
read_pixel("frame_001.png", x=100, y=240)
  → Pixel at (100,240): RGBA: (233, 69, 96, 255), Hex: #e94560

# 3. 合成视频
create_video("animation.mp4",
    images=["frame_001.png", "frame_002.png", "frame_003.png", ...],
    fps=30)
  → [ok: created video 'animation.mp4' (10 images, 30 fps, 123456 bytes)]

# 4. 也生成 GIF 版本
create_video("animation.gif",
    images=["frame_001.png", "frame_002.png", ...],
    duration_per_image=0.1,
    loop=0)
  → [ok: created video 'animation.gif' (10 images, 10 fps, 45678 bytes)]
```

### 🔒 路径沙箱保护

### 🔒 路径沙箱保护

所有文件操作均限制在工作区根目录下，拒绝 `..` 路径穿越，安全可靠。

### 📝 Git 自动版本管理

- 启动时自动检查 git 可用性，若工作区不是 git 仓库则自动初始化
- 每轮对话后自动提交文件改动，方便用 `git restore` / `git checkout` 回退

### ⏸️ Ctrl+C 上下文保留（Context Preservation）

- 当用户在 AI 响应期间按下 **Ctrl+C**，当前对话的**上下文被完整保留**
- 用户的提示词、历史对话消息均保持不变，仅清除本次不完整的 AI 回复
- 自动为中断点保存一个带 `(interrupted)` 标签的快照，方便通过 `/back` 回退
- 中断后可直接输入新的提示词继续对话，无需重新描述任务

### ⏪ 上下文回退（Context Rollback）

- 自动为每轮对话保存上下文快照
- 可随时回退到之前任意版本（`/back <id>`）
- 支持撤销上一轮对话（`/undo`）
- 回退上下文时自动同步回退 git 文件改动

### ⏯️ 终端流控制（Ctrl+S / Ctrl+Q）

- 使用终端内核级的 **IXON 流控制**，无需原始模式（raw mode）或后台线程
- **Ctrl+S** 暂停所有输出（stdout + stderr），**Ctrl+Q** 恢复输出
- 在终端内核层面生效，对应用程序完全透明
- 启动时自动检测并启用 IXON，若终端不支持则提示用户

### 🎨 彩色终端输出

- 自动检测 TTY，Markdown 渲染（表格、代码块、标题等）
- 工具调用与结果以不同颜色区分显示

### 📊 Token 用量统计

- 每轮显示输入/输出 token 数
- `/tokens` 命令查看累计用量

---

## 🧠 支持的模型

### DeepSeek

| 模型 | 说明 |
|------|------|
| `deepseek-v4-pro` | DeepSeek 通用对话模型（默认） |

- **Provider 名称**: `deepseek`
- **API 地址**: `https://api.deepseek.com/v1`
- **密钥环境变量**: `DEEPSEEK_API_KEY`

### 智谱 GLM

| 模型 | 说明 |
|------|------|
| `glm-4-flash` | 快速响应（~2 秒），适合日常使用（默认） |
| `glm-4` | 平衡模型（~5 秒） |
| `glm-4.5` | 最强模型（~16 秒），适合复杂任务 |

- **Provider 名称**: `glm`
- **API 地址**: `https://open.bigmodel.cn/api/paas/v4`
- **密钥环境变量**: `ZHIPU_API_KEY`

> 两者均兼容 OpenAI Chat Completions + Function Calling 协议，可方便地扩展更多 provider。

---

## 🔧 安装依赖（Termux）

### 1️⃣ 基础编译依赖

```bash
pkg update
pkg install clang cmake libcurl git libandroid-spawn
```

### 2️⃣ Agent 工具依赖（按需安装）

Agent 的 47 个工具中，部分需要额外安装软件才能使用。以下是完整清单：

| 工具 | 所需安装 | 说明 |
|------|----------|------|
| `notify` / `clipboard` / `vibrate` / `screenshot` / `system_info` | `pkg install termux-api` | 需同时安装 **Termux:API Android App**（[F-Droid](https://f-droid.org/packages/com.termux.api/) 或 [GitHub Releases](https://github.com/termux/termux-api/releases)），否则这些工具会卡住或失败 ❗ |
| `ocr` | `pkg install tesseract` | 已预装 `eng`（英文）和 `chi_sim`（简体中文）语言包 |
| `qr_encode` / `qr_decode` | `pkg install python` + `pip install qrcode pyzbar` | 二维码生成与解码 |
| `plot_chart` | `pkg install python` + `pip install matplotlib` | 数据可视化图表 |
| `render_mermaid` | `npm install -g @mermaid-js/mermaid-cli` | Mermaid 图表渲染为 SVG |
| `image_info` / `image_convert` / `image_to_svg` / `show_image` (ASCII 模式) | `pkg install python` + `pip install Pillow` | 图片处理（读取、转换、嵌入 SVG、ASCII 艺术渲染） |
| `create_image` / `read_pixel` / `draw_pixel` / `draw_rect` / `draw_line` | `pkg install python` + `pip install Pillow` | 图片创建与像素级编辑 |
| `create_video` | `pkg install ffmpeg` | 将多张图片组合成视频（MP4/GIF/WebM） |
| `weather` | 无需安装 | 基于 wttr.in 在线服务 |
| `fetch_url` | 无需安装 | 内置 libcurl HTTP 客户端 |
| 文件操作类工具（`read_file` / `write_file` / `run_command` 等） | 无需安装 | 内置 C++ 实现 |
| `diff_files` / `compress` / `decompress` | 无需安装 | 调用系统 `diff`、`zip`、`tar` 命令 |

> ⚠️ **重要提醒**：`termux-api` 包安装后，还需要去 [F-Droid](https://f-droid.org/packages/com.termux.api/) 或 [GitHub](https://github.com/termux/termux-api/releases) **下载安装 Termux:API Android App**，并在系统设置中授予其通知、剪贴板等权限。仅 `pkg install termux-api` 是不够的！

#### 🐍 使用 Python 的工具一览

以下工具在实现中依赖 Python 解释器（`python3`）及部分第三方库。`pkg install python` 安装的是 **Termux 官方 Python 包**（`python`），它自带 `pip`，可直接安装第三方库。

| 工具 | 依赖的 Python 包 | 安装命令 | 说明 |
|------|------------------|----------|------|
| `run_python` | 仅标准库 | `pkg install python` | 执行用户提供的 Python 代码片段，预导入 `sys`、`json`、`math`、`random`、`datetime`、`os`、`re`、`collections`、`itertools`、`statistics` |
| `parse_html` | 仅标准库（`html.parser`） | `pkg install python` | 解析 HTML 内容，提取文本/链接/CSS 选择器匹配 |
| `parse_xml` | 仅标准库（`xml.etree.ElementTree`） | `pkg install python` | 解析 XML 内容，支持 XPath 查询 |
| `parse_json` | 仅标准库（`json`） | `pkg install python` | 解析 JSON 内容，支持点号路径查询 |
| `get_location` | 仅标准库（`json`） | `pkg install python` | 解析 `termux-location` 返回的 JSON 定位数据 |
| `image_info` | **Pillow**（`PIL`） | `pkg install python && pip install Pillow` | 获取图片元数据（格式、尺寸、色彩模式） |
| `image_convert` | **Pillow**（`PIL`） | `pkg install python && pip install Pillow` | 转换图片格式或调整尺寸 |
| `image_to_svg` | **Pillow**（`PIL`） | `pkg install python && pip install Pillow` | 将位图嵌入为 base64 SVG |
| `show_image` (ASCII 模式) | **Pillow**（`PIL`） | `pkg install python && pip install Pillow` | 将图片渲染为 ASCII 艺术字符画 |
| `create_image` / `read_pixel` / `draw_pixel` / `draw_rect` / `draw_line` | **Pillow**（`PIL`） | `pkg install python && pip install Pillow` | 创建图片、像素级读写、绘制基本图形 |
| `qr_encode` | **qrcode** | `pkg install python && pip install qrcode` | 生成二维码图片 |
| `qr_decode` | **Pillow** + **pyzbar** | `pkg install python && pip install Pillow pyzbar` | 解码图片中的二维码/条形码（无 pyzbar 时自动降级到 `zbarimg`） |
| `plot_chart` | **matplotlib** | `pkg install python && pip install matplotlib` | 根据数据生成图表（柱状图/折线图/饼图/散点图） |

> 💡 **提示**：仅使用标准库的工具（`run_python`、`parse_html`、`parse_xml`、`parse_json`、`get_location`）只需 `pkg install python`，无需额外 `pip install`。需要第三方库的工具会在调用失败时给出安装提示。

> 💡 **批量安装建议**：如果想一次性装好所有 Python 依赖，可以运行：
> ```bash
> pkg install python
> pip install Pillow qrcode pyzbar matplotlib
> ```

### 3️⃣ 编译与安装

```bash
# 在项目根目录执行
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

编译产物为 `build/coding-agent`。

#### 一键安装

```bash
# 安装到默认位置 ~/.local/bin（无需 sudo）
cmake --install build

# 或指定自定义安装前缀
cmake --install build --prefix /usr/local

# 也可以通过 cmake 变量预设置安装路径
cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/coding-agent && cmake --build build && cmake --install build
```

安装后可直接运行 `coding-agent`（需确保安装目录在 `PATH` 中，即 `~/.local/bin` 已在 PATH 或手动添加）。

> 💡 Termux 下 CMake 会自动探测 `$PREFIX` 并设置 Termux 特有的头文件/库搜索路径及 RPATH。

---

## 🔧 安装依赖（普通 Linux）

以下适用于 **Ubuntu 22.04+ / Debian 12+ / Fedora** 等普通 Linux 发行版（非 Android），**x86_64 与 ARM64 通用**。构建系统会根据 `__ANDROID__` 宏自动区分 Termux 与普通 Linux，无需额外配置。

### 1️⃣ 基础编译依赖

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install cmake g++ libcurl4-openssl-dev git
```

> **编译器说明**：项目需要 **C++20** 支持，且大量使用 `std::format`（GCC 13 才完整支持，否则会报 `std::format` 未定义的编译错误）。
> - **GCC 13+**（CMake 会自动检测可用编译器）
>
> ⚠️ **GCC 版本注意**：
> - **Ubuntu 24.04+ / Debian 13+ / Fedora** 默认自带 GCC 13+，直接 `apt install g++` 即可。
> - **Ubuntu 22.04 默认是 GCC 11，会编译失败**。可手动安装新版本并设为默认：
>
> ```bash
> sudo apt install gcc-13 g++-13
> sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100
> sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100
> # 若报"找不到 gcc-13 包"，需先添加 toolchain PPA：
> #   sudo add-apt-repository ppa:ubuntu-toolchain-r/test && sudo apt update
> ```
>
> > 💡 ARM64 与 x64 的安装命令**完全相同**，无需任何额外配置。

### 2️⃣ Agent 工具依赖（按需安装）

与 Termux 类似，部分工具需要额外软件：

| 工具 | Ubuntu/Debian 安装命令 | 说明 |
|------|------------------------|------|
| `ocr` | `sudo apt install tesseract-ocr tesseract-ocr-eng tesseract-ocr-chi-sim` | OCR 文字识别 |
| `qr_encode` / `qr_decode` | `sudo apt install python3-pip && pip install qrcode pyzbar` | 二维码生成与解码 |
| `plot_chart` | `sudo apt install python3-pip && pip install matplotlib` | 数据可视化图表 |
| `render_mermaid` | 安装 Node.js 后 `npm install -g @mermaid-js/mermaid-cli` | Mermaid 图表渲染 |
| `image_info` / `image_convert` / `image_to_svg` / `create_image` / `draw_*` 等图片工具 | `sudo apt install python3-pip && pip install Pillow` | 图片处理与创建 |
| `create_video` | `sudo apt install ffmpeg` | 图片合成视频 |
| `run_python` / `parse_html` / `parse_xml` / `parse_json` | `sudo apt install python3`（通常已预装） | Python 代码执行与数据解析 |
| `weather` | 无需安装 | 基于 wttr.in 在线服务 |
| `fetch_url` | 无需安装（已链接 libcurl） | 内置 HTTP 客户端 |
| 文件操作类工具 | 无需安装 | 内置 C++ 实现 |
| `diff_files` / `compress` / `decompress` | 无需安装（系统自带 diff、zip、tar） | 文件对比与压缩 |

> ⚠️ **不可用的 Termux 专属工具**：`notify`、`clipboard`、`vibrate`、`screenshot`、`system_info`、`get_location` 这 6 个工具依赖 **Termux:API**，在普通 Linux 上不可用。调用时程序会返回错误提示（不会崩溃）。

#### 🐍 Python 工具批量安装

```bash
# 一次性安装所有 Python 依赖
sudo apt install python3 python3-pip
pip install Pillow qrcode pyzbar matplotlib
```

### 3️⃣ 编译与安装

```bash
# 在项目根目录执行（低内存设备用 -O2 替代 -O3，避免编译卡死；内存充足的设备可去掉 CXX_FLAGS 参数）
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" && cmake --build build  # -O2 替代默认 -O3，避免小内存设备（如香橙派）编译时内存耗尽卡死
```

编译产物为 `build/coding-agent`。

#### 一键安装

```bash
# 安装到默认位置 ~/.local/bin
cmake --install build

# 或指定自定义目录
cmake --install build --prefix /usr/local
```

> 💡 CMake 在非 Termux 环境下不会设置 `$PREFIX` 路径，构建系统会自动跳过 Termux 特有的 RPATH 配置，使用标准 Linux 库搜索路径。

---

## 🔧 安装依赖（Windows x64）

以下适用于 **Windows 10/11 x64**。全程使用 **纯 Windows 原生工具链**，不依赖 MSYS2 / Cygwin / WSL 等任何类 Unix 环境。两条路线可选：**MinGW-w64（WinLibs）** 或 **MSVC（Visual Studio Build Tools）**。

### 方案一：MinGW-w64（WinLibs）+ 项目自带 libcurl（推荐，零下载）

> WinLibs 是 **MinGW-w64 GCC 的独立便携发行版**（解压即用，只含 `g++`/`gcc`/`windres`/`gdb`/`mingw32-make` 等编译器与工具，**不含 libcurl**）。它本质是原生的 Windows 程序，生成的原生 `.exe` 不依赖任何 POSIX 层。
>
> **libcurl 已随本项目附带**在 `third_party/curl-windows/`（curl 官方 Windows 包，含 `include/curl/*.h`、`lib/libcurl.dll.a` 导入库、`bin/libcurl-x64.dll`）。CMake 会自动找到它，**无需下载、无需 vcpkg、无需配置路径**。

1. 下载 [WinLibs](https://winlibs.com/) 的 **GCC x86_64 UCRT 运行时** 便携版（`.zip` 或 `.7z`），解压到 `C:\mingw64`（解压后应有 `C:\mingw64\bin\g++.exe`），并把 `C:\mingw64\bin` 加入 **PATH**：

```cmd
set PATH=C:\mingw64\bin;%PATH%
setx PATH "C:\mingw64\bin;%PATH%"
```

2. 安装 [CMake](https://cmake.org/download/) 和 [Ninja](https://ninja-build.org/)（解压加 PATH）。

3. 编译项目（**cmd 或 PowerShell** 均可，CMake 自动找到项目内 libcurl 并自动把 `libcurl-x64.dll` 复制到 exe 旁）：

```cmd
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物为 `build\coding-agent.exe`（同目录已有 `libcurl-x64.dll`，可直接运行）。

> 💡 `third_party/curl-windows/` 已随项目附带（含 curl 官方 Windows 包的 libcurl 开发文件），解压即可用，无需额外下载。

### 方案二：MinGW-w64 + vcpkg（备选，需从源码编译）

若官方 curl 包结构不符预期，可用 vcpkg 编译 MinGW 版 libcurl：

```cmd
git clone https://github.com/microsoft/vcpkg C:\vcpkg
cd /d C:\vcpkg
bootstrap-vcpkg.bat
vcpkg install curl:x64-mingw-static

:: 回到项目目录
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-mingw-static
cmake --build build
```

### 方案三：Visual Studio Build Tools（MSVC）+ vcpkg

> 若不想从源码编译 libcurl，用 MSVC 路线最省事——vcpkg 对 `x64-windows` 提供**官方预编译二进制**，一条命令秒装。

1. 安装 **Visual Studio Build Tools 2022**（免费，只需勾选「使用 C++ 的桌面开发」工作负载，无需完整 VS IDE）：https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
2. 安装 [vcpkg](https://github.com/microsoft/vcpkg)：

```cmd
git clone https://github.com/microsoft/vcpkg C:\vcpkg
cd /d C:\vcpkg
bootstrap-vcpkg.bat
.\vcpkg install curl:x64-windows
```

3. 用 vcpkg 工具链编译（在「x64 Native Tools Command Prompt for VS 2022」或 PowerShell 中）：

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

产物为 `build\Release\coding-agent.exe`。

### 一键安装

```powershell
# MinGW (WinLibs)
cmake --install build            # 默认安装到 %USERPROFILE%\.local\bin

# 或指定目录
cmake --install build --prefix C:\tools\coding-agent
```

> 💡 Windows 下 `run_command` 通过 `cmd.exe /C` 执行命令；终端流控（Ctrl+S/Ctrl+Q）与 `/dev/tty` 实时镜像在 Windows 上不可用，程序会自动降级。

### Agent 工具依赖（Windows）

部分工具在 Windows 下由内置实现替代了 POSIX 命令（`grep`/`find`/`diff` 改为 Python 脚本，`zip`/`unzip` 改为系统自带 bsdtar），因此依赖比 Linux 更少：

| 工具 | 所需安装 | 说明 |
|------|----------|------|
| `run_python` / `parse_html` / `parse_xml` / `parse_json` | Python 3（勾选 Add to PATH） | 仅标准库 |
| `search_text` / `find_files` / `diff_files` | Python 3（同上） | Windows 下由内置 Python 脚本实现 |
| `image_info` / `image_convert` / `image_to_svg` / `create_image` / `read_pixel` / `draw_pixel` / `draw_rect` / `draw_line` / `show_image`(ASCII) | Python 3 + `pip install Pillow` | 图片处理 |
| `qr_encode` | Python 3 + `pip install qrcode` | 二维码生成 |
| `qr_decode` | Python 3 + `pip install pyzbar` | 二维码解码（wheel 自带 zbar DLL） |
| `plot_chart` | Python 3 + `pip install matplotlib` | 图表绘制 |
| `ocr` | [Tesseract OCR](https://github.com/UB-Mannheim/tesseract/wiki)（勾选中文语言包） | OCR 识别 |
| `render_mermaid` | Node.js LTS + `npm install -g @mermaid-js/mermaid-cli` | Mermaid 渲染 |
| `create_video` | [ffmpeg](https://www.gyan.dev/ffmpeg/builds/)（解压加 PATH） | 视频合成 |
| `compress` / `decompress` | 无需安装 | Windows 10+ 自带 bsdtar（`tar`） |
| `weather` / `fetch_url` | 无需安装 | Windows 10+ 自带 `curl` |
| `show_image`(系统查看器) | 无需安装 | 使用 `start` 打开 |
| `get_datetime` / `get_calendar` | 无需安装 | C++ 内置实现 |
| 文件操作类工具 | 无需安装 | C++ 内置实现 |

> ❌ **Termux 专属工具在 Windows 上不可用**（返回错误提示，不会崩溃）：`notify`、`clipboard`、`vibrate`、`screenshot`、`system_info`、`get_location`。

### Windows x64 下开发 C/C++ / Win32 程序

使用 **独立 MinGW-w64（WinLibs）** 工具链，在 cmd / PowerShell 中即可直接编译 C/C++ 与 Win32 API 程序，无需任何类 Unix 环境。

**编译命令示例**

```cmd
# 控制台程序
g++ main.cpp -o app.exe -O2 -Wall -Wextra -std=c++20

# C 控制台程序
gcc main.c -o app.exe -O2 -Wall -std=c17

# GUI 窗口程序（Win32，不弹黑框）
g++ main.cpp -o app.exe -O2 -mwindows -lgdi32 -luser32 -lkernel32 -lcomctl32 -lshell32

# 宽字符版 Win32（wWinMain + L"..." 字符串）
g++ main.cpp -o app.exe -O2 -municode -mwindows -lgdi32 -luser32

# 带资源文件（图标/菜单/对话框/版本信息）
windres app.rc -o app_res.o && g++ main.cpp app_res.o -o app.exe -mwindows -lgdi32 -luser32 -lcomctl32

# 多文件
g++ a.cpp b.cpp c.cpp -o app.exe -O2 -std=c++20
```

**常用 Win32 链接库**

| 链接库 | 用途 |
|--------|------|
| `-luser32` | 窗口/控件/输入（`CreateWindow`、`MessageBox`、`GetMessage`） |
| `-lgdi32` | GDI 绘图（`Rectangle`、`TextOut`、`CreatePen`、`SelectObject`） |
| `-lcomctl32` | 通用控件（ListView、TreeView、Toolbar） |
| `-lshell32` | Shell 操作（`ShellExecuteW`、`SHGetFolderPath`） |
| `-lcomdlg32` | 通用对话框（`GetOpenFileName`、`ChooseColor`） |
| `-lole32` | OLE/COM（`CoInitialize`、`CoCreateInstance`） |
| `-lws2_32` | Winsock 网络（`socket`、`connect`、`send`） |
| `-ladvapi32` | 注册表与安全（`RegOpenKeyEx`） |
| `-lwinmm` | 多媒体（`PlaySound`、`timeBeginPeriod`） |
| `-luuid` | GUID 定义（常与 COM 一起用） |

**命令行环境说明**

agent 在 Windows 上的 `run_command` 走 `cmd.exe /C`，因此编译命令需使用 cmd 语法：

- ✅ 支持：`&&`、`|`、`>`、`>>`、`%VAR%`、`set VAR=...`
- ❌ 不支持（bash 语法会失败）：`$(...)`、反引号、`export`、`$VAR`、heredoc
- 运行程序：cmd 下 `app.exe`，PowerShell 下 `.\app.exe`
- 路径：`C:\dir\file` 或 `C:/dir/file` 均可，含空格需加引号

> 💡 MinGW-w64 默认产出原生 64 位 Windows `.exe`。

> 💡 **控制台编码与颜色**：coding-agent 启动时会自动把控制台输出/输入代码页设为 **UTF-8**（等价于 `chcp 65001`），并开启 **ANSI 虚拟终端（VT）处理**，因此中文不会乱码、彩色输出正常渲染，无需手动操作。输入侧通过 **`ReadConsoleW` 宽字符读取再转码为 UTF-8**，规避了 conhost 对窄字符（`std::cin`）UTF-8 输入丢字/乱码的问题。若终端不支持 VT（如极老的控制台），程序会自动降级为纯文本输出，不会出现 `35m` 这类转义序列残留。

---

## ⚙️ 配置 API 密钥

程序通过**环境变量**读取 API 密钥，支持以下方式：

### 方式一：export 环境变量（推荐）

```bash
# DeepSeek
export DEEPSEEK_API_KEY="sk-..."
./build/coding-agent

# 或 智谱 GLM
export ZHIPU_API_KEY="你的智谱API密钥"
./build/coding-agent --provider glm
```

### 方式二：单行命令临时设置

```bash
# DeepSeek
DEEPSEEK_API_KEY="sk-..." ./build/coding-agent

# 智谱 GLM
ZHIPU_API_KEY="你的智谱API密钥" ./build/coding-agent --provider glm
```

### 方式三：命令行参数

```bash
./build/coding-agent --api-key "sk-..."
```

---

## 💡 使用方式

### 1️⃣ 交互模式（REPL）

```bash
./build/coding-agent
```

进入 REPL 后输入提示词，单独一行输入 `.`（或 `Ctrl-D`）提交：

```
> 帮我把日志模块重构成异步的
.
```

### 2️⃣ 单次任务模式

```bash
# 方式一：--once 参数
./build/coding-agent --once "分析 src 目录并总结架构"

# 方式二：直接传位置参数（隐含 --once）
./build/coding-agent "修复 main.cpp 里的编译警告"
```

### 3️⃣ 指定 provider 和模型

```bash
# 使用 DeepSeek（默认）
./build/coding-agent --provider deepseek --model deepseek-v4-pro

# 使用智谱 GLM
./build/coding-agent --provider glm --model glm-4-flash

# 使用智谱最强模型
./build/coding-agent --provider glm --model glm-4.5
```

### 4️⃣ 指定工作目录

```bash
./build/coding-agent --root /data/data/com.termux/files/home/my-project
```

---

## 📖 命令行选项

```
coding-agent [OPTIONS] [--once "prompt"]

OPTIONS
  -p, --provider <name>   deepseek | glm            (默认: deepseek)
  -m, --model <name>      模型 id，如 deepseek-v4-pro、glm-4.5
      --api-key <key>     API key（否则读取环境变量）
  -r, --root <dir>        工作区根目录              (默认: 当前目录)
  -t, --temperature <f>   0.0 - 2.0                 (默认: 0.3)
      --max-tokens <n>    最大输出 token 数
      --max-iters <n>     Agent 循环上限            (默认: 100)
      --once "prompt"     执行单个任务后退出
  -h, --help              显示帮助

ENV
  DEEPSEEK_API_KEY        deepseek provider 的密钥
  ZHIPU_API_KEY           glm provider 的密钥
  CODING_AGENT_PROVIDER   默认 provider 覆盖
  CODING_AGENT_MODEL      默认模型覆盖
```

---

## 💬 REPL 命令

| 命令 | 说明 |
|------|------|
| `/help` | 显示帮助 |
| `/model NAME` | 切换模型 |
| `/provider N` | 切换 provider（`deepseek` \| `glm`） |
| `/clear` | 清空对话历史（同时重置版本历史） |
| `/tokens` | 查看累计 token 用量 |
| `/snap [label]` | 为当前上下文保存一个带标签的快照 |
| `/versions` | 列出所有已保存的上下文版本（别名 `/snaps`、`/history`） |
| `/back <id>` | 将上下文回退到指定版本（同时回退 git 文件改动） |
| `/undo` | 撤销最近一轮对话（回退到上一个版本，同时回退 git 文件改动） |
| `/exit` | 退出 |

### 终端快捷键

| 快捷键 | 说明 |
|--------|------|
| `Ctrl+S` | 暂停输出（终端流控制） |
| `Ctrl+Q` | 恢复输出 |
| `Ctrl+C` | 中断 AI 响应，保留对话上下文，返回提示符 |
| `Ctrl-D` | 提交提示词（在空行上按则退出） |

---

## ⏪ 上下文回退示例

```
> 帮我把日志模块重构成异步的
.
[... agent 完成第一轮 ...]
[12 in / 340 out]
> /snap 重构前
.
[snapshot #2 saved: 重构前]
> 现在加上批量刷新的支持
.
[... agent 完成第二轮，但效果不好 ...]
> /undo
.
[undone: back to version #2 (8 messages)]
[git] files restored to previous snapshot
> /versions
.
context versions (* = current):
  #1  14:02:01  1 msgs  init
* #2  14:02:30  8 msgs  重构前
```

回退后，后续对话将基于版本 #2 的上下文继续，仿佛第二轮从未发生过，且 git 文件也已同步回退。

---

## 🏗️ 项目结构

```
cpp_agent/
├── CMakeLists.txt        # 构建配置（C++20，libcurl，Termux 探测，一键安装）
├── build/
│   └── coding-agent      # 编译产物
├── third_party/
│   ├── README.md         # third_party 说明
│   └── curl-windows/     # Windows libcurl 开发文件（已随项目附带）
└── src/
    ├── main.cpp          # 入口：参数解析、REPL、Agent 循环、系统提示词、终端流控制
    ├── llm.hpp           # OpenAI 兼容 chat-completion 客户端 + tool calling
    ├── tools.hpp         # 文件/Shell 工具实现与 JSON Schema 定义
    ├── context.hpp       # 上下文版本管理 / 回退（快照、回滚、撤销）
    ├── git.hpp           # Git 集成：可用性检查、仓库初始化、自动提交
    ├── http.hpp          # libcurl 轻量封装（JSON POST / Bearer 鉴权）
    ├── json.hpp          # 自包含 JSON 值/解析器/序列化器（无外部依赖）
    ├── markdown.hpp      # Markdown → 终端 ANSI 渲染器
    └── platform.hpp      # 跨平台抽象（TTY/信号/时间/终端宽度/exe 目录）
```

---

## 🧠 工作原理

1. **系统提示词**：启动时根据工作区根目录生成系统提示，告知模型可用工具（共 47 个，详见上方工具集表格）与行为准则（先探索再修改、用 `write_file` 落地改动、用 `run_command` 验证构建/测试等）。
2. **Agent 循环**（`run_turn`）：
   - 将完整对话历史 + 工具 schema 发送给 LLM 的 `/chat/completions` 接口。
   - 若返回 `tool_calls`，逐个执行并把结果以 `tool` 角色消息回填到历史。
   - 重复直到模型不再调用工具（给出最终答复）或调用 `finish`。
   - 受 `--max-iters` 限制，防止无限循环。
3. **工具执行**（`tools::execute`）：解析 JSON 参数 → 路径沙箱校验 → 执行 → 截断过长输出（默认 60KB）后返回。
4. **Shell 执行**（`run_shell`）：用 `posix_spawn` 启动 `/bin/sh -c`，通过管道捕获合并的 stdout+stderr，`select` 实现超时，超时则 `SIGKILL` 终止。
5. **Markdown 渲染**：LLM 返回的 Markdown 文本在终端中自动渲染为带颜色和样式的输出。
6. **终端流控制（Ctrl+S / Ctrl+Q）**：利用终端内核自带的 **IXON 流控制**。当 IXON 启用时，终端驱动在**内核层面**拦截 Ctrl+S 和 Ctrl+Q——Ctrl+S 暂停 stdout/stderr 输出，Ctrl+Q 恢复输出，应用程序完全无感知。无需原始模式（raw mode）、无需信号处理、无需后台线程，也不会与 `std::getline` 等正常输入产生冲突。启动时通过 `ensure_ixon()` 检测并启用 IXON，若终端不支持则提示用户。
7. **上下文版本管理**（`context::History`）：每轮对话后保存消息历史的完整快照；回退时恢复指定快照并丢弃其后版本。
8. **Git 自动提交**（`git::commit_changes`）：每轮对话后执行 `git add -A && git commit`，记录文件级别的改动历史。

---

## ⚠️ 安全说明

- 文件工具通过 `resolve_under_root` 将路径规范化并校验是否位于工作区根目录内，阻止目录穿越。
- `run_command` 在工作区根目录执行，带硬超时（1–300 秒）。
- **请仅在可信的工作区中运行**：`run_command` 拥有执行任意 shell 命令的能力，等同于在终端手动执行。
- 上下文回退只回滚对话历史，**不回滚**工具对文件系统造成的实际改动。如需回退文件，请使用 Git 命令或 `/back` / `/undo`（它们会自动同步回退 git 文件）。

---

## 📝 许可证

本项目未附带显式许可证文件，默认保留所有权利。如需使用请先与作者确认。