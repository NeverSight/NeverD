**Sprachen**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM-Dekompilation

[← Dokumentationsindex](README.de.md)

NeverD lädt klassischen Bytecode der Ethereum Virtual Machine, erzeugt ein
eigenes 256-Bit-LowIR, Stack-SSA-MedIR und rekonstruiertes HighIR und gibt LLVM
IR, C23 oder Solidity aus. Strikte Analyse ist Standard: Ein nicht zugewiesener
oder im gewählten Hardfork inaktiver Opcode meldet einen Fehler an seinem
exakten PC.

Solidity und C sind semantische Rekonstruktionen. Opcode-Reihenfolge,
256-Bit-Arithmetik, Stack-Prüfungen und validierter Kontrollfluss bleiben
erhalten; ursprünglicher Quelltext, Bezeichner und Typen werden nicht behauptet.

## Schnellstart

```bash
# Verifiziertes LLVM IR mit i256/i512.
./build/bin/neverd lift contract.evm -o contract.ll

# Alle EVM-Analysestufen anzeigen.
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# C23 oder Solidity ausgeben.
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Historischen Opcode-Satz wählen oder unbekannte Bytes als Fehlerknoten behalten.
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`, `cfg` und Low/Med/High/LLVM-Abfragen der C-API akzeptieren ebenfalls
EVM. Binäres EVM-Rewriting wird ausdrücklich abgelehnt; `patch` bleibt nativ.

## Akzeptierte Eingaben

| Eingabe | Erkennung und Normalisierung |
|---------|------------------------------|
| Rohbytes | `.raw`, `.evmraw` oder binärer Inhalt mit expliziter EVM-Erweiterung |
| Hextext | Optionales `0x`, beliebiger ASCII-Whitespace, `.evm`, `.hex`, `.bin`, `.bytecode`; validiertes Hex ohne Erweiterung wird ebenfalls erkannt |
| Compiler-Artefakt | `.json` mit `deployedBytecode`, `runtimeBytecode` oder `bytecode` an der Wurzel oder unter `evm`; solc-Standard-JSON `contracts → file → contract → evm` wird unterstützt |

Runtime-/Deployment-Bytecode hat Vorrang vor Creation-Code. Ist nur Creation
vorhanden, erkennt NeverD begrenzte konstante `CODECOPY`/`RETURN`-Wrapper und
extrahiert den kopierten Runtime-Bereich. Ein Feld mit nur optionalem `0x` gilt
als leer; ein leeres Runtime-Feld verdeckt keinen nutzbaren Creation-Fallback.
Eine abschließende Solidity-CBOR-Map wird nur entfernt, wenn Länge, Map-Marker
und ein bekannter `solc`-, `ipfs`- oder Swarm-Schlüssel gültig sind.

Fehlerhaftes Hex, ungerade Ziffern, ungelöste Linker-Platzhalter, mehrdeutige
Multi-Contract-Artefakte, ungültige Metadata-Grenzen und leerer Code erzeugen
verwertbare Fehler. `BytecodeLoadOptions::ArtifactContract` wählt `Contract`
oder `path/File.sol:Contract`. Bei gleichen Namen in mehreren Quelldateien wird
der unqualifizierte Name abgelehnt, damit JSON-Reihenfolge nie falsch auswählt.

EVM ist im zentralen Loader-Register statt in einem Backend-Plugin registriert.
CLI, C-API, Disassembler, CFG und IR-Abfragen nutzen dadurch dieselbe
normalisierte Abbildung und dieselben Optionen.

## Hardforks und Opcodes

Alle 150 zugewiesenen Legacy-Opcodes von Frontier bis Fusaka sind abgedeckt,
einschließlich `PUSH0`, transientem Storage, `MCOPY`, Blob-Opcodes und `CLZ`.
`latest` wählt standardmäßig Fusaka.

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

Aliasnamen `dao`, Schreibweisen mit Unterstrich, `merge`, `prague` und `osaka`
sind erlaubt. `latest` und `osaka` lösen derzeit zur kanonischen Revision
`fusaka` auf.

`latest` meint die jüngste finalisierte Mainnet-Revision in NeverD, nicht die
Spitze der Ethereum-Entwicklung. [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)
ist für Q4 2026 vorgesehen; die noch im Review befindlichen Instruktionen
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) und
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) werden nur mit
`--evm-hardfork=amsterdam` (oder `bogota`) aktiviert und bleiben bis zur
Finalisierung außerhalb von `latest`. Bei EIP-8024 wird nur ein gültiges
Immediate konsumiert; ein ungültiger Kandidat bleibt die nächste Instruktion.

EOF wurde im
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2)
entfernt und ist in execution-spec-tests als
[aus Osaka entfernt und nicht geplant](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md)
vermerkt. NeverD behandelt den zurückgezogenen Entwurf nicht als Mainnet-Regel.

Strict lehnt unbekannte und fork-inaktive Bytes ab. `--evm-relaxed` bewahrt sie
in LowIR und Diagnosen, aber Backends lösen bei Ausführung einen Fehler aus;
unbekannte Bytes werden nie still zu NOP.

## Metadata-Architektur im LLVM-Stil

Handgeschriebene EVM-Metadata folgt LLVMs mehrfach eingebundenem `.def`-Muster:

- `EVMOpcodes.def` ist die einzige Wahrheit für 150 finalisierte und vier
  opt-in Entwicklungs-Opcodes: Encoding, tatsächliche Pop/Push-Änderungen,
  Immediate-Art, Klasse, Aktivierungs-Fork,
  Haupteffekt, orthogonaler EVM-Memory-, Source-State- und Call-Value-Zugriff
  sowie Terminierung stehen in jedem Record; es gibt keine stillen Defaults.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def` und
  `EVMCallValueAccesses.def` definieren geschlossene typisierte Domänen. `CALL`
  kann externer Call und Memory Read/Write sein, `EXTCODECOPY` Context Read und
  Memory Write. State verwendet die Lattice `None/Read/Write/Unknown`.
  Payability ist unabhängig: `CALLVALUE` führt normalerweise zu `payable`.
  Nur wenn der Analyzer den kanonischen `ISZERO(CALLVALUE)`-Guard und `REVERT`
  im Nonzero-Zweig beweist, wird dieser compiler-generierte Read unterdrückt.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` und
  `OutputLanguages.def` erzeugen geordnete Enums, Parser, Anzeigenamen,
  CLI-Auswahl und C-ABI-Werte. `EVMConstants.h` besitzt Breiten und Limits.
- `Semantics.h` enthält den zielunabhängigen Scalar-ALU-Evaluator. Interpreter
  und Constant Folding teilen die geprüfte `APInt`-Implementierung; LLVM-, C-
  und Solidity-Lowering bleiben explizit und fail-loud.

Der Decoder ist die Rohbyte-Grenze. Zugewiesene Identität und Fork-Aktivierung
sind getrennt: Relaxed Decode erhält Name, Einführungs-Fork und Immediate-Breite
eines inaktiven Opcodes, gibt ihm aber konservative Fehlersemantik. Damit
verschiebt ein inaktives Immediate keine späteren Grenzen. Analyse, Interpreter
und Emitter verwenden das generierte `Opcode` und Metadata-Abfragen; Raw Encoding
erscheint nur an Trace-/Host-ABI-Grenzen. Die 17 Stack-Inputs von `SWAP16` und
maximal 7 Host-Argumente sind getrennt compile-time abgeleitet.

`OpcodeInfo` kann nicht halb gültig default-konstruiert werden; sein Name ist
ein `llvm::StringLiteral`. Der Compile-time-Validator prüft doppelte Encodings,
unbekannte Properties, ALU-Verträge, Effect/State-Konsistenz,
PUSH/DUP/SWAP/LOG-Familien, Terminatoren und Host-Resultate. Nur eine explizite
Factory erzeugt konservative Unknown-Metadata.

Die `.def`-Dateien sind handgepflegte Datenbanken wie LLVMs
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def).
`.inc` bleibt echten generierten oder literalen Include-Fragmenten vorbehalten.
Reichere deklarative Records stehen in `.td`; daraus erzeugt
[TableGen](https://llvm.org/docs/TableGen/ProgRef.html) `.inc`. Weil NeverD noch
keinen EVM-TableGen-Schritt hat, wäre ein `.inc` ohne Generator nur Zeremonie.
Der C++-Code folgt den [LLVM-Richtlinien](https://llvm.org/docs/CodingStandards.html),
LLVM-ADT/String-Typen an Grenzen und vollständigen fail-loud Switches.

Ein neuer Opcode benötigt einen vollständigen `EVM_OPCODE`-Record, gemeinsame
Scalar-Semantik, explizite Backend-Lowerings und fokussierte Tests. Ein Hardfork
benötigt einen geordneten `EVM_HARDFORK`-Record und Aliase. Typisierte API,
Lookup, Validierung, Klassifikation und CLI wachsen ohne Paralleltabellen.

## Analysemodell

- **LowIR** erhält PC, Encoding, PUSH-Immediates (rechts mit Null aufgefüllt),
  Blöcke, Kanten, validierte `JUMPDEST`-Ziele, Erreichbarkeit und Stack-Höhe.
- **MedIR** bildet Stack-Werte als 256-Bit-SSA ab, erzeugt Merge-Phis, faltet
  reine Operationen und hält Effect, Memory, State und Call-Value orthogonal.
- **HighIR** rekonstruiert Dispatcher-Selector, wahrscheinliche Calldata-/Return-
  Wörter, Mutability, konstante Slots, Events, Reverts und Function-/CFG-Regionen
  best-effort. Payability und State-Lattice sind unabhängig. Ein erreichbarer
  ungelöster Jump joint zu `Unknown` und macht Solidity konservativ
  `nonpayable`; widersprüchliche Selector-Muster werden diagnostiziert und
  weggelassen.
- **LLVM** emittiert eine verifier-saubere `i32 @evm_execute(ptr)`-State-Machine
  mit geprüftem 1024-Wort-`i256`-Stack, `i512`-Zwischenwerten, geschützter signed
  division, saturierenden Shifts, exaktem `BYTE`/`SIGNEXTEND`/`CLZ` und
  validierten Dynamic-Jump-Switches.

Der deterministische Interpreter ist der semantische Oracle. LLVM/C werden
ausgeführt und verglichen; Solidity wird in Anvil deployt und anhand Storage und
Trace verglichen. Ein Pre-Fusaka-Raw-Corpus läuft zusätzlich in Anvils nativer
EVM und prüft ALU, Calldata Copy, überlappendes `MCOPY`, Memory Expansion,
Keccak und Return Data unabhängig.

Account-Operanden werden gemäß
[Execution Spec](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py)
auf 160 Bit maskiert; Environment-Breiten werden vor der Ausführung validiert
und `BLOCKHASH` beachtet das 256-Block-Fenster. Der EIP-211-Return-Data-Puffer
ist vom finalen Frame-Output getrennt: Nur `RETURN`/`REVERT` setzen
`ExecutionResult::ReturnData`; CREATE/CREATE2 folgen derselben Regel.

## Vertrag des erzeugten C

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

Umgebungsoperationen verwenden folgende Host-ABI. `a0` ist der ursprüngliche
Stack-Top, ungenutzte Argumente sind Null und der Rückgabewert ist das erste
gepushte Wort. Der Trace-Hook läuft vor jeder Instruktion.

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

Das Frontend muss mindestens 512-Bit-`_BitInt` unterstützen. Apples Darwin-
Clang tut das noch nicht; auf macOS ist ein passendes Non-Darwin-Target oder
direkt NeverDs LLVM-Ausgabe nötig.

## Vertrag des erzeugten Solidity

Die Ausgabe kombiniert Selector-spezifische Function-/Storage-/Event-/Error-
Deklarationen mit einer exakten PC-/Stack-State-Machine. Ein konstanter Slot
wird etwa `recovered_storage_slot_3 = uint256(0x3)`, niemals eine erfundene
sequenzielle State-Variable.

Der Contract ist absichtlich `abstract`. `_evmHost` wird für Umgebungseffekte
überschrieben; `_evmTrace` ist virtuell und emittiert standardmäßig `EVMTrace`.

```bash
solc --bin contract.sol
```

## C-API

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

`neverd_decompile_all` bleibt kompatibel und liefert C. Neu sind
`neverd_session_bitness`, `neverd_evm_set_strict`,
`neverd_evm_set_hardfork` und `neverd_decompile_all_ex`. Solidity für Native,
der alte LLVM-to-C-Pfad für EVM und Native-Object-Roundtrip für EVM werden
ausdrücklich abgelehnt und nicht ignoriert.

## Explizite Grenzen

- Nur Legacy-Bytecode; EOF-Container werden nicht dekodiert.
- Amsterdam/Bogota sind explizite Entwicklungsziele; `latest` bleibt bis zur
  Finalisierung der geplanten Opcodes beim finalisierten Fusaka.
- Kein RPC, Chain-State-Discovery, Gas/Refund oder Precompile-Ausführung.
- Creation-Extraktion erkennt übliche statische Wrapper, keinen vollen Constructor.
- Dynamische Sprünge bleiben indirekt, wenn keine begrenzte Konstantenanalyse sie beweist.
- ABI-Typen, Namen, Mappings, Events und Custom Errors sind Best-effort.
- Eigenständige Ausführung von Effekten benötigt C-/Solidity-Host-Hooks.
