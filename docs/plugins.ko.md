**언어**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← 문서 색인](README.ko.md)

# 네이티브 플러그인

NeverD 네이티브 플러그인은 호스트 프로세스에 로드되는 신뢰된 공유 라이브러리입니다.
`neverd/sdk/NeverDPlugin.h`의 순수 C 선언을 사용하고
`neverd/sdk/NeverDCAPI.h`의 공개 C API를 호출합니다. 프로세스 내 Python 작성 방식이
더 적합하다면 [Python 플러그인 가이드](python-plugins.ko.md)를 사용하십시오.

## 호환성과 신뢰 경계

현재 `neverd_plugin_t` descriptor에는 ABI version이나 구조체 크기 field가 없습니다.
플러그인을 로드할 NeverD와 정확히 같은 revision이 배치한 header로 빌드하고, NeverD를
업그레이드할 때마다 플러그인을 다시 빌드하십시오. 플러그인과 호스트는 같은 운영 체제와
아키텍처 및 ABI 호환 toolchain도 사용해야 합니다.

네이티브 플러그인은 임의의 프로세스 내 코드입니다. NeverD는 플러그인을 sandbox에
격리하거나 crash를 분리하지 않으며, session 또는 호스트 프로세스 접근을 제한하지도
않습니다. 신뢰하는 플러그인만 로드하십시오.

## Descriptor와 callback

각 라이브러리는 이름이 정확히 `neverd_plugin`인 data symbol 하나를 export합니다.

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

`NEVERD_PLUGIN_EXPORT`는 Windows에서 `__declspec(dllexport)`로, 그 밖의
환경에서 기본 ELF/Mach-O visibility로 확장됩니다. source는 C로 유지하십시오.
C++ 구현이 불가피하다면 descriptor에 C linkage를 명시적으로 부여해야 합니다.

`Name`은 비어 있지 않아야 하며 한 호스트 안에서 고유해야 합니다. 호스트는 로드할 때
metadata string 네 개를 모두 snapshot합니다. `Version`, `Author`, `Description`은
비어 있어도 됩니다. 네 가지 type 값은 분류용 metadata label일 뿐입니다.

| 값 | 현재 의미 |
|----|-----------|
| `NEVERD_PLUGIN_GENERIC` | 범용 extension label |
| `NEVERD_PLUGIN_LOADER` | Loader label이며 binary loader를 등록하지 않음 |
| `NEVERD_PLUGIN_PROCESSOR` | 분석/처리 label이며 작업을 schedule하지 않음 |
| `NEVERD_PLUGIN_UI` | UI label이며 NeverD는 네이티브 플러그인 GUI host를 제공하지 않음 |

모든 callback pointer는 선택 사항입니다. 호출은 호스트 caller의 thread에서 직접
동기적으로 실행됩니다.

| Callback | 계약 |
|----------|------|
| `Init(session)` | 성공 시 `0`을 반환합니다. 0이 아니면 error를 기록하며 초기화에 실패한 플러그인의 `Term`은 호출하지 않습니다. |
| `Term()` | `Init`이 성공한 경우에만 teardown 중에 호출하며, 이후 라이브러리를 unload합니다. |
| `Run(session, arg)` | 플러그인 작업을 수행하고 정수 결과를 반환합니다. CLI는 `0`을 전달하며 embedding C API는 다른 인자를 선택할 수 있습니다. callback이 없으면 `-1`을 반환합니다. |
| `Event(event)` | 호스트가 dispatch한 event를 처리합니다. 성공 시 `0`을 반환하고 0이 아니면 error를 기록합니다. callback이 없으면 아무 작업도 하지 않습니다. |

일반적인 embedding 순서는 load, initialize, run 또는 event dispatch, terminate입니다.
C API는 `Run`이나 `Event`에 이 순서를 강제하지 않으므로 embedding host가 lifecycle을
소유합니다.

## Event는 호스트가 dispatch함

`neverd_event_t`에는 여섯 가지 event 값 중 하나가 들어갑니다.

| Event | `event->Data`의 payload |
|-------|-------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | payload 없음 |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | payload 없음 |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

Event는 session API나 `neverd` 명령줄 도구가 자동으로 발생시키지 않습니다. embedding
host가 event를 생성하여 dispatch합니다.

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

Event와 payload string은 callback이 반환할 때까지만 빌린 값입니다. 해제하거나 보관하지
마십시오. session handle은 호스트 소유이므로 플러그인이 파괴하거나 호스트 teardown
이후 사용해서는 안 됩니다. NeverD C API가 할당한 결과는 NeverD 소유이며
`neverd_free_string()`으로 해제해야 합니다. NeverD는 callback이나 session의 동시
접근을 보장하지 않습니다. embedding host는 자체적으로 안전한 동기화를 제공하지 않는 한
lifecycle, run 및 event 호출을 직렬화해야 합니다.

## 번들 예제 빌드

예제를 명시적으로 활성화하십시오. 기본값은 계속 `NEVERD_BUILD_PLUGINS=OFF`입니다.

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

single-configuration generator에서 관련 artifact 경로는 다음과 같습니다.

| Artifact | Path |
|----------|------|
| CLI | `build/bin/neverd`(Windows에서는 `neverd.exe`) |
| Host library | `build/bin/libneverd.so`, `build/bin/libneverd.dylib` 또는 `build/bin/neverd.dll` |
| 배치된 header | `build/bin/sdk/neverd/sdk/` |
| Example | `build/bin/plugins/example_plugin.so`, `.dylib` 또는 `.dll` |

multi-configuration build는 같은 runtime artifact를 선택한 configuration 아래에 둡니다.
예를 들어 `build/bin/Release/`를 선택하면 플러그인은
`build/bin/Release/plugins/`에, header는 `build/bin/Release/sdk/`에 있습니다.

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## Standalone 플러그인 빌드

NeverD는 현재 SDK를 install하거나 `NeverDConfig.cmake`를 제공하지 않으므로 지원되는
`find_package(NeverD)`가 없습니다. standalone build에는 배치된 header와 정확히 같은
호스트 build의 link library를 명시적으로 지정하십시오.

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

`NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk`(또는 configuration별 `sdk`
directory)로 configure합니다. Linux/macOS에서는 `NEVERD_LINK_LIBRARY`에 일치하는
`.so`/`.dylib`를 지정합니다. Windows에서는 generator가 생성한 `neverd.lib` import
library를 지정하고, 일치하는 `neverd.dll`은 호스트 executable과 함께 두어야 합니다.
import library 위치는 generator마다 다르므로 실제 file을 명시적으로 전달하십시오.

## Discovery와 CLI walkthrough

CLI는 다음 순서로 directory를 scan하며 먼저 나온 canonical path와 plugin name이
우선합니다.

1. 실행 중인 `neverd` executable 옆의 `plugins`.
2. `$HOME/.neverd/plugins`(`HOME`이 비어 있지 않으면 사용하며, Windows에서는 native profile directory를 fallback으로 사용).
3. `NEVERD_PLUGIN_PATH`의 비어 있지 않은 각 entry(기재 순서).
4. `--plugin-dir`로 지정한 directory.

`NEVERD_PLUGIN_PATH` entry 구분자는 Linux/macOS에서 `:`, Windows에서 `;`입니다.
canonical하게 같은 directory는 한 번만 scan합니다. 네이티브 라이브러리는 호스트
suffix만 대상으로 합니다. Linux는 `.so`, macOS는 `.dylib`, Windows는 `.dll`입니다.
Python 지원 build는 `.py` file도 scan합니다.

build tree 예제는 이미 executable과 나란한 directory에 있습니다.

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

multi-configuration build에서는 `build/bin`을 `build/bin/Release`로 바꾸십시오. 같은
플러그인이 옆에 없는 NeverD executable용 copy를 install하려면 호스트 platform에 맞는
command를 사용한 뒤 해당 executable을 같은 `--list`와 `--run` option으로 실행합니다.

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

선택적인 sibling/home directory가 없으면 무시합니다. 선택적 directory에 잘못된
플러그인이 있으면 warning을 출력하지만 scan을 계속합니다. `NEVERD_PLUGIN_PATH`와
`--plugin-dir`에 지정된 각 directory는 필수입니다. directory가 없거나 candidate가
거부되면 CLI가 0이 아닌 값으로 종료됩니다. canonical file 중복, plugin name 중복,
`neverd_plugin` export 누락 및 잘못된 descriptor type은 거부됩니다.
`neverd_plugins_load_file`을 직접 호출하면 지원하지 않는 suffix의 file도 거부됩니다.

embedding host는 plugin-management 결과와 `neverd_last_error(session)`을 모두 확인해야
합니다. `neverd_plugins_load_file`은 `1` 또는 `0`을 반환합니다.
`neverd_plugins_load_dir`은 로드된 수를 반환하며 부분적으로 성공한 뒤에도 거부된
candidate를 보고할 수 있습니다. `neverd_plugins_run`은 plugin 결과를 반환하고 plugin이
없거나 실행할 수 없을 때 `-1`을 사용합니다. C API가 반환한 모든 error 또는 JSON
string은 `neverd_free_string()`으로 해제하십시오.
