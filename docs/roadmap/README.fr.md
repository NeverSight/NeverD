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

Étendre NeverD au **bytecode EVM** : lifting 1:1 vers la même pile IR, C structuré / LLVM IR.

### Objectifs

- Loader EVM · lifter d’opcodes 1:1 (strict) · modèle pile/mémoire · JUMP/JUMPI → CFG · storage/calldata · sorties HighIR/LLVM-C · CLI/C API unifiés

### Pourquoi EVM

- Fidélité pour l’audit · un moteur pour natif et contrats · pas d’omission silencieuse

---

## 3. Décompilation Solana eBPF (SBF)

Programmes **Solana eBPF / SBF** avec la même sémantique strict.

### Objectifs

- Loader SBF · lifter eBPF/SBF 1:1 · Account/CPI · même pipeline · API unifiée

### Pourquoi Solana eBPF

- Cible d’audit majeure · ISA type BPF adaptée au MedIR · un seul SDK C

---

## 4. Renforcement moteur & produit (continu)

| Domaine | Direction |
|---------|-----------|
| Couverture lifter | Combler les trous natifs sans relâcher strict |
| Tests sémantiques | Étendre Unicorn / roundtrip |
| ABI plugins | Nouveaux formats en plugins si pertinent |
| Docs / matrice | Mettre à jour le README seulement après tests |

---

## Calendrier

EVM et Solana restent en recherche / conception ; la complétude des formats natifs est terminée et couverte par des tests de régression. Pas de dates promises.

| Fonctionnalité | Statut |
|----------------|--------|
| Complétude formats natifs (PE ARM*, Mach-O i386) | Terminée |
| Décompilation EVM | Recherche / conception |
| Décompilation Solana eBPF (SBF) | Recherche / conception |
| Renforcement moteur & produit | Continu |
