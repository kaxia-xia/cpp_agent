# coding-agent — Android Termux 自主编程助手

> **专为 Android Termux 环境打造的 AI 编程助手**，由 DeepSeek / 智谱 GLM 等大模型驱动，在终端中自主完成代码编写、重构、调试等任务。

本项目是一个运行在 **Android Termux** 终端中的自主软件工程助手（autonomous coding agent）。它使用 C++20 编写，**零第三方运行时依赖**（仅需 libcurl），自带 JSON 解析器、HTTP 客户端和工具调度引擎，可在 Termux 中流畅运行。

---

## 📱 适用环境

| 环境 | 说明 |
|------|------|
| **Android Termux** ✅ | 首选目标平台，已内置 Termux 前缀探测与 libc++ ABI 适配 |
| 桌面 Linux | 也可运行，但项目构建系统优先为 Termux 优化 |

---

## ✨ 功能特性

### 🤖 自主 Agent 循环

- 模型可连续调用工具（读文件、写文件、执行命令等），直到完成任务或达到迭代上限
- 支持**流式输出**，实时显示模型回复与工具调用过程
- 内置**重复检测**：连续相同工具调用超过 3 次自动终止，防止死循环

### 🛠️ 内置工具集

| 工具 | 作用 |
|------|------|
| `read_file` | 读取工作区内文本文件 |
| `write_file` | 创建/覆盖文件（自动创建父目录） |
| `list_dir` | 列出目录条目 |
| `run_command` | 通过 `/bin/sh -c` 执行 shell 命令（带超时与退出码） |
| `finish` | 标记任务完成并返回最终答复 |

### 🔒 路径沙箱保护

所有文件操作均限制在工作区根目录下，拒绝 `..` 路径穿越，安全可靠。

### 📝 Git 自动版本管理

- 启动时自动检查 git 可用性，若工作区不是 git 仓库则自动初始化
- 每轮对话后自动提交文件改动，方便用 `git restore` / `git checkout` 回退

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

1. **系统提示词**：启动时根据工作区根目录生成系统提示，告知模型可用工具与行为准则（先探索再修改、用 `write_file` 落地改动、用 `run_command` 验证构建/测试等）。
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
