**Языки**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI-дружественный движок анализа и декомпиляции — 1:1 подъём на LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; Чистый C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#сборка)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-orange.svg)](#sdk-и-плагины)

[Документация](../README.ru.md) · [Дорожная карта](../roadmap/README.ru.md) · [Участие](CONTRIBUTING.ru.md)

</div>

---

> GitHub всегда показывает английский `README.md` на главной странице репозитория. Используйте языковые ссылки выше для локализованных версий.

## Обзор

NeverD — движок нативного и smart-contract анализа/декомпиляции с **1:1-поднятием инструкций**. Он загружает **PE**, **ELF**, **Mach-O**, legacy-байткод **EVM** и программы Solana **SBF ELF**. Нативные цели используют [Capstone](https://www.capstone-engine.org/); EVM и SBF имеют отдельные version-aware decoders и staged IR. Все пути используют рукописную семантику. Инструкции сохраняют поведение в **LLVM IR**, **C**, **Rust для SBF**, **Solidity-реконструкции для EVM** или **перезаписанном нативном бинарнике**.

Strict-режим **включён по умолчанию**. Инструкция без lifter’а бросает `UnliftedInstruction` — без пропуска, угадывания или тихого `NOP`.

CLI, интеграторы и ИИ-агенты используют один движок — **`libneverd`** — через **чистый C API**. Они не линкуют Capstone, LLVM или внутренний C++ напрямую.

Форматы входа, host-контракты и ограничения описаны в руководствах [EVM](../evm.ru.md) и [Solana SBF](../sbf.ru.md).

## Почему NeverD?

- **Семантика 1:1** — рукописные lifter’ы; неподдерживаемые опкоды бросают исключение в strict по умолчанию
- **Дружественный к LLM** — структурированный C, LLVM IR и JSON-анализ через чистый C API с детерминированными ошибками
- **Один конвейер, несколько выходов** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → перезаписанный нативный бинарник
- **Перезапись бинарников** — PE / ELF / Mach-O, section-трамплины или inplace
- **Набор средств анализа** — CLI, отладочная информация, сигнатуры, плагины и опциональные обфускационные проходы

## Поддерживаемые цели

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Каждая ячейка матрицы реализована, но глубина интеграционного тестирования различается. См. [матрицу покрытия архитектуры](../architecture.ru.md#support-and-test-depth). Mach-O i386 использует релокируемые `thin`-объекты, поскольку современная macOS не может линковать устаревшие исполняемые файлы i386.

Legacy-байткод EVM поддерживается независимо от нативных контейнеров: все 150
назначенных opcodes от Frontier до Fusaka проходят через Low/Med/High IR,
проверенный LLVM `i256`, C23 `_BitInt(256)` и Solidity. См.
[декомпиляцию EVM](../evm.ru.md).

Программы Solana SBF v0-v4 ELF используют отдельный strict loader, полные
версионированные metadata ISA, Low/Med/High IR, проверенный LLVM, переносимый
C11 и безопасный стабильный Rust. См. [декомпиляцию Solana SBF](../sbf.ru.md).

## Как это работает

```text
Binary (PE / ELF / Mach-O)
  → Loader + DebugInfo
  → Capstone decode
  → LowIR     architecture-neutral NdOps · CFG
  → MedIR     types · ABI · calls · memory · SSA
       │
       ├─ lift        MedIR → LLVM IR
       ├─ decompile   MedIR → HighIR → C
       │              MedIR → LLVM IR → opt → C   (-llvm)
       └─ patch       MedIR → LLVM IR → codegen → binary

EVM (raw / hex / compiler artifact)
  → нормализация runtime + hardfork-aware decode
  → EVM LowIR → EVM stack-SSA MedIR → восстановленный EVM HighIR
       ├─ lift        → проверенный LLVM i256/i512
       └─ decompile   → C23 _BitInt(256) или Solidity-реконструкция

Solana SBF ELF (v0-v4)
  → учитывающий версию legacy/strict loader + verifier
  → SBF LowIR → нормализованный MedIR → восстановленный SBF HighIR
       ├─ lift        → проверенный LLVM i64 runtime ABI
       └─ decompile   → переносимый C11 или безопасный стабильный Rust
```

| Ступень | Роль |
|---------|------|
| **LowIR** | ~77 опкодов `NdOp` + CFG |
| **MedIR** | Типы, соглашения о вызовах, модель памяти, SSA |
| **HighIR** | Структурированный control flow (`if` / `while` / `for`) |
| **LLVM** | Оптимизация, вывод C или генерация машинного кода |

## Быстрый старт

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Конвейер
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Анализ
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

Сигнатурные библиотеки устанавливаются в `build/bin/signatures/` при сборке. `sigs --auto` выбирает набор по формату, архитектуре и разрядности.

## Сборка

**Требования:** CMake ≥ 3.20 · Ninja · компилятор C++20 · Git submodule (LLVM fork + Capstone)

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Первая конфигурация собирает LLVM fork локально (часто 30–60 минут). Последующие сборки инкрементальны. Пресеты: `CMakePresets.json` → `release` / `relwithdebinfo` / `debug`.

<details>
<summary><strong>Готовый LLVM · артефакты · тесты · опции CMake</strong></summary>

<br>

**Готовый LLVM**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

Обычная CI NeverD на push и pull request намеренно собирает submodule LLVM из исходников. При ручном запуске workflow `CI` выберите `use_prebuilt_llvm`, чтобы проверить опубликованные пакеты; готовый LLVM включается только вручную выбранным `true`. Без этого остаётся тот же путь сборки из исходников, что и в автоматической CI.

Опубликованный пакет выбирается по хосту, на котором работает CMake:

| Хост | Артефакт релиза |
|------|-----------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Каждый архив сверяется с дайджестом, закреплённым в `cmake/NeverDLLVMPrebuilt.cmake`, — или с опубликованным рядом `.sha256`, если тег этими пинами не описан, — прежде чем распаковаться в `~/.cache/neverd-llvm/<tag>/<arch>/` (или по пути из `NEVERD_LLVM_PREBUILT_CACHE_DIR`). Релизная сборка использует ccache на macOS и Linux; сборки clang-cl на Windows используют sccache с кэшем GitHub Actions в качестве backend. Кэши компилятора лишь ускоряют пересборку и никогда не публикуются как артефакты релиза.

Тег релиза версионирует пакет NeverD, а `BUILDINFO.txt` фиксирует точный commit форка LLVM. Если LLVM по-прежнему сообщает `23.0.0`, но исходники форка изменились, обычный неизменяемый выбор — ревизия пакета вроде `neverd-llvm-v23.0.0-r1` (затем `-r2`), а не `23.0.1`, если только не изменилась собственная patch-версия LLVM. Направьте `NEVERD_LLVM_PREBUILT_TAG` на эту новую ревизию.

Чтобы починить существующий изменяемый релиз `neverd-llvm-v23.0.0` на месте, запустите workflow `NeverD LLVM Release` из ветки `main` репозитория llvm-project и включите `overwrite_existing_assets`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0 \
  -f overwrite_existing_assets=true
```

Это заменяет одноимённые артефакты, но намеренно не передвигает существующий тег Git. Тем же изменением обновите дайджесты, закреплённые в `cmake/NeverDLLVMPrebuilt.cmake`: именно они, а не тег, называют сборку, которую ожидает ревизия NeverD, поэтому устаревший `~/.cache/neverd-llvm/neverd-llvm-v23.0.0/` заменяется при следующей конфигурации, а архив, не совпадающий ни с одним закреплённым дайджестом, останавливает эту конфигурацию расхождением контрольной суммы — вместо того чтобы всплыть позже отсутствующим заголовком, которого в старом пакете не было. Новый тег `-rN` полностью избавляет от перезаписи на месте. Workflow отклоняет случайную замену, пока флажок не установлен, и отклоняет её полностью, если GitHub пометил релиз как неизменяемый.

**Артефакты**

| Путь | Описание |
|------|----------|
| `build/bin/neverd` | Единый CLI |
| `build/bin/neverd-bench` | Бенчмарки (JSON) |
| `build/bin/neverd-sigmaker` | Генератор `.pat` из статических библиотек |
| `build/bin/libneverd.*` | Разделяемая библиотека движка |
| `build/bin/sdk/` | Канонический корень include для C SDK; используйте `<neverd/sdk/NeverDCAPI.h>` или `<neverd/sdk/NeverDPlugin.h>` с сохранённой иерархией `neverd/sdk/` |
| `build/bin/signatures/` | Встроенные сигнатурные библиотеки |

**Тесты**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| Цель | Описание |
|------|----------|
| `check-neverd` | Все тесты |
| `check-neverd-semantic` | Только семантический roundtrip (Unicorn) |

Целевые сборочные цели, метки CTest, требования к fixtures и межформатная матрица перезаписи описаны в разделе [Тестирование NeverD](../testing.ru.md).

**Опции CMake**

| Опция | По умолчанию | Описание |
|-------|--------------|----------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | Готовый LLVM для CI |
| `NEVERD_BUILD_SHARED` | `ON` | Собрать `libneverd` |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Пример плагинов |
| `BUILD_TESTING` | `OFF` | Юнит-тесты |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### Конвейер

| Команда | Вывод | Описание |
|---------|-------|----------|
| `lift` | `.ll` | Подъём в LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | C, EVM Solidity или SBF Rust через `--language` |
| `decompile -llvm` | `.c` | Через LLVM IR + оптимизатор |
| `patch` | бинарник | Перезапись машинного кода |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>Команды анализа</strong></summary>

<br>

| Команда | Назначение |
|---------|------------|
| `info` / `dashboard` / `headers` | Метаданные и обзор |
| `funcs` | Найденные функции |
| `disasm` | Дизассемблирование (`--func` имя или hex) |
| `sym-explore` | Ограниченное исследование путей нативного LowIR (`--func`; вывод JSON) |
| `audit` | Дефекты жизни кучи: утечка, двойное освобождение, использование после освобождения (JSON) |
| `hunt` | Переполнения опасных копий с символьным доказательством и значениями-кандидатами (JSON) |
| `hex` | Hex dump по адресу |
| `cfg` / `callgraph` | CFG / граф вызовов (JSON; опционально DOT/SVG) |
| `xrefs` | Перекрёстные ссылки |
| `strings` / `search` | Строки / поиск байт или текста |
| `imports` / `exports` / `symbols` / `relocs` | Таблицы |
| `segments` / `sections` / `entrypoints` | Раскладка |
| `diff` | Сравнение двух бинарников (`-a` / `-b`) |
| `sigs` | Сигнатуры (`--auto`) |
| `rename` / `annotate` / `bookmarks` | Разметка сессии |
| `export` | Экспорт результатов |
| `plugins` | Список или запуск плагинов |

Большинство команд анализа принимают `--json`.

</details>

## SDK и плагины

Интеграторы используют **чистый C API** из `libneverd`:

| Заголовок | Роль |
|-----------|------|
| `NeverDCAPI.h` | Сессия, подъём, декомпиляция, patch, IR / CFG, аннотации |
| `NeverDPlugin.h` | ABI плагинов как динамических библиотек |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

Для EVM `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)` явно выбирает
Solidity; `neverd_decompile_all` по-прежнему выводит C. См.
[примеры API C для EVM](../evm.ru.md#api-c).

Нативные разделяемые библиотеки и файлы Python `.py` используют один жизненный
цикл плагинов. Нативный пример собирается с `-DNEVERD_BUILD_PLUGINS=ON`;
[руководство по нативным плагинам](../plugins.ru.md) описывает чистый дескриптор
C, callbacks, сборку и линковку, обнаружение, работу CLI и ограничения ABI.
Поддержка Python включена по умолчанию и полностью удаляется параметром
`-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`; typed SDK и package workflow описаны в
[руководстве по плагинам Python](../python-plugins.ru.md). Оба вида используют
`<neverd-dir>/plugins`, `~/.neverd/plugins` и `$NEVERD_PLUGIN_PATH`.

## Зависимости

| Компонент | Роль | Источник |
|-----------|------|----------|
| **LLVM** (fork) | IR, оптимизация, codegen, диагностика | `third_party/llvm-project` или готовый |
| **Capstone** | Декодирование | `third_party/capstone` |

Сторонние компоненты сохраняют свои лицензии.

## Участие

Изменения интегрируются в ветку **`dev`**. Настройка окружения, инструкции Release/Debug, стиль, целевые тесты и требования к pull request описаны в [руководстве для участников](CONTRIBUTING.ru.md). Руководства по [архитектуре](../architecture.ru.md) и [тестированию](../testing.ru.md) сопоставляют типовые изменения с соответствующим кодом и наборами проверок.

## Лицензия

[AGPL-3.0](../../LICENSE)

Компоненты LLVM сохраняют лицензию Apache-2.0 WITH LLVM-exception. Capstone сохраняет свою лицензию.
