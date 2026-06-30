# coding-agent

一个运行在 Linux 终端的自主软件工程助手（autonomous coding agent），由 **DeepSeek** / **智谱 GLM** 等 OpenAI 兼容的大模型驱动。它能在你的工作区里读取/写入文件、列出目录、执行 shell 命令，并通过多轮工具调用（tool calling）循环自主完成编码、重构、调试等任务。

整个项目用 C++20 编写，**零第三方运行时依赖**（仅依赖 libcurl），自带 JSON 解析器/序列化器、HTTP 客户端封装和工具调度引擎，可同时运行于桌面 Linux 和 Termux（Android）环境。

---

## ✨ 特性

- **多轮 Agent 循环**：模型可连续调用工具，直到调用 `finish` 结束任务或达到迭代上限。
- **Git 自动版本管理**：启动时自动检查 git 可用性，若工作区不是 git 仓库则自动初始化；每轮对话后自动提交文件改动，方便用 `git restore` / `git checkout` 回退文件。
- **上下文回退（Context Rollback）**：自动为每一轮对话保存上下文快照，可随时回退到之前任意一个版本（见 [上下文回退](#-上下文回退)）。
- **内置工具集**：

  | 工具 | 作用 |
  |------|------|
  | `read_file` | 读取工作区内文本文件 |
  | `write_file` | 创建/覆盖文件（自动创建父目录） |
  | `list_dir` | 列出目录条目（`dir`/`file`/`link`） |
  | `run_command` | 通过 `/bin/sh -c` 执行命令，带超时与退出码捕获 |
  | `finish` | 标记任务完成并返回最终答复 |

- **沙箱路径保护**：所有文件操作都解析到工作区根目录之下，拒绝 `..` 路径逃逸。
- **双模式运行**：
  - 交互式 REPL（多行输入，`.` 或 `Ctrl-D` 提交）
  - 单次任务模式 `--once "prompt"`（适合脚本化调用）
- **多 Provider 支持**：内置 DeepSeek、智谱 GLM，运行时可切换。
- **Token 用量统计**：每轮显示输入/输出 token，`/tokens` 查看累计。
- **彩色终端输出**：自动检测 TTY，Markdown 渲染（表格、代码块、标题等）。
- **跨平台构建**：CMake 自动探测 Termux 前缀与 libc++ ABI。

---

## 🏗️ 项目结构

```
cpp_agent/
├── CMakeLists.txt        # 构建配置（C++20，libcurl，Termux 探测）
├── glm-agent             # 智谱 GLM 一键启动脚本
└── src/
    ├── main.cpp          # 入口：参数解析、REPL、Agent 循环、系统提示词
    ├── llm.hpp           # OpenAI 兼容 chat-completion 客户端 + tool calling
    ├── tools.hpp         # 文件/Shell 工具实现与 JSON Schema 定义
    ├── context.hpp       # 上下文版本管理 / 回退（快照、回滚、撤销）
    ├── git.hpp           # Git 集成：可用性检查、仓库初始化、自动提交
    ├── http.hpp          # libcurl 轻量封装（JSON POST / Bearer 鉴权）
    ├── json.hpp          # 自包含 JSON 值/解析器/序列化器（无外部依赖）
    └── markdown.hpp      # Markdown → 终端 ANSI 渲染器
```

---

## 🔧 依赖

- **C++20** 编译器（clang ≥ 14 / gcc ≥ 12）
- **CMake** ≥ 3.16
- **libcurl**（开发头文件）
- **pthreads**（系统自带）
- **git**（可选，推荐安装以启用自动版本管理）

Termux 下安装依赖：

```sh
pkg install clang cmake libcurl git
```

桌面 Linux（Debian/Ubuntu）：

```sh
sudo apt install build-essential cmake libcurl4-openssl-dev git
```

---

## 🚀 构建与运行

### 构建

```sh
cmake -B build && cmake --build build
```

构建产物为 `build/coding-agent`。

### 配置 API Key

通过环境变量提供密钥（按所选 provider 设置其一）：

```sh
export DEEPSEEK_API_KEY="sk-..."   # DeepSeek
export ZHIPU_API_KEY="..."          # 智谱 GLM
```

也可用 `--api-key` 参数显式传入，或在项目根目录创建 `.env` 文件：

```
ZHIPU_API_KEY=你的密钥
```

### 使用 GLM 模式（推荐）

项目提供了便捷启动脚本 `glm-agent`，自动使用智谱 GLM 模型：

```sh
# 交互模式
./glm-agent

# 单次任务
./glm-agent "分析 src 目录并总结架构"
```

脚本会自动读取 `ZHIPU_API_KEY` 环境变量或项目根目录的 `.env` 文件。

### 交互模式

```sh
./build/coding-agent
```

进入 REPL 后输入提示词，单独一行输入 `.`（或 `Ctrl-D`）提交：

```
> 帮我给这个项目加一个 README
.
```

### 单次任务模式

```sh
./build/coding-agent --once "分析 src 目录并总结架构"
# 或直接传位置参数（隐含 --once）
./build/coding-agent "修复 main.cpp 里的编译警告"
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
| `/back <id>` | 将上下文回退到指定版本（丢弃其后所有版本） |
| `/undo` | 撤销最近一轮对话（回退到上一个版本） |
| `/exit` | 退出 |

---

## ⏪ 上下文回退

Agent 的「上下文」即发送给大模型的完整对话历史（系统提示 + 用户/助手/工具消息）。随着对话推进，上下文会不断增长；一旦某轮走偏、引入错误信息或上下文被污染，继续对话往往越改越乱。

本项目内置**上下文版本管理**（`src/context.hpp`），采用类似 `git checkout` 的**线性快照模型**：

- **自动快照**：每次成功完成一轮 REPL 对话后，自动保存一个快照（标签取自用户提示词的首行）。
- **手动快照**：用 `/snap [label]` 在任意时刻打一个带标签的检查点。
- **回退**：用 `/back <id>` 把上下文恢复到某个历史版本，该版本之后的所有快照与消息都会被丢弃。
- **撤销**：`/undo` 是回退到上一版本的快捷方式。
- **查看**：`/versions` 列出全部版本，`*` 标记当前所在版本。

> 注意：回退只影响**对话上下文**（发给 LLM 的消息历史），不会撤销 `write_file`/`run_command` 等工具对工作区文件已造成的实际改动。如需还原文件，请配合 Git 自动版本管理使用（见下文）。

### 示例

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
> /versions
.
context versions (* = current):
  #1  14:02:01  1 msgs  init
* #2  14:02:30  8 msgs  重构前
```

回退后，后续对话将基于版本 #2 的上下文继续，仿佛第二轮从未发生过。

---

## 🗃️ Git 自动版本管理

程序内置 Git 集成（`src/git.hpp`），让文件级别的回退变得简单可靠。

### 启动行为

| 条件 | 行为 |
|------|------|
| git 未安装或不在 PATH | 输出警告，提示安装 git |
| git 已安装，工作区无 `.git` | 自动 `git init`，设置 user.name/user.email，创建初始提交 |
| git 已安装，工作区已有仓库 | 正常运行，不做额外操作 |

### 自动提交

每轮对话结束后（无论 `--once` 还是 REPL 模式），只要 agent 修改了文件，就会自动执行：

```sh
git add -A
git commit -m "agent: <用户提示摘要>"
```

提交信息包含用户提示的前 80 个字符，方便识别每轮改动的目的。即使 `run_turn` 抛出异常，已修改的文件也会被提交，避免丢失部分改动。

### 回退文件改动

有了自动提交，用户可以随时用标准 git 命令回退：

```sh
# 查看提交历史
git log --oneline

# 查看某次改动的详情
git show <commit-hash>

# 撤销某个文件的修改（回到最近一次提交的状态）
git checkout -- src/main.cpp

# 撤销整个工作区到上一次提交
git restore .

# 回退到某个历史版本
git reset --hard <commit-hash>
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
5. **Markdown 渲染**：LLM 返回的 Markdown 文本在终端中自动渲染为带颜色和样式的输出（表格、代码块、标题等）。
6. **上下文版本管理**（`context::History`）：每轮对话后保存消息历史的完整快照；回退时恢复指定快照并丢弃其后版本，使上下文始终沿单线演进。
7. **Git 自动提交**（`git::commit_changes`）：每轮对话后执行 `git add -A && git commit`，记录文件级别的改动历史。

---

## 🔌 Provider 配置

内置两个 provider（定义于 `src/llm.hpp`）：

| name | base_url | 默认模型 | 密钥环境变量 |
|------|----------|----------|--------------|
| `deepseek` | `https://api.deepseek.com/v1` | `deepseek-chat` | `DEEPSEEK_API_KEY` |
| `glm` | `https://open.bigmodel.cn/api/paas/v4` | `glm-4-flash` | `ZHIPU_API_KEY` |

两者均兼容 OpenAI Chat Completions + Function Calling 协议，因此可方便地扩展更多 provider。

---

## ⚠️ 安全说明

- 文件工具通过 `resolve_under_root` 将路径规范化并校验是否位于工作区根目录内，阻止目录穿越。
- `run_command` 在工作区根目录执行，带硬超时（1–300 秒）。
- **请仅在可信的工作区中运行**：`run_command` 拥有执行任意 shell 命令的能力，等同于在终端手动执行。
- 上下文回退只回滚对话历史，**不回滚**工具对文件系统造成的实际改动。如需回退文件，请使用 Git 命令。

---

## 📝 许可证

本项目未附带显式许可证文件，默认保留所有权利。如需使用请先与作者确认。
