**語言**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← 文件索引](README.zh-TW.md)

# 原生外掛

NeverD 原生外掛是載入宿主處理程序中的可信賴共享函式庫。它們使用
`neverd/sdk/NeverDPlugin.h` 中的純 C 宣告，並呼叫
`neverd/sdk/NeverDCAPI.h` 中的公開 C API。如果較適合在處理程序內使用 Python
撰寫擴充功能，請參閱 [Python 外掛指南](python-plugins.zh-TW.md)。

## 相容性與信任邊界

目前的 `neverd_plugin_t` 描述元沒有 ABI 版本或結構大小欄位。外掛必須使用將要載入
它的同一 NeverD 精確 revision 所部署的標頭建置；每次升級 NeverD 後都要重新建置
外掛。外掛與宿主也必須使用相同的作業系統、架構以及 ABI 相容的工具鏈。

原生外掛是在處理程序內執行的任意程式碼。NeverD 不會將其置於 sandbox、隔離
crash，也不會限制它存取工作階段或宿主處理程序。請只載入你信任的外掛。

## 描述元與回呼

每個函式庫都要匯出一個且僅一個名為 `neverd_plugin` 的資料符號：

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

在 Windows 上，`NEVERD_PLUGIN_EXPORT` 展開為 `__declspec(dllexport)`；在其他
平台則使用預設 ELF/Mach-O visibility。請保持原始檔為 C；如果必須以 C++ 實作，
請明確為描述元指定 C linkage。

`Name` 不得為空，且在同一個宿主中必須唯一。宿主會在載入時複製全部四個中繼資料
字串。`Version`、`Author` 和 `Description` 可以為空。四種 type 值僅用於分類，
只是中繼資料標籤：

| 值 | 目前含義 |
|----|----------|
| `NEVERD_PLUGIN_GENERIC` | 通用擴充標籤 |
| `NEVERD_PLUGIN_LOADER` | Loader 標籤；不會註冊二進位 loader |
| `NEVERD_PLUGIN_PROCESSOR` | 分析/處理標籤；不會排程工作 |
| `NEVERD_PLUGIN_UI` | UI 標籤；NeverD 不提供原生外掛 GUI 宿主 |

所有回呼指標都是選用的。呼叫會在宿主呼叫方的執行緒上直接、同步執行。

| 回呼 | 契約 |
|------|------|
| `Init(session)` | 成功時傳回 `0`。非零結果會記錄錯誤；初始化失敗後不會呼叫該外掛的 `Term`。 |
| `Term()` | 僅在 `Init` 成功後於 teardown 時呼叫，接著卸載該函式庫。 |
| `Run(session, arg)` | 執行外掛工作並傳回整數結果。CLI 傳入 `0`；嵌入式 C API 可以選擇其他引數。缺少該回呼時傳回 `-1`。 |
| `Event(event)` | 處理宿主分派的事件。成功時傳回 `0`；非零結果會記錄錯誤。缺少該回呼時不執行任何操作。 |

一般的嵌入順序是載入、初始化、執行或分派事件，最後終止。C API 不會強制 `Run` 或
`Event` 遵循該順序，因此生命週期由嵌入宿主管理。

## 事件由宿主分派

`neverd_event_t` 包含以下六種事件值之一：

| 事件 | `event->Data` 中的 payload |
|------|----------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | 無 payload |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | 無 payload |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

工作階段 API 和 `neverd` 命令列工具都不會自動發出事件。嵌入宿主負責建構並分派事件：

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

事件和 payload 字串都是借用值，有效期僅到回呼傳回為止；不要釋放或保留它們。
工作階段 handle 歸宿主所有：外掛不得銷毀它，也不得在宿主 teardown 後繼續使用。
NeverD C API 配置的結果歸 NeverD 所有，必須使用 `neverd_free_string()` 釋放。
NeverD 不保證回呼或工作階段的並行存取安全；除非嵌入宿主自行提供安全同步，否則應
循序執行生命週期、run 和 event 呼叫。

## 建置內附範例

需要明確啟用範例；預設值仍為 `NEVERD_BUILD_PLUGINS=OFF`：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

使用單一設定 generator 時，相關產物位於：

| 產物 | 路徑 |
|------|------|
| CLI | `build/bin/neverd`（Windows 上為 `neverd.exe`） |
| 宿主函式庫 | `build/bin/libneverd.so`、`build/bin/libneverd.dylib` 或 `build/bin/neverd.dll` |
| 已部署標頭 | `build/bin/sdk/neverd/sdk/` |
| 範例 | `build/bin/plugins/example_plugin.so`、`.dylib` 或 `.dll` |

多重設定建置會將相同的 runtime 產物放到所選設定之下。例如使用
`build/bin/Release/` 時，外掛位於 `build/bin/Release/plugins/`，標頭位於
`build/bin/Release/sdk/`：

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## 建置獨立外掛

NeverD 目前不會安裝 SDK，也不提供 `NeverDConfig.cmake`，因此沒有受支援的
`find_package(NeverD)`。獨立建置必須明確指向已部署的標頭，以及同一精確宿主建置
產生的連結函式庫：

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

設定時使用 `NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk`（或特定設定的
`sdk` 目錄）。在 Linux/macOS 上，`NEVERD_LINK_LIBRARY` 是相符的 `.so`/`.dylib`。
在 Windows 上，它是 generator 產生的 `neverd.lib` import library；相符的
`neverd.dll` 必須與宿主執行檔放在一起。import library 的位置取決於 generator，
因此請明確傳入實際檔案。

## 探索順序與 CLI walkthrough

CLI 依照以下順序掃描目錄；較早出現的 canonical path 和外掛名稱優先：

1. 正在執行的 `neverd` 執行檔旁的 `plugins`。
2. `$HOME/.neverd/plugins`（`HOME` 非空時優先使用；Windows 上回退到原生 profile 目錄）。
3. `NEVERD_PLUGIN_PATH` 中每個非空項目，依出現順序。
4. 透過 `--plugin-dir` 指定的目錄。

`NEVERD_PLUGIN_PATH` 在 Linux/macOS 上使用 `:` 分隔項目，在 Windows 上使用 `;`。
canonical 後相同的目錄只掃描一次。原生函式庫只識別宿主平台對應的 suffix：Linux
為 `.so`，macOS 為 `.dylib`，Windows 為 `.dll`。啟用 Python 的建置還會掃描
`.py` 檔案。

建置樹中的範例已位於執行檔旁的目錄中：

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

多重設定建置請將 `build/bin` 替換為 `build/bin/Release`。如果某個 NeverD 執行檔
旁還沒有同名外掛，可使用對應宿主平台的命令將副本安裝到使用者目錄，再對該執行檔
使用相同的 `--list` 和 `--run` 選項：

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

缺少選用的 sibling/home 目錄時會直接忽略。選用目錄中存在格式錯誤的外掛時會產生
warning，但掃描仍會繼續。`NEVERD_PLUGIN_PATH` 和 `--plugin-dir` 指定的每個目錄都是
必要的：目錄不存在或有候選檔案遭到拒絕時，CLI 會以非零狀態結束。重複的 canonical
檔案、重複的外掛名稱、缺少 `neverd_plugin` export 和無效的描述元 type 都會遭到
拒絕。直接呼叫 `neverd_plugins_load_file` 時，還會拒絕 suffix 不受支援的檔案。

嵌入宿主應同時檢查外掛管理結果和 `neverd_last_error(session)`。
`neverd_plugins_load_file` 傳回 `1` 或 `0`；`neverd_plugins_load_dir` 傳回成功載入的
數量，即使部分成功也可能報告遭到拒絕的候選檔案。`neverd_plugins_run` 傳回外掛結果，
當外掛不存在或無法執行時使用 `-1`。C API 傳回的每個錯誤字串或 JSON 字串都必須
使用 `neverd_free_string()` 釋放。
