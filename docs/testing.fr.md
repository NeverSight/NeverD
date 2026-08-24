**Langues**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← Index de la documentation](README.fr.md)

# Tester NeverD

Les tests de NeverD répondent à trois questions distinctes : la représentation
a-t-elle la forme attendue, un parcours complet fonctionne-t-il avec une
fixture binaire et le code généré préserve-t-il le comportement ? Choisissez la
plus petite suite qui répond à la question du changement, puis exécutez
l’agrégat plus large avant une pull request à haut risque.

## Configurer une compilation de test

Les tests sont désactivés sans `BUILD_TESTING`. Une compilation Release est le
choix normal pour la suite complète ; Debug conserve les assertions et le pas à
pas, mais n’est volontairement pas optimisé ni représentatif des benchmarks de
décodage.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

L’ensemble complet de fixtures nécessite `clang` pour compiler vers plusieurs
cibles et les linkers LLVM (`ld.lld` et `lld-link`) sur le `PATH`. CMake produit
sans condition de nombreux objets relogeables et les fixtures ELF/PE liées
lorsque le linker correspondant existe. Un test ignoré parce que l’hôte ne peut
pas compiler ou lier sa fixture est une couverture non exécutée, pas une
réussite de la cible.

Consultez [CONTRIBUTING.md](i18n/CONTRIBUTING.fr.md) pour le clone, les profils
de compilation et LLVM précompilé sur macOS.

## Organisation des tests

`add_neverd_unittest` crée un exécutable GoogleTest et attribue à chaque cas
découvert un label CTest identique au nom de cette cible.

| Zone source | Cible et label CTest | Couverture |
|-------------|----------------------|------------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Invocation de sous-processus multiplateforme, quoting, redirections et codes de sortie |
| `unittests/libc` | `NeverDLibCTests` | Noms libc connus et classification |
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Catalogue de puits, priorité d’identité, préfiltre d’arguments, chasse de débordement de copie, audit de durée de vie du tas et matrice obligatoire de six cellules PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/lift` | `NeverDLiftTests` | Formes LowIR decoder/lifter, étapes IR, loaders, relocations, fixtures de format, décompilation et patch représentatif |
| La plupart de `unittests/semantic` | `NeverDSemanticTests` | Sémantique différentielle des instructions, ABI, contrôle, expressions C et lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMAnalyzerTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadata hardfork, normalisation d’entrée, CFG/SSA/récupération, sémantique interpréteur, exécution différentielle LLVM/C/Solidity et API publique |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | Métadonnées v0-v4 et dispositions ELF, comportement strict du verifier/loader, 23 artefacts ELF épinglés, oracle officiel indépendant, disponibilité exhaustive des opcodes, entrées hostiles, CFG/récupération et différences exécutées LLVM/C/Rust |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Équivalence réécriture/obfuscation sur quatre ISA et trois formats objet |
| Fichiers de transformation ciblés dans `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sondes rapides à relier séparées du gros binaire sémantique |
| `unittests/corpus` (sous-module) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests` | Métadonnées d’exceptions et d’exécution lues dans 317 binaires réels épinglés, chacun accompagné d’un manifeste énonçant les planchers que sa récupération doit franchir |

Les références d’enregistrement sont
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) et
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) et
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt) et
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt).

### Le corpus binaire épinglé

Chaque autre suite construit ce qu’elle teste ; le corpus, non : c’est un
sous-module de binaires produits par de vraies chaînes d’outils, sur des hôtes
et pour des cibles que ce dépôt ne peut pas atteindre. Chacun est épinglé par
empreinte, et le manifeste voisin énonce les planchers que sa récupération doit
franchir. C’est le seul endroit où une affirmation sur ce que NeverD lit dans,
disons, un objet partagé `armv7` compilé en `-O2` et dépouillé trouve une
réponse plutôt qu’un débat.

Les suites ne sont construites que si l’étape de configuration a reçu l’ordre
de les chercher : ce drapeau est donc tout ce qui les maintient sous test.

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` exécute toutes les lignes ;
`check-neverd-windows-eh-corpus`, `check-neverd-rust-eh-corpus`,
`check-neverd-go-eh-corpus`, `check-neverd-cxx-itanium-eh-corpus` et
`check-neverd-objc-eh-corpus` en exécutent une chacune. Les trois hôtes de CI
configurent avec le drapeau et passent les cinq lignes : les octets sont
identiques partout, mais ce qui les lit ne l’est pas, et un passage du corpus
sur un hôte ne prouve rien sur les deux autres.
`scripts/audit_ci_test_inventory.py` refuse un inventaire auquel manque l’un des
cinq labels, car une compilation qui a cessé sans bruit de lire le corpus est
une régression qu’aucun test ne peut attraper — le test est justement ce qui a
disparu.

À chaque exécution, l’audit des opcodes EVM effectue un `git fetch` superficiel
du `HEAD` distant du
[dépôt go-ethereum officiel](https://github.com/ethereum/go-ethereum), puis
indique le commit exact audité. Il réutilise le cache bare ignoré
`build/evm-opcode-audit/go-ethereum.git`, mais le rafraîchit avant de lire
l’inventaire fermé des opcodes et leur affectation d’octets :

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

CI exécute ce même audit live à chaque push et pull request, à la demande et une
fois par jour, afin de détecter la dérive upstream même sans modification de
NeverD. Pour une reproduction hors ligne ou historique, sélectionnez
explicitement un checkout existant :

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

L’audit n’autorise que les exclusions nommées dans
`EVMUpstreamOpcodePolicy.def` ; tout opcode upstream ni représenté ni
explicitement examiné fait échouer la commande. Son parser et ses diagnostics
de dérive disposent d’une couverture unitaire Python indépendante en CI :

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Pour une modification du contrôle de flux EVM, exécutez d’abord le contrat de
point fixe et de domaine des hauteurs :

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Ces cas couvrent les retours internes entre blocs, les fusions finies à
plusieurs cibles, la convergence des boucles et l’ordre déterministe des arêtes,
les hauteurs de pile dépendantes du chemin, le widening borné, la
sur-approximation cartésienne induite par la corrélation, les sauts inconnus,
les cibles précisément invalides et les fautes de pile en modes strict et
relâché. Exécutez ensuite les sept binaires EVM et l’audit des métadonnées
upstream ; une modification du CFG peut affecter l’emitter et l’intégration même
si la forme locale de l’analyseur est correcte.

Pour les modifications de dataflow MedIR/HighIR, exécutez aussi les contrats de
phi constant, selector, opérande typé, graphe mal formé et chaîne profonde :

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

Ces cas prouvent les phi cycliques égaux et contradictoires, les expressions de
selector non adjacentes et inter-blocs, les deux ordres d’opérandes d’égalité,
les contrôles exacts de largeur ABI, les opérandes typés storage/event/calldata,
le traitement déterministe d’un MedIR mal formé et un parcours itératif de
16 384 valeurs productrices.

## Production des fixtures

### Fixtures de lift et de format

`unittests/lift/CMakeLists.txt` compile les sources C et assembleur vers
plusieurs cibles pendant le build. Les triples Clang produisent des objets ELF
x86-64, i386, AArch64 et ARM32, des objets et images liées PE/COFF, ainsi que des
objets Mach-O i386 PIC/non-PIC. Avec LLD, certains objets sont aussi liés en
exécutables pour les tests de patch. `NeverDLiftTests` dépend de la cible
`lift-test-objects` ; une compilation normale de ce binaire rafraîchit donc les
fixtures générées.

La plupart des tests lift utilisent `NeverDLiftFixture.h` pour invoquer le CLI
`neverd` construit et inspecter LowIR, MedIR, HighIR, LLVM IR, le C généré ou un
binaire réécrit. La variable d’environnement `NEVERD` peut remplacer le chemin
du CLI lors d’une expérience manuelle ciblée ; les exécutions CTest ordinaires
utilisent l’exécutable intégré par CMake.

### Fixtures de sûreté mémoire

`unittests/safety/fixtures/binaries` contient des images PE, ELF et Mach-O
versionnées pour x86-64 et AArch64, accompagnées du PDB ou du dSYM que fournit
chaque format et d’un MAP d’éditeur de liens pour chaque image. Le MAP est ce
qu’une compilation dépouillée livre encore, aussi chaque cellule est-elle
également analysée en nommant le MAP explicitement, ce qui fige ce qu’un
résultat a le droit d’affirmer lorsqu’il ne reste ni types ni lignes source.
`NeverDSafetyIntegrationTests` exécute les six cellules sur chaque hôte ; la
configuration échoue si une image ou un fichier compagnon requis manque, et la
suite n’a aucun chemin de contournement lié à la chaîne d’outils de l’hôte.

Les binaires équivalents proviennent d’un seul fichier source. Reconstruisez la
fixture smoke native de l’hôte avec `make`, ou régénérez la matrice complète
versionnée avec :

```bash
make -C unittests/safety/fixtures matrix
```

La recette de la matrice exige les cibles croisées Linux et Windows de Clang,
les outils COFF de LLD, les deux architectures Darwin et `dsymutil`. Ses chemins
de débogage sont remappés et l’enregistrement de la ligne de commande CodeView
est désactivé, afin que les compagnons versionnés ne capturent pas le chemin
absolu de l’espace de travail d’un développeur.

### Reconstruction des exceptions Windows

Les modifications des exceptions Windows fondées sur des tables exigent à la
fois des tests de représentation et un test de patch sur un PE lié. Le filtre
de lift ciblé couvre le modèle normalisé unwind/SEH/C++, les entrées corrompues,
les arêtes exceptionnelles du CFG, HighIR, la génération LLVM WinEH, le
remplacement du répertoire d’exceptions et la reconstruction Guard CF/EH
continuation :

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

La fixture assembleur x64 protégée nécessite la cible Windows de Clang et
`lld-link` ; son édition de liens CMake utilise `/guard:cf` et `/guard:ehcont`.
Un skip dû à l’absence du cross-linker ne prouve pas le chemin final-image. Un
test d’intégration réussi démontre que le PE réécrit peut être rechargé et que
ses tables runtime-function, unwind, load-config, Guard CF et Guard EH
continuation restent triées, présentes dans le fichier et limitées à des cibles
exécutables.

La fixture FH3 liée couvre séparément la fermeture C++ native : tables d’état
fixes, annotations HighC, conservation de la personality, cibles catch générées
et graphe IP-to-state rechargé.

Voir [Reconstruction des exceptions Windows](windows-exception-reconstruction.fr.md)
pour la matrice de support analyse/native et le contrat de patch fail-closed.

### Modèles d'exceptions par langage

Tout ce qui n'est pas le modèle tabulaire Windows tient dans une cible ciblée.
`NeverDLanguageEHTests` couvre la chaîne de frames DWARF, la zone de données
spécifique au langage d'Itanium, ARM EHABI, le compact unwind de Darwin, les
métadonnées de frame du runtime Go, la machinerie de panique de Rust et les
trois runtimes Objective-C :

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

Les tables de cette suite sont assemblées octet par octet plutôt que compilées,
car la plupart des combinaisons visées ne sont émises conjointement par aucune
chaîne d'outils. Objective-C en est le cas le plus net : les trois runtimes
émettent une LSDA Itanium et ne diffèrent que par le contenu d'un emplacement de
la table de types — et cette différence est totale, non graduelle. L'emplacement
d'Apple adresse un `objc_typeinfo` dont les deux premiers champs imitent
délibérément `std::type_info` ; celui d'Objective-C++ de GNUstep adresse une
véritable sous-classe de `std::type_info` ; et celui du runtime GNU n'est même
pas un pointeur, mais la chaîne du nom de classe elle-même. Appliquer la
convention d'un runtime à la table d'un autre n'échoue pas : cela rapporte un
nom de classe lu au milieu d'autre chose. C'est pourquoi le runtime est établi à
partir de la personality de la frame avant qu'un seul emplacement ne soit lu.

La même suite fige deux distinctions faciles à confondre et fausses une fois
confondues. `@catch(id)` et `@catch(...)` sont des gestionnaires différents — le
premier prend n'importe quel objet Objective-C et laisse une exception étrangère
poursuivre sa route — et chaque runtime les écrit différemment ; un décodeur qui
rapporte les deux comme un catch-all pose un gestionnaire sur des exceptions qui
seraient en fait passées à côté. Et une table de sites d'appel setjmp/longjmp
indexe des sites d'appel et non des adresses : un lecteur qui ne reconnaît pas
l'une des personalities SJLJ n'échoue pas, il invente des plages protégées et
des landing pads que le programme n'a jamais nommés.

Reconnaître cette forme n'est pas la refuser. Une entrée SJLJ est une paire de
valeurs ULEB128 — un sélecteur de dispatch et un décalage d'action — et ce
décalage y signifie exactement ce qu'il signifie dans la forme adressée : la
chaîne d'actions, les types rattrapés et les spécifications d'exception se
lisent donc tous dans une table qui ne nomme aucun code. Seule la région que
garde chaque entrée reste inconnue, car ce sont les écritures que la fonction
fait elle-même dans son emplacement de call-site qui l'énoncent, et non quoi
que ce soit dans la table. La suite fixe aussi l'octet auquel il ne faut pas se
fier ici : GCC écrit `DW_EH_PE_uleb128` comme encodage de call-site et LLVM
écrit `DW_EH_PE_udata4`, tous deux émettent ensuite de l'ULEB128 quoi qu'il
arrive, et aucune personality ne le lit jamais — un décodeur ne le doit donc
pas non plus.

L'identité de la personality est fixée en même temps, car c'est elle qui décide
comment se lit chacune des tables ci-dessus. GNAT nomme sa routine des trois
façons dont GCC nomme celle de chaque frontal — `_v0`, `_sj0`, `_seh0` — et,
sous Windows, enregistre un symbole tout en renvoyant vers un autre : les
quatre graphies doivent donc aboutir à Ada. D en est l'image inversée : trois
compilateurs, trois noms pour une seule routine, un seul jeu de tables derrière
eux.

### Allers-retours différentiels Unicorn

La fixture sémantique teste le comportement plutôt que la forme textuelle :

1. Écrire un petit cas C/assembleur ou construire du LLVM IR.
2. Le compiler avec Clang/LLVM pour la cible demandée.
3. Exécuter le code machine original dans Unicorn et capturer le retour attendu ou un autre état défini par la fixture.
4. Le charger et le lifter dans NeverD, émettre LLVM IR puis recompiler le résultat en code machine.
5. Exécuter le code régénéré avec les mêmes ABI, entrées, disposition mémoire et modèle CPU.
6. Comparer les résultats observables.

L’implémentation principale est
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h).
La fixture patch-full utilise `Codegen::compileForRewrite`, le même backend de
réécriture que les opérations patch, puis compare le code de référence et
transformé sur toute la grille ISA/format 4×3.

Un échec sémantique déterministe de NeverD doit faire échouer le test. Réservez
les skips aux frontières explicites de capacité externe et lisez leur raison :
un résumé vert sans cross-linker ne prouve pas que le parcours du format a été
exécuté.

### Backends différentiels EVM

Les tests interpréteur fournissent un oracle déterministe 256 bits. La suite
emitter compile et exécute LLVM, abaisse C23 avec Clang vers le même host harness
et, si `solc`, `anvil`, `cast` et `jq` sont présents, déploie le Solidity généré
localement. Elle compare status, storage et compteurs de trace. Un corpus raw
séparé exécute ALU pré-Fusaka, copies calldata/memory, `MCOPY` superposé, Keccak
et return data dans l’EVM native d’Anvil.

`NeverDEVMOpcodeTests` impose aussi l’architecture metadata : les 150 opcodes
font un roundtrip encoding/valeur typée ; limites de familles, alias hardfork et
maxima stack/host dérivés sont vérifiés.

### Backends différentiels Solana SBF

Les tests de métadonnées SBF valident chaque fonctionnalité de version, les frontières de collision d’opcodes, les hash syscall Murmur3, les relocations et les constantes de machine ELF, de registre et d’adresse VM. Les fixtures du loader génèrent, sans binaire incorporé, les dispositions historiques à sections v0-v2 et les dispositions strictes v3/v4 sans section, fondées sur les program headers.

`NeverDSBFISAConformanceTests` vérifie chaque encodage d’octet pour chaque
version v0-v4 face à un manifeste typé audité indépendamment.
`NeverDSBFExternalOracleTests` compare ensuite les décisions d’activation et
de frontière avec un processus Anza officiel construit séparément.
`NeverDSBFUpstreamConformanceTests` attribue un résultat explicite aux 23 ELF
à la révision Anza épinglée.

`NeverDSBFSemanticTests` exécute directement les octets d’instruction vérifiés et ne consomme pas le MedIR : modifier ou corrompre l’IR normalisé ne peut donc pas faire coïncider accidentellement l’oracle source avec un backend. Il couvre la sémantique v2 non monotone, la mémoire, les syscalls, les frames d’appels internes, les fautes, les traces et les limites de ressources. Les modules LLVM sont vérifiés ; le C généré est compilé avec les avertissements traités comme erreurs, et Rust avec `-D warnings`. Les tests de l’API publique parcourent tous les niveaux IR, le désassemblage, le CFG, les métadonnées, LLVM, C et Rust depuis un ELF SBF strict généré.

## Cibles en une commande

Les cibles personnalisées construisent leurs dépendances puis exécutent CTest
avec un parallélisme dérivé des CPU de l’hôte :

| Cible CMake | Sélection |
|-------------|-----------|
| `check-neverd` | Tous les tests enregistrés |
| `check-neverd-semantic` | `NeverDSemanticTests` uniquement |
| `check-neverd-sbf` | Toutes les cibles/tous les cas `NeverDSBF*Tests` |
| `check-neverd-patch-full` | `NeverDPatchFullTests` uniquement |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests` uniquement |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests` uniquement |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests` uniquement |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` et `NeverDAvxUpperXformTests` n’ont actuellement pas
de cible pratique `check-neverd-*`. Construisez-les et sélectionnez leur label
comme ci-dessous. `check-neverd-semantic` n’inclut pas non plus les binaires de
transformation ou patch-full séparés ; utilisez `check-neverd` pour l’agrégat
complet.

## Flux CTest incrémental

Construisez d’abord l’exécutable propriétaire, puis sélectionnez son label. Vous
évitez ainsi de relier de grandes cibles sémantiques sans rapport.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# Toutes les cibles/tous les cas EVM ciblés
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Toutes les cibles/tous les cas Solana SBF ciblés
cmake --build build-release --target check-neverd-sbf --parallel 4
```

Utilisez un nom CTest dérivé de GoogleTest pour une seule régression :

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Sélecteurs utiles :

| Commande | Rôle |
|----------|------|
| `ctest --test-dir build-release -N` | Lister les cas découverts sans les exécuter |
| `ctest --test-dir build-release -L '<regex>'` | Sélectionner un label de binaire de test |
| `ctest --test-dir build-release -R '<regex>'` | Sélectionner des noms de cas |
| `ctest --test-dir build-release --output-on-failure` | Afficher les diagnostics uniquement en cas d’échec |
| `ctest --test-dir build-release --stop-on-failure` | Arrêter au premier échec |
| `ctest --test-dir build-release --parallel 4` | Exécuter jusqu’à quatre cas en parallèle |

La découverte GoogleTest utilise `DISCOVERY_MODE PRE_TEST` ; le binaire
correspondant doit donc exister avant l’énumération par CTest. Les timeouts par
cas et de découverte séparés sont définis dans `cmake/AddNeverD.cmake` et ne
doivent être élargis que pour des suites dont les cas lourds ont été mesurés.

## Quels tests changent avec le code ?

| Zone modifiée | Commencer par | Puis envisager |
|---------------|---------------|----------------|
| Lifter d’architecture ou decode | Cas nommé dans `NeverDLiftTests` | Aller-retour sémantique de l’ISA correspondant |
| CFG LowIR, découverte de fonctions, tables de saut | Cas lift CFG/switch | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests` ou `NeverDTwoTableXformTests` |
| MedIR, ABI, flags, types, SSA | Cas lift MedIR/convention d’appel | Cas `NeverDSemanticTests` multi-ISA |
| HighIR ou C structuré | Cas HighIR/decompile | `NeverDCFGLoopXformTests` et compilation du C généré |
| Loader PE/ELF/Mach-O ou relocation d’entrée | Fixture de format correspondante dans `unittests/lift` | Test de chargement/décompilation toutes étapes de la cellule |
| Codegen de réécriture ou relocation de sortie | Cas `RewriteCodegenRTTests` | `NeverDPatchFullTests` et fixture patch liée si disponible |
| Transformation LLVM IR utilisée par patch | Binaire de transformation ciblé | Grille de passes composées `NeverDPatchFullTests` |
| C API ou CLI | Test SDK/query direct et `unittests/semantic/CLIEndToEndTests.cpp` | Suite pipeline/format pertinente |
| Loader, opcode, IR ou backend EVM | Plus petite cible propriétaire `NeverDEVM*Tests` | Toutes les cibles EVM et compilation du C/Solidity généré |
| Loader, ISA, IR ou backend SBF | Plus petite cible propriétaire `NeverDSBF*Tests` | Toutes les cibles SBF et compilation du C/Rust généré |
| Reconnaissance libc | `NeverDLibCTests` | Cas sémantiques call/ABI si le comportement change |
| Audit de durée de vie du tas ou chasse de débordement de copie | `NeverDSafetyTests` | Les six cellules de `NeverDSafetyIntegrationTests` |
| Exécution ou quoting de processus | `NeverDTestProcessTests` | Un cas CLI/sémantique affecté sur chaque hôte pris en charge |

Les tests doivent exprimer le contrat à la frontière stable la plus basse. Un
test de forme LowIR est utile pour attribuer le lifter ; un aller-retour
sémantique est nécessaire si deux formes IR plausibles peuvent se comporter
différemment. Évitez les dumps de fonction complets quand une petite assertion
opcode, CFG ou d’état observable suffit.

## Relation avec la CI

La CI construit en Release avec les tests activés sur Linux, macOS et Windows,
puis audite l’inventaire découvert avant d’appliquer les exclusions de labels
propres à la plateforme. Les profils sont définis dans
`.github/workflows/ci.yml` et `scripts/audit_ci_test_inventory.py`.
`NeverDSafetyTests` et `NeverDSafetyIntegrationTests` sont exigés sur chaque
hôte de la matrice ; chaque exécution lit les mêmes fixtures PE, ELF et Mach-O
versionnées pour x86-64 et AArch64. Comme aucun shard de la matrice ne représente
toutes les suites coûteuses, un `check-neverd` local reste le signal pré-fusion
complet le plus clair si la machine possède tous les outils croisés nécessaires.

## Profil actuel de conformité et de sanitizers Solana SBF

Cette liste actuelle remplace la liste SBF abrégée ci-dessus. La suite source
differentielle exige `rustc` en plus de clang ; un compiler skip signifie une
couverture absente. L’agrégat complet comprend `NeverDSBFProgramImageTests`,
`NeverDSBFMalformedCorpusTests`, `NeverDSBFISAConformanceTests`,
`NeverDSBFUpstreamConformanceTests`, `NeverDSBFLLVMDifferentialTests` et
`NeverDSBFSourceDifferentialTests`, ainsi que les targets metadata, loader,
analyzer, semantic, emitter et integration. Le profil intégré enregistre les
targets nommées et leurs résultats, pas un total rapidement variable.

Le profil sanitizer se construit séparément dans `build-sbf-asan-ubsan`. Les
targets ciblées tournent en fail-fast sans rapport ASan ou UBSan ;
l’integration reste dans la build LLVM intégrée car le package prebuilt omet le
header fork-only requis.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```

### Snapshot de preuve SBF épinglé (2026-08-24)

La gate épingle Anza `sbpf` sur
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave sur
`ef210d67f2fabeee1730498188fa78854260c679` et le SDK Solana sur
`122f32e571ce39face4beffaccea733e37c207fd`. Le manifest ELF officiel réussit
23/23 ; `NeverDSBFExternalOracleTests` confronte 1,411 cas opcode/boundary via
`SBFOfficialOracleProtocol.def`, `SBFOfficialVerifierCases.def` et
`SBFOfficialExecutionConstants.def`.
`SBFOfficialELFMutations.def` est le contrat tabulé des ELF malformés ; son
total variable n’est pas figé.
Séparément, le `41-case strict ELF differential` exécute toute la matrice
strict-v3 via `verify-elf-batch` officiel et NeverD ; ses 41 cas ne font pas
partie du total 1,411.

La matrice d’exécution officielle supplémentaire reste séparée : exactement 508
cas actifs `(Version,Opcode)` plus 58 cas de frontière donnent 566 cas
d’exécution exacte. Elle ne remplace pas les 1,411 probes du verifier ni le
`41-case strict ELF differential`, et n’entre dans aucun de ces totaux.
`NeverDSBFAgaveConformanceTests` authentifie Firedancer test-vectors
`68bb4af40235562e8852fa23d5727e49c2a0b862` et confronte les 1,955 `sol_compat_elf_loader_v1` fixtures du
loader (1,399 acceptées, 556 rejetées). Pour chaque ELF accepté, elle compare
`entry_pc`, `text_off`, `text_cnt`, `rodata_hash` et `calldests_hash`. Cette gate n’exécute pas le verifier
d’instructions ultérieur.
La Linux Release CI utilise `--print-pinned-revision`,
`--print-test-vectors-revision` et `--print-toolchain`, puis exporte
`NEVERD_SBPF_ORACLE` et `NEVERD_AGAVE_CONFORMANCE_ROOT`, rendant les deux gates
externes obligatoires. Localement, sans environnement oracle/corpus explicite,
les cas sont découverts mais peuvent skip.

`SBF_RUNTIME_VERSION` rend `RuntimeVersionPolicy::ChainProfile` dépendant du
cluster/slot historique : les feature accounts officielles font progresser
l’ISA maximal de V0 à V1, V2 puis V3 ; il reste aujourd’hui V3. v4 explicite
utilise `RuntimeVersionPolicy::UpstreamToolchain` pour
l’analyse offline. La limite actuelle de 10 MiB vaut exactement `10'485'760`
octets ; 65,536 n’est qu’une provenance/test historique. `SBFFaultCodes.def`
stabilise les valeurs de fault d’exécution et `SBFSourceStatuses.def` possède
séparément l’ABI du source généré.

Les fixtures à l’échelle 10,000 protègent worklist, function ownership et
multi-latch sans figer un temps machine. Les lignes cluster/account/slot
permettent un `RPC activation audit`, les tests ordinaires restant déterministes
et offline.
