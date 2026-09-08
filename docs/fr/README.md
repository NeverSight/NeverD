**Langues**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

[← Projet NeverD](project.md)

# Documentation NeverD

L’aperçu du projet, la compilation et le CLI se trouvent dans le README du dépôt. Les références de conception et de test destinées aux contributeurs sont regroupées ici.

| Document | Description |
|----------|-------------|
| [README (français)](project.md) | Aperçu, démarrage rapide, compilation, SDK, CLI |
| [Contribution](CONTRIBUTING.md) | Environnement, profils de compilation, workflow, style et exigences de PR |
| [Architecture](architecture.md) | Parcours IR, frontières, lifting strict, profondeur de support et points de modification |
| [Tests](testing.md) | Suites, fixtures générées, allers-retours Unicorn et commandes incrémentales |
| [Reconstruction des exceptions Windows](windows-exception-reconstruction.md) | Matrice de support SEH/C++, contrat IR, règles de patch natif et validation PE |
| [Audit et chasse de sûreté mémoire](memory-safety.md) | Analyse de durée de vie du tas et de débordement de copie : contrat d’identité par format, catalogue puits/sources, verdicts, budgets et schéma JSON |
| [Plugins natifs](plugins.md) | ABI de descripteur en C pur, callbacks et événements, procédure de compilation/liaison, découverte et règles de compatibilité |
| [Plugins Python](python-plugins.md) | Création, API de session et d’événements, isolation, tests et publication |
| [Décompilation EVM](evm.md) | Entrées, hardforks, IR par étapes, ABI host C/LLVM, reconstruction Solidity et limites |
| [Décompilation Solana SBF](sbf.md) | SBF v0-v4, LLVM IR, sorties C/Rust, vérification et limites connues |
| [Reconstruction Java pour Android](android.md) | CLI expérimentale APK/DEX/smali, environnements d’exécution, multidex, rapport JSON, dépannage et limites |
| [Récupération des sources iOS](ios.md) | IPA/.app/Mach-O, corps et dispositions Objective-C/Swift, CLI/export, couverture, limites et tests exécutés |
| [Feuille de route](roadmap/README.md) | État : formats natifs, EVM et Solana SBF implémentés |
| [English README](../../README.md) | Version anglaise |
| [Autres langues](../README.md) | Autres versions localisées |
