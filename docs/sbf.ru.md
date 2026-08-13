**Языки**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Декомпиляция Solana SBF

[← Индекс документации](README.ru.md)

NeverD загружает deploy-артефакты Solana как полноценные SBF-программы и
предоставляет весь путь через CLI и `libneverd`:

```text
SBF ELF
  → учитывающие версию ELF loader и verifier
  → LowIR без потерь + CFG
  → нормализованный MedIR + факты о регистрах
  → восстановленные функции, syscalls, CPI/account-наблюдения и регионы
       ├─ проверенный LLVM IR
       ├─ переносимый C11
       └─ безопасный stable Rust
```

Реализация следует текущей VM Anza `sbpf`, а не generic Linux eBPF. Metadata
версий, opcode, syscall, relocation и протокола находятся в базах `.def` под
`include/neverd/sbf/`; loaders и backends используют сгенерированные типизированные
таблицы без дублирования encoding и названий.

## Вход и поддерживаемые версии VM

Вход — ELF64 little-endian программа Solana (`.so`).

| SBF | ELF layout | Machine ID | Важное поведение ISA | Статус |
|-----|------------|------------|-----------------------|--------|
| v0 | legacy sections/relocations | `EM_BPF`, `EM_SBPF` | фиксированные frames с виртуальными промежутками, LDDW, legacy memory opcodes | legacy |
| v1 | legacy sections/relocations | `EM_BPF`, `EM_SBPF` | вручную настроенные stack frames | legacy |
| v2 | legacy sections/relocations | `EM_BPF`, `EM_SBPF` | PQR arithmetic, перемещённые memory encodings, swapped immediate subtraction, source-register CALLX | legacy, немонотонный |
| v3 | strict program headers, без dynamic relocations | `EM_BPF` | static syscalls/calls, JMP32, destination-register CALLX, bytecode `0x100000000`, rodata в нуле | текущий deploy toolchain format |
| v4 | strict program headers, без dynamic relocations | `EM_BPF` | ISA v3 и aligned memory-mapping contract | текущий upstream `sbpf`; доступность зависит от cluster |

Номер версии сам по себе не является спецификацией, поэтому
`SBFVersionFeatures.def` хранит поведенческие изменения, а таблица версий их
составляет. Каждая запись несёт предложение SIMD, принявшее изменение, и
предикат, который `anza-xyz/sbpf` предоставляет для того же вопроса: несколько
предложений попадают в одну версию, а одно предложение меняет несколько
несвязанных вещей. SIMD-0173 одновременно переносит классы инструкций памяти и
выводит из обращения `lddw`, тогда как SIMD-0174 независимо добавляет класс PQR
в той же версии. Запись предложения у возможности, а не у версии, и делает
восстановленное утверждение о версии прослеживаемым до документа, который его
решил; по той же причине два правила `callx` — разные возможности: SIMD-0173
читает регистр источника, а SIMD-0377 — регистр назначения.

Изменения v2 намеренно не переходят в v3. Feature checks явны, это не догадки
`version >= N`. Strict по умолчанию отвергает malformed headers/ranges/alignments,
unsupported writable legacy sections, invalid continuations/registers/
frame-pointer writes/branches и неактивные opcodes, указывая slot и virtual address.

## Runtime, о котором идёт речь

Версия ISA берётся из файла. Почти ничто другое — нет. Какие syscalls
разрешаются, зависит от сети и слота; на каких байтах лежит поле аккаунта,
зависит от loader, которому принадлежит программа; получает ли entrypoint второй
аргумент, зависит от переключателя, который поворачивает сама сеть; а можно ли
программу развернуть — вопрос, отличный от того, выполняется ли она. Один
переключатель версии не выражает ничего из этого, поэтому это отдельные оси с
отдельными таблицами.

`SBFRuntimeFeatures.def` фиксирует кластеры, назначения и гейты, меняющие то, что
сообщает NeverD: каждый — со своим идентификатором runtime, аккаунтом, само
существование которого его включает, и слотом, на котором каждый кластер его
активировал. Гейт, у которого нет строки для кластера, там не активирован.
`simd-0321` включён на каждом кластере; `simd-0449` и syscall SHA-512 включены на
testnet и devnet и выключены на mainnet — именно поэтому программа, работающая на
devnet, отказывает на mainnet.

`SBFLoaders.def` фиксирует принадлежность и сериализацию. Развёртывание и
исполнение перестали быть одним ответом много лет назад: `loader-v1` и
`loader-v2` отвергают каждую управляющую инструкцию, которую им присылают, и
продолжают выполнять уже принадлежащие им программы — поэтому их сериализация
по-прежнему обязана быть читаемой.

| Загрузчик | Сериализация | Развёртывает | Исполняет |
|-----------|--------------|--------------|-----------|
| loader-v1 | `abi-v0` | нет | да |
| loader-v2 | `abi-v1` | нет | да |
| loader-v3 | `abi-v1` | да | да |
| loader-v4 | `abi-v1` | нет | нет (встроенная программа удалена) |

`SBFAccountLayout.def` размещает каждое поле аккаунта в каждой сериализации.
Различие не сводится к выравнивающим байтам: поля упорядочены по-разному, так
что по смещению три невыровненная форма держит первый байт адреса аккаунта, а
выровненная — его флаг executable, и само значение никак не объявляет, какая из
них прочитана. Повторно встреченный аккаунт занимает один байт в `abi-v0` и
восемь в `abi-v1`, что сбивает весь обход записей, а не одно поле.

Разрешается ли вызов — это три вопроса, а не один, поэтому
`SBFSyscallLifecycle.def` хранит, насколько устоялась опубликованная сигнатура, а
`SBFSyscallRegistration.def` хранит остальное: в каком реестре появляется
syscall, какой гейт им управляет и в какую сторону этот гейт направлен.
Направление важно, потому что гейт способен отнять так же легко, как и добавить —
именно активация `disable_fees_sysvar` убрала syscall sysvar комиссий, — а
прочитать отнимающий гейт как добавляющий значит перевернуть ответ сразу для всех
кластеров. `sol_alloc_free_` вовсе не нуждается в гейте: runtime продолжает его
соблюдать и отказывается принимать новую программу, которая его вызывает; это
различие между двумя реестрами и ничто иное.

На runtime, где активирован `simd-0321`, entrypoint получает в `r2` ещё и адрес
данных инструкции. NeverD моделирует его как самостоятельный вид значения, а не
как константу, потому что его местоположение зависит от аккаунтов: выдуманный
адрес позволил бы сообщить load через него как именованное поле аккаунта. До
активации регистр приходит нулевым, и программа, читающая его, читает ноль.
Поэтому сгенерированные точки входа LLVM, C и Rust принимают и входной буфер, и
данные инструкции: вызываемое, которому нельзя передать второе, не воспроизведёт
программу, которая его читает.

Текущий toolchain использует `cargo build-sbf`. Production-программы v3+ в
основном Rust, upstream C toolchain не нацелен на v3. Это не ограничивает NeverD:
любой принятый SBF можно вывести как C или Rust.

- [Программы Solana](https://solana.com/docs/core/programs)
- [Выполнение](https://solana.com/docs/core/programs/program-execution)
- [Справочник syscall](https://solana.com/docs/core/programs/syscall-reference)
- [Anza sbpf VM](https://github.com/anza-xyz/sbpf)
- [Agave changelog](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

## CLI

```bash
neverd info program.so
neverd headers --json program.so

neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

neverd lift -o program.ll program.so
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so

# Укажите, о каком runtime идёт ответ. Ничего из этого нет в файле программы.
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`, `--sbf-slot`, `--sbf-loader` и `--sbf-purpose` выбирают профиль
runtime. Значения по умолчанию описывают mainnet-beta в его нынешнем виде, под
`loader-v3`, для уже развёрнутой программы. Вопрос о развёртывании вместо этого
сообщает syscalls, которые не пустили бы программу в сеть, даже если сама сеть
продолжала бы её выполнять.

`--sbf-version=auto|v0|v1|v2|v3|v4` меняет semantics только после проверки
обнаруженного layout. Он предназначен для повреждённых или исследовательских
fixtures, а не для интерпретации недоверенного файла как другого packaging standard.

## Анализ и восстановление

LowIR сохраняет 8-byte encoding, raw fields, LDDW continuations, resolved calls,
syscall hashes, blocks, edges, reachability и diagnostics. MedIR нормализует
version-specific encoding в типизированные 32/64-bit operations, явные extensions,
guarded arithmetic, memory widths и call kinds. Register dataflow отслеживает
константы и stack/rodata addresses.

HighIR восстанавливает entry/internal functions, direct call edges, официальные
syscall names, strings, natural loops, reducible conditionals и консервативные
Solana observations. `sol_invoke_signed_rust`/`sol_invoke_signed_c` — CPI;
memory на input register — account/input access. Без IDL Anchor types и account
layouts не выдумываются.

C и Rust разделяют backend-neutral structuring pass. При единственном reducible
представлении выводятся `if`/`if-else` и `while`/`loop`; internal calls, CALLX и
irreducible flow сохраняют точный PC dispatcher.

База syscall включает logging, memory, PDA, SHA-256/Keccak/Blake3, Poseidon,
secp256k1, curves/alt-bn128, modular exponentiation, CPI, return data, sibling
instructions, compute units и sysvars, включая epoch rewards. Relocations
`R_BPF_64_64`, `R_BPF_64_RELATIVE`, `R_BPF_64_32` обрабатываются централизованно.
Text relocations, обе половины LDDW и официальный Murmur3 CALL key применяются
до decode. Если `R_BPF_64_32` уже применён и stripped, registry key заново
вычисляется по symbols и target slots для восстановления internal calls.

## Восстановление программы Solana

Поверх машинной модели SBF NeverD сообщает, что программа означает именно как
программа Solana. Каждый зафиксированный факт несёт породившее его свидетельство,
а то, что байты не определяют, остаётся незаполненным, а не угаданным.

| Восстановлено | Свидетельство |
|---------------|---------------|
| Адреса base58 в read-only данных | совпадение в `SBFKnownAddresses.def` либо константа, которую создаёт код |
| Собственный объявленный адрес | `sol_memcmp_` ровно на длину ключа против read-only константы |
| Диспетчеризация инструкций Anchor | 64-битное сравнение, константа которого равна SHA-256 discriminator с namespace |
| Цели CPI | запись instruction, достижимая от аргумента invoke |
| Операция, которую выбирает вызов | selector, перечисленный в `SBFProgramInstructions.def`, либо ведущий Anchor discriminator |
| Seed выводимого адреса | массив seed-дескрипторов, достижимый от аргумента деривации |
| Чтение и запись полей аккаунта | load или store, адрес которого доказуемо попадает в сериализованный input |

Загрузчик передаёт один аргумент — сериализованный буфер input в начале региона
input, поэтому распространение констант от этого входного состояния даёт
именованные поля аккаунта вместо сырых смещений. `SBFAccountLayout.def` хранит
официальную сериализацию; её фиксированные поля проверяются на покрытие своего
диапазона без зазора.

Anchor выводит discriminator, хешируя `<namespace>:<name>` через SHA-256 и
оставляя первые восемь байт, что необратимо. Поэтому NeverD только подтверждает
кандидатов: `SBFAnchorNames.def` — словарь повторяющихся имён, а `--sbf-idl`
задаёт собственный IDL программы и имеет приоритет. 64-битное сравнение
называется discriminator лишь после того, как хотя бы одно из них разрешилось в
имя.

`SBFKnownAddresses.def` фиксирует адреса протокола и канонических программ.
Каждая запись обязана декодироваться ровно в 32 байта, что обеспечивает набор
тестов. Восстановлению также нужен ABI системных вызовов: SBPFv3 отображает
read-only данные на нулевой адрес, поэтому аргумент длины и низкий адрес данных —
одно и то же число. `SBFSyscalls.def` поэтому фиксирует, какие регистры
аргументов несут адрес VM, и только за ними идёт анализ.

Два invoke-syscall описывают одну и ту же instruction двумя разными структурами,
и `SBFCPIABI.def` хранит оба формата с ключом по выбирающему их syscall. Чтение
одного со смещениями другого не даёт ошибки: оно молча сообщает первый аккаунт
как вызываемую программу. Затем `SBFProgramInstructions.def` называет
запрошенную у канонической программы операцию по selector, который публикует её
собственный интерфейс: индекс варианта bincode для программ system, stake,
lookup-table и upgradeable-loader и ведущий байт для token-программ, включая
диапазон расширений Token-2022 поверх нумерации, общей с исходной token-программой.
Не перечисленный selector сообщается числом.

### Рабочая память и окна syscall

Программа почти никогда не передаёт runtime константу. Она собирает массив seed,
сериализованную instruction и её payload в собственном frame или в куче и
передаёт указатель. Чтение одного загруженного образа показало бы указатель и
ничего из того, что он адресует, поэтому восстановление ведёт побайтовую модель
памяти, писать в которую может только эта программа; она ограничена
`kMaxModeledScratchBytes`.

Что переживёт вызов, решают два факта. `SBFSyscalls.def` говорит, какие регистры
аргументов несут адрес VM; `SBFSyscallMemory.def` говорит, что runtime делает
через них, как чтение или запись с протяжённостью `Fixed`, `Counted` или
`Opaque`. Syscall без окна записи не может изменить ни одного байта вызывающей
стороны, поэтому всё доказанное до `sol_log_` доказано и после. Запись,
ограниченная аргументом длины, аннулирует ровно это окно. Запись `Opaque`
аннулирует свой базовый адрес и всё выше него, потому что буфер никогда не
простирается ниже своего начала и не пересекает границу региона VM. Сводка
эффектов в `SBFSyscalls.def` и таблица окон проверяются друг против друга в обе
стороны, так что ни одна не может разойтись сама по себе.

`sol_memcpy_`, `sol_memmove_` и `sol_memset_` не просто аннулируют, а
прослеживаются: при доказанных приёмнике, длине и источнике байты приёмника
становятся известны. Именно это восстанавливает операцию, которую вызывает
Anchor-программа, ведь её payload копируется на место, а не отображается.

Вызов функции, которую этот анализ не описал, считается пишущим всюду, куда он
может дотянуться. Вызываемый работает в собственном frame, поэтому вызов, все
аргументные регистры которого доказуемо не адресуют рабочую память, оставляет
модель нетронутой; всё остальное её сбрасывает. `sol_invoke_signed_rust` и
`sol_invoke_signed_c` пишут данные аккаунтов, а не память вызывающей стороны, так
что две invocation, собранные в одном блоке, обе читаемы.

Модель — прямой must-анализ по внутрипроцедурному CFG: байт доживает до блока
только тогда, когда каждый путь до него записал одно и то же значение. Рёбра
вызовов не прослеживаются, потому что вызываемый ничего не наследует от frame
вызывающего. Программы более чем с `kMaxScratchFlowBlocks` блоками сохраняют
поблочное восстановление и теряют только факты, пересекающие границу блока.

`SBFLints.def` каталогизирует наблюдения по всей программе: отсутствие проверки
signer или owner, непостоянная цель вызова, устаревший или закрытый feature gate
системный вызов и версия SBPF, которую SIMD-0500 перестанет принимать при
развёртывании. У каждого есть серьёзность и уверенность, и ни один lint не меняет
декодированную семантику. Ничто в этом слое не обращается к сети.

## Контракт сгенерированного LLVM runtime

LLVM никогда не считает VM address host pointer. Checked declarations
load/store/syscall возвращают `i32` status; load/syscall пишут `i64` через output
pointer. Любой nonzero status ветвится в явный SBF fault block. Module проходит
`llvm::verifyModule` перед выходом.

## Контракт сгенерированного C host

```c
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;
```

`width` задан в битах; nonzero host return становится явным SBF status.
Представлены registers, return PC, сохранённые r6-r9, frame pointer, VM addresses,
division faults, wide PQR и wrapping shifts. Выводятся только нужные helpers,
поэтому проходит `clang -Wall -Wextra -Werror`.

## Контракт сгенерированного Rust host

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

Вывод — безопасный stable Rust без raw pointers. Entry point generic по trait и
использует fixed-size safe arrays. Тесты компилируют его командой
`rustc --edition=2021 -D warnings`.

## API C

После SBF load остаются доступны функции session, disassembly, IR dumps,
CFG/call graph JSON, sections, symbols, relocations, strings и headers. Rust
выбирается добавленным без нарушения ABI значением output-language enum.

```c
neverd_session_t session = neverd_session_create();
neverd_sbf_set_strict(session, 1);
neverd_sbf_set_version(session, "auto");
/* О каком runtime идёт ответ. Значения по умолчанию описывают mainnet-beta в
   его нынешнем виде, под loader-v3, для уже развёрнутой программы. */
neverd_sbf_set_cluster(session, "devnet");
neverd_sbf_set_slot(session, 474768000);
neverd_sbf_set_loader(session, "loader-v3");
neverd_sbf_set_purpose(session, "deployment");
const char *rust = neverd_decompile_all_ex(
    session, "program.so", NEVERD_OUTPUT_RUST, 0, 0);
/* consume rust, then: */
neverd_free_string(rust);
neverd_session_destroy(session);
```

## Проверка и ограничения

`unittests/sbf/` покрывает metadata invariants, loaders v0-v4, strict verifier,
CFG/recovery, verified LLVM, warning-free C/Rust compilation, raw interpreter,
независимый от MedIR, и C API. Fixture conditional+loop выполняется в обоих
языках против raw oracle; официальный ELF corpus `sbpf` используется локально
без добавления сторонних бинарников.

- SBF rewriting и object-code roundtrip явно запрещены.
- Anchor IDL/type recovery и live RPC/accounts находятся вне loader.
- Syscalls и VM memory в output доступны через host contract, это не автономный runtime.
- Relaxed служит инспекции и не назначает угаданную semantics.

## Текущая база соответствия (2026-08-10)

После relocations единый неизменяемый `ProgramImage` с VM-адресами служит общим
источником истины для decoder, interpreter, восстановления strings и бэкендов
LLVM/C/Rust. Отдельных копий text или rodata, способных разойтись с семантикой
loader, больше нет.

Замкнутые наборы записаны в `SBFVersions.def`, `SBFOpcodes.def`,
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFVersionFeatures.def`, `SBFProtocolLimits.def`,
`SBFSyscalls.def`, `SBFSyscallMemory.def`, `SBFCPIABI.def`,
`SBFProgramInstructions.def` и
`SBFUpstreamSources.def`. Одноразовые диагностики и имена LLVM blocks остаются
локальными, как принято в самом LLVM.

`SBFProtocolLimits.def` фиксирует историческое значение 65 536 инструкций и
текущий предел account data в 10 MiB; NeverD выводит из последнего
консервативную границу декодирования.

В strict v3/v4 ограниченные program headers задают runtime-контракт; section и
symbol tables — лишь необязательное debug enrichment и не делают корректный
image недействительным при отсутствии или повреждении. Legacy v0-v2 объединяет
`.text`, `.rodata`, `.data.rel.ro` и `.eh_frame`; `R_BPF_64_64`,
`R_BPF_64_RELATIVE` и `R_BPF_64_32` применяются ровно один раз до заморозки
образа.

| Свидетельство | Проверенный результат |
|---------------|-----------------------|
| Официальный ELF manifest | 20/20 артефактов из `sbpf/tests/elfs` |
| ISA-матрица | все 256 encoding для v0-v4, то есть 1,280 ячеек, плюс границы verifier |
| Дифференциальное выполнение | raw-byte oracle против LLVM ORC, C11 и stable Rust с trace memory/fault/syscall |
| Интегрированный набор | 145/145 случаев в 14 тестовых бинарниках |
| ASan + UBSan | 141/141 core-случаев в 13 бинарниках без отчётов |

Аудит закреплён на Anza `sbpf`
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` и Agave
`cae40aa610fdbdb313209bc1eec737079eb59688`. Для обновления проверьте
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` и
`SBFUpstreamSources.def`, затем выполните:

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
  cmake --build build --target check-neverd-sbf
```

Сравнение показало: `sol-azy` падает на текущем strict ELF и оставляет
неопределённый legacy CFG-узел; `solana-data-reverser` работает с account data,
`SolDragon` помечает анализ как WIP, а `bn-ebpf-solana` требует Binary Ninja.
Поэтому официальные `sbpf` и Agave остаются семантическим эталоном.
