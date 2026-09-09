**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# Récupération du code natif et des sources iOS

[← Index de la documentation](README.md) · [Vue mobile](../mobile.md)

`neverd mobile` accepte IPA, `.app` et Mach-O. Il exporte du C natif, les métadonnées du runtime et, à titre expérimental, des sources Objective-C `.m` et Swift `.swift` pour les corps natifs pris en charge. Un résultat publié peut contenir des méthodes non récupérées : examinez la couverture avant utilisation. Les conteneurs mobiles relèvent de la CLI ; le SDK C natif charge séparément le Mach-O sélectionné.

La compilation élimine commentaires, mise en forme, identifiants et constructions du langage source. Ce processus reconstruit une représentation source, sans retrouver le texte original ni certifier une équivalence de comportement pour une application quelconque. Il ne lance pas l’application analysée.

## Démarrage et dépendances

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Compilez la cible `neverd` avec une chaîne compatible C++20. Le traitement mobile fonctionne dans la CLI native sans interpréteur Python. Le démanglage des signatures Swift est fourni par `LLVMSwiftDemangle` dans le fork LLVM de NeverD. Les compilations depuis les sources et les paquets LLVM publiés correspondants incluent ce composant ; NeverD ne récupère pas séparément les sources de Swift. Compiler et exécuter NeverD ne nécessite aucune installation du compilateur ou de la chaîne Swift. Les dépendances natives comme LLVM et Capstone restent nécessaires : distribuez les bibliothèques et mentions de licence requises par votre compilation. La compilation indépendante des sources Apple générées et les tests de comportement Swift sur macOS nécessitent, selon le cas, Apple Clang, le SDK et `swiftc`.

La récupération des signatures Swift exploite directement les nœuds structurés de `LLVMSwiftDemangle` dans le processus C++. Elle ne recherche ni ne lance d’exécutable externe de démanglage ou de commande de découverte de chaîne. L’ancienne option de chemin exécutable est supprimée et l’ancienne variable d’environnement du démangleur n’est plus lue. `--metadata-only` n’exécute ni l’exporteur natif de sources ni le démanglage des signatures.

L’inventaire de signatures dans `metadata/swift-signatures.json` identifie le composant intégré ainsi :

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
```

## Entrées et sélection

Une IPA doit contenir exactement un `Payload/*.app` au premier niveau. Pour IPA et `.app`, `CFBundleExecutable` dans `Info.plist` désigne le programme principal. `--artifact` est relatif à ce bundle dans les deux formats et sélectionne un seul exécutable intégré, sans analyser récursivement tous les frameworks ou extensions. Une entrée Mach-O brute n’accepte pas `--artifact`.

Pour les binaires fat, `--arch=auto` privilégie arm64, arm, x86_64, puis i386. Une tranche absente ou non prise en charge échoue explicitement. La projection source vise actuellement arm64/x86_64 ; choisir une autre famille ne garantit pas des sources Objective-C/Swift. Une tranche sélectionnée avec `cryptid != 0` est refusée : fournissez une entrée déjà déchiffrée et lisible. Archives et répertoires refusent chemins dangereux, liens symboliques, fichiers spéciaux et entrées en conflit.

## Options et limites de ressources

| Option | Défaut | Signification |
|--------|--------|---------------|
| `-o DIRECTORY` | Obligatoire | Nouveau répertoire hors du répertoire d’entrée ; la sortie existante est conservée |
| `--platform=auto\|ios` | `auto` | Détecter la plateforme ou choisir iOS |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | Choisir une tranche Mach-O |
| `--artifact PATH` | Programme principal | Exécutable relatif au bundle |
| `--metadata-only` | Désactivé | Métadonnées seules, sans récupération source ni appel d’outil |
| `--max-func N` | `0` | Limite de fonctions natives ; zéro signifie toutes les fonctions découvertes ; ignorée en mode métadonnées |
| `--timeout N` | `300` | Budget total positif d’analyse en secondes ; les processus enfants utilisent le temps restant |
| `--max-files N` | `20000` | Budget positif d’entrées ; l’inventaire Swift est également borné |
| `--max-bytes N` | `2147483648` | Budget positif d’octets pour entrée, extraction et sortie finale |
| `--json` | Désactivé | Rapport versionné au format JSON |

Le répertoire de travail est surveillé et peut utiliser jusqu’à trois fois les budgets d’entrées/octets pour les copies et résultats intermédiaires. Les journaux sont limités à 16 MiB par processus, et le JSON des signatures Swift à 32 MiB. Ces contrôles de ressources n’isolent pas les processus. Augmenter le délai ne supprime aucune autre limite. Les méthodes exclues par `--max-func` restent non récupérées lorsqu’elles figurent dans les métadonnées.

## Sources Objective-C et structure du runtime

Le chargeur natif associe enregistrement de méthode, adresse IMP exécutable et encodage de type pris en charge à des emplacements ABI source explicites. Les arguments fixes scalaires/pointeurs conservent `self`/`_cmd`, paramètres inutilisés, banques séparées d’entiers/flottants et positions de pile prises en charge. Réinterprétation des bits float/double et conversion numérique restent distinctes. Les indications de type servent à la projection source ; elles ne constituent ni preuve ABI authentifiée ni autorisation de modifier le code exécutable.

`sources/objc.m` place les instructions réellement reconstruites dans des corps `@implementation` et conserve auxiliaires C et appels typés nécessaires. Les cibles d’appel exigent une liaison source prise en charge ; cibles inconnues et groupes de dépendances incomplets restent non récupérés. Définitions absentes, adresses non exécutables, encodages contradictoires, ABI non gérées, décodage incomplet ou IR refusé ne deviennent pas des méthodes récupérées du seul fait d’une déclaration.

Les métadonnées de classe conservent superclasse, début/taille d’instance et ivars scalaires/pointeurs dont offsets, largeurs et alignements sont vérifiés. Les déclarations ajoutent du remplissage au besoin. Une méthode exigeant une disposition inconnue reste non récupérée. Les catégories gardent leurs identités classe/catégorie/adresse et implémentations distinctes ; les enregistrements strictement identiques répétés dans les deux inventaires ne comptent qu’une fois. Les catégories externes utilisent une déclaration Foundation existante lorsqu’elle est prise en charge. Les en-têtes externes inconnus sont signalés comme dépendances absentes, sans inventer de classe de remplacement.

Les appels de Blocks Objective-C pris en charge exigent une ABI scalaire fixe complète, avec l’objet Block implicite et tous les emplacements des arguments et du résultat. L’encodage d’exécution `@?` est élargi en `id` uniquement dans les déclarations ; il ne fournit pas le prototype d’appel. Les références aux Blocks globaux conservent l’identité de l’objet partagé. Les captures scalaires synchrones prises en charge nécessitent une preuve du stockage natif des captures et du flux d’appel. Les captures qui s’échappent ou sont asynchrones, les règles de propriété objet/byref non modélisées, les auxiliaires copy/dispose et les dispositions inconnues restent non récupérés.

La reconstruction du runtime est limitée : propriétés et protocoles complets, annotations de propriété mémoire originales, agrégats arbitraires, arguments variadiques, corps dépendant d’exceptions et dispositions Block/captures non modélisées ne sont pas promis. L’encodage décrit les paramètres fixes et ne prouve pas l’absence de points de suspension dans l’original. Les pointeurs chaînés sont utilisés uniquement lorsque le chargeur a résolu leurs emplacements ; les formats non résolus conservent leurs diagnostics.

## Sources Swift et stockage

La sortie structurée du demangler sépare signatures appelables et métadonnées non appelables. Avant projection du corps natif, les signatures prises en charge sont liées aux symboles, entrées et ABI machine explicites du binaire sélectionné. Les receveurs Swift suivent l’ABI Swift, sans substitution des arguments cachés Objective-C. Un fichier de signatures fourni par l’utilisateur reste une indication à valider.

L’émetteur expérimental construit les fonctions libres, méthodes de classe, initialiseurs désignés et méthodes de structures à disposition fixe pris en charge, dont certaines formes de receveur mutating. Déclarations de classes/structures et champs stockés exigent des métadonnées de disposition récupérées. Les appels natifs ne sont émis que si les déclarations et corps nécessaires forment un groupe de dépendances complet et pris en charge. Les unités source regroupent déclarations et méthodes, sans pont appelant le binaire original.

Les corps des getter/setter Swift pris en charge proviennent de l’implémentation native et sont assemblés en propriétés. Un stockage sous-jacent privé conserve la disposition établie des champs ; les initialiseurs et les autres méthodes utilisent les mêmes noms de stockage. Une déclaration de propriété ou un enregistrement de champ ne suffit pas à établir la récupération du corps d’un accesseur.

Les constructeurs allouants, destructeurs/libérations triviaux, accesseurs de métadonnées de type et entrées `_modify`/resume pris en charge peuvent être projetés dans une unité de type émise. Chaque entrée exige une preuve bornée de l’ensemble du flux natif et de ses effets, des dépendances contexte/initialiseur/propriété effectivement récupérées, ainsi que les audits de gestion des exceptions et d’IR des corps associés. Les écritures de l’allocateur doivent correspondre à l’initialiseur réel ; `_modify` doit lier le champ mutable exact et la continuation. Les appels de métadonnées d’exécution conservent leur sémantique modélisée dans le type récupéré. Ces entrées sont explicitement des projections de source du compilateur, et non des corps de méthodes ordinaires récupérés séparément ni le texte source d’origine.

Dispositions génériques ou résilientes, fonctions async/throwing, conventions inconnues, accesseurs/allocateurs/thunks non pris en charge, initialisation incomplète et dépendances natives/runtime non liées restent individuellement `unrecovered`. Un symbole mangled ou nom de type nominal ne constitue pas une méthode récupérée. Symboles supprimés et nœuds du demangler non classifiés rendent la couverture partielle ou inconnue.

## Sortie et couverture

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

Les fichiers du langage source existent uniquement si du code peut être émis. `objc.json` contient classes, catégories, ivars et encodages bruts ; `objc.h` contient les déclarations prises en charge. `swift.json` contient types nominaux et symboles mangled. Les JSON de signatures/méthodes conservent classification, omissions, raisons et compteurs. Les journaux contiennent les diagnostics natifs et ceux de l’export Swift natif lorsque celui-ci est exécuté. Aucun journal de découverte de chaîne Swift externe ou de démanglage externe n’est produit. Les chemins de `report.json` sont relatifs à son répertoire. Le binaire sélectionné est un artefact d’analyse, jamais une dépendance liée comme pont de récupération dans le code généré.

Copies temporaires du paquet et JSON intermédiaires sont supprimés. Sans corps natifs, un traitement normal échoue même si les métadonnées existent. Le mode métadonnées produit seulement l’artefact sélectionné, `objc.h`, `objc.json`, `swift.json` et `report.json` ; sources et fichiers de signatures/couverture sont absents, et `native_function_count`, `objc_method_recovery`, `swift_method_recovery` valent `null`. Tous les modes utilisent les métadonnées Objective-C résolues par le chargeur natif. Les métadonnées Swift sont lues dans l’image native avec des bornes vérifiées ; les fixups, dispositions relogeables ou références non pris en charge conservent des diagnostics de résultat partiel.

Ce rapport illustratif abrégé montre volontairement une récupération partielle :

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

Le `status: "success"` externe signifie que la sortie validée a été publiée. Les états `recovered`, `partial`, `unrecovered`, `no-methods` décrivent l’inventaire découvert, sans prouver équivalence sémantique ni exhaustivité du programme original. Chaque méthode non récupérée a une raison. Pour Objective-C, `recovered` exige également des métadonnées runtime complètes. Un inventaire vide ne prouve pas l’absence de méthodes.

Le `coverage_status` Swift compte seulement les éléments appelables classifiés. Le `status` Swift global tient aussi compte des symboles inconnus et peut être `unclassified`, `unsupported-architecture` ou `no-symbols`. Les métadonnées non appelables figurent dans `non_method_symbols` avec `not-callable`, les inconnues avec `unclassified`. `types`, `type_metadata_count`, `source_type_count` comptent indépendamment métadonnées/types émis, sans augmenter artificiellement les méthodes.

Chaque ligne Swift récupérée indique `source_representation` avec la valeur `native-method-body` ou `compiler-generated-from-type`. Les projections du compilateur conservent aussi `compiler_projection_kind` et `compiler_projection_evidence`. `source_body_method_count` compte les corps natifs récupérés ; `compiler_projection_method_count` compte les projections du compilateur prouvées. Leur somme vaut `recovered_method_count`. Les entrées du compilateur restent dans le dénominateur `method_count` et conservent leur identité exacte dans une seule unité source `type` correspondante. Des métadonnées de type ou un nom de dépendance seuls n’augmentent pas la couverture récupérée. Le JSON natif par lot inclut `source` dans les lignes du compilateur et les unités de type ; les `source_units` du rapport mobile conservent seulement les descriptions, sans `source`, et le source complet se trouve dans `sources/swift.swift`.

Le lot Swift natif décrit `source_units` par `{kind, module, name, source, method_entries, method_identities}` ; kind vaut `function` ou `type`, et chaque identité `{entry, mangled_symbol}`. Des symboles différents peuvent partager une entrée avec leurs propres projections ABI. Chaque identité récupérée doit apparaître exactement une fois, aucune identité non récupérée ne peut figurer. `method_entries` doit être exactement la projection ordonnée des entrées de `method_identities`, adresses répétées comprises. Des identités strictement identiques ne peuvent être fusionnées silencieusement. `source` concatène les sources des unités avec un saut de ligne chacune. Mobile conserve la source complète dans `sources/swift.swift` et les descriptions dans le JSON de couverture. Le `source` individuel sert à l’inspection ; concaténer ces lignes ne reconstruit pas correctement les classes.

## Exports natifs directs et SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

L’export Swift consomme l’inventaire structuré de signatures produit par le parseur intégré lors d’un traitement mobile normal. Le JSON Objective-C contient `native_source`, `native_function_count`, `objc_metadata`, puis source C, nom, type de retour et paramètres par méthode. Mobile vérifie encore déclarations, corps et dispositions avant `.m` ; sa couverture finale peut être inférieure à celle du C natif. Un export natif réussi peut ne contenir aucune méthode récupérée.

Pour une session Mach-O déjà chargée, `neverd_objc_methods_json(session, max_functions)` et `neverd_swift_methods_json(session, signatures_json, max_functions)` renvoient ces rapports. Zéro sélectionne toutes les fonctions découvertes. Libérez les chaînes avec `neverd_free_string` ; `NULL` indique un échec expliqué par l’erreur de session. Ces API ne chargent pas les conteneurs IPA ou `.app`.

## Vérification et dépannage

Sur macOS, les builds avec `BUILD_TESTING` activé proposent `check-neverd-mobile-ios`, qui exécute les trois suites de récupération native via CTest.

Python sert uniquement aux scripts de test de développement ci-dessous ; la récupération mobile intégrée s’exécute dans la CLI native C++20.

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

Sur macOS, les scripts Objective-C compilent les originaux, récupèrent le `.m`, puis lient uniquement le source généré à un programme d’appel indépendant. Le script scalaire couvre les bornes entières, branches, boucles, lectures/écritures par pointeur, arguments implicites, identité des bits float/double, arguments mixtes et arguments sur la pile. Le script d’appels ajoute la distribution des messages, l’héritage, les Category, le stockage des variables d’instance, les auxiliaires natifs et les appels/captures/identités partagées des Blocks. Le corpus d’appels exige 21/21 méthodes récupérées et 134/134 résultats indépendants pour chaque variante arm64/x86_64 × classic/default. Exécutez ces contrôles avec la compilation actuelle de la CLI native.

Le script Swift strict vérifie 22 déclarations utilisateur, trois entrées getter/setter et sept entrées appelables générées par le compilateur ; aucune ne peut disparaître de l’inventaire. Chaque variante comporte 855 vérifications indépendantes des résultats du programme original. Il compile indépendamment le `.swift` généré et son programme d’appel, sans dylib, module ou pont d’origine, ni déclaration de remplacement écrite à la main. Les cas couvrent les appels scalaires/natifs, l’initialisation et le stockage des classes, les méthodes de structures par valeur/mutating, les arguments flottants et sur la pile, les pointeurs et les boucles. La CLI native C++20 doit réussir les quatre variantes arm64/x86_64 × classic/default sans cas ignoré : chacune doit récupérer 25 corps natifs et sept projections du compilateur, conserver les 32 identités appelables et réussir 855/855 vérifications pour les originaux comme pour le Swift généré compilé indépendamment. Ces résultats se limitent à ce corpus et ne garantissent pas la récupération d’applications quelconques ni du texte source d’origine. Le script refuse la couverture manquante, les échecs de compilation du source et les différences de comportement.

Les trois scripts acceptent `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N` et `--work-dir NEW_DIRECTORY`. `--setup-only` valide les originaux et ne teste pas la récupération. Les architectures inexécutables sur l’hôte sont explicitement ignorées lorsque cela est autorisé ; un cas ignoré n’est pas une réussite. Les artefacts d’échec conservés permettent de distinguer une couverture source manquante, une erreur de compilation et une différence de comportement. Consultez les résultats actuels avant d’annoncer une prise en charge vérifiée.

La publication est transactionnelle : choisissez un nouveau répertoire, vérifiez d’abord le code de sortie et placez le JSON redirigé ailleurs. Les échecs suppriment les résultats temporaires et préservent l’existant. Un backend terminant en erreur fournit une fin de journal bornée ; délais et budgets ont leurs propres messages. La CLI native renvoie zéro en cas de succès et une valeur non nulle en cas d’échec. Avec `--json`, les erreurs traitées comprennent `schema_version`, `status: "error"` et `error`. L’analyse des arguments, le démarrage de l’exécutable ou des bibliothèques natives et les interruptions peuvent être signalés uniquement sur stderr. Vérifiez d’abord le code de sortie.

Pour une tranche chiffrée, fournissez une entrée lisible ; pour une architecture absente, examinez les tranches disponibles. Consultez les raisons exactes et diagnostics des méthodes omises. Augmenter `--max-func` aide seulement les fonctions exclues par cette limite. Dispositions, signatures, en-têtes externes, exceptions ou ABI manquants exigent une implémentation ou des métadonnées valides supplémentaires, pas une affirmation de récupération complète. Conservez les mentions de licence applicables lors de la distribution des outils ou paquets générés.
