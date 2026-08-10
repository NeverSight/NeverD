**Sprachen**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← Dokumentationsindex](../README.de.md)

# NeverD-Roadmap

Dieses Dokument skizziert geplante Richtungen jenseits der nativen PE-/ELF-/Mach-O-Pipeline. Unverändert: **1:1-Lifting**, **strict fail-loud**, gemeinsame **vierstufige IR**.

---

## 1. Native Formatvollständigkeit

Ziele abschließen, die Loader schon teilweise erkennen.

| Punkt | Notizen |
|-------|---------|
| PE AArch64 | Windows ARM64: Unwind/`.pdata`, Trampoline, Rewrite-Roundtrip |
| PE ARM32 (Thumb-2) | Windows on ARM ist Thumb-only |
| Mach-O i386 | gängige clang-Relocs; zuerst thin objects |

### Prinzipien

- Zelle erst nach Format-Tests als unterstützt markieren
- Bestehendes ELF / PE x86 / Mach-O arm64+x64 nicht brechen
- Instruktionsmodus auf Image-Ebene

---

## 2. EVM-Bytecode-Dekompilation

**EVM**-Vertragsbytecode 1:1 in denselben IR-Stack heben und C, Solidity-orientierten Quelltext und LLVM IR ausgeben.

### Ziele

- EVM-Loader · 1:1-Opcode-Lifter (strict) · Stack/Speicher · JUMP/JUMPI-CFG · Storage/Calldata · C23/Solidity/LLVM · einheitliche CLI/C-API

**Status:** Für Legacy-EVM von Frontier bis Fusaka abgeschlossen: alle 150
zugewiesenen Opcodes, Raw/Hex/Artifact-Eingaben, Runtime-Extraktion, CFG und
Stack-SSA, Strict/Relaxed-Analyse, C23/LLVM/Solidity-Backends, CLI/C-API und
Differentialtests gegen Anvil. Host-ABI und Grenzen stehen unter
[EVM-Dekompilation](../evm.de.md).

### Warum EVM

- Treue für Audits · ein Engine für Native und Contracts · keine stillen Lücken

---

## 3. Solana-eBPF-(SBF)-Dekompilation

**Solana eBPF / SBF** mit derselben strict-Semantik.

### Ziele

- SBF-Loader · 1:1-eBPF/SBF-Lifter · Account/CPI · gleiche Pipeline · einheitliche API

**Status:** Die Unterstützung für die aktuellen Anza-`sbpf`-Verträge v0-v4 ist abgeschlossen. Implementiert sind ältere Section-/Relocation-ELFs und strikte Program-Header-only-ELFs, eine vollständige versionierte Instruktionsdatenbank, strikte Verifikation, gestufte Low/Med/High IR, Syscall-/CPI-/Account-Beobachtungen, verifiziertes LLVM, portables C11, sicheres stabiles Rust, CLI-/C-API-Integration sowie ein unabhängiges, begrenztes semantisches Raw-Bytecode-Oracle. v4 wird gemäß Upstream nachgeführt; ob es auf einem bestimmten Cluster deployt oder ausgeführt werden kann, hängt weiterhin von dessen Feature-Aktivierung ab. Siehe [Solana-SBF-Dekompilation](../sbf.de.md).

### Warum Solana eBPF

- Wichtiges Audit-Ziel · BPF-ISA passt zu MedIR · ein C-SDK

---

## 4. Engine- & Produkt-Härtung (laufend)

| Bereich | Richtung |
|---------|----------|
| Lifter-Abdeckung | Native Lücken schließen ohne Strict zu lockern |
| Semantiktests | Unicorn / Roundtrip ausbauen |
| Plugin-ABI | Neue Formate als Plugins wo sinnvoll |
| Docs / Matrix | README erst nach Tests aktualisieren |

---

## Zeitplan

Native Formatvollständigkeit, EVM- und Solana-SBF-Dekompilation sind abgeschlossen und regressionstestgedeckt. Keine Termine zugesagt.

| Feature | Status |
|---------|--------|
| Native Formatvollständigkeit (PE ARM*, Mach-O i386) | Abgeschlossen |
| EVM-Bytecode-Dekompilation | Abgeschlossen — C, Solidity und LLVM; regressionstestgedeckt |
| Solana-eBPF-(SBF)-Dekompilation | Abgeschlossen — v0-v4, C, Rust und LLVM; regressionstestgedeckt |
| Engine- & Produkt-Härtung | Laufend |
