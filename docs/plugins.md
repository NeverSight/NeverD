**Languages**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← Documentation Index](README.md)

# Native plugins

NeverD native plugins are trusted shared libraries loaded into the host
process. They use the pure-C declarations in `neverd/sdk/NeverDPlugin.h` and
call the public C API in `neverd/sdk/NeverDCAPI.h`. Use the
[Python plugin guide](python-plugins.md) when process-local Python authoring is
more appropriate.

## Compatibility and trust boundary

The current `neverd_plugin_t` descriptor has no ABI version or structure-size
field. Build a plugin with the headers staged by the exact NeverD revision that
will load it, and rebuild the plugin whenever NeverD is upgraded. The plugin and
host must also use the same operating system and architecture and
ABI-compatible toolchains.

Native plugins are arbitrary in-process code. NeverD does not sandbox them,
isolate crashes, or restrict their access to the session or host process. Load
only plugins that you trust.

## Descriptor and callbacks

Each library exports one data symbol named exactly `neverd_plugin`:

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

`NEVERD_PLUGIN_EXPORT` expands to `__declspec(dllexport)` on Windows and
default ELF/Mach-O visibility elsewhere. Keep the source as C, or give the
descriptor C linkage explicitly if a C++ implementation is unavoidable.

`Name` must be non-empty and unique within one host. The host snapshots all
four metadata strings while loading. `Version`, `Author`, and `Description`
may be empty. The four type values are classification metadata only:

| Value | Meaning today |
|-------|---------------|
| `NEVERD_PLUGIN_GENERIC` | General extension label |
| `NEVERD_PLUGIN_LOADER` | Loader label; it does not register a binary loader |
| `NEVERD_PLUGIN_PROCESSOR` | Analysis/processing label; it does not schedule work |
| `NEVERD_PLUGIN_UI` | UI label; NeverD does not provide a native plugin GUI host |

All callback pointers are optional. Calls are direct and synchronous on the
host caller's thread.

| Callback | Contract |
|----------|----------|
| `Init(session)` | Return `0` on success. A nonzero result records an error; `Term` will not be called for that failed initialization. |
| `Term()` | Called during teardown only after a successful `Init`. The library is then unloaded. |
| `Run(session, arg)` | Perform plugin work and return an integer result. The CLI passes `0`; the embedding C API may choose another argument. A missing callback returns `-1`. |
| `Event(event)` | Handle a host-dispatched event. Return `0` on success; a nonzero result records an error. A missing callback is a no-op. |

The normal embedding order is load, initialize, run or dispatch events, then
terminate. The C API does not enforce that ordering for `Run` or `Event`, so the
embedding host owns the lifecycle.

## Events are host-dispatched

`neverd_event_t` carries one of six event values:

| Event | Payload in `event->Data` |
|-------|--------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | No payload |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | No payload |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

Events are not emitted automatically by session APIs or by the `neverd`
command-line tool. An embedding host constructs the event and dispatches it:

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

The event and payload strings are borrowed until the callback returns; do not
free or retain them. The session handle is host-owned: a plugin must not destroy
it or use it after host teardown. Results allocated by the NeverD C API belong
to NeverD and must be released with `neverd_free_string()`. NeverD does not
promise concurrent callback or session access: embedding hosts should serialize
lifecycle, run, and event calls unless they provide their own safe
synchronization.

## Build the bundled example

Enable examples explicitly; the default remains `NEVERD_BUILD_PLUGINS=OFF`:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

For a single-configuration generator, the relevant artifacts are:

| Artifact | Path |
|----------|------|
| CLI | `build/bin/neverd` (`neverd.exe` on Windows) |
| Host library | `build/bin/libneverd.so`, `build/bin/libneverd.dylib`, or `build/bin/neverd.dll` |
| Staged headers | `build/bin/sdk/neverd/sdk/` |
| Example | `build/bin/plugins/example_plugin.so`, `.dylib`, or `.dll` |

A multi-configuration build places the same runtime artifacts beneath the
selected configuration, for example `build/bin/Release/`, with the plugin in
`build/bin/Release/plugins/` and headers in `build/bin/Release/sdk/`:

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## Build a standalone plugin

NeverD does not currently install its SDK or provide a `NeverDConfig.cmake`, so
there is no supported `find_package(NeverD)`. Point a standalone build at the
staged headers and an explicit link library from the exact host build:

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

Configure with `NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk` (or the
configuration-specific `sdk` directory). On Linux/macOS,
`NEVERD_LINK_LIBRARY` is the matching `.so`/`.dylib`. On Windows it is the
generator-produced `neverd.lib` import library, while the matching `neverd.dll`
must remain with the host executable. Import-library locations are
generator-dependent, so pass the actual file explicitly.

## Discovery and CLI walkthrough

The CLI scans directories in this order; earlier canonical paths and plugin
names win:

1. `plugins` beside the running `neverd` executable.
2. `$HOME/.neverd/plugins` (`HOME` is used when non-empty; on Windows, the native profile directory is the fallback).
3. Each non-empty entry in `NEVERD_PLUGIN_PATH`, in order.
4. The directory supplied by `--plugin-dir`.

`NEVERD_PLUGIN_PATH` uses `:` between entries on Linux/macOS and `;` on
Windows. Canonically equivalent directories are scanned once. For native
libraries, only the host suffix is considered: `.so` on Linux, `.dylib` on
macOS, and `.dll` on Windows. Python-enabled builds also scan `.py` files.

The build-tree example is already in the executable's sibling directory:

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

For a multi-configuration build, replace `build/bin` with
`build/bin/Release`. To install a copy for a NeverD executable that does not
already have the same plugin beside it, use the command for the host platform,
then run that executable with the same `--list` and `--run` options:

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

Missing optional sibling/home directories are ignored. A malformed plugin in
an optional directory produces a warning and scanning continues. Every
directory named by `NEVERD_PLUGIN_PATH` and `--plugin-dir` is required: a
missing directory or rejected candidate makes the CLI exit nonzero. Duplicate
canonical files, duplicate plugin names, a missing `neverd_plugin` export, and
invalid descriptor types are rejected. A direct `neverd_plugins_load_file`
call also rejects a file with an unsupported suffix.

Embedding hosts should check both plugin-management results and
`neverd_last_error(session)`. `neverd_plugins_load_file` returns `1` or `0`;
`neverd_plugins_load_dir` returns the number loaded and can report rejected
candidates even after partial success. `neverd_plugins_run` returns the plugin
result and uses `-1` when the plugin is absent or cannot run. Free every error
or JSON string returned by the C API with `neverd_free_string()`.
