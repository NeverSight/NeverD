**Языки**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI-дружественный движок анализа и декомпиляции — 1:1 подъём на LLVM**

PE · ELF · Mach-O · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · SBF &nbsp;|&nbsp; Чистый C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#сборка)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O%20%7C%20SBF-informational.svg)](#поддерживаемые-цели)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM%20%7C%20SBF-orange.svg)](#поддерживаемые-цели)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk-и-плагины)

[Документация](../README.ru.md) · [Дорожная карта](../roadmap/README.ru.md) · [Участие](CONTRIBUTING.ru.md)

</div>

---

> GitHub всегда показывает английский `README.md` на главной странице репозитория. Используйте языковые ссылки выше для локализованных версий.

## Обзор

NeverD — движок анализа и декомпиляции нативных и smart-contract бинарников вокруг **1:1-поднятия на уровне инструкций**. Он загружает **PE**, **ELF**, **Mach-O** и программы Solana **SBF ELF**. Нативные цели декодируются через [Capstone](https://www.capstone-engine.org/); SBF использует отдельный учитывающий версию decoder и поэтапный IR. Все пути используют рукописную семантику, а не приближённый перевод. Поддерживаемые инструкции сохраняют поведение в **LLVM IR**, **структурированном C**, **безопасном стабильном Rust для SBF** или в **перезаписанном нативном бинарнике**.

Strict-режим **включён по умолчанию**. Инструкция без lifter’а бросает `UnliftedInstruction` — без пропуска, угадывания или тихого `NOP`.

CLI, интеграторы и ИИ-агенты используют один движок — **`libneverd`** — через **чистый C API**. Они не линкуют Capstone, LLVM или внутренний C++ напрямую.

Декомпиляция Solana SBF уже доступна; см. [руководство SBF](../sbf.ru.md). Другие цели и усиление отслеживаются в [дорожной карте](../roadmap/README.ru.md).

## Почему NeverD?

- **Семантика 1:1** — рукописные lifter’ы; неподдерживаемые опкоды бросают исключение в strict по умолчанию
- **Дружественный к LLM** — структурированный C, LLVM IR и JSON-анализ через чистый C API с детерминированными ошибками
- **Один конвейер, несколько выходов** — `lift` → LLVM IR · `decompile` → C/Rust · `patch` → перезаписанный нативный бинарник
- **Перезапись бинарников** — PE / ELF / Mach-O, section-трамплины или inplace
- **Набор средств анализа** — CLI, отладочная информация, сигнатуры, плагины и опциональные обфускационные проходы

## Поддерживаемые цели

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Каждая ячейка матрицы реализована, но глубина интеграционного тестирования различается. См. [матрицу покрытия архитектуры](../architecture.ru.md#support-and-test-depth). Mach-O i386 использует релокируемые `thin`-объекты, поскольку современная macOS не может линковать устаревшие исполняемые файлы i386.

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

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Анализ
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
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

**Артефакты**

| Путь | Описание |
|------|----------|
| `build/bin/neverd` | Единый CLI |
| `build/bin/neverd-bench` | Бенчмарки (JSON) |
| `build/bin/neverd-sigmaker` | Генератор `.pat` из статических библиотек |
| `build/bin/libneverd.*` | Разделяемая библиотека движка |
| `build/bin/sdk/` | `NeverDCAPI.h`, `NeverDPlugin.h` |
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
| `decompile` | `.c` / `.rs` | C или SBF Rust через `--language` |
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

Соберите пример плагина с `-DNEVERD_BUILD_PLUGINS=ON`. Пути загрузки: `<neverd-dir>/plugins`, `~/.neverd/plugins`, `$NEVERD_PLUGIN_PATH`.

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
