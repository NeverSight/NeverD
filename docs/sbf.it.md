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

Un numero di versione non è di per sé una specifica, perciò
`SBFVersionFeatures.def` contiene i cambiamenti di comportamento e la tabella
delle versioni li compone. Ogni record porta la proposta SIMD che ha accettato
il cambiamento e il predicato che `anza-xyz/sbpf` espone per la stessa domanda,
perché più proposte atterrano in una versione e una proposta cambia più cose
scorrelate: SIMD-0173 sposta le classi di istruzioni di memoria e ritira
`lddw`, mentre SIMD-0174 aggiunge indipendentemente la classe PQR nella stessa
versione. Registrare la proposta sulla feature anziché sulla versione è ciò che
mantiene una versione recuperata tracciabile fino al documento che l’ha decisa,
ed è il motivo per cui le due regole `callx` sono feature separate: SIMD-0173
legge il registro sorgente e SIMD-0377 quello di destinazione.

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

## Recupero del programma Solana

Sopra il modello macchina SBF, NeverD riporta che cosa significa un programma in
quanto programma Solana. Ogni fatto registrato porta la prova che lo ha prodotto,
e ciò che i byte non decidono resta non impostato invece che indovinato.

| Recuperato | Prova |
|------------|-------|
| Indirizzi base58 nei dati read-only | corrispondenza in `SBFKnownAddresses.def`, oppure una costante che il codice materializza |
| L'indirizzo dichiarato del programma | un `sol_memcmp_` di esattamente una lunghezza di chiave contro una costante read-only |
| Dispatch delle istruzioni Anchor | un confronto a 64 bit la cui costante eguaglia un discriminator SHA-256 con namespace |
| Target CPI | il record instruction raggiungibile dall'argomento dell'invoke |
| L'operazione che una chiamata seleziona | un selettore elencato in `SBFProgramInstructions.def`, oppure un discriminator Anchor iniziale |
| Seed di un indirizzo derivato | l'array di descrittori di seed raggiungibile dall'argomento di derivazione |
| Letture e scritture dei campi account | un load o store il cui indirizzo cade dimostrabilmente nell'input serializzato |

Il loader passa un solo argomento, il buffer di input serializzato alla base
della regione di input, quindi la propagazione di costanti da quello stato
iniziale dà campi account con nome invece di offset grezzi.
`SBFAccountLayout.def` contiene la serializzazione ufficiale; i suoi campi fissi
sono verificati come copertura esatta della loro estensione.

Anchor deriva un discriminator applicando SHA-256 a `<namespace>:<name>` e
tenendo i primi otto byte, operazione a senso unico. NeverD quindi conferma solo
candidati: `SBFAnchorNames.def` è un dizionario di nomi ricorrenti e `--sbf-idl`
fornisce l'IDL del programma, che ha precedenza. Un confronto a 64 bit viene
chiamato discriminator solo quando almeno uno di essi si risolve in un nome.

`SBFKnownAddresses.def` registra indirizzi di protocollo e di programmi canonici.
Ogni voce deve decodificare in esattamente 32 byte, cosa che la suite di test
impone. Il recupero richiede anche l'ABI delle syscall: SBPFv3 mappa i dati
read-only all'indirizzo zero, perciò un argomento di lunghezza e un indirizzo
dati basso sono lo stesso numero. `SBFSyscalls.def` registra quindi quali
registri argomento portano un indirizzo VM, e solo quelli vengono seguiti.

Le due syscall di invocazione descrivono la stessa instruction con due strutture
diverse, e `SBFCPIABI.def` conserva entrambi i layout, indicizzati dalla syscall
che li seleziona. Leggerne uno con gli offset dell'altro non fallisce: riporta in
silenzio il primo account come programma invocato. `SBFProgramInstructions.def`
nomina poi l'operazione richiesta a un programma canonico a partire dal selettore
che la sua interfaccia pubblica: un indice di variante bincode per i programmi
system, stake, lookup-table e upgradeable-loader, e un byte iniziale per i
programmi token, incluso l'intervallo di estensioni di Token-2022 sopra la
numerazione condivisa con il programma token originale. Un selettore non elencato
viene riportato come numero.

### Memoria di lavoro e finestre delle syscall

Un programma non consegna quasi mai una costante al runtime. Assembla un array di
seed, una instruction serializzata e il suo payload nel proprio frame o
nell'heap, e passa un puntatore. Leggere solo l'immagine caricata mostrerebbe il
puntatore e nulla di ciò che indirizza; il recupero mantiene quindi un modello
byte per byte della memoria che solo questo programma può scrivere, limitato da
`kMaxModeledScratchBytes`.

Due fatti decidono cosa sopravvive a una chiamata. `SBFSyscalls.def` dice quali
registri argomento portano un indirizzo VM; `SBFSyscallMemory.def` dice cosa fa
il runtime attraverso di essi, come lettura o scrittura con estensione `Fixed`,
`Counted` oppure `Opaque`. Una syscall senza finestra di scrittura non può
cambiare alcun byte del chiamante, quindi tutto ciò che era provato prima di
`sol_log_` lo è ancora dopo. Una scrittura limitata da un argomento di lunghezza
invalida esattamente quella finestra. Una scrittura `Opaque` invalida il proprio
indirizzo base e tutto ciò che sta sopra, perché un buffer non si estende mai
sotto il proprio inizio né oltre il confine di una regione VM. Il riepilogo degli
effetti in `SBFSyscalls.def` e la tabella delle finestre sono verificati l'uno
contro l'altro in entrambe le direzioni, così nessuno dei due può divergere da
solo.

`sol_memcpy_`, `sol_memmove_` e `sol_memset_` vengono seguite e non solo
invalidate: con destinazione, lunghezza e sorgente provate, i byte di
destinazione diventano noti. È questo che recupera l'operazione invocata da un
programma Anchor, dato che il suo payload viene copiato in posizione anziché
mappato.

Una chiamata a una funzione che questa analisi non ha descritto si assume scriva
ovunque possa arrivare. Il chiamato gira in un frame proprio, quindi una chiamata
i cui registri argomento dimostrabilmente non indirizzano memoria di lavoro
lascia il modello intatto; qualsiasi altra cosa lo scarta.
`sol_invoke_signed_rust` e `sol_invoke_signed_c` scrivono dati di account e non
la memoria del chiamante, così due invocazioni assemblate in uno stesso blocco
restano entrambe leggibili.

Il modello è un'analisi «must» in avanti sul CFG intraprocedurale: un byte
sopravvive fino a un blocco solo quando ogni percorso che lo raggiunge ha scritto
lo stesso valore. Gli archi di chiamata non vengono seguiti, perché un chiamato
non eredita nulla dal frame del chiamante. I programmi con più di
`kMaxScratchFlowBlocks` blocchi mantengono il recupero per blocco e perdono solo
i fatti che attraversano un confine di blocco.

`SBFLints.def` cataloga osservazioni sull'intero programma: controllo signer o
owner mancante, target di invocazione non costante, syscall deprecata o dietro
feature gate, e una versione SBPF che SIMD-0500 smetterà di accettare per il
deployment. Ognuna porta severità e confidenza, e nessun lint cambia la semantica
decodificata. Nulla in questo livello contatta la rete.

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

## Baseline di conformità corrente (2026-08-10)

Dopo le relocation, un solo `ProgramImage` immutabile e indirizzato dalla VM è
la fonte di verità condivisa da decoder, interpreter, recupero string e backend
LLVM/C/Rust. Non esistono copie separate di text o rodata che possano divergere
dalla semantica del loader.

I record chiusi risiedono in `SBFVersions.def`, `SBFOpcodes.def`,
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFVersionFeatures.def`, `SBFProtocolLimits.def`,
`SBFSyscalls.def`, `SBFSyscallMemory.def`, `SBFCPIABI.def`,
`SBFProgramInstructions.def` e
`SBFUpstreamSources.def`. Diagnostica e nomi di blocchi LLVM usati una sola volta
restano locali, seguendo la convenzione effettiva di LLVM.

`SBFProtocolLimits.def` registra il valore storico di 65.536 istruzioni e il
limite corrente di 10 MiB per gli account data; NeverD deriva da quest'ultimo
il limite conservativo di decodifica.

In strict v3/v4 i program header con limiti verificati formano il contratto
runtime; section e symbol table sono debug enrichment opzionale e non invalidano
un’immagine valida se mancanti o corrotte. Legacy v0-v2 unisce `.text`,
`.rodata`, `.data.rel.ro` e `.eh_frame`; `R_BPF_64_64`,
`R_BPF_64_RELATIVE` e `R_BPF_64_32` vengono applicate esattamente una volta
prima che l’immagine diventi immutabile.

| Evidenza | Risultato verificato |
|----------|----------------------|
| Manifest ELF ufficiale | 20/20 artefatti da `sbpf/tests/elfs` |
| Matrice ISA | tutti i 256 encoding per v0-v4, cioè 1,280 celle, più i limiti del verifier |
| Esecuzione differenziale | oracle raw-byte contro LLVM ORC, C11 e Rust stable, incluse trace memory/fault/syscall |
| Aggregato integrato | 145/145 casi in 14 binari di test |
| ASan + UBSan | 141/141 casi core in 13 binari senza report |

L’audit fissa Anza `sbpf` a
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` e Agave a
`cae40aa610fdbdb313209bc1eec737079eb59688`. Per aggiornarlo, rivedere
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` e
`SBFUpstreamSources.def`, quindi eseguire:

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

Il confronto mostra che `sol-azy` va in crash sull’ELF strict corrente e lascia
un nodo CFG legacy indefinito; `solana-data-reverser` tratta account data,
`SolDragon` indica l’analisi come WIP e `bn-ebpf-solana` richiede Binary Ninja.
Gli `sbpf` e Agave ufficiali restano quindi l’autorità semantica.
