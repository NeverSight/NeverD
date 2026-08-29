**Langues**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← Index documentation](../README.fr.md)

# Feuille de route NeverD

Ce document décrit les directions majeures au-delà du pipeline natif PE / ELF / Mach-O. Principes inchangés : **lifting 1:1**, **échec strict**, **IR à quatre étapes**.

---

## 1. Complétude des formats natifs

Achever les cibles déjà partiellement reconnues par les loaders.

| Élément | Notes |
|---------|--------|
| PE AArch64 | Windows ARM64 : unwind/`.pdata`, trampolines, roundtrip rewrite |
| PE ARM32 (Thumb-2) | Windows on ARM est Thumb-only |
| Mach-O i386 | Relocations clang courantes ; objets thin d’abord |

### Principes

- Ne pas marquer supporté avant tests format (load → lift → decompile / patch)
- Ne pas casser ELF / PE x86 / Mach-O arm64+x64
- Mode d’instruction au niveau image

---

## 2. Décompilation bytecode EVM

Étendre NeverD au **bytecode EVM** : lifting 1:1 vers la même pile IR, sorties C, Solidity et LLVM IR.

### Objectifs

- Loader EVM · lifter d’opcodes 1:1 (strict) · pile/mémoire · JUMP/JUMPI → CFG · storage/calldata · C23/Solidity/LLVM · CLI/C API unifiés

**Statut :** Le décodage et le lifting des opcodes legacy de Frontier à Fusaka
sont terminés et couverts par régression. La reconstruction source continue de
façon prudente : selectors, events, types, standards, noms et contrôle dynamique
ne sont rapportés que lorsque les preuves le permettent, jamais comme source
d’origine, ABI complète ou conformité ERC totale. Selectors canoniques de
fonction, variantes ABI propres à chaque standard et formes de retour réussies
restent séparés : un selector ERC partagé ne peut ni inventer un standard ni
emprunter un type de retour incompatible. Amsterdam est une cible
Review/development opt-in ; `latest` reste Fusaka. EOFv1/EIP-7692 n’est pas
planifié et EIP-3540 est Stagnant, donc aucun n’est présenté comme mainnet final.
Voir [décompilation EVM](../evm.fr.md) pour les limites.

### Pourquoi EVM

- Fidélité pour l’audit · un moteur pour natif et contrats · pas d’omission silencieuse

---

## 3. Décompilation Solana eBPF (SBF)

Programmes **Solana eBPF / SBF** avec la même sémantique strict.

### Objectifs

- Loader SBF · lifter eBPF/SBF 1:1 · Account/CPI · même pipeline · API unifiée

**État :** La prise en charge des contrats Anza `sbpf` v0-v4 actuels est terminée. L’implémentation couvre les anciens ELF à sections/relocations et les ELF stricts reposant uniquement sur les program headers, une base d’instructions versionnée complète, une vérification stricte, les IR Low/Med/High par étapes, les observations syscall/CPI/account, LLVM vérifié, du C11 portable, du Rust stable sûr, l’intégration CLI/C API et un oracle sémantique indépendant et borné pour le bytecode brut. v4 suit l’upstream ; son déploiement ou son exécution sur un cluster donné dépend toujours de l’activation des fonctionnalités de ce cluster. Voir [Décompilation Solana SBF](../sbf.fr.md).

### Pourquoi Solana eBPF

- Cible d’audit majeure · ISA type BPF adaptée au MedIR · un seul SDK C

---

## 4. Audit et chasse de sûreté mémoire

Analyser un binaire levé pour les défauts de durée de vie du tas (fuite, double libération, utilisation après libération) et les débordements de copies dangereuses, en JSON structuré, avec un modèle borné du solveur pour un débordement prouvé. L’analyse s’exécute sur l’IR indépendant du format et la vue d’identité partagée, donc **PE, ELF et Mach-O sont des cibles à égalité**, et réutilise l’exécution symbolique et le solveur bitvector internes — pas de solveur externe ni de conteneur.

| Élément | Notes |
|---------|--------|
| Piste `audit` | Machine d’état du tas sur l’IR + résumés d’évasion : fuite, double libération, utilisation après libération |
| Piste `hunt` | Catalogue de puits + préfiltre d’arguments + capacité de destination + témoin du solveur |
| Contrat d’identité | Résolution des puits par format (IAT PE, PLT ELF, bind dyld Mach-O) et sources de noms PDB / DWARF / MAP |

**État :** La Phase 1 est implémentée pour PE, ELF et Mach-O. P0 comprend les analyses en monde fermé du cycle de vie du tas et des copies dangereuses, ainsi que la preuve additive du schéma v1 avec rejeu `process-input-v1` des valeurs littérales exactes de l’environnement et de la première consommation de l’entrée standard ; les autres types restent non rejouables avec une raison. P1 couvre les débordements pile/global, les lectures locales non initialisées et les chaînes de format. Les effets d’appel inconnus ou partiellement applicables restent UNKNOWN. La couverture des verdicts et de l’identité est verrouillée par [`unittests/safety`](../../unittests/safety) et le bout-en-bout [`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp), qui exécute sur chaque hôte la matrice obligatoire PE/ELF/Mach-O × x86-64/AArch64. Voir [Audit et chasse de sûreté mémoire](../memory-safety.fr.md). Les contrôles binaires, le fuzzing hybride et la portée interprocédurale élargie de P2 restent des travaux ultérieurs de la feuille de route, hors acceptation de la Phase 1.

---

## 5. Renforcement moteur & produit (continu)

| Domaine | Direction |
|---------|-----------|
| Couverture lifter | Combler les trous natifs sans relâcher strict |
| Tests sémantiques | Étendre Unicorn / roundtrip |
| ABI plugins | Maintenir l’[ABI des plugins natifs](../plugins.fr.md) comme contrat d’extension dans le processus ; les valeurs Loader et UI restent des métadonnées jusqu’à ce que des API hôte explicites existent |
| Docs / matrice | Mettre à jour le README seulement après tests |

---

## Calendrier

Les formats natifs, le décodage/lifting EVM legacy jusqu’à Fusaka, Solana SBF et
la sûreté mémoire Phase 1 sont couverts par régression. La reconstruction source EVM
prudente reste en cours. Pas de dates promises.

| Fonctionnalité | Statut |
|----------------|--------|
| Complétude formats natifs (PE ARM*, Mach-O i386) | Terminée |
| Décodage/lifting EVM legacy | Terminé jusqu’à Fusaka ; couvert par régression |
| Reconstruction source EVM | En cours — étayée et prudente |
| Décompilation Solana eBPF (SBF) | Terminée — v0-v4, C, Rust et LLVM ; couverte par régression |
| Audit et chasse de sûreté mémoire | Phase 1 terminée — analyses P0/P1, preuve de rejeu et matrice native formats/architectures présentes ; suites P2 planifiées |
| Renforcement moteur & produit | Continu |
