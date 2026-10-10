**Langues**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 6241c9252ec89317b9c488c7e2b6b8c7bd60329dfdd734df27e1a7a7139b1eff -->

[← Projet NeverD](project.md)

# Documentation NeverD

L’aperçu du projet, la compilation et le CLI se trouvent dans le README du dépôt. Les références de conception et de test destinées aux contributeurs sont regroupées ici.

**Mobile (CLI expérimentale) :** `neverd mobile` récupère Java depuis les APK, DEX et smali [Android](android.md), et le C natif ainsi que les sources Objective-C/Swift prises en charge depuis les IPA, `.app` et Mach-O [iOS](ios.md). Les rapports JSON décrivent résultats et couverture. Commencez par la [vue d’ensemble mobile](mobile.md), puis consultez les commandes et limites des guides de plateforme.

Les guides anglais se trouvent directement dans `docs/`. Les traductions sont regroupées dans `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/` et `zh-TW/`. Chaque répertoire contient l’index `README.md`, la présentation `project.md`, les guides thématiques, `CONTRIBUTING.md`, `ATTRIBUTION.md` et `roadmap.md`. Les images partagées restent dans `assets/`.

| Document | Description |
|----------|-------------|
| [README (français)](project.md) | Aperçu, démarrage rapide, compilation, SDK, CLI |
| [Contribution](CONTRIBUTING.md) | Environnement, profils de compilation, workflow, style et exigences de PR |
| [Architecture](architecture.md) | Parcours IR, frontières, lifting strict, profondeur de support et points de modification |
| [Analyse Web hors ligne (anglais)](../web-analysis.md) | Inspection C++ des artefacts, sources, liaisons et cartes, politique des métadonnées, SDK et validation actuelle |
| [Profil Bun autonome (anglais)](../web-bun-profile.md) | Extraction ELF fixe, plages de preuves originales, décodage des sources et provenance des fixtures figées |
| [Profil d’extraction ASAR (anglais)](../web-asar-profile.md) | Association des captures internes et externes, états d’intégrité, consommateurs des membres et dépendance Unicode native |
| [Profil de preuves Electron (anglais)](../web-electron-profile.md) | Chemins d’entrée capturés, périmètre des manifestes et sources, preuves de fenêtres et ponts, canaux IPC candidats |
| [Profil source HTML (anglais)](../web-html-profile.md) | Inventaire C++ borné des scripts, références locales capturées et ancres des sources intégrées originales |
| [Tests](testing.md) | Suites, fixtures générées, allers-retours Unicorn et commandes incrémentales |
| [Atelier de bureau (anglais)](../gui.md) | Disposition de désassembleur classique, worker séparé, bases de projet, localisation et connexions MCP |
| [Reconnaissance des bibliothèques (anglais)](../library-recognition.md) | Identités STL, ATL/MFC, COM et libc étayées, profils et repliage réversible du code C |
| [Validation du bureau (anglais)](../gui-qualification.md) | Mesures GUI, limites du packaging et validations de plateforme restantes |
| [Récupération de sources à partir d’un interpréteur](interpreter-recovery.md) | Spécialisation expérimentale `--devirtualize`, contrôles CLI, contrat d’exécution, preuves et limites; propositions de preuve pour boucles imbriquées; budgets de découverte explicites et API C versionnée; API de preuve exacte du natif vers LLVM |
| [Reconstruction des exceptions Windows](windows-exception-reconstruction.md) | Matrice de support SEH/C++, contrat IR, règles de patch natif et validation PE |
| [Exécution CPU et environnements invités](emulation.md) | Choix du moteur, environnements invités, validation native et limites actuelles |
| [Exécution CPU](cpu-execution.md) | Configuration, capacités, disponibilité des backends et résultats typés |
| [Preuves bitvector](solver.md) | Preuves Z3 facultatives, synthèse vérifiée, tests indépendants et export |
| [Émulation de processus invités](process-emulation.md) | Profil Linux ELF, démarrage, services, limites et tests |
| [Environnements de processus macOS/iOS](darwin-emulation.md) | Démarrage Mach-O, plateformes appareil et simulateur, services Darwin et règles de pages |
| [macOS HVF](macos-hvf.md) | Exécution matérielle avec l’ISA de l’hôte, droits de signature, packaging et validation |
| [Émulation des pilotes Windows](driver-emulation.md) | Cycle WDM/KMDF x64 borné, requêtes, scénarios matériels, SEH, sous-ensembles PnP, choix du moteur et limites |
| [Audit et chasse de sûreté mémoire](memory-safety.md) | Analyse de durée de vie du tas et de débordement de copie : contrat d’identité par format, catalogue puits/sources, verdicts, budgets et schéma JSON |
| [Plugins natifs](plugins.md) | ABI de descripteur en C pur, callbacks et événements, procédure de compilation/liaison, découverte et règles de compatibilité |
| [Plugins Python](python-plugins.md) | Création, API de session et d’événements, isolation, tests et publication |
| [Vue d’ensemble mobile](mobile.md) | CLI expérimentale Android/iOS, entrées, sorties, rapports et limites |
| [Reconstruction Java pour Android](android.md) | APK (y compris multidex), DEX, fichiers/répertoires smali → Java ; CLI, rapport JSON, dépannage, limites et vérification |
| [Récupération des sources iOS](ios.md) | IPA/.app/Mach-O (arm64/x86_64) → C natif et sources Objective-C/Swift prises en charge ; dispositions, CLI/export, couverture JSON, limites et tests exécutés |
| [Décompilation EVM](evm.md) | Entrées, hardforks, IR par étapes, ABI host C/LLVM, reconstruction Solidity et limites |
| [Décompilation Solana SBF](sbf.md) | SBF v0-v4, LLVM IR, sorties C/Rust, vérification et limites connues |
| [Feuille de route](roadmap.md) | État : formats natifs, EVM et Solana SBF implémentés |
| Documentation traduite | Les liens de langue ci-dessus ouvrent l’index et la présentation de chaque langue |
| [Dépaquetage des exécutables compressés](unpack.md) | Récupération d'images PE32+ compressées par observation d'un processus invité borné : règles d'entrée, image reconstruite, identification et limites |
