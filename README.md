# coding-agent — Android Termux 自主编程助手

> **专为 Android Termux 环境打造的 AI 编程助手**，由 DeepSeek / 智谱 GLM 等大模型驱动，在终端中自主完成代码编写、重构、调试等任务。

本项目是一个运行在 **Android Termux** 终端中的自主软件工程助手（autonomous coding agent）。它使用 C++20 编写，**零第三方运行时依赖**（仅需 libcurl），自带 JSON 解析器、HTTP 客户端和工具调度引擎，可在 Termux 中流畅运行。

---

## 📱 适用环境

| 环境 | 说明 |
|------|------|
| **Android Termux** ✅ | 唯一支持的目标平台，已内置 Termux 前缀探测与 libc++ ABI 适配 |

---

## ✨ 功能特性

### 🤖 自主 Agent 循环

- 模型可连续调用工具（读文件、写文件、执行命令等），直到完成任务或达到迭代上限
- 支持**流式输出**，实时显示模型回复与工具调用过程
- 内置**重复检测**：连续相同工具调用超过 3 次自动终止，防止死循环

### 🛠️ Agent 工具集（模型可调用的工具）

Agent 模型可通过 Function Calling 机制调用以下 **38 个工具**，自主完成代码编写、文件操作、网络请求、数据解析等任务：

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
| `speak` | 文字转语音（TTS），让手机开口说话 |
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
| `screenshot` | 截取手机屏幕（结合 ocr 可分析屏幕内容） |
| `plot_chart` | 根据数据生成图表（柱状图/折线图/饼图/散点图） |
| `finish` | 标记任务完成并返回最终答复 |

### 🧩 新增工具详解

#### 📱 Termux 特色工具（需安装 termux-api）

以下工具利用 Termux:API 与 Android 系统交互，让 Agent 突破终端限制：

| 工具 | 依赖 | 说明 |
|------|------|------|
| `clipboard` | `termux-clipboard-get/set` | 读写系统剪贴板，Agent 可直接把代码放剪贴板 |
| `notify` | `termux-notification` | 发送通知到通知栏，长时间任务完成时提醒 |
| `speak` | `termux-tts-speak` | 文字转语音，Agent 能"开口说话" |
| `vibrate` | `termux-vibrate` | 震动反馈，出错或完成时震动提醒 |
| `screenshot` | `termux-screencap` | 截取屏幕，结合 `ocr` 可分析屏幕内容 |
| `system_info` | `termux-battery-status` | 获取电池/CPU/内存/存储/网络信息 |

> 安装：`pkg install termux-api` 即可获得以上所有功能

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

#### 🎮 组合技示例

```
screenshot → ocr → "屏幕上显示编译错误：..."
                → notify "发现编译错误！"
                → speak "主人，代码出问题了，我来修复"
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
| `deepseek-chat` | DeepSeek 通用对话模型（默认） |

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

```bash
pkg update
pkg install clang cmake libcurl git libandroid-spawn
```

---

## 🚀 编译

```bash
# 在项目根目录执行
cmake -B build && cmake --build build
```

编译产物为 `build/coding-agent`。

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
./build/coding-agent --provider deepseek --model deepseek-chat

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
  -m, --model <name>      模型 id，如 deepseek-chat、glm-4.5
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
├── CMakeLists.txt        # 构建配置（C++20，libcurl，Termux 探测）
├── build/
│   └── coding-agent      # 编译产物
└── src/
    ├── main.cpp          # 入口：参数解析、REPL、Agent 循环、系统提示词、终端流控制
    ├── llm.hpp           # OpenAI 兼容 chat-completion 客户端 + tool calling
    ├── tools.hpp         # 文件/Shell 工具实现与 JSON Schema 定义
    ├── context.hpp       # 上下文版本管理 / 回退（快照、回滚、撤销）
    ├── git.hpp           # Git 集成：可用性检查、仓库初始化、自动提交
    ├── http.hpp          # libcurl 轻量封装（JSON POST / Bearer 鉴权）
    ├── json.hpp          # 自包含 JSON 值/解析器/序列化器（无外部依赖）
    └── markdown.hpp      # Markdown → 终端 ANSI 渲染器
```

---

## 🧠 工作原理

1. **系统提示词**：启动时根据工作区根目录生成系统提示，告知模型可用工具（共 38 个，详见上方工具集表格）与行为准则（先探索再修改、用 `write_file` 落地改动、用 `run_command` 验证构建/测试等）。
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
