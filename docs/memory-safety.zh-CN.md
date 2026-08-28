**语言**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← 文档索引](README.zh-CN.md)

# 内存安全审计与猎取

NeverD 对已加载二进制做两类内存安全分析，并以结构化 JSON 报告结果。两条轨道都跑在格式无关的提升 IR 上，因此 **PE/COFF、ELF、Mach-O 是同等的一等目标**——发现不会藏在某一种格式的扫描器或导入表后面。

| 轨道 | 命令 | 报告内容 |
|------|------|----------|
| **审计（Audit）** | `neverd audit <binary>` | 堆对象生命周期缺陷及未初始化局部栈读取 |
| **猎取（Hunt）** | `neverd hunt <binary>` | 危险拷贝越界，并给出符号证据与候选输入值；仅在存在完整 `process-input-v1` 计划时为 `replayable=true` |

引擎复用 NeverD 自研符号执行与位向量求解器生成见证并确认可达性；不依赖外部求解器、虚拟机或容器。

---

## 核心不变量：失败即闭合

未提升的操作、缺少摘要的调用、ABI 未能恢复参数的调用、未解析的间接目标，或预算耗尽，一律给出 **UNKNOWN**，从不给出 SAFE。无法恢复容量的目的缓冲区也是 UNKNOWN。严格提升保持原样；安全层只在其上叠加保守裁决。

调用效果采用闭世界语义：只有前置条件与所有相关效果均已知时才应用摘要。未知效果或只能部分适用的摘要保持 UNKNOWN；分析不会把缺口假定为无效果或调用成功。

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

目录是可配置表，不是写死的集合。每个 **sink** 条目声明弱点类别、角色（copy、format、alloc、free、realloc）以及相关参数槽（目的、源、长度、容量）。JSON 中的 copy 或 format sink 还会提供可执行的调用 effect。每个 **source** 条目命名一个受攻击者影响的输入提供者。

内置条目位于 [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) 与 [`SafetySources.def`](../include/neverd/safety/SafetySources.def)，覆盖常见 C 运行时拷贝族（`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…）、带显式目的容量的加固 `_chk` 变体、分配与释放族（`malloc`/`calloc`/`realloc`/`free`、operator `new`/`delete`），以及可选的 Win32 堆 API。输入源包括 POSIX（`getenv`、`read`、`recv`、`fgets`、`fread`、`scanf`、程序参数）**以及** Win32（`GetCommandLineA/W`、`ReadFile`、`GetEnvironmentVariable*`），因此 PE 猎取不限于 POSIX 输入。

各格式拼写折叠到同一条目：去掉前导下划线（`_malloc`、`___strcpy_chk`），通过别名匹配重整后的 operator new/delete。

若 JSON copy 或 format sink 省略 `effect`，则根据所引用的最高参数槽推导适用性：copy 要求精确的参数个数，format sink 接受从该最小参数个数到可变参数上限的调用。可选的 `effect` 对象可用 `min_arity` 与 `max_arity`（或 `"variadic"`）显式设定可接受的参数个数范围，包括超出所推导 copy 精确参数个数的额外 wrapper 参数；`min_arity` 必须至少为最高被引用角色槽加一，而 `formats` 与 `abis` 用于限制适用性。若调用的参数个数、目标格式或 ABI 不匹配，则不应用摘要，并按闭世界规则保持 UNKNOWN。

可用规格文件扩展或覆盖目录：

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 },
    { "name": "my_format", "kind": "format", "dst": 0, "fmt": 2,
      "effect": { "min_arity": 3, "max_arity": "variadic",
                  "formats": ["elf"], "abis": ["sysv"] } }
  ],
  "sources": [
    { "name": "my_read", "out": 1, "return_tainted": true }
  ]
}
```

对于自定义 source，`out` 与 `return_tainted` 仅是发现元数据，并不建立可执行的内存、返回值或 taint effect。当前 source schema 缺少这些语义所需的类型化成功条件、内存修改、目标格式与 ABI 契约，因此依赖自定义 source effect 的分析必须保持 UNKNOWN。内置 source 不受影响：其经过适用性检查的类型化描述符继续提供可执行 effect。

不会因为存在同名 source 条目就推导出无界的仅目的参数自定义 sink。类似 `gets` 的自定义 sink 必须显式设置 `"unbounded": true`；把同名函数加入 source 目录不会赋予它可执行 effect，互相矛盾的源/长度字段会以事务方式拒绝。

---

## 猎取：拷贝越界裁决

对每个拷贝 sink，猎取按此顺序恢复目的容量——调试声明的数组大小，然后是已知大小的堆分配点，然后是可靠的栈帧上界——并通过反向 SSA 行走（跟随栈槽 spill/reload）对决定写入长度的参数分类：

- **常量长度** 在精确容量内时为 SAFE。常量越界只有在已佐证路径上可达 sink 时才为 UNSAFE；否则保持 UNKNOWN。
- **加固** `_chk` 拷贝带运行时目的上界。请求被拒绝，或该上界已证明不超过恢复出的对象容量时为 SAFE；存在越过对象的可行写入时为 UNSAFE；上界未恢复或结论不足时为 UNKNOWN。
- **可证明有界** 的长度（返回长度的调用、掩码、钳位）在求解前退出，并记录原因。只有目的大小精确时才是 SAFE；若只有包含区域上界，则仍为 UNKNOWN。
- **受攻击者影响** 且容量已知的长度交给位向量求解器：若存在大于容量的可行长度，裁决为 UNSAFE。只有能构造完整 `process-input-v1` 计划时，候选值才可重放。初始范围仅包括精确的字面环境值，以及至多第一次受支持的 `read(0)` 系列标准输入消费返回的字节。argv、文件、网络、自定义或有歧义的输入保持不可重放并附带原因。
- 其余情况——未知长度或未知容量——为 UNKNOWN。

每一项恢复出的容量都是真实对象大小的 **上界**，因此被证明的越界不会是误报。

### 格式化输入

对于 `scanf`/`fscanf` 及其带版本拼写，可读的常量格式会把每个未抑制转换映射到其实际的可变参数输出参数。无界 `%s`/`%[` 输出会把 taint 传播到后续字符串使用；数值与字符输出会污染从被写对象加载的值，但不污染输出指针值本身。`sscanf` 仅在其输入字符串已经受攻击者影响时传播这些 effect。`%Ns`/`%N[` 等有界文本输出会连同包含终止符的 `MaxBytes` extent 一起传播 taint；宽字符变体使用平台的 `wchar_t` 宽度计算该字节 extent。被抑制的转换、多余参数、位置依赖或不受支持的格式以及 `%n` 保持 UNKNOWN，不作猜测。

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "replay": { "adapter": "process-input-v1", "reason": "argv input is not supported by process-input-v1" }, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

`replayable` 是派生证据，而非独立承诺：仅当 `replay` 包含供 `process-input-v1` 适配器使用的完整输入计划时才为真。计划记录精确的环境字节、使用时第一次受支持的 `read(0)` 系列标准输入字节序列，以及从求解器赋值 ID 到这些输入的绑定；无法构造时由 `replay.reason` 说明原因。这些字段以增量方式加入；顶层 `schema_version` 仍为 `1`。

---

## 误报边界与范围

- 容量要么精确、要么是真实对象大小的上界，因此 UNSAFE 反映真实越界。若精确声明大小不可用，而包含区域上界又不足以证明安全，则结果为 UNKNOWN。
- 长度受限的拷贝在求解前退出并计入 `skipped`；精确容量可证明 SAFE，只有上界时仍保持 UNKNOWN。
- 已入目录的宽字符与追加拷贝，在元素字节宽度或目的字符串现有长度未恢复时保持 UNKNOWN。出参分配器与条件 `realloc` 的所有权转移无法证明时也保持 UNKNOWN。
- **P0**（本发布，三种格式）：sink 目录、参数预过滤、拷贝越界猎取、堆生命周期审计。每个测试主机都必须运行六个已检入样例，覆盖 PE、ELF、Mach-O × x86-64、AArch64。
- **P1**：栈/全局越界、未初始化局部读取与格式串检查已提供；更丰富的 PDB 栈类型和更多平台分配器仍是增量覆盖项，缺少精确摘要时保持 UNKNOWN。
- **P2**：patch 插入的运行时检查、过程间攻击者可达性。
