**Языки**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← Индекс документации](README.ru.md)

# Тестирование NeverD

Тесты NeverD отвечают на три разных вопроса: имеет ли представление ожидаемую
форму, работает ли полный маршрут с бинарным fixture и сохраняет ли
сгенерированный код поведение. Выберите минимальный набор, отвечающий на вопрос
изменения, а перед рискованным pull request запустите более широкий агрегат.

## Настройка тестовой сборки

Тесты отключены, если не включён `BUILD_TESTING`. Для полного набора обычно
выбирается Release; Debug сохраняет assertions и пошаговое выполнение, но
намеренно не оптимизирован и не отражает производительность декодирования.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

Полный набор fixture требует `clang` для кросс-компиляции и LLVM linker
(`ld.lld` и `lld-link`) в `PATH`. CMake всегда создаёт множество перемещаемых
fixture и слинкованные ELF/PE fixture при наличии соответствующего linker.
Тест, пропущенный из-за невозможности собрать или слинковать fixture на хосте,
означает неисполненное покрытие, а не успешную проверку цели.

Клонирование, профили сборки и готовый LLVM для macOS описаны в
[CONTRIBUTING.md](i18n/CONTRIBUTING.ru.md).

## Структура тестов

`add_neverd_unittest` создаёт один исполняемый GoogleTest и назначает каждому
обнаруженному случаю метку CTest с именем этой исполняемой цели.

| Область исходников | Цель и метка CTest | Покрытие |
|--------------------|--------------------|----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Кросс-платформенные дочерние процессы, quoting, перенаправления и коды выхода |
| `unittests/libc` | `NeverDLibCTests` | Известные имена libc и классификация |
| `unittests/lift` | `NeverDLiftTests` | Формы LowIR decoder/lifter, стадии IR, loader, relocation, fixture форматов, декомпиляция и представительные patch-маршруты |
| Большинство файлов `unittests/semantic` | `NeverDSemanticTests` | Дифференциальная семантика инструкций, ABI, управления, выражений C и lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMAnalyzerTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadata hardfork, нормализация входа, CFG/SSA/recovery, семантика interpreter, differential execution LLVM/C/Solidity и публичный API |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFSemanticTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFEmitterTests`, `NeverDSBFIntegrationTests` | Метаданные v0-v4 и компоновки ELF, строгая верификация, CFG/восстановление, независимое исполнение raw-кода, проверка LLVM, компиляция C/Rust и маршрутизация публичного API |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Эквивалентность переписывания/обфускации для четырёх ISA и трёх объектных форматов |
| Целевые файлы преобразований в `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Быстро перелинковываемые проверки отдельно от большого семантического бинарника |
| `unittests/corpus` (подмодуль) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests` | Метаданные исключений и рантайма, прочитанные из 305 зафиксированных настоящих бинарников; для каждого манифест объявляет нижние границы, которые восстановление обязано преодолеть |

Источники регистрации:
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) и
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) и
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt).

### Зафиксированный бинарный corpus

Каждый другой набор сам собирает то, что проверяет. Corpus — нет: это подмодуль
с бинарниками, которые выпустили настоящие тулчейны, на хостах и для целей,
недоступных этому репозиторию; каждый зафиксирован по дайджесту, а рядом лежит
манифест с нижними границами, которые обязано преодолеть восстановление. Это
единственное место, где утверждение о том, что NeverD читает, скажем, из
обрезанного `-O2` разделяемого объекта `armv7`, получает ответ, а не спор.

Наборы собираются, только если шаг configure получил указание их искать, поэтому
именно этот флаг и удерживает их под проверкой:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` запускает все линии; `check-neverd-windows-eh-corpus`,
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus` и
`check-neverd-cxx-itanium-eh-corpus` — по одной. Все три хоста CI выполняют
configure с этим флагом и прогоняют все четыре линии: байты везде одинаковы, а
то, что их читает, — нет, и прогон corpus на одном хосте ничего не доказывает
про два других. `scripts/audit_ci_test_inventory.py` отклоняет инвентарь, в
котором не хватает хотя бы одной из четырёх меток, потому что сборка, тихо
переставшая читать corpus, — это регрессия, которую не поймает ни один тест:
пропало как раз то, что проверяло.

При каждом запуске аудит opcodes EVM выполняет неглубокий `git fetch` удалённого
`HEAD` из [официального репозитория
go-ethereum](https://github.com/ethereum/go-ethereum), после чего сообщает точный
проверенный commit. Он повторно использует игнорируемый bare cache
`build/evm-opcode-audit/go-ethereum.git`, но обновляет его перед чтением
закрытого перечня opcodes и назначений байтов:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

CI выполняет тот же live audit при каждом push и pull request, при ручном
запуске и раз в день, поэтому drift upstream обнаруживается даже без изменений
NeverD. Для offline- или исторического воспроизведения явно выберите существующий
checkout:

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

Аудит разрешает только исключения, перечисленные в
`EVMUpstreamOpcodePolicy.def`; opcode upstream, который не представлен и не
проверен явно, приводит к ошибке. Parser и диагностика drift имеют независимое
покрытие Python unit tests в CI:

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

При изменении управления EVM сначала запустите контракт fixed point и домена
высот:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Эти случаи покрывают межблочные внутренние returns, конечные multi-target
merges, сходимость циклов и детерминированный порядок edges, зависящие от пути
высоты стека, ограниченный widening, вызванную корреляцией Cartesian
over-approximation, неизвестные jumps, точные недопустимые цели и stack faults
в strict- и relaxed-режимах. Затем запустите все семь бинарников EVM и аудит
upstream metadata: изменение CFG может влиять на emitter и integration, даже
если локальная форма analyzer корректна.

При изменении dataflow MedIR/HighIR запустите также контракты constant-phi,
selector, typed-operand, malformed-graph и deep-chain:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

Эти случаи доказывают равные и конфликтующие циклические phi, несмежные и
межблочные выражения selector, оба порядка операндов равенства, точные проверки
ширины ABI, типизированные операнды storage/event/calldata, детерминированную
обработку malformed MedIR и итеративный producer walk по 16 384 значениям.

## Создание fixture

### Fixture подъёма и форматов

`unittests/lift/CMakeLists.txt` кросс-компилирует исходники C и assembly во время
сборки. Целевые triple Clang создают ELF-объекты x86-64, i386, AArch64 и ARM32,
PE/COFF-объекты и слинкованные образы, а также PIC/no-PIC Mach-O i386-объекты.
При наличии LLD выбранные объекты также линкуются в исполняемые файлы для patch-
тестов. `NeverDLiftTests` зависит от цели `lift-test-objects`, поэтому обычная
сборка тестового бинарника обновляет сгенерированные fixture.

Большинство lift-тестов используют `NeverDLiftFixture.h`, чтобы вызвать
собранный CLI `neverd` и проверить LowIR, MedIR, HighIR, LLVM IR,
сгенерированный C или переписанный бинарник. Переменная окружения `NEVERD` может
переопределить путь CLI для целевого ручного эксперимента; обычные запуски CTest
используют встроенный CMake исполняемый файл.

### Реконструкция исключений Windows

Изменения табличных исключений Windows требуют как тестов представления, так и
patch-теста связанного PE. Целевой фильтр набора lift охватывает нормализованную
модель unwind/SEH/C++, обработку повреждённых входов, исключительные рёбра CFG,
HighIR, генерацию LLVM WinEH, замену каталога исключений и реконструкцию Guard
CF/EH continuation:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

Защищённой x64 assembly fixture нужны Windows-target Clang и `lld-link`; её
CMake-link использует `/guard:cf` и `/guard:ehcont`. Skip из-за отсутствующего
cross-linker не является доказательством для пути final-image. Успешный
интеграционный тест доказывает, что переписанный PE можно повторно загрузить, а
его таблицы runtime-function, unwind, load-config, Guard CF и Guard EH
continuation остаются отсортированными, присутствуют в файле и указывают только
на исполняемые цели.

Связанная FH3 fixture независимо проверяет нативное C++-замыкание: фиксированные
таблицы состояний, аннотации HighC, сохранение personality, созданные catch-цели
и повторно загруженный граф IP-to-state.

См. [Реконструкцию исключений Windows](windows-exception-reconstruction.ru.md)
для матрицы поддержки анализа/нативной генерации и fail-closed-контракта patch.

### Языковые модели исключений

Всё, что не относится к табличной модели Windows, собрано в одной
сфокусированной цели. `NeverDLanguageEHTests` покрывает цепочку кадров DWARF,
языковую область данных Itanium, ARM EHABI, Darwin compact unwind, метаданные
кадров рантайма Go, механику паники Rust и три рантайма Objective-C:

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

Таблицы этого набора собираются побайтно, а не компилируются: большинство
проверяемых сочетаний не выдаёт совместно ни один тулчейн. Objective-C —
наиболее показательный случай: все три рантайма выдают Itanium LSDA и
различаются только тем, что лежит в ячейке таблицы типов, и это различие
полное, а не количественное. Ячейка Apple адресует `objc_typeinfo`, первые два
поля которого намеренно повторяют `std::type_info`; ячейка Objective-C++ у
GNUstep адресует настоящего наследника `std::type_info`; а ячейка рантайма GNU
вообще не указатель, а сама строка с именем класса. Применение соглашения
одного рантайма к таблице другого не приводит к ошибке — оно сообщает имя
класса, прочитанное из середины чего-то постороннего. Поэтому рантайм
устанавливается по personality кадра до чтения любой ячейки.

Тот же набор фиксирует два различия, которые легко слить и ошибочно сливать.
`@catch(id)` и `@catch(...)` — разные обработчики: первый принимает любой
объект Objective-C и пропускает чужое исключение мимо, — и каждый рантайм
записывает их по-своему; декодер, сообщающий об обоих как о catch-all, вешает
обработчик на исключения, которые на деле пролетели бы мимо. А таблица
call-site в модели setjmp/longjmp индексирует точки вызова, а не адреса: чтец,
не распознавший одну из personality SJLJ, не падает, а выдумывает защищённые
диапазоны и landing pad, которых программа никогда не называла.

### Дифференциальные циклы Unicorn

Семантический fixture проверяет поведение, а не текстовую форму:

1. Написать небольшой случай C/assembly или построить LLVM IR.
2. Скомпилировать его Clang/LLVM для требуемой цели.
3. Исполнить исходный машинный код в Unicorn и получить ожидаемый возврат или другое заданное fixture состояние.
4. Загрузить и поднять в NeverD, вывести LLVM IR и снова скомпилировать результат в машинный код.
5. Исполнить восстановленный код с теми же ABI, входами, раскладкой памяти и моделью CPU.
6. Сравнить наблюдаемые результаты.

Основная реализация находится в
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h).
Patch-full fixture использует `Codegen::compileForRewrite`, тот же backend
переписывания, что и операции patch, а затем сравнивает исходный и
преобразованный код по полной сетке ISA/формат 4×3.

Детерминированная семантическая ошибка NeverD должна проваливать тест. Skips
допустимы только для явной границы внешних возможностей; читайте их причину.
Зелёный итог без cross-linker не доказывает исполнение маршрута формата.

### Дифференциальные бэкенды EVM

Тесты interpreter дают детерминированный 256-bit oracle. Suite emitter
компилирует и исполняет LLVM, переводит C23 через Clang в тот же host harness и,
при наличии `solc`, `anvil`, `cast` и `jq`, разворачивает generated Solidity
локально. Сравниваются status, storage и trace count. Отдельный raw corpus
исполняет pre-Fusaka ALU, calldata/memory copy, перекрывающийся `MCOPY`, Keccak
и return data в native EVM Anvil.

`NeverDEVMOpcodeTests` также обеспечивает metadata architecture: все 150 opcodes
проходят encoding/typed-value roundtrip; проверяются family boundaries,
hardfork aliases и derived stack/host maxima.

### Дифференциальные бэкенды Solana SBF

Тесты метаданных SBF проверяют все свойства версий, границы коллизий опкодов, Murmur3-хеши syscall, релокации, константы ELF machine, регистров и VM-адресов. Fixture загрузчика без включения сторонних бинарников генерируют как устаревшие секционные компоновки v0-v2, так и строгие бессекционные компоновки v3/v4 на основе program headers.

`NeverDSBFSemanticTests` напрямую исполняет проверенные байты инструкций и не использует MedIR, поэтому изменение или повреждение нормализованного IR не может случайно согласовать исходный oracle с бэкендом. Покрыты немонотонная семантика v2, память, syscalls, внутренние call frames, faults, traces и ограничения ресурсов. Модули LLVM проходят проверку; сгенерированный C компилируется с предупреждениями как ошибками, а Rust — с `-D warnings`. Тесты публичного API проходят все стадии IR, дизассемблирование, CFG, метаданные, LLVM, C и Rust, начиная со сгенерированного строгого SBF ELF.

## Однокомандные цели

Пользовательские цели собирают зависимости, а затем запускают CTest с
параллелизмом, рассчитанным по CPU хоста:

| Цель CMake | Выборка |
|------------|---------|
| `check-neverd` | Все зарегистрированные тесты |
| `check-neverd-semantic` | Только `NeverDSemanticTests` |
| `check-neverd-sbf` | Все цели/случаи `NeverDSBF*Tests` |
| `check-neverd-patch-full` | Только `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | Только `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | Только `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | Только `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

У `NeverDIndCallXformTests` и `NeverDAvxUpperXformTests` сейчас нет удобной цели
`check-neverd-*`. Соберите их и выберите по метке, как показано ниже.
`check-neverd-semantic` также не включает отдельные бинарники преобразований или
patch-full; для полного агрегата используйте `check-neverd`.

## Инкрементальный процесс CTest

Сначала соберите принадлежащий набору исполняемый файл, затем выберите его метку.
Так не придётся перелинковывать посторонние большие семантические цели.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# Все целевые тесты/цели EVM
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Все целевые тесты/цели Solana SBF
cmake --build build-release --target \
  NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFAnalyzerTests \
  NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests NeverDSBFEmitterTests \
  NeverDSBFIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'SBF' --output-on-failure --parallel 4
```

Используйте имя CTest, полученное из GoogleTest, для одной регрессии:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Полезные селекторы:

| Команда | Назначение |
|---------|------------|
| `ctest --test-dir build-release -N` | Показать обнаруженные случаи без запуска |
| `ctest --test-dir build-release -L '<regex>'` | Выбрать метку тестового бинарника |
| `ctest --test-dir build-release -R '<regex>'` | Выбрать имена случаев |
| `ctest --test-dir build-release --output-on-failure` | Показывать диагностику только при ошибках |
| `ctest --test-dir build-release --stop-on-failure` | Остановиться после первой ошибки |
| `ctest --test-dir build-release --parallel 4` | Запускать до четырёх случаев параллельно |

Обнаружение GoogleTest использует `DISCOVERY_MODE PRE_TEST`, поэтому
соответствующий тестовый бинарник должен существовать до перечисления CTest.
Таймауты случая и отдельного обнаружения определены в `cmake/AddNeverD.cmake` и
могут увеличиваться только для наборов с измеренными тяжёлыми случаями.

## Какие тесты должны меняться вместе с кодом?

| Область изменения | Начать с | Затем рассмотреть |
|-------------------|----------|-------------------|
| Lifter архитектуры или decode | Именованный случай в `NeverDLiftTests` | Семантический цикл соответствующей ISA |
| LowIR CFG, обнаружение функций, таблицы переходов | Случаи lift CFG/switch | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests` или `NeverDTwoTableXformTests` |
| MedIR, ABI, флаги, типы, SSA | Случаи lift MedIR/соглашения вызовов | Межархитектурные случаи `NeverDSemanticTests` |
| HighIR или структурированный C | Случаи HighIR/decompile | `NeverDCFGLoopXformTests` и проверка компиляции сгенерированного C |
| Loader PE/ELF/Mach-O или входной relocation | Соответствующий fixture формата в `unittests/lift` | Полностадийная проверка загрузки/декомпиляции ячейки |
| Rewrite codegen или выходной relocation | Случаи `RewriteCodegenRTTests` | `NeverDPatchFullTests` и слинкованный patch fixture при наличии |
| Преобразование LLVM IR для patch | Целевой бинарник преобразования | Сетка составных проходов `NeverDPatchFullTests` |
| C API или CLI | Прямой SDK/query-тест и `unittests/semantic/CLIEndToEndTests.cpp` | Соответствующий набор pipeline/формата |
| EVM loader, opcode, IR или backend | Минимальная ответственная цель `NeverDEVM*Tests` | Все цели EVM и компиляция сгенерированных C/Solidity |
| SBF loader, ISA, IR или backend | Минимальная ответственная цель `NeverDSBF*Tests` | Все цели SBF и компиляция сгенерированных C/Rust |
| Распознавание libc | `NeverDLibCTests` | Семантические случаи call/ABI при изменении поведения |
| Запуск процессов или quoting | `NeverDTestProcessTests` | Один затронутый CLI/семантический случай на каждом поддерживаемом хосте |

Тесты должны выражать контракт на самой низкой стабильной границе. Проверка
формы LowIR полезна для привязки к lifter; семантический цикл обязателен, если
две правдоподобные формы IR могут вести себя по-разному. Не используйте golden-
дампы целых функций, когда достаточно небольшого утверждения об opcode, CFG или
наблюдаемом состоянии.

## Связь с CI

CI собирает Release с тестами на Linux, macOS и Windows, проверяет обнаруженный
инвентарь, а затем применяет платформенные исключения меток. Профили определены
в `.github/workflows/ci.yml` и `scripts/audit_ci_test_inventory.py`. Поскольку
ни один shard матрицы не представляет все дорогие наборы, локальный
`check-neverd` остаётся самым ясным полным сигналом перед merge на машине со
всеми необходимыми cross-инструментами.

## Текущий профиль соответствия и sanitizer для Solana SBF

Этот актуальный список заменяет сокращённый SBF-список выше. Suite source
differential требует `rustc` помимо clang; пропуск compiler означает отсутствие
coverage. Полный aggregate включает `NeverDSBFProgramImageTests`,
`NeverDSBFMalformedCorpusTests`, `NeverDSBFISAConformanceTests`,
`NeverDSBFUpstreamConformanceTests`, `NeverDSBFLLVMDifferentialTests` и
`NeverDSBFSourceDifferentialTests`, а также targets metadata, loader, analyzer,
semantic, emitter и integration. Интегрированный профиль проходит 145/145
случаев в 14 бинарниках.

Sanitizer-профиль собирается отдельно в `build-sbf-asan-ubsan`. Он проходит
141/141 core-случаев в 13 бинарниках без отчётов ASan или UBSan; integration
остаётся в интегрированной LLVM-сборке, поскольку prebuilt package не содержит
нужный fork-only header.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```
