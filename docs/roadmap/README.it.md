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

Estendere NeverD al **bytecode EVM** con elevazione 1:1 sullo stesso stack IR.

### Obiettivi

- Loader EVM · lifter opcode 1:1 (strict) · stack/memoria · JUMP/JUMPI → CFG · storage/calldata · HighIR/LLVM-C · CLI/C API unificati

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

## 4. Rafforzamento motore e prodotto (continuo)

| Area | Direzione |
|------|-----------|
| Copertura lifter | Chiudere gap nativi senza allentare strict |
| Test semantici | Espandere Unicorn / roundtrip |
| ABI plugin | Nuovi formati come plugin se ha senso |
| Docs / matrice | Aggiornare README solo dopo i test |

---

## Tempistica

La decompilazione Solana SBF e la completezza dei formati nativi sono concluse e coperte da test di regressione. EVM resta in ricerca / progettazione. Nessuna data impegnativa.

| Funzione | Stato |
|----------|-------|
| Completezza formati nativi (PE ARM*, Mach-O i386) | Completata |
| Decompilazione EVM | Ricerca / progettazione |
| Decompilazione Solana eBPF (SBF) | Completata — v0-v4, C, Rust e LLVM; coperta da regressione |
| Rafforzamento motore e prodotto | Continuo |
