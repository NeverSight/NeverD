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
| `unittests/lift` | `NeverDLiftTests` | Formes LowIR decoder/lifter, étapes IR, loaders, relocations, fixtures de format, décompilation et patch représentatif |
| La plupart de `unittests/semantic` | `NeverDSemanticTests` | Sémantique différentielle des instructions, ABI, contrôle, expressions C et lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMAnalyzerTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadata hardfork, normalisation d’entrée, CFG/SSA/récupération, sémantique interpréteur, exécution différentielle LLVM/C/Solidity et API publique |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFSemanticTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFEmitterTests`, `NeverDSBFIntegrationTests` | Métadonnées v0-v4 et dispositions ELF, vérification stricte, CFG/récupération, exécution brute indépendante, vérification LLVM, compilation C/Rust et routage de l’API publique |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Équivalence réécriture/obfuscation sur quatre ISA et trois formats objet |
| Fichiers de transformation ciblés dans `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sondes rapides à relier séparées du gros binaire sémantique |

Les références d’enregistrement sont
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) et
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) et
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt).

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
cmake --build build-release --target \
  NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFAnalyzerTests \
  NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests NeverDSBFEmitterTests \
  NeverDSBFIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'SBF' --output-on-failure --parallel 4
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
`.github/workflows/ci.yml` et `scripts/audit_ci_test_inventory.py`. Comme aucun
shard de la matrice ne représente toutes les suites coûteuses, un
`check-neverd` local reste le signal pré-fusion complet le plus clair si la
machine possède tous les outils croisés nécessaires.

## Profil actuel de conformité et de sanitizers Solana SBF

Cette liste actuelle remplace la liste SBF abrégée ci-dessus. La suite source
differentielle exige `rustc` en plus de clang ; un compiler skip signifie une
couverture absente. L’agrégat complet comprend `NeverDSBFProgramImageTests`,
`NeverDSBFMalformedCorpusTests`, `NeverDSBFISAConformanceTests`,
`NeverDSBFUpstreamConformanceTests`, `NeverDSBFLLVMDifferentialTests` et
`NeverDSBFSourceDifferentialTests`, ainsi que les targets metadata, loader,
analyzer, semantic, emitter et integration. Le profil intégré réussit 124/124
cas dans 13 binaires.

Le profil sanitizer se construit séparément dans `build-sbf-asan-ubsan`. Il
réussit 121/121 cas core dans 12 binaires sans rapport ASan ou UBSan ;
l’integration reste dans la build LLVM intégrée car le package prebuilt omet le
header fork-only requis.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```
