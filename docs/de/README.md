**Sprachen**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

[← NeverD-Projekt](project.md)

# NeverD-Dokumentation

Projektüberblick, Build und CLI stehen in der Repository-README. Architektur- und Testreferenzen für Mitwirkende sind hier gebündelt.

| Dokument | Beschreibung |
|----------|--------------|
| [README (Deutsch)](project.md) | Überblick, Schnellstart, Build, SDK, CLI |
| [Mitwirken](CONTRIBUTING.md) | Entwicklungsumgebung, Build-Profile, Ablauf, Stil und PR-Anforderungen |
| [Architektur](architecture.md) | IR-Pfade, Komponentengrenzen, striktes Lifting, Supporttiefe und Änderungsorte |
| [Tests](testing.md) | Testsuiten, generierte Fixtures, Unicorn-Roundtrips und inkrementelle Befehle |
| [Windows-Ausnahmerekonstruktion](windows-exception-reconstruction.md) | SEH/C++-Supportmatrix, IR-Vertrag, native Patch-Regeln und PE-Validierung |
| [Speicher-Audit und Hunt](memory-safety.md) | Heap-Lebensdauer- und Copy-Überlaufanalyse: Identitätsvertrag je Format, Senken-/Quellenkatalog, Urteile, Budgets und JSON-Schema |
| [Native Plugins](plugins.md) | Reine C-Deskriptor-ABI, Callbacks und Ereignisse, Build-/Link-Ablauf, Erkennung und Kompatibilitätsregeln |
| [Python-Plugins](python-plugins.md) | Plugin-Entwicklung, Session-/Event-API, Isolation, Tests und Veröffentlichung |
| [EVM-Dekompilation](evm.md) | Eingaben, Hardforks, IR-Stufen, C-/LLVM-Host-ABI, Solidity-Rekonstruktion und Grenzen |
| [Solana-SBF-Dekompilation](sbf.md) | SBF v0-v4, LLVM IR, C-/Rust-Ausgabe, Verifikation und bekannte Grenzen |
| [Java-Rekonstruktion für Android](android.md) | Experimentelle APK-/DEX-/Smali-CLI, Laufzeitumgebungen, Multidex, JSON-Bericht, Fehlerbehandlung und Grenzen |
| [iOS-Quelltextrekonstruktion](ios.md) | IPA/.app/Mach-O, Objective-C-/Swift-Körper und Layouts, CLI/Export, Abdeckung, Grenzen und Ausführungstests |
| [Roadmap](roadmap.md) | Status: Native Formate, EVM und Solana SBF implementiert |
| [English README](../../README.md) | Englische Version |
| [Andere Sprachen](../README.md) | Weitere lokalisierte Versionen |
