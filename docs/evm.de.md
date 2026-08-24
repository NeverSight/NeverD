**Sprachen**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM-Dekompilation

[← Dokumentationsindex](README.de.md)

NeverD lädt klassischen Bytecode der Ethereum Virtual Machine, erzeugt ein
eigenes 256-Bit-LowIR, Stack-SSA-MedIR und rekonstruiertes HighIR und gibt LLVM
IR, C23 oder Solidity aus. Strikte Analyse ist Standard, doch Legacy-EVM
validiert nicht alle Bytes eines Images als Opcodes: Abgelehnt wird nur eine
definitiv `Reachable` Ausführungs-Lane, die einen nicht zugewiesenen oder im Fork
inaktiven Opcode erreicht, und zwar an dessen exaktem PC. Tote Bytes und nur
`MayReachable` CFG-Kandidaten werden nicht zu Strict-Fehlern.

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
extrahiert den kopierten Runtime-Bereich. Der Constructor-Durchlauf verwendet
denselben Einzelinstruktions-Decoder wie der echte Decoder, unter dem
analysierten Hardfork; ein Byte, das auf einem Fork Daten und auf einem anderen
ein Opcode ist, kann die Grenze daher nicht verschieben. Ein vorhandenes
`deployedBytecode`- oder `runtimeBytecode`-Feld ist maßgeblich: Ein explizites
`0x` gilt als leeres, natürlich stoppendes Runtime-Programm und verhindert
absichtlich den Rückfall auf Creation-Code. Ein fehlendes Feld darf zum nächsten
Kandidaten übergehen; fehlendes oder ausschließlich aus Whitespace bestehendes
Hex ohne expliziten Präfix wird abgelehnt. Explizite Roheingabe darf ebenfalls
leer sein.

### Compiler-Trailer

`EVMMetadataFields.def` führt beide Trailer-Formate. Solidity schreibt eine
CBOR-Map, deren zwei Schlussbytes nur die Map zählen; `vyper` schreibt ein
CBOR-Array, das mit dieser Map endet und dessen zwei Schlussbytes den gesamten
Footer inklusive ihrer selbst zählen. Die eine Rahmung als die andere zu lesen
scheitert nicht laut — es landet zwei Bytes daneben und entfernt zwei Bytes
echten Code —, deshalb werden beide versucht, und eine Eingabe, die zu keiner
passt, bleibt unangetastet.

Der Trailer wird zweimal gelesen: einmal auf der Eingabe wie gegeben und einmal
auf dem Runtime-Code, der nach dem Auspacken eines Deployment-Wrappers übrig
bleibt. Vyper hat seinen Trailer in den Initcode verschoben und lässt den
Runtime-Code ohne einen; ein Leser, der erst nach dem Auspacken hinsieht, meldet
deshalb einen unbekannten Build für einen Contract, der sich selbst benannt hat.
Ein Sequenz-Footer nennt zusätzlich die Länge des Runtime-Codes, die Längen der
Datensektionen und die Länge der Immutables, was den zurückgegebenen Code
begrenzt, ohne den Constructor auszuführen.

### Container, die keine Instruktionen sind

`EVMBytecodeContainers.def` klassifiziert die Eingabe vor jedem Decode. Seit
EIP-3541 `0xEF` undeploybar gemacht hat, verspricht ein führendes `0xEF`, dass
die Bytes keine Instruktionen sind:

| Container | Marker | Behandlung |
|-----------|--------|------------|
| legacy | — | wird als Instruktionen dekodiert |
| delegation (`eip-7702`) | `0xef0100` und genau 23 Byte | meldet das Zielkonto; Analyse endet |
| eof (`eip-3540`) | `0xef00` | abgelehnt; kein Fork hat es aktiviert |

Die zwanzig Bytes eines Delegation-Indikators sind eine Adresse, kein Code. Sie
zu dekodieren läse die Adresse als Opcodes und erzeugte den Kontrollflussgraphen
eines Kontos; deshalb meldet `info` das Ziel und die Analyse verweigert mit
Begründung. Die Verweigerung unterscheidet beide Fälle: Vor Pectra ist der
Marker noch nicht vergeben, ab Pectra fehlt schlicht der Runtime-Code des Ziels.
Ein Marker in jeder anderen Länge ist fehlerhafte Eingabe statt einer Variante
des Containers und bleibt Instruktionen, damit der Decoder das Byte benennen
kann, das er nicht lesen konnte.

Fehlerhaftes Hex, ungerade Ziffern, ungelöste Linker-Platzhalter, mehrdeutige
Multi-Contract-Artefakte, ungültige Metadata-Grenzen sowie fehlendes oder leeres
Hex erzeugen verwertbare Fehler. Eine explizit leere Roheingabe oder ein
`0x`-Runtime-Feld bleibt dagegen ein gültiges leeres Programm.
`BytecodeLoadOptions::ArtifactContract` wählt `Contract`
oder `path/File.sol:Contract`. Bei gleichen Namen in mehreren Quelldateien wird
der unqualifizierte Name abgelehnt, damit JSON-Reihenfolge nie falsch auswählt.

EVM ist im zentralen Loader-Register statt in einem Backend-Plugin registriert.
CLI, C-API, Disassembler, CFG und IR-Abfragen nutzen dadurch dieselbe
normalisierte Abbildung und dieselben Optionen.

## Hardforks und Opcodes

Der finalisierte Legacy-Opcode-Satz ist von Frontier bis Fusaka abgedeckt,
einschließlich `PUSH0`, transientem Storage, `MCOPY`, Blob-Opcodes und `CLZ`.
Die für Amsterdam geplanten Opcodes sind nur über ein ausdrückliches
Entwicklungs-Fork-Ziel verfügbar; `latest` bleibt Fusaka.

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
entfernt. EOFv1/EIP-7692 ist nicht eingeplant; der Containerentwurf
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540) ist Stagnant. Das alte
Repository `execution-spec-tests` ist archiviert, die gepflegten Tests sind nach
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests)
umgezogen. NeverD gibt daher keinen experimentellen EOF-Container als Mainnet-Regel aus.

Strict lehnt ein unbekanntes oder fork-inaktives Byte nur ab, wenn eine definitiv
`Reachable` Lane beweist, dass die Ausführung es erreicht. `--evm-relaxed`
bewahrt solche Bytes als typisierte Fehlerpräfixe und in Diagnosen; Backends
lösen bei Ausführung weiterhin einen Fehler aus, niemals still einen NOP.

## Metadata-Architektur im LLVM-Stil

Handgeschriebene EVM-Metadata folgt LLVMs mehrfach eingebundenem `.def`-Muster:

- `EVMOpcodes.def` ist die einzige Wahrheit für jeden finalisierten Legacy- und
  jeden opt-in Entwicklungs-Opcode: Encoding, tatsächliche Pop/Push-Änderungen,
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
- `EVMImmediateKinds.def` definiert PUSH-Daten fester Breite und EIP-8024s
  bedingte Single-/Pair-Encodings; `EVMDecodeStatuses.def` besitzt das stabile
  Vokabular, das LowIR und Disassembly veröffentlichen.
  `EVMUpstreamOpcodePolicy.def` verzeichnet den go-ethereum-Namensalias und die
  absichtlichen historischen und nicht eingeplanten EOF-Ausschlüsse. Die
  orthogonale `EVMUpstreamSemanticsPolicy.def` ordnet Forks `params.Rules` zu,
  benennt Ausnahmen der grundlegenden Stack-Vorprüfung und klassifiziert
  Dynamic-Immediate-Familien. Das Audit lehnt Drift bei Byte, Aktivierung,
  `base_min_stack` und `net_stack_delta` sowie jede neue ungeprüfte
  Upstream-Konstante ab.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` und
  `OutputLanguages.def` erzeugen geordnete Enums, Parser, Anzeigenamen,
  CLI-Auswahl und C-ABI-Werte. `EVMAnalysisLimits.def`,
  `EVMInterpreterLimits.def`, `EVMABIParserLimits.def` und
  `EVMABITableLimits.def` deklarieren die stufenspezifischen Grenzen für
  Analyse, Interpreter, Parser und öffentliche Tabellen. `EVMConstants.h`
  besitzt gemeinsame Protokollbreiten und stabile interne Namen und erzeugt aus
  `EVMAnalysisLimits.def` die Analyse-Standardwerte und Diagnoseoptionsnamen;
  Interpreter- und ABI-Header erzeugen die Grenzen aus ihren eigenen Tabellen.
- `EVMCalls.def` beschreibt die vier Instruktionen, die ein anderes Programm
  aufrufen, und das Gitter der Herkünfte einer Callee-Adresse. Ein einziges Flag
  pro Eintrag, ob ein Value-Operand zwischen Callee und Argumentfenster liegt,
  leitet jede spätere Operandenposition ab; die Tabelle wird gegen die
  Opcode-Datenbank geprüft, damit die Ableitung nicht von den deklarierten
  Pop-Zahlen abweicht.
- `EVMPrecompiles.def` ist das Verzeichnis der Adressen, an denen das Protokoll
  selbst antwortet, jeweils mit dem Fork, der sie reserviert hat, und dem
  Vorschlag, der sie eingeplant hat. `P256VERIFY` an `0x100` wird `eip-7951`
  zugeschrieben, dem finalen Vorschlag, der die Adresse mit Fusaka im Mainnet
  reserviert hat; der Rollup-Vorschlag, aus dem ihre Schnittstelle stammt, hat
  sie nie eingeplant. Gas fehlt absichtlich: Die Kosten einer Precompile sind
  eine Funktion ihrer Eingabe und wurden mehrfach neu bepreist, ohne dass
  Adresse oder Operation sich änderten.
- `EVMMetadataFields.def` und `EVMBytecodeContainers.def` beschreiben, was eine
  Eingabe ist, bevor sie dekodiert wird: die beiden Rahmungen des
  Compiler-Trailers und die Container, deren Bytes überhaupt keine Instruktionen
  sind.
- `EVMRecoveredFacts.def` besitzt die Schreibweisen der
  Rekonstruktionsvokabulare, damit ein Name, der in der Ausgabe erscheint, an
  einer Stelle lebt statt in einem `switch`, in dem ein neuer Enumerator fehlen
  kann. `EVMKnownSignatures.def` speichert kanonische Schreibweise und Selector
  jeder Funktion einmal und deklariert getrennte Standard-Mitgliedschaften als
  `KnownFunctionVariantInfo`, jeweils mit Return-Liste und
  independent/non-independent Beweisrolle. Eine von ERC-20 und ERC-721 geteilte
  Signatur bleibt damit ein aufrufbarer Kandidat, beweist allein aber keinen
  Standard und übernimmt nicht den Rückgabetyp der ersten Variante. Events und
  Custom Errors behalten eigene typisierte Records.
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

- **EVM LowIR** erhält PC, Encoding, typisierten Immediate-Status und dekodierte
  Stack-Tiefenoperanden (einschließlich Rechts-Nullauffüllung von PUSH und der
  bedingten Verbrauchsregel aus EIP-8024), Blöcke, Vorgänger-/Nachfolgerkanten,
  validierte `JUMPDEST`-Ziele, Erreichbarkeit und Stack-Höhendomänen. Die
  CFG-Rekonstruktion ist ein deterministischer, programmweiter Fixpunkt: Pro
  Stack-Slot wird eine beschränkte endliche Menge von 256-Bit-Werten propagiert,
  pro konkreter Höhe bleibt ein abstrakter Stack erhalten. Konstanten über
  Internal-Call-/Return-Blöcke, Stack-Shuffles, `PC`/`CODESIZE` und skalare
  ALU-Operationen können so ein oder mehrere konkrete Sprungziele auflösen. Ein
  wirklich unbekanntes Ziel bleibt eine explizite indirekte Kante.

  Auf einem Back-Edge wird ein geänderter loop-carried Slot semantisch zu `Top`
  überapproximiert, damit der Fixpunkt konvergiert; diese Rekurrenzabstraktion ist
  unabhängig von Ressourcen. `MaxAbstractValuesPerSlot`,
  `MaxStackHeightVariants`, `MaxAbstractInstructionTransfers` und die Grenzen
  für Instruktionen, Blöcke, Zustände, Werte, Stacks, Lanes, Edges und Worklist
  sind benannte Budgets. Null oder Erschöpfung ist vor dem
  Einfügen ein harter Fehler, niemals zusätzliches Emergency-Widening oder stilles
  Abschneiden.

  `EVMLowFaultKinds.def::InvalidJumpDestination` bleibt bei einem
  `end-of-code JUMPI` pfadsensitiv: Eine sicher wahre Bedingung mit ungültigem
  Ziel besitzt keinen erfolgreichen Nachlauf und ergibt einen definitiven
  Fehler; eine sicher falsche Bedingung ist erfolgreich. Bei Unknown bleibt nur
  der möglicherweise erfolgreiche False-Pfad erhalten, ohne die ganze Lane
  fälschlich als definitiven Fehler zu markieren.
- **EVM MedIR** bildet jeden Stack-Wert als 256-Bit-SSA-Wert ab und verdrahtet
  alle Merge-Phis, bevor eine deterministische Sparse-Constant-Worklist läuft.
  Das private Gitter ist `Uninitialized`, eine exakte `Constant` oder
  `Overdefined`: Gleiche Konstanten propagieren über Blöcke und verankerte
  Phi-Zyklen, während ein widersprüchlicher oder laufzeitabhängiger Zyklus keine
  Konstante erfinden kann. Die Worklist prüft Def-Use-IDs und verwendet denselben
  ALU-Evaluator aus `Semantics.h` wie der Interpreter. MedIR erhält außerdem den
  primären semantischen Effekt sowie orthogonalen EVM-Speicherzugriff
  `none/read/write/readwrite`, Quellzustands- und Call-Value-Zugriff. Jede
  Whole-Stack-Lane des LowIR behält eine eigene SSA-Execution-Lane; Phis nennen
  ihre Source-Lane. Inkompatible Stacks werden nicht an der Maximalhöhe ausgerichtet.
- **EVM HighIR** rekonstruiert Solidity-Dispatcher-Selector, wahrscheinliche
  Calldata- und Return-Wörter, Mutability, konstante Storage-Slots, LOG-/Event-
  Fakten, Revert-Fakten und Function-/CFG-Regionen. Ein geprüfter Producer-Index
  und ein iterativer, memoizierter Value-Walk gewinnen Fakten aus typisierten
  MedIR-Operanden statt aus Instruktionsabständen: Selector-Vergleiche dürfen
  Blöcke und Phis überqueren, beide `EQ`-Operandenreihenfolgen verwenden und eine
  abgeleitete 32-Bit-Maske behalten; Argument-Offsets, Storage-Keys, Event
  topic0, Non-Payable-/Receive-Guards und exakte 32-Byte-Return-Größen nutzen
  ihre semantischen Eingaben. Der Walk ist durch den MedIR-Graphen strukturell
  begrenzt und behandelt fehlerhafte, gemischte oder zyklische Ausdrücke als
  unbekannt. Widersprüchliche Ziele desselben Selectors werden diagnostiziert
  und weggelassen. Payability bleibt unabhängig vom State-Access-Gitter; ein
  erreichbarer ungelöster dynamischer Sprung erzwingt konservatives
  `nonpayable`. Der bytegenaue flusssensitive Speicher-Dataflow verfolgt konstante
  Writes blockübergreifend, kombiniert Overlap/Kill und invalidiert Wissen bei
  dynamischen oder unbekannten Writes. Nachgewiesen sind derzeit Selector und
  bekannte Panic-Bytes. Für eine bekannte Custom-Error-Deklaration behält der
  Solidity-Emitter kanonische Parametertypen; er behauptet nicht, jeden Runtime-
  Argumentwert zu rekonstruieren. Andere Fakten bleiben Evidenzkandidaten.

  Die Selector-Suche beginnt nur an der Root-Lane und folgt den
  Nichttreffer-Kanten des Dispatchers; ein selectorähnlicher Vergleich in einem
  Handler wird nicht zur öffentlichen Funktion. Receive und Fallback sind
  ebenfalls auf die Root-Lane beschränkt und verlangen ein definitiv
  erreichbares erfolgreiches Ende: Revert, Fault, ein nicht zahlbarer
  Empty-Calldata-Handler oder ein nur möglicher Pfad beweisen keinen Entry. Eine
  unvereinbare Calldata-Nutzung verwirft den kanonischen Kandidaten, und ein
  geteilter Selector ist kein unabhängiger Standardbeleg. Erst genügend
  kompatible unabhängige Selectors oder starke Evidenz durch exakte Topic/Arity,
  Storage-Slot oder Proxy wählen Standard und Variante. Eine statische
  Return-Liste wird nur ausgegeben, wenn alle definitiv erreichbaren
  erfolgreichen Enden auf dieselbe exakte ABI-Bytezahl kommen; ungelöste
  Transfers, widersprüchliche Formen oder Mismatches scheitern geschlossen.
  Revert und Fault sind keine erfolgreichen Returns.

  HighIR budgetiert Funktionen, Lane-/Operationsbesuche,
  Blockreferenzen in Regionen, Memory-Anfragen und -Bytes, Zustandszellen und
  Worklist-Updates getrennt. Der Memory-Fixpunkt verarbeitet nur definitiv
  erreichbare Ausführungs-Lanes, bildet einen Byte-Konsens-Meet und liefert bei
  Erschöpfung einen harten Fehler, ohne Fakten abzuschneiden.

  HighIR erfasst außerdem die ausgehende Hälfte der Schnittstelle: jeden `CALL`,
  `CALLCODE`, `DELEGATECALL` und `STATICCALL` mit der Herkunft des Callees, der
  reservierten Adresse, die er benennt, wenn der analysierte Fork dort eine
  reserviert, dem Selector, den der Aufruf an den Anfang der Callee-Calldata
  schreibt, und dem übertragenen Wert, sofern er konstant ist. `CREATE` und
  `CREATE2` bleiben ausgeschlossen: Sie führen Code aus, der noch keine Adresse
  hat, es gibt also keinen Callee zu rekonstruieren.

  Eine rekonstruierte ausgehende Signatur zählt nie zu den Standards, denen das
  Programm selbst antwortet. `transfer(address,uint256)` zu senden sagt, dass
  das Programm einen Token benutzt, nicht dass es einer ist; beides zu
  vermengen würde jeden Router und Vault als ERC-20 melden. Ein delegierender
  Aufruf wird zusätzlich als Proxy-Fakt gemeldet, weil nur bei ihm der Code des
  Callees gegen den eigenen Storage dieses Programms läuft.

  Die Precompile-Suche richtet sich nach dem analysierten Fork, nicht nach dem
  neuesten existierenden. Der Aufruf einer Precompile-Adresse, die erst ein
  späterer Fork einführt, erreicht ein Konto ohne Code, gelingt und liefert
  nichts zurück; sie zu benennen würde eine Operation melden, die das Programm
  nachweislich nicht ausgeführt hat.
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

Vor jedem opcode-spezifischen Effekt prüft der Interpreter die typisierte
Mindesthöhe, Pops sowie erhaltene Höhe plus Pushes vorab; Underflow oder Overflow
können keine halbe Instruktion ausführen. `EVMForkSemantics.def` wählt für Byte
`0x44` vor Paris `DIFFICULTY` und ab Paris `PREVRANDAO`. `REVERT`, semantische
Faults, Step-Limit und Ressourcenerschöpfung durch Allocation/Length setzen
Storage, transienten Storage, Logs und Selfdestruct-Effekte auf den
Eingabe-Snapshot zurück und bewahren Frame-Diagnose sowie explizite Revert-Bytes.
Ein Allokationsfehler wird ohne neue Fehlerstring-Allokation als
`ExecutionFaultKind::ResourceExhausted` markiert. Konnte selbst der Snapshot
nicht erzeugt werden, ist `HasPersistentStateSnapshot` false und das Ergebnis
niemals committable.

### Öffentliche IR- und Ressourcengrenzen

Die öffentliche Funktion `execute` prüft zuerst, dass
`Code`/`Fork`/`Instructions`/`JumpDestinations` kanonisches LowIR bilden. Ein
veränderter Fork, gefälschter Instruktionseintrag, widersprüchliches Encoding
oder eine falsche Sprungzieltabelle liefert daher `llvm::Error`, bevor der
Interpreter die Instruktionstabelle indiziert. Das öffentliche `lowerToMedIR`
prüft der Reihe nach Optionen, Ressourcen und Struktur; danach dekodiert ein
`canonical decode replay` den `Low.Code` mit eingebettetem Fork/Strictness und
vergleicht jedes LowIR-Feld. Erst dann darf `lowerCanonicalLowToMedIR` laufen
oder dürfen Indizes und aufruferproportionale Ausgaben entstehen. Öffentliches
`recoverHighIR` replay-validiert externe LowIR/MedIR ebenso. Die privaten Pfade
`lowerCanonicalLowToMedIR` und `recoverCanonicalHighIR` sind nur für
`analyze`-eigenes IR; sie überspringen lediglich redundantes, nichtrekursives
Replay, während alle HighIR option/resource budgets verpflichtend bleiben.

Der Dispatcher-Beweis hält pro `MedStateLane` eine sortierte
`Any/Exact/Excluded`-Selector-Domäne. Joins vereinigen Exact-Mengen, schneiden
Excluded-Ausschlussmengen und ziehen eine Exact-Menge von einem kofiniten
Ausschluss ab; eine aufgeweitete Domäne stellt die Lane erneut an. Ein
Gleichheitstest erfasst den Kandidaten der True-Kante nur, wenn der Selector
zulässig ist, und schließt ihn auf der False-Kante aus. Rohes
`XOR(selector, constant)` erfasst die Null-/False-Kante als Treffer, wenn alle
kanonischen Nachfolger denselben Einstieg benennen; dieser Fallthrough muss
kein `JUMPDEST` treffen. Die Nichtnull-/True-Kante ist der Nichttreffer und
schließt den Selector aus; `ISZERO` macht denselben Ausdruck zum Gleichheitstest.
Selector-Wort, Null-Calldata-Wort, Calldata-Größe und Call-Value-Guard werden
kantenweise verfeinert. Eine unbekannte Bedingung beendet den Beweis, statt
einen nur möglichen Zweig zu verfolgen.

Nach Erkennung eines Funktionskandidaten setzt der Function-Scope-Traversal mit
dessen `exact singleton selector` fort. Springt die Funktion in den gemeinsamen
Dispatcher zurück, folgen `SelectorEquality`, rohes `XOR` und `SelectorWord`
nur dem zum bereits gematchten Selector passenden `definite edge`. Unknown oder
unabhängige Prädikate behalten konservativ alle `definite edges`. Eine
Heuristik, die andere Entry Blocks ausschließt, ist verboten: legitimer
`shared body/tail-call`-Kontrollfluss bleibt im Function Scope.

Externe CALL/CREATE-Ergebnisse sind davon getrennt: Das Host-Ergebnis ist
tatsächlich nichtdeterministisch, daher untersucht die Analyse beide präzisen
CFG-Kanten. So bleibt die ERC-1167-Fallback-Recovery erhalten, ohne eine
unlesbare Selector-Bedingung als Evidenz zu behandeln; ein wirklich Unknown
Dispatcher schlägt weiterhin geschlossen fehl.

`EVMAnalysisLimits.def` gibt linearem Decoder und CFG-Builder über
`MaxLowDiagnostics` und `MaxLowDiagnosticBytes` ein gemeinsames aggregiertes
LowIR-Diagnosebudget. Beide Pfade belasten exakte Anzahl und finale Bytes vor und
verwerfen ein Null-Limit. LowIR- und HighIR-Diagnosebudgets bleiben unabhängig.
Dieselbe Tabelle berechnet `MaxHighDispatchCandidates`, das
programmweite Aggregat `MaxHighRecoveredArguments`, `MaxHighDiagnostics` und
`MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells` sowie `MaxHighMemoryValueVisits` unabhängig.
Kandidaten und wiederhergestellte Argumenteinträge werden vor dem Einfügen in
einen Zielcontainer und vor jeder Namens-/Typallokation vorbelastet. Jede
HighIR-Ausgabediagnose wird vor Konstruktion oder Kopie nach Anzahl und finalen
Nachrichtenbytes berechnet, einschließlich der festen Malformed-IR-Diagnose.
Eine erschöpfte Grenze liefert den benannten harten Fehler, ohne Diagnose oder
Fakten still zu verwerfen.
Die standardmäßige Root-CFG-Region belastet `MaxHighRegionBlockReferences`,
bevor ihre Block-PC-Liste reserviert oder kopiert wird.

`EVMABIParserLimits.def` begrenzt Tuple-Schachtelung, Typknoten und aggregierte
Array-Dimensionen. `EVMABITableLimits.def` begrenzt Kardinalität und
Gesamttext öffentlicher Signatur-/Variantentabellen. Die öffentliche
Tabellenprüfung wendet diese Grenzen vor Parsen oder Hashen an und verwirft
danach ungültige Enumwerte, Kind-Metadaten, Standards, Selector-Beweisrollen,
nichtkanonische Typen, abgeleitete Hashes, Mitgliedschaften und Kollisionen.
Produktive Selector-Suche ist indiziert, Event-Suche nutzt eine sortierte
Topic-Tabelle, und Topic-APIs prüfen vor Vergleich oder Ordnung, dass ein
`APInt` genau ein EVM-Wort breit ist.

`EVMInterpreterLimits.def` deklariert `MaxSteps`, `MaxMemoryBytes`,
`MaxTraceEntries`, `MaxLogEntries`, aggregiertes `MaxLogDataBytes`,
aggregiertes `MaxHostReturnDataBytes`, `MaxCalldataBytes`, aggregiertes
`MaxHostEnvironmentEntries`, aggregiertes `MaxExternalCodeBytes` und
`MaxPersistentStateEntries`. Das Host-Entry-Aggregat umfasst `BlockHashes`,
`Balances`, `CodeHashes`, `ExternalCode` und `BlobHashes`; das Code-Byte-Limit
umfasst alle `ExternalCode`-Bodies.
`MaxSteps` behält das ausdrückliche Ergebnis `StepLimit`. Laufzeitwachstum von
Memory, Trace, Logs, Logdaten und neuen Persistent-State-Schlüsseln wird
vorbelastet; eine Überschreitung liefert `ResourceExhausted` und rollt
persistenten Zustand, Logs und Selfdestruct-Effekte zurück. Zu große initiale
Host-Return-Data-Aggregate oder Persistent-State-Maps sind stattdessen ein
`execute`-API-Fehler. Der Interpreter hält Host-Return-Data als `ArrayRef`-Views
und verwendet `lower_bound` auf der bereits validierten sortierten
Instruktionstabelle, ohne Buffer zu kopieren oder pro Ausführung eine PC-Map
aufzubauen. Der `const execute preflight` prüft Programm und alle Host-Input-
Grenzen vor jeder Environment-, Snapshot- oder Result-Kopie.

### Live-Differenzaudit gegen go-ethereum

Der normale lokale Audit und CI erzwingen bei jedem Lauf
`git fetch --depth=1 --force` für den Remote-`HEAD` des offiziellen Default-Branches von
`https://github.com/ethereum/go-ethereum.git`. Jeder Lauf erzeugt ein
unvorhersagbar benanntes privates temporäres
Bare-Repository; es gibt kein gemeinsam genutztes dauerhaftes Git-Repository
und keinen Cache. Nur die von diesem Fetch gelieferte authority ref und der
daraus aufgelöste exakte SHA wählen die Revision. Das Skript meldet den SHA und
prüft ihn in einem detached temporären Worktree; anschließend werden
authority repository und Worktree gemeinsam vernichtet. Weder `local_docs` noch ein
vorhandener Checkout oder ein Submodule sind Audit-Pfade; ein Submodule-Pin wäre
gerade dann veraltet, wenn Live-Drift erkannt werden soll.

Jeder Git-Befehl entfernt zuerst alle geerbten `GIT_*`, einschließlich
`GIT_CONFIG_*`, und setzt danach nur geprüfte Werte. `GIT_CONFIG_NOSYSTEM` und
`GIT_CONFIG_GLOBAL` schalten System-/globale Konfiguration ab;
`GIT_ATTR_NOSYSTEM` und das befehlslokale `core.attributesFile` deaktivieren
System-/globale Attribute, `core.hooksPath` deaktiviert Hooks. Das private
Repository verwirft unerwartete
lokale Konfiguration, Grafts, `objects/info/alternates` und `refs/replace`;
`GIT_NO_REPLACE_OBJECTS` deaktiviert zusätzlich Ersetzungen. Jede Abweichung
führt zu einem geschlossenen Fehler.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

Die öffentliche CLI bietet ausschließlich `--manifest-output`; Quelle, Ref und
Toolchain sind nicht wählbar. Ihr geschlossenes Manifest verwendet `schema 3`.
Der Go-Probe reflektiert das vollständige exportierte boolesche Inventar von
`params.Rules`, ruft für jeden abgebildeten Fork
`LookupInstructionSet(params.Rules)` auf und prüft alle 256 Byte-Slots. Die
Belegung bestimmt ausschließlich geths `operation.undefined`; `HasCost`
ist nur eine Kosten-Gegenprüfung, weil es auch für definierte Nullkosten-
Operationen false liefert. Jeder Slot mit `defined && !HasCost` muss genau ab
seinem deklarierten Aktivierungs-Fork zu `EVM_GETH_ACTIVE_WITHOUT_COST` passen.
Ein undefinierter Slot mit Kosten, ein ungeprüfter definierter Slot oder ein
Upstream-Umbau ohne diesen Marker schlägt geschlossen fehl. Unbekannte,
doppelte, fehlende, außerhalb des Wertebereichs liegende oder nicht geparste
Felder und Datensätze sind Fehler. Jeder `.def parser` verwirft außerdem nicht
verbrauchte makroähnliche Eingabe, statt eine `partial` Policy zu akzeptieren.
`EVMUpstreamOpcodePolicy.def` verwaltet Aliase und typisierte historische bzw.
nicht eingeplante EOF-Ausschlüsse samt Overlap-/Inactive-Invarianten. Die
orthogonale `EVMUpstreamSemanticsPolicy.def` verwaltet das geschlossene,
reflektierte `params.Rules`-Inventar, Fork-Zuordnungen, Base-Stack-Ausnahmen und
Dynamic-Immediate-Familien. CI läuft bei Pushes nach `dev`, Pull Requests, manueller
Auslösung und täglich; bei Fehlern werden exakte Revision, Manifest und Log als
Artifact hochgeladen.

Konkret ordnet `EVMUpstreamSemanticsPolicy.def` jedes exportierte boolesche
`params.Rules`-Feld mit genau einem `EVM_GETH_RULE_FIELD` einer der Kategorien
`MappedForkSelector`, `NoOpcodeAllocation` oder
`ExcludedSelectorExpectedError` zu. Das Audit aktiviert jedes Feld einzeln und
ruft `LookupInstructionSet` auf: Die ersten beiden Kategorien verlangen nil
error, die dritte error; der vollständige 256-Slot-Opcode/Stack-Fingerprint muss
stets `ExpectedFork` entsprechen. Die geprüften No-Allocation-Felder
`IsEIP155`, `IsEIP2929`, `IsEIP4762` und `IsPetersburg` ergeben Frontier;
`IsUBT` muss fehlschlagen und den Cancun-Fingerprint liefern.

`EVMUpstreamSemanticsPolicy.def` deklariert die OpCodes jeder dynamischen
EIP-8024-Familie, ihre Operationsart und ihr gültiges Stack-Delta;
`EVMEIP8024Immediates.def` bleibt die alleinige Decode-Autorität der Immediates.
Single- und Pair-Inventar klassifizieren jeweils alle 256 Bytewerte explizit.
Per `go -overlay` holt das Audit die echten privaten `operation.execute`-Handler
und prüft die `canonical fork jump tables` sowie die
`mainnet active/scheduled jump tables` einzeln. Eine `inactive` Familie wird
explizit protokolliert; eine nur teilweise vorhandene Familie schlägt fehl. Für
jede aktive Tabelle werden die drei deklarierten Operationen über alle Bytes
(`3x256`) plus `3 missing-operand cases` ausgeführt. Akzeptanz, PC-Delta,
marker-abgeleitete Operanden/Mutation, exakter gültiger Underflow und das
`0x00`-Verhalten bei fehlendem Operanden werden mit denselben deklarativen
Policies verglichen, ohne die Formel in Python zu duplizieren.

`EVM_HARDFORK_LATEST` besitzt genau ein kanonisches Ziel. Das geschlossene
`EVMUpstreamForkAliases.def` mappt Prague auf Pectra, Osaka und BPO1 bis BPO5 auf
Fusaka; Paris, Shanghai, Cancun, Amsterdam und Bogota sind Identitäten. Ein
unbekannter neuer Name schlägt geschlossen fehl. Jedes Audit fixiert und
protokolliert ein `audit_unix_time`, verlangt von
`MainnetChainConfig.LatestFork(time)` das NeverD-Latest und von
`LatestFork(max uint64)` einen Alias im Inventar mit bereits geprüftem
kanonischem Fork; beide Instruction Sets werden vollständig verglichen. Das
Manifest enthält `authority=official-fresh-fetch`, offizielle URL, angefordertes
`HEAD` und aufgelöstes SHA. Der Probe setzt `GOTOOLCHAIN=local`.

Go und Python erzwingen vor der Materialisierung feindlicher Metadaten
`input/collection/string hard limits`; zu große Eingaben, Collections oder
Strings schlagen geschlossen fehl. Zusätzlich gilt `bounded diagnostic output`:
Eine überlange Anzeige trägt den `digest` des vollständigen Inhalts und einen
`explicit truncated marker`. Jeder Kindprozess hat begrenzte Ausgabe und eine
Frist; bei Überschreitung wird die gesamte `process group` beziehungsweise der
process tree beendet und ihre Pipes werden geleert.

Der aktuelle schema-3-Live-Beleg enthält `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, Revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, lokales `Go 1.24.0`,
`stack_limit=1024` und `diagnostics=[]`. Er vergleicht `21 fork tables` und
`20 Rules probes` mit `15 mapped/4 no-op/1 expected-error`. Beide
`mainnet active/scheduled`-Einträge nennen `upstream BPO2`, das der geschlossene
Alias auf `NeverD Fusaka` abbildet. EIP-8024 umfasst `23 table targets`; nur
`Amsterdam/Bogota` sind aktiv und ergeben `1536 candidate executions` sowie
`6 missing-operand cases`. Die `three handler symbols` stimmen über beide
aktiven Ziele überein. Python-Audit `67/67` und `C++ Opcode 10/10` schließen den
Beleg. Unter macOS lief das echte Audit erfolgreich in `sandbox-exec`, wobei das
finale `go run` offline war; der Linux-Workflow erzwingt `bubblewrap`.

Alle Go-Phasen — `go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` und `go run` — laufen in einem `capability-root`-Dateisystem-
Sandbox. Lesbar sind nur privater Probe, frisches geth, validiertes
`resolved GOROOT` und exakt benötigte System-Runtime-Roots; schreibbar sind nur
isolierte Environment-Roots. Netzwerk wird nur nötigen Dependency-Phasen
gewährt, der finale Lauf bleibt offline. Sentinels im `host HOME/workspace`
müssen unlesbar sein und dürfen in keiner Ausgabe erscheinen. Linux bildet die
Policy mit `bubblewrap` ohne `/` broad bind nach.

`NeverDEVMDecoderPropertyTests` prüft für jeden decoder-verändernden Fork alle
Zwei-Byte-Eingaben, den vollständigen Decode und die exakten `JUMPDEST`-Grenzen.
Zusätzlich durchlaufen deterministische feindliche Bytefolgen begrenzter Länge
jeden Fork.

LowIR-Lanes für den gesamten Stack bewahren Korrelationen innerhalb eines Pfads.
`MayReachable` ist nur ein CFG-Kandidat. Ein geänderter loop-carried Slot wird am
Back-Edge semantisch `Top`, unabhängig von Budgets; deren Erschöpfung führt ohne
Emergency-Widening zum Fehler. HighIR-Speicher verfolgt konstante Writes,
Overlap/Kill und unbekannte Invalidierung. Nachgewiesen sind Selector und bekannte
Panic-Bytes; bekannte Custom-Error-Deklarationen behalten kanonische Typen, ohne
Anspruch auf jeden Runtime-Argumentwert. Weitere Fakten sind Evidenzkandidaten.

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
