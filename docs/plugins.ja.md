**言語**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← ドキュメント索引](README.ja.md)

# ネイティブプラグイン

NeverD のネイティブプラグインは、ホストプロセス内に読み込まれる信頼済みの
共有ライブラリです。`neverd/sdk/NeverDPlugin.h` の純粋 C 宣言を使い、
`neverd/sdk/NeverDCAPI.h` の公開 C API を呼び出します。プロセス内で Python を
使う方が適切な場合は、[Python プラグインガイド](python-plugins.ja.md)を参照して
ください。

## 互換性と信頼境界

現在の `neverd_plugin_t` descriptor には ABI version も構造体サイズ field も
ありません。プラグインは、それを読み込む NeverD とまったく同じ revision が
配置した header でビルドし、NeverD を更新するたびに再ビルドしてください。
プラグインとホストは、同じ OS とアーキテクチャ、および ABI 互換の toolchain
も使用する必要があります。

ネイティブプラグインは任意のプロセス内コードです。NeverD は sandbox 化、
crash 分離、session やホストプロセスへのアクセス制限を行いません。信頼できる
プラグインだけを読み込んでください。

## Descriptor と callback

各ライブラリは、正確に `neverd_plugin` という名前の data symbol を 1 つ
export します。

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

`NEVERD_PLUGIN_EXPORT` は Windows では `__declspec(dllexport)`、それ以外では
ELF/Mach-O の default visibility に展開されます。source は C のままにするか、
C++ 実装が避けられない場合は descriptor に明示的な C linkage を与えてください。

`Name` は空でなく、1 つのホスト内で一意でなければなりません。ホストは読み込み時に
4 つすべての metadata string を snapshot します。`Version`、`Author`、
`Description` は空でもかまいません。4 つの type 値は分類用 metadata にすぎません。

| 値 | 現在の意味 |
|----|------------|
| `NEVERD_PLUGIN_GENERIC` | 汎用 extension label |
| `NEVERD_PLUGIN_LOADER` | Loader label。binary loader は登録しない |
| `NEVERD_PLUGIN_PROCESSOR` | 解析／処理 label。work は schedule しない |
| `NEVERD_PLUGIN_UI` | UI label。NeverD はネイティブプラグイン用 GUI host を提供しない |

すべての callback pointer は任意です。呼び出しはホスト caller の thread 上で直接、
同期的に行われます。

| Callback | 契約 |
|----------|------|
| `Init(session)` | 成功時は `0` を返します。非 0 は error を記録し、初期化に失敗したプラグインの `Term` は呼ばれません。 |
| `Term()` | `Init` が成功した場合だけ teardown 時に呼ばれ、その後ライブラリが unload されます。 |
| `Run(session, arg)` | プラグインの処理を行い整数結果を返します。CLI は `0` を渡し、埋め込み C API は別の値を選べます。callback がない場合は `-1` です。 |
| `Event(event)` | ホストが dispatch した event を処理します。成功時は `0`、非 0 は error を記録します。callback がない場合は何もしません。 |

通常の埋め込み順序は、load、initialize、run または event dispatch、terminate です。
C API は `Run` や `Event` にこの順序を強制しないため、lifecycle は埋め込みホストが
所有します。

## Event はホストが dispatch する

`neverd_event_t` は 6 種類の event 値のいずれかを保持します。

| Event | `event->Data` 内の payload |
|-------|----------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | payload なし |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | payload なし |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

Event は session API や `neverd` CLI から自動的には発行されません。埋め込みホストが
event を構築して dispatch します。

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

Event と payload string は callback が返るまでの借用物です。解放したり保持したり
しないでください。session handle はホスト所有であり、プラグインが破棄したり、
ホストの teardown 後に使ったりしてはいけません。NeverD C API が割り当てた結果は
NeverD が所有し、`neverd_free_string()` で解放する必要があります。NeverD は callback
や session への並行アクセスを保証しません。埋め込みホストは、自身で安全な同期を
用意しない限り lifecycle、run、event の呼び出しを直列化してください。

## 同梱 example のビルド

Example は明示的に有効化します。default は `NEVERD_BUILD_PLUGINS=OFF` のままです。

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

single-configuration generator で関連する artifact は次の場所にあります。

| Artifact | Path |
|----------|------|
| CLI | `build/bin/neverd`（Windows では `neverd.exe`） |
| Host library | `build/bin/libneverd.so`、`build/bin/libneverd.dylib`、または `build/bin/neverd.dll` |
| 配置済み header | `build/bin/sdk/neverd/sdk/` |
| Example | `build/bin/plugins/example_plugin.so`、`.dylib`、または `.dll` |

multi-configuration build は、同じ runtime artifact を選択した configuration の下に
置きます。たとえば `build/bin/Release/` の場合、プラグインは
`build/bin/Release/plugins/`、header は `build/bin/Release/sdk/` にあります。

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## Standalone plugin のビルド

NeverD は現在 SDK を install せず、`NeverDConfig.cmake` も提供しないため、
サポートされる `find_package(NeverD)` はありません。standalone build には、
配置済み header と、まったく同じホスト build の link library を明示的に渡します。

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

`NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk`（または configuration 固有の
`sdk` directory）で configure します。Linux/macOS では
`NEVERD_LINK_LIBRARY` は一致する `.so`/`.dylib` です。Windows では generator が
生成した `neverd.lib` import library を指定し、一致する `neverd.dll` はホスト
executable と同じ場所に残してください。import library の場所は generator ごとに
異なるため、実際の file を明示的に渡します。

## Discovery と CLI walkthrough

CLI は次の順に directory を scan します。先に現れた canonical path と plugin name
が優先されます。

1. 実行中の `neverd` executable と同じ場所の `plugins`。
2. `$HOME/.neverd/plugins`（`HOME` が空でなければ使用し、Windows では native profile directory にフォールバック）。
3. `NEVERD_PLUGIN_PATH` の空でない各 entry（記述順）。
4. `--plugin-dir` で指定した directory。

`NEVERD_PLUGIN_PATH` の区切りは Linux/macOS では `:`、Windows では `;` です。
canonical に同一な directory は 1 回だけ scan されます。ネイティブライブラリでは、
host の suffix だけが対象です。Linux は `.so`、macOS は `.dylib`、Windows は
`.dll` です。Python 対応 build は `.py` file も scan します。

build tree の example は、すでに executable と同階層の directory にあります。

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

multi-configuration build では `build/bin` を `build/bin/Release` に置き換えます。
同じプラグインがまだ隣にない NeverD executable 用に copy を install するには、
host platform に合う command を使い、その executable を同じ `--list` と `--run`
option で実行します。

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

任意の sibling/home directory が存在しない場合は無視されます。任意 directory 内の
不正なプラグインは warning になりますが、scan は継続します。
`NEVERD_PLUGIN_PATH` と `--plugin-dir` が示す各 directory は必須です。directory が
存在しない、または candidate が reject されると CLI は非 0 で終了します。
canonical file や plugin name の重複、`neverd_plugin` export の欠落、不正な
descriptor type は reject されます。`neverd_plugins_load_file` の直接呼び出しは、
未対応 suffix の file も reject します。

埋め込みホストは plugin-management の結果と `neverd_last_error(session)` の両方を
確認してください。`neverd_plugins_load_file` は `1` または `0` を返します。
`neverd_plugins_load_dir` は load できた数を返し、部分的に成功していても reject した
candidate を報告できます。`neverd_plugins_run` は plugin の結果を返し、plugin が
存在しないか実行できない場合は `-1` を使います。C API が返すすべての error string
または JSON string は `neverd_free_string()` で解放してください。
