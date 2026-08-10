**Lingue**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Decompilazione Solana SBF

[← Indice della documentazione](README.it.md)

NeverD carica gli artefatti deploy Solana come programmi SBF di prima classe ed
espone l’intero percorso tramite CLI e `libneverd`:

```text
SBF ELF
  → loader ELF e verifier version-aware
  → LowIR lossless + CFG
  → MedIR normalizzato + fatti sui registri
  → funzioni, syscall, osservazioni CPI/account e regioni recuperate
       ├─ LLVM IR verificato
       ├─ C11 portabile
       └─ Rust stable sicuro
```

L’implementazione segue la VM Anza `sbpf` corrente, non eBPF Linux generico.
I metadata di version, opcode, syscall, relocation e protocollo vivono nei
database `.def` sotto `include/neverd/sbf/`; loader e backend consumano tabelle
tipizzate generate senza duplicare encoding o nomi.

## Input e versioni VM supportati

L’input è un programma Solana ELF64 little-endian (`.so`).

| SBF | Layout ELF | Machine ID | Comportamento ISA rilevante | Stato |
|-----|------------|------------|-----------------------------|-------|
| v0 | section/relocation legacy | `EM_BPF`, `EM_SBPF` | frame fissi con gap virtuali, LDDW, memory opcode legacy | legacy |
| v1 | section/relocation legacy | `EM_BPF`, `EM_SBPF` | stack frame regolati manualmente | legacy |
| v2 | section/relocation legacy | `EM_BPF`, `EM_SBPF` | aritmetica PQR, encoding memory spostati, sottrazione immediate scambiata, CALLX da source register | legacy, non monotono |
| v3 | program header strict, nessuna relocation dinamica | `EM_BPF` | syscall/call statici, JMP32, CALLX da destination register, bytecode a `0x100000000`, rodata a zero | formato toolchain deploy corrente |
| v4 | program header strict, nessuna relocation dinamica | `EM_BPF` | ISA v3 e contratto memory mapping allineato | upstream `sbpf` corrente; disponibilità cluster variabile |

I cambiamenti v2 non passano intenzionalmente a v3. I feature check sono
espliciti, non ipotesi `version >= N`. Strict, predefinito, rifiuta header, range
o alignment malformati, section legacy writable non supportate, continuation,
register, frame-pointer write o branch invalidi e opcode inattivi, indicando
instruction slot e virtual address.

Il toolchain Solana corrente usa `cargo build-sbf`. I programmi v3+ sono
orientati a Rust e il toolchain C upstream non targetta v3; NeverD non è limitato:
ogni input accettato può essere emesso come C o Rust.

- [Programmi Solana](https://solana.com/docs/core/programs)
- [Esecuzione](https://solana.com/docs/core/programs/program-execution)
- [Riferimento syscall](https://solana.com/docs/core/programs/syscall-reference)
- [VM Anza sbpf](https://github.com/anza-xyz/sbpf)
- [Changelog Agave](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

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

`--sbf-version=auto|v0|v1|v2|v3|v4` cambia semantics solo dopo la verifica del
layout rilevato. Serve per fixture danneggiate o di ricerca, non per reinterpretare
un file non affidabile con un altro packaging standard.

## Analisi e recovery

LowIR conserva encoding da otto byte, raw field, continuation LDDW, call risolti,
hash syscall, block, edge, reachability e diagnostics. MedIR normalizza encoding
versionati in operation tipizzate 32/64 bit, extension esplicite, aritmetica
protetta, memory width e call kind. Il register dataflow segue costanti e address
stack/rodata.

HighIR recupera function entry/internal, direct call edge, nomi syscall ufficiali,
string, natural loop, conditional reducibili e osservazioni Solana conservative.
`sol_invoke_signed_rust`/`sol_invoke_signed_c` sono CPI; memory basata sull’input
register è account/input access. Non inventa type Anchor o account layout senza IDL.

C e Rust condividono un structuring pass backend-neutral. Emette `if`/`if-else`
e `while`/`loop` quando esiste una rappresentazione reducible unica; internal
call, CALLX e flow irreducibile mantengono l’esatto PC dispatcher.

Il database syscall copre logging, memory, PDA, SHA-256/Keccak/Blake3, Poseidon,
secp256k1, curve/alt-bn128, big modular exponentiation, CPI, return data, sibling
instruction, compute unit e sysvar come epoch rewards. Le relocation
`R_BPF_64_64`, `R_BPF_64_RELATIVE`, `R_BPF_64_32` sono centralizzate. Text
relocation, entrambe le metà LDDW e la chiave CALL Murmur3 ufficiale si applicano
prima del decode. Se `R_BPF_64_32` è già applicata e stripped, la registry key
viene ricalcolata da symbol e target slot per recuperare internal call.

## Contratto runtime LLVM generato

LLVM non tratta mai VM address come host pointer. Le declaration checked
load/store/syscall restituiscono status `i32`; load/syscall scrivono `i64` tramite
output pointer. Ogni status nonzero salta a un SBF fault block esplicito. Il
module supera `llvm::verifyModule` prima dell’uscita.

## Contratto host C generato

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

`width` è in bit; un host return nonzero è uno status SBF esplicito. Sono
rappresentati register, return PC, r6-r9 preservati, frame pointer, VM address,
division fault, PQR wide e wrapping shift. Sono emessi solo helper usati, quindi
l’output supera `clang -Wall -Wextra -Werror`.

## Contratto host Rust generato

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

L’output è Rust stable sicuro senza raw pointer. L’entry point è generico sul
trait e usa array safe a dimensione fissa. I test compilano con
`rustc --edition=2021 -D warnings`.

## C API

Dopo il load SBF restano disponibili function, disassembly, dump IR, CFG/call
graph JSON, section, symbol, relocation, string e header. Rust viene scelto con
il valore output-language aggiunto mantenendo stabile l’ABI.

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

## Verifica e limiti

`unittests/sbf/` copre invarianti metadata, loader v0-v4, verifier strict,
CFG/recovery, LLVM verificato, compilazione C/Rust warning-free, interpreter raw
indipendente da MedIR e C API. Una fixture conditional+loop gira in entrambe le
lingue contro l’oracle raw; il corpus ELF `sbpf` ufficiale è usato localmente
senza includere binari di terzi.

- SBF rewriting e object-code roundtrip sono rifiutati esplicitamente.
- Anchor IDL/type recovery e RPC/account live sono fuori dal loader.
- Syscall e VM memory dell’output passano da un host contract, non da un runtime autonomo.
- Relaxed serve all’ispezione e non assegna semantics ipotetiche.
