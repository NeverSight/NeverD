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
сообщает NeverD: каждый — со своим идентификатором runtime, feature-аккаунтом,
состояние которого фиксирует активацию, и слотом, на котором каждый кластер его
активировал. Pending-аккаунт может существовать, не включая гейт. Гейт, у
которого нет строки активации для кластера, там не активирован.
`simd-0321` включён на каждом кластере; `simd-0449` и syscall SHA-512 включены на
testnet и devnet и выключены на mainnet — именно поэтому программа, работающая на
devnet, отказывает на mainnet.

В закреплённой ревизии Agave гейт
`syscall_parameter_address_restrictions` (`simd-0459`) ужесточает контракт
VM-адресов и выравнивания параметров syscall и CPI; финализированное состояние
RPC фиксирует активацию на слотах 429,840,000 в mainnet, 407,468,256 в testnet и
462,240,000 в devnet. Гейт `account_data_direct_mapping` при скорректированном
адресном пространстве заменяет копию данных аккаунта во входном буфере на
регионы памяти с прямым backing; в mainnet он не активирован, а в testnet и
devnet активируется на слотах 408,332,256 и 463,968,000. Ни один гейт не создаёт
новый Account ABI и не меняет логические смещения полей ABIv0/ABIv1: выбор
сериализации остаётся за владеющим loader, а NeverD фиксирует оба гейта как
метаданные топологии runtime.

Биты feature остаются append-only. Наблюдаемый snapshot уже превысил 32 бита,
поэтому `RuntimeFeatureMask` — единственный тип `uint64_t` для хранения и host
ABI. `RuntimeFeatureDisposition` отличает действующий `RuntimeBranch` от
Ширина ABI v2 заморожена и не расширяется in-place; для полей свыше 64 бит добавляют v3 или multiword-представление, не меняя ширину v2.
`FoldedBranch`: его активная сторона безусловна в закреплённой ревизии, но
старая сторона всё ещё важна на исторических слотах. Финализированные данные
RPC об активации (`—` означает, что активации нет):

| gate | domain / disposition | mainnet | testnet | devnet |
|------|----------------------|---------|---------|--------|
| `disable_deploy_of_alloc_free_syscall` | `ProgramAdmission` / `FoldedBranch` | 209,088,008 | 195,356,264 | 224,208,000 |
| `enable_bpf_loader_set_authority_checked_ix` | `LoaderManagement` / `RuntimeBranch` | 251,424,000 | 247,628,260 | 255,744,000 |
| `remove_bpf_loader_incorrect_program_id` | `LoaderManagement` / `FoldedBranch` | 237,168,000 | 224,300,256 | 247,104,000 |
| `simplify_alt_bn128_syscall_error_codes` | `SyscallSemantics` / `FoldedBranch` | 274,320,000 | 278,300,256 | 308,448,000 |
| `abort_on_invalid_curve` | `SyscallSemantics` / `RuntimeBranch` | 311,904,000 | 300,764,256 | 342,576,000 |
| `deplete_cu_meter_on_vm_failure` | `VMFaultPolicy` / `RuntimeBranch` | 327,888,000 | 319,340,257 | 364,176,000 |
| `fix_alt_bn128_multiplication_input_length` | `SyscallSemantics` / `FoldedBranch` | 361,152,000 | 346,988,256 | 397,440,000 |
| `raise_cpi_nesting_limit_to_8` | `CPIExecution` / `RuntimeBranch` | — | — | — |
| `increase_cpi_account_info_limit` | `CPIExecution` / `FoldedBranch` | 403,056,000 | 385,868,256 | 435,456,000 |
| `poseidon_enforce_padding` | `SyscallSemantics` / `FoldedBranch` | 406,080,000 | 385,868,256 | 438,048,000 |
| `fix_alt_bn128_pairing_length_check` | `SyscallSemantics` / `FoldedBranch` | 406,944,000 | 385,868,256 | 438,480,000 |
| `alt_bn128_little_endian` | `SyscallSemantics` / `RuntimeBranch` | 425,088,000 | 406,604,256 | 456,192,000 |
| `enable_alt_bn128_g2_syscalls` | `SyscallSemantics` / `RuntimeBranch` | 425,520,000 | 406,604,256 | 457,056,000 |
| `loader_v3_minimum_extend_program_size` | `LoaderManagement` / `RuntimeBranch` | 432,864,000 | 416,540,256 | 470,880,000 |

Эта область намеренно не заявляет покрытие всего `FeatureSnapshot` Agave.
NeverD включает гейты loader, verifier, VM, entry/input, syscall и инфраструктуры
CPI только тогда, когда они напрямую меняют декодирование или выдаваемый host
contract. Планирование транзакций, fees, consensus, проверка precompile на уровне
транзакции и бизнес-семантика `CPI target built-in` принадлежат `external runtime`;
добавление их битов без реализации built-in заявило бы несуществующую возможность.

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
кластеров. `sol_alloc_free_` остаётся зарегистрированным для исполнения по обе
стороны границы. Deployment регистрировал его до
`disable_deploy_of_alloc_free_syscall`, а начиная со слота активации конкретного
кластера отвергает. В закреплённой ревизии Agave активная сторона deployment уже
встроена в построение реестра; NeverD сохраняет гейт, чтобы исторический профиль
получал ответ до активации.

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
| Адреса base58 в read-only данных | совпадение в `SBFKnownAddresses.def` и `SBFAnchorNamespaces.def` либо константа, которую создаёт код |
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

Восстановление scratch выполняется по требованию: fixed point scratch для Solana
CPI/PDA строится только при наличии настоящего `scratch consumer`; программы без него
пропускают `whole-CFG fixed point`. `SBFAnalysisLimits.def` задаёт хостовую
`analysis policy`, а не `protocol limits`: `MaxModeledScratchBytes` ограничивает
1,024 bytes на каждый `program point`, а `ScratchFlowRetainedByteBudget` — это
`logical retained estimate` в 8,388,608 bytes. При превышении бюджета восстановление
явно выполняет widening к `ScratchRecoveryPrecision::BlockLocal`. Теряются только
`cross-block must-facts`; `block-local replay` остаётся `sound` и может восстановить
`same-block stores`. Printer стабильно
выводит строку `recovery scratch-precision=block-local`; widening никогда не возвращает
`half-converged must-facts`.

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

Сохранять scratch может только разрешённый runtime syscall и лишь согласно его
проверенным окнам записи. Любой внутренний, косвенный или иначе неразрешённый
вызов очищает смоделированные байты, даже если ни один текущий аргумент не
указывает на scratch: ранее утёкший указатель или глобальный alias всё ещё может
дать вызываемому доступ на запись. `sol_invoke_signed_rust` и
`sol_invoke_signed_c` пишут данные аккаунтов, а не память вызывающей стороны, так
что две invocation, собранные в одном блоке, обе читаемы.

Модель — прямой must-анализ по внутрипроцедурному CFG: байт доживает до блока
только тогда, когда каждый путь до него записал одно и то же значение. Рёбра
вызовов не прослеживаются, потому что вызываемый ничего не наследует от frame
вызывающего. У worklist зависимостей нет снижения точности по числу блоков;
необязательный Release-gate проходит полный предел 10 MiB и `1,310,720` инструкций.

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
#include <stdint.h>

typedef enum neverd_sbf_status {
  NEVERD_SBF_OK = 0,
  NEVERD_SBF_INVALID_INSTRUCTION = 1,
  NEVERD_SBF_MEMORY_ACCESS = 2,
  NEVERD_SBF_DIVIDE_BY_ZERO = 3,
  NEVERD_SBF_DIVIDE_OVERFLOW = 4,
  NEVERD_SBF_CALL_DEPTH = 5,
  NEVERD_SBF_UNKNOWN_SYSCALL = 6,
  NEVERD_SBF_UNKNOWN_FUNCTION = 7,
  NEVERD_SBF_EXECUTION_OVERRUN = 8,
} neverd_sbf_status;
/* v2 is fixed-width: values 0..8 reuse the legacy constants above. */
typedef uint32_t neverd_sbf_status_v2;
enum {
  NEVERD_SBF_INVALID_REGISTER = 9,
  NEVERD_SBF_INVALID_BRANCH = 10,
};
typedef uint64_t neverd_sbf_runtime_feature_mask;
typedef struct neverd_sbf_runtime_features {
  neverd_sbf_runtime_feature_mask bits;
} neverd_sbf_runtime_features;

/* Generated feature constants have the form NEVERD_SBF_RUNTIME_FEATURE_<Name>. */
typedef struct neverd_sbf_syscall_invocation {
  uint32_t hash;
  uint64_t arguments[5];
  neverd_sbf_runtime_features runtime_features;
} neverd_sbf_syscall_invocation;

/* v1 is the exact legacy four-field ABI. */
/* All callback fields return int, including the v2 callback. */
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  /* Legacy syscall callback: hash, five arguments, output value. */
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;

/* The v1 entrypoint reads only the four fields above. */
neverd_sbf_status neverd_sbf_program(
    neverd_sbf_environment *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);

/* v2 is a distinct ABI: the old layout is embedded and never extended in place. */
typedef struct neverd_sbf_environment_v2 {
  neverd_sbf_environment base;
  /* NULL callback falls back to base.syscall. */
  int (*syscall_with_features)(
      void *, const neverd_sbf_syscall_invocation *, uint64_t *result);
  /* NULL selects the program snapshot; a pointer to zero is an explicit empty snapshot. */
  const neverd_sbf_runtime_features *runtime_features;
} neverd_sbf_environment_v2;

neverd_sbf_status_v2 neverd_sbf_program_v2(
    neverd_sbf_environment_v2 *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);
```

`width` задаётся в битах. Каждый сгенерированный C-callback возвращает `int`, включая
`syscall_with_features`. В v1 entrypoint `neverd_sbf_program` ноль означает успех;
любой ненулевой возврат `load` или `store` нормализуется к
`NEVERD_SBF_MEMORY_ACCESS`, а ненулевой возврат `syscall` — к
`NEVERD_SBF_UNKNOWN_SYSCALL`; контракты помечены как `v1-load-store-nonzero` и
`v1-syscall-nonzero`; v1 не передаёт точный status callback.
Внутренние ошибки `InvalidRegister` и `InvalidBranch` также нормализуются к
`NEVERD_SBF_INVALID_INSTRUCTION` (`internal-invalid-instruction`).
v2 entrypoint `neverd_sbf_program_v2` является путём точных status: распознанное
значение callback из `neverd_sbf_status_v2`, включая 9 и 10, сохраняется как
обработанная fault (`v2-exact-status`). v2 entrypoint также сохраняет внутренние `InvalidRegister` и
`InvalidBranch` как 9 и 10. Неизвестное значение callback использует
сгенерированный fallback конкретной операции (`operation-specific-fallback`). При null
`syscall_with_features` происходит fallback к `base.syscall`; этот callback также
возвращает `int` (`feature-aware-null-base-syscall`).
Struct и entrypoint v1 остаются совместимыми с legacy hosts. Отдельный v2 entrypoint
передаёт `syscall_with_features` и разрешённый runtime-feature snapshot. Генерируемый
код представляет registers, return PCs, callee-saved r6-r9, frame pointers, VM addresses,
division faults, wide PQR operations и wrapping shifts. Выводятся только реально нужные
helpers, поэтому минимальный output проходит `clang -Wall -Wextra -Werror`.

## Контракт сгенерированного Rust host

```rust
// The v1 source contract remains Result-based.
pub enum SbfError {
    InvalidInstruction, MemoryAccess, DivideByZero, DivideOverflow,
    CallDepth, UnknownSyscall, UnknownFunction, ExecutionOverrun,
}

#[repr(u32)]
#[non_exhaustive]
pub enum SbfErrorV2 {
    InvalidInstruction = 0, MemoryAccess = 1, DivideByZero = 2,
    DivideOverflow = 3, CallDepth = 4, UnknownSyscall = 5,
    UnknownFunction = 6, ExecutionOverrun = 7, InvalidRegister = 8,
    InvalidBranch = 9,
}

pub struct SbfRuntimeFeatures { bits: u64 }
impl SbfRuntimeFeatures {
    pub const fn from_bits(bits: u64) -> Self { Self { bits } }
    pub const fn bits(self) -> u64 { self.bits }
    pub const fn contains(self, feature: Self) -> bool {
        (self.bits & feature.bits) != 0
    }
}

pub struct SbfSyscallInvocation {
    pub hash: u32,
    pub args: [u64; 5],
    pub runtime_features: SbfRuntimeFeatures,
}

pub enum SbfSyscallOutcomeV2 {
    Unregistered,
    Returned(u64),
    Fault(SbfErrorV2),
}

pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}

pub trait SbfEnvironmentV2 {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfErrorV2>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfErrorV2>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfErrorV2> {
        let _ = (hash, args);
        Err(SbfErrorV2::UnknownSyscall)
    }
    fn syscall_outcome(&mut self, hash: u32, args: [u64; 5])
        -> SbfSyscallOutcomeV2 {
        match self.syscall(hash, args) {
            Ok(value) => SbfSyscallOutcomeV2::Returned(value),
            Err(SbfErrorV2::UnknownSyscall) => SbfSyscallOutcomeV2::Unregistered,
            Err(error) => SbfSyscallOutcomeV2::Fault(error),
        }
    }
    // Some(SbfRuntimeFeatures::from_bits(0)) is an explicit empty snapshot.
    fn runtime_features(&self) -> Option<SbfRuntimeFeatures> { None }
    fn syscall_with_features(
        &mut self, invocation: SbfSyscallInvocation
    ) -> SbfSyscallOutcomeV2 {
        self.syscall_outcome(invocation.hash, invocation.args)
    }
}

pub fn neverd_sbf_program<E: SbfEnvironment>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfError> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated program body")
}
pub fn neverd_sbf_program_v2<E: SbfEnvironmentV2>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfErrorV2> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated v2 program body")
}
```

Старый entrypoint `neverd_sbf_program` и `SbfEnvironment` образуют
`v1-result-abi`; методы host используют `Result`. Значение
`Some(SbfRuntimeFeatures::from_bits(0))` обозначает
`explicit-empty-snapshot` и отличается от `None`. `syscall_outcome` — это
`result-host-bridge` от Result-метода host к `SbfSyscallOutcomeV2`. Поскольку
`SbfErrorV2` помечен `#[non_exhaustive]`, вызывающая сторона обязана применять
`non-exhaustive-wildcard` (`_`) в match.

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
/* Optional: name Anchor handlers from the program's own IDL. */
neverd_sbf_set_idl(session, idl_json);
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

## Текущая база соответствия (2026-08-24)

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
| Официальный ELF manifest | 23/23 артефактов из `sbpf/tests/elfs` |
| Официальный oracle | `NeverDSBFExternalOracleTests` сравнивает 1,411 opcode/boundary-случаев с закреплённым verifier |
| Дифференциальное выполнение | raw-byte oracle против LLVM ORC, C11 и stable Rust с trace memory/fault/syscall |
| Интегрированный набор | `check-neverd-sbf` запускает все зарегистрированные suites; быстро меняющийся итог не фиксируется |
| ASan + UBSan | целевые тесты идут fail-fast без отчётов; быстро меняющийся итог не фиксируется |

Аудит закреплён на Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84` и Agave
`ef210d67f2fabeee1730498188fa78854260c679`. Для обновления проверьте
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` и
`SBFUpstreamSources.def`, затем выполните:

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

Сравнение показало: `sol-azy` падает на текущем strict ELF и оставляет
неопределённый legacy CFG-узел; `solana-data-reverser` работает с account data,
`SolDragon` помечает анализ как WIP, а `bn-ebpf-solana` требует Binary Ninja.
Поэтому официальные `sbpf` и Agave остаются семантическим эталоном.

## Проверяемый контракт свидетельств от 2026-08-24

`SBFUpstreamSources.def` закрепляет аудит на Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679` и Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`. Официальный manifest проходит
23/23; `NeverDSBFExternalOracleTests` сравнивает 1,411 opcode/boundary-случаев
с отдельно собранным официальным verifier через `SBFOfficialOracleProtocol.def`
и `SBFOfficialVerifierCases.def`, и `SBFOfficialExecutionConstants.def`. Повреждённые ELF задаются
`SBFOfficialELFMutations.def` и табличным corpus; быстро меняющийся итог намеренно
не фиксируется.
Отдельная `41-case дифференциальная проверка строгих ELF` прогоняет всю матрицу strict-v3
через официальный процесс `verify-elf-batch` и NeverD; эти 41 случай не входят
в итог 1,411 opcode/verifier.

Официальная дополнительная матрица выполнения (`additional execution matrix`)
отдельна: в ней ровно 508 активных случаев `(Version,Opcode)` и ещё 58
граничных, всего 566 точных случаев выполнения. Она не заменяет и не входит в
1,411 `verifier probes`, а также не заменяет и не входит в 41-case
дифференциальной проверки строгих ELF.

`NeverDSBFAgaveConformanceTests` также аутентифицирует ревизию Firedancer
test-vectors `68bb4af40235562e8852fa23d5727e49c2a0b862` и сверяет все 1,955 fixture
`sol_compat_elf_loader_v1` (1,399 приняты, 556 отклонены). Для каждого принятого
ELF дополнительно сравниваются `entry_pc`, `text_off`, `text_cnt`, `rodata_hash`
и `calldests_hash`. Этот gate намеренно проверяет только loader и не запускает
последующий verifier инструкций, сохраняя два этапа Agave раздельными.

Chain profile по умолчанию честно следует Agave: строки `SBF_RUNTIME_VERSION`
вычисляют максимальную ISA для исторического cluster/slot и продвигают её от
V0 к V1, V2 и V3 при активации официальных feature accounts; текущий максимум
остаётся V3. Это `RuntimeVersionPolicy::ChainProfile`. Только явный
`--sbf-version=v4` выбирает
`RuntimeVersionPolicy::UpstreamToolchain` для offline-анализа по закреплённому
`sbpf`, не утверждая, что v4 активирован on-chain. Текущий предел 10 MiB равен
ровно `10'485'760` байтам; 65,536 сохраняется лишь как исторический
provenance/test и не применяется при выполнении.

Типизированные `.def`-реестры задают features, syscalls, faults и source ABI:
`SBFSyscallRegistration.def`, `SBFValidationRules.def`, `SBFFaultCodes.def`,
`SBFSourceStatuses.def`, `SBFArgumentRegisters.def` и `SBFEdgeKinds.def`.
`SBFFaultCodes.def` задаёт стабильные значения execution fault, а
`SBFSourceStatuses.def` отдельно владеет ABI сгенерированного source. Loader
работает `raw-first`: сначала исправляет relative CALL, затем один раз применяет
raw relocations в порядке ELF ordinal. Стабильный порядок ошибок: text identity,
CALL, relocation, entrypoint, read-only layout. File/VM mapping gap-aware и не
создаёт байты внутри промежутков.

CFG и dataflow строятся на функцию: call edge не становится локальным
predecessor, shared tail остаётся неоднозначным, а все latch одной петли образуют
единую multi-latch region. Worklist и ownership проверяются fixture на 10,000
функций, обратных блоков и conditional latch без машинно-зависимого времени.

Публичный SBF call graph работает как `callgraph-budget=fail-closed`:
типизированные лимиты input, provenance, node, edge, element и
`CallGraphOutputByteBudget` делают JSON точным или пустым. При исчерпании
возвращается `{"nodes":[],"edges":[]}`, задаётся `neverd_last_error()`,
а частичная relation никогда не публикуется.

Каждая строка активации хранит cluster, feature account и slot, поэтому возможен
`RPC activation audit` против live node при обычном offline-анализе. Сравнение
включает Blueshift, `qedsvm` (Lean-доказательства выбранных путей, но текущий ELF
loader принимает только V0), `leanprover-solanalib`, `sol-azy`,
`bn-ebpf-solana` и Ghidra/SolDragon. `ezBPF` в
`88829078a6d7682a2baed0d696d500401c263750` прямо называет себя deprecated и
направляет к Blueshift; это архивный предшественник с единственной таблицей
byte-to-enum, а не version-aware decoder для moved-memory, JMP32 и нынешней
матрицы v0-v4. Сравнительные pins фиксируют Blueshift на
`704e40f7aa82446555b19d9ffbc0a6e18a35480f`, `qedsvm` на
`99bd5ede85374adc7fc5c835c2432ecf4e123fd1`, `leanprover-solanalib` на
`6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec`, а четыре локальных инструмента —
`sol-azy` `362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`, `solana-data-reverser`
`bf90923adec984a61ca0437e9d341360ac1b11ee`, `SolDragon`
`002b98677a5e595a773af6607b77210f5ea71db7` и `bn-ebpf-solana`
`c3fe0de45d37eb68dcb08f2498c6e1f986056572`. Среди проверенных публичных
универсальных SBF-декомпиляторов NeverD имеет самое сильное найденное нами
воспроизводимое свидетельство; это ограниченное сравнение, а не абсолютное
утверждение о «первом месте в мире».

В публичный аудит добавлены `r2ghidra-solana` на pin
`eca0b8e2d307e00991e289b8f9b0f45743819f1b` с Ghidra C-like UX и
`C-like-pdg`, а также представлениями account/Anchor/string/syscall; CI на этом
HEAD прошло, но Solana-specific testsuite закомментирован, а CI smoke всего лишь
декомпилирует `/bin/ls`. Прямой reproducer подтверждает: официальный
V0-`relative_call_sbpfv0.so` выдаёт разумный C, тогда как официальный
V3-`relative_call.so` завершается ошибкой в `pdg`; результат воспроизводим.
`radare2-solana` закреплён на
`292d845681be377cadc9959a74c2cadeb6e7f412` и расширяет V2-only SIMD-0173/0174 как
`>=V2` до V3/V4, тогда как официальный `program.rs` помечает их только V2.
`SBPF-3-1` закреплён на `0e602c93007faa96bccb8e1e12040954ff108b6f`; его 2/2
тривиальных cargo-теста проходят без CI, version detection остаётся placeholder,
возвращающим none/V0, high-nibble opcode decoder ошибается, а jump использует imm,
а не off. V0/V3 relative_call ELF дают один и тот же неверный pseudocode. Преимущество
NeverD — официальные воспроизводимые V0–V4 свидетельства loader/verifier/runtime/
process-oracle; это не отрицает UX и C output этих инструментов.

`SBFComparisonTools.def` — единственный источник отображаемых имён и полных
revision сравниваемых инструментов. Финальный ограниченный публичный sweep также
показал следующее:

- `blastrock/Solana-eBPF-for-Ghidra` закреплён на
  `c3ad719004726fe924dbed901eca2744ad82c85d` и даёт настоящую Ghidra P-code UX,
  но единственная SLEIGH-модель без версий фиксирует CALLX на `dst` и смешивает
  legacy/current opcodes. Реальных тестов и CI нет, а в default source отсутствует
  класс relocation constants, на который есть ссылки.
- `SolEmu-Ghidra` закреплён на `6520af2ff104d5adbec24632ba3afa3bef0da529`,
  наследует тот же decoder и добавляет emulator UX вокруг явно simulated или
  placeholder CPI-, crypto- и ZK-поведения; реальных тестов и CI также нет.
  `Ghidra_sBPF` закреплён на `907bd4476432ca83bb2352686ad1ccafdb38504c`,
  позволяет вручную выбрать v1-v3, но накопительно включает V2-only encodings в
  V3, не имеет авто-выбора V0/V4, тестов и CI.
- `solana-ebpf-ida-processor` закреплён на
  `aacd215907266190ed9c6c1b408ca9309f92ecdd`: это полезный IDA disassembler/
  relocation UI, но не source lifter; смешанная таблица всегда читает CALLX из
  `imm`, тестов и CI нет. `solana-bpf-reverse` закреплён на
  `39479a3bddb8cb866ee499266a76a1b54069b222`, строит эвристические отчёты и
  Rust TODO scaffold из hard-coded layout; запуск дал 9 pass, 2 fail и 1 skip,
  без CI.
- `solens` закреплён на `22defa1c8f4118dacd42f5c291f1ac31609fc0e5`: V2-only
  terminal disassembler, 0 тестов и без CI. `sbpf-decompiler` закреплён на
  `37b8bc0edc7ce347abee466f5f974e900c1948df`; текущая реализация — три строки
  `Hello, world!`, 0 тестов и без CI.
- `sbpf-eye` закреплён на `5277a52aeb58e50b6ff8f9020414334765369b49` и прямо
  описан как lightweight WIP instruction/CFG TUI: 3 теста проходят, но нет
  semantic IR, source emitter или CI. `svm_bytecode_analyzer` закреплён на
  `12aa236db8964e6be661e38131c2dc81588cf19c`: это disassembler/CFG analyzer,
  не lifter; bytes register/offset декодируются неверно, запуск дал 17 pass и
  1 fail, без CI.
- `giraffexiu/Solana-eBPF-for-Ghidra` закреплён на
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07`: one-commit snapshot той же
  Ghidra-линии без новой version semantics, тестов или CI. `CertSBF` закреплён на
  `bb93a97cf0c64d119d08ec851e8e820315beb59e`: ценная Isabelle/HOL-формализация
  старой rBPF semantics, а не текущий whole-program V0-V4 source decompiler.

Эти результаты усиливают только сравнительное свидетельство в ограниченном
публичном snapshot и не являются абсолютным выводом о будущих или private tools.

Финальный RPC-аудит 2026-08-24 дал точное совпадение: 38 feature accounts и 89
activation rows; mainnet slot 441305159, testnet 433055669, devnet 487238699.
Пустой pending account, принадлежащий системе (`VirtualAddressSpaceAdjustments` на
mainnet), не был активирован. RPC URL в документации не фиксируется.

Linux Release CI читает точные pins через `--print-pinned-revision`,
`--print-test-vectors-revision` и `--print-toolchain`, аутентифицирует oracle и
sparse corpus и экспортирует `NEVERD_SBPF_ORACLE` и
`NEVERD_AGAVE_CONFORMANCE_ROOT`, поэтому оба внешних теста обязательны. Обычный
локальный запуск без явного oracle/corpus env обнаруживает случаи, но может их
пропустить.
