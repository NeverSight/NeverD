**语言**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← 文档索引](../README.zh-CN.md)

# NeverD 路线图

本文档概述 NeverD 在现有原生 PE / ELF / Mach-O 管线之外的主要规划方向。全局原则不变：**1:1 指令级提升**、**strict 显式失败**（不支持则报错，不静默跳过），以及同一套 **四级 IR** 支撑 lift / decompile / patch。

---

## 1. 原生格式补齐

完成 loader 已部分识别、但格式级尚未端到端打通的目标，使支持矩阵与用户真实可用能力一致。


| 项目                | 说明                                          |
| ----------------- | ------------------------------------------- |
| PE AArch64        | Windows ARM64：unwind/`.pdata`、跳板、rewrite 往返 |
| PE ARM32（Thumb-2） | Windows on ARM 仅 Thumb；解码/发射必须遵守该模式         |
| Mach-O i386       | 应用常见 clang 重定位；优先 thin object               |


### 设计原则

- 格式×架构单元格在格式级测试通过（load → lift → decompile / patch）前不标为支持
- 不破坏现有 ELF / PE x86 / Mach-O arm64+x64 行为
- 优先使用镜像级指令模式（如 Thumb vs ARM），避免分散启发式

---

## 2. EVM 字节码反编译

将 NeverD 扩展到 **以太坊虚拟机（EVM）** 合约字节码——把 EVM 操作码提升进同一 IR 栈，输出 C、面向 Solidity 的源码与 LLVM IR，服务审计与分析。

### 目标

- **EVM loader** — 接受运行时字节码及常见制品形态（部署码、creation / runtime 拆分等）
- **操作码 lifter** — 手写 1:1 语义；未知/新操作码在 strict 下显式失败
- **栈与内存模型** — 将 EVM 栈机状态回收为 MedIR 变量 / 内存操作
- **控制流恢复** — JUMP / JUMPI → CFG；尽量结构化为 HighIR
- **存储与 calldata** — 建模 `SLOAD`/`SSTORE`、calldata、returndata 及常见 ABI 调用形态
- **反编译输出** — 带显式 host-effect 契约的可编译 C23 与 Solidity 状态机，以及已验证 LLVM IR
- **CLI / C API** — `neverd decompile` / session API 对 EVM 输入与原生二进制一致

**状态：** Frontier 到 Fusaka 的传统操作码解码与 lifting 已完成并有回归测试覆盖。
源码重建仍在持续保守演进：selector、event、类型、标准、名称与动态控制流只有在证据
充分时才报告，不声称原始源码身份、完整 ABI 或完整 ERC 合规性。规范函数 selector、
逐标准 ABI 变体与成功返回形状彼此分离，因此共享 ERC selector 既不能凭空证明某个
标准，也不会借用不兼容的返回类型。Amsterdam 只作为
Review/development 的显式 opt-in target；`latest` 仍为 Fusaka。EOFv1/EIP-7692 尚未
排期，EIP-3540 为 Stagnant，均不冒充已定案主网行为。host ABI 与明确限制见
[EVM 反编译](../evm.zh-CN.md)。

### 为什么做 EVM？

- 审计需要忠实还原链上逻辑；近似反编译会掩盖语义
- 复用 Low → Med → High → LLVM，原生与合约共用一套引擎
- 与原生侧一致：不静默「不支持就跳过」

---

## 3. Solana eBPF（SBF）反编译

支持 **Solana eBPF / SBF** 链上程序——将 SBF 机器码提升进 NeverD IR，并以同样的 strict 语义反编译。

### 目标

- **SBF / sbpf loader** — 加载 Solana program ELF（及必要的打包形态）
- **eBPF/SBF lifter** — 针对 Solana BPF ISA 子集手写 1:1 语义；缺口 strict 报错
- **Account 与 CPI 感知** — 在表现为调用/intrinsic 时恢复常见 Solana 运行时模式（account info、syscall、CPI）
- **CFG 与结构化输出** — 与原生相同：LowIR → MedIR → HighIR / LLVM → C
- **CLI / C API** — 统一的 session load / analyze / decompile 入口

**状态：** 当前 Anza `sbpf` v0-v4 合约支持已完成。实现支持旧式 section/relocation ELF 与严格的仅 program-header ELF、完整的版本化指令数据库、严格验证、分阶段 Low/Med/High IR、syscall/CPI/account 观察、已验证的 LLVM、可移植 C11、安全的稳定版 Rust、CLI/C API 集成，以及独立且有界的原始字节码语义 oracle。v4 会跟随上游维护；能否在特定集群部署或执行仍取决于该集群的 feature activation。详见 [Solana SBF 反编译](../sbf.zh-CN.md)。

### 为什么做 Solana eBPF？

- 链上 SBF 与 EVM 同为重要审计目标
- BPF 形态 ISA 适合 NeverD 现有 CFG + SSA MedIR
- 一套 C SDK 覆盖原生 + 合约字节码，减少安全研究工具碎片化

---

## 4. 内存安全审计与猎取

对已提升的二进制做堆对象生命周期缺陷（泄漏、重复释放、释放后使用）与危险拷贝越界分析，并以结构化 JSON 报告；对已证明的越界给出有界求解器模型。分析跑在格式无关的 IR 与共享身份视图上，因此 **PE、ELF、Mach-O 是同等目标**，并复用自研符号执行与位向量求解器——不依赖外部求解器或容器。

| 项目 | 说明 |
|------|------|
| `audit` 轨道 | IR 上的堆状态机 + 逃逸摘要：泄漏、重复释放、释放后使用 |
| `hunt` 轨道 | 汇目录 + 参数预过滤 + 目标容量 + 求解器见证 |
| 身份契约 | 按格式解析汇（PE IAT、ELF PLT、Mach-O dyld bind）以及 PDB / DWARF / MAP 名称来源 |

**状态：** PE、ELF、Mach-O 的 P0 实现已存在，但关闭仍需进程输入重放适配器和完整的调用效果摘要。判定与身份覆盖由 [`unittests/safety`](../../unittests/safety)（目录、扫描器、参数预过滤、对象模型、hunt、audit）以及在每个主机上强制运行 PE/ELF/Mach-O × x86-64/AArch64 六单元 fixture 矩阵的端到端 [`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp) 锁定。详见 [内存安全审计与猎取](../memory-safety.zh-CN.md)。P1 将扩展到栈/全局越界、未初始化读取与格式串。

---

## 5. 引擎与产品加固（持续）

支撑上述方向、并提升当前原生引擎质量的横切工作。


| 领域        | 方向                                 |
| --------- | ---------------------------------- |
| Lifter 覆盖 | 在不放松 strict 的前提下缩小原生操作码缺口          |
| 语义测试      | 新 ISA 落地时扩展 Unicorn / roundtrip 覆盖 |
| 插件 ABI    | 适合时用插件承载新格式的 loader / 分析 pass      |
| 文档 / 矩阵   | 仅在测试落地后更新 README 支持表               |


---

## 时间线

原生格式补齐、Fusaka 及以前的传统 EVM 解码/lifting、Solana SBF 反编译与内存安全 P0
已有回归覆盖；保守的 EVM 源码重建仍在进行。不承诺具体发布日期。


| 功能                          | 状态        |
| --------------------------- | --------- |
| 原生格式补齐（PE ARM*、Mach-O i386） | 已完成       |
| EVM 传统解码/lifting              | 到 Fusaka 已完成；有回归测试覆盖 |
| EVM 源码重建                      | 持续进行 — 有证据才报告，保持保守 |
| Solana eBPF（SBF）反编译         | 已完成 — v0-v4、C、Rust 与 LLVM；有回归测试覆盖 |
| 内存安全审计与猎取                   | 进行中 — P0 实现已存在；重放/调用摘要收尾待完成 |
| 引擎与产品加固                     | 持续进行      |
