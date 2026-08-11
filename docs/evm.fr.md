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
fusaka, amsterdam, bogota, latest
```

Les alias `dao`, les formes avec underscore, `merge`, `prague` et `osaka` sont
acceptés. `latest` et `osaka` correspondent actuellement à la révision canonique
`fusaka`.

`latest` désigne la dernière révision mainnet finalisée implémentée, pas la tête
de développement Ethereum. [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)
est annoncé pour le T4 2026 ; les instructions encore en Review
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) et
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) ne sont activées
qu’avec `--evm-hardfork=amsterdam` (ou `bogota`) et restent hors de `latest`
jusqu’à finalisation. Pour EIP-8024, seul un immédiat valide est consommé ; un
candidat invalide reste l’instruction suivante.

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

- `EVMOpcodes.def` est l’unique source de vérité pour 150 opcodes finalisés et
  quatre opcodes de développement opt-in : encodage, mutations pop/push
  réelles, type d’immédiat, classe, fork d’activation, effet
  principal, accès mémoire EVM, état source, call-value et terminaison figurent
  dans chaque enregistrement ; aucun défaut implicite n’est hérité.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def` et
  `EVMCallValueAccesses.def` définissent des domaines fermés et typés. Les
  propriétés sont orthogonales : `CALL` est appel externe et lecture/écriture
  mémoire ; `EXTCODECOPY` lit le contexte et écrit la mémoire. L’état suit la
  lattice `None/Read/Write/Unknown`. La payabilité reste indépendante : une
  lecture `CALLVALUE` implique `payable`, sauf si l’analyse prouve le garde
  canonique `ISZERO(CALLVALUE)` dont la branche non nulle termine en `REVERT`.
- `EVMImmediateKinds.def` définit les données PUSH de largeur fixe et les
  encodages single/pair conditionnels d’EIP-8024 ; `EVMDecodeStatuses.def`
  possède le vocabulaire stable exposé par LowIR et le désassemblage.
  `EVMUpstreamOpcodePolicy.def` consigne l’alias de nom go-ethereum et les
  exclusions historiques/retirées volontaires ;
  `scripts/audit_evm_opcode_metadata.py` refuse toute dérive d’octet et toute
  nouvelle constante upstream non examinée.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` et
  `OutputLanguages.def` génèrent enums ordonnées, parseurs, noms, choix CLI et
  valeurs ABI C. `EVMConstants.h` centralise largeurs, limites et noms stables.
- `EVMCalls.def` décrit les quatre instructions qui appellent un autre programme
  et le treillis des provenances possibles d’une adresse de callee. Un seul
  drapeau par enregistrement, la présence d’un opérande de value entre le callee
  et la fenêtre d’arguments, dérive toutes les positions suivantes, et la table
  est validée face à la base d’opcodes pour que la dérivation ne dérive pas des
  compteurs de pop déclarés.
- `EVMPrecompiles.def` est le dictionnaire des adresses auxquelles le protocole
  répond lui-même, chacune avec le fork qui l’a réservée. Le gas en est
  volontairement absent : le coût d’une precompile est fonction de son entrée et
  a été retarifé sans que l’adresse ni l’opération changent.
- `EVMRecoveredFacts.def` détient les orthographes des vocabulaires de faits
  récupérés, afin qu’un nom atteignant la sortie vive à un seul endroit plutôt
  que dans un `switch` où un nouvel énumérateur peut être oublié.
  `EVMKnownSignatures.def` fait de même pour les trois rôles d’une signature.
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

- **EVM LowIR** conserve PC, encodage, état typé de l’immédiat et opérandes de
  profondeur de pile décodés (dont le remplissage à droite par des zéros pour
  PUSH et la règle de consommation conditionnelle d’EIP-8024), blocs, arêtes
  prédécesseur/successeur, cibles `JUMPDEST` validées, atteignabilité et domaines
  de hauteur de pile. La reconstruction du CFG est un point fixe déterministe à
  l’échelle du programme : chaque slot propage un ensemble fini borné de valeurs
  256 bits et chaque hauteur concrète conserve une pile abstraite. Les constantes
  transportées à travers les blocs d’appel/retour internes, les permutations de
  pile, `PC`/`CODESIZE` et les opérations ALU scalaires peuvent donc résoudre une
  ou plusieurs cibles concrètes. Une cible réellement inconnue reste une arête
  indirecte explicite au lieu d’être devinée.

  `AnalyzeOptions::MaxAbstractValuesPerSlot` borne chaque ensemble fini ; tout
  dépassement élargit le slot à `Unknown`. `MaxStackHeightVariants` borne le
  nombre de hauteurs dépendantes du chemin dans un bloc et produit une erreur
  explicite de limite d’analyse plutôt que de tronquer le CFG. Les deux limites
  refusent zéro. Les valeurs finies issues d’une opération cartésienne après une
  fusion de pile non relationnelle sont marquées comme sur-approximations : les
  candidats invalides sont diagnostiqués, mais ne suffisent pas à faire rejeter
  le bytecode par l’analyse stricte quand seule la corrélation des slots a été
  perdue. Une cible précisément invalide échoue toujours au PC exact du saut. En
  mode relâché, les fautes de pile sont diagnostiquées et ne terminent que le
  chemin abstrait fautif ; aucun fallthrough impossible n’est fabriqué.
- **EVM MedIR** représente chaque valeur de pile par une valeur SSA 256 bits et
  relie tous les phi de fusion avant d’exécuter une worklist déterministe de
  constantes clairsemées. Son treillis privé vaut `Uninitialized`, une
  `Constant` exacte ou `Overdefined` : les constantes égales se propagent entre
  blocs et cycles phi ancrés, tandis qu’un cycle contradictoire ou dépendant du
  runtime ne peut inventer une constante. La worklist vérifie les identifiants
  def-use et emploie le même évaluateur ALU de `Semantics.h` que l’interpréteur.
  MedIR conserve aussi l’effet sémantique principal et, orthogonalement, l’accès
  mémoire EVM `none/read/write/readwrite`, l’accès à l’état source et au
  call-value. À cette frontière, une pile LowIR polymorphe est alignée
  prudemment par le sommet ; les slots absents sur certains chemins deviennent
  des valeurs inconnues explicites et un diagnostic déterministe consigne la
  perte de précision.
- **EVM HighIR** récupère les selectors du dispatcher Solidity, les mots
  probables de calldata et de retour, la mutabilité, les slots de storage
  constants, les faits LOG/event et revert, ainsi que les régions function/CFG.
  Un index de producteurs vérifié et un parcours de valeurs itératif et mémoïsé
  récupèrent les faits depuis les opérandes typés de MedIR, et non selon la
  distance entre instructions : les comparaisons de selector peuvent traverser
  blocs et phi, employer les deux ordres d’opérandes de `EQ` et conserver un
  masque 32 bits dérivé ; offsets d’arguments, clés de storage, topic0 d’event,
  gardes non-payable/receive et tailles exactes de retour de 32 octets utilisent
  leurs entrées sémantiques. Le graphe MedIR borne structurellement ce parcours,
  qui traite toute expression mal formée, mixte ou cyclique comme inconnue. Les
  cibles contradictoires d’un même selector sont diagnostiquées et omises. La
  payabilité reste indépendante du treillis d’accès à l’état, et un saut
  dynamique atteignable non résolu impose une reconstruction `nonpayable`
  prudente. Tant que MedIR ne possède pas de memory SSA, la récupération du
  payload d’une custom error et celle de l’appel sortant restent les seules
  heuristiques à fenêtre d’instructions bornée ; noms et types récupérés demeurent explicitement heuristiques.

  HighIR enregistre aussi la moitié sortante de l’interface : chaque `CALL`,
  `CALLCODE`, `DELEGATECALL` et `STATICCALL`, avec la provenance du callee,
  l’adresse réservée qu’il nomme lorsque le fork analysé en réserve une, le
  selector que l’appel place en tête du calldata du callee, et la valeur
  transférée lorsqu’elle est constante. `CREATE` et `CREATE2` sont exclus car
  ils exécutent du code qui n’a pas encore d’adresse : il n’y a pas de callee à
  récupérer.

  Une signature sortante récupérée ne rejoint jamais les standards auxquels le
  programme répond. Envoyer `transfer(address,uint256)` dit que le programme
  utilise un token, pas qu’il en est un, et confondre les deux signalerait tout
  routeur et tout vault comme ERC-20. Un appel délégant est de plus signalé
  comme fait de proxy, car il est le seul membre de la famille dont le code du
  callee s’exécute sur le storage de ce programme.

  La recherche de precompile est filtrée par le fork analysé et non par le plus
  récent existant. Appeler l’adresse d’une precompile qu’un fork ultérieur
  introduit atteint un compte sans code, réussit et ne renvoie rien ; la nommer
  signalerait donc une opération que le programme n’a manifestement pas
  effectuée.
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
- Amsterdam/Bogota sont des cibles de développement explicites ; `latest`
  reste Fusaka finalisé jusqu’à la finalisation des opcodes prévus.
- Pas de RPC, découverte d’état, comptabilité gas/remboursement ou précompiles.
- L’extraction de création reconnaît des wrappers statiques, pas une transaction complète.
- Les sauts dynamiques restent indirects sauf preuve constante bornée.
- Types ABI, noms, mappings, événements et erreurs sont récupérés au mieux.
- L’exécution autonome des effets exige les hooks host C/Solidity.
