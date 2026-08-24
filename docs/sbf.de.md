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

Eine Versionsnummer ist selbst keine Spezifikation, daher hält
`SBFVersionFeatures.def` die Verhaltensänderungen und die Versionstabelle setzt
sie zusammen. Jeder Eintrag trägt den SIMD-Vorschlag, der die Änderung
angenommen hat, sowie das Prädikat, das `anza-xyz/sbpf` für dieselbe Frage
anbietet, denn mehrere Vorschläge landen in einer Version und ein Vorschlag
ändert mehrere unabhängige Dinge: SIMD-0173 verschiebt die
Memory-Instruktionsklassen und zieht `lddw` zurück, während SIMD-0174 in
derselben Version unabhängig davon die PQR-Klasse ergänzt. Den Vorschlag am
Feature statt an der Version festzuhalten, hält eine rekonstruierte
Versionsaussage auf das Dokument rückführbar, das sie entschieden hat, und ist
der Grund, weshalb die beiden `callx`-Regeln getrennte Features sind: SIMD-0173
liest das Quellregister, SIMD-0377 das Zielregister.

v2-Änderungen gelten bewusst nicht für v3. Feature-Checks sind explizit, keine
`version >= N`-Vermutungen. Strict ist Standard und lehnt fehlerhafte Header,
Ranges, Alignments, unsupported writable Legacy-Sections, ungültige
Continuations/Register/Frame-Pointer-Writes/Branches und inaktive Opcodes mit
Instruction Slot und virtueller Adresse ab.

## Die Runtime, über die eine Beschreibung spricht

Die ISA-Version steht in der Datei. Fast nichts sonst. Welche Syscalls sich
auflösen, hängt von Chain und Slot ab; an welchen Bytes ein Account-Feld liegt,
hängt vom Loader ab, dem das Programm gehört; ob der Entry Point ein zweites
Argument bekommt, hängt von einem Schalter ab, den die Chain umlegt; und ob ein
Programm deploybar ist, ist eine andere Frage als, ob es läuft. Ein einzelner
Versionsschalter kann davon nichts ausdrücken, deshalb sind das getrennte Achsen
mit getrennten Tabellen.

`SBFRuntimeFeatures.def` verzeichnet Cluster, Zwecke und die Gates, die ändern,
was NeverD meldet, jeweils mit dem Runtime-Identifier, dem Feature-Account,
dessen Zustand die Aktivierung festhält, und dem Slot, an dem jedes Cluster sie
aktiviert hat. Ein pending Account kann existieren, ohne sein Gate einzuschalten.
Ein Gate ohne Zeile für ein Cluster ist dort nicht aktiviert. `simd-0321` ist in
jedem Cluster an; `simd-0449` und der SHA-512-Syscall sind auf Testnet und
Devnet an und auf Mainnet aus — genau deshalb scheitert auf Mainnet ein
Programm, das auf Devnet funktioniert.

In der gepinnten Agave-Revision verschärft das Gate
`syscall_parameter_address_restrictions` (`simd-0459`) den Vertrag für
VM-Adressen und Alignment von Syscall- und CPI-Parametern. Der finalisierte
RPC-Zustand verzeichnet die Aktivierung bei Slot 429,840,000 auf Mainnet,
407,468,256 auf Testnet und 462,240,000 auf Devnet. Das Gate
`account_data_direct_mapping` ersetzt bei angepasstem Adressraum die Kopie der
Account-Daten im Eingabepuffer durch direkt hinterlegte Speicherregionen; es
ist auf Mainnet nicht aktiviert und wird bei 408,332,256 auf Testnet sowie
463,968,000 auf Devnet aktiv. Keines der Gates erzeugt ein neues Account-ABI
oder ändert logische ABIv0/ABIv1-Feldoffsets: Der besitzende Loader wählt
weiterhin die Serialisierung, NeverD führt beide als Runtime-Topologiemetadaten.

Feature-Bits bleiben append-only. Da der beobachtbare Snapshot 32 Bits
überschritten hat, ist `RuntimeFeatureMask` der einzige `uint64_t`-Typ für
Speicherung und Host-ABI. `RuntimeFeatureDisposition` unterscheidet einen
Die v2-ABI-Breite bleibt eingefroren und wird nicht in-place erweitert; mehr als 64 Bit erfordern v3 oder eine multiword-Darstellung, nie eine Änderung der v2-Breite.
aktiven `RuntimeBranch` von einem `FoldedBranch`, dessen aktive Seite in der
gepinnten Revision unbedingt ist, dessen alte Seite aber für historische Slots
weiter zählt. Finalisierte RPC-Aktivierungen (`—` bedeutet nicht aktiviert):

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

Dieser Umfang beansprucht bewusst nicht die gesamte Agave-`FeatureSnapshot`.
NeverD nimmt Loader-, Verifier-, VM-, Entry/Input-, Syscall- und
CPI-Infrastruktur-Gates nur auf, wenn sie Decoding oder den ausgegebenen
Host-Vertrag direkt ändern. Transaction Scheduling, Gebühren, Konsens,
transaction-weite Precompile-Prüfung und `CPI target built-in`-Geschäftssemantik
gehören zur `external runtime`; ihre Bits ohne Built-in-Implementierung
aufzunehmen würde eine nicht vorhandene Fähigkeit ausweisen.

`SBFLoaders.def` verzeichnet Ownership und Serialisierung. Deployen und
Ausführen sind seit Jahren nicht mehr dieselbe Antwort: `loader-v1` und
`loader-v2` lehnen jede Management-Instruktion ab, die sie erreicht, und führen
die Programme weiter aus, die ihnen bereits gehören — weshalb ihre
Serialisierung weiterhin lesbar sein muss.

| Loader | Serialisierung | Deployt | Führt aus |
|--------|----------------|---------|-----------|
| loader-v1 | `abi-v0` | nein | ja |
| loader-v2 | `abi-v1` | nein | ja |
| loader-v3 | `abi-v1` | ja | ja |
| loader-v4 | `abi-v1` | nein | nein (Built-in entfernt) |

`SBFAccountLayout.def` verortet jedes Account-Feld unter jeder Serialisierung.
Die beiden unterscheiden sich nicht nur im Padding — sie ordnen die Felder
anders an, sodass an Offset drei die unaligned Form das erste Byte der
Account-Adresse trägt und die aligned Form ihr Executable-Flag, und nichts am
Wert verrät, welche gelesen wurde. Ein wiederholter Account belegt zudem ein
Byte in `abi-v0` und acht in `abi-v1`, was einen Lauf über die Einträge
verschiebt statt eines einzelnen Felds.

Ob ein Aufruf sich auflöst, sind drei Fragen und nicht eine. Deshalb hält
`SBFSyscallLifecycle.def` fest, wie gefestigt die veröffentlichte Signatur ist,
und `SBFSyscallRegistration.def` den Rest: in welcher Registry ein Syscall
auftaucht, welches Gate ihn regiert und in welche Richtung dieses Gate zeigt.
Die Richtung zählt, weil ein Gate ebenso gut etwas wegnehmen wie hinzufügen
kann — die Aktivierung von `disable_fees_sysvar` hat den Fees-Sysvar-Syscall
entfernt —, und ein entfernendes Gate als hinzufügendes zu lesen invertiert die
Antwort für alle Cluster auf einmal. `sol_alloc_free_` bleibt auf beiden Seiten
der Grenze für die Ausführung registriert. Deployment registrierte es vor
`disable_deploy_of_alloc_free_syscall` und lehnt es ab dem clusterspezifischen
Aktivierungs-Slot ab. Die gepinnte Agave-Revision hat die aktive Deployment-Seite
in die Registry-Konstruktion gefaltet; NeverD bewahrt das Gate, damit ein
historisches Profil die Antwort vor der Aktivierung erhält.

Auf einer Runtime, die `simd-0321` aktiviert hat, erhält der Entry Point
zusätzlich die Adresse der Instruction Data in `r2`. NeverD modelliert sie als
eigene Art von Wert statt als Konstante, denn wo sie landet, hängt von den
Accounts ab: Eine erfundene Adresse ließe einen Load über sie als benanntes
Account-Feld melden. Vor der Aktivierung kommt das Register als Null an, und ein
Programm, das es liest, liest eine Null. Die erzeugten LLVM-, C- und
Rust-Entry-Points nehmen deshalb den Input-Puffer und die Instruction Data, denn
ein Callable, dem man das Zweite nicht geben kann, reproduziert kein Programm,
das es liest.

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

# Sagen, über welche Runtime die Antwort spricht. Nichts davon steht in der
# Programmdatei.
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`, `--sbf-slot`, `--sbf-loader` und `--sbf-purpose` wählen das
Runtime-Profil. Die Vorgaben beschreiben Mainnet-Beta im aktuellen Stand, unter
`loader-v3`, für ein bereits deploytes Programm. Wer stattdessen nach dem
Deployment fragt, erhält die Syscalls, die ein Programm von der Chain fernhalten
würden, obwohl die Chain es weiter ausführen würde.

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

## Solana-Programmrekonstruktion

Über dem SBF-Maschinenmodell berichtet NeverD, was ein Programm als
Solana-Programm bedeutet. Jede festgehaltene Aussage trägt den Beleg, der sie
erzeugt hat; was die Bytes nicht entscheiden, bleibt offen statt geraten.

| Rekonstruiert | Beleg |
|---------------|-------|
| Base58-Adressen in Read-only-Daten | Treffer in `SBFKnownAddresses.def` und `SBFAnchorNamespaces.def` oder eine vom Code erzeugte Konstante |
| Die deklarierte eigene Adresse | ein `sol_memcmp_` über genau eine Schlüssellänge gegen eine Read-only-Konstante |
| Anchor-Instruktionsdispatch | ein 64-Bit-Vergleich, dessen Konstante einem SHA-256-Discriminator entspricht |
| CPI-Ziele | der vom Invoke-Argument aus erreichbare Instruction-Datensatz |
| Die von einem Aufruf gewählte Operation | ein in `SBFProgramInstructions.def` geführter Selektor oder ein führender Anchor-Discriminator |
| Seeds einer abgeleiteten Adresse | das vom Derivations-Argument aus erreichbare Seed-Deskriptor-Array |
| Zugriffe auf Account-Felder | ein Load oder Store, dessen Adresse nachweislich im serialisierten Input liegt |

Der Loader übergibt ein Argument, den serialisierten Input-Puffer an der Basis
der Input-Region. Konstantenpropagation ab diesem Eintrittszustand liefert daher
benannte Account-Felder statt roher Offsets. `SBFAccountLayout.def` hält die
offizielle Serialisierung; die festen Felder werden darauf geprüft, ihren Bereich
lückenlos zu überdecken.

Anchor bildet einen Discriminator, indem es `<namespace>:<name>` mit SHA-256
hasht und die ersten acht Bytes behält, was nicht umkehrbar ist. NeverD bestätigt
deshalb nur Kandidaten: `SBFAnchorNames.def` ist ein Wörterbuch wiederkehrender
Namen, und `--sbf-idl` liefert das eigene IDL des Programms, das Vorrang hat. Ein
64-Bit-Vergleich heißt erst dann Discriminator, wenn mindestens einer davon einen
Namen auflöst.

`SBFKnownAddresses.def` verzeichnet Protokoll- und kanonische Programmadressen.
Jeder Eintrag muss zu genau 32 Byte dekodieren, was die Testsuite erzwingt. Die
Rekonstruktion braucht zudem die Syscall-ABI: SBPFv3 bildet Read-only-Daten auf
Adresse null ab, sodass ein Längenargument und eine niedrige Datenadresse
dieselbe Zahl sind. `SBFSyscalls.def` hält daher fest, welche Argumentregister
eine VM-Adresse führen; nur diesen wird gefolgt.

Die beiden Invoke-Syscalls beschreiben dieselbe Instruction mit zwei
verschiedenen Strukturen, und `SBFCPIABI.def` führt beide Layouts, verschlüsselt
nach dem Syscall, der sie auswählt. Das falsche Layout schlägt nicht fehl, es
meldet stillschweigend den ersten Account als aufgerufenes Programm.
`SBFProgramInstructions.def` benennt danach die angeforderte Operation anhand des
Selektors, den das jeweilige Interface veröffentlicht: ein Bincode-Variantenindex
für System-, Stake-, Lookup-Table- und Upgradeable-Loader-Programm, ein
führendes Byte für die Token-Programme, samt Token-2022-Erweiterungsbereich über
der mit dem ursprünglichen Token-Programm geteilten Nummerierung. Ein nicht
geführter Selektor wird als Zahl gemeldet.

### Scratch-Speicher und Syscall-Fenster

Ein Programm übergibt der Runtime fast nie eine Konstante. Es baut ein
Seed-Array, eine serialisierte Instruction und deren Payload im eigenen Frame
oder auf dem Heap zusammen und übergibt einen Zeiger. Nur das geladene Image zu
lesen zeigte den Zeiger und nichts von dem, worauf er verweist; die
Rekonstruktion führt deshalb ein byte-genaues Modell des Speichers, den nur
dieses Programm schreiben kann, begrenzt durch `kMaxModeledScratchBytes`.

Die Scratch-Rekonstruktion ist bedarfsgesteuert: Der Solana-CPI/PDA-Scratch-Fixpunkt
wird nur aufgebaut, wenn ein echter `scratch consumer` existiert; Programme ohne ihn
überspringen den `whole-CFG fixed point`. `SBFAnalysisLimits.def` beschreibt die
Host-`analysis policy`, nicht `protocol limits`: `MaxModeledScratchBytes` erlaubt
1,024 Bytes je `program point`, und `ScratchFlowRetainedByteBudget` ist eine
`logical retained estimate` von 8,388,608 Bytes. Bei überschrittenem Budget weitet
die Rekonstruktion ausdrücklich auf `ScratchRecoveryPrecision::BlockLocal`.
Dabei gehen nur `cross-block must-facts` verloren; `block-local replay` bleibt `sound`
und kann weiterhin `same-block stores` wiederherstellen.
Der Printer gibt stabil die Zeile `recovery scratch-precision=block-local` aus, und
widening liefert niemals `half-converged must-facts` zurück.

Zwei Fakten entscheiden, was einen Aufruf überlebt. `SBFSyscalls.def` sagt,
welche Argumentregister eine VM-Adresse führen; `SBFSyscallMemory.def` sagt, was
die Runtime durch sie tut, als Lesen oder Schreiben mit einer Ausdehnung
`Fixed`, `Counted` oder `Opaque`. Ein Syscall ohne Schreibfenster kann kein Byte
des Aufrufers ändern, also gilt alles vor `sol_log_` Bewiesene auch danach. Ein
durch ein Längenargument begrenztes Schreiben verwirft genau dieses Fenster. Ein
`Opaque`-Schreiben verwirft seine Basisadresse und alles darüber, denn ein Puffer
reicht nie unter seinen Anfang und nie über eine VM-Regionsgrenze. Die
Effektzusammenfassung in `SBFSyscalls.def` und die Fenstertabelle werden in
beiden Richtungen gegeneinander geprüft, sodass keine allein abdriften kann.

`sol_memcpy_`, `sol_memmove_` und `sol_memset_` werden verfolgt statt nur
verworfen: mit bewiesenem Ziel, bewiesener Länge und bewiesener Quelle sind die
Zielbytes bekannt. Genau das rekonstruiert die Operation, die ein
Anchor-Programm aufruft, denn dessen Payload wird kopiert und nicht gemappt.

Nur ein aufgelöster Runtime-Syscall darf Scratch erhalten, und dann
ausschließlich gemäß seinen auditierten Schreibfenstern. Jeder interne,
indirekte oder sonst unaufgelöste Aufruf verwirft die modellierten Bytes – selbst
wenn aktuell kein Argument auf Scratch zeigt –, denn ein früher entwichener
Zeiger oder globaler Alias kann dem Aufgerufenen weiterhin Schreibzugriff geben.
`sol_invoke_signed_rust` und `sol_invoke_signed_c` schreiben Account-Daten statt
Aufruferspeicher, sodass zwei in einem Block zusammengebaute Invocations beide
lesbar bleiben.

Das Modell ist eine Vorwärts-Must-Analyse über den funktionsinternen CFG: Ein
Byte überlebt in einen Block nur, wenn jeder Pfad dorthin denselben Wert
geschrieben hat. Call-Kanten werden nicht verfolgt, weil ein Aufgerufener nichts
vom Frame seines Aufrufers erbt. Seine Abhängigkeits-Worklist hat keinen
blockzahlabhängigen Präzisionsausstieg; ein optionales Release-Gate prüft die
volle 10-MiB-Grenze von `1,310,720` Instruktionen.

`SBFLints.def` katalogisiert Beobachtungen über das ganze Programm: fehlende
Signer- oder Owner-Prüfung, ein nicht konstantes Invoke-Ziel, ein veralteter oder
feature-gegateter Syscall und eine SBPF-Version, die SIMD-0500 zur Deployment
nicht mehr annimmt. Jede trägt Schweregrad und Konfidenz, und kein Lint ändert
die dekodierte Semantik. Nichts in dieser Schicht kontaktiert das Netzwerk.

## Vertrag der erzeugten LLVM-Runtime

LLVM behandelt VM-Adressen nie als Host-Pointer. Geprüfte Load/Store/Syscall-
Deklarationen liefern `i32`-Status; Load/Syscall schreiben `i64` über Output-
Pointer. Nonzero springt in einen expliziten SBF-Fault-Block. Das Module besteht
`llvm::verifyModule` vor der Ausgabe.

## Vertrag des erzeugten C-Hosts

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

`width` wird in Bit angegeben. Jeder generierte C-Callback gibt `int` zurück,
einschließlich `syscall_with_features`. Beim v1-Entrypoint `neverd_sbf_program`
bedeutet null Erfolg; jeder ungleich null zurückgegebene Wert von `load` oder
`store` wird zu `NEVERD_SBF_MEMORY_ACCESS` normalisiert, jeder ungleich null
zurückgegebene Wert von `syscall` zu `NEVERD_SBF_UNKNOWN_SYSCALL`; dies sind die
Vertragsmarker `v1-load-store-nonzero` und `v1-syscall-nonzero`; v1 reicht
keinen exakten Callback-Status durch. Interne Fehler `InvalidRegister` und
`InvalidBranch` werden ebenfalls zu `NEVERD_SBF_INVALID_INSTRUCTION` normalisiert
(`internal-invalid-instruction`).
Der v2-Entrypoint `neverd_sbf_program_v2` ist der Pfad für exakte Statuswerte:
Ein erkannter Callback-Wert aus `neverd_sbf_status_v2`, einschließlich 9 oder 10,
bleibt als behandelter Fehler erhalten (`v2-exact-status`). Der v2-Entrypoint erhält auch interne
`InvalidRegister`- und `InvalidBranch`-Fehler als 9 bzw. 10. Ein unbekannter
Callback-Wert verwendet den vom Generator vorgesehenen operationsspezifischen
Fallback (`operation-specific-fallback`). Ein null gesetztes `syscall_with_features`
fällt auf `base.syscall` zurück; auch dieser Callback gibt `int` zurück
(`feature-aware-null-base-syscall`).
Struct und Entrypoint v1 bleiben mit Legacy-Hosts kompatibel. Der getrennte
v2-Entrypoint liefert `syscall_with_features` und den aufgelösten Runtime-Feature-
Snapshot. Der generierte Code stellt Register, Return-PCs, callee-saved r6-r9,
Frame Pointer, VM-Adressen, Divisionsfehler, breite PQR-Operationen und Wrapping-
Shifts dar. Es werden nur tatsächlich benötigte Helpers emittiert, daher besteht
minimaler Output `clang -Wall -Wextra -Werror`.

## Vertrag des erzeugten Rust-Hosts

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

Der alte Entry-Point `neverd_sbf_program` und `SbfEnvironment` bilden den
`v1-result-abi`; ihre Hostmethoden verwenden `Result`. Ein
`Some(SbfRuntimeFeatures::from_bits(0))` ist der Marker
`explicit-empty-snapshot` und unterscheidet sich von `None`. `syscall_outcome`
ist die `result-host-bridge` von der Result-basierten Hostmethode zu
`SbfSyscallOutcomeV2`. Da `SbfErrorV2` mit `#[non_exhaustive]` markiert ist,
müssen Aufrufer beim Match einen `non-exhaustive-wildcard` (`_`) verwenden.

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
/* Über welche Runtime die Antwort spricht. Die Vorgaben beschreiben
   Mainnet-Beta im aktuellen Stand, unter loader-v3, für ein bereits deploytes
   Programm. */
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

## Aktuelle Konformitätsbasis (2026-08-24)

Nach den Relocations ist ein unveränderliches, VM-adressiertes `ProgramImage`
die gemeinsame Wahrheit für Decoder, Interpreter, String-Recovery sowie die
LLVM-, C- und Rust-Backends. Separate Text- oder Rodata-Kopien können daher
nicht von der Loader-Semantik abweichen.

Geschlossene Datensätze liegen in `SBFVersions.def`, `SBFOpcodes.def`,
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFVersionFeatures.def`, `SBFProtocolLimits.def`,
`SBFSyscalls.def`, `SBFSyscallMemory.def`, `SBFCPIABI.def`,
`SBFProgramInstructions.def` und
`SBFUpstreamSources.def`. Einmalige Diagnosen und LLVM-Blocknamen bleiben lokal,
entsprechend der tatsächlichen LLVM-Konvention.

`SBFProtocolLimits.def` erfasst den historischen Wert von 65.536 Instructions
und die aktuelle Account-Data-Grenze von 10 MiB; NeverD leitet daraus die
konservative Decode-Grenze ab.

Bei strict v3/v4 bilden begrenzte Program Headers den Runtime-Vertrag; Section-
und Symboltabellen sind optionale Debug-Anreicherung und machen ein gültiges
Image bei Fehlen oder Beschädigung nicht ungültig. Legacy v0-v2 vereinigt
`.text`, `.rodata`, `.data.rel.ro` und `.eh_frame`; `R_BPF_64_64`,
`R_BPF_64_RELATIVE` und `R_BPF_64_32` werden genau einmal vor dem Einfrieren
des Images angewandt.

| Nachweis | Geprüftes Ergebnis |
|----------|--------------------|
| Offizielles ELF-Manifest | 23/23 Artefakte aus `sbpf/tests/elfs` |
| Offizieller Oracle | `NeverDSBFExternalOracleTests` gleicht 1,411 Opcode-/Grenzfälle mit dem gepinnten Verifier ab |
| Ausführungsdifferenz | Raw-Byte-Oracle gegen LLVM ORC, C11 und stabiles Rust samt Memory/Fault/Syscall-Trace |
| Integriertes Aggregat | `check-neverd-sbf` führt alle registrierten Suites aus; eine schnell driftende Summenzahl wird nicht fixiert |
| ASan + UBSan | fokussierte Targets laufen fail-fast ohne Report; eine schnell driftende Summenzahl wird nicht fixiert |

Die Prüfung ist auf Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84` und Agave
`ef210d67f2fabeee1730498188fa78854260c679` fixiert. Zur Aktualisierung
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` und
`SBFUpstreamSources.def` prüfen und ausführen:

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

Der Vergleich zeigte: `sol-azy` stürzt beim aktuellen strict ELF ab und lässt
einen undefinierten Legacy-CFG-Knoten zurück; `solana-data-reverser` behandelt
Account-Daten, `SolDragon` kennzeichnet die Analyse als WIP und
`bn-ebpf-solana` benötigt Binary Ninja. Offizielles `sbpf` und Agave bleiben
daher die semantische Autorität.

## Geprüfter Evidenzvertrag vom 2026-08-24

`SBFUpstreamSources.def` fixiert die Prüfung auf Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679` und das Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`. Das offizielle Manifest besteht
23/23; `NeverDSBFExternalOracleTests` vergleicht 1,411 Opcode-/Grenzfälle über
`SBFOfficialOracleProtocol.def`, `SBFOfficialVerifierCases.def` und
`SBFOfficialExecutionConstants.def` mit einem
separat gebauten offiziellen Verifier. Fehlerhafte ELF-Fälle stammen aus
`SBFOfficialELFMutations.def` und einem tabellengesteuerten Corpus; eine
schnell driftende Gesamtzahl wird absichtlich nicht dokumentiert.
Getrennt führt die `41 Fälle strikter ELF-Differenzprüfung` die vollständige
Strict-v3-Matrix durch den offiziellen `verify-elf-batch`-Prozess und NeverD;
diese 41 Fälle gehören nicht zur Summe von 1,411.

Die offizielle zusätzliche Ausführungsmatrix (`additional execution matrix`) ist
separat: Sie enthält genau 508 aktive Fälle `(Version,Opcode)` sowie 58
Grenzfälle, zusammen 566 exakte Ausführungsfälle. Sie ersetzt die 1,411
`verifier probes` nicht und zählt nicht zu ihnen; ebenso wenig zur 41-Fälle
umfassende strikte ELF-Differenzprüfung.

`NeverDSBFAgaveConformanceTests` authentifiziert außerdem die Firedancer-
test-vectors-Revision `68bb4af40235562e8852fa23d5727e49c2a0b862` und gleicht alle
1,955 `sol_compat_elf_loader_v1`-Fixtures ab (1,399 akzeptiert, 556 verworfen).
Für jedes akzeptierte ELF werden zusätzlich `entry_pc`, `text_off`, `text_cnt`,
`rodata_hash` und `calldests_hash` verglichen. Dieses Gate prüft absichtlich nur
den Loader und führt den nachgelagerten Instruction-Verifier nicht aus, damit
Agaves zwei Phasen getrennt bleiben.

Das Standard-Chain-Profil bleibt Agave-treu: `SBF_RUNTIME_VERSION`-Zeilen
berechnen für historischen Cluster/Slot das maximale ISA und schalten es mit
den offiziellen Feature-Accounts von V0 über V1 und V2 auf V3; aktuell bleibt
V3. Dies läuft unter `RuntimeVersionPolicy::ChainProfile`. Nur ein explizites `--sbf-version=v4`
wählt `RuntimeVersionPolicy::UpstreamToolchain` für Offline-Analyse gemäß dem
gepinnten `sbpf`; dies behauptet keine On-Chain-Aktivierung von v4. Die aktuelle
10-MiB-Grenze ist exakt `10'485'760` Byte. 65,536 bleibt nur historische
Provenienz/Testdatum und wird nicht als Laufzeitgrenze durchgesetzt.

Typisierte `.def`-Register sind die Autorität für Features, Syscalls, Faults
und Source-ABI: `SBFSyscallRegistration.def`, `SBFValidationRules.def`, `SBFFaultCodes.def`,
`SBFSourceStatuses.def`, `SBFArgumentRegisters.def` und `SBFEdgeKinds.def`.
`SBFFaultCodes.def` stabilisiert die Execution-Fault-Werte;
`SBFSourceStatuses.def` besitzt getrennt die Generated-Source-ABI.
Der Loader arbeitet `raw-first`: relative CALLs zuerst, Raw-Relocations genau
einmal in ELF-Ordinalfolge; die stabile Fehlerreihenfolge ist Textidentität,
CALL, Relocation, Entrypoint und Read-only-Layout. File-/VM-Abbildung ist
gap-aware und erfindet in Lücken keine Bytes.

CFG und Dataflow sind funktionslokal: Call-Edges sind keine lokalen
Predecessors, Shared Tails bleiben mehrdeutig und alle Latches einer Schleife
bilden eine Multi-Latch-Region. Worklist und Ownership werden mit 10,000
Funktionen, rückwärts geordneten Blöcken und Conditional Latches geprüft, ohne
eine maschinenspezifische Laufzeit zu behaupten.

Die öffentliche SBF-Callgraph-API verwendet `callgraph-budget=fail-closed`:
typisierte Input-, Provenance-, Node-, Edge-, Element- und
`CallGraphOutputByteBudget`-Grenzen machen das JSON exakt oder leer. Bei
Erschöpfung liefert sie `{"nodes":[],"edges":[]}`, setzt `neverd_last_error()`
und veröffentlicht nie eine partielle Relation.

Jede Aktivierungszeile enthält Cluster, Feature-Account und Slot; ein
`RPC activation audit` kann sie mit einem Live-Node vergleichen, während die
normale Analyse offline bleibt. Der Vergleich umfasst Blueshift, `qedsvm`
(Lean-Beweise für ausgewählte Pfade, aber derzeit nur V0 im ELF-Loader),
`leanprover-solanalib`, `sol-azy`, `bn-ebpf-solana` und Ghidra/SolDragon.
`ezBPF` bezeichnet sich bei `88829078a6d7682a2baed0d696d500401c263750`
selbst als deprecated und verweist auf Blueshift; es ist ein archivierter
Vorgänger mit einer einzelnen Byte-zu-Enum-Map, kein versionsbewusster Decoder
für moved-memory, JMP32 und die heutige v0-v4-Matrix. Unter
die Vergleichs-Pins fallen Blueshift `704e40f7aa82446555b19d9ffbc0a6e18a35480f`,
`qedsvm` `99bd5ede85374adc7fc5c835c2432ecf4e123fd1` und
`leanprover-solanalib` `6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec`; die vier lokalen
Werkzeuge sind mit `sol-azy` `362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`,
`solana-data-reverser` `bf90923adec984a61ca0437e9d341360ac1b11ee`,
`SolDragon` `002b98677a5e595a773af6607b77210f5ea71db7` und
`bn-ebpf-solana` `c3fe0de45d37eb68dcb08f2498c6e1f986056572` fixiert.
den geprüften öffentlichen allgemeinen SBF-Decompilern besitzt NeverD in
diesem Snapshot die stärkste reproduzierbare Evidenz, die wir fanden; das ist
eine begrenzte Vergleichsaussage, kein absoluter „Weltmeister“-Anspruch.

Die Vergleichsaufnahme umfasst außerdem `r2ghidra-solana` bei
`eca0b8e2d307e00991e289b8f9b0f45743819f1b`; es bietet Ghidra-UX mit
`C-like-pdg` sowie Account-, Anchor-, String- und Syscall-Sichten. Das CI lief
am gepinnten HEAD durch, aber der Solana-spezifische Testsuite-Teil ist
auskommentiert und der CI-Smoke dekompiliert nur `/bin/ls`. Der direkte Reproducer
bestätigt: Das offizielle V0-`relative_call_sbpfv0.so` liefert plausibles C,
während das offizielle V3-`relative_call.so` bei `pdg` scheitert; der Befund ist
reproduzierbar. `radare2-solana` ist
bei `292d845681be377cadc9959a74c2cadeb6e7f412` gepinnt; sein Decoder erweitert
V2-only SIMD-0173/0174 als `>=V2` auf V3/V4, obwohl das offizielle `program.rs`
diese nur für V2 ausweist. `SBPF-3-1` bei
`0e602c93007faa96bccb8e1e12040954ff108b6f` hat 2/2 triviale cargo-Tests ohne
CI; die Versionserkennung ist ein none/V0-Platzhalter, der High-Nibble-Opcode-
Decoder ist falsch, und der Sprung verwendet imm statt off. Die beiden
relative_call-ELFs für V0/V3 erzeugen denselben falschen Pseudocode. NeverD
Vorteil ist die offizielle, reproduzierbare V0–V4-Evidenz aus Loader, Verifier,
Runtime und Process-Oracle; die bessere UX und C-Ausgabe der Werkzeuge bleibt
unberührt.

`SBFComparisonTools.def` ist die einzige Autorität für Anzeigenamen und volle
Revisionen der Vergleichswerkzeuge. Der abschließende begrenzte öffentliche
Sweep ergab zusätzlich:

- `blastrock/Solana-eBPF-for-Ghidra` bei
  `c3ad719004726fe924dbed901eca2744ad82c85d` bietet echte Ghidra-P-Code-UX,
  aber ein einziges unversioniertes SLEIGH-Modell fixiert CALLX auf `dst` und
  mischt Legacy-/Current-Opcodes. Echte Tests und CI fehlen; im Default-Quellbaum
  fehlt außerdem eine referenzierte Relocation-Constant-Klasse.
- `SolEmu-Ghidra` bei `6520af2ff104d5adbec24632ba3afa3bef0da529`
  übernimmt diesen byte-identischen Decoder und ergänzt Emulator-UX um explizit
  simuliertes oder als Placeholder markiertes CPI-, Krypto- und ZK-Verhalten;
  echte Tests und CI fehlen ebenfalls. `Ghidra_sBPF` bei
  `907bd4476432ca83bb2352686ad1ccafdb38504c` bietet manuell wählbare v1-v3-
  Sprachen, nimmt V2-only-Codierungen jedoch kumulativ in V3 auf, besitzt keine
  V0/V4-Autowahl und keine Tests oder CI.
- `solana-ebpf-ida-processor` bei
  `aacd215907266190ed9c6c1b408ca9309f92ecdd` ist eine nützliche IDA-
  Disassembler-/Relocation-UI, kein Source-Lifter; seine gemischte Opcode-Tabelle
  liest CALLX immer aus `imm`, Tests und CI fehlen. `solana-bpf-reverse` bei
  `39479a3bddb8cb866ee499266a76a1b54069b222` erzeugt aus hart codierten Layout-
  Annahmen heuristische Reports und Rust-TODO-Gerüste; der Lauf ergab 9 pass,
  2 fail und 1 skip, ohne CI.
- `solens` bei `22defa1c8f4118dacd42f5c291f1ac31609fc0e5` ist ein V2-only-
  Terminal-Disassembler mit 0 Tests und ohne CI. `sbpf-decompiler` bei
  `37b8bc0edc7ce347abee466f5f974e900c1948df` besteht derzeit aus einer
  dreizeiligen `Hello, world!`-Implementierung, 0 Tests und keiner CI.
- `sbpf-eye` bei `5277a52aeb58e50b6ff8f9020414334765369b49` ist ausdrücklich eine
  lightweight WIP-Instruktions-/CFG-TUI: 3 Tests bestehen, Semantic IR,
  Source-Emitter und CI fehlen. `svm_bytecode_analyzer` bei
  `12aa236db8964e6be661e38131c2dc81588cf19c` ist ein Disassembler/CFG-Analyzer,
  kein Lifter; Register-/Offset-Bytes werden falsch dekodiert, der Lauf ergab
  17 pass und 1 fail, ohne CI.
- `giraffexiu/Solana-eBPF-for-Ghidra` bei
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07` ist ein One-Commit-Snapshot
  derselben Ghidra-Linie ohne zusätzliche Versionssemantik, Tests oder CI.
  `CertSBF` bei `bb93a97cf0c64d119d08ec851e8e820315beb59e` ist eine wertvolle
  Isabelle/HOL-Formalisierung älterer rBPF-Semantik, kein aktueller
  Whole-Program-V0-V4-Source-Decompiler.

Diese Befunde stärken nur die Vergleichsevidenz im begrenzten öffentlichen
Snapshot; sie sind keine absolute Aussage über künftige oder private Werkzeuge.

Das abschließende RPC-Audit vom 2026-08-24 stimmte exakt überein: 38 Feature-
Accounts und 89 Aktivierungszeilen; Mainnet bei Slot 441305159, Testnet bei
433055669 und Devnet bei 487238699. Das leere, ausstehende system-owned Konto
(`VirtualAddressSpaceAdjustments` auf Mainnet) war nicht aktiviert. Eine RPC-URL
wird in dieser Dokumentation nicht fest verdrahtet.

Linux Release CI liest die exakten Pins mit `--print-pinned-revision`,
`--print-test-vectors-revision` und `--print-toolchain`, authentifiziert Oracle
und Sparse-Corpus und exportiert `NEVERD_SBPF_ORACLE` sowie
`NEVERD_AGAVE_CONFORMANCE_ROOT`; beide externen Tests sind dort Pflicht. Ein
normaler lokaler Lauf ohne explizite Oracle-/Corpus-Umgebung entdeckt die Fälle
weiterhin, darf sie aber überspringen.
