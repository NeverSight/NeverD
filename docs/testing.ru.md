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
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Каталог стоков, приоритет идентичности, предфильтр аргументов, охота на переполнение копий, аудит жизни кучи и обязательная шестиячеечная матрица PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/lift` | `NeverDLiftTests` | Формы LowIR decoder/lifter, стадии IR, loader, relocation, fixture форматов, декомпиляция и представительные patch-маршруты |
| Большинство файлов `unittests/semantic` | `NeverDSemanticTests` | Дифференциальная семантика инструкций, ABI, управления, выражений C и lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadata hardfork, нормализация, неоднозначность ABI/signature, CFG/SSA/recovery, полный перебор границ decoder и враждебных inputs, факты proxy/call, семантика interpreter, differential LLVM/C/Solidity и публичный API |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | Метаданные v0-v4 и компоновки ELF, строгая работа verifier/loader, 23 закреплённых ELF-артефакта, независимый официальный oracle, полный охват opcode, враждебные входы, CFG/восстановление и исполняемые различия LLVM/C/Rust |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Эквивалентность переписывания/обфускации для четырёх ISA и трёх объектных форматов |
| Целевые файлы преобразований в `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Быстро перелинковываемые проверки отдельно от большого семантического бинарника |
| `unittests/corpus` (подмодуль) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests` | Метаданные исключений и рантайма, прочитанные из 317 зафиксированных настоящих бинарников; для каждого манифест объявляет нижние границы, которые восстановление обязано преодолеть |

Источники регистрации:
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) и
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) и
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt) и
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt).

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
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus`,
`check-neverd-cxx-itanium-eh-corpus` и `check-neverd-objc-eh-corpus` — по одной.
Все три хоста CI выполняют configure с этим флагом и прогоняют все пять линий:
байты везде одинаковы, а то, что их читает, — нет, и прогон corpus на одном
хосте ничего не доказывает про два других.
`scripts/audit_ci_test_inventory.py` отклоняет инвентарь, в котором не хватает
хотя бы одной из пяти меток, потому что сборка, тихо переставшая читать
corpus, — это регрессия, которую не поймает ни один тест: пропало как раз то,
что проверяло.

Live-аудит opcodes EVM запускается так:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

Локально и в CI стандартный путь принудительно выполняет
`git fetch --depth=1 --force` с официального URL
`https://github.com/ethereum/go-ethereum.git` и проверяет в detached worktree
только точный SHA, только что полученный из удалённого `HEAD` default branch.
Каждый запуск использует приватный временный bare repository с непредсказуемым
именем, сохраняет authority ref official fetch и точный SHA на время detached
worktree, затем уничтожает repository и worktree вместе. Общего постоянного Git
repository или cache нет. `local_docs`, готовый checkout и submodule
не являются путями аудита: закреплённый submodule устарел бы именно тогда, когда
требуется обнаружить live drift.

Каждая команда Git сначала удаляет все унаследованные `GIT_*`, включая
`GIT_CONFIG_*`, и устанавливает только проверенные значения.
`GIT_CONFIG_NOSYSTEM` и `GIT_CONFIG_GLOBAL` отключают system/global config;
`GIT_ATTR_NOSYSTEM` и command-scoped `core.attributesFile` отключают
system/global attributes, а `core.hooksPath` — hooks. Неожиданная
private-repository config, grafts, `objects/info/alternates` или `refs/replace` приводят
к отказу проверки; `GIT_NO_REPLACE_OBJECTS` отключает replacement lookup.

Probe отражает все экспортированные bool-поля `params.Rules`, вызывает
`LookupInstructionSet(params.Rules)` и сканирует все 256 slots.
`EVMUpstreamOpcodePolicy.def` хранит aliases и типизированные
исторические/незапланированные EOF-исключения;
`EVMUpstreamSemanticsPolicy.def` хранит закрытый Rules inventory, mappings forks,
base-stack exceptions и dynamic-immediate families.

CI выполняет тот же live audit только при push в `dev`, pull request, ручном
запуске и ежедневно. Go-probe вызывает открытый API
`LookupInstructionSet(params.Rules)` для каждого сопоставленного fork.
Публичная CLI предоставляет только `--manifest-output`; закрытый manifest
использует `schema 3` и не позволяет выбирать источник, ref, checkout или
toolchain.
`EVMUpstreamOpcodePolicy.def` хранит alias и проверенные исторические/незапланированные EOF
исключения; независимый `EVMUpstreamSemanticsPolicy.def` — правила fork и
исключения stack semantics. Закрытый manifest проверяет точную revision,
activation, byte/name, `base_min_stack` и `net_stack_delta` и отвергает неизвестные
или повторные поля, forks, имена и bytes. Allocation определяется только
`operation.undefined`; `HasCost` — лишь проверка стоимости, поскольку false
возвращается и для определённых zero-cost операций. Каждый slot
`defined && !HasCost` должен точно соответствовать
`EVM_GETH_ACTIVE_WITHOUT_COST` с объявленного fork. Undefined slot со стоимостью,
непроверенный defined slot или утрата marker приводят к закрытому отказу.
Отсутствующие, выходящие за диапазон или синтаксически неиспользованные
объявления также отвергаются: каждый `.def parser` отказывается от `partial`
policy. При сбое CI revision, manifest и log загружаются как artifact. Parser и
диагностика имеют независимые Python unit tests:

`EVMUpstreamSemanticsPolicy.def` относит каждое экспортированное булево поле
`params.Rules` одной записью `EVM_GETH_RULE_FIELD` к `MappedForkSelector`,
`NoOpcodeAllocation` или `ExcludedSelectorExpectedError`. Probe включает поля по
одному через `LookupInstructionSet`: первые две категории требуют nil error,
третья — error, а полный fingerprint 256 opcode/stack slots должен равняться
`ExpectedFork`. `IsEIP155`, `IsEIP2929`, `IsEIP4762` и `IsPetersburg` сейчас
no-allocation поля с Frontier fingerprint; `IsUBT` обязан дать error и Cancun.

`EVMUpstreamSemanticsPolicy.def` объявляет динамические семьи EIP-8024, виды
операций и допустимые stack deltas; `EVMEIP8024Immediates.def` отдельно владеет
decode immediate и классифицирует 256 bytes single/pair. Через `go -overlay`
аудит получает настоящие private handlers `operation.execute` и проходит по
`canonical fork jump tables` и `mainnet active/scheduled jump tables` таблица за
таблицей. Семья `inactive` записывается явно, `partial` семья отвергается. Каждая
активная таблица проверяет `DUPN`, `SWAPN` и `EXCHANGE` со всеми immediate (`3x256`) и
`3 missing-operand cases` по тем же декларативным источникам.

У `EVM_HARDFORK_LATEST` одна canonical цель. Закрытый
`EVMUpstreamForkAliases.def` задаёт Prague→Pectra, Osaka и BPO1–BPO5→Fusaka и
identity для Paris/Shanghai/Cancun/Amsterdam/Bogota; неизвестные имена дают
закрытый отказ. Записанный `audit_unix_time` управляет проверкой
`MainnetChainConfig.LatestFork(time)` (обязан совпасть с NeverD latest) и
alias/probe проверкой `LatestFork(max uint64)`; обе instruction set сравниваются
полностью. Manifest фиксирует `authority=official-fresh-fetch`, официальный URL,
запрошенный `HEAD` и SHA. Probe использует `GOTOOLCHAIN=local`.

Go-probe и Python-контроллер применяют `input/collection/string hard limits`;
чрезмерные inputs, collections или strings приводят к закрытому отказу. Для
`bounded diagnostic output` чрезмерно длинное отображение получает полный
`digest` и `explicit truncated marker`. Вывод и срок каждого child ограничены;
превышение завершает всю `process group`/process tree и осушает pipes.

Текущий receipt schema 3 фиксирует `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, локальный `Go 1.24.0`,
`stack_limit=1024` и `diagnostics=[]`. Он охватывает `21 fork tables` и
`20 Rules probes` с `15 mapped/4 no-op/1 expected-error`. Обе записи
`mainnet active/scheduled` сообщают `upstream BPO2`, закрыто отображённый в
`NeverD Fusaka`. Из `23 table targets` только `Amsterdam/Bogota` активны:
`1536 candidate executions` и `6 missing-operand cases`. `three handler symbols`
совпадают на обеих активных целях. Python audit прошёл `67/67`, а
`C++ Opcode 10/10`. Реальный macOS run прошёл под `sandbox-exec`, финальный
`go run` был offline; Linux workflow принудительно применяет `bubblewrap`.

Все Go stages — `go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` и `go run` — проходят через filesystem sandbox
`capability-root`. Он читает только private probe, fresh geth, проверенный
`resolved GOROOT` и точно необходимые system runtime roots и пишет только в
isolated environment roots. Сеть выдаётся лишь необходимым dependency stages,
финальный run offline. Тесты требуют отказа для sentinels в
`host HOME/workspace` и отсутствия их содержимого в output. Linux проверяет
изоморфный `bubblewrap` без `/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Одиннадцать целей EVM, сейчас зарегистрированных в CMake:

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` перебирает все двухбайтовые входы для каждого
fork, меняющего decoder, сравнивает полный decode и точные границы `JUMPDEST`, а
также прогоняет через все forks детерминированные враждебные входы ограниченной длины.

При изменении управления EVM сначала запустите контракт fixed point и домена
высот:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Эти случаи покрывают межблочные returns, конечные multi-target merges, сходимость,
детерминированный порядок edges, path-sensitive lanes полного стека, сохранение
корреляции, неизвестные jumps, точные недопустимые цели, fail-loud budgets и
stack faults. `MayReachable` сохраняет только кандидата CFG и не создаёт
достоверных фактов. Затем запустите все одиннадцать целей EVM и live upstream audit.

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

### Fixture безопасности памяти

`unittests/safety/fixtures/binaries` содержит зафиксированные образы PE, ELF и
Mach-O для x86-64 и AArch64 вместе со спутником PDB или dSYM, который даёт
каждый формат, и компоновочным MAP для каждого образа. MAP — единственное, что
всё ещё поставляет обрезанная сборка, поэтому каждая ячейка дополнительно
анализируется с явно указанным MAP: это фиксирует, что находка вправе
утверждать, когда не осталось ни типов, ни строк исходника.
`NeverDSafetyIntegrationTests` выполняет все шесть ячеек на каждом хосте;
конфигурация завершается ошибкой, если отсутствует любой требуемый образ или
спутник, и у набора нет пути пропуска из-за инструментов хоста.

Равнозначные двоичные файлы происходят из одного исходного файла. Пересоберите
родную для хоста smoke-fixture командой `make` либо перегенерируйте полную
зафиксированную матрицу так:

```bash
make -C unittests/safety/fixtures matrix
```

Рецепту матрицы нужны кросс-цели Clang для Linux и Windows, COFF-инструменты
LLD, обе архитектуры Darwin и `dsymutil`. Его отладочные пути переотображаются,
а запись командной строки CodeView отключена, чтобы зафиксированные спутники не
сохраняли абсолютный путь рабочего пространства разработчика.

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

Распознать эту форму — не то же самое, что отказаться её читать. Запись SJLJ —
это пара значений ULEB128 (селектор диспетчеризации и смещение действия), и это
смещение означает здесь ровно то же, что и в адресной форме, так что цепочка
действий, перехватываемые типы и спецификации исключений целиком читаются из
таблицы, которая не называет никакого кода. Неизвестной остаётся только
область, которую охраняет каждая запись, потому что о ней говорят записи,
которые сама функция делает в свой слот call-site, а не что-либо в таблице.
Набор закрепляет и тот единственный байт, которому здесь нельзя доверять: GCC
пишет в качестве кодировки call-site `DW_EH_PE_uleb128`, LLVM пишет
`DW_EH_PE_udata4`, оба затем всё равно выдают ULEB128, и ни одна personality
его не читает — значит, не должен и декодер.

Рядом закрепляется и то, какая это personality, потому что именно это решает,
как читается каждая из таблиц выше. GNAT называет свою процедуру теми же тремя
способами, какими GCC называет процедуру каждого фронтенда — `_v0`, `_sj0`,
`_seh0`, — а под Windows регистрирует один символ, переадресуя на другой, так
что все четыре написания обязаны сойтись на Ada. D — зеркальный случай: три
компилятора, три имени одной процедуры и один и тот же набор таблиц за ними.

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

Тесты Low/Med сохраняют path-sensitive whole-stack execution lanes и lane identity
для phi; исчерпание budget, включая `MaxAbstractInstructionTransfers`, является
hard error. Strict отвергает unknown/fork-inactive opcode только на доказанной
`Reachable` lane; `MayReachable` не создаёт определённых фактов. HighIR ограничивает
selector/receive/fallback root lane и успешными terminals. Общий selector не даёт
независимого свидетельства стандарта: variant и return list выбираются только по
стандартной `KnownFunctionVariantInfo` и точной return shape, согласованной всеми
успешными terminals.

Interpreter выполняет типизированный stack preflight до любого эффекта opcode.
`EVMForkSemantics.def` определяет byte `0x44` как `DIFFICULTY` до Paris и
`PREVRANDAO` начиная с Paris. `REVERT`, faults, step limit и resource exhaustion
откатывают состояние транзакции. Ошибка allocation имеет вид
`ExecutionFaultKind::ResourceExhausted`; если не создан даже входной snapshot,
`HasPersistentStateSnapshot` равен false и commit невозможен.

### Регрессии публичных границ и бюджетов EVM

Тесты публичного API независимо изменяют канонические
`Code`/`Fork`/`Instructions`/`JumpDestinations` и каждую LowIR table, range, ID,
lane и edge reference. `execute` обязан вернуть `llvm::Error` до lookup
инструкции, а `lowerToMedIR` — отвергнуть весь malformed или over-budget LowIR
до построения индексов или пропорционального input выделения. Для
`lowerToMedIR` tests требуют проверки options, resources и structure до
field-by-field `canonical decode replay` и до `lowerCanonicalLowToMedIR`.
Публичный HighIR recovery replay-проверяет внешние LowIR/MedIR; только `analyze`
использует `lowerCanonicalLowToMedIR` и `recoverCanonicalHighIR` для собственного
canonical IR без рекурсивного/повторного replay, но со всеми HighIR
option/resource budgets. Затем интерпретатор
проверяет точную границу и +1 для всех пределов `EVMInterpreterLimits.def`:
`MaxSteps` сохраняет отдельный `StepLimit`; исчерпание `MaxMemoryBytes`,
`MaxTraceEntries`, `MaxLogEntries`, aggregate `MaxLogDataBytes` или runtime
`MaxPersistentStateEntries` возвращает `ResourceExhausted` и откатывает эффекты
транзакции. Слишком большой начальный aggregate `MaxHostReturnDataBytes` или
persistent state является API error. `MaxCalldataBytes`, aggregate
`MaxHostEnvironmentEntries` для `BlockHashes`, `Balances`, `CodeHashes`,
`ExternalCode`, `BlobHashes` и aggregate `MaxExternalCodeBytes` также дают API
error. `const execute preflight` отвергает их до копирования environment,
snapshot или result. Views return-data `ArrayRef` и `lower_bound` по
отсортированной таблице покрыты без копии buffer или PC map.

Отдельные LowIR boundary tests покрывают aggregate diagnostic limits
`MaxLowDiagnostics` и `MaxLowDiagnosticBytes`: linear decode и построение CFG
заранее учитывают точное число/итоговые bytes и отвергают ноль.
Тесты безопасности HighIR покрывают отсортированный по lane домен
`Any/Exact/Excluded`, match/exclusion равенства, false-edge match и true-edge
mismatch сырого `XOR(selector, constant)`, уточнение zero word/calldata size/call
value и fail-closed unknown conditions. Их тесты точной границы и -1 покрывают из
`EVMAnalysisLimits.def`
`MaxHighDispatchCandidates`, aggregate
`MaxHighRecoveredArguments`, `MaxHighDiagnostics`, `MaxHighDiagnosticBytes`,
`MaxHighReferenceVisits`, `MaxHighMemoryTransferCells` и
`MaxHighMemoryValueVisits`. Каждая output diagnostic, включая фиксированную
malformed diagnostic, должна учесть количество и финальные bytes до allocation.
LowIR и HighIR diagnostic budgets проверяются независимо; default root CFG
region обязана тарифицировать `MaxHighRegionBlockReferences` до reserve или
копирования block PC.
Регрессии function scope покрывают back-jump через `EQ` и `raw XOR` в общий
dispatcher. Они гарантируют, что другая функция не загрязняет `arguments`,
`mutability`, `return shape` или `region`, а общие bodies и tail calls остаются
достижимыми.
Внешние CALL/CREATE outcomes проверяются как недетерминированные результаты host
по обоим точным CFG-рёбрам, сохраняя восстановление fallback ERC-1167.
Нечитаемое selector condition остаётся Unknown и не может создать fallback или
function facts.

CFG tests выводят `InvalidJumpDestination` из `EVMLowFaultKinds.def` для
`end-of-code JUMPI`: definitely true с invalid target не имеет successful tail
и даёт definite fault; definitely false успешен; unknown сохраняет возможно
успешный false path, не помечая всю lane definite fault.

ABI-тесты применяют точную границу и +1 к grammar limits из
`EVMABIParserLimits.def` и cardinality/text limits публичных таблиц из
`EVMABITableLimits.def`. Они также отвергают неверные kind/standard/evidence
enums, несовпадающую metadata, неканонические signature/return lists, ошибочно
independent общие selectors, dangling/duplicate variants и event-topic `APInt`
неверной word width до indexed selector или sorted topic lookup.

`NeverDEVMOpcodeTests` также обеспечивает metadata architecture: каждый назначенный
opcode проходит encoding/typed-value roundtrip; проверяются family boundaries,
hardfork aliases и derived stack/host maxima.

### Дифференциальные бэкенды Solana SBF

Тесты метаданных SBF проверяют все свойства версий, границы коллизий опкодов, Murmur3-хеши syscall, релокации, константы ELF machine, регистров и VM-адресов. Fixture загрузчика без включения сторонних бинарников генерируют как устаревшие секционные компоновки v0-v2, так и строгие бессекционные компоновки v3/v4 на основе program headers.

`NeverDSBFISAConformanceTests` проверяет каждый байтовый encoding для каждой
версии v0-v4 по независимо аудированному типизированному manifest.
`NeverDSBFExternalOracleTests` затем сравнивает решения об активации и границах
с отдельно собранным официальным процессом Anza.
`NeverDSBFUpstreamConformanceTests` назначает явный исход всем 23 ELF на
закреплённой ревизии Anza.

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
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Все целевые тесты/цели Solana SBF
cmake --build build-release --target check-neverd-sbf --parallel 4
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
| Аудит жизни кучи или охота на переполнение копий | `NeverDSafetyTests` | Все шесть ячеек `NeverDSafetyIntegrationTests` |
| Запуск процессов или quoting | `NeverDTestProcessTests` | Один затронутый CLI/семантический случай на каждом поддерживаемом хосте |

Тесты должны выражать контракт на самой низкой стабильной границе. Проверка
формы LowIR полезна для привязки к lifter; семантический цикл обязателен, если
две правдоподобные формы IR могут вести себя по-разному. Не используйте golden-
дампы целых функций, когда достаточно небольшого утверждения об opcode, CFG или
наблюдаемом состоянии.

## Связь с CI

CI собирает Release с тестами на Linux, macOS и Windows, проверяет обнаруженный
инвентарь, а затем применяет платформенные исключения меток. Профили определены
в `.github/workflows/ci.yml` и `scripts/audit_ci_test_inventory.py`.
`NeverDSafetyTests` и `NeverDSafetyIntegrationTests` обязательны на каждом
хосте матрицы; каждый запуск читает одни и те же зафиксированные PE-, ELF- и
Mach-O-fixtures для x86-64 и AArch64. Поскольку ни один shard матрицы не
представляет все дорогие наборы, локальный `check-neverd` остаётся самым ясным
полным сигналом перед merge на машине со всеми необходимыми cross-инструментами.

## Текущий профиль соответствия и sanitizer для Solana SBF

Этот актуальный список заменяет сокращённый SBF-список выше. Suite source
differential требует `rustc` помимо clang; пропуск compiler означает отсутствие
coverage. Полный aggregate включает `NeverDSBFProgramImageTests`,
`NeverDSBFMalformedCorpusTests`, `NeverDSBFISAConformanceTests`,
`NeverDSBFUpstreamConformanceTests`, `NeverDSBFLLVMDifferentialTests` и
`NeverDSBFSourceDifferentialTests`, а также targets metadata, loader, analyzer,
semantic, emitter и integration. Интегрированный профиль записывает именованные
targets и результаты, а не быстро меняющийся итог.

Sanitizer-профиль собирается отдельно в `build-sbf-asan-ubsan`. Целевые тесты
идут fail-fast без отчётов ASan или UBSan; integration
остаётся в интегрированной LLVM-сборке, поскольку prebuilt package не содержит
нужный fork-only header.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```

### Закреплённый snapshot SBF-свидетельств (2026-08-24)

Gate фиксирует Anza `sbpf` на
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave на
`ef210d67f2fabeee1730498188fa78854260c679` и Solana SDK на
`122f32e571ce39face4beffaccea733e37c207fd`. Официальный ELF manifest проходит
23/23; `NeverDSBFExternalOracleTests` сравнивает 1,411 opcode/boundary-случаев
через `SBFOfficialOracleProtocol.def`, `SBFOfficialVerifierCases.def` и
`SBFOfficialExecutionConstants.def`.
`SBFOfficialELFMutations.def` — табличный контракт malformed ELF; его меняющийся
итог не фиксируется.
Отдельный `41-case strict ELF differential` прогоняет всю матрицу strict-v3
через официальный `verify-elf-batch` и NeverD; эти 41 случай не входят в итог
1,411.

Дополнительная официальная матрица исполнения остаётся отдельной: ровно 508
активных случаев `(Version,Opcode)` плюс 58 граничных случаев дают 566 случаев
точного исполнения. Она не заменяет 1,411 verifier probes или
`41-case strict ELF differential` и не входит в их итоги.
`NeverDSBFAgaveConformanceTests` аутентифицирует Firedancer test-vectors
`68bb4af40235562e8852fa23d5727e49c2a0b862` и сверяет все 1,955 `sol_compat_elf_loader_v1` fixture loader
(1,399 приняты, 556 отклонены). Для каждого принятого ELF он сравнивает
`entry_pc`, `text_off`, `text_cnt`, `rodata_hash` и `calldests_hash`. Этот gate не запускает последующий verifier
инструкций.
Linux Release CI использует `--print-pinned-revision`,
`--print-test-vectors-revision` и `--print-toolchain`, экспортируя
`NEVERD_SBPF_ORACLE` и `NEVERD_AGAVE_CONFORMANCE_ROOT`, поэтому оба внешних gate
обязательны. Локально без явного oracle/corpus env случаи обнаруживаются, но
могут быть пропущены.

`SBF_RUNTIME_VERSION` делает `RuntimeVersionPolicy::ChainProfile` зависимым от
исторического cluster/slot: официальные feature accounts продвигают максимум
ISA от V0 к V1, V2 и V3; сейчас он остаётся V3. Явный v4 использует
`RuntimeVersionPolicy::UpstreamToolchain` для
offline-анализа. Текущий предел 10 MiB равен ровно `10'485'760` байтам; 65,536
— лишь исторический provenance/test. `SBFFaultCodes.def` задаёт стабильные
значения execution fault, а `SBFSourceStatuses.def` отдельно владеет ABI source.

Fixtures масштаба 10,000 защищают worklist, function ownership и multi-latch,
не фиксируя машинное время. Строки cluster/account/slot позволяют
`RPC activation audit`, сохраняя обычные тесты детерминированными и offline.
