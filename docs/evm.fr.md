**Langues**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# Décompilation EVM

[← Index de la documentation](README.fr.md)

NeverD charge le bytecode historique de l’Ethereum Virtual Machine, construit
un LowIR 256 bits, un MedIR en SSA de pile et un HighIR récupéré, puis produit
du LLVM IR, du C23 ou du Solidity. L’analyse stricte est activée par défaut,
mais l’EVM legacy ne valide pas toute l’image à l’avance : un opcode non attribué
ou inactif n’est refusé à son PC exact que lorsqu’une lane d’exécution
définitivement `Reachable` prouve qu’elle l’atteint. Les octets morts et candidats
CFG seulement `MayReachable` ne deviennent pas des erreurs strictes.

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
et extrait la tranche runtime. Le parcours du constructeur emploie le même
décodeur d’instruction unique que le décodeur réel, sous le hardfork analysé :
un octet qui est une donnée sur un fork et un opcode sur un autre ne peut donc
pas déplacer la frontière. Un champ `deployedBytecode` ou `runtimeBytecode`
présent fait autorité : un `0x` explicite est accepté comme runtime vide qui
s’arrête naturellement et empêche volontairement le fallback vers le code de
création. Seul un champ absent permet d’essayer le candidat suivant ; un hex
absent ou composé uniquement d’espaces, sans préfixe explicite, est refusé. Une
entrée raw explicitement vide est également acceptée.

### Trailers de compilateur

`EVMMetadataFields.def` recense les deux formats de trailer. Solidity écrit une
map CBOR dont les deux derniers octets ne comptent que la map ; `vyper` écrit un
tableau CBOR terminé par cette map, dont les deux derniers octets comptent tout
le footer, eux compris. Lire un cadrage comme s’il était l’autre n’échoue pas
bruyamment : on tombe deux octets plus loin et on retire deux octets de code
réel. Les deux sont donc tentés, et une entrée qui ne correspond à aucun est
laissée telle quelle.

Le trailer est lu deux fois : une fois sur l’entrée telle qu’elle est donnée, une
fois sur le code runtime restant après déballage d’un wrapper de déploiement.
Vyper a déplacé son trailer dans l’initcode et laisse le code runtime sans
trailer ; un lecteur qui ne regarde qu’après déballage signale donc un build
inconnu pour un contrat qui s’était nommé. Un footer de séquence indique en outre
la longueur du code runtime, celles des sections de données et celle des
immutables, qui bornent le code renvoyé sans exécuter le constructeur.

### Conteneurs qui ne sont pas des instructions

`EVMBytecodeContainers.def` classe l’entrée avant tout décodage. Depuis
qu’EIP-3541 a rendu `0xEF` non déployable, un `0xEF` en tête promet que les
octets ne sont pas des instructions :

| Conteneur | Marqueur | Traitement |
|-----------|----------|------------|
| legacy | — | décodé comme instructions |
| délégation (`eip-7702`) | `0xef0100` et exactement 23 octets | signale le compte cible ; l’analyse s’arrête |
| eof (`eip-3540`) | `0xef00` | refusé ; aucun fork ne l’a activé |

Les vingt octets d’un indicateur de délégation sont une adresse, pas du code.
Les décoder lirait l’adresse comme des opcodes et produirait un graphe de flot de
contrôle d’un compte : `info` signale donc la cible et l’analyse refuse en
donnant la raison. Ce refus distingue les deux cas : avant Pectra le marqueur
n’est pas encore attribué, et à partir de Pectra le code runtime de la cible est
simplement absent. Un marqueur de toute autre longueur est une entrée mal formée
plutôt qu’une variante du conteneur, et reste des instructions pour que le
décodeur puisse nommer l’octet qu’il n’a pas su lire.

Hexadécimal mal formé, nombre impair de chiffres, placeholder de liaison non
résolu, artefact multi-contrat ambigu, bornes de metadata invalides et hex absent
ou blanc produisent une erreur exploitable. Un raw vide explicite ou runtime
`0x` reste un programme vide valide. `BytecodeLoadOptions::ArtifactContract`
sélectionne `Contract` ou `path/File.sol:Contract`. Un nom non qualifié est
refusé s’il existe dans plusieurs fichiers afin que l’ordre JSON ne choisisse
jamais silencieusement le mauvais contrat.

L’EVM est enregistré dans le registre central des loaders, pas dans un plugin
backend. CLI, API C, désassembleur, CFG et requêtes IR utilisent donc exactement
la même image normalisée et les mêmes options.

## Hardforks et opcodes

Tous les opcodes legacy attribués de Frontier à Fusaka sont couverts, dont
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
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2).
EOFv1/EIP-7692 n’est pas planifié et la proposition de conteneur
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540) est Stagnant. L’ancien dépôt
`execution-spec-tests` est archivé ; ses tests maintenus ont migré vers
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests).
NeverD ne présente donc pas un conteneur EOF expérimental comme comportement mainnet.

Le mode strict ne rejette un octet inconnu ou fork-inactive que si une lane
d’état définitivement `Reachable` prouve son exécution. `--evm-relaxed` le
conserve comme fault prefix typé et diagnostic, mais les backends fautent s’ils
l’atteignent ; il ne devient jamais un NOP.

## Architecture de metadata à la manière de LLVM

Les metadata EVM écrites à la main suivent le modèle `.def` multi-inclus de LLVM :

- `EVMOpcodes.def` est l’unique source de vérité pour tous les opcodes legacy
  finalisés et de développement opt-in : encodage, mutations pop/push
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
  exclusions historiques et EOF non planifiées volontaires ;
  `scripts/audit_evm_opcode_metadata.py` refuse toute dérive d’octet et toute
  nouvelle constante upstream non examinée.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` et
  `OutputLanguages.def` génèrent enums ordonnées, parseurs, noms, choix CLI et
  valeurs ABI C. `EVMAnalysisLimits.def`, `EVMInterpreterLimits.def`,
  `EVMABIParserLimits.def` et `EVMABITableLimits.def` déclarent les plafonds
  propres à l’analyse, l’interpréteur, le parser et les tables publiques.
  `EVMConstants.h` centralise les largeurs de protocole partagées et les noms
  internes stables, puis matérialise depuis `EVMAnalysisLimits.def` les valeurs
  par défaut de l’analyse et les noms d’options diagnostiques ; les headers de
  l’interpréteur et de l’ABI matérialisent les limites de leurs propres tables.
- `EVMCalls.def` décrit les quatre instructions qui appellent un autre programme
  et le treillis des provenances possibles d’une adresse de callee. Un seul
  drapeau par enregistrement, la présence d’un opérande de value entre le callee
  et la fenêtre d’arguments, dérive toutes les positions suivantes, et la table
  est validée face à la base d’opcodes pour que la dérivation ne dérive pas des
  compteurs de pop déclarés.
- `EVMPrecompiles.def` est le dictionnaire des adresses auxquelles le protocole
  répond lui-même, chacune avec le fork qui l’a réservée et la proposition qui
  l’a programmée. `P256VERIFY` à `0x100` est créditée à `eip-7951`, la
  proposition Final qui l’a réservée sur mainnet avec Fusaka ; la proposition
  rollup dont vient son interface ne l’a jamais programmée. Le gas en est
  volontairement absent : le coût d’une precompile est fonction de son entrée et
  a été retarifé sans que l’adresse ni l’opération changent.
- `EVMMetadataFields.def` et `EVMBytecodeContainers.def` décrivent ce qu’est une
  entrée avant tout décodage : les deux cadrages de trailer de compilateur, et
  les conteneurs dont les octets ne sont pas du tout des instructions.
- `EVMRecoveredFacts.def` détient les orthographes des vocabulaires de faits
  récupérés, afin qu’un nom atteignant la sortie vive à un seul endroit plutôt
  que dans un `switch` où un nouvel énumérateur peut être oublié.
  `EVMKnownSignatures.def` stocke une fois le spelling et le selector canoniques
  d’une fonction, puis sépare dans les `KnownFunctionVariantInfo` par standard
  les listes de retour et le rôle d’evidence independent/non-independent. Un
  spelling partagé ERC-20/ERC-721 reste donc un seul candidat appelable, sans
  prouver seul l’un des standards ni emprunter le retour du premier variant.
  Events et custom errors gardent des records typés distincts.
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

  Sur un back-edge, tout slot loop-carried modifié est sémantiquement
  sur-approximé à `Top` pour faire converger le point fixe ; cette abstraction de
  récurrence est indépendante des ressources. Instructions, blocs, états,
  valeurs, piles, lanes, edges, mises à jour de worklist et transferts
  instruction×lane ont des budgets nommés, dont `MaxAbstractValuesPerSlot`,
  `MaxStackHeightVariants` et `MaxAbstractInstructionTransfers`. Zéro ou leur épuisement est une erreur dure
  avant insertion, jamais un `emergency widening` ni une troncature silencieuse.

  `EVMLowFaultKinds.def::InvalidJumpDestination` reste sensible au chemin sur un
  `end-of-code JUMPI` : une condition certainement true vers une cible invalide
  n’a aucune fin réussie et produit un fault certain ; une condition certainement
  false réussit. Unknown ne conserve que son chemin false potentiellement réussi
  sans étiqueter à tort toute la lane comme fault certain.
- **EVM MedIR** représente chaque valeur de pile par une valeur SSA 256 bits et
  relie tous les phi de fusion avant d’exécuter une worklist déterministe de
  constantes clairsemées. Son treillis privé vaut `Uninitialized`, une
  `Constant` exacte ou `Overdefined` : les constantes égales se propagent entre
  blocs et cycles phi ancrés, tandis qu’un cycle contradictoire ou dépendant du
  runtime ne peut inventer une constante. La worklist vérifie les identifiants
  def-use ; valeurs, state lanes, entrées de pile, opérations, références
  operation-lane, entrées phi et mises à jour de worklist ont des limites
  indépendantes. Elle emploie le même évaluateur ALU de `Semantics.h` que l’interpréteur.
  MedIR conserve aussi l’effet sémantique principal et, orthogonalement, l’accès
  mémoire EVM `none/read/write/readwrite`, l’accès à l’état source et au
  call-value. Chaque lane de pile complète du LowIR garde une lane d’exécution
  SSA distincte et les phi nomment leur lane source ; des piles incompatibles
  ne sont plus alignées par leur hauteur maximale.
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
  prudente. Le dataflow mémoire byte par byte suit les écritures à offset constant
  entre blocs, compose overlap/kill et invalide les connaissances lors d’une
  écriture dynamique ou inconnue. Les preuves de payload couvrent actuellement
  le selector et les octets Panic connus. Pour une déclaration de custom error
  connue, l’emitter Solidity conserve les types de paramètres canoniques ; il ne
  prétend pas récupérer chaque valeur d’argument au runtime.

  La découverte des selectors part uniquement de la lane racine et suit les
  arêtes non appariées du dispatcher ; un test ressemblant à un selector dans un
  handler ne devient pas une fonction publique. Receive et fallback sont eux
  aussi contraints par la racine et exigent un terminal réussi définitivement
  atteignable : revert, fault, handler de calldata vide non-payable ou chemin
  seulement possible ne les établit pas. Un usage calldata contradictoire écarte
  le candidat canonique et un selector partagé n’apporte aucune evidence
  indépendante de standard. Seuls assez de selectors indépendants compatibles ou
  une preuve forte par topic/arity exacts, slot de storage ou proxy reconnaissent
  le standard et son variant. La liste de retours statiques n’est émise que si
  tous les terminaux réussis définitivement atteignables concordent sur le nombre
  exact d’octets ABI ; transfert non résolu, formes divergentes ou mismatch
  échouent fermés. Revert et fault ne sont pas des retours réussis. Les autres
  faits restent des candidats étayés.

  HighIR borne séparément fonctions, visites lane/opération, références de blocs
  de région, requêtes et octets mémoire, cellules d’état et mises à jour de
  worklist. Le point fixe mémoire ne consomme que les lanes exécutées
  définitivement atteignables, fait le meet par consensus d’octets et renvoie
  une erreur dure à l’épuisement du budget, sans tronquer les faits.

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

Avant tout effet propre à l’opcode, l’interpréteur preflight la hauteur requise
typée, les pops et la hauteur conservée plus les pushes ; underflow ou overflow
ne peut donc exécuter une demi-instruction. `EVMForkSemantics.def` choisit le sens
du byte `0x44` : `DIFFICULTY` avant Paris, `PREVRANDAO` à partir de Paris.
`REVERT`, faute sémantique, step limit et épuisement allocation/length restaurent
storage, transient storage, logs et effets selfdestruct au snapshot d’entrée,
tout en gardant diagnostics de frame et octets explicites de revert. Un échec
d’allocation devient `ExecutionFaultKind::ResourceExhausted` sans allouer un
message ; si le snapshot lui-même n’a pu être créé,
`HasPersistentStateSnapshot` vaut false et le résultat n’est jamais committable.

### Frontières publiques de l’IR et des ressources

La fonction publique `execute` vérifie d’abord que
`Code`/`Fork`/`Instructions`/`JumpDestinations` forment un LowIR canonique. Un
fork altéré, un enregistrement d’instruction forgé, un encoding incohérent ou
une table de destinations erronée renvoie donc `llvm::Error` avant que
l’interpréteur n’indexe la table d’instructions. Le `lowerToMedIR` public valide
dans l’ordre options, ressources et structure ; ensuite un
`canonical decode replay` décode `Low.Code` avec le fork/strictness embarqué et
compare chaque champ LowIR. Ce n’est qu’alors que `lowerCanonicalLowToMedIR`,
les index et les allocations proportionnelles à l’appelant sont permis. Le
`recoverHighIR` public replay-valide aussi les LowIR/MedIR externes. Les chemins
privés `lowerCanonicalLowToMedIR` et `recoverCanonicalHighIR` sont réservés à
l’IR détenu par `analyze` : ils sautent seulement le replay redondant non
récursif, tout en appliquant tous les HighIR option/resource budgets.

La preuve du dispatcher conserve un domaine selector trié
`Any/Exact/Excluded` par `MedStateLane`. Les joins unissent les ensembles Exact,
intersectent les exclusions Excluded et soustraient un ensemble Exact d’une
exclusion cofinie ; élargir le domaine remet la lane au travail. Une égalité ne
retient le candidat de l’arête true que si le selector reste autorisé, puis
l’exclut sur l’arête false. Un `XOR(selector, constant)` brut retient l’arête
zéro/false comme match lorsque tous les successeurs canoniques désignent la même
entrée ; ce fallthrough n’a pas besoin de viser un `JUMPDEST`. L’arête
non-zéro/true est le mismatch et exclut ce selector ; `ISZERO` transforme la
même expression en égalité. Selector word, calldata word nul, taille du calldata
et call value guard se raffinent arête par arête. Une condition unknown arrête
la preuve plutôt que de suivre une branche seulement possible.

Après reconnaissance d’un candidat fonction, le parcours de son scope continue
avec son `exact singleton selector`. Si la fonction revient au dispatcher
partagé, `SelectorEquality`, le `XOR` brut et `SelectorWord` ne suivent que le
`definite edge` compatible avec le selector déjà reconnu. Les prédicats Unknown
ou sans rapport conservent prudemment tous les `definite edges`. Aucune
heuristique d’exclusion des autres entry blocks n’est utilisée : le contrôle
légitime `shared body/tail-call` reste dans le scope de la fonction.

Les résultats externes de CALL/CREATE sont distincts : le résultat de l’hôte est
réellement non déterministe, donc l’analyse explore les deux arêtes CFG précises.
Elle conserve ainsi la récupération du fallback ERC-1167 sans prendre une
condition selector illisible pour une preuve ; un dispatcher réellement Unknown
échoue toujours de manière fermée.

`EVMAnalysisLimits.def` donne au decoder linéaire et au builder CFG un budget
agrégé commun de diagnostics LowIR via `MaxLowDiagnostics` et
`MaxLowDiagnosticBytes`. Les deux chemins préfacturent le nombre exact et les
octets finaux et refusent une limite nulle. Les budgets de diagnostic LowIR et
HighIR restent indépendants. La même table facture
`MaxHighDispatchCandidates`,
l’agrégat global `MaxHighRecoveredArguments`, `MaxHighDiagnostics` et
`MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells` et `MaxHighMemoryValueVisits`. Les enregistrements
de candidat et d’argument récupéré sont préfacturés avant insertion dans tout
conteneur cible ou allocation de nom/type. Chaque diagnostic de sortie HighIR
est facturé par nombre et octets finaux avant construction ou copie, y compris
le diagnostic fixe d’IR mal formé ; épuiser ce budget renvoie l’erreur dure
nommée, sans omettre silencieusement diagnostic ni fait.
La région CFG racine par défaut facture `MaxHighRegionBlockReferences` avant de
réserver ou copier sa liste de PC de blocs.

`EVMABIParserLimits.def` borne l’imbrication des tuples, les nœuds de type et
les dimensions de tableaux agrégées. `EVMABITableLimits.def` borne cardinalité
et texte agrégé des tables publiques de signatures/variantes. La validation
publique applique ces plafonds avant parsing ou hachage, puis rejette enums,
metadata de kind, standards, rôles d’evidence selector, types non canoniques,
hashes dérivés, memberships et collisions invalides. Le lookup selector de
production est indexé, le lookup event utilise une table triée par topic et les
API topic vérifient qu’un `APInt` mesure exactement un mot EVM avant comparaison
ou tri.

`EVMInterpreterLimits.def` déclare `MaxSteps`, `MaxMemoryBytes`,
`MaxTraceEntries`, `MaxLogEntries`, l’agrégat `MaxLogDataBytes`, l’agrégat
`MaxHostReturnDataBytes`, `MaxCalldataBytes`, l’agrégat
`MaxHostEnvironmentEntries`, l’agrégat `MaxExternalCodeBytes` et
`MaxPersistentStateEntries`. L’agrégat d’entrées hôte couvre `BlockHashes`,
`Balances`, `CodeHashes`, `ExternalCode` et `BlobHashes` ; la limite en octets
couvre tous les corps `ExternalCode`. `MaxSteps` conserve le
résultat explicite `StepLimit`. La croissance runtime de la mémoire, trace,
logs, données de log et nouvelles clés d’état persistant est préfacturée ;
dépasser ces plafonds renvoie `ResourceExhausted` et restaure état persistant,
logs et effets selfdestruct. Un agrégat initial de host return data ou une map
d’état persistant trop grande est plutôt une erreur d’API `execute`.
L’interpréteur garde les host return data en vues `ArrayRef` et emploie
`lower_bound` sur la table d’instructions triée et déjà validée, sans copier de
buffer ni reconstruire une map de PC à chaque exécution. Le
`const execute preflight` valide programme et limites hôte avant toute copie
d’environment, snapshot ou result.

### Audit différentiel live de go-ethereum

L’audit local standard et la CI forcent à chaque exécution
`git fetch --depth=1 --force` du `HEAD` distant de la branche par défaut officielle
`https://github.com/ethereum/go-ethereum.git`. Chaque exécution crée un dépôt
bare privé, temporaire et au nom imprévisible ;
il n’existe ni dépôt Git persistant partagé ni cache. Seuls l’authority ref
renvoyé par ce fetch et son SHA exact résolu choisissent la révision. Le SHA est
annoncé puis sondé dans un worktree temporaire detached ; le dépôt d’autorité et
le worktree sont ensuite détruits ensemble. Ni `local_docs`, ni un checkout existant, ni un submodule
ne sont une voie d’audit ; un submodule figé serait périmé précisément quand il
faut détecter la dérive live.

Chaque commande Git efface d’abord tous les `GIT_*` hérités, dont
`GIT_CONFIG_*`, puis n’installe que les valeurs auditées. `GIT_CONFIG_NOSYSTEM`
et `GIT_CONFIG_GLOBAL` désactivent les configurations système/globale ;
`GIT_ATTR_NOSYSTEM` et `core.attributesFile` au niveau commande désactivent les
attributs système/globaux, tandis que `core.hooksPath` désactive les hooks. Le
dépôt privé rejette toute configuration locale inattendue, les
grafts, `objects/info/alternates` et `refs/replace` ;
`GIT_NO_REPLACE_OBJECTS` désactive aussi les remplacements. Toute déviation
échoue de façon fermée.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

La CLI publique n’expose que `--manifest-output` ; source, ref et toolchain ne
sont pas sélectionnables. Son manifest fermé utilise `schema 3`. La sonde Go
réfléchit tout l’inventaire booléen exporté de `params.Rules`, appelle
`LookupInstructionSet(params.Rules)` pour chaque fork mappé et inspecte les 256
slots de byte. L’allocation est décidée uniquement par `operation.undefined`
de geth ; `HasCost` n’est qu’un contrôle croisé du coût, car il vaut aussi false
pour une opération définie de coût nul. Chaque slot `defined && !HasCost` doit
correspondre exactement à `EVM_GETH_ACTIVE_WITHOUT_COST` à partir de son fork
d’activation déclaré ; un slot indéfini avec coût, un slot défini non revu ou la
disparition upstream du marqueur provoquent un échec fermé. Chaque table compare
aussi `base_min_stack` et `net_stack_delta`. Tout champ ou
enregistrement inconnu, dupliqué, manquant, hors plage ou non analysé provoque
un échec. Chaque `.def parser` rejette aussi les entrées ressemblant à une macro
mais non consommées, au lieu d’accepter une policy `partial`.
`EVMUpstreamOpcodePolicy.def` porte alias et
exclusions historiques/EOF non planifiées typées, et valide leurs invariants
overlap/inactive. L’orthogonal `EVMUpstreamSemanticsPolicy.def` porte l’inventaire
fermé réfléchi de `params.Rules`, le mapping des forks, les exceptions base-stack
et les familles dynamic-immediate. La CI s’exécute sur les push vers `dev`, les pull requests,
le déclenchement manuel et le planning quotidien ; un échec publie comme artifact
la révision exacte, le manifest et le journal.

Plus précisément, `EVMUpstreamSemanticsPolicy.def` classe chaque champ booléen
exporté de `params.Rules` par une unique entrée `EVM_GETH_RULE_FIELD` comme
`MappedForkSelector`, `NoOpcodeAllocation` ou
`ExcludedSelectorExpectedError`. L’audit active chaque champ isolément et
appelle `LookupInstructionSet` : les deux premières catégories exigent nil
error, la troisième error ; l’empreinte opcode/stack complète des 256 slots doit
toujours égaler `ExpectedFork`. Les champs sans allocation `IsEIP155`,
`IsEIP2929`, `IsEIP4762` et `IsPetersburg` donnent Frontier ; `IsUBT` doit
échouer et donner l’empreinte Cancun.

`EVMUpstreamSemanticsPolicy.def` déclare les opcodes de chaque famille dynamique
EIP-8024, son type d’opération et son delta de pile valide ;
`EVMEIP8024Immediates.def` reste l’autorité distincte de décodage des immediates
et classe toutes les valeurs single/pair. Via `go -overlay`, l’audit obtient les
vrais handlers privés `operation.execute` et couvre, table par table, les
`canonical fork jump tables` et les `mainnet active/scheduled jump tables`.
Une famille `inactive` est consignée explicitement ; une famille `partial` est
une erreur. Pour chaque table active, les trois opérations déclarées sont
exécutées sur tous les immediates (`3x256`) et les `3 missing-operand cases`.
Acceptation, delta PC, opérandes/mutation issus des marqueurs, underflow exact et
comportement `0x00` absent sont comparés aux mêmes policies déclaratives, sans
dupliquer la formule.

`EVM_HARDFORK_LATEST` a exactement une cible canonique. Le mapping fermé
`EVMUpstreamForkAliases.def` envoie Prague vers Pectra, Osaka et BPO1 à BPO5 vers
Fusaka ; Paris, Shanghai, Cancun, Amsterdam et Bogota sont des identités. Tout
nouveau nom inconnu échoue de manière fermée. Chaque audit fixe et consigne un
`audit_unix_time`, impose que `MainnetChainConfig.LatestFork(time)` corresponde
au latest de NeverD et que `LatestFork(max uint64)` appartienne à l’inventaire
d’alias avec son fork canonique déjà sondé ; les deux tables d’instructions sont
comparées intégralement. Le manifest inscrit `authority=official-fresh-fetch`,
l’URL officielle, le `HEAD` demandé et le SHA résolu. Le probe fixe
`GOTOOLCHAIN=local`.

Go et Python imposent des `input/collection/string hard limits` avant de
matérialiser des métadonnées hostiles ; toute entrée, collection ou chaîne
surdimensionnée échoue de façon fermée. Pour `bounded diagnostic output`, un
affichage trop long porte le `digest` du contenu complet et un
`explicit truncated marker`. Chaque processus enfant a une sortie et une
échéance bornées ; tout dépassement tue le `process group` entier/process tree
puis draine ses pipes.

Le reçu live schema 3 actuel consigne `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, la révision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, `Go 1.24.0` local,
`stack_limit=1024` et `diagnostics=[]`. Il compare `21 fork tables` et
`20 Rules probes`, classés `15 mapped/4 no-op/1 expected-error`. Les deux entrées
`mainnet active/scheduled` nomment `upstream BPO2`, que l’alias fermé mappe vers
`NeverD Fusaka`. EIP-8024 couvre `23 table targets` ; seuls
`Amsterdam/Bogota` sont actifs, soit `1536 candidate executions` et
`6 missing-operand cases`. Les `three handler symbols` concordent sur les deux
cibles actives. L’audit Python `67/67` et `C++ Opcode 10/10` sont verts. Sur
macOS, l’audit réel a réussi sous `sandbox-exec`, le `go run` final hors ligne ;
le workflow Linux impose `bubblewrap`.

Toutes les phases Go — `go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` et `go run` — s’exécutent dans un sandbox filesystem
`capability-root`. Seuls le probe privé, geth fraîchement récupéré, le
`resolved GOROOT` validé et les racines runtime système strictement nécessaires
sont lisibles ; seules les racines d’environnement isolées sont inscriptibles.
Le réseau n’est accordé qu’aux phases de dépendances qui en ont besoin et le run
final reste hors ligne. Les sentinels du `host HOME/workspace` sont refusés et
leur contenu ne peut apparaître dans aucune sortie. Linux reproduit cette
politique avec `bubblewrap`, sans `/` broad bind.

`NeverDEVMDecoderPropertyTests` épuise tous les inputs de deux octets à chaque
fork qui modifie le decoder, compare le décodage complet et les frontières
`JUMPDEST` exactes, puis soumet à tous les forks des chaînes hostiles
déterministes de longueur bornée.

Les lanes de chemin LowIR/MedIR conservent les corrélations et `MayReachable` ne
fournit qu’un candidat CFG. Selector, receive, fallback, forme de retour et
mémoire byte par byte de HighIR ne consomment que des lanes exécutées
définitivement atteignables. Les selectors partagés sont séparés des
`KnownFunctionVariantInfo` par standard et le retour doit satisfaire tous les
terminaux réussis. Tout budget épuisé échoue bruyamment, sans emergency widening
ni troncature silencieuse.

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
