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

**EVM**-Vertragsbytecode 1:1 in denselben IR-Stack heben; strukturiertes C / LLVM IR.

### Ziele

- EVM-Loader · 1:1-Opcode-Lifter (strict) · Stack/Speicher · JUMP/JUMPI-CFG · Storage/Calldata · HighIR/LLVM-C · einheitliche CLI/C-API

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

Solana-SBF-Dekompilation und native Formatvollständigkeit sind abgeschlossen und regressionstestgedeckt. EVM bleibt in Forschung / Design. Keine Termine zugesagt.

| Feature | Status |
|---------|--------|
| Native Formatvollständigkeit (PE ARM*, Mach-O i386) | Abgeschlossen |
| EVM-Bytecode-Dekompilation | Forschung / Design |
| Solana-eBPF-(SBF)-Dekompilation | Abgeschlossen — v0-v4, C, Rust und LLVM; regressionstestgedeckt |
| Engine- & Produkt-Härtung | Laufend |
