**Sprachen**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Solana-SBF-Dekompilation

[← Dokumentationsindex](README.de.md)

NeverD lädt Solana-Deploy-Artefakte als SBF-Programme erster Klasse und stellt
den gesamten Pfad über CLI und `libneverd` bereit:

```text
SBF ELF
  → versionsbewusster ELF-Loader und Verifier
  → verlustfreies LowIR + CFG
  → normalisiertes MedIR + Register-Fakten
  → Funktionen, Syscalls, CPI-/Account-Beobachtungen und Regionen
       ├─ verifiziertes LLVM IR
       ├─ portables C11
       └─ sicheres stabiles Rust
```

Die Implementierung folgt der aktuellen Anza-`sbpf`-VM statt generischem Linux
eBPF. Version-, Opcode-, Syscall-, Relocation- und Protokoll-Metadata liegen in
`.def`-Datenbanken unter `include/neverd/sbf/`; Loader und Backends verwenden
generierte typisierte Tabellen ohne Encodings oder Schreibweisen zu duplizieren.

## Unterstützte Eingabe und VM-Versionen

Eingabe ist ein ELF64-Little-Endian-Solana-Programm (`.so`).

| SBF | ELF-Layout | Machine ID | Wichtiges ISA-Verhalten | Status |
|-----|------------|------------|-------------------------|--------|
| v0 | Legacy-Sections/Relocations | `EM_BPF`, `EM_SBPF` | feste Frames mit virtuellen Lücken, LDDW, Legacy-Memory-Opcodes | legacy |
| v1 | Legacy-Sections/Relocations | `EM_BPF`, `EM_SBPF` | manuell angepasste Stack-Frames | legacy |
| v2 | Legacy-Sections/Relocations | `EM_BPF`, `EM_SBPF` | PQR-Arithmetik, verschobene Memory-Encodings, vertauschte Immediate-Subtraktion, Source-Register-CALLX | legacy, nicht monoton |
| v3 | strikte Program Headers, keine dynamischen Relocations | `EM_BPF` | statische Syscalls/Calls, JMP32, Destination-Register-CALLX, Bytecode bei `0x100000000`, Rodata bei Null | aktuelles Deploy-Toolchain-Format |
| v4 | strikte Program Headers, keine dynamischen Relocations | `EM_BPF` | v3-ISA plus aligned Memory-Mapping-Vertrag | aktuelles Upstream-`sbpf`; Cluster variieren |

v2-Änderungen gelten bewusst nicht für v3. Feature-Checks sind explizit, keine
`version >= N`-Vermutungen. Strict ist Standard und lehnt fehlerhafte Header,
Ranges, Alignments, unsupported writable Legacy-Sections, ungültige
Continuations/Register/Frame-Pointer-Writes/Branches und inaktive Opcodes mit
Instruction Slot und virtueller Adresse ab.

Das aktuelle Solana-Toolchain nutzt `cargo build-sbf`. Moderne v3+-Programme
sind Rust-orientiert, das Upstream-C-Toolchain zielt nicht auf v3. NeverDs
Ausgaben bleiben davon unberührt: Jede akzeptierte Eingabe kann C oder Rust werden.

- [Solana-Programme](https://solana.com/docs/core/programs)
- [Programmausführung](https://solana.com/docs/core/programs/program-execution)
- [Syscall-Referenz](https://solana.com/docs/core/programs/syscall-reference)
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

`--sbf-version=auto|v0|v1|v2|v3|v4` ändert Semantik erst nach Prüfung des
erkannten ELF-Layouts. Es dient beschädigten oder Forschungs-Fixtures, nicht der
Umdeutung einer nicht vertrauenswürdigen Datei als anderer Packaging-Standard.

## Analyse und Rekonstruktion

LowIR behält Acht-Byte-Encoding, Raw Fields, LDDW-Continuations, aufgelöste
Calls, Syscall-Hashes, Blöcke, Kanten, Erreichbarkeit und Diagnosen. MedIR
normalisiert versionsabhängige Encodings zu typisierten 32/64-Bit-Operationen,
expliziten Extensions, geschützter Arithmetik, Memory-Breiten und Call-Arten.
Register-Dataflow verfolgt Konstanten sowie Stack-/Rodata-Adressen.

HighIR rekonstruiert Entry-/interne Funktionen, direkte Kanten, offizielle
Syscall-Namen, Strings, natürliche Loops, reduzierbare Conditionals und
konservative Solana-Beobachtungen. `sol_invoke_signed_rust` und
`sol_invoke_signed_c` sind CPI; auf dem Input-Register basierende Memory-Zugriffe
sind Account/Input. Ohne IDL werden keine Anchor-Typen oder Layouts erfunden.

C und Rust teilen einen backend-neutralen Structuring-Pass. Bei eindeutiger
reduzierbarer Darstellung entstehen `if`/`if-else` und `while`/`loop`; interne
Calls, CALLX und irreduzibler Flow behalten den exakten PC-Dispatcher.

Die Syscall-Datenbank umfasst Logging, Memory, PDA, SHA-256/Keccak/Blake3,
Poseidon, secp256k1, Kurven/alt-bn128, große modulare Potenzen, CPI, Return Data,
Sibling Instructions, Compute Units und aktuelle Sysvars einschließlich Epoch
Rewards. `R_BPF_64_64`, `R_BPF_64_RELATIVE`, `R_BPF_64_32` werden zentral
verarbeitet. Text-Relocations inklusive beider LDDW-Hälften und offiziellem
Murmur3-CALL-Key gelten vor dem Decode. Bei bereits angewandtem/gestripptem
`R_BPF_64_32` wird der Registry-Key aus Symbolen und Target Slots rekonstruiert.

## Vertrag der erzeugten LLVM-Runtime

LLVM behandelt VM-Adressen nie als Host-Pointer. Geprüfte Load/Store/Syscall-
Deklarationen liefern `i32`-Status; Load/Syscall schreiben `i64` über Output-
Pointer. Nonzero springt in einen expliziten SBF-Fault-Block. Das Module besteht
`llvm::verifyModule` vor der Ausgabe.

## Vertrag des erzeugten C-Hosts

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

`width` ist in Bits; ein Host-Return ungleich Null wird expliziter SBF-Status.
Register, Return-PC, erhaltene r6-r9, Frame Pointer, VM-Adressen, Division-Faults,
weite PQR-Operationen und Wrapping-Shifts sind abgebildet. Nur verwendete Helpers
werden emittiert, sodass `clang -Wall -Wextra -Werror` besteht.

## Vertrag des erzeugten Rust-Hosts

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

Die Ausgabe ist sicheres stabiles Rust ohne Raw Pointer. Der Entry Point ist über
das Trait generisch und verwendet sichere Arrays fester Größe. Tests kompilieren
mit `rustc --edition=2021 -D warnings`.

## C-API

Nach SBF-Load bleiben Session-Funktionen, Disassembly, IR-Dumps, CFG/Call-Graph-
JSON, Sections, Symbole, Relocations, Strings und Headers verfügbar. Rust wird
über den ABI-stabil ergänzten Output-Language-Enum gewählt.

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

## Verifikation und Grenzen

`unittests/sbf/` deckt Metadata-Invarianten, v0-v4-Loader, Strict-Verifier,
CFG/Rekonstruktion, verifiziertes LLVM, warnungsfreie C/Rust-Kompilation,
einen von MedIR unabhängigen Raw-Interpreter und die C-API ab. Eine Conditional-
plus-Loop-Fixture läuft in beiden Sprachen gegen den Raw-Oracle; das offizielle
`sbpf`-ELF-Corpus dient lokal ohne Drittanbieter-Binaries im Repository.

- SBF-Rewriting und Object-Code-Roundtrip werden ausdrücklich abgelehnt.
- Anchor IDL-/Typ-Rekonstruktion und Live-RPC/Accounts gehören nicht zum Loader.
- Syscalls und VM-Memory des Outputs laufen über einen Host-Vertrag, nicht eine
  eigenständige Solana-Runtime.
- Relaxed dient der Inspektion; ungültige Instruktionen erhalten keine geratene Semantik.
