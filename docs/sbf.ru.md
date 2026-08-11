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

Изменения v2 намеренно не переходят в v3. Feature checks явны, это не догадки
`version >= N`. Strict по умолчанию отвергает malformed headers/ranges/alignments,
unsupported writable legacy sections, invalid continuations/registers/
frame-pointer writes/branches и неактивные opcodes, указывая slot и virtual address.

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
```

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
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFSyscalls.def` и
`SBFUpstreamSources.def`. Одноразовые диагностики и имена LLVM blocks остаются
локальными, как принято в самом LLVM.

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
| Интегрированный набор | 104/104 случаев в 13 тестовых бинарниках |
| ASan + UBSan | 101/101 core-случаев в 12 бинарниках без отчётов |

Аудит закреплён на Anza `sbpf`
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` и Agave
`cae40aa610fdbdb313209bc1eec737079eb59688`. Для обновления проверьте
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` и
`SBFUpstreamSources.def`, затем выполните:

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

Сравнение показало: `sol-azy` падает на текущем strict ELF и оставляет
неопределённый legacy CFG-узел; `solana-data-reverser` работает с account data,
`SolDragon` помечает анализ как WIP, а `bn-ebpf-solana` требует Binary Ninja.
Поэтому официальные `sbpf` и Agave остаются семантическим эталоном.
