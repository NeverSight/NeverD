**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 3ceb56e2abf86f594f437a16b09a1888650cb49d4cf06860711e73458c52c68d -->

[← Documentation Index](README.md)

# Correctifs de source et objets natifs

NeverD compile une fonction de remplacement avec le compilateur choisi, puis l’installe à une entrée originale explicite. Le compilateur définit la sémantique C/C++ ; NeverD gère les adresses, les relocations, le stockage, les informations de déroulement et la publication. La réparation binaire et la réécriture du code levé gardent leurs pipelines. Le reste du programme n’est pas reconstruit.

<!-- i18n-section: compiler -->
## Choix du compilateur

Choisissez `--compiler=clang`, `--compiler=gcc` ou `--compiler=neverc`. Clang et le mode ordinaire de NeverC peuvent produire de l’IR LLVM ; `object` conserve les instructions natives. La CLI choisit les objets par défaut pour GCC. Le SDK Python choisit aussi les objets pour GCC. Aucun autre compilateur n’est utilisé implicitement.

Utilisez `--compiler-arg` ou `compiler_args` du SDK pour les chemins d’inclusion, les définitions de macros et les options source prises en charge. Les changements de cible ou de mode de sortie et les jetons de fichier de réponse (`@file`) sont refusés à toute position, y compris comme valeur séparée d’une option. Les chemins `-I` et les définitions `-D` ordinaires restent pris en charge.

Remplacez les adresses des exemples par des entrées originales vérifiées. La déclaration doit respecter l’ABI machine originale, notamment la convention d’appel et la disposition des structures ; une adresse ne le prouve pas. Pour C++, utilisez `--language=c++` et le bon exécutable Clang++/G++. Préférez une entrée `extern "C"` ou indiquez son nom décoré exact. Les fonctions auxiliaires sont autorisées. `--entry` et `--func` sélectionnent explicitement le remplacement ; l’ancien `--from-c` respecte aussi l’adresse.

Vous pouvez aussi choisir explicitement `artifact="object"`.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## Profils vérifiés et limites

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C et DynCode hébergé**

NeverC accepte uniquement du C pur sur ELF, PE et Mach-O x64/ARM64. NeverD refuse NeverC avec `--language=c++` avant de lancer le compilateur, sans traduction ni changement implicite de compilateur. Le C++ et les exceptions utilisent Clang, ou un pilote GCC compatible avec le profil objet ELF vérifié. Les cadres C hosted peuvent transmettre une exception levée et interceptée par le programme C++ environnant grâce aux véritables données de déroulement ; cela ne donne pas à NeverC un langage C++.

`dyncode`: `--compile-mode=dyncode` utilise `-fdyncode-hosted` avec source, une entrée explicite, objet natif et aucune liaison originale. CLI et Python choisissent l’objet pour ce mode et GCC. Les empreintes, tous les octets liés et l’entrée sont comparés avant publication ; de vraies CFI/SEH ou données compactes complètes assurent le déroulage. État modifiable/TLS, conversions implicites syscall/PEB/heap/string, imports irrésolus et code réordonné sont refusés. Données et callbacks peuvent être des paramètres.

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

Clang LLVM IR admet aussi Windows ARM64 C++ FH3 avec propriété xdata, ABI du cadre parent et groupes funclet/FuncInfo. Les gestionnaires de langage d’objets natifs restent exclus. `check_source_patch_cpp_runtime.py` exécute neuf scénarios sur Windows natif x64/ARM64 ; preuves CPU et déroulage OS sont distinguées.

| Compilateur | Artefact | Profil d’exécution OS vérifié |
|---|---|---|
| Clang 22.1.6 | IR LLVM | ELF x64/ARM64 C/C++ ; Mach-O x64/ARM64 C/C++ ; PE x86/x64/ARM64 C; PE x64 C++ FH3 |
| Clang 22.1.6 | Objet natif | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C |
| GCC/G++ 13.3.0 | Objet natif | ELF x64/ARM64 C/C++ avec un runtime Itanium compatible |
| NeverC 3389.1.5 mode ordinaire | IR LLVM | ELF x64 C ; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 mode ordinaire | Objet natif | ELF x64 C |
| NeverC 3389.1.5 C | LLVM IR / objet natif | ELF / PE / Mach-O x64/ARM64: source C pure |
| NeverC 3389.1.5 `hosted-v1` | Objet natif + charge scellée | ELF / PE / Mach-O x64/ARM64: source C et propagation à travers les cadres C |

Le tableau décrit des profils OS vérifiés, sans garantie pour toutes les versions ou options. Les installateurs natifs ELF, PE et Mach-O acceptent les quatre architectures. GCC exige un pilote ELF compatible et ne produit pas de LLVM IR. Les relocations inconnues, TLS, constructeurs globaux au chargement, LTO, symboles common/IFUNC et imports non résolus échouent explicitement. Le reçu JSON conserve le compilateur, les arguments, les liaisons et les adresses ; les objets natifs ont aussi un SHA-256.

Le champ `target` du reçu d’un objet natif provient de l’ABI de l’image chargée, y compris les indicateurs de virgule flottante ARM ELF et la distinction iOS/watchOS. Les objets compilés et fournis suivent la même règle d’identité.

La matrice CPU exige une installation réussie du source, de l’IR fourni et des objets natifs dans les douze combinaisons. Elle couvre séparément ARMv7k watchOS et ARMv7 iOS, les données modifiables ou constantes, les arguments flottants et de pile des fonctions originales, et les entrées après changement d’adresse de chargement. Cette exécution CPU ne certifie ni le chargeur OS, ni le dérouleur C++, ni une ABI arbitraire ; voir les profils OS ci-dessus.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU` signifie installation source／IR／objet, rechargement et résultat exact. ARM ELF déduit EABI5, l’état ARM/Thumb et l’ABI flottante de l’image et enregistre un index `EHABI` complet et trié. PE conserve les lignes x64/ARM et les relocations `HIGHLOW`/`DIR64`/`MOV32T` ; x86 utilise le code d’enregistrement du compilateur. Mach-O distingue watchOS DWARF et iOS `SjLj` selon le sous-type CPU. Les branches d’entrée ARM/Thumb sont relatives et restent valides après rebasage.

Les objets natifs utilisent leurs symboles de fonction bornés, définitions auxiliaires／de section et tables de déroulement, sans inventer d’ID propriétaire MC ni recompiler l’IR. Les limites des fonctions COFF exigent des tailles auxiliaires exactes, des sections à fonction unique (`-ffunction-sections`) ou des entrées natives décodées de la table des fonctions ; le symbole suivant ne sert jamais à deviner une taille. L’adaptateur watchOS conserve les cadres des fonctions feuilles pour obtenir de vrais enregistrements. Les gestionnaires de langage COFF/Mach-O natifs ne font pas partie du profil objet vérifié ; utiliser l’IR vérifié pour les exceptions C++. Les dérouleurs ARM statiques avec registre exidx figé, pointeurs absolus Mach-O non enregistrés et stockage RELRO non pris en charge sont refusés.

Les objets C de GCC ont aussi été installés et exécutés sur ELF x86, ARM32 et ARM64 via Linux QEMU user mode. L’adaptateur sélectionne ARM/Thumb depuis l’image ; les propriétés GNU suivent l’alignement de la largeur des pointeurs ELF.

<!-- i18n-section: references -->
## Références originales et stockage

Les liaisons nomment des déclarations externes : `function` exige une fonction originale ou un stub d’import vérifié ; `data` exige du stockage inscriptible ; `const` exige une déclaration IR constante. Les données des objets demandent une étendue non nulle. Celle-ci ne peut traverser les limites du stockage ni rendre RELRO inscriptible. Le rappel du corps original remplacé est refusé : ce profil n’a pas de trampoline conservant le prologue.

Le nouveau code reste RX et l’état mutable RW. Seuls les imports existants sont réutilisés ; les slots GOT/IAT et les stubs appelables sont distincts. Mach-O exige de la place pour les commandes de chargement, par exemple `-Wl,-headerpad,0x400` ; PE exige de la place pour les en-têtes de sections. Un espace insuffisant échoue. ELF `.data.rel.ro` et les protections CET/IBT originales exigent des contrats supplémentaires et sont actuellement refusés.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

La croissance des tables Mach-O exige aussi des espaces fichier et virtuels adjacents libres ; la réserve d’en-tête seule ne les fournit pas. Les fixtures distinguent ces deux exigences.

<!-- i18n-section: exceptions -->
## Exceptions C++

ELF utilise les FDE/LSDA du compilateur ; Mach-O utilise compact unwind/DWARF. PE x64/ARM64 admet les tables FH3 avec un runtime MSVC existant compatible et des identités de types cohérentes. Les données type-info peuvent demander des liaisons explicites. FH4, les personalities personnalisées, la source SEH et les modes CFG/EHCont/XFG actifs sont refusés ; les protections originales sont préservées.

Les originaux ELF x86/x64 exigeant IBT sont actuellement refusés : l’installateur source ne fournit pas encore de trampoline conservant ENDBR.

Les cas exécutés couvrent le retour normal, le catch interne, une nouvelle exception capturée par l’appelant original, une exception traversant le remplacement depuis un appelé original, le nettoyage RAII exactement une fois, la relance, la terminaison noexcept et les fonctions inchangées. L’appelant doit déjà permettre la propagation. Le correctif ne recrée pas les catches supprimés autour d’une fonction prouvée sans exception.

<!-- i18n-section: publication -->
## SDK et publication

L’API C additive est `neverd_patch_source` avec `neverd_source_patch_options_v1` ; `neverd_patch_source_report` renvoie le reçu. L’entrée objet fournit sa longueur en octets. L’ancienne API `neverd_patch_from_c` reste disponible.

Les entrées sont figées. Le candidat est préparé près de la destination, rechargé et vérifié pour chaque entrée avant publication exclusive. Les fichiers et alias symboliques existants ne sont pas écrasés. Une liaison manquante fait échouer toute la demande. `--mode=inplace` est refusé. Les transformations IR de session ne s’appliquent pas aux objets natifs. Mach-O vérifie et renouvelle strictement la signature ; une signature PE n’est pas supprimée silencieusement.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## Compilation et vérification

Choisissez un compilateur compatible avec `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (Clang 22.1.6 vérifié). La compilation intégrée corrige une copie LLVM dans le dossier de compilation sans modifier le sous-module ni le cache partagé. Le défaut PLT x64 des précompilés r3/r4 peut provoquer un refus de correctif IR ; les objets natifs évitent ce chemin MC. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` exige une production PLT exacte et corrigée.

`NeverDSourcePatchTests` couvre C/API/objets/exceptions natifs ; `NeverDSourcePatchMatrixTests` vérifie toutes les combinaisons avec Unicorn. `SourcePatchPE` exécute sept scénarios C++ avec `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` et `NEVERD_TEST_WINDOWS_SDK_LIB`. Hors Windows, `NEVERD_TEST_WINDOWS_RUNNER` est aussi requis. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` transforme les entrées manquantes en échec. Le workflow Windows dédié impose l’exécution native et refuse les cas ignorés.

Les preuves Linux sur le Mac de développement utilisent Docker et un chargeur Linux explicite ; PE utilise Wine 11. Mach-O ARM64 s’exécute sur le Mac. Les résultats CI Windows natifs se vérifient séparément : configurer un job ne prouve pas sa réussite. Les comptes historiques, échecs de référence et limites des émulateurs figurent dans [les contrats détaillés et le journal de validation](../source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

La copie LLVM corrige aussi les références GOT de données Mach-O x64 : la table de catch typé de Clang se mesure depuis la fin du mot de relocation de quatre octets. r3/r4 la décale de quatre octets ; la vérification avant publication refuse ce résultat. Sur le Mac, x64 utilise Rosetta 2 et ARM64 s’exécute directement.

La qualification native Windows x64 [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) a réussi les 16 tests ciblés PE/objets/trampoline et les 143 tests de la matrice de cibles CPU, sans aucun test ignoré. Le cas FH3 a exécuté sept scénarios C++ ; la matrice utilise Unicorn. Cette exécution a utilisé Clang 22.1.6 avec MSVC et le SDK Windows natifs, ainsi que LLVM intégré.

Le workflow Windows prépare aussi 27 remplacements PE pour x86, x64 et ARM64. `scripts/check_source_patch_pe_runtime.py` vérifie les appels non feuilles, l’état mutable des pointeurs et les arguments flottants ou sur la pile via l’IR source, les objets compilés et les objets fournis. Chaque original et chaque résultat est exécuté trois fois, soit 30 observations de processus par architecture. x86/x64 utilisent Windows x64 ; ARM64 utilise `windows-11-arm`. La vérification exige l’hôte natif attendu, le même commit, des empreintes de fichiers identiques et un inventaire complet ; les exécuteurs externes sont consignés séparément. Ce profil ne démontre pas la prise en charge des exceptions C++ ARM64.

`NEVERD_TEST_MATRIX_NEVERC` sélectionne le compilateur facultatif de la matrice. NeverC 3389.1.5 a réussi les 66 vérifications x64/ARM64 pour ELF, PE et Mach-O, dont l’installation IR/objet natif et l’exécution CPU. Cette version ne prend pas en charge x86 ni ARM32 ; utilisez Clang, ou un pilote GCC adapté pour ELF. Pour compiler du PE avec NeverC, l’adaptateur ajoute `-fno-autolink` : les références sont résolues dans l’image existante sans lier un autre CRT. Les objets fournis contenant des contrats de liaison `.drectve` restent refusés.

La qualification des processus Windows natifs [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817) a réussi les 90 observations : 30 pour chacune des architectures x86, x64 et ARM64, avec IR source, objets compilés et objets fournis. x86/x64 ont été exécutés sur Windows x64 ; ARM64 sur `windows-11-arm`, avec les mêmes images vérifiées par empreinte. Cette exécution a également réussi les 16 tests ciblés PE/objets/trampoline et les 143 tests de la matrice Unicorn, sans aucun test ignoré. Le cas FH3 a conservé ses sept scénarios C++ x64.

La qualification native fixe NeverC à `182fd2c5dc`, avec des originaux indépendants et l’installateur NeverD actuel. `check_source_patch_hosted_pe_runtime.py` exige 36 observations par architecture Windows pour les résultats IR/objet/hosted C et deux cadres C traversés par une exception. `check_source_patch_elf_runtime.py` exige 120 observations par architecture Linux pour C, la propagation hosted et neuf scénarios d’exceptions C++ Clang. Reçus, empreintes et architecture réelle doivent correspondre ; cela ne signifie pas que NeverC lui-même tourne sur l’hôte CI. Sous Linux x64 et ARM64, les neuf scénarios sont exécutés pour chacune des voies IR Clang, objets natifs Clang et objets natifs GCC. NeverC reste limité au C.

Les 204 observations de l’ancien profil de traduction sont historiques et ne prouvent pas le contrat actuel limité au C. Les trois voies NeverC actuelles utilisent C ; Clang C++ FH3 est qualifié séparément. macOS x64 utilise Rosetta 2. Unicorn/QEMU ne remplacent pas la validation native du système et du dérouleur.