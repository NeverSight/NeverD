**语言**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← 文档索引](README.zh-CN.md)

# Python 插件

NeverD 可以把 Python 文件作为一等插件加载。Python 插件与原生插件共享相同的元数据、生命周期、排序、重名规则、事件流和会话 C ABI。受支持的编写包是 `neverd-plugin`；请勿直接导入私有的 `_neverd_plugin` 桥接模块。

## 构建与运行要求

`NEVERD_ENABLE_PYTHON_PLUGINS` 默认为 `ON`。启用后，CMake 必须能够找到 CPython 3.10 或更高版本的解释器及其嵌入开发库：

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

若需要不链接 CPython 的纯原生 `libneverd`，请设置 `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`。启用 Python 的构建会把匹配的包和示例放到 `build/bin/sdk/python/`；也可以直接运行 `python3 -m pip install build/bin/sdk/python` 安装该目录。

## 编写插件

一个模块必须只声明一个带装饰器的类：

```python
from neverd_plugin import Event, Plugin, PluginType, Session


@Plugin(
    name="Analysis Report",
    version="1.0.0",
    author="Your team",
    description="Reports basic information about the loaded binary",
    type=PluginType.PROCESSOR,
)
class AnalysisReport:
    def on_init(self, session: Session) -> int | None:
        print(session.architecture)
        return None

    def on_run(self, session: Session, arg: int) -> int | None:
        print(session.file_path, session.function_count)
        return 0

    def on_event(self, event: Event) -> int | None:
        print(event.type.name)
        return None

    def on_term(self) -> None:
        pass
```

所有钩子均可选。`None` 表示成功；整数返回值必须能放入 C `int`。元数据版本必须是严格的 SemVer。名称必须是非空 UTF-8 字符串，任何包含内嵌 NUL 的元数据都会被拒绝。

仓库提供了 [`minimal.py`](../pluginsdk/python/examples/minimal.py)、[`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py) 和演示证明门控优化 API 的 [`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py) 三个示例。

## 加载与检查插件

C API 可以确定性地加载指定 `.py` 文件，也可以扫描目录：

```c
if (!neverd_plugins_load_file(session, "plugins/report.py")) {
  const char *message = neverd_last_error(session);
  /* log message */
  neverd_free_string(message);
}

neverd_plugins_init(session);
int result = neverd_plugins_run(session, "Analysis Report", 0);
neverd_plugins_term(session);
```

`neverd_plugins_list_json` 使用 `"kind":"python"` 或 `"kind":"native"` 标识每一项。目录发现按规范路径排序，并可在同一目录中同时接受原生库与 Python 文件。重复的规范路径和重复的插件名称都会报错。

## 会话与事件 API

`Session` 在每次调用 C API 前都会重新验证宿主能力。其类型化接口包含文件、架构和格式元数据，位数与表计数，函数视图，加载与分析，字节读取，反汇编、反编译及常用查询。高级操作可通过 `session.raw` 访问 `neverd_plugin.abi` 中的每一项声明：

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### 有界符号路径探索

对于原生 LowIR 函数，`session.symbolic_explore` 会返回类型化的路径结果、基本块轨迹、资源使用量以及可选的路径谓词：

```python
result = session.symbolic_explore(
    0x401000,
    max_paths=64,
    max_steps=1 << 16,
    max_block_visits=3,
    include_expressions=True,
)
if not result.exact:
    print(result.unmodelled_ops)
for path in result.paths:
    print(path.outcome, path.blocks, path.predicate)
```

如果路径数、步数、循环访问次数或未解析分支达到上限并终止遍历，`complete` 为 false。`exact` 还要求没有任何操作被保守地替换为未知状态；不受支持的 LowIR 操作、没有摘要的调用和通过未解析地址进行的存储都会计入 `unmodelled_ops`。EVM 和 SBF 会话不提供原生 LowIR 探索。

### 已验证的 LowIR 混合执行分支翻转

`session.lowir_concolic` 从显式指定的入口寄存器字节范围沿一条原生 LowIR 轨迹执行，只返回由求解器生成、并经全新重放在同一控制决策出现位置验证的候选：

```python
from neverd_plugin import ConcolicRegisterSeed

report = session.lowir_concolic(
    0x401000,
    [ConcolicRegisterSeed(offset=56, bytes=4, value=0)],
)
for flip in report.flips:
    if flip.candidate_id is not None:
        print(report.candidates[flip.candidate_id].seed)
```

寄存器偏移是 NeverD 寄存器文件中的字节偏移，不是原生指针或寄存器编号。报告始终是非穷举的；UNSAT、求解器预算耗尽、投影拒绝和重放拒绝均保留为类型化翻转结果，而不是异常。

### 内存安全审计与猎取

`session.audit()` 与 `session.hunt()` 返回解析后的 JSON 报告（与 CLI 同一模式）。它们需要已提升的原生会话：

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

EVM 和 SBF 会话会拒绝这些调用。

六种不可变事件分别是 `BINARY_LOADED`、`BINARY_CLOSING`、`FUNCTION_SELECTED`、`ADDRESS_CHANGED`、`ANALYSIS_DONE` 和 `PATCH_APPLIED`。回调期间会复制载荷字符串；与当前事件类型无关的字段为 `None`。

切勿保存 `Session` 并在终止后继续使用。原生 capsule 会在 `on_term` 开始前以及原生会话释放前失效。后续调用会抛出 `RuntimeError`，而不会解引用过期内存。

### 严格二进制 sanitizer 发布

`session.sanitize()` 执行实验性的全点位或拒绝 `binary-sanitizer-v1` 事务；只有完整、一致的认证 receipt 通过校验后才返回冻结的 `SanitizeResult`。非 Darwin 主机在 lifting、guard 生成、候选创建或命名空间变更前拒绝。`PUBLISH_INDETERMINATE` 与 `PUBLISHED_INCOMPLETE` 均为失败，且表示目标可能已经存在，使用或重试前必须检查。

Darwin 的完整 receipt 只认证事务期间持有的目标目录对象。该目录可在打开后改名，因此 receipt 不保证原始路径在事务中或返回后仍指向该对象，也不是持久、可独立复核的路径绑定；稍后重新打开路径的代码必须保留外部锚点并重新认证对象。Python 当前没有原生全进程重放方法；`NativeProcessReplayAdapter` 只是故障关闭的 Phase 0 C++ 可用性／工厂边界，所有主机都返回能力全 false 且没有操作表。

### 证明门控的合成与 LLVM 优化

`synthesize_expression` 与为 ABI 兼容而保留的、仅处理 MBA 的
`simplify_expression` 相互独立。只有求解器返回
`ProofStatus.EQUIVALENT` 时，改写结果才会被提交；反例、证明不完整和搜索
预算耗尽都会保留原表达式，并分别报告结果原因与搜索/证明工作量。
`ProofStatus.INVALID` 表示证明问题本身无效，与预算导致的
`ProofStatus.UNKNOWN` 严格区分；两者都会以拒绝改写的方式关闭。

`optimize_llvm_ir` 在事务副本上组合 NeverD 的语义不动点与标准 LLVM
优化管线，验证成功后才返回提交的模块：

```python
from neverd_plugin import (
    LLVMOptimizationLevel,
    OptimizationMode,
    ProofStatus,
    optimize_llvm_ir,
    synthesize_expression,
)

rewrite = synthesize_expression(
    "(x >> 4) + ((x >> 2) >> 2)", exhaustive=True
)
if rewrite.changed:
    assert rewrite.proof_status is ProofStatus.EQUIVALENT

module = optimize_llvm_ir(
    llvm_ir,
    mode=OptimizationMode.DEEP,
    llvm_level=LLVMOptimizationLevel.O2,
    enable_synthesis=True,
    exhaustive=True,
)
print(module.output_ir, module.semantic_rewrites, module.proof_queries)
```

生产调用方可以分别限制 MBA 的工作量与元数、合成搜索与 SAT 工作量以及
LLVM 收敛。对 `simplify_expression` 显式使用 `exhaustive=True` 会选择无上限
的 MBA 元数/工作策略，并移除原生解析器的嵌套与位宽策略上限；对
`synthesize_expression`，它会移除解析、搜索工作量和 SAT 上限，但保留调用方
指定的语法范围；对 `optimize_llvm_ir`，它会移除收敛、搜索工作量和 SAT 上限。
Python 层不会另加表达式限制，内存安全和 IR 表示边界仍然适用。对应的 C 入口为
`neverd_simplify_expr`、`neverd_synthesize_expr` 和
`neverd_optimize_llvm_ir`，并提供类型化释放函数和版本化 JSON 适配器。

## 错误、隔离与信任

Python 异常绝不会穿过 C++ 栈展开。NeverD 会捕获完整的格式化 traceback，并通过 `neverd_last_error` 暴露。每个规范插件路径都使用唯一模块名加载；终止时模块会被移除，之后重新加载会得到全新的模块与类状态。CPython 只初始化一次，启动阶段的 GIL 会被释放，任何宿主线程执行回调时都会获取 GIL；NeverD 不会终结可能与其他组件共享的解释器。

插件在 NeverD 进程内执行任意 Python，并且可以调用完整 C API。只加载可信文件。这是扩展边界，不是沙箱。

## 编写、测试与打包

若需要编辑器和类型检查支持，请安装纯 Python 包，或把源码树加入 `PYTHONPATH`：

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

审计要求每个导出的 C 声明与其 `ctypes` 签名和所有权规则完全一致；同时检查输出语言值、CMake 与包版本、CI 功能开关、Action 固定版本、制品流转及 PyPI OIDC 策略。原生适配器测试为 `NeverDPluginRuntimeTests`；嵌入式 Python 测试为 `NeverDPythonRuntimeTests` 和 `NeverDPythonPluginTests`。

`Python Plugin SDK` 工作流构建一个 wheel 和一份源码发行包，将二者安装到干净环境并上传经过验证的制品。只有已发布的 GitHub Release 才会经受审批保护的 `pypi` environment 和 Trusted Publishing 发布；不使用长期 PyPI token。
