**Lingue**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

[← Progetto NeverD](project.md)

# Documentazione NeverD

Panoramica, build e CLI sono nel README del repository. I riferimenti di design e test per i contributor sono raccolti qui.

| Documento | Descrizione |
|-----------|-------------|
| [README (italiano)](project.md) | Panoramica, avvio rapido, build, SDK, CLI |
| [Contribuire](CONTRIBUTING.md) | Ambiente, profili di build, workflow, stile e requisiti PR |
| [Architettura](architecture.md) | Percorsi IR, confini dei componenti, lifting strict, profondità del supporto e punti di modifica |
| [Test](testing.md) | Suite, fixture generate, roundtrip Unicorn e comandi incrementali |
| [Ricostruzione delle eccezioni Windows](windows-exception-reconstruction.md) | Matrice di supporto SEH/C++, contratto IR, regole di patch nativo e validazione PE |
| [Audit e hunt di sicurezza della memoria](memory-safety.md) | Analisi di vita dell’heap e overflow di copia: contratto di identità per formato, catalogo sink/source, verdetti, budget e schema JSON |
| [Plugin nativi](plugins.md) | ABI del descrittore C puro, callback ed eventi, flusso di build/link, rilevamento e regole di compatibilità |
| [Plugin Python](python-plugins.md) | Sviluppo, API di sessione ed eventi, isolamento, test e pubblicazione |
| [Decompilazione EVM](evm.md) | Input, hardfork, IR a stadi, ABI host C/LLVM, ricostruzione Solidity e limiti |
| [Decompilazione Solana SBF](sbf.md) | SBF v0-v4, LLVM IR, output C/Rust, verifica e limiti noti |
| [Ricostruzione Java per Android](android.md) | CLI sperimentale APK/DEX/smali, ambienti di esecuzione, multidex, report JSON, risoluzione dei problemi e limiti |
| [Recupero dei sorgenti iOS](ios.md) | IPA/.app/Mach-O, corpi e layout Objective-C/Swift, CLI/export, copertura, limiti e prove eseguibili |
| [Roadmap](roadmap/README.md) | Stato: formati nativi, EVM e Solana SBF implementati |
| [English README](../../README.md) | Versione inglese |
| [Altre lingue](../README.md) | Altre versioni localizzate |
