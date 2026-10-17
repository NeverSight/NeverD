**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 1eae101089e730c29ba3fda92db515fe148fe41b758d6f5b69156252591d9b3f -->

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
| Clang 22.1.x | IR LLVM | ELF x64/ARM64 C/C++ ; Mach-O x64/ARM64 C/C++ ; PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | Objet natif | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | Objet natif | ELF x64/ARM64 C/C++ avec un runtime Itanium compatible; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
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
## Rappels de l'original, extension de la disposition et dépendances

Les liaisons désignent des déclarations externes : `function` authentifie une fonction originale ou un stub d'import, `data` exige une zone inscriptible et `const` une déclaration IR constante. Une donnée objet exige une étendue non nulle, sans franchir les limites de stockage ni autoriser l'écriture en RELRO. Utilisez explicitement `original` pour appeler le corps sauvegardé de la fonction remplacée. L'entrée publique appelle toujours le remplacement ; l'entrée sauvegardée exécute un préfixe relocalisé en instructions entières puis poursuit dans l'original. Sources, IR fournis et objets natifs partagent ce contrat, y compris les arguments flottants, sur pile et les rappels multiples.

Un rappel exige des limites de fonction, un mode d'instruction, une continuation et des preuves de déroulement authentifiés. Instructions inconnues, références PC-relatives dangereuses, branches vers le préfixe copié, relocalisations de chargement non prises en charge, rappels superposés et prologues avec gestionnaires non modélisés sont refusés avant publication. La déclaration doit toujours respecter l'ABI originale. Le reçu consigne les entrées sauvegardées dans `original_callbacks`.

PE32/PE32+ peut agrandir une table d'en-têtes de sections pleine en déplaçant les plages du fichier et en actualisant les pointeurs modélisés de débogage, symboles, certificats et overlay, tout en conservant les RVA. Pour Mach-O x64/ARM64 PIE, une extension vérifiée du préfixe d'en-tête fournit le stockage des commandes et du déroulement si la garde page-zero adjacente et le segment linkedit final le permettent ; les adresses du code et des données d'origine restent fixes. Ce profil n'exige ni `-Wl,-headerpad,0x400` ni pages de déroulement libres adjacentes. Références explicites à l'en-tête, commandes inconnues, propriété mêlant code et données, plages malformées et dispositions non prises en charge sont refusées. Les capacités d'en-tête et de déroulement sont planifiées ensemble avant installation.

Le nouveau code reste RX et l'état mutable RW. Une demande explicite `--dependency` ajoute fonctions ou données via les métadonnées dynamiques ELF, les descripteurs d'import/IAT PE ou les métadonnées dylib/bind Mach-O. Identités, ordinaux et versions des imports existants sont conservés ; cases et stubs appelables gardent leurs rôles distincts. Choisissez le nom/chemin attendu par le chargeur du système cible. L'exemple utilise un nom de bibliothèque partagée ELF. Une bibliothèque ou un symbole absent doit produire une véritable erreur du chargeur à l'exécution. TLS, IFUNC, nouveaux contrats de versions, constructeurs au chargement et relocalisations non modélisées restent exclus ; ELF `.data.rel.ro` et CET/IBT exigent encore d'autres contrats. Pour les objets COFF natifs, déclarez les nouveaux imports de fonctions/données avec `__declspec(dllimport)` afin que les relocalisations identifient l'IAT. Les références externes directes ordinaires sont refusées.

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

Les options de dépendances, ajoutées en fin de structure et vérifiées selon sa taille, concernent sources, IR fournis et objets natifs. Le reçu inclut `added_imports` ; après rechargement du candidat, rappels, imports, permissions et octets installés sont revérifiés. Un échec préserve l'entrée et toute destination existante.

<!-- i18n-section: examples -->
## Exemples exécutables

[Exemples complets Windows/macOS/Linux](../../examples/source-patch/README.md) avec les sources du programme, du remplacement et de la bibliothèque, les commandes de compilation et un vrai plugin Python SDK hébergé. Exécutez depuis la racine du dépôt avec un build NeverD prenant en charge Python et un nouveau répertoire de sortie.

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

Les deux voies sont exécutées par défaut ; ajoutez `--api cli` ou `--api sdk` pour en choisir une. L’adresse cible est détectée automatiquement. Pour `7`, le programme original affiche `8` et les deux versions patchées affichent `36`. Le pilote vérifie les exécutions, les reçus et le refus réel du chargeur sans bibliothèque ; les commandes complètes sont conservées dans `commands.json`.

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
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## Compilation et vérification

Les six jobs natifs OS/ISA sélectionnés ont réussi sans échec ni saut : 3669 exécutions GoogleTest, 1008 observations de processus natifs (504 fonctionnelles et 504 d’exceptions C++), 12 refus réels du chargeur, 96 images et 84 reçus de publication.

| Hôte | Source | Harness | Job CI | Tests | Observations / refus |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

Ces artefacts qualifient exactement les commits source et harness ci-dessous. Les commits ultérieurs d’intégration dev et de documentation ont leurs propres contrôles locaux et documentaires. Les anciens échecs ou annulations restent des échecs. Les 216 observations supplémentaires historiques, PE ARM32 et les résultats CPU/compatibilité Apple 32 bits anciens ne prouvent aucune nouvelle couverture matérielle native ou d’exceptions C++.

La qualification Windows ARM64 utilise une DLL C Unicorn ARM64 native, réellement construite à partir du sous-module fixé avec LLVM-MinGW vérifié, une bibliothèque d'import MSVC et une cible CMake explicitement substituée. La copie de compilation isolée authentifie trois corrections d'hôte : paire de sauts sans SEH, callback ARM64 de validation des pages avec synchronisation du cache d'instructions et masques immédiats 64 bits pour Windows LLP64. Les sources et la révision du sous-module restent inchangées. Cela qualifie l'hôte de la matrice CPU, sans établir la prise en charge du JIT MSVC ARM64 par défaut. Les cinq contrôles de reprise de l'hôte restent distincts des preuves des processus patchés natifs et des exceptions C++.

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

Les 204 observations de l’ancien profil de traduction sont historiques et ne prouvent pas le contrat actuel limité au C. Les trois voies NeverC actuelles utilisent C ; Clang C++ FH3 est qualifié séparément. Sur le Mac de développement Apple Silicon, macOS x64 utilise Rosetta 2. Unicorn/QEMU ne remplacent pas la validation native du système et du dérouleur.

Les relocalisations d’instructions AArch64 exigent des addends RELA explicites. Les addends REL implicites d’instructions sont refusés avant publication ; leurs champs de bits ne sont pas interprétés comme des données ordinaires. Les relocalisations de données PREL64 prises en charge conservent leurs addends signés complets en REL comme en RELA.

La qualification du producteur C [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213) a réussi les 312 observations de processus natifs : 36 sur chacune des architectures Windows x64 et ARM64, et 120 sur chacune des architectures Linux x64 et ARM64. Linux vérifie séparément neuf scénarios d’exceptions pour Clang IR, les objets natifs Clang et les objets natifs GCC ; les entrées NeverC IR, objet et hosted DynCode sont toutes en C.

La qualification de l’installateur reconstruit [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600) a réussi 19 vérifications PE/objet, les 143 cas de la matrice CPU et 474 observations de processus natifs Windows/Linux. Les processus Windows x86 ont tourné sur Windows x64 ; x64 et ARM64 ont utilisé des hôtes correspondants. La [qualification Linux](https://github.com/NeverSight/NeverD/actions/runs/38585759996) séparée a réussi les 108 cas initiaux, plus trois vérifications LLVM-C et trois Clang/GCC C++, sans échec ni saut.

La [qualification d’intégration 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097) a revérifié les 312 observations de processus sur Windows/Linux natifs x64 et ARM64. Les 30 images finales correspondent aux octets déjà qualifiés ; les empreintes source et les justificatifs de publication C uniquement ont été actualisés et vérifiés indépendamment.

La [qualification Linux étendue à 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38626866257) a réussi 429 contrôles exécutés, sans échec ni omission : les 108 cas initiaux, 55 contrôles Swift complets, 143 de cibles CPU, 117 de sessions natives, trois LLVM-C et trois C++ Clang/GCC. La suite locale complète de correctifs binaires a également réussi ses 22 961 cas pour cette intégration. Les processus natifs et la couverture par émulation CPU restent comptabilisés séparément.

La [qualification de l’installateur Windows reconstruit à 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627581732) a réussi les 182 contrôles ciblés et les 474 exécutions de processus natifs, sans échec ni test ignoré. Un audit indépendant des artefacts a vérifié chaque empreinte d’image, architecture, scénario et répétition. Après l’intégration ABI/déroulement de pile à `e1cac949f`, la suite complète de patch binaire a de nouveau réussi ses 22,961 cas, sans échec ni test ignoré. NeverC reste limité au C pur ; les remplacements C++ utilisent Clang/GCC.

<!-- i18n-section: runtime-coverage -->
## Qualification des processus macOS et 32 bits

Le workflow des images exige une exécution native sur macOS ARM64 (`macos-15`) et Intel x64 (`macos-15-intel`). Chaque architecture exécute 96 observations couvrant les sources et objets C Clang, les pointeurs vers un état modifiable, les arguments flottants/sur pile, neuf scénarios d’exceptions C++ Clang et les sorties IR/objet/hosted du NeverC C pur existant. Les signatures Mach-O sont vérifiées strictement avant exécution ; le code hosted installé doit être identique, octet par octet, à la charge du compilateur.

Le même workflow exige 30 observations Windows x86 sur Windows x64, 48 observations Linux x86 sur Linux x64 et 96 observations Linux ARM32 via QEMU en mode utilisateur, réparties également entre ARM et Thumb. Linux inclut les objets GCC adaptés à la cible. Chaque scénario est répété trois fois. Les rapports distinguent `native`, `compatibility`, `rosetta` et `qemu-user`, et conservent les architectures cible et hôte réelle. Rosetta ne satisfait pas une exigence Intel native.

Les 14 profils CPU restent couverts par la matrice de 283 tests source/IR/objet. PE ARM32, Mach-O i386, ARMv7 iOS et ARMv7k watchOS ont actuellement une qualification CPU, sans garantie du chargeur OS natif ni du déroulement C++. NeverC reste limité au C pur sur x64/ARM64 ; Clang et les pilotes ELF GCC correspondants produisent le code 32 bits. Le vérificateur ne construit ni ne modifie NeverC.

Windows x64 et ARM64 exécutent aussi chacun 36 observations Clang C++ FH3.

Les pointeurs modifiables Mach-O sont initialisés à l’exécution ; les initialisations d’adresse statiques nécessitent toujours un enregistrement des relocalisations au chargement non pris en charge et font l’objet d’un test de refus. Les fonctions COMDAT ELF de taille nulle ne sont admises que si la signature est la seule fonction au début de sa section exécutable et si la CFI correspond exactement à toute la section. Les cas avec décalage, partage, remplissage, groupe absent ou CFI absente restent refusés.

ELF ARM et Thumb ont des profils distincts. Les producteurs reçoivent explicitement `-marm` ou `-mthumb`, et les liaisons natives conservent le bit d’état d’instruction original. Les processus ELF 32 bits utilisent des PIE avec de vraies relocalisations dynamiques. ARM32 QEMU enregistre un `--loader` glibc explicite et un sysroot : son calcul AUXV ne suit pas les en-têtes déplacés comme le noyau Linux. Cette route vérifie le chargement glibc et l’exécution sous QEMU, sans constituer une preuve matérielle ARM32 ou du chargeur noyau natif.

Pour ces images ELF corrigées, les voies `qemu-user` de Linux x86 et ARM32 nécessitent toutes deux un `--loader` glibc explicite issu du sysroot correspondant à l’architecture cible. Linux x86 en mode `compatibility` utilise la prise en charge 32 bits du noyau de l’hôte Linux x64 et est consigné séparément de l’exécution QEMU ; cela ne prouve pas une exécution sur un hôte x86 natif 32 bits.

La [qualification des processus](https://github.com/NeverSight/NeverD/actions/runs/38694434082) de `fcee81c9a5e2f20a71090ade23ae833175f64c59` a réussi les 750 observations : 576 sur des hôtes natifs de même architecture, 78 en mode de compatibilité x86 et 96 ARM/Thumb avec QEMU en mode utilisateur. Un audit indépendant a vérifié les empreintes des 126 images, les environnements d’exécution et tous les scénarios. La [reconstruction Windows](https://github.com/NeverSight/NeverD/actions/runs/38694628242) a réussi 194 contrôles ciblés et 474 observations de processus Windows/Linux. La [reconstruction Linux](https://github.com/NeverSight/NeverD/actions/runs/38694638358) a réussi 444 contrôles, dont les 62 échecs et 46 cas ignorés d’origine, sans échec ni omission. Les limites matérielles et de chargeur ci-dessus restent applicables. NeverC reste réservé au C pur et son dépôt n’a pas été modifié.

La [validation ARM/Thumb complémentaire](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263) a réussi les 96 observations (48 ARM, 48 Thumb) des 32 images de test C issues de `fcee81c9a5e2f20a71090ade23ae833175f64c59` sur `ubuntu-24.04-arm`. Le noyau Linux a chargé directement chaque image via `/lib/ld-linux-armhf.so.3`, sans QEMU ni commande explicite de chargeur. Il s’agit du mode `compatibility` sur un hôte ARM64, et non d’un hôte 32 bits natif. Le commit du banc de test `3ad17ac9608d6bd9f5a4d44953ec375524e5730b` et celui des images sont enregistrés séparément. Des audits indépendants des artefacts ont vérifié les empreintes des images, les 30 reçus de publication, l’intégralité des scénarios et répétitions, ainsi que l’absence de règles d’interpréteur `binfmt_misc` correspondantes.

Des [sondages distincts des runners hébergés](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719) ont montré que le runner Windows 11 ARM64 fourni refuse les programmes ARM32 avec `WinError 216`. Sous macOS 15.7.9 avec Xcode 16.4, les SDK fournis n’ont pas permis l’édition de liens des programmes macOS i386 ou iOS ARMv7. Un programme watchOS ARMv7k a été lié avec succès, mais aucune exécution sur appareil physique n’a été établie. La réussite des sondages ne valide pas ces plateformes ; leur couverture matérielle native reste ouverte. NeverC n’a été ni compilé ni modifié.

Les [vérifications Linux supplémentaires dans Actions personnel](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274) ont réussi les 96 exécutions ARM/Thumb dans un système invité ARM32 complet sous QEMU TCG, avec noyau Linux 32 bits et glibc, ainsi que 30 exécutions PE ARM32 avec Wine dans ce même invité. Le noyau invité charge directement les images ELF ; cette émulation système est distincte du parcours `qemu-user` avec chargeur explicite. L'installation du registre Wine dans la couche de compatibilité de l'hôte ARM64 est consignée séparément et ne compte pour aucune exécution de correctif. L'initialisation de Wine dans l'invité et chaque exécution mesurée doivent réussir dans les délais d'origine.

Les [vérifications de compatibilité Apple](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358) ont réussi 30 exécutions Mach-O i386 avec Darling sous Linux et 60 exécutions ARMv7/ARMv7k avec touchHLE sous macOS Intel. Chaque original et chaque remplacement issu d'IR source, d'un objet compilé ou d'un objet fourni sont exécutés trois fois. Les audits indépendants ont vérifié les 72 images, 66 reçus de publication et 216 exécutions de ces essais supplémentaires, dont les tailles des artefacts GitHub, les SHA256, les commits exacts et les inventaires complets. Les entrées Linux restent fixées à `fcee81c9a5` ; les entrées PE ont été produites à `a542f97ba`, celles de Mach-O à `8f5f06762`. Les commits du dispositif de test sont consignés séparément dans les exécutions liées.

Les corrections Mach-O authentifient les fonctions Thumb sélectionnées indépendamment du mode ARM/Thumb mixte de l'image, normalisent les pointeurs d'export Thumb attestés et les adresses de code de déroulement ARM32 sans modifier les pointeurs de données ni les longueurs des plages. La compilation intégrant les sources LLVM a réussi localement les 283 contrôles de la matrice CPU et les 258 contrôles de contrats d'exception, sans échec ni omission. Les sondes Mach-O anciennes utilisent un éditeur de liens libre documenté et réservent de l'espace pour le déroulement ; i386 possède une entrée de démarrage correctement alignée et un segment page zéro de 64 KiB.

Ces résultats attestent l'exécution de C dans les émulateurs et couches de compatibilité indiqués. Wine n'est pas Microsoft Windows, Darling n'est pas macOS et l'exécution ARMv7k dans l'émulation de haut niveau iOS ne valide ni watchOS, ni WatchKit, ni un appareil Apple. Aucune couverture de SDK Apple ancien ou de matériel natif 32 bits n'en est déduite ; ces sondes ne valident pas le déroulement des exceptions C++. Les preuves natives et de compatibilité antérieures restent distinctes. NeverC n'a été ni compilé ni modifié.
