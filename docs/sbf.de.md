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
was NeverD meldet, jeweils mit dem Runtime-Identifier, dem Account, dessen
Existenz sie einschaltet, und dem Slot, an dem jedes Cluster sie aktiviert hat.
Ein Gate ohne Zeile für ein Cluster ist dort nicht aktiviert. `simd-0321` ist in
jedem Cluster an; `simd-0449` und der SHA-512-Syscall sind auf Testnet und
Devnet an und auf Mainnet aus — genau deshalb scheitert auf Mainnet ein
Programm, das auf Devnet funktioniert.

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
Antwort für alle Cluster auf einmal. `sol_alloc_free_` braucht überhaupt kein
Gate: Die Runtime bedient es weiter und nimmt zugleich kein neues Programm mehr
an, das es aufruft; das ist ein Unterschied zwischen den beiden Registries und
sonst nichts.

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
| Base58-Adressen in Read-only-Daten | Treffer in `SBFKnownAddresses.def` oder eine vom Code erzeugte Konstante |
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

Ein Aufruf einer Funktion, die diese Analyse nicht beschrieben hat, gilt als
Schreiber auf alles Erreichbare. Ein Aufgerufener läuft in einem eigenen Frame,
also lässt ein Aufruf, dessen Argumentregister nachweislich keinen
Scratch-Speicher adressieren, das Modell intakt; alles andere verwirft es.
`sol_invoke_signed_rust` und `sol_invoke_signed_c` schreiben Account-Daten statt
Aufruferspeicher, sodass zwei in einem Block zusammengebaute Invocations beide
lesbar bleiben.

Das Modell ist eine Vorwärts-Must-Analyse über den funktionsinternen CFG: Ein
Byte überlebt in einen Block nur, wenn jeder Pfad dorthin denselben Wert
geschrieben hat. Call-Kanten werden nicht verfolgt, weil ein Aufgerufener nichts
vom Frame seines Aufrufers erbt. Programme mit mehr als
`kMaxScratchFlowBlocks` Blöcken behalten die blockweise Rekonstruktion und
verlieren nur die Fakten über Blockgrenzen hinweg.

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
/* Über welche Runtime die Antwort spricht. Die Vorgaben beschreiben
   Mainnet-Beta im aktuellen Stand, unter loader-v3, für ein bereits deploytes
   Programm. */
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

## Aktuelle Konformitätsbasis (2026-08-10)

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
| Offizielles ELF-Manifest | 20/20 Artefakte aus `sbpf/tests/elfs` |
| ISA-Matrix | alle 256 Encodings für v0-v4, also 1,280 Zellen, plus Verifier-Grenzen |
| Ausführungsdifferenz | Raw-Byte-Oracle gegen LLVM ORC, C11 und stabiles Rust samt Memory/Fault/Syscall-Trace |
| Integriertes Aggregat | 145/145 Fälle in 14 Test-Binaries |
| ASan + UBSan | 141/141 Core-Fälle in 13 Binaries ohne Report |

Die Prüfung ist auf Anza `sbpf`
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` und Agave
`cae40aa610fdbdb313209bc1eec737079eb59688` fixiert. Zur Aktualisierung
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` und
`SBFUpstreamSources.def` prüfen und ausführen:

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
  cmake --build build --target check-neverd-sbf
```

Der Vergleich zeigte: `sol-azy` stürzt beim aktuellen strict ELF ab und lässt
einen undefinierten Legacy-CFG-Knoten zurück; `solana-data-reverser` behandelt
Account-Daten, `SolDragon` kennzeichnet die Analyse als WIP und
`bn-ebpf-solana` benötigt Binary Ninja. Offizielles `sbpf` und Agave bleiben
daher die semantische Autorität.
