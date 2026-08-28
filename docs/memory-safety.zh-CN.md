**语言**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← 文档索引](README.zh-CN.md)

# 内存安全审计与猎取

NeverD 对已加载二进制做两类内存安全分析，并以结构化 JSON 报告结果。两条轨道都跑在格式无关的提升 IR 上，因此 **PE/COFF、ELF、Mach-O 是同等的一等目标**——发现不会藏在某一种格式的扫描器或导入表后面。

| 轨道 | 命令 | 报告内容 |
|------|------|----------|
| **审计（Audit）** | `neverd audit <binary>` | 堆对象生命周期缺陷：泄漏、重复释放、释放后使用 |
| **猎取（Hunt）** | `neverd hunt <binary>` | 危险拷贝越界，并给出符号证据与候选输入值；在进程输入适配器映射到实际字节前为 `replayable=false` |

引擎复用 NeverD 自研符号执行与位向量求解器生成见证并确认可达性；不依赖外部求解器、虚拟机或容器。

---

## 核心不变量：失败即闭合

未提升的操作、缺少摘要的调用、ABI 未能恢复参数的调用、未解析的间接目标，或预算耗尽，一律给出 **UNKNOWN**，从不给出 SAFE。无法恢复容量的目的缓冲区也是 UNKNOWN。严格提升保持原样；安全层只在其上叠加保守裁决。

---

## 按格式的身份契约

两条轨道都要求 lift 管线（它负责恢复每次调用的参数），并通过 NeverD 其余部分共用的身份视图为每条发现命名被调函数。调试信息发现顺序不变：

| 格式 | 调试信息（优先级从高到低） | 导入 / thunk 解析 |
|------|----------------------------|-------------------|
| **PE/COFF** | `--pdb`、调试目录或同级 `.pdb`，然后是 MSVC `/MAP` | IAT 槽与 `__imp_` thunk、序号导入 |
| **ELF** | 镜像内 DWARF、拆分 `*.debug`，然后是 GNU/LLD MAP | PLT stub 解析为导入名 |
| **Mach-O** | 镜像内 DWARF、相邻 `.dSYM`，然后是 ld64 `-map` | dyld bind / 间接符号槽与 stub helper |

`--pdb` / `--map` 指定权威伴生文件：读失败是错误，不是静默回退。`--no-debug` 在所有格式上都只读镜像本身。

PDB 过程签名用于区分有返回值的分配函数与 `void` 释放函数。PDB 局部变量和栈类型的丰富恢复仍有限；无法确认精确对象大小时，猎取会回退到帧布局／分配点模型，并给出 UNKNOWN，而不会虚构容量。

### 名称来源优先级

每条发现都带 `name_source`，说明被调名来自何处，按以下优先级选择：

1. `rename` — 调用方提供的重命名
2. `import` — IAT（PE）、PLT（ELF）或 dyld-bind / stub（Mach-O）条目
3. `export` / `symbol` — 镜像已经陈述的导出、符号表条目或其他非占位名
4. `pdb` / `dwarf` / `map` — 为占位名建立身份或与镜像既有名称一致的调试符号
5. `sig` — 签名库匹配
6. `synthetic` — 为未命名例程铸造的占位名

仅由 DWARF 命名的静态链接 `memcpy` 报告 `dwarf`；导入的 `memcpy` 在所有格式上都报告 `import`。伴生文件不会覆盖镜像已经陈述的不同非占位名，签名匹配也不会覆盖任何已陈述身份。

---

## Sink 与 source 目录

目录是可配置表，不是写死的集合。每个 **sink** 条目声明弱点类别、角色（copy、format、alloc、free、realloc）以及相关参数槽（目的、源、长度、容量）。每个 **source** 条目命名一个受攻击者影响的输入提供者。

内置条目位于 [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) 与 [`SafetySources.def`](../include/neverd/safety/SafetySources.def)，覆盖常见 C 运行时拷贝族（`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…）、带显式目的容量的加固 `_chk` 变体、分配与释放族（`malloc`/`calloc`/`realloc`/`free`、operator `new`/`delete`），以及可选的 Win32 堆 API。输入源包括 POSIX（`getenv`、`read`、`recv`、`fgets`、`fread`、`scanf`、程序参数）**以及** Win32（`GetCommandLineA/W`、`ReadFile`、`GetEnvironmentVariable*`），因此 PE 猎取不限于 POSIX 输入。

各格式拼写折叠到同一条目：去掉前导下划线（`_malloc`、`___strcpy_chk`），通过别名匹配重整后的 operator new/delete。

可用规格文件扩展或覆盖目录：

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 }
] }
```

---

## 猎取：拷贝越界裁决

对每个拷贝 sink，猎取按此顺序恢复目的容量——调试声明的数组大小，然后是已知大小的堆分配点，然后是可靠的栈帧上界——并通过反向 SSA 行走（跟随栈槽 spill/reload）对决定写入长度的参数分类：

- **常量长度** 在精确容量内时为 SAFE。常量越界只有在已佐证路径上可达 sink 时才为 UNSAFE；否则保持 UNKNOWN。
- **加固** `_chk` 拷贝带运行时目的上界。请求被拒绝，或该上界已证明不超过恢复出的对象容量时为 SAFE；存在越过对象的可行写入时为 UNSAFE；上界未恢复或结论不足时为 UNKNOWN。
- **可证明有界** 的长度（返回长度的调用、掩码、钳位）在求解前退出，并记录原因。只有目的大小精确时才是 SAFE；若只有包含区域上界，则仍为 UNKNOWN。
- **受攻击者影响** 且容量已知的长度交给位向量求解器：若存在大于容量的可行长度，裁决为 UNSAFE，并将求解器模型报告为符号证据与候选值；在进程输入适配器可用前不可重放（`replayable=false`）。
- 其余情况——未知长度或未知容量——为 UNKNOWN。

每一项恢复出的容量都是真实对象大小的 **上界**，因此被证明的越界不会是误报。

---

## 审计：堆生命周期裁决

对每次分配，审计在控制流图上跟踪句柄（含栈 spill/reload），并应用逃逸摘要（返回、写入非栈地址，或交给不透明被调）：

- **泄漏** — 句柄既未释放也不允许逃逸。
- **重复释放** — 某条路径上第二次释放在第一次之后可达。
- **释放后使用** — 释放之后仍可达解引用或已建模的非释放使用。

分配与释放 **包装函数** 通过逐函数逃逸摘要识别，因此 `malloc`/`free` 转发器不会掩盖缺陷。互斥分支上的释放不报告为重复释放。

堆状态机先生成候选事件序列（分配、释放、使用或返回出口）；只有第二遍在符号 LowIR 路径上按序重放这些事件，并由求解器证明路径谓词可满足后，发现才会成为高置信度 UNSAFE。缺失 LowIR、不透明操作、无摘要调用、求解器不确定或探索预算耗尽都会把候选降为 UNKNOWN。可能别名导致的内存 havoc 单独计数，因此普通栈帧写入不会无差别否决本来精确的可达性证据。

---

## 预算、输出与绑定

猎取探索与求解器受预算约束（`--max-paths`、`--max-steps`、`--max-loop`、`--solver-conflicts`）；预算耗尽给出 UNKNOWN。两条命令都打印 JSON，并尊重 `-o`。退出码：SAFE 为 `0`，UNSAFE 为 `2`，UNKNOWN 或出错为 `1`。

同一分析也可通过 C API（`neverd_session_audit_json` / `neverd_session_hunt_json`，带版本化 `neverd_safety_options`）和 Python SDK（`Session.audit()` / `Session.hunt()`）使用。

### 发现 schema

```json
{
  "class": "buffer_overflow",
  "function": "parse_header",
  "name": "strcpy",
  "name_source": "import",
  "call_va": "0x11a4",
  "source": "reader.c:42",
  "sink": "strcpy",
  "arg_index": 1,
  "flow": "TAINTED",
  "verdict": "UNSAFE",
  "confidence": "HIGH",
  "capacity": 16,
  "capacity_kind": "exact",
  "corroboration": "path predicate and overflow are jointly satisfiable",
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

---

## 误报边界与范围

- 容量要么精确、要么是真实对象大小的上界，因此 UNSAFE 反映真实越界。若精确声明大小不可用，而包含区域上界又不足以证明安全，则结果为 UNKNOWN。
- 长度受限的拷贝在求解前退出并计入 `skipped`；精确容量可证明 SAFE，只有上界时仍保持 UNKNOWN。
- 已入目录的宽字符与追加拷贝，在元素字节宽度或目的字符串现有长度未恢复时保持 UNKNOWN。出参分配器与条件 `realloc` 的所有权转移无法证明时也保持 UNKNOWN。
- **P0**（本发布，三种格式）：sink 目录、参数预过滤、拷贝越界猎取、堆生命周期审计。每个测试主机都必须运行六个已检入样例，覆盖 PE、ELF、Mach-O × x86-64、AArch64。
- **P1**：栈/全局越界、未初始化读、格式串、更丰富的 PDB 栈类型、更多平台分配器。
- **P2**：patch 插入的运行时检查、过程间攻击者可达性。
