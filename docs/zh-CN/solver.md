**语言**：[English](../solver.md) | [简体中文](solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← 文档索引](README.md)

# Bitvector 证明后端

NeverD 默认使用内置 bitvector solver。精确 MBA 推导独立于通用 solver。表达式合成只有在证明等价后才接受候选；反例或未决查询都会保留原表达式。

内置乘法电路先移除两个因子中已知为零的低位前缀，只构造剩余位的乘积，再恢复完整输出字。可选 Z3 适配器也按偶数常量系数实际能观测的位宽编码乘积。两者均保留全位宽等价查询、独立输入模型和现有资源限制。CLI 和 GUI 的源码优化共享内置电路，无需额外开关。

<!-- i18n-section: builtin-comparisons -->

## 内置比较电路

超过八位的比较先比较高半部分，高半部分相等时再使用低半部分的结果。小块使用减法进位。当部分寄存器更新只改变低位时，共享编码器可以复用高位前缀的门。有符号比较仍将两个符号位取反。表达式语义和现有资源限制保持不变；预算耗尽仍返回 `Unknown`。

`BitBlaster.WidePredicatesAgreeWithTheEvaluator` 在 8 至 256 位之间选取多种宽度，将有符号和无符号谓词与表达式求值器比较，覆盖奇数宽度、边界相邻值和第 64 位以上的有效位，并排除错误输出值。部分计数器测试检查完整查询、反例模型和门预算耗尽。原生缓存比较回归保留所有寄存器和标志观测，证明比较与更新的两种顺序，并拒绝被修改的原始循环体。

<!-- i18n-section: pristine-encoding -->

## 搜索前的编码副本

`BitVectorSolver::cloneEncoding()` 复制尚未尝试 SAT 搜索的完整编码；搜索后或编码失败时返回空指针。副本独立持有可变子句、根传播、门和位映射，保留变量顺序、门计费和求解器设置。上下文必须比两个求解器存活更久；源求解器可独立修改或销毁。

SAT 引擎在每个文字的 watch 列表内联存放四个条目，较长列表动态增长，从而减少构造、搜索前复制和销毁短列表时的独立分配。传播顺序、子句内容、独立所有权和全部工作限额保持不变。

<!-- i18n-section: context-finite-proofs -->

## 同一上下文内的完整证明

原生独立性检查器将有限域缓存绑定到实际只追加节点的符号上下文。紧凑键保留精确谓词、有序投影和值数量上限。仅复用完整数值域或已证明的非唯一性；每次未命中仍须完成枚举及最终排除证明。缓存存活期间，上下文及已有节点含义必须保持稳定。预备令牌不能跨缓存所有者，也不能交给同一地址上的替代所有者复用。存储仍受原有字数上限约束。其他使用者保留结构键和变量重命名复用。

<!-- i18n-section: z3-build -->

## 可选 Z3 构建

启用 Z3 后，CMake 通过 `FetchContent` 下载固定的 4.13.3 源码版本，并与 NeverD 一起构建静态库；不需要系统安装 Z3：

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
python scripts/build.py build-release --parallel 4 --target neverd \
  NeverDSolverTests NeverDSymbolicTests neverd-solver-bench
```

默认 provider 为 `NEVERD_Z3_PROVIDER=FETCH`。首次配置会下载源码，后续构建复用 build 目录的 `_deps`。不会构建 Z3 CLI、测试、示例、文档或语言绑定；Z3 Python 生成器使用 NeverD 已依赖的 Python interpreter。使用已安装库时选择 `-DNEVERD_Z3_PROVIDER=SYSTEM`，也可指定 `-DZ3_ROOT=/path/to/prefix`。缺少开发文件会失败，不会静默切换 provider。离线构建可用 `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3` 指定本地源码。

默认 `NEVERD_ENABLE_Z3=OFF` 时，NeverD 不下载、不搜索也不链接 Z3。运行时显式请求不可用后端会失败，不会回退。

<!-- i18n-section: synthesis -->

## 证明门控的表达式合成

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

默认值为 `--solver=builtin`。选择 solver 必须同时使用 `--synthesize`；普通 MBA 简化器不会调用 Z3。Z3 timeout 按每次检查计时，不包括表达式翻译。取消是协作式的，不是硬性墙钟期限。0 使用默认 1000 ms；`--exhaustive` 移除该限制。SAT 冲突、传播和 watch 访问限制仅适用于内置后端，与 Z3 同用会被拒绝。Z3 检查会增加 proof-query 计数；内置 SAT 工作计数保持为 0。

C API 在 `neverd_synthesize_options` 中追加 `solver_backend` 和 `solver_timeout_ms`。有大小边界的读取器仍为旧调用方选择内置后端。`neverd_solver_backend_available()` 报告构建能力；Python 也可通过 `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)` 选择。当前仅覆盖表达式合成；concolic 执行、安全分析及既有 IR 优化默认仍沿用原 solver 策略。内部用户可通过语义简化器的 proof callback 提供 Z3 verifier。

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## 独立检查与查询导出

启用 Z3 时，`NeverDSolverTests` 包含独立表达式求值和跨后端检查。参考表达式会直接构造，避免 NeverD 表达式 builder 的错误把测试两侧一起简化。禁用 Z3 的构建会明确跳过 oracle 用例，但仍测试后端不可用契约。

缩放乘积测试使用独立长乘法电路和整数运算检查内置电路，覆盖至 257 位。Z3 测试证明它与独立的全位宽编码一致，并检查高输入位及故意改错的最高输出位。

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

每条非注释行格式为 `original ; candidate`；没有分号时，以 MBA 简化器的结果作为候选。工具报告判定、模型重放及耗时；计时包括 session 创建、翻译和求解，不包括解析、MBA 简化、导出和销毁。每次重复使用全新 solver。SAT 模型必须在表达式求值器中重现差异。相互矛盾的确定判定、无效查询或模型会使运行失败；`unknown` 会记录，但不构成等价证明。

SMT-LIB 导出包含等价编码的 DAG、永久断言和最后一次查询的假设，可用 `z3 query-N.smt2` 重放。应记录资源限制和 solver 版本。两个后端的预算单位不同，因此比较的是有界工作负载，不是相同工作量。Bitvector 证明采用表达式语言的 total fixed-width 语义。机器异常、内存效果和 LLVM poison 仍由提升／翻译边界负责，表达式证明并不认证这些行为。

尚未搜索的编码副本从自身决策队列移除根层已赋值变量，并重建严格的活跃度／编号堆顺序。赋值和子句保持完整，根层事实在所有回溯中保留。源对象所有权、实际决策、完整模型和全部搜索预算均不变。

SMT-LIB 导出的 DAG 是语义等价的编码，可能包含上述精确缩放商，而不保证与原始 DAG 结构相同。
