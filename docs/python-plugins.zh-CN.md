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

仓库提供了 [`minimal.py`](../pluginsdk/python/examples/minimal.py) 和 [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py) 两个示例。

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

六种不可变事件分别是 `BINARY_LOADED`、`BINARY_CLOSING`、`FUNCTION_SELECTED`、`ADDRESS_CHANGED`、`ANALYSIS_DONE` 和 `PATCH_APPLIED`。回调期间会复制载荷字符串；与当前事件类型无关的字段为 `None`。

切勿保存 `Session` 并在终止后继续使用。原生 capsule 会在 `on_term` 开始前以及原生会话释放前失效。后续调用会抛出 `RuntimeError`，而不会解引用过期内存。

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
