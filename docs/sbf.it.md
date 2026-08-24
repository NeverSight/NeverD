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

## Il runtime di cui parla una descrizione

La versione ISA viene dal file. Quasi nient’altro. Quali syscall si risolvano
dipende dalla chain e dallo slot; a quali byte si trovi un campo account dipende
dal loader che possiede il programma; se l’entrypoint riceva un secondo
argomento dipende da un interruttore che aziona la chain; e se un programma
possa essere distribuito è una domanda diversa dal se venga eseguito. Un unico
selettore di versione non può esprimere niente di tutto questo, perciò questi
sono assi separati con tabelle separate.

`SBFRuntimeFeatures.def` registra cluster, finalità e i gate che cambiano ciò
che NeverD riporta, ciascuno con l’identificatore runtime, il feature account il
cui stato registra l’attivazione e lo slot in cui ogni cluster lo ha attivato.
Un account pending può esistere senza attivare il gate. Un gate senza
riga per un cluster non è stato attivato lì. `simd-0321` è attivo su ogni
cluster; `simd-0449` e la syscall SHA-512 sono attivi su testnet e devnet e
spenti su mainnet, ed è esattamente per questo che un programma che funziona su
devnet fallisce su mainnet.

Nella revisione Agave fissata, il gate
`syscall_parameter_address_restrictions` (`simd-0459`) rende più rigido il
contratto per indirizzi VM e allineamento dei parametri syscall e CPI; lo stato
RPC finalizzato registra l’attivazione agli slot 429,840,000 su mainnet,
407,468,256 su testnet e 462,240,000 su devnet. Il gate
`account_data_direct_mapping` sostituisce la copia dei dati account nel buffer
di input con regioni di memoria direttamente supportate quando è in uso lo
spazio di indirizzi corretto; non è attivo su mainnet e si attiva agli slot
408,332,256 su testnet e 463,968,000 su devnet. Nessuno dei due gate crea un
nuovo Account ABI o modifica gli offset logici ABIv0/ABIv1: il loader
proprietario continua a scegliere la serializzazione e NeverD li registra come
metadati della topologia runtime.

I bit delle feature restano append-only. Poiché lo snapshot osservabile supera
ormai 32 bit, `RuntimeFeatureMask` è l’unico tipo `uint64_t` per storage e host
ABI. `RuntimeFeatureDisposition` distingue un `RuntimeBranch` vivo da un
La larghezza dell’ABI v2 resta congelata e non si estende in-place; oltre 64 bit serve v3 o una rappresentazione multiword, mai cambiare la larghezza di v2.
`FoldedBranch` il cui lato attivo è incondizionato nella revisione fissata, ma
il cui lato precedente conta ancora negli slot storici. Attivazioni RPC
finalizzate (`—` significa non attivo):

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

Questo ambito non pretende deliberatamente di coprire tutto il
`FeatureSnapshot` di Agave. NeverD include i gate di loader, verifier, VM,
entry/input, syscall e infrastruttura CPI soltanto quando cambiano direttamente
il decoding o l’host contract emesso. Scheduling delle transazioni, fees,
consensus, verifica precompile a livello di transazione e semantica applicativa
di un `CPI target built-in` appartengono all’`external runtime`; aggiungerne i
bit senza implementare quei built-in dichiarerebbe una capacità inesistente.

`SBFLoaders.def` registra proprietà e serializzazione. Distribuire ed eseguire
hanno smesso di essere la stessa risposta anni fa: `loader-v1` e `loader-v2`
rifiutano ogni management instruction che ricevono e continuano a eseguire i
programmi che già possiedono, ed è per questo che la loro serializzazione deve
restare leggibile.

| Loader | Serializzazione | Distribuisce | Esegue |
|--------|-----------------|--------------|--------|
| loader-v1 | `abi-v0` | no | sì |
| loader-v2 | `abi-v1` | no | sì |
| loader-v3 | `abi-v1` | sì | sì |
| loader-v4 | `abi-v1` | no | no (built-in rimosso) |

`SBFAccountLayout.def` colloca ogni campo account sotto ciascuna
serializzazione. Le due non differiscono soltanto nel padding: ordinano i campi
in modo diverso, così all’offset tre la forma unaligned ha il primo byte
dell’indirizzo dell’account e quella aligned il suo flag executable, e nulla nel
valore annuncia quale delle due sia stata letta. Un account ripetuto occupa
inoltre un byte in `abi-v0` e otto in `abi-v1`, il che disallinea l’intera visita
delle entry e non un singolo campo.

Se una chiamata si risolva sono tre domande, non una, perciò
`SBFSyscallLifecycle.def` contiene quanto sia assestata la firma pubblicata e
`SBFSyscallRegistration.def` contiene il resto: in quale registry compare una
syscall, quale gate la governa e in che direzione punta quel gate. La direzione
conta perché un gate può togliere con la stessa facilità con cui aggiunge —
attivare `disable_fees_sysvar` è ciò che ha rimosso la syscall del sysvar fees —
e leggere come additivo un gate che rimuove inverte la risposta per tutti i
cluster in un colpo solo. `sol_alloc_free_` resta registrata per l’esecuzione su
entrambi i lati del confine. Il deployment la registrava prima di
`disable_deploy_of_alloc_free_syscall`, poi la rifiuta dallo slot di attivazione
specifico del cluster. La revisione Agave fissata ha incorporato il lato attivo
del deployment nella costruzione del registry; NeverD conserva il gate affinché
un profilo storico ottenga la risposta precedente all’attivazione.

Su un runtime che ha attivato `simd-0321` l’entrypoint riceve anche l’indirizzo
degli instruction data in `r2`. NeverD lo modella come un genere di valore a sé
e non come una costante, perché dove atterri dipende dagli account: inventare un
indirizzo permetterebbe di riportare un load fatto attraverso di esso come campo
account con nome. Prima dell’attivazione il registro arriva a zero, e un
programma che lo legge legge uno zero. Gli entry point LLVM, C e Rust generati
prendono quindi il buffer di input e gli instruction data, perché un callable a
cui non si può dare il secondo non può riprodurre un programma che lo legge.

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

# Indica di quale runtime parla la risposta. Nulla di tutto ciò sta nel file
# del programma.
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`, `--sbf-slot`, `--sbf-loader` e `--sbf-purpose` selezionano il
profilo runtime. I default descrivono mainnet-beta com’è oggi, sotto
`loader-v3`, per un programma già distribuito. Chiedere invece del deployment
riporta le syscall che terrebbero un programma fuori dalla chain anche se la
chain continuerebbe a eseguirlo.

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
| Indirizzi base58 nei dati read-only | corrispondenza in `SBFKnownAddresses.def` e `SBFAnchorNamespaces.def`, oppure una costante che il codice materializza |
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

Il recupero scratch è guidato dalla domanda: il fixed point dello scratch Solana
CPI/PDA viene costruito solo quando esiste un vero `scratch consumer`; i programmi
senza tale consumer saltano il `whole-CFG fixed point`. `SBFAnalysisLimits.def`
definisce l’`analysis policy` dell’host, non i `protocol limits`:
`MaxModeledScratchBytes` vale 1,024 byte per `program point`, mentre
`ScratchFlowRetainedByteBudget` è una `logical retained estimate` di 8,388,608 byte.
Quando il budget è superato, il recupero applica widening esplicito a
`ScratchRecoveryPrecision::BlockLocal`. Vanno persi solo i `cross-block must-facts`;
`block-local replay` resta `sound` e può ancora recuperare i `same-block stores`.
Il printer emette stabilmente
`recovery scratch-precision=block-local` e widening non restituisce mai
`half-converged must-facts`.

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

Solo una syscall di runtime risolta può conservare scratch, e soltanto secondo
le sue finestre di scrittura verificate. Ogni chiamata interna, indiretta o
altrimenti irrisolta cancella i byte modellati, anche quando nessun argomento
attuale punta a scratch, perché un puntatore sfuggito in precedenza o un alias
globale può ancora consentire al chiamato di modificarli.
`sol_invoke_signed_rust` e `sol_invoke_signed_c` scrivono dati di account e non
la memoria del chiamante, così due invocazioni assemblate in uno stesso blocco
restano entrambe leggibili.

Il modello è un'analisi «must» in avanti sul CFG intraprocedurale: un byte
sopravvive fino a un blocco solo quando ogni percorso che lo raggiunge ha scritto
lo stesso valore. Gli archi di chiamata non vengono seguiti, perché un chiamato
non eredita nulla dal frame del chiamante. La worklist delle dipendenze non ha
vie di fuga di precisione basate sul numero di blocchi; un gate Release
facoltativo esercita l'intero limite di 10 MiB e `1,310,720` istruzioni.

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

`width` è espresso in bit. Ogni callback C generato restituisce `int`, incluso
`syscall_with_features`. Nell’entrypoint v1 `neverd_sbf_program`, zero indica
successo; ogni ritorno non zero da `load` o `store` viene normalizzato a
`NEVERD_SBF_MEMORY_ACCESS`, e ogni ritorno non zero da `syscall` a
`NEVERD_SBF_UNKNOWN_SYSCALL`; i contratti sono `v1-load-store-nonzero` e
`v1-syscall-nonzero`; v1 non propaga uno status esatto del callback.
Gli errori interni `InvalidRegister` e `InvalidBranch` vengono anch’essi
normalizzati a `NEVERD_SBF_INVALID_INSTRUCTION`
(`internal-invalid-instruction`).
L’entrypoint v2 `neverd_sbf_program_v2` è il percorso degli status esatti: un
valore callback riconosciuto di `neverd_sbf_status_v2`, incluso 9 o 10, resta il
fault gestito (`v2-exact-status`). L’entrypoint v2 conserva inoltre gli errori interni `InvalidRegister`
e `InvalidBranch` come 9 e 10. Un valore callback sconosciuto usa il fallback
specifico dell’operazione generato (`operation-specific-fallback`). Se
`syscall_with_features` è nullo, ricade su `base.syscall`; anche questo callback
restituisce `int` (`feature-aware-null-base-syscall`).
Struct ed entrypoint v1 restano compatibili con gli host legacy. Usa l’entrypoint v2
separato per ricevere `syscall_with_features` e lo snapshot delle runtime features
risolto. Il codice generato rappresenta registri, return PC, r6-r9 callee-saved,
frame pointer, indirizzi VM, division fault, operazioni PQR wide e wrapping shift.
Vengono emessi solo gli helper effettivamente usati, quindi l’output minimo supera
`clang -Wall -Wextra -Werror`.

## Contratto host Rust generato

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

Il vecchio entrypoint `neverd_sbf_program` e `SbfEnvironment` formano il
`v1-result-abi`; i loro metodi host usano `Result`. Un
`Some(SbfRuntimeFeatures::from_bits(0))` è il marker
`explicit-empty-snapshot`, distinto da `None`. `syscall_outcome` è il
`result-host-bridge` dal metodo host basato su Result a `SbfSyscallOutcomeV2`.
Poiché `SbfErrorV2` è marcato `#[non_exhaustive]`, i chiamanti devono usare un
`non-exhaustive-wildcard` (`_`) nei match.

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
/* Di quale runtime parla la risposta. I default descrivono mainnet-beta come
   sta oggi, sotto loader-v3, per un programma già distribuito. */
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

## Baseline di conformità corrente (2026-08-24)

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
| Manifest ELF ufficiale | 23/23 artefatti da `sbpf/tests/elfs` |
| Oracle ufficiale | `NeverDSBFExternalOracleTests` confronta 1,411 casi opcode/boundary con il verifier fissato |
| Esecuzione differenziale | oracle raw-byte contro LLVM ORC, C11 e Rust stable, incluse trace memory/fault/syscall |
| Aggregato integrato | `check-neverd-sbf` esegue tutte le suite registrate; non si fissa un totale che cambia rapidamente |
| ASan + UBSan | i target mirati girano fail-fast senza report; non si fissa un totale che cambia rapidamente |

L’audit fissa Anza `sbpf` a
`2510663bb8d894e8e3094be351e4bb4b604f1f84` e Agave a
`ef210d67f2fabeee1730498188fa78854260c679`. Per aggiornarlo, rivedere
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` e
`SBFUpstreamSources.def`, quindi eseguire:

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

Il confronto mostra che `sol-azy` va in crash sull’ELF strict corrente e lascia
un nodo CFG legacy indefinito; `solana-data-reverser` tratta account data,
`SolDragon` indica l’analisi come WIP e `bn-ebpf-solana` richiede Binary Ninja.
Gli `sbpf` e Agave ufficiali restano quindi l’autorità semantica.

## Contratto di evidenza verificato il 2026-08-24

`SBFUpstreamSources.def` fissa l’audit su Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679` e Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`. Il manifest ufficiale passa 23/23;
`NeverDSBFExternalOracleTests` confronta 1,411 casi opcode/boundary con il
verifier ufficiale indipendente tramite `SBFOfficialOracleProtocol.def` e
`SBFOfficialVerifierCases.def` e `SBFOfficialExecutionConstants.def`. Gli ELF malformati provengono da
`SBFOfficialELFMutations.def` e da un corpus tabellare; non si congela un totale
che cambia rapidamente.
Separatamente, il `differenziale ELF strict di 41 casi` esegue l’intera matrice
strict-v3 tramite il processo ufficiale `verify-elf-batch` e NeverD; questi 41
casi non fanno parte del totale 1,411.

La matrice ufficiale aggiuntiva di esecuzione (`additional execution matrix`) è
separata: contiene esattamente 508 casi attivi `(Version,Opcode)` più 58 casi
di confine, per 566 casi di esecuzione esatti. Non sostituisce né rientra nei
1,411 `verifier probes` e non rientra nel differenziale ELF strict
da 41 casi.

`NeverDSBFAgaveConformanceTests` autentica anche la revision Firedancer
test-vectors `68bb4af40235562e8852fa23d5727e49c2a0b862` e confronta tutti i 1,955
fixture `sol_compat_elf_loader_v1` (1,399 accettati e 556 rifiutati). Per ogni
ELF accettato confronta inoltre `entry_pc`, `text_off`, `text_cnt`,
`rodata_hash` e `calldests_hash`. Questa gate verifica volutamente solo il
loader e non esegue il successivo instruction verifier, mantenendo separate le
due fasi di Agave.

Il profilo chain predefinito resta fedele ad Agave: le righe
`SBF_RUNTIME_VERSION` calcolano per cluster/slot storico l’ISA massimo e lo
fanno avanzare da V0 a V1, V2 e V3 quando si attivano i feature account
ufficiali; il massimo corrente resta V3. Usa
`RuntimeVersionPolicy::ChainProfile`. Solo `--sbf-version=v4` esplicito sceglie
`RuntimeVersionPolicy::UpstreamToolchain` per analisi offline secondo lo `sbpf`
fissato, senza sostenere che v4 sia attivo on-chain. Il limite corrente di
10 MiB è esattamente `10'485'760` byte; 65,536 resta solo provenance/test
storico e non è imposto in esecuzione.

I registri `.def` tipizzati sono l’autorità per feature, syscall, fault e
source ABI: `SBFSyscallRegistration.def`, `SBFValidationRules.def`, `SBFFaultCodes.def`,
`SBFSourceStatuses.def`, `SBFArgumentRegisters.def` e `SBFEdgeKinds.def`.
`SBFFaultCodes.def` stabilizza i valori degli execution fault;
`SBFSourceStatuses.def` possiede separatamente l’ABI del source generato. Il
loader è `raw-first`: corregge le relative CALL, applica una volta le raw
relocation in ordine ELF ordinal e mantiene l’ordine d’errore text identity,
CALL, relocation, entrypoint e read-only layout. Il mapping file/VM è gap-aware
e non inventa byte nei vuoti.

CFG e dataflow sono per funzione: un call edge non è un predecessor locale,
una shared tail resta ambigua e tutti i latch di un loop formano un’unica
regione multi-latch. Worklist e ownership sono verificati con 10,000 funzioni,
blocchi in ordine inverso e conditional latch, senza stimare tempi di macchina.

Il call graph SBF pubblico usa `callgraph-budget=fail-closed`: i limiti tipizzati
di input, provenance, node, edge, element e `CallGraphOutputByteBudget` rendono
il JSON esatto oppure vuoto. All’esaurimento restituisce
`{"nodes":[],"edges":[]}`, imposta `neverd_last_error()` e non pubblica mai una
relazione parziale.

Ogni riga di attivazione contiene cluster, feature account e slot; un
`RPC activation audit` può confrontarla con un nodo live lasciando offline
l’analisi ordinaria. Il confronto include Blueshift, `qedsvm` (prove Lean di
path selezionati, ma ELF loader attualmente solo V0), `leanprover-solanalib`,
`sol-azy`, `bn-ebpf-solana` e Ghidra/SolDragon.
`ezBPF` si dichiara deprecated al commit
`88829078a6d7682a2baed0d696d500401c263750` e rimanda a Blueshift; è un
predecessore archiviato con un’unica mappa byte-to-enum, non un decoder
version-aware per moved-memory, JMP32 e la matrice v0-v4 attuale. In questo
audit i pin di confronto sono Blueshift `704e40f7aa82446555b19d9ffbc0a6e18a35480f`,
`qedsvm` `99bd5ede85374adc7fc5c835c2432ecf4e123fd1` e `leanprover-solanalib`
`6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec`; i quattro strumenti locali sono
fissati a `sol-azy` `362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`,
`solana-data-reverser` `bf90923adec984a61ca0437e9d341360ac1b11ee`, `SolDragon`
`002b98677a5e595a773af6607b77210f5ea71db7` e `bn-ebpf-solana`
`c3fe0de45d37eb68dcb08f2498c6e1f986056572`.
snapshot NeverD ha
l’evidenza riproducibile più forte che abbiamo trovato tra i decompiler SBF
generali pubblici auditati; è un’affermazione comparativa limitata, non un
«numero uno al mondo» assoluto.

La revisione include anche l’audit pubblico di `r2ghidra-solana`, fissato a
`eca0b8e2d307e00991e289b8f9b0f45743819f1b`, con UX Ghidra C-like e
`C-like-pdg` per account, Anchor, stringhe e syscall; il CI passa al HEAD fissato,
ma la suite Solana specifica è commentata e lo smoke CI decompila soltanto
`/bin/ls`. Il reproducer diretto conferma che l’`relative_call_sbpfv0.so` ufficiale
di V0 produce C ragionevole, mentre l’`relative_call.so` ufficiale di V3 fallisce
in `pdg`; il risultato è riproducibile. `radare2-solana`, fissato a
`292d845681be377cadc9959a74c2cadeb6e7f412`, estende SIMD-0173/0174 esclusivi di
V2 come `>=V2` anche a V3/V4, mentre il `program.rs` ufficiale li dichiara solo
V2. `SBPF-3-1`, fissato a `0e602c93007faa96bccb8e1e12040954ff108b6f`, ha solo
2/2 test cargo banali senza CI; il version detection placeholder restituisce
none/V0, il decoder opcode high-nibble è errato e il salto usa imm invece di
off. Gli ELF relative_call V0/V3 producono lo stesso pseudocode errato. Il
vantaggio di NeverD è l’evidenza ufficiale riproducibile di loader, verifier,
runtime e process-oracle V0–V4, senza negare UX e output C degli strumenti.

`SBFComparisonTools.def` è l’unica autorità per display name e revision complete
degli strumenti confrontati. L’ultimo sweep pubblico e delimitato aggiunge questi
risultati:

- `blastrock/Solana-eBPF-for-Ghidra`, fissato a
  `c3ad719004726fe924dbed901eca2744ad82c85d`, offre vera UX Ghidra P-code, ma un
  solo modello SLEIGH senza versione fissa CALLX su `dst` e mescola opcode
  legacy/current. Non ha test reali né CI e nel source predefinito manca una
  classe di costanti relocation che viene referenziata.
- `SolEmu-Ghidra`, fissato a `6520af2ff104d5adbec24632ba3afa3bef0da529`, eredita
  quel decoder identico e aggiunge UX da emulatore attorno a comportamenti CPI,
  crittografici e ZK esplicitamente simulati o placeholder; anche qui mancano test
  reali e CI. `Ghidra_sBPF`, fissato a
  `907bd4476432ca83bb2352686ad1ccafdb38504c`, permette la scelta manuale v1-v3,
  ma include cumulativamente in V3 gli encoding esclusivi di V2, senza selezione
  automatica V0/V4, test o CI.
- `solana-ebpf-ida-processor`, fissato a
  `aacd215907266190ed9c6c1b408ca9309f92ecdd`, è una utile UI IDA per
  disassembly/relocation, non un source lifter; la sua tabella mista legge sempre
  CALLX da `imm` e non ha test o CI. `solana-bpf-reverse`, fissato a
  `39479a3bddb8cb866ee499266a76a1b54069b222`, genera report euristici e scaffold
  Rust TODO da layout hard-coded; il run ha dato 9 pass, 2 fail e 1 skip, senza CI.
- `solens`, fissato a `22defa1c8f4118dacd42f5c291f1ac31609fc0e5`, è un
  disassembler terminale V2-only con 0 test e senza CI. `sbpf-decompiler`, fissato
  a `37b8bc0edc7ce347abee466f5f974e900c1948df`, oggi implementa soltanto tre righe
  `Hello, world!`, con 0 test e senza CI.
- `sbpf-eye`, fissato a `5277a52aeb58e50b6ff8f9020414334765369b49`, si dichiara
  TUI lightweight WIP per instruction/CFG: 3 test passano, ma non ha IR semantico,
  source emitter o CI. `svm_bytecode_analyzer`, fissato a
  `12aa236db8964e6be661e38131c2dc81588cf19c`, è un analyzer disassembler/CFG,
  non un lifter; decodifica male i byte register/offset e il run ha dato 17 pass
  e 1 fail, senza CI.
- `giraffexiu/Solana-eBPF-for-Ghidra`, fissato a
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07`, è uno snapshot di un commit della
  stessa linea Ghidra, senza nuova semantica di versione, test o CI. `CertSBF`,
  fissato a `bb93a97cf0c64d119d08ec851e8e820315beb59e`, è una preziosa
  formalizzazione Isabelle/HOL del vecchio rBPF, non un source decompiler V0-V4
  corrente dell’intero programma.

Questi risultati rafforzano solo l’evidenza comparativa nello snapshot pubblico
delimitato; non sono una conclusione assoluta su strumenti futuri o privati.

L’audit RPC finale del 2026-08-24 ha coinciso esattamente: 38 feature accounts e
89 activation rows; mainnet allo slot 441305159, testnet a 433055669 e devnet a
487238699. L’account vuoto pending, di proprietà del sistema
(`VirtualAddressSpaceAdjustments` su mainnet), non era attivato. Nessun URL RPC
è fissato nella documentazione.

La Linux Release CI legge i pin esatti con `--print-pinned-revision`,
`--print-test-vectors-revision` e `--print-toolchain`, autentica oracle e corpus
sparse ed esporta `NEVERD_SBPF_ORACLE` e `NEVERD_AGAVE_CONFORMANCE_ROOT`; entrambi
i test esterni sono quindi obbligatori. Un run locale normale senza env
oracle/corpus esplicito scopre i casi ma può saltarli.
