**Langues**: [English](../mobile.md) | [简体中文](../zh-CN/mobile.md) | [繁體中文](../zh-TW/mobile.md) | [日本語](../ja/mobile.md) | [한국어](../ko/mobile.md) | [Français](mobile.md) | [Deutsch](../de/mobile.md) | [Español](../es/mobile.md) | [Italiano](../it/mobile.md) | [Русский](../ru/mobile.md) | [العربية](../ar/mobile.md)

# Reconstruction des applications mobiles

[← Index de la documentation](README.md) · [Guide Android complet](android.md) · [Guide iOS complet](ios.md)

`neverd mobile` reconstruit du Java lisible depuis des entrées Android APK, DEX et smali. Pour les entrées iOS IPA, `.app` et Mach-O, il exporte du C natif et reconstruit les corps de méthodes Objective-C pris en charge sous forme de code source `.m`, accompagnés de code Swift expérimental et de métadonnées d’exécution. Ce parcours CLI est expérimental ; les conteneurs mobiles ne sont pas acceptés par le SDK C natif ni par le chargeur graphique.

## Configuration

Compilez la cible `neverd` avec la prise en charge de C++20. Le traitement mobile est intégré à la CLI native et n’utilise aucun interpréteur Python. Distribuez l’exécutable avec les bibliothèques natives nécessaires à votre compilation.

Le traitement ZIP natif utilise zlib pour CRC-32 et DEFLATE. CMake privilégie une bibliothèque installée via `find_package` ; à défaut, il télécharge zlib 1.3.2 avec un SHA256 fixé et la compile statiquement. Cette implémentation ZIP mobile ne nécessite aucun outil auxiliaire Python sous Windows. Conservez les notices des dépendances dans [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).

Le moteur par défaut est implémenté en C++20 et ne nécessite aucun environnement Python, Java ou JADX à l’exécution. Seul un `--jadx PATH` explicite sélectionne l’adaptateur de compatibilité installé séparément ; `NEVERD_JADX` et PATH ne le sélectionnent pas automatiquement, et aucun repli automatique n’est effectué. L’adaptateur facultatif nécessite JADX 1.5.6+ avec les plugins d’entrée DEX/smali standard et Java 11+. Son rapport indique le moteur réel `jadx` et sa version. L’installation et les licences des dépendances restent documentées dans le [guide Android](android.md#adaptateur-de-compatibilité-jadx-facultatif).

## Android

Pour obtenir rapidement un répertoire des classes sans reconstruction Java,
utilisez `neverd mobile app.apk --list-classes`, éventuellement avec
`--class-prefix com.example` ou `--json`. Le
[contrat d’inventaire](android.md#inventaire-rapide-des-classes) décrit l’ordre,
les limites et la validation des charges utiles sélectionnées. Le mode requête
ne nécessite aucun répertoire de sortie ; son `-o` facultatif désigne un nouveau
fichier.

Pour les références directes du bytecode, utilisez
`neverd mobile app.apk --find-refs string --query 'example' --json`.
Le [contrat des requêtes de références](android.md#requêtes-de-références-dans-le-code)
couvre aussi les opérandes de type, méthode et champ, les correspondances
littérales/exactes, les positions de chaque occurrence, la conservation UTF-16
et le périmètre de validation du code. Sans `--json`, cette opération produit
du JSON Lines. Elle partage le comportement de nouveau fichier pour `-o`.

Les exemples et sorties suivants décrivent la reconstruction.

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Tous les `classes.dex`, `classes2.dex` et fichiers DEX numérotés suivants à la racine d’un APK sont analysés ensemble. Un répertoire smali est parcouru récursivement et toutes ses classes sont analysées en une seule invocation, y compris les classes imbriquées et voisines. Utilisez un répertoire pour reconstruire des classes qui se référencent entre elles. Un fichier smali seul ne fournit que cette classe.

La sortie de reconstruction intégrée contient `sources/`, `metadata/android-methods.json` et `report.json`, avec `backend: {"name": "neverd", "version": "1", "execution": "builtin"}`. Le rapport intègre `android_method_recovery` : `method_count = recovered_method_count + declaration_only_method_count`, et `unrecovered_method_count` doit être nul avant publication. Les déclarations originales `native`/`abstract` sont comptées séparément des corps reconstruits. L’adaptateur externe explicite conserve ses propres journaux de backend. Les ressources APK, manifestes, bibliothèques natives et le code chargé dynamiquement sont hors de ce parcours Java ; les bibliothèques natives peuvent être analysées séparément avec `neverd decompile`.

Les lecteurs de reconstruction intégrés partagent un modèle Dalvik typé développé indépendamment et un générateur Java au travail borné pour le code ordinaire représentable DEX 035/037–040 et smali. DEX 041, les appels dynamiques tels que `invoke-custom`, certains chemins d’initialisation, les opérations inconnues et les identifiants impossibles à représenter en Java échouent explicitement. Le Java généré peut utiliser une boucle de répartition ; il n’exécute pas le DEX original et ne l’appelle pas via une passerelle d’exécution. Les commentaires, la mise en forme et les noms supprimés de l’original ne peuvent pas être restaurés. Le moteur expérimental ne promet ni la parité fonctionnelle avec JADX, ni l’équivalence sémantique, ni la reconstruction complète de tout APK.

## iOS

Le [guide iOS complet](ios.md) documente la sélection IPA, `.app` et Mach-O, la configuration, toutes les options CLI, les schémas de code source, la couverture et la vérification.

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

Chaque exécution sélectionne un exécutable. `--artifact` est relatif au bundle de l’application pour IPA comme pour `.app`. La sélection dans un binaire universel privilégie arm64, arm, x86_64, puis i386 ; la projection vers le langage source cible arm64/x86_64. Les tranches sélectionnées chiffrées sont refusées. Le parcours natif expérimental produit du C et les corps de méthodes Objective-C pris en charge, y compris les liaisons ABI pour scalaires/pointeurs, virgule flottante, paramètres mixtes et positions sur la pile. Les dispositions des classes/variables d’instance à l’exécution et les catégories distinctes sont conservées lorsqu’elles sont validées ; les dispositions, signatures, appels et autres dépendances non résolus restent des omissions explicites.

La reconstruction Swift classe les signatures dans le processus C++ avec `LLVMSwiftDemangle` du fork LLVM de NeverD. Elle ne lance aucun démangleur externe ni commande de détection de chaîne d’outils et ne nécessite aucun compilateur Swift installé pour compiler ou exécuter NeverD. Les compilations du fork depuis les sources et les paquets LLVM correspondants incluent ce composant ; NeverD ne récupère aucune dépendance source Swift séparée. L’inventaire des signatures enregistre `demangler: {"name": "llvm-swift-demangle", "execution": "builtin", "version": "6.3.3"}`. Les signatures prises en charge sont liées aux points d’entrée natifs et aux emplacements ABI avant l’émission de véritables fonctions `.swift`, méthodes/initialiseurs de classes et méthodes de structures à disposition fixe. Les formes génériques/résilientes, asynchrones/à exceptions, les formes appelables générées à l’exécution non prises en charge et les groupes incomplets de dépendances source restent non reconstruits.

La sortie normale comprend `sources/native.c`, éventuellement `sources/objc.m` et `sources/swift.swift`, les déclarations et métadonnées d’exécution, le JSON de couverture des méthodes/signatures, les journaux, `artifacts/selected.macho` et `report.json`. Il n’y a aucun journal de détection externe de chaîne Swift ni de démanglage externe. Le code généré n’appelle pas le binaire original comme passerelle de reconstruction. Les `source_units` Swift regroupent les déclarations de types et les méthodes ; les lignes de méthodes indépendantes ne doivent pas être concaténées pour reconstruire des classes. Le `status: "success"` externe signifie la publication de la sortie, pas une couverture complète des méthodes ni une équivalence sémantique.

`--metadata-only` n’exécute ni l’exportateur de code natif ni le démanglage des signatures, et ne produit aucun code source ni couverture des méthodes. Tous les modes utilisent les métadonnées Objective-C résolues par le chargeur natif. Les métadonnées Swift utilisent des lectures bornées de l’image native ; les correctifs, dispositions relogeables ou références non pris en charge conservent des diagnostics partiels. `--max-func` limite la reconstruction des fonctions natives et est ignoré en mode métadonnées seules. L’absence de corps de fonctions natives fait échouer une exécution normale. Les entrées décompressées temporaires sont supprimées.

Les commentaires originaux, la mise en forme, les identifiants supprimés et les structures source perdues à la compilation ne peuvent pas être reconstruits exactement. Avant d’utiliser la sortie, consultez le statut et le motif de reconstruction de chaque méthode, les compteurs Swift distincts d’éléments appelables/non appelables/non classés et les limites documentées.

## Limites et échecs

Pour la reconstruction, `-o` doit désigner un nouveau répertoire extérieur à toute entrée de type répertoire. Les modes requête acceptent à la place un nouveau fichier de sortie facultatif. Une sortie existante n’est jamais écrasée. Le travail de reconstruction est préparé temporairement et publié uniquement après la réussite de la reconstruction et de la validation des sorties. Les résultats des requêtes sont conservés en mémoire jusqu’à la réussite de tous les DEX sélectionnés. Les exécutions réussies de la CLI native renvoient zéro. Les échecs de reconstruction renvoient une valeur non nulle ; `--json` signale les échecs traités avec `schema_version`, `status: "error"` et `error`. Les erreurs d’analyse des arguments, de démarrage de l’exécutable natif ou des bibliothèques, ainsi que les interruptions, peuvent à la place être signalées sur stderr. Les consommateurs doivent d’abord vérifier le code de sortie.

Les valeurs par défaut sont 20 000 entrées, 2 GiB de données d’entrée/extraites ou de sortie finale et 300 secondes pour l’analyse Android/iOS intégrée ou chaque processus JADX explicite. Les processus enfants iOS reçoivent le budget total d’analyse restant. Le lecteur et le générateur intégrés imposent aussi un budget de travail borné. Définissez `--max-files`, `--max-bytes` et `--timeout` pour ajuster ces limites positives. L’espace de travail temporaire est surveillé pendant l’exécution des backends, avec jusqu’à trois fois les limites d’entrées/octets pour permettre la coexistence des entrées préparées et des sorties intermédiaires. Les diagnostics sont plafonnés à 16 MiB par processus.

Pendant la reconstruction, la préparation APK n’écrit que les `classes.dex`, `classes2.dex` et fichiers DEX numérotés suivants à la racine. Tous les membres ZIP subissent néanmoins les contrôles d’en-têtes/plages, la décompression et la validation de longueur et CRC, et comptent dans les limites d’entrées d’archive et d’octets non compressés. Les ressources non écrites peuvent porter des noms distincts sensibles à la casse tels que `res/-A.xml` et `res/-a.xml`. Les noms ZIP exactement dupliqués et les conflits d’identité fichier/répertoire restent des erreurs ; les vérifications portables de collisions de casse du système de fichiers s’appliquent aux membres réellement écrits. L’extraction complète, y compris pour les entrées IPA, refuse toujours ces collisions de sortie. Les chemins de traversée, liens, fichiers spéciaux et entrées ZIP chiffrées sont refusés dans toute l’archive. Les entrées de type répertoire refusent aussi les liens symboliques et fichiers spéciaux.

Ces limites sont des contrôles de robustesse, pas un bac à sable pour le code des backends tiers. Les commandes explicites JADX et d’export de code natif s’exécutent comme processus enfants locaux. Les sorties temporaires ayant échoué sont supprimées. Les sorties de backend non nulles incluent une fin de journal de diagnostic bornée. Les dépassements de temps des backends conservent le message de délai dépassé et ajoutent une fin de journal bornée lorsque du texte a été capturé. Les échecs de lancement et violations de budget conservent leurs propres messages d’erreur.

## Vérification

Python n’est utilisé que par les bancs de test de développement ci-dessous ; la reconstruction mobile intégrée s’exécute dans la CLI native C++20.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

Les tests des composants couvrent l’analyse, les conteneurs dangereux, les échecs des backends, le nettoyage des sorties, la sélection d’architecture et la préservation des sorties. Les tests qui invoquent la CLI compilée utilisent `NEVERD_BUILD_DIR`. Le test interne Android emploie un JDK (`java` et `javac`) et D8 pour construire des jeux d’essai DEX/APK indépendants, puis compiler et exécuter le Java reconstruit. Ce sont des dépendances de vérification, pas des prérequis de reconstruction intégrée. Exécutez les tests sur la compilation actuelle et examinez le résultat avant de considérer un cas comme vérifié. Le test de compatibilité séparé nécessite aussi JADX ; la réussite de jeux d’essai ne prouve pas la reconstruction de toute application.

Sous macOS, avec Apple Clang, son SDK et NeverD compilé, exécutez la comparaison réelle d’exécution Objective-C :

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

Le test construit son propre jeu d’essai Objective-C, reconstruit ses implémentations de méthodes et ne lie que le `.m` reconstruit avec le même banc d’appels indépendant. Son jeu de 22 méthodes compare 141 résultats observables couvrant les bornes d’entiers, branchements, boucles, lectures/écritures par pointeur, arguments cachés et inutilisés, bits d’identité float/double, paramètres mixtes et positions sur la pile. Une exécution actuelle réussie est nécessaire avant de considérer un cas comme vérifié. Toutes les variantes d’architecture et de correctifs demandées doivent aboutir ; la configuration par défaut couvre arm64/x86_64 × classic/default. Une variante manquante ou une architecture que l’hôte ne peut pas exécuter constitue un échec, sans possibilité d’ignorer le cas. Ces preuves par jeux d’essai n’établissent pas la complétude pour tout programme iOS. `NeverDMobileIOSBackend` est enregistré dans CTest sous macOS, y compris dans le profil principal de tests CI.

Le test indépendant de reconstruction Swift est `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`. Il recompile le Swift généré et son banc de test sans lier le binaire original ; les éléments appelables non pris en charge et les différences de comportement sont des échecs. Consultez le [guide iOS](ios.md) pour la sémantique de couverture et les preuves d’échec conservées.

Le [workflow Mobile Real Applications](../../.github/workflows/mobile-real-apps.yml) utilise des applications publiques fixées dans le [manifeste du corpus](../../scripts/mobile_real_apps.json). Son contrôle d’acceptation exige des inventaires indépendants de chaque DEX de chaque APK et de chaque Mach-O de chaque bundle iOS complet, la reconstruction indépendante des originaux et du code généré, ainsi que des comparaisons de comportement. Des étapes manquantes, une couverture d’inventaire inconnue ou des cas obligatoires absents font échouer ce contrôle. Les étapes de recompilation et de comportement pour les applications réelles restent incomplètes ; la prise en charge conserve donc le label Experimental. La réussite des tests de garde du banc n’établit pas une réussite sur des applications réelles.

Les cas Android tentent de compiler l’ensemble inventorié du Java généré avec javac et D8, en utilisant uniquement les déclarations du SDK Android. Le bytecode original de l’application, les implémentations de dépendances et les stubs de remplacement ne peuvent pas combler les lacunes de reconstruction. La reconstruction partielle et les erreurs de compilation restent dans les preuves ; une compilation réussie exige toujours une vérification indépendante, une reconstruction complète de l’APK et une comparaison de comportement ART. L’oracle iOS confronte les enregistrements de méthodes Objective-C sur disque aux sorties des outils Apple, en conservant les identités de classe, métaclasse, catégorie, liste et ordinal. Les pointeurs non résolus ou emplacements omis laissent l’inventaire inconnu. Des inventaires distincts de déclarations SDK pour appareils et simulateurs soutiennent l’importation de frameworks sans traiter les en-têtes comme une preuve de disposition d’instance ou de comportement.
