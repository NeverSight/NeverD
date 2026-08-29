**Языки**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← Оглавление документации](README.ru.md)

# Нативные плагины

Нативные плагины NeverD — доверенные разделяемые библиотеки, загружаемые в
процесс хоста. Они используют чистые объявления C из
`neverd/sdk/NeverDPlugin.h` и вызывают публичный API C из
`neverd/sdk/NeverDCAPI.h`. Если для разработки внутри процесса больше подходит
Python, используйте [руководство по плагинам Python](python-plugins.ru.md).

## Совместимость и граница доверия

В текущем дескрипторе `neverd_plugin_t` нет ни версии ABI, ни поля размера
структуры. Собирайте плагин с заголовками, размещёнными точно той же ревизией
NeverD, которая будет его загружать, и пересобирайте плагин при каждом
обновлении NeverD. Плагин и хост также должны использовать одинаковые ОС и
архитектуру, а также ABI-совместимые toolchains.

Нативные плагины выполняют произвольный код внутри процесса. NeverD не помещает
их в sandbox, не изолирует сбои и не ограничивает доступ к сессии или процессу
хоста. Загружайте только доверенные плагины.

## Дескриптор и callbacks

Каждая библиотека экспортирует ровно один символ данных с точным именем
`neverd_plugin`:

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

На Windows `NEVERD_PLUGIN_EXPORT` разворачивается в `__declspec(dllexport)`, а
на остальных платформах — в стандартную видимость ELF/Mach-O. Оставляйте
исходный код на C; если реализация на C++ неизбежна, явно задайте дескриптору
линковку C.

`Name` не может быть пустым и должно быть уникальным в пределах одного хоста.
При загрузке хост копирует все четыре строки метаданных. `Version`, `Author` и
`Description` могут быть пустыми. Четыре значения типа — только метки
классификации:

| Значение | Текущее значение |
|----------|------------------|
| `NEVERD_PLUGIN_GENERIC` | Метка расширения общего назначения |
| `NEVERD_PLUGIN_LOADER` | Метка loader; не регистрирует загрузчик бинарников |
| `NEVERD_PLUGIN_PROCESSOR` | Метка анализа/обработки; не планирует работу |
| `NEVERD_PLUGIN_UI` | Метка UI; NeverD не предоставляет GUI-хост нативных плагинов |

Все указатели callback необязательны. Вызовы выполняются напрямую и синхронно
в потоке вызывающей стороны хоста.

| Callback | Контракт |
|----------|----------|
| `Init(session)` | При успехе возвращает `0`. Ненулевой результат записывает ошибку; `Term` не вызывается после неудачной инициализации. |
| `Term()` | Вызывается при завершении только после успешного `Init`, после чего библиотека выгружается. |
| `Run(session, arg)` | Выполняет работу плагина и возвращает целое число. CLI передаёт `0`; встраивающий API C может выбрать другой аргумент. Отсутствующий callback возвращает `-1`. |
| `Event(event)` | Обрабатывает событие, переданное хостом. При успехе возвращает `0`; ненулевой результат записывает ошибку. Отсутствующий callback ничего не делает. |

Обычный порядок для встраивания: загрузка, инициализация, запуск или передача
событий, затем завершение. API C не навязывает этот порядок для `Run` или
`Event`, поэтому жизненным циклом управляет встраивающий хост.

## События передаёт хост

`neverd_event_t` содержит одно из шести значений событий:

| Событие | Payload в `event->Data` |
|---------|--------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | Без payload |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | Без payload |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

API сессии и командная строка `neverd` не генерируют события автоматически.
Встраивающий хост создаёт событие и передаёт его:

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

Событие и строки payload заимствованы до возврата из callback: не освобождайте
и не сохраняйте их. Дескриптор сессии принадлежит хосту: плагин не должен
уничтожать его или использовать после завершения хоста. Результаты, выделенные
API C NeverD, принадлежат NeverD и освобождаются через `neverd_free_string()`.
NeverD не гарантирует параллельный доступ к callbacks или сессии: встраивающий
хост должен сериализовать вызовы lifecycle, run и event, если сам не
предоставляет безопасную синхронизацию.

## Сборка встроенного примера

Примеры включаются явно; значением по умолчанию остаётся
`NEVERD_BUILD_PLUGINS=OFF`:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

Для одноконфигурационного generator соответствующие артефакты находятся здесь:

| Артефакт | Путь |
|----------|------|
| CLI | `build/bin/neverd` (`neverd.exe` на Windows) |
| Библиотека хоста | `build/bin/libneverd.so`, `build/bin/libneverd.dylib` или `build/bin/neverd.dll` |
| Размещённые заголовки | `build/bin/sdk/neverd/sdk/` |
| Пример | `build/bin/plugins/example_plugin.so`, `.dylib` или `.dll` |

Многоконфигурационная сборка помещает те же runtime-артефакты под выбранную
конфигурацию, например в `build/bin/Release/`; плагин будет в
`build/bin/Release/plugins/`, а заголовки — в `build/bin/Release/sdk/`:

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## Сборка отдельного плагина

Сейчас NeverD не устанавливает SDK и не предоставляет `NeverDConfig.cmake`,
поэтому поддерживаемого `find_package(NeverD)` нет. Передайте отдельной сборке
размещённые заголовки и библиотеку линковки точно от той же сборки хоста:

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

Настройте `NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk` (либо каталог `sdk`
конкретной конфигурации). В Linux/macOS `NEVERD_LINK_LIBRARY` указывает на
соответствующий `.so`/`.dylib`. В Windows это созданная generator библиотека
импорта `neverd.lib`, а соответствующая `neverd.dll` должна оставаться рядом с
исполняемым файлом хоста. Расположение библиотеки импорта зависит от generator,
поэтому передавайте фактический файл явно.

## Обнаружение и walkthrough CLI

CLI сканирует каталоги в следующем порядке; более ранние канонические пути и
имена плагинов имеют приоритет:

1. `plugins` рядом с запущенным исполняемым файлом `neverd`.
2. `$HOME/.neverd/plugins` (если `HOME` не пуст, используется он; в Windows запасным вариантом служит нативный каталог профиля).
3. Все непустые элементы `NEVERD_PLUGIN_PATH` по порядку.
4. Каталог из `--plugin-dir`.

В `NEVERD_PLUGIN_PATH` элементы разделяются `:` в Linux/macOS и `;` в Windows.
Канонически одинаковые каталоги сканируются один раз. Для нативных библиотек
учитывается только suffix хоста: `.so` в Linux, `.dylib` в macOS и `.dll` в
Windows. Сборки с поддержкой Python также сканируют файлы `.py`.

Пример из дерева сборки уже находится в соседнем с исполняемым файлом каталоге:

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

Для многоконфигурационной сборки замените `build/bin` на `build/bin/Release`.
Чтобы установить копию для исполняемого файла NeverD, рядом с которым ещё нет
этого плагина, используйте команду для платформы хоста, а затем запустите этот
файл с теми же параметрами `--list` и `--run`:

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

Отсутствующие необязательные каталоги sibling/home игнорируются. Некорректный
плагин в необязательном каталоге вызывает предупреждение, но сканирование
продолжается. Каждый каталог из `NEVERD_PLUGIN_PATH` и `--plugin-dir` обязателен:
отсутствующий каталог или отклонённый кандидат приводит к ненулевому коду
завершения CLI. Дубликаты канонических файлов и имён плагинов, отсутствие
экспорта `neverd_plugin` и недопустимые типы дескриптора отклоняются. Прямой
вызов `neverd_plugins_load_file` также отклоняет файл с неподдерживаемым suffix.

Встраивающие хосты должны проверять и результаты управления плагинами, и
`neverd_last_error(session)`. `neverd_plugins_load_file` возвращает `1` или
`0`; `neverd_plugins_load_dir` возвращает число загруженных плагинов и может
сообщить об отклонённых кандидатах даже при частичном успехе.
`neverd_plugins_run` возвращает результат плагина и использует `-1`, когда
плагин отсутствует или не может быть запущен. Каждую строку ошибки или JSON,
возвращённую API C, освобождайте через `neverd_free_string()`.
