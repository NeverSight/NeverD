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

**Status:** Legacy-Opcode-Decoding und -Lifting von Frontier bis Fusaka sind
abgeschlossen und regressionstestgedeckt. Source-Rekonstruktion bleibt eine
laufende, konservative Analyse: Selector, Events, Typen, Standards, Namen und
dynamischer Kontrollfluss werden nur mit hinreichender Evidenz gemeldet, nie als
Originalquelle, vollständige ABI oder volle ERC-Konformität. Kanonische
Funktions-Selectors, standardbezogene ABI-Varianten und erfolgreiche Return-
Formen bleiben getrennt; ein geteilter ERC-Selector kann weder einen Standard
erfinden noch einen unvereinbaren Rückgabetyp übernehmen. Amsterdam ist ein
explizites opt-in Review-/Development-Target; `latest` bleibt Fusaka.
EOFv1/EIP-7692 ist nicht geplant und EIP-3540 Stagnant, also keine finale
Mainnet-Semantik. Siehe [EVM-Dekompilation](../evm.de.md).

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

## 4. Speicher-Audit und Hunt

Ein geliftetes Binärfile auf Heap-Lebensdauerfehler (Leak, Double-Free, Use-after-Free) und gefährliche Copy-Überläufe analysieren, als strukturiertes JSON, mit einem begrenzten Solver-Modell für einen bewiesenen Überlauf. Die Analyse läuft auf dem formatneutralen IR und der gemeinsamen Identitätsansicht, daher sind **PE, ELF und Mach-O gleichrangige Ziele**, und sie nutzt die eigene symbolische Ausführung und den Bitvektor-Solver — kein externer Solver, kein Container.

| Punkt | Hinweise |
|-------|----------|
| Spur `audit` | Heap-Zustandsmaschine über IR + Escape-Zusammenfassungen: Leak, Double-Free, Use-after-Free |
| Spur `hunt` | Senkenkatalog + Argument-Vorfilter + Zielkapazität + Solver-Zeuge |
| Erreichbarkeitsevidenz | Kontrollstatus ab bekannten Einstiegen plus unabhängiger Angreifer-Fixpunkt und exakter Wurzel-/Aufrufkettenzeuge |
| Identitätsvertrag | Senkenauflösung je Format (PE-IAT, ELF-PLT, Mach-O-dyld-Bind) und PDB-/DWARF-/MAP-Namensquellen |

**Status:** Phase 1 ist für PE, ELF und Mach-O implementiert. P0 umfasst Closed-World-Analysen für Heap-Lebensdauer und gefährliche Kopien sowie additive Schema-v1-Evidenz mit `process-input-v1`-Replay für exakte literale Umgebungswerte und den ersten unterstützten `read(0)`-Familienaufruf auf der Standardeingabe; andere Eingabearten bleiben mit Begründung nicht abspielbar. P1 deckt Stack-/Global-Überläufe, uninitialisierte lokale Reads und Formatstrings ab. Unbekannte oder nur teilweise anwendbare Aufrufeffekte bleiben UNKNOWN. Urteils- und Identitätsabdeckung ist durch [`unittests/safety`](../../unittests/safety) und den End-to-End-[`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp) festgeschrieben, der auf jedem Host die verpflichtende PE/ELF/Mach-O × x86-64/AArch64-Matrix ausführt. Siehe [Speicher-Audit und Hunt](../memory-safety.de.md).

Der aktuelle interprozedurale Slice ergänzt `reachability.status` und
`reachability.attacker_control` in Schema v1, ohne den unabhängigen `verdict` zu
ändern. Er berichtet `application`-, `image`- oder `export`-Wurzeln, exakte
interne Aufrufketten und geschlossen fehlschlagende UNKNOWN-Zustände. Die
Budgets `max_call_depth` und `max_summary_iterations` sind über C-API, beide
CLI-Befehle und beide Python-Methoden verfügbar. `control_reachable` und
`attacker_reachable` sind
daher Erreichbarkeits- und keine Urteilssummen.

P2-Analyseoberflächen und -Pläne verwenden versionierte Grenzen mit explizitem Status:

| Plan | Umfang | Status |
|------|--------|--------|
| `lowir-concolic-v1` | Hybride/concolic LowIR-Erkundung und Seed-Erzeugung | Experimentell; per Replay verifizierte Register-Seeds auf PE/ELF/Mach-O × x86-64/AArch64 |
| `binary-sanitizer-v1` | In eine umgeschriebene native Binärdatei eingefügte Laufzeitprüfungen | Experimentell auf Darwin: Counted-Write-Guards nach dem Alles-oder-Ablehnen-Prinzip und authentisierte Create-Exclusive- bzw. Same-Source-No-Change-Veröffentlichung |
| `process-replay-v1` | Breiteres Prozess-Replay für argv, Dateien, Netzwerk und wiederholte Reads jenseits des aktuellen `process-input-v1` | Nur Phase-0-Grenze: Plan-/Koordinatorvalidierung und fail-closed Abfrage nativer Verfügbarkeit; kein Host stellt native Replay-Operationen bereit |

Der Concolic-Adapter ist eine separate Analyseoberfläche und keine Erweiterung
des Abnahmevertrags für Phase-1-Sicherheitsberichte. Der experimentelle
Sanitizer ist über `neverd_session_sanitize`, `neverd patch --sanitize=strict`
und Python `Session.sanitize` verfügbar; Nicht-Darwin-Hosts lehnen vor Lifting
oder Namespace-Änderungen ab. Ein vollständiger Receipt authentisiert nur das
während der Transaktion gehaltene Zielverzeichnisobjekt. Da dieses nach dem
Öffnen umbenannt werden kann, belegt er weder die fortlaufende noch die
nachträgliche Bindung des ursprünglichen Pfadnamens und ist keine dauerhafte
Pfadbindung. `NativeProcessReplayAdapter` bleibt eine Alles-oder-nichts-
Phase-0-Abfrage-/Factory-Grenze; alle Hosts melden derzeit alle Fähigkeiten als
false und liefern keine Operationstabelle.

---

## 5. Engine- & Produkt-Härtung (laufend)

| Bereich | Richtung |
|---------|----------|
| Lifter-Abdeckung | Native Lücken schließen ohne Strict zu lockern |
| Semantiktests | Unicorn / Roundtrip ausbauen |
| Plugin-ABI | Die [native Plugin-ABI](../plugins.de.md) als In-Process-Erweiterungsvertrag pflegen; Loader- und UI-Werte bleiben Metadaten, bis explizite Host-APIs existieren |
| Docs / Matrix | README erst nach Tests aktualisieren |

---

## Zeitplan

Native Formate, Legacy-EVM-Decoding/Lifting bis Fusaka, Solana SBF und
Speichersicherheit Phase 1 einschließlich des aktuellen Known-Entry-
Erreichbarkeitsslices sind regressionstestgedeckt. Die konservative EVM-Source-
Rekonstruktion läuft weiter. Keine Termine zugesagt.

| Feature | Status |
|---------|--------|
| Native Formatvollständigkeit (PE ARM*, Mach-O i386) | Abgeschlossen |
| Legacy-EVM-Decoding/Lifting | Bis Fusaka abgeschlossen; regressionstestgedeckt |
| EVM-Source-Rekonstruktion | Laufend — evidenzgestützt und konservativ |
| Solana-eBPF-(SBF)-Dekompilation | Abgeschlossen — v0-v4, C, Rust und LLVM; regressionstestgedeckt |
| Speicher-Audit und Hunt | Phase 1 plus Known-Entry-Erreichbarkeitsslice abgeschlossen; `lowir-concolic-v1` und Darwin-`binary-sanitizer-v1` sind experimentell; natives `process-replay-v1` bleibt hinter dem fail-closed Phase-0-Adapter nicht verfügbar |
| Engine- & Produkt-Härtung | Laufend |
