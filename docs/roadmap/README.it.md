**Lingue**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← Indice documentazione](../README.it.md)

# Roadmap NeverD

Questo documento delinea le direzioni principali oltre la pipeline nativa PE / ELF / Mach-O. Principi: **elevazione 1:1**, **fail-loud strict**, **IR a quattro stadi**.

---

## 1. Completezza dei formati nativi

Chiudere i target già parzialmente riconosciuti dai loader.

| Voce | Note |
|------|------|
| PE AArch64 | Windows ARM64: unwind/`.pdata`, trampoline, rewrite roundtrip |
| PE ARM32 (Thumb-2) | Windows on ARM è Thumb-only |
| Mach-O i386 | Reloc clang comuni; prima thin object |

### Principi

- Non segnare supportato prima dei test di formato
- Non rompere ELF / PE x86 / Mach-O arm64+x64
- Modalità istruzione a livello immagine

---

## 2. Decompilazione bytecode EVM

Estendere NeverD al **bytecode EVM** con lifting 1:1 sullo stesso stack IR e output C, Solidity e LLVM IR.

### Obiettivi

- Loader EVM · lifter opcode 1:1 (strict) · stack/memoria · JUMP/JUMPI → CFG · storage/calldata · C23/Solidity/LLVM · CLI/C API unificati

**Stato:** Decode e lifting degli opcode legacy da Frontier a Fusaka sono
completati e coperti da regressione. La ricostruzione sorgente continua in modo
conservativo: selector, event, tipi, standard, nomi e control flow dinamico sono
riportati solo con evidenza sufficiente, mai come sorgente originale, ABI completa
o piena conformità ERC. Selector canonici di funzione, varianti ABI per standard
e forme di ritorno riuscite restano separati: un selector ERC condiviso non può
inventare uno standard né prendere un tipo di ritorno incompatibile. Amsterdam è un target Review/development opt-in;
`latest` resta Fusaka. EOFv1/EIP-7692 non è pianificato ed EIP-3540 è Stagnant,
quindi nessuno è presentato come mainnet definitivo. Vedi
[decompilazione EVM](../evm.it.md).

### Perché EVM

- Fedeltà per audit · un motore per nativo e contratti · niente omissioni silenziose

---

## 3. Decompilazione Solana eBPF (SBF)

Programmi **Solana eBPF / SBF** con la stessa semantica strict.

### Obiettivi

- Loader SBF · lifter eBPF/SBF 1:1 · Account/CPI · stessa pipeline · API unificata

**Stato:** Il supporto ai contratti Anza `sbpf` v0-v4 correnti è completo. L’implementazione gestisce ELF legacy con sezioni/rilocazioni ed ELF rigorosi basati solo sui program header, un database completo di istruzioni versionate, verifica rigorosa, IR Low/Med/High a stadi, osservazioni syscall/CPI/account, LLVM verificato, C11 portabile, Rust stabile e sicuro, integrazione CLI/C API e un oracle semantico indipendente e limitato per il bytecode grezzo. v4 segue l’upstream; la possibilità di distribuirlo o eseguirlo su uno specifico cluster dipende comunque dall’attivazione delle funzionalità del cluster. Vedi [Decompilazione Solana SBF](../sbf.it.md).

### Perché Solana eBPF

- Target di audit importante · ISA BPF adatta al MedIR · un solo SDK C

---

## 4. Audit e hunt di sicurezza della memoria

Analizzare un binario sollevato per difetti di vita dell’heap (leak, doppia free, use-after-free) e overflow di copie pericolose, in JSON strutturato, con un modello limitato del solver per un overflow dimostrato. L’analisi gira sull’IR indipendente dal formato e sulla vista di identità condivisa, quindi **PE, ELF e Mach-O sono bersagli paritari**, e riutilizza l’esecuzione simbolica e il solver bitvector interni — nessun solver esterno né contenitore.

| Voce | Note |
|------|------|
| Pista `audit` | Macchina a stati dell’heap su IR + riassunti di escape: leak, doppia free, use-after-free |
| Pista `hunt` | Catalogo di sink + prefiltro degli argomenti + capacità di destinazione + testimone del solver |
| Contratto di identità | Risoluzione dei sink per formato (IAT PE, PLT ELF, bind dyld Mach-O) e fonti di nomi PDB / DWARF / MAP |

**Stato:** La Phase 1 è implementata per PE, ELF e Mach-O. P0 comprende analisi closed-world del ciclo di vita heap e delle copie pericolose, oltre all’evidenza additiva dello schema v1 con replay `process-input-v1` per valori letterali esatti dell’ambiente e il primo consumo dello standard input; gli altri tipi restano non riproducibili con una motivazione. P1 copre overflow di stack/globale, letture locali non inizializzate e stringhe di formato. Gli effetti delle chiamate sconosciuti o applicabili solo in parte restano UNKNOWN. La copertura di verdetti e identità è bloccata da [`unittests/safety`](../../unittests/safety) e dal test end-to-end [`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp), che esegue su ogni host la matrice obbligatoria PE/ELF/Mach-O × x86-64/AArch64. Vedi [Audit e hunt di sicurezza della memoria](../memory-safety.it.md). Una base P2 è ora implementata: la versione `lowir-concolic-v1` segue una traccia LowIR nativa limitata e pubblica solo seed di registro verificati tramite replay, con evidenza PE/ELF/Mach-O × x86-64/AArch64 attraverso C, CLI e Python. L’inserimento di controlli binari, la pianificazione e mutazione del corpus per il fuzzing ibrido, la proiezione degli input di memoria e la raggiungibilità interprocedurale più ampia restano attività successive, fuori dall’accettazione della Phase 1.

---

## 5. Rafforzamento motore e prodotto (continuo)

| Area | Direzione |
|------|-----------|
| Copertura lifter | Chiudere gap nativi senza allentare strict |
| Test semantici | Espandere Unicorn / roundtrip |
| ABI plugin | Mantenere l’[ABI dei plugin nativi](../plugins.it.md) come contratto di estensione nel processo; i valori Loader e UI restano metadati finché non esistono API host esplicite |
| Docs / matrice | Aggiornare README solo dopo i test |

---

## Tempistica

Formati nativi, decode/lifting EVM legacy fino a Fusaka, Solana SBF e sicurezza
della memoria P0 sono coperti da regressione. La ricostruzione sorgente EVM
conservativa resta in corso. Nessuna data impegnativa.

| Funzione | Stato |
|----------|-------|
| Completezza formati nativi (PE ARM*, Mach-O i386) | Completata |
| Decode/lifting EVM legacy | Completato fino a Fusaka; coperto da regressione |
| Ricostruzione sorgente EVM | In corso — evidence-backed e conservativa |
| Decompilazione Solana eBPF (SBF) | Completata — v0-v4, C, Rust e LLVM; coperta da regressione |
| Audit e hunt di sicurezza della memoria | Phase 1 completa — analisi P0/P1 ed evidenza di replay presenti; i seed di registro LowIR concolic verificati tramite replay sono la base P2 già realizzata, mentre l’orchestrazione restante è pianificata |
| Rafforzamento motore e prodotto | Continuo |
