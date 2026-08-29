**语言**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← 文档索引](README.zh-CN.md)

# 原生插件

NeverD 原生插件是加载到宿主进程中的可信共享库。它们使用
`neverd/sdk/NeverDPlugin.h` 中的纯 C 声明，并调用
`neverd/sdk/NeverDCAPI.h` 中的公开 C API。如果更适合在进程内使用 Python
编写扩展，请参阅 [Python 插件指南](python-plugins.zh-CN.md)。

## 兼容性与信任边界

当前 `neverd_plugin_t` 描述符没有 ABI 版本或结构体大小字段。插件必须使用将要加载
它的同一 NeverD 精确 revision 所部署的头文件构建；每次升级 NeverD 后都要重新构建
插件。插件与宿主还必须使用相同的操作系统、架构以及 ABI 兼容的工具链。

原生插件是在进程内执行的任意代码。NeverD 不会对其进行 sandbox 隔离、隔离崩溃，
也不会限制它访问会话或宿主进程。请只加载你信任的插件。

## 描述符与回调

每个库都要导出一个且仅一个名为 `neverd_plugin` 的数据符号：

```c
#include "neverd/sdk/NeverDPlugin.h"

static int on_init(neverd_session_t session) {
  (void)session;
  return 0;
}

static void on_term(void) {}

static int on_run(neverd_session_t session, int arg) {
  (void)session;
  return arg;
}

static int on_event(const neverd_event_t *event) {
  if (event && event->Type == NEVERD_EVT_BINARY_LOADED) {
    const char *path = event->Data.BinaryLoaded.Path; /* borrowed */
    (void)path;
  }
  return 0;
}

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "My Plugin",
    .Version = "1.0.0",
    .Author = "Your Name",
    .Description = "A native NeverD extension",
    .Type = NEVERD_PLUGIN_GENERIC,
    .Init = on_init,
    .Term = on_term,
    .Run = on_run,
    .Event = on_event,
};
```

在 Windows 上，`NEVERD_PLUGIN_EXPORT` 展开为 `__declspec(dllexport)`；在其他
平台上则使用默认 ELF/Mach-O visibility。请保持源文件为 C；如果必须用 C++ 实现，
请显式为描述符指定 C linkage。

`Name` 不得为空，并且在同一个宿主中必须唯一。宿主会在加载时复制全部四个元数据
字符串。`Version`、`Author` 和 `Description` 可以为空。四种 type 值仅用于分类，
只是元数据标签：

| 值 | 当前含义 |
|----|----------|
| `NEVERD_PLUGIN_GENERIC` | 通用扩展标签 |
| `NEVERD_PLUGIN_LOADER` | Loader 标签；不会注册二进制 loader |
| `NEVERD_PLUGIN_PROCESSOR` | 分析/处理标签；不会调度工作 |
| `NEVERD_PLUGIN_UI` | UI 标签；NeverD 不提供原生插件 GUI 宿主 |

所有回调指针都是可选的。调用会在宿主调用方的线程上直接、同步执行。

| 回调 | 契约 |
|------|------|
| `Init(session)` | 成功时返回 `0`。非零结果会记录错误；初始化失败后不会调用该插件的 `Term`。 |
| `Term()` | 仅在 `Init` 成功后于 teardown 时调用，随后卸载该库。 |
| `Run(session, arg)` | 执行插件工作并返回整数结果。CLI 传入 `0`；嵌入式 C API 可以选择其他参数。缺少该回调时返回 `-1`。 |
| `Event(event)` | 处理宿主派发的事件。成功时返回 `0`；非零结果会记录错误。缺少该回调时不执行任何操作。 |

通常的嵌入顺序是加载、初始化、运行或派发事件，最后终止。C API 不会强制 `Run` 或
`Event` 遵循该顺序，因此生命周期由嵌入宿主管理。

## 事件由宿主派发

`neverd_event_t` 包含以下六种事件值之一：

| 事件 | `event->Data` 中的 payload |
|------|----------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | 无 payload |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | 无 payload |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

会话 API 和 `neverd` 命令行工具都不会自动发出事件。嵌入宿主负责构造并派发事件：

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

事件和 payload 字符串都是借用值，有效期仅到回调返回为止；不要释放或保留它们。
会话句柄归宿主所有：插件不得销毁它，也不得在宿主 teardown 后继续使用。NeverD C API
分配的结果归 NeverD 所有，必须使用 `neverd_free_string()` 释放。NeverD 不保证回调或
会话的并发访问安全；除非嵌入宿主自行提供安全同步，否则应串行执行生命周期、run 和
event 调用。

## 构建内置示例

需要显式启用示例；默认值仍为 `NEVERD_BUILD_PLUGINS=OFF`：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

使用单配置 generator 时，相关产物位于：

| 产物 | 路径 |
|------|------|
| CLI | `build/bin/neverd`（Windows 上为 `neverd.exe`） |
| 宿主库 | `build/bin/libneverd.so`、`build/bin/libneverd.dylib` 或 `build/bin/neverd.dll` |
| 已部署头文件 | `build/bin/sdk/neverd/sdk/` |
| 示例 | `build/bin/plugins/example_plugin.so`、`.dylib` 或 `.dll` |

多配置构建会把相同的运行时产物放到所选配置之下。例如使用
`build/bin/Release/` 时，插件位于 `build/bin/Release/plugins/`，头文件位于
`build/bin/Release/sdk/`：

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## 构建独立插件

NeverD 当前不会安装 SDK，也不提供 `NeverDConfig.cmake`，因此没有受支持的
`find_package(NeverD)`。独立构建必须显式指向已部署的头文件，以及同一精确宿主
构建生成的链接库：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_neverd_plugin LANGUAGES C)

set(NEVERD_SDK_ROOT "" CACHE PATH
    "Directory containing neverd/sdk/NeverDPlugin.h")
set(NEVERD_LINK_LIBRARY "" CACHE FILEPATH
    "libneverd.so, libneverd.dylib, or the Windows neverd.lib import library")

if(NOT EXISTS "${NEVERD_SDK_ROOT}/neverd/sdk/NeverDPlugin.h")
  message(FATAL_ERROR "Set NEVERD_SDK_ROOT to the staged NeverD SDK")
endif()
if(NOT EXISTS "${NEVERD_LINK_LIBRARY}")
  message(FATAL_ERROR "Set NEVERD_LINK_LIBRARY to the matching libneverd")
endif()

add_library(my_plugin SHARED my_plugin.c)
target_include_directories(my_plugin PRIVATE "${NEVERD_SDK_ROOT}")
target_link_libraries(my_plugin PRIVATE "${NEVERD_LINK_LIBRARY}")
set_target_properties(my_plugin PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  PREFIX "")
```

配置时设置 `NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk`（或特定配置的
`sdk` 目录）。在 Linux/macOS 上，`NEVERD_LINK_LIBRARY` 是匹配的 `.so`/`.dylib`。
在 Windows 上，它是 generator 生成的 `neverd.lib` import library；匹配的
`neverd.dll` 必须与宿主可执行文件放在一起。import library 的位置取决于 generator，
因此请显式传入实际文件。

## 发现顺序与 CLI walkthrough

CLI 按以下顺序扫描目录；较早出现的规范路径和插件名优先：

1. 正在运行的 `neverd` 可执行文件旁的 `plugins`。
2. `$HOME/.neverd/plugins`（`HOME` 非空时优先使用；Windows 上回退到原生 profile 目录）。
3. `NEVERD_PLUGIN_PATH` 中每个非空条目，按出现顺序。
4. 通过 `--plugin-dir` 指定的目录。

`NEVERD_PLUGIN_PATH` 在 Linux/macOS 上使用 `:` 分隔条目，在 Windows 上使用 `;`。
规范化后相同的目录只扫描一次。原生库只识别宿主平台对应的后缀：Linux 为 `.so`，
macOS 为 `.dylib`，Windows 为 `.dll`。启用 Python 的构建还会扫描 `.py` 文件。

构建树中的示例已经位于可执行文件旁的目录中：

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

多配置构建请把 `build/bin` 替换为 `build/bin/Release`。如果某个 NeverD 可执行文件
旁还没有同名插件，可使用对应宿主平台的命令将副本安装到用户目录，然后对该可执行文件
使用相同的 `--list` 和 `--run` 选项：

```bash
# Linux
mkdir -p "$HOME/.neverd/plugins"
cp build/bin/plugins/example_plugin.so "$HOME/.neverd/plugins/"

# macOS
mkdir -p "$HOME/.neverd/plugins"
cp build/bin/plugins/example_plugin.dylib "$HOME/.neverd/plugins/"

# Windows PowerShell
New-Item -ItemType Directory -Force "$HOME/.neverd/plugins"
Copy-Item build/bin/plugins/example_plugin.dll "$HOME/.neverd/plugins/"
```

缺少可选的 sibling/home 目录时会直接忽略。可选目录中存在格式错误的插件时会产生
warning，但扫描仍会继续。`NEVERD_PLUGIN_PATH` 和 `--plugin-dir` 指定的每个目录都是
必需的：目录不存在或有候选文件被拒绝时，CLI 会以非零状态退出。重复的规范文件、重复
的插件名、缺失的 `neverd_plugin` export 和无效的描述符 type 都会被拒绝。直接调用
`neverd_plugins_load_file` 时，还会拒绝后缀不受支持的文件。

嵌入宿主应同时检查插件管理结果和 `neverd_last_error(session)`。
`neverd_plugins_load_file` 返回 `1` 或 `0`；`neverd_plugins_load_dir` 返回成功加载的
数量，即使部分成功也可能报告被拒绝的候选文件。`neverd_plugins_run` 返回插件结果，
当插件不存在或无法运行时使用 `-1`。C API 返回的每个错误字符串或 JSON 字符串都必须
使用 `neverd_free_string()` 释放。
