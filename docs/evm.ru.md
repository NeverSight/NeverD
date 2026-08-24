**Языки**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# Декомпиляция EVM

[← Индекс документации](README.ru.md)

NeverD загружает традиционный байткод Ethereum Virtual Machine, строит
специализированные 256-битный LowIR, стековый SSA MedIR и восстановленный HighIR,
а затем выводит LLVM IR, C23 или Solidity. Строгий анализ включён по умолчанию,
но legacy EVM не проверяет каждый байт image как opcode: ошибка на точном PC
возникает только тогда, когда заведомо `Reachable` lane выполнения достигает
неназначенного или неактивного в выбранном fork opcode. Мёртвые байты и лишь
`MayReachable` кандидаты CFG не становятся strict-ошибками.

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
на другом — опкодом, не может сдвинуть границу. Присутствующее поле
`deployedBytecode` или `runtimeBytecode` имеет приоритет: явное значение `0x`
принимается как пустой, естественно остановившийся runtime и намеренно запрещает
fallback к creation bytecode. Отсутствующее поле позволяет перейти к следующему
кандидату; отсутствующий либо состоящий лишь из пробелов hex без явного префикса
отвергается. Явный raw-вход также может быть пустым.

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
неоднозначные multi-contract artifacts, неверные границы metadata и
отсутствующий либо пустой hex дают понятные ошибки. Явный пустой raw-вход или
runtime `0x`, напротив, остаётся корректной пустой программой.
`BytecodeLoadOptions::ArtifactContract` выбирает
`Contract` или `path/File.sol:Contract`. Если имя встречается в нескольких
файлах, неквалифицированная форма отвергается, исключая выбор по порядку JSON.

EVM зарегистрирован в центральном loader registry, а не спрятан в backend
plugin. Поэтому CLI, API C, дизассемблер, CFG и запросы IR получают одну и ту же
нормализованную image и опции.

## Hardforks и opcodes

Финализированный набор legacy opcodes поддержан от Frontier до Fusaka, включая
`PUSH0`, transient storage, `MCOPY`, blob opcodes и `CLZ`. Запланированные для
Amsterdam opcodes доступны лишь при явном выборе development-fork; `latest`
остаётся Fusaka.

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
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2).
EOFv1/EIP-7692 не запланирован, а предложение контейнера
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540) имеет статус Stagnant. Старый
репозиторий `execution-spec-tests` архивирован; поддерживаемые тесты перенесены в
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests).
NeverD не выдаёт экспериментальный EOF-контейнер за поведение mainnet.

Strict mode отвергает неизвестный или fork-inactive byte только если заведомо
`Reachable` lane доказывает, что выполнение его достигает. `--evm-relaxed`
сохраняет такой byte как типизированный fault-prefix и в диагностике; backend
по-прежнему выдаёт fault при выполнении, и byte никогда молча не становится NOP.

## Архитектура metadata в стиле LLVM

Ручные EVM metadata следуют многократно включаемому шаблону LLVM `.def`:

- `EVMOpcodes.def` — единственный источник истины для каждого финализированного
  legacy и opt-in development opcode. Encoding, реальные изменения pop/push,
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
  фиксирует naming alias go-ethereum и намеренные исторические/незапланированные
  EOF-исключения. Ортогональный `EVMUpstreamSemanticsPolicy.def` сопоставляет
  forks с `params.Rules`, именует исключения базовой preflight-проверки стека и
  классифицирует dynamic-immediate семейства. Аудит отвергает drift byte,
  activation, `base_min_stack` и `net_stack_delta`, а также любую новую
  непроверенную константу upstream.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` и
  `OutputLanguages.def` порождают упорядоченные enums, parsers, display names,
  варианты CLI и значения C ABI. `EVMAnalysisLimits.def`,
  `EVMInterpreterLimits.def`, `EVMABIParserLimits.def` и
  `EVMABITableLimits.def` объявляют раздельные пределы анализа, интерпретатора,
  parser и публичных таблиц. `EVMConstants.h` хранит общие протокольные ширины и
  стабильные внутренние имена, а из `EVMAnalysisLimits.def` материализует
  defaults анализа и имена диагностических опций; headers интерпретатора и ABI
  материализуют пределы из собственных таблиц.
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
  которого новый перечислитель может выпасть. `EVMKnownSignatures.def` один раз
  хранит канонические написание и selector каждой функции, а затем объявляет
  отдельные стандартные membership-записи `KnownFunctionVariantInfo` с return
  lists и ролью evidence independent/non-independent. Общая для ERC-20 и ERC-721
  сигнатура остаётся одним вызываемым кандидатом, но сама не доказывает ни один
  стандарт и не заимствует return type первой variant. Events и custom errors
  сохраняют отдельные типизированные records.
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

  На back-edge изменившийся loop-carried slot семантически over-approximate до
  `Top`, чтобы fixed point сходился; эта абстракция рекуррентности не связана с
  ресурсами. `MaxAbstractValuesPerSlot`, `MaxStackHeightVariants`,
  `MaxAbstractInstructionTransfers` и пределы для инструкций, блоков, состояний,
  значений, стеков, lanes, edges и worklist — именованные budgets.
  Ноль или исчерпание даёт hard error до вставки, а не emergency widening или
  тихое усечение.

  `EVMLowFaultKinds.def::InvalidJumpDestination` учитывает пути для
  `end-of-code JUMPI`: при заведомо true условии и неверной цели успешного хвоста
  нет и записывается definite fault; заведомо false условие успешно. Unknown
  условие сохраняет только возможно успешный false-путь и не помечает ошибочно
  всю lane как definite fault.
- **EVM MedIR** представляет каждое значение стека как 256-bit SSA и связывает
  все merge phi до запуска детерминированной sparse constant worklist. Частный
  lattice имеет состояния `Uninitialized`, одна точная `Constant` или
  `Overdefined`: равные константы распространяются через blocks и закреплённые
  phi-циклы, а конфликтующий или зависящий от runtime цикл не может придумать
  константу. Worklist проверяет def-use ID и использует тот же ALU evaluator из
  `Semantics.h`, что и interpreter. MedIR также независимо сохраняет основной
  semantic effect, доступ к EVM-memory `none/read/write/readwrite`, доступ к
  source-level state и call-value. Каждой whole-stack lane LowIR соответствует
  отдельная SSA execution lane, а phi явно называет source lane; несовместимые
  стеки не выравниваются по максимальной высоте.
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
  dynamic jump вынуждает консервативное восстановление `nonpayable`. Побайтовый
  flow-sensitive dataflow памяти отслеживает writes с постоянным offset между
  blocks, объединяет overlap/kill и сбрасывает знания при неизвестной write.
  Сейчас для payload доказаны selector и известные байты Panic. Для известной
  декларации custom error Solidity-emitter сохраняет канонические типы параметров,
  но не заявляет восстановление каждого runtime argument value. Другие факты
  остаются кандидатами по свидетельствам.

  Поиск selectors начинается только в root lane и следует по рёбрам несовпадения
  dispatcher: похожее на selector сравнение внутри handler не превращается в
  публичную функцию. Receive и fallback также ограничены root и требуют заведомо
  достижимого успешного terminal. Revert, fault, неплатёжный empty-calldata
  handler или лишь возможный path их не доказывают. Несовместимое использование
  calldata отбрасывает канонического кандидата, а общий selector не даёт
  независимого доказательства стандарта. Только достаточные совместимые
  независимые selectors либо сильное свидетельство точных topic/arity, storage
  slot или proxy позволяют выбрать standard и variant. Статический return list
  выводится лишь когда все заведомо достижимые успешные terminals согласуются с
  точным числом ABI bytes; неразрешённые transfers, противоречивые формы или
  mismatch fail-closed. Revert и fault не являются успешными returns.

  HighIR раздельно ограничивает budgets для функций, посещений lane/operation,
  ссылок blocks в regions, memory requests и bytes, state cells и обновлений
  worklist. Memory fixed point использует только заведомо достижимые execution
  lanes, выполняет bytewise consensus meet и при исчерпании возвращает hard
  error, не обрезая факты.

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

До любого эффекта конкретного opcode interpreter выполняет preflight
типизированной требуемой высоты, pops и сохранённой высоты плюс pushes; underflow
или overflow не могут выполнить половину инструкции. `EVMForkSemantics.def`
выбирает смысл byte `0x44`: `DIFFICULTY` до Paris и `PREVRANDAO` начиная с Paris.
`REVERT`, семантические faults, step limit и исчерпание ресурсов из-за
allocation/length откатывают storage, transient storage, logs и selfdestruct
effects к входному snapshot, сохраняя frame diagnostics и явные revert bytes.
Ошибка allocation отмечается `ExecutionFaultKind::ResourceExhausted` без
выделения строки ошибки; если не удалось создать даже snapshot,
`HasPersistentStateSnapshot` имеет значение false и результат нельзя commit.

### Публичные границы IR и ресурсов

Публичный `execute` сначала проверяет, что
`Code`/`Fork`/`Instructions`/`JumpDestinations` образуют канонический LowIR.
Изменённый fork, поддельная запись инструкции, несовместимый encoding или
ошибочная таблица jump destinations поэтому возвращают `llvm::Error` до того,
как интерпретатор индексирует таблицу инструкций. Публичный `lowerToMedIR`
последовательно проверяет options, resource limits и structure, затем выполняет
`canonical decode replay` для `Low.Code` с вложенными fork/strictness и сравнивает
каждое поле LowIR. Лишь после этого допустимы `lowerCanonicalLowToMedIR`,
построение индексов и выделение output пропорционально записям вызывающего.
Публичный `recoverHighIR` аналогично replay-проверяет внешние LowIR/MedIR.
Приватные `lowerCanonicalLowToMedIR` и `recoverCanonicalHighIR` предназначены
только для IR, принадлежащего `analyze`: они пропускают лишь повторный
нерекурсивный replay, сохраняя обязательными все HighIR option/resource budgets.

Доказательство dispatcher хранит для каждой `MedStateLane` отсортированный
домен selector `Any/Exact/Excluded`. Join объединяет Exact-множества, пересекает
наборы исключений Excluded и вычитает Exact-множество из кофинитного исключения;
расширение домена ставит lane на повторный обход. Равенство записывает candidate
на true edge лишь когда selector разрешён, а на false edge исключает его. Сырой
`XOR(selector, constant)` записывает zero/false edge как match, когда все
канонические successors называют один entry; такой fallthrough не обязан вести
на `JUMPDEST`. Nonzero/true edge — mismatch, исключающий selector, а `ISZERO`
превращает то же выражение в равенство. Selector word, нулевой calldata word,
calldata size и call value guard уточняются по отдельным рёбрам. Unknown
condition прекращает доказательство, а не ведёт его по лишь возможной ветви.

После распознавания кандидата функции обход её scope продолжается с его
`exact singleton selector`. Если функция возвращается в общий dispatcher,
условия `SelectorEquality`, сырого `XOR` и `SelectorWord` проходят лишь
`definite edge`, согласованный с уже сопоставленным selector. Unknown или
несвязанные предикаты консервативно сохраняют все `definite edges`. Эвристика
исключения других entry blocks не применяется: законный поток
`shared body/tail-call` остаётся в scope функции.

Внешние исходы CALL/CREATE отличаются: результат host действительно
недетерминирован, поэтому анализ проходит оба точных CFG-ребра. Это сохраняет
восстановление fallback ERC-1167, но не считает нечитаемое selector condition
доказательством; действительно Unknown dispatcher по-прежнему отказывает закрыто.

`EVMAnalysisLimits.def` задаёт linear decoder и CFG builder единый aggregate
бюджет LowIR diagnostics через `MaxLowDiagnostics` и `MaxLowDiagnosticBytes`.
Оба пути заранее учитывают точное число и итоговые bytes и отвергают нулевой
лимит. Бюджеты diagnostics LowIR и HighIR независимы. Та же таблица раздельно
учитывает `MaxHighDispatchCandidates`,
общепрограммный aggregate `MaxHighRecoveredArguments`, `MaxHighDiagnostics` и
`MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells` и `MaxHighMemoryValueVisits`. Записи candidate и
recovered argument предварительно тарифицируются до вставки в любой целевой
container или выделения name/type. Каждая выходная диагностика HighIR до
построения или копирования точно тарифицируется по количеству и итоговым байтам
сообщения, включая фиксированную диагностику malformed IR. Исчерпание бюджета
возвращает именованную hard error, не отбрасывая молча диагностику или факты.
Default root CFG region тарифицирует `MaxHighRegionBlockReferences` до reserve
или копирования списка block PC.

`EVMABIParserLimits.def` ограничивает вложенность tuples, узлы типов и aggregate
array dimensions. `EVMABITableLimits.def` ограничивает cardinality и общий текст
публичных таблиц signature/variant. Публичная проверка применяет пределы до
parse/hash, а затем отвергает неверные enums, kind metadata, standards, роли
selector evidence, неканонические типы, derived hashes, memberships и collisions.
Production lookup selector индексирован, lookup event использует таблицу,
отсортированную по topic, а topic API до сравнения или упорядочивания проверяет,
что `APInt` имеет ширину ровно одного EVM word.

`EVMInterpreterLimits.def` объявляет `MaxSteps`, `MaxMemoryBytes`,
`MaxTraceEntries`, `MaxLogEntries`, aggregate `MaxLogDataBytes`, aggregate
`MaxHostReturnDataBytes`, `MaxCalldataBytes`, aggregate
`MaxHostEnvironmentEntries`, aggregate `MaxExternalCodeBytes` и
`MaxPersistentStateEntries`. Aggregate host entries охватывает `BlockHashes`,
`Balances`, `CodeHashes`, `ExternalCode` и `BlobHashes`, а byte limit — тела всех
`ExternalCode`. `MaxSteps` сохраняет
явный результат `StepLimit`. Рост runtime memory, trace, logs, log data и новых
ключей persistent state предварительно тарифицируется; превышение возвращает
`ResourceExhausted` и откатывает persistent state, logs и selfdestruct effects.
Слишком большой начальный aggregate host return data или map persistent state —
это вместо того ошибка API `execute`. Интерпретатор хранит host return data как
views `ArrayRef` и применяет `lower_bound` к уже проверенной отсортированной
таблице инструкций, не копируя buffers и не перестраивая PC map для каждого
запуска. `const execute preflight` проверяет программу и все host-input limits
до копирования environment, snapshot состояния или result.

### Live-аудит расхождений с go-ethereum

Стандартный локальный аудит и CI при каждом запуске принудительно выполняют
`git fetch --depth=1 --force` удалённого `HEAD` официальной default branch из
`https://github.com/ethereum/go-ethereum.git`. Каждый запуск создаёт приватный
временный bare-репозиторий с непредсказуемым именем; общего постоянного Git
repository или cache нет. Ревизию выбирают только authority ref, возвращённый
этим fetch, и разрешённый из него точный SHA. Скрипт сообщает SHA и проверяет его
в detached временном worktree, после чего authority repository и worktree
уничтожаются вместе. Ни `local_docs`, ни готовый checkout, ни submodule не являются путями
аудита; закреплённый submodule устарел бы как раз тогда, когда требуется
обнаружить live drift.

Каждая команда Git сначала удаляет все унаследованные `GIT_*`, включая
`GIT_CONFIG_*`, и устанавливает только проверенные значения.
`GIT_CONFIG_NOSYSTEM` и `GIT_CONFIG_GLOBAL` отключают system/global config;
`GIT_ATTR_NOSYSTEM` и command-scoped `core.attributesFile` отключают
system/global attributes, а `core.hooksPath` — hooks. Приватный repository
отвергает неожиданную локальную конфигурацию, grafts,
`objects/info/alternates` и `refs/replace`; `GIT_NO_REPLACE_OBJECTS` дополнительно
отключает replacements. Любое отклонение приводит к закрытому отказу.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

Публичная CLI предоставляет только `--manifest-output`; источник, ref и
toolchain выбирать нельзя. Закрытый manifest использует `schema 3`. Go-probe
отражает полный экспортированный булев inventory `params.Rules`,
вызывает `LookupInstructionSet(params.Rules)` для каждого сопоставленного fork и
сканирует все 256 byte slots. Распределение определяется только маркером geth
`operation.undefined`; `HasCost` служит лишь перекрёстной проверкой стоимости,
поскольку возвращает false и для определённых операций с нулевой стоимостью.
Каждый slot `defined && !HasCost` обязан точно соответствовать
`EVM_GETH_ACTIVE_WITHOUT_COST`, начиная с объявленного fork активации. Undefined
slot со стоимостью, непроверенный defined slot или изменение upstream, скрывающее
marker, приводят к закрытому отказу. Неизвестные, повторные, отсутствующие,
выходящие за диапазон или неразобранные поля и записи являются ошибками. Каждый
`.def parser` также отвергает неиспользованный текст, похожий на macro, вместо
принятия `partial` policy.
`EVMUpstreamOpcodePolicy.def` хранит aliases и типизированные
исторические/незапланированные EOF-исключения и проверяет их overlap/inactive
инварианты. Независимый `EVMUpstreamSemanticsPolicy.def` хранит закрытый
отражённый inventory `params.Rules`, mappings forks, base-stack exceptions и
dynamic-immediate families.
CI запускается при push в `dev`, pull request, вручную и ежедневно; при сбое
точная ревизия, manifest и log загружаются как artifact.

Точнее, `EVMUpstreamSemanticsPolicy.def` относит каждое экспортированное
булево поле `params.Rules` ровно одной записью `EVM_GETH_RULE_FIELD` к
`MappedForkSelector`, `NoOpcodeAllocation` или
`ExcludedSelectorExpectedError`. Аудит включает поля по одному и вызывает
`LookupInstructionSet`: первые две категории требуют nil error, третья — error,
а полный fingerprint всех 256 opcode/stack slots всегда должен совпадать с
`ExpectedFork`. Проверенные no-allocation поля `IsEIP155`, `IsEIP2929`,
`IsEIP4762` и `IsPetersburg` дают Frontier; `IsUBT` должен завершиться ошибкой и
вернуть Cancun fingerprint.

`EVMUpstreamSemanticsPolicy.def` объявляет opcodes каждой динамической семьи
EIP-8024, вид операции и допустимый stack delta;
`EVMEIP8024Immediates.def` остаётся отдельным источником истины для декодирования
immediate и классифицирует все значения single/pair. Через `go -overlay` аудит
получает настоящие private handlers `operation.execute` и проверяет по одной
`canonical fork jump tables` и `mainnet active/scheduled jump tables`. Семья
`inactive` записывается явно, а `partial` семья является ошибкой. Для каждой
активной таблицы выполняются три операции со всеми immediate (`3x256`) и
`3 missing-operand cases`; acceptance, PC delta, marker-derived mutation,
underflow и отсутствующий `0x00` сверяются с теми же декларативными policy.

У `EVM_HARDFORK_LATEST` ровно одна canonical цель. Закрытый
`EVMUpstreamForkAliases.def` отображает Prague в Pectra, Osaka и BPO1–BPO5 в
Fusaka; Paris, Shanghai, Cancun, Amsterdam и Bogota — identity. Неизвестное
новое имя приводит к закрытому отказу. Каждый audit фиксирует и записывает один
`audit_unix_time`, требует, чтобы `MainnetChainConfig.LatestFork(time)`
соответствовал NeverD latest, а `LatestFork(max uint64)` присутствовал в alias
inventory и его canonical fork уже был проверен; обе instruction table
сравниваются полностью. Manifest записывает `authority=official-fresh-fetch`,
официальный URL, запрошенный `HEAD` и разрешённый SHA. Probe фиксирует
`GOTOOLCHAIN=local`.

Go и Python применяют `input/collection/string hard limits` до материализации
враждебных metadata; чрезмерные input, collection или string приводят к
закрытому отказу. Для `bounded diagnostic output` чрезмерно длинное отображение
содержит `digest` полного текста и `explicit truncated marker`. Вывод и срок
каждого дочернего процесса ограничены; превышение завершает всю `process group`/
`process tree` и осушает её pipes.

Текущий live receipt schema 3 фиксирует `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, локальный `Go 1.24.0`,
`stack_limit=1024` и `diagnostics=[]`. Он сравнивает `21 fork tables` и
`20 Rules probes`, разделённые как `15 mapped/4 no-op/1 expected-error`. Обе
записи `mainnet active/scheduled` называют `upstream BPO2`, который закрытый
alias отображает в `NeverD Fusaka`. EIP-8024 охватывает `23 table targets`; лишь
`Amsterdam/Bogota` активны и дают `1536 candidate executions` и
`6 missing-operand cases`. `three handler symbols` совпадают на обеих активных
целях. Python audit `67/67` и `C++ Opcode 10/10` прошли. Реальный audit на macOS
успешно прошёл в `sandbox-exec`, финальный `go run` был offline; Linux workflow
требует `bubblewrap`.

Все этапы Go — `go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` и `go run` — выполняются в filesystem sandbox типа
`capability-root`. Чтение разрешено только private probe, fresh geth,
проверенному `resolved GOROOT` и точно необходимым system runtime roots; запись —
только isolated environment roots. Сеть добавляется лишь dependency-этапам,
которым она нужна, а финальный run остаётся offline. Sentinels в
`host HOME/workspace` должны быть недоступны, а их содержимое не должно появиться
в output. Linux применяет изоморфную политику `bubblewrap` без `/` broad bind.

`NeverDEVMDecoderPropertyTests` перебирает все двухбайтовые входы для каждого
fork, меняющего decoder, сравнивает полный decode и точные границы `JUMPDEST`, а
также пропускает через все forks детерминированные враждебные строки ограниченной
длины.

LowIR lanes полного стека сохраняют корреляцию. `MayReachable` — лишь кандидат
CFG. Изменившийся loop-carried slot семантически становится `Top` на back-edge,
независимо от budgets; их исчерпание ошибочно без emergency widening. Память
HighIR отслеживает постоянные writes, overlap/kill и неизвестную invalidation.
Для payload доказаны selector и известные байты Panic; известная декларация
custom error сохраняет канонические типы, но не обещает каждый runtime argument
value. Другие факты остаются кандидатами по свидетельствам.

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
