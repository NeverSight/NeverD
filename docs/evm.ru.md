**Языки**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# Декомпиляция EVM

[← Индекс документации](README.ru.md)

NeverD загружает традиционный байткод Ethereum Virtual Machine, строит
специализированные 256-битный LowIR, стековый SSA MedIR и восстановленный HighIR,
а затем выводит LLVM IR, C23 или Solidity. Строгий анализ включён по умолчанию:
неназначенный или неактивный в выбранном hardfork opcode вызывает ошибку на
точном значении PC.

Solidity и C — семантические реконструкции. Они сохраняют порядок opcodes,
256-битную арифметику, проверки стека и проверенный поток управления, но не
претендуют на восстановление исходного текста, имён или типов.

## Быстрый старт

```bash
# Проверенный LLVM IR со значениями i256/i512.
./build/bin/neverd lift contract.evm -o contract.ll

# Просмотреть все уровни анализа EVM.
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# Вывести C23 или Solidity.
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Выбрать исторический набор или сохранить неизвестные opcodes как узлы сбоя.
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`, `cfg` и запросы Low/Med/High/LLVM API C также принимают EVM.
Перезапись EVM явно запрещена; `patch` остаётся операцией для нативных бинарников.

## Поддерживаемые входы

| Вход | Распознавание и нормализация |
|------|------------------------------|
| Сырые байты | `.raw`, `.evmraw` или двоичное содержимое с явным расширением EVM |
| Hex-текст | Необязательный `0x`, произвольные пробелы ASCII, `.evm`, `.hex`, `.bin`, `.bytecode`; проверенный hex без расширения также распознаётся |
| Артефакт компилятора | `.json` с `deployedBytecode`, `runtimeBytecode` или `bytecode` в корне либо под `evm`; поддерживается стандартный JSON solc `contracts → file → contract → evm` |

Runtime/deployed bytecode предпочтительнее creation bytecode. Если есть только
creation code, NeverD распознаёт ограниченные константные оболочки
`CODECOPY`/`RETURN` и извлекает скопированный runtime. Обход конструктора
использует тот же декодер одиночной инструкции, что и настоящий decoder, под
анализируемым hardfork, поэтому байт, который на одном форке является данными, а
на другом — опкодом, не может сдвинуть границу. Поле только с необязательным
`0x` считается пустым, поэтому пустой `deployedBytecode` или `runtimeBytecode` не
скрывает полезный creation fallback.

### Трейлеры компилятора

`EVMMetadataFields.def` сводит в таблицу обе формы трейлера. Solidity пишет CBOR
map, два последних байта которой считают только саму map; `vyper` пишет CBOR
array, оканчивающийся этой map, и его два последних байта считают весь footer,
включая себя. Прочитать одну разметку как другую не значит громко упасть: чтение
попадает на два байта в сторону и удаляет два байта настоящего кода, поэтому
пробуются обе, а вход, не совпавший ни с одной, остаётся нетронутым.

Трейлер читается дважды: один раз по входу как он есть и один раз по
runtime-коду, который остаётся после разворачивания оболочки развёртывания. Vyper
перенёс свой трейлер в initcode и оставляет runtime-код без него, так что
читатель, смотрящий только после разворачивания, сообщает о неизвестной сборке
для контракта, который сам себя назвал. Footer последовательности сообщает также
длину runtime-кода, длины секций данных и длину immutables, а они ограничивают
возвращаемый код без выполнения конструктора.

### Контейнеры, которые не являются инструкциями

`EVMBytecodeContainers.def` классифицирует вход до любого декодирования. С тех
пор как EIP-3541 сделал `0xEF` неразвёртываемым, ведущий `0xEF` обещает, что эти
байты — не инструкции:

| Контейнер | Маркер | Обработка |
|-----------|--------|-----------|
| legacy | — | декодируется как инструкции |
| delegation (`eip-7702`) | `0xef0100` и ровно 23 байта | сообщается целевой аккаунт; анализ прекращается |
| eof (`eip-3540`) | `0xef00` | отвергается; ни один форк его не активировал |

Двадцать байт индикатора делегирования — это адрес, а не код. Их декодирование
прочитало бы адрес как опкоды и построило бы граф потока управления аккаунта,
поэтому `info` сообщает цель, а анализ отказывается с указанием причины. Отказ
различает два случая: до Pectra маркер ещё не назначен, а начиная с Pectra
runtime-код цели просто отсутствует. Маркер любой другой длины — это
некорректный вход, а не разновидность контейнера, и он остаётся инструкциями,
чтобы decoder мог назвать байт, который не смог прочитать.

Некорректный hex, нечётное число цифр, неразрешённые linker placeholders,
неоднозначные multi-contract artifacts, неверные границы metadata и пустой код
дают понятные ошибки. `BytecodeLoadOptions::ArtifactContract` выбирает
`Contract` или `path/File.sol:Contract`. Если имя встречается в нескольких
файлах, неквалифицированная форма отвергается, исключая выбор по порядку JSON.

EVM зарегистрирован в центральном loader registry, а не спрятан в backend
plugin. Поэтому CLI, API C, дизассемблер, CFG и запросы IR получают одну и ту же
нормализованную image и опции.

## Hardforks и opcodes

Поддержаны все 150 назначенных legacy opcodes от Frontier до Fusaka, включая
`PUSH0`, transient storage, `MCOPY`, blob opcodes и `CLZ`. По умолчанию `latest`
выбирает Fusaka.

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

Принимаются `dao`, варианты с подчёркиванием, `merge`, `prague` и `osaka`.
Сейчас `latest` и `osaka` разрешаются в каноническую ревизию `fusaka`.

`latest` означает последнюю финализированную mainnet-ревизию в NeverD, а не
вершину разработки Ethereum. [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)
планируется на Q4 2026; инструкции в стадии Review
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) и
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) включаются только
при `--evm-hardfork=amsterdam` (или `bogota`) и до финализации не входят в
`latest`. В EIP-8024 потребляется только допустимый immediate; недопустимый
кандидат остаётся следующей инструкцией.

EOF исключили в
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2), а
execution-spec-tests отмечает его как
[удалённый из Osaka и незапланированный](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md).
NeverD не выдаёт отозванное предложение за поведение mainnet.

Strict mode отвергает неизвестные и fork-inactive bytes. `--evm-relaxed`
сохраняет их в LowIR и диагностике, но backend выдаёт fault при выполнении;
неизвестный байт никогда молча не становится NOP.

## Архитектура metadata в стиле LLVM

Ручные EVM metadata следуют многократно включаемому шаблону LLVM `.def`:

- `EVMOpcodes.def` — единственный источник истины для 150 финальных и четырёх
  opt-in development opcodes. Encoding, реальные изменения pop/push,
  immediate kind, class, activation fork, основной
  effect, независимые доступы к EVM memory, source state и call-value, а также
  termination находятся в одной записи без неявных defaults.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def` и
  `EVMCallValueAccesses.def` задают закрытые типизированные домены. `CALL` может
  одновременно быть external call и memory read/write, `EXTCODECOPY` — context
  read и memory write. State использует lattice `None/Read/Write/Unknown`.
  Payability независимо: `CALLVALUE` обычно приводит к `payable`; read исключают
  лишь когда analyzer доказывает канонический guard `ISZERO(CALLVALUE)` и
  `REVERT` в ненулевой ветви.
- `EVMImmediateKinds.def` задаёт данные PUSH фиксированной ширины и условные
  single/pair encodings EIP-8024; `EVMDecodeStatuses.def` хранит стабильный
  словарь, доступный через LowIR и disassembly. `EVMUpstreamOpcodePolicy.def`
  фиксирует naming alias go-ethereum и намеренные исторические/отозванные
  исключения; `scripts/audit_evm_opcode_metadata.py` отвергает byte drift и
  любую новую непроверенную константу upstream.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` и
  `OutputLanguages.def` порождают упорядоченные enums, parsers, display names,
  варианты CLI и значения C ABI. `EVMConstants.h` хранит ширины и пределы.
- `EVMCalls.def` описывает четыре инструкции, вызывающие другую программу, и
  решётку источников адреса вызываемого. Один флаг в записи — стоит ли операнд
  value между вызываемым и окном аргументов — выводит все последующие позиции
  операндов, а таблица проверяется против базы опкодов, чтобы вывод не разошёлся
  с объявленными счётчиками pop.
- `EVMPrecompiles.def` — словарь адресов, на которых отвечает сам протокол, с
  форком, зарезервировавшим каждый из них, и предложением, поставившим его в
  расписание. `P256VERIFY` по адресу `0x100` отнесён к `eip-7951`: именно это
  Final-предложение зарезервировало его в mainnet вместе с Fusaka, а
  rollup-предложение, откуда пришёл его интерфейс, так и не поставило его в
  расписание. Газ намеренно отсутствует: стоимость precompile является функцией
  её входа и переоценивалась без изменения адреса или операции.
- `EVMMetadataFields.def` и `EVMBytecodeContainers.def` описывают, чем является
  вход ещё до его декодирования: две разметки трейлеров компилятора и
  контейнеры, байты которых вовсе не являются инструкциями.
- `EVMRecoveredFacts.def` владеет написаниями словарей восстановленных фактов,
  поэтому имя, попадающее в вывод, живёт в одном месте, а не в `switch`, из
  которого новый перечислитель может выпасть. `EVMKnownSignatures.def` делает то
  же для трёх ролей сигнатуры.
- `Semantics.h` хранит независимый от target scalar ALU evaluator. Interpreter
  и constant folding разделяют проверенную реализацию `APInt`; lowerings
  LLVM/C/Solidity остаются явными и fail-loud.

Decoder — граница сырых байтов. Assigned identity отделена от fork activation:
relaxed decode сохраняет name, introduction fork и immediate width неактивного
opcode, но даёт консервативную faulting semantic query. Поэтому неактивный
immediate не сдвигает последующие границы. Анализ, interpreter и emitters
используют сгенерированный `Opcode` и metadata queries; raw encoding появляется
только в trace/host ABI. 17 stack inputs `SWAP16` и максимум 7 host arguments —
отдельные пределы, выведенные compile-time.

`OpcodeInfo` нельзя default-construct в наполовину корректном виде, а его имя —
`llvm::StringLiteral`. Compile-time validator отвергает дубликаты encoding,
неизвестные properties, неверные ALU contracts, effect/state mismatches,
ошибки семейств PUSH/DUP/SWAP/LOG, terminators и host results. Консервативные
unknown metadata создаёт только явная factory.

`.def` — ручные базы вроде LLVM
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def).
`.inc` предназначен для действительно сгенерированных фрагментов, например
TableGen. Богатые declarative records хранятся в `.td`, а
[TableGen](https://llvm.org/docs/TableGen/ProgRef.html) создаёт `.inc`. Пока у
NeverD нет EVM TableGen step, `.inc` без генератора был бы лишь имитацией.
C++ следует [стандартам LLVM](https://llvm.org/docs/CodingStandards.html),
LLVM ADT/string types на границах и исчерпывающим fail-loud switches.

Новый opcode требует полной записи `EVM_OPCODE`, общей scalar semantics,
явных backend lowerings и целевых tests. Новый hardfork — упорядоченной записи
`EVM_HARDFORK` и aliases. Типизированные API, lookup, validation, classification
и CLI расширяются без параллельных таблиц.

## Модель анализа

- **EVM LowIR** хранит PC, encoding, типизированный статус immediate и
  декодированные операнды глубины стека (включая дополнение PUSH нулями справа и
  правило условного потребления EIP-8024), blocks, predecessor/successor edges,
  проверенные цели `JUMPDEST`, reachability и домены высоты стека. CFG
  восстанавливается детерминированным fixed point для всей программы: для
  каждого stack slot распространяется ограниченное конечное множество 256-bit
  значений, а для каждой конкретной высоты сохраняется отдельный abstract stack.
  Поэтому константы через blocks внутренних call/return, перестановки стека,
  `PC`/`CODESIZE` и скалярные операции ALU могут разрешать одну или несколько
  конкретных целей. Действительно неизвестная цель остаётся явным indirect edge.

  `AnalyzeOptions::MaxAbstractValuesPerSlot` ограничивает каждое конечное
  множество; превышение расширяет slot до `Unknown`. `MaxStackHeightVariants`
  ограничивает число зависящих от пути высот в block и выдаёт явную ошибку
  analysis-limit вместо усечения CFG. Оба лимита отвергают ноль. Конечные
  значения, созданные Cartesian-операцией после нереляционного слияния стеков,
  помечаются как over-approximation: недопустимые кандидаты диагностируются, но
  не заставляют strict-анализ отвергать bytecode только из-за потерянной
  корреляции slot. Точная недопустимая цель по-прежнему завершается ошибкой на
  соответствующем jump PC. В relaxed-режиме stack faults диагностируются и
  завершают только ошибочный abstract path; невозможный fallthrough не создаётся.
- **EVM MedIR** представляет каждое значение стека как 256-bit SSA и связывает
  все merge phi до запуска детерминированной sparse constant worklist. Частный
  lattice имеет состояния `Uninitialized`, одна точная `Constant` или
  `Overdefined`: равные константы распространяются через blocks и закреплённые
  phi-циклы, а конфликтующий или зависящий от runtime цикл не может придумать
  константу. Worklist проверяет def-use ID и использует тот же ALU evaluator из
  `Semantics.h`, что и interpreter. MedIR также независимо сохраняет основной
  semantic effect, доступ к EVM-memory `none/read/write/readwrite`, доступ к
  source-level state и call-value. Полиморфный стек LowIR на этой границе
  консервативно выравнивается по вершине; отсутствующие на некоторых путях slots
  становятся явными unknown values, а детерминированная диагностика фиксирует
  потерю точности.
- **EVM HighIR** восстанавливает Solidity dispatcher selectors, вероятные слова
  calldata и return, mutability, постоянные storage slots, факты LOG/event и
  revert, а также function/CFG regions. Проверенный producer index и итеративный
  memoized value walk восстанавливают факты из типизированных операндов MedIR, а
  не по расстоянию между инструкциями: сравнения selector могут пересекать
  blocks и phi, использовать любой порядок операндов `EQ` и сохранять
  производную 32-bit mask; argument offsets, storage keys, event topic0,
  non-payable/receive guards и точные размеры return в 32 байта используют свои
  семантические входы. Граф MedIR структурно ограничивает итеративный обход, а
  malformed, смешанные или циклические выражения считаются unknown.
  Конфликтующие цели одного selector диагностируются и пропускаются. Payability
  остаётся независимой от state-access lattice, а достижимый неразрешённый
  dynamic jump вынуждает консервативное восстановление `nonpayable`. Пока MedIR
  не имеет memory SSA, восстановление payload custom error и payload исходящего
  вызова остаются единственными ограниченными эвристиками окна инструкций; восстановленные имена и типы явно
  остаются эвристическими.

  HighIR также фиксирует исходящую половину интерфейса: каждый `CALL`,
  `CALLCODE`, `DELEGATECALL` и `STATICCALL` вместе с происхождением вызываемого,
  зарезервированным адресом, который он называет, если анализируемый форк такой
  резервирует, селектором, который вызов кладёт в начало calldata вызываемого, и
  переданным значением, когда оно константно. `CREATE` и `CREATE2` исключены:
  они исполняют код, у которого ещё нет адреса, поэтому восстанавливать нечего.

  Восстановленная исходящая сигнатура никогда не попадает в список стандартов,
  на которые отвечает сама программа. Отправка `transfer(address,uint256)`
  говорит, что программа использует токен, а не что она им является, и смешение
  этих двух вещей заставило бы считать ERC-20 каждый роутер и каждое хранилище.
  Делегирующий вызов дополнительно сообщается как proxy-факт, поскольку только у
  него код вызываемого исполняется над собственным storage этой программы.

  Поиск precompile ограничен анализируемым форком, а не самым новым из
  существующих. Вызов адреса precompile, которую вводит более поздний форк,
  попадает в аккаунт без кода, завершается успешно и ничего не возвращает, так
  что называть её значило бы сообщить об операции, которую программа заведомо не
  выполняла.
- **LLVM** выдаёт verifier-clean state machine `i32 @evm_execute(ptr)` с
  проверенным стеком 1024 × `i256`, intermediates `i512`, защищённым signed
  division, saturating shifts, точными `BYTE`/`SIGNEXTEND`/`CLZ` и switches.

Детерминированный interpreter — semantic oracle. LLVM/C компилируются и
сравниваются; Solidity разворачивается в Anvil и сверяется по storage/trace.
Сырой pre-Fusaka corpus запускается также в native EVM Anvil и независимо
проверяет ALU, calldata copy, перекрывающийся `MCOPY`, memory expansion, Keccak
и return data. Account operands маскируются до 160 бит по
[спецификации](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py),
ширины окружения проверяются, `BLOCKHASH` соблюдает окно 256 блоков. Буфер EIP-211
отделён от финального output: только `RETURN`/`REVERT` заполняют
`ExecutionResult::ReturnData`; CREATE/CREATE2 следуют тому же правилу.

## Контракт сгенерированного C

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

Операции окружения используют следующую host ABI. `a0` — исходная вершина стека,
неиспользуемые аргументы равны нулю, return — первое pushed value. Trace hook
вызывается перед каждой инструкцией.

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment, uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);
void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

```bash
clang -std=c2x -ffreestanding -c contract.c
```

Frontend должен поддерживать `_BitInt` не менее 512 бит. Apple Clang для Darwin
пока ограничен; на macOS нужен подходящий non-Darwin target либо LLVM output.

## Контракт сгенерированного Solidity

Output сочетает объявления function/storage/event/error для selectors с точной
PC/stack state machine. Константный slot выводится как
`recovered_storage_slot_3 = uint256(0x3)`, а не как выдуманная последовательная
state variable.

Contract намеренно `abstract`. Переопределите `_evmHost` для эффектов окружения;
`_evmTrace` виртуален и по умолчанию испускает `EVMTrace`.

```bash
solc --bin contract.sol
```

## API C

```c
neverd_session_t session = neverd_session_create();
neverd_evm_set_hardfork(session, "cancun");
neverd_evm_set_strict(session, 1);
if (!neverd_session_load(session, "contract.evm") ||
    !neverd_session_analyze(session)) {
  /* inspect neverd_last_error(session) */
}
const char *solidity = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_SOLIDITY, 0, 0);
const char *c = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_C, 0, 0);
neverd_free_string(solidity);
neverd_free_string(c);
neverd_session_destroy(session);
```

`neverd_decompile_all` сохраняет совместимость и выводит C. Новые entry points:
`neverd_session_bitness`, `neverd_evm_set_strict`,
`neverd_evm_set_hardfork`, `neverd_decompile_all_ex`. Solidity для native,
старый LLVM-to-C route для EVM и native object roundtrip для EVM отвергаются
явно, а не игнорируются.

## Явные ограничения

- Только legacy bytecode; EOF containers пока не декодируются.
- Amsterdam/Bogota — явные development targets; до финализации запланированных
  opcodes `latest` остаётся финальным Fusaka.
- Нет RPC, поиска chain state, учёта gas/refund и выполнения precompiles.
- Извлечение creation распознаёт обычные static wrappers, но не эмулирует транзакцию.
- Dynamic jumps остаются indirect без ограниченного constant proof.
- ABI types, names, mappings, events и custom errors восстанавливаются best-effort.
- Самостоятельное выполнение эффектов требует C/Solidity host hooks.
