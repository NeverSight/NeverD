**Языки**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](project.md) | [العربية](../ar/project.md)

<!-- i18n-source: d157cf302643e879be2748919ad949d2979c1d36798e295ee566f247834396f4 -->

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI-дружественный движок анализа и декомпиляции — 1:1 подъём на LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; SDK для C и Python

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#сборка)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-и-плагины)

[Документация](README.md) · [Android](android.md) · [iOS](ios.md) · [Дорожная карта](roadmap.md) · [Участие](CONTRIBUTING.md)

</div>

---

> GitHub всегда показывает английский `README.md` на главной странице репозитория. Используйте языковые ссылки выше для локализованных версий.

<!-- i18n-section: overview -->

## Обзор

NeverD — движок нативного и smart-contract анализа/декомпиляции с **1:1-поднятием инструкций**. Он загружает **PE**, **ELF**, **Mach-O**, legacy-байткод **EVM** и программы Solana **SBF ELF**. Нативные цели используют [Capstone](https://www.capstone-engine.org/); EVM и SBF имеют отдельные version-aware decoders и staged IR. Все пути используют рукописную семантику. Инструкции сохраняют поведение в **LLVM IR**, **C**, **Rust для SBF**, **Solidity-реконструкции для EVM** или **перезаписанном нативном бинарнике**.

Strict-режим **включён по умолчанию**. Инструкция без lifter’а бросает `UnliftedInstruction` — без пропуска, угадывания или тихого `NOP`.

CLI, интеграторы и ИИ-агенты используют один движок — **`libneverd`** — через **чистый C API**. Они не линкуют Capstone, LLVM или внутренний C++ напрямую.

Форматы входа, host-контракты и ограничения описаны в руководствах [EVM](evm.md) и [Solana SBF](sbf.md).

Экспериментальная команда `neverd mobile app.apk -o recovered-app` восстанавливает Java из APK (multidex), DEX и smali и создаёт `report.json`. Восстановление Android использует только встроенный движок NeverD на C++20 и не требует среды выполнения Python или Java. Пути с пробелами заключайте в кавычки. Поддерживаемые входы, отчёты и ограничения описаны в [руководстве Android](android.md).

Экспериментальный процесс iOS `neverd mobile App.ipa -o recovered-ios` экспортирует нативный C и поддерживаемые исходники Objective-C/Swift из IPA, `.app` или Mach-O. Раскладки runtime, единицы исходников и пропуски методов сохраняются явно; код не использует мост к оригинальному бинарнику. Настройка, покрытие и независимая компиляция описаны в [руководстве iOS](ios.md).

Экспериментальное [восстановление исходников интерпретатора](interpreter-recovery.md) использует `neverd decompile --devirtualize --func ENTRY`, чтобы специализировать поддерживаемые скомпонованные интерпретаторы x64 ELF/PE в HighC или LLVMC через общий конвейер LowIR/MedIR. Подсказки управления разделяют контексты декодера, не фиксируя входные данные времени выполнения. Неразрешённое управление, неподдерживаемая семантика и исчерпанные бюджеты вызывают явный отказ; режим не доказывает безопасность замены бинарного кода или эквивалентность исключений.

Бюджеты восстановления задаются явно: `--vm-max-fields`, `--vm-max-refinements` и `--vm-max-queries` сохраняют значения по умолчанию 16, 16 и 4096. Совместимый C API v3 и правила отказа описаны в руководстве.

Восстановление также поддерживает `--vm-chain-transfers=N` (по умолчанию 0) и `--vm-no-control-discovery`. Цепочки сохраняют символические связи между переходами с доказанной единственной целью; при достижении лимита возвращаются обычные границы CFG. В режиме машинного состояния `--vm-entry-frame=begin:end` объявляет не проверяемый при исполнении диапазон смещений от входного RSP без циклического переполнения. Точная числовая предпосылка сохраняется в C и отчёте, не разрешая доступ к памяти и не доказывая эквивалентность.

Восстановление с машинным состоянием принимает `--vm-entry-alignment=A:R` как явно заданную и проверяемую область начального RSP. `A` должно быть положительной степенью двойки, а `R < A`. Другие значения возвращают статус 2 до доступа к гостевой памяти или записи состояния. Старшие биты адреса свободны; по умолчанию выравнивание не предполагается. Эта опция не удостоверяет нативную эквивалентность.

`--vm-external-stores-disjoint-frame` добавляет явную предпосылку без проверки при исполнении: полный диапазон каждой внешней STORE не пересекает `--vm-entry-frame`. Сохраняются только уже известные факты внутри диапазона. LOAD и алиасы между внешними указателями не ограничиваются; поведение по умолчанию остаётся консервативным. API нативного доказательства отвергают этот домен.

Крупные восстановленные функции, превышающие лимит построения SSA, могут использовать `--llvm` с ограниченным контрактом изменяемого скалярного хранилища. Входы, переносимые между итерациями значения и результаты прежних чтений сохраняют смысл. Неподдерживаемое неявное состояние, параметры в векторных регистрах, релокации образа, неоднозначное хранилище и некорректный поток управления вызывают явную ошибку; HighC отклоняет этот запасной путь. Вывод следует прежнему контракту машинного состояния и не добавляет сертификат эквивалентности.

Отдельный C++ API доказательства циклов выводит ограниченные инварианты и лексикографические ранги для вложенных циклов, затем повторно проверяет уточнение нативного кода до LowIR. См. [руководство по восстановлению](interpreter-recovery.md); выдаваемый C не сертифицируется.

Отдельный C++ API `checkBinaryLLVMRefinement` объединяет новые нативные и LLVM-проверки точного артефакта LLVM; компиляция C остаётся вне области доказательства.

Восстановление PE также проверяет байты DIR64 по предпочтительному базовому адресу и исключает записи импорта; контракт не подтверждает эквивалентность ASLR или инициализации.

При явном контракте состояния машины восстановление поддерживает ограниченные разбиения по выравниванию входного стека и внутреннюю очистку `RET imm16`. Автоматическая композиция доказательств native-to-LLVM для этих разбиений ещё не реализована.

Ограниченное восстановление `REP MOVS/STOS` сохраняет порядок элементов и поведение при перекрытии; доказательство исходных инструкций ещё не реализовано.

<!-- i18n-section: why-neverd -->

## Почему NeverD?

- **Семантика 1:1** — рукописные lifter’ы; неподдерживаемые опкоды бросают исключение в strict по умолчанию
- **Дружественный к LLM** — структурированный C, LLVM IR и JSON-анализ через чистый C API с детерминированными ошибками
- **Один конвейер, несколько выходов** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → перезаписанный нативный бинарник
- **Перезапись бинарников** — PE / ELF / Mach-O, section-трамплины или inplace
- **Набор средств анализа** — CLI, отладочная информация, сигнатуры, плагины и опциональные обфускационные проходы

<!-- i18n-section: supported-targets -->

## Поддерживаемые цели

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Каждая ячейка матрицы реализована, но глубина интеграционного тестирования различается. См. [матрицу покрытия архитектуры](architecture.md#support-and-test-depth). Mach-O i386 использует релокируемые `thin`-объекты, поскольку современная macOS не может линковать устаревшие исполняемые файлы i386.

Legacy-байткод EVM поддерживается независимо от нативных контейнеров: все 150
назначенных opcodes от Frontier до Fusaka проходят через Low/Med/High IR,
проверенный LLVM `i256`, C23 `_BitInt(256)` и Solidity. См.
[декомпиляцию EVM](evm.md).

Программы Solana SBF v0-v4 ELF используют отдельный strict loader, полные
версионированные metadata ISA, Low/Med/High IR, проверенный LLVM, переносимый
C11 и безопасный стабильный Rust. См. [декомпиляцию Solana SBF](sbf.md).

<!-- i18n-section: mobile-source-recovery -->

### Восстановление мобильных исходников

Экспериментальная команда `neverd mobile` поддерживает следующие мобильные входы и форматы исходников.

| Платформа | Вход | Выход |
|-----------|------|-------|
| [Android](android.md) | APK (включая multidex), DEX, файл или каталог smali | Java и отчёт JSON |
| [iOS](ios.md) | IPA, `.app`, Mach-O (arm64 / x86_64) | Нативный C, поддерживаемые исходники Objective-C / Swift и отчёт о покрытии JSON |

Восстановление зависит от поддерживаемых шаблонов кода; охват и ограничения описаны в [обзоре мобильных платформ (английский)](../mobile.md) и руководствах платформ.

<!-- i18n-section: cpu-workloads -->

### Выполнение CPU и гостевые среды

Выполнение CPU разделяет допуск ISA, гостевую память, транспорт бэкенда и политику гостевой ОС. `NEVERD_ENABLE_CPU_EMULATION` включает слой CPU x64/ARM64; `NEVERD_ENABLE_DRIVER_EMULATION` добавляет ограниченную среду Windows WDM/KMDF x64. `linux-elf64-v1` выполняет поддерживаемые процессы Linux ELF. См. [Выполнение CPU](cpu-execution.md), [Эмуляция гостевых процессов](process-emulation.md) и [Эмуляция драйверов Windows](driver-emulation.md).

`windows-pe64-v1` поддерживает ограниченные консольные процессы Windows x64/ARM64 с PEB/TEB, TLS EXE, именованными Win32 API и явными ациклическими графами стартовых DLL. DLL поддерживают импорт кода/данных по имени или ординалу, DIR64-перебазирование и реальные записи загрузчика. Вход/TLS DLL, динамическая загрузка, перенаправленные экспорты, CRT/GUI, пользовательский SEH и потоки ещё не завершены; нативных доказательств ARM64 KVM/WHP пока нет.

Виртуальная память Windows поддерживает `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` и `FlushInstructionCache` для текущего процесса. Уровень ОС управляет резервированием; `AddressSpace` остаётся единственным владельцем отображений подтверждённых страниц, прав доступа и физической памяти. Тесты проверяют изменение кода, ошибки доступа и повторное использование лимита памяти.

`driver-strict` / `checked-x64-v1` поддерживает KVM на совместимых хостах Linux x64 и WHP на Windows x64; `auto` выбирает этот нативный транспорт, а другая ISA использует Unicorn. Явный Unicorn и прежний API V1 сохраняют переносимый программный профиль. Нативное выполнение проверяет канонические адреса и эффекты до входа; недоступное оборудование вызывает ошибку без подмены. Неподдерживаемые инструкции и поведение OS завершаются явной ошибкой. Нативная CI для Windows x64 с отключённым Unicorn проходит все 359 обязательные проверки: 131 проверок CPU, 224 результата драйверов из 26 встроенных образов, 46 образов WDK и 40 сценарных вариантов по предпочтительным и перемещённым адресам, а также четыре проверки границ SEH ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Нативные свидетельства ARM64 ещё отсутствуют; совместимость произвольных драйверов или Android/Darwin не установлена.

Проверяемый x64 поддерживает `MOVS/STOS/LODS` для обычной RAM и `CLD/STD`, с поэлементным возобновлением, отменой и проверкой страниц. Зависящие от CPU старшие биты при нулевом счётчике и обращения STOS/LODS к устройствам остаются вне контракта.

Проверяемый x64 также поддерживает `CMPS/SCAS` для обычной RAM с `REPE/REPNE`, арифметическими флагами, досрочным завершением, остановкой по элементам и восстановлением после сбоя. Сравнения с устройствами исключены.

`checked-aarch64-v1` и `checked-user-aarch64-v1` предоставляют ограниченные ARM64 FP32/FP64, SIMD фиксированной ширины и полный FPCR/FPSR/векторный контекст. Совместимые Linux ARM64 используют KVM, Windows ARM64 — WHP, другие ISA — Unicorn. Нативные свидетельства ARM64 ещё требуются; драйверы Windows загружаются только для x64.

Нативные стартовые проверки x64 и ARM64 подтверждают ограниченное полное исполнение состояния с исключительным правом на память. XSAVE-пакеты и кеши таблиц с идентичностью ISA имеют единого владельца; доказательства нативных нагрузок ARM64 ещё неполны.

Нативные поля x64 `FOP/FIP/FDP` следуют правилам сохранения/восстановления хоста: AMD может обнулять неактивные метаданные x87. Стартовые проверки проверяют их с ожидающим немаскированным исключением.

<!-- i18n-section: how-it-works -->

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

<!-- i18n-section: quick-start -->

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

# Восстановление исходников Android / iOS (экспериментально)
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# Анализ
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

Сигнатурные библиотеки устанавливаются в `build/bin/signatures/` при сборке. `sigs --auto` выбирает набор по формату, архитектуре и разрядности. Если заголовок Rich PE-файла указывает выпуск Visual Studio его компоновщика, загружается только `vs<year>.pat` этого выпуска и файлы, не относящиеся ни к одному выпуску. `--sig-base <dir>` выбирает так же из другого дерева сигнатур. Файл шаблонов размером от 1 МиБ разбирается один раз: его модули сохраняются в `neverd/signatures` в каталоге кэша пользователя и при последующих загрузках отображаются в память. `NEVERD_SIGNATURE_CACHE` задаёт другой каталог, а значение `off` отключает кэш.

<!-- i18n-section: building -->

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r3
cmake --build build
```

Обычная CI NeverD на push и pull request намеренно собирает submodule LLVM из исходников. При ручном запуске workflow `CI` выберите `use_prebuilt_llvm`, чтобы проверить опубликованные пакеты; готовый LLVM включается только вручную выбранным `true`. Без этого остаётся тот же путь сборки из исходников, что и в автоматической CI.

Опубликованный пакет выбирается по хосту, на котором работает CMake:

| Хост | Артефакт релиза |
|------|-----------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Каждый архив перед распаковкой в `~/.cache/neverd-llvm/<tag>/<arch>/` или `NEVERD_LLVM_PREBUILT_CACHE_DIR` проверяется по дайджесту из `cmake/NeverDLLVMPrebuilt.cmake`. Для тегов вне зафиксированных значений используется опубликованный `.sha256`. Для версии по умолчанию `BUILDINFO.txt` должен также указывать точный коммит подмодуля LLVM. Релизные сборки используют ccache в macOS/Linux и sccache с кешем GitHub Actions для Windows clang-cl. Кеши лишь ускоряют повторные сборки и не публикуются как артефакты релиза.

По умолчанию используется ревизия пакета `neverd-llvm-v23.0.0-r3`. Git-тег, цель релиза, коммит исходников и дайджесты трёх архивов образуют неизменяемую версионную привязку. Каталоги сборки со старым базовым тегом, `neverd-llvm-v23.0.0-r1` или `neverd-llvm-v23.0.0-r2` автоматически переходят на `r3`, если явно не задан `NEVERD_LLVM_PREBUILT_SHA256`. `Prebuilt LLVM Audit` запускается при push, pull request и каждые шесть часов. Он вызывает `scripts/audit_prebuilt_llvm_release.py`, сопоставляя привязку с текущим релизом GitHub и каждым опубликованным файлом контрольной суммы.

Если fork LLVM изменился, но LLVM по-прежнему сообщает `23.0.0`, выпускайте следующую ревизию пакета — `neverd-llvm-v23.0.0-r4`, затем `-r5`. Не перезаписывайте существующий релиз и не придумывайте версию LLVM `23.0.1`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r4 \
  -f overwrite_existing_assets=false
```

После успешного workflow одновременно обновите тег по умолчанию, коммит и три дайджеста в `cmake/NeverDLLVMPrebuilt.cmake`. Новый пакет кешируется в `.cache/neverd-llvm/<tag>`; устаревший или повторно опубликованный архив отклоняется до распаковки. `overwrite_existing_assets` предназначен только для исторического восстановления и в обычном процессе выключен.

**Артефакты**

| Путь | Описание |
|------|----------|
| `build/bin/neverd` | Единый CLI |
| `build/bin/neverd-bench` | Бенчмарки (JSON) |
| `build/bin/neverd-sigmaker` | Генератор `.pat` из статических библиотек |
| `build/bin/libneverd.*` | Разделяемая библиотека движка |
| `build/bin/sdk/` | Канонический корень include для C SDK; используйте `<neverd/sdk/NeverDCAPI.h>` или `<neverd/sdk/NeverDPlugin.h>` с сохранённой иерархией `neverd/sdk/` |
| `build/bin/sdk/python/` | Типизированный пакет плагинов Python и примеры |
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

Целевые сборочные цели, метки CTest, требования к fixtures и межформатная матрица перезаписи описаны в разделе [Тестирование NeverD](testing.md).

**Опции CMake**

| Опция | По умолчанию | Описание |
|-------|--------------|----------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | Готовый LLVM для CI |
| `NEVERD_BUILD_SHARED` | `ON` | Собрать `libneverd` |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | Встроенная поддержка плагинов CPython 3.10+ |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Пример плагинов |
| `BUILD_TESTING` | `OFF` | Юнит-тесты |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | Группа семантических тестов с зависимостью от Unicorn (при `BUILD_TESTING=ON`) |

</details>

<!-- i18n-section: desktop-workbench -->

## Настольная рабочая среда

Необязательная [среда Qt Quick (английский)](../gui.md) предоставляет закрепляемые представления инструкций, CFG, шестнадцатеричных данных, C и IR, все 11 языков интерфейса, сохранённые аннотации и подключения MCP. Анализ выполняется в отдельном процессе без Qt; сборки только с CLI остаются независимыми. Поддерживаемые сценарии и необходимые до выпуска проверки платформ перечислены в [протоколе квалификации (английский)](../gui-qualification.md).

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### Конвейер

| Команда | Вывод | Описание |
|---------|-------|----------|
| `lift` | `.ll` | Подъём в LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | C, EVM Solidity или SBF Rust через `--language` |
| `decompile -llvm` | `.c` | Через LLVM IR + оптимизатор |
| `decompile --devirtualize` | `.c` + необязательный JSON | Экспериментальное восстановление интерпретаторов x64; нужен `--func`; [контракт и примеры](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | Экспериментально: [Android](android.md), [iOS](ios.md) |
| `patch` | бинарник | Перезапись машинного кода |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

Если 32-битный ARM-бинарник не содержит метаданных режима ARM/Thumb функции, задайте режим входа перед декомпиляцией:

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

Повторите параметр для других неоднозначных входов, при необходимости используя `:arm`. Указания, противоречащие проверенным метаданным, приводят к отказу загрузки; допустимые указания действуют только на точный вход. C API предоставляет такую же настройку до загрузки через `neverd_session_set_arm_function_mode()`.

<details>
<summary><strong>Команды анализа</strong></summary>

<br>

| Команда | Назначение |
|---------|------------|
| `info` / `dashboard` / `headers` | Метаданные и обзор |
| `funcs` | Найденные функции |
| `disasm` | Дизассемблирование (`--func` имя или hex) |
| `sym-explore` | Ограниченное исследование путей нативного LowIR (`--func`; вывод JSON) |
| `audit` | Ошибки времени жизни объектов кучи и чтение неинициализированного локального стека (JSON) |
| `hunt` | Выход за границы опасных копирований с символьными свидетелями и дополнительными доказательствами воспроизведения `process-input-v1` при наличии полного плана (схема JSON v1) |
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

<!-- i18n-section: sdk-and-plugins -->

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
[примеры API C для EVM](evm.md#c-api).

Нативные разделяемые библиотеки и файлы Python `.py` используют один жизненный
цикл плагинов. Нативный пример собирается с `-DNEVERD_BUILD_PLUGINS=ON`;
[руководство по нативным плагинам](plugins.md) описывает чистый дескриптор
C, callbacks, сборку и линковку, обнаружение, работу CLI и ограничения ABI.
Поддержка Python включена по умолчанию и полностью удаляется параметром
`-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`; typed SDK и package workflow описаны в
[руководстве по плагинам Python](python-plugins.md). Оба вида используют
`<neverd-dir>/plugins`, `~/.neverd/plugins` и `$NEVERD_PLUGIN_PATH`.

<!-- i18n-section: dependencies -->

## Зависимости

| Компонент | Роль | Источник |
|-----------|------|----------|
| **LLVM** (fork) | IR, оптимизация, codegen, диагностика | `third_party/llvm-project` или готовый |
| **Capstone** | Декодирование | `third_party/capstone` |

Сторонние компоненты сохраняют свои лицензии.

<!-- i18n-section: contributing -->

## Участие

Изменения интегрируются в ветку **`dev`**. Настройка окружения, инструкции Release/Debug, стиль, целевые тесты и требования к pull request описаны в [руководстве для участников](CONTRIBUTING.md). Руководства по [архитектуре](architecture.md) и [тестированию](testing.md) сопоставляют типовые изменения с соответствующим кодом и наборами проверок.

<!-- i18n-section: license -->

## Лицензия

[GNU AGPL только версии 3](../../LICENSE). При распространении охватываемого кода NeverD или его адаптаций сохраняйте уведомления об авторских правах, лицензии и отсутствии гарантий, включая атрибуцию проекта и источник из [NOTICE](../../NOTICE). Это относится и к повторному использованию с помощью ИИ/LLM, и к преобразованиям кода на базе LLVM.

Требования, область применения и примеры приведены в разделе [Атрибуция и цитирование](ATTRIBUTION.md). Для прослеживаемости рекомендуем указывать исходный файл и точную версию или коммит. [CITATION.cff](../../CITATION.cff) содержит метаданные цитирования ПО; цитирование само по себе не заменяет соблюдение лицензии.

Компоненты LLVM сохраняют лицензию Apache-2.0 WITH LLVM-exception. Capstone сохраняет свою лицензию.
