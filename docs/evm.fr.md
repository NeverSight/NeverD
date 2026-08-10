**Langues**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# Décompilation EVM

[← Index de la documentation](README.fr.md)

NeverD charge le bytecode historique de l’Ethereum Virtual Machine, construit
un LowIR 256 bits, un MedIR en SSA de pile et un HighIR récupéré, puis produit
du LLVM IR, du C23 ou du Solidity. L’analyse stricte est activée par défaut :
un opcode non attribué ou inactif pour le hardfork choisi déclenche une erreur
à son PC exact.

Les sorties Solidity et C sont des reconstructions sémantiques. Elles préservent
l’ordre des opcodes, l’arithmétique 256 bits, les contrôles de pile et le flot de
contrôle validé, sans prétendre reproduire le source, les noms ou les types initiaux.

## Démarrage rapide

```bash
# LLVM IR vérifié avec des valeurs i256/i512.
./build/bin/neverd lift contract.evm -o contract.ll

# Examiner chaque niveau d’analyse EVM.
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# Produire du C23 ou du Solidity.
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Choisir un jeu historique ou conserver les opcodes inconnus comme fautes.
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`, `cfg` et les requêtes Low/Med/High/LLVM de l’API C acceptent aussi
l’EVM. La réécriture binaire EVM est explicitement refusée ; `patch` reste une
opération pour binaires natifs.

## Entrées acceptées

| Entrée | Reconnaissance et normalisation |
|--------|---------------------------------|
| Octets bruts | `.raw`, `.evmraw` ou contenu binaire avec une extension EVM explicite |
| Texte hexadécimal | Préfixe `0x` facultatif, espaces ASCII arbitraires, extensions `.evm`, `.hex`, `.bin`, `.bytecode` ; l’hexadécimal sans extension est détecté après validation |
| Artefact de compilateur | `.json` contenant `deployedBytecode`, `runtimeBytecode` ou `bytecode` à la racine ou sous `evm` ; JSON standard solc `contracts → file → contract → evm` inclus |

Le bytecode runtime/déployé est préféré au code de création. À défaut, NeverD
reconnaît les wrappers de constructeur constants et bornés `CODECOPY`/`RETURN`
et extrait la tranche runtime. Un champ ne contenant que le préfixe facultatif
`0x` est vide : un runtime vide ne masque donc pas un fallback de création utile.
La map CBOR Solidity finale n’est retirée que si sa longueur, son marqueur et
une clé `solc`, `ipfs` ou Swarm connue sont tous valides.

Hexadécimal mal formé, nombre impair de chiffres, placeholder de liaison non
résolu, artefact multi-contrat ambigu, bornes de metadata invalides ou code vide
produisent une erreur exploitable. `BytecodeLoadOptions::ArtifactContract`
sélectionne `Contract` ou `path/File.sol:Contract`. Un nom non qualifié est
refusé s’il existe dans plusieurs fichiers afin que l’ordre JSON ne choisisse
jamais silencieusement le mauvais contrat.

L’EVM est enregistré dans le registre central des loaders, pas dans un plugin
backend. CLI, API C, désassembleur, CFG et requêtes IR utilisent donc exactement
la même image normalisée et les mêmes options.

## Hardforks et opcodes

Les 150 opcodes legacy attribués de Frontier à Fusaka sont couverts, dont
`PUSH0`, le stockage transitoire, `MCOPY`, les opcodes blob et `CLZ`. `latest`
cible Fusaka par défaut.

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, latest
```

Les alias `dao`, les formes avec underscore, `merge`, `prague` et `osaka` sont
acceptés. `latest` et `osaka` correspondent actuellement à la révision canonique
`fusaka`.

`latest` désigne la dernière révision mainnet finalisée implémentée, pas la tête
de développement Ethereum. [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)
est annoncé pour le T4 2026 ; les instructions encore en Review
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) et
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) restent hors de
la table jusqu’à finalisation. L’octet immédiat EIP-8024 possède notamment des
règles de masquage `JUMPDEST` différentes de `PUSH`.

EOF a été retiré dans
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2) et est
répertorié comme
[retiré d’Osaka et non planifié](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md).
NeverD ne traite donc pas cette proposition retirée comme comportement mainnet.

Le mode strict rejette les octets inconnus ou inactifs. `--evm-relaxed` les
conserve dans LowIR et les diagnostics, mais les backends fautent si l’exécution
les atteint ; ils ne deviennent jamais silencieusement des NOP.

## Architecture de metadata à la manière de LLVM

Les metadata EVM écrites à la main suivent le modèle `.def` multi-inclus de LLVM :

- `EVMOpcodes.def` est l’unique source de vérité des 150 opcodes : encodage,
  contrat de pile complet, largeur immédiate, classe, fork d’activation, effet
  principal, accès mémoire EVM, état source, call-value et terminaison figurent
  dans chaque enregistrement ; aucun défaut implicite n’est hérité.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def` et
  `EVMCallValueAccesses.def` définissent des domaines fermés et typés. Les
  propriétés sont orthogonales : `CALL` est appel externe et lecture/écriture
  mémoire ; `EXTCODECOPY` lit le contexte et écrit la mémoire. L’état suit la
  lattice `None/Read/Write/Unknown`. La payabilité reste indépendante : une
  lecture `CALLVALUE` implique `payable`, sauf si l’analyse prouve le garde
  canonique `ISZERO(CALLVALUE)` dont la branche non nulle termine en `REVERT`.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` et
  `OutputLanguages.def` génèrent enums ordonnées, parseurs, noms, choix CLI et
  valeurs ABI C. `EVMConstants.h` centralise largeurs, limites et noms stables.
- `Semantics.h` contient l’évaluateur ALU scalaire indépendant de la cible.
  L’interpréteur et le constant folding partagent le même `APInt` vérifié ; les
  lowerings LLVM/C/Solidity restent explicites et échouent bruyamment.

Le décodeur est la frontière des octets. Identité attribuée et activation par
fork sont séparées : le décodage relaxed conserve nom, fork d’introduction et
largeur immédiate d’une instruction inactive, tout en lui donnant une sémantique
conservatrice fautive. Une instruction immédiate inactive ne décale donc pas les
frontières suivantes. Analyse, interprétation et emitters utilisent l’enum
`Opcode` générée ; les encodages bruts ne réapparaissent qu’aux ABI de trace et
host callbacks. Les 17 entrées de `SWAP16` et les 7 arguments host maximaux sont
deux limites distinctes calculées à la compilation.

`OpcodeInfo` ne peut être construit à moitié valide et son nom est un
`llvm::StringLiteral`. Le validateur compile-time refuse doublons, propriétés
inconnues, contrats ALU invalides, incohérences effect/état, familles
PUSH/DUP/SWAP/LOG incorrectes, branches non terminatrices et résultats host
incompatibles. Une fabrique explicite est la seule source de metadata unknown.

Les `.def` sont des bases écrites à la main, analogues à
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def).
`.inc` est réservé aux fragments réellement générés, par exemple par TableGen.
Les records plus riches vivent en `.td`, puis
[TableGen](https://llvm.org/docs/TableGen/ProgRef.html) génère les `.inc`.
NeverD n’ayant pas encore cette étape pour EVM, un `.inc` sans générateur ne
ferait qu’imiter un produit généré. Le C++ suit les
[règles LLVM](https://llvm.org/docs/CodingStandards.html), les ADT/chaînes LLVM
aux frontières et des switches sémantiques exhaustifs.

Ajouter un opcode exige un `EVM_OPCODE` complet, sa sémantique scalaire si
applicable, les lowerings explicites et des tests ciblés. Ajouter un hardfork
exige un `EVM_HARDFORK` ordonné et ses alias. API typée, tables, validation,
classification et CLI s’étendent sans tables parallèles.

## Modèle d’analyse

- **LowIR** conserve PC, encodage, immédiats PUSH (zero-padding droit si
  tronqués), blocs, arêtes, `JUMPDEST` validés, atteignabilité et hauteur de pile.
- **MedIR** transforme chaque valeur de pile en SSA 256 bits, crée les phi,
  replie les opérations pures et conserve séparément effet, mémoire, état et
  call-value pour dataflow, alias, mutabilité et payabilité.
- **HighIR** récupère selectors, mots calldata/retour probables, mutabilité,
  slots constants, événements, revert et régions function/CFG. Noms et types
  restent heuristiques. Un saut dynamique atteignable non résolu joint l’état à
  `Unknown` et force Solidity à `nonpayable`; les selectors contradictoires sont
  diagnostiqués et omis.
- **LLVM** produit une machine `i32 @evm_execute(ptr)` vérifiée avec pile de
  1024 mots `i256`, intermédiaires `i512`, division signée gardée, shifts saturés,
  `BYTE`/`SIGNEXTEND`/`CLZ` exacts et switches de sauts validés.

L’interpréteur déterministe est l’oracle. LLVM et C sont compilés et comparés ;
Solidity est déployé dans Anvil et comparé sur stockage et traces. Un corpus brut
pré-Fusaka s’exécute aussi dans l’EVM native d’Anvil pour vérifier indépendamment
ALU, copies calldata, `MCOPY` superposé, expansion mémoire, Keccak et returndata.
Les operands compte sont masqués à 160 bits selon la
[spécification](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py),
les largeurs environnementales sont validées et `BLOCKHASH` respecte 256 blocs.
Le buffer EIP-211 est séparé de la sortie finale : seuls `RETURN` et `REVERT`
remplissent `ExecutionResult::ReturnData`; CREATE/CREATE2 respectent cette règle.

## Contrat C généré

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

Les opérations environnementales utilisent l’ABI suivante. `a0` est le sommet
initial, les arguments inutilisés valent zéro et le retour est le premier mot
empilé. Le trace hook précède chaque instruction.

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment, uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);
void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

```bash
clang -std=c2x -ffreestanding -c contract.c
```

Le frontend doit accepter `_BitInt` sur au moins 512 bits. La cible Darwin du
Clang Apple ne le permet pas encore ; sous macOS, utilisez une cible non-Darwin
compatible ou directement la sortie LLVM.

## Contrat Solidity généré

La sortie combine les déclarations function/storage/event/error par selector
et une machine PC/pile exacte. Un slot constant devient par exemple
`recovered_storage_slot_3 = uint256(0x3)`, jamais une variable séquentielle qui
inventerait un layout.

Le contrat est volontairement `abstract`. Surchargez `_evmHost` pour les effets
environnementaux ; `_evmTrace` est virtuel et émet `EVMTrace` par défaut.

```bash
solc --bin contract.sol
```

## API C

```c
neverd_session_t session = neverd_session_create();
neverd_evm_set_hardfork(session, "cancun");
neverd_evm_set_strict(session, 1);
if (!neverd_session_load(session, "contract.evm") ||
    !neverd_session_analyze(session)) {
  /* inspect neverd_last_error(session) */
}
const char *solidity = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_SOLIDITY, 0, 0);
const char *c = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_C, 0, 0);
neverd_free_string(solidity);
neverd_free_string(c);
neverd_session_destroy(session);
```

`neverd_decompile_all` reste compatible et produit du C. Les nouvelles entrées
sont `neverd_session_bitness`, `neverd_evm_set_strict`,
`neverd_evm_set_hardfork` et `neverd_decompile_all_ex`. Solidity pour un binaire
natif, l’ancien chemin LLVM-to-C pour EVM et le roundtrip objet natif pour EVM
sont refusés explicitement, jamais ignorés.

## Limites explicites

- Bytecode legacy uniquement ; les conteneurs EOF ne sont pas décodés.
- Les opcodes Amsterdam en Review sont désactivés ; `latest` est Fusaka finalisé.
- Pas de RPC, découverte d’état, comptabilité gas/remboursement ou précompiles.
- L’extraction de création reconnaît des wrappers statiques, pas une transaction complète.
- Les sauts dynamiques restent indirects sauf preuve constante bornée.
- Types ABI, noms, mappings, événements et erreurs sont récupérés au mieux.
- L’exécution autonome des effets exige les hooks host C/Solidity.
