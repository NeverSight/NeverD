**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# Récupération du code natif et des sources iOS

[← Index de la documentation](README.md) · [Vue mobile](../mobile.md)

`neverd mobile` accepte IPA, `.app` et Mach-O. Il exporte du C natif, les métadonnées du runtime et, à titre expérimental, des sources Objective-C `.m` et Swift `.swift` pour les corps natifs pris en charge. Un résultat publié peut contenir des méthodes non récupérées : examinez la couverture avant utilisation. Les conteneurs mobiles relèvent de la CLI ; le SDK C natif charge séparément le Mach-O sélectionné.

La compilation élimine commentaires, mise en forme, identifiants et constructions du langage source. Ce processus reconstruit une représentation source, sans retrouver le texte original ni certifier une équivalence de comportement pour une application quelconque. Il ne lance pas l’application analysée.

## Démarrage et dépendances

```sh
cmake --build build --target neverd
python3 --version
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Compilez NeverD normalement et distribuez le répertoire voisin `mobile/` avec l’exécutable. Python 3.10+ est requis, choisi par `--python PATH`, puis `NEVERD_PYTHON`, puis `python3`/`python` dans PATH. Aucun téléchargement automatique de dépendances. Recompiler indépendamment les sources Apple générées sous macOS nécessite Apple Clang, le SDK et la chaîne Swift ; cela reste distinct de l’analyse native statique.

La récupération des signatures Swift choisit `--swift-demangle PATH`, puis `NEVERD_SWIFT_DEMANGLE`, puis `swift-demangle` dans PATH. Sous macOS, une recherche limitée dans le temps avec `xcrun --find swift-demangle` constitue le dernier recours automatique. Un outil explicitement configuré mais absent provoque un échec ; une recherche automatique infructueuse conserve les symboles non classifiés avec `unavailable`. Sans symboles Swift, aucun demangler n’est nécessaire. `--metadata-only` n’invoque ni backend natif ni demangler.

```sh
neverd mobile App.ipa -o recovered-swift \
  --python python3 --swift-demangle /path/to/swift-demangle --timeout=600 --json
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
| `--python PATH` | Environnement/PATH | Interpréteur Python 3.10+ de l’auxiliaire |
| `--swift-demangle PATH` | Environnement/PATH/chaîne | Demangler des signatures Swift |
| `--timeout N` | `300` | Nombre positif de secondes par processus backend |
| `--max-files N` | `20000` | Budget positif d’entrées ; l’inventaire Swift est également borné |
| `--max-bytes N` | `2147483648` | Budget positif d’octets pour entrée, extraction et sortie finale |
| `--json` | Désactivé | Rapport versionné au format JSON |

Le répertoire de travail est surveillé et peut utiliser jusqu’à trois fois les budgets d’entrées/octets pour les copies et résultats intermédiaires. Les journaux sont limités à 16 MiB par processus, et le JSON des signatures Swift à 32 MiB. Ces contrôles de ressources n’isolent pas les processus. Augmenter le délai ne supprime aucune autre limite. Les méthodes exclues par `--max-func` restent non récupérées lorsqu’elles figurent dans les métadonnées.

## Sources Objective-C et structure du runtime

Le chargeur natif associe enregistrement de méthode, adresse IMP exécutable et encodage de type pris en charge à des emplacements ABI source explicites. Les arguments fixes scalaires/pointeurs conservent `self`/`_cmd`, paramètres inutilisés, banques séparées d’entiers/flottants et positions de pile prises en charge. Réinterprétation des bits float/double et conversion numérique restent distinctes. Les indications de type servent à la projection source ; elles ne constituent ni preuve ABI authentifiée ni autorisation de modifier le code exécutable.

`sources/objc.m` place les instructions réellement reconstruites dans des corps `@implementation` et conserve auxiliaires C et appels typés nécessaires. Les cibles d’appel exigent une liaison source prise en charge ; cibles inconnues et groupes de dépendances incomplets restent non récupérés. Définitions absentes, adresses non exécutables, encodages contradictoires, ABI non gérées, décodage incomplet ou IR refusé ne deviennent pas des méthodes récupérées du seul fait d’une déclaration.

Les métadonnées de classe conservent superclasse, début/taille d’instance et ivars scalaires/pointeurs dont offsets, largeurs et alignements sont vérifiés. Les déclarations ajoutent du remplissage au besoin. Une méthode exigeant une disposition inconnue reste non récupérée. Les catégories gardent leurs identités classe/catégorie/adresse et implémentations distinctes ; les enregistrements strictement identiques répétés dans les deux inventaires ne comptent qu’une fois. Les catégories externes utilisent une déclaration Foundation existante lorsqu’elle est prise en charge. Les en-têtes externes inconnus sont signalés comme dépendances absentes, sans inventer de classe de remplacement.

La reconstruction du runtime est limitée : propriétés et protocoles complets, annotations de propriété mémoire originales, agrégats arbitraires, arguments variadiques, corps dépendant d’exceptions et dispositions Block/captures non modélisées ne sont pas promis. L’encodage décrit les paramètres fixes et ne prouve pas l’absence de points de suspension dans l’original. Les pointeurs chaînés sont utilisés uniquement lorsque le chargeur a résolu leurs emplacements ; les formats non résolus conservent leurs diagnostics.

## Sources Swift et stockage

La sortie structurée du demangler sépare signatures appelables et métadonnées non appelables. Avant projection du corps natif, les signatures prises en charge sont liées aux symboles, entrées et ABI machine explicites du binaire sélectionné. Les receveurs Swift suivent l’ABI Swift, sans substitution des arguments cachés Objective-C. Un fichier de signatures fourni par l’utilisateur reste une indication à valider.

L’émetteur expérimental construit les fonctions libres, méthodes de classe, initialiseurs désignés et méthodes de structures à disposition fixe pris en charge, dont certaines formes de receveur mutating. Déclarations de classes/structures et champs stockés exigent des métadonnées de disposition récupérées. Les appels natifs ne sont émis que si les déclarations et corps nécessaires forment un groupe de dépendances complet et pris en charge. Les unités source regroupent déclarations et méthodes, sans pont appelant le binaire original.

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

Les fichiers du langage source existent uniquement si du code peut être émis. `objc.json` contient classes, catégories, ivars et encodages bruts ; `objc.h` contient les déclarations prises en charge. `swift.json` contient types nominaux et symboles mangled. Les JSON de signatures/méthodes conservent classification, omissions, raisons et compteurs. Les journaux incluent diagnostics natifs et, si utilisés, recherche de chaîne Swift, demangling et export Swift natif. Les chemins de `report.json` sont relatifs à son répertoire. Le binaire sélectionné est un artefact d’analyse, jamais une dépendance liée comme pont de récupération dans le code généré.

Copies temporaires du paquet et JSON intermédiaires sont supprimés. Sans corps natifs, un traitement normal échoue même si les métadonnées existent. Le mode métadonnées produit seulement l’artefact sélectionné, `objc.h`, `objc.json`, `swift.json` et `report.json` ; sources et fichiers de signatures/couverture sont absents, et `native_function_count`, `objc_method_recovery`, `swift_method_recovery` valent `null`. Son lecteur Objective-C Python ne résout ni pointeurs chaînés ni pointeurs d’objets relogeables. La récupération complète utilise les métadonnées Objective-C résolues du chargeur natif. Le lecteur Swift brut peut encore signaler des références non prises en charge comme partielles.

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
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

Le `status: "success"` externe signifie que la sortie validée a été publiée. Les états `recovered`, `partial`, `unrecovered`, `no-methods` décrivent l’inventaire découvert, sans prouver équivalence sémantique ni exhaustivité du programme original. Chaque méthode non récupérée a une raison. Pour Objective-C, `recovered` exige également des métadonnées runtime complètes. Un inventaire vide ne prouve pas l’absence de méthodes.

Le `coverage_status` Swift compte seulement les éléments appelables classifiés. Le `status` Swift global tient aussi compte des symboles inconnus et peut être `unavailable`, `unclassified`, `unsupported-architecture` ou `no-symbols`. Les métadonnées non appelables figurent dans `non_method_symbols` avec `not-callable`, les inconnues avec `unclassified`. `types`, `type_metadata_count`, `source_type_count` comptent indépendamment métadonnées/types émis, sans augmenter artificiellement les méthodes.

Le lot Swift natif décrit `source_units` par `{kind, module, name, source, method_entries, method_identities}` ; kind vaut `function` ou `type`, et chaque identité `{entry, mangled_symbol}`. Des symboles différents peuvent partager une entrée avec leurs propres projections ABI. Chaque identité récupérée doit apparaître exactement une fois, aucune identité non récupérée ne peut figurer. `method_entries` doit être exactement la projection ordonnée des entrées de `method_identities`, adresses répétées comprises. Des identités strictement identiques ne peuvent être fusionnées silencieusement. `source` concatène les sources des unités avec un saut de ligne chacune. Mobile conserve la source complète dans `sources/swift.swift` et les descriptions dans le JSON de couverture. Le `source` individuel sert à l’inspection ; concaténer ces lignes ne reconstruit pas correctement les classes.

## Exports natifs directs et SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

L’export Swift consomme l’inventaire structuré de signatures d’un traitement mobile normal avec demangler. Le JSON Objective-C contient `native_source`, `native_function_count`, `objc_metadata`, puis source C, nom, type de retour et paramètres par méthode. Mobile vérifie encore déclarations, corps et dispositions avant `.m` ; sa couverture finale peut être inférieure à celle du C natif. Un export natif réussi peut ne contenir aucune méthode récupérée.

Pour une session Mach-O déjà chargée, `neverd_objc_methods_json(session, max_functions)` et `neverd_swift_methods_json(session, signatures_json, max_functions)` renvoient ces rapports. Zéro sélectionne toutes les fonctions découvertes. Libérez les chaînes avec `neverd_free_string` ; `NULL` indique un échec expliqué par l’erreur de session. Ces API ne chargent pas les conteneurs IPA ou `.app`.

## Vérification et dépannage

```sh
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

Sous macOS, le runner Objective-C compile ses propres exemples originaux, récupère `.m`, puis lie uniquement les sources générées avec un programme appelant indépendant. Il couvre limites entières, branches, boucles, pointeurs, paramètres cachés, identité des bits float/double, paramètres mixtes et pile. Le runner Swift recompile indépendamment `.swift` et son programme de test, sans dylib original, module, pont ni déclarations de remplacement manuscrites. Il vérifie scalaires/appels natifs, initialisation/stockage de classe, méthodes de structures par valeur/mutating, flottants, pile, pointeurs et boucles. Ces contrôles stricts peuvent révéler des limitations ; leur existence ne prouve pas la réussite de tous les cas sur chaque build.

Les deux scripts acceptent `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N`, `--work-dir NEW_DIRECTORY`. `--setup-only` vérifie les originaux, pas la récupération. Une architecture inexécutable sur l’hôte est explicitement ignorée lorsque permis ; un saut n’est pas une réussite. Conservez les artefacts pour distinguer couverture manquante, erreurs de compilation et différences de comportement. Consultez les résultats actuels avant de déclarer le support vérifié.

La publication est transactionnelle : choisissez un nouveau répertoire, vérifiez d’abord le code de sortie et placez le JSON redirigé ailleurs. Les échecs suppriment les résultats temporaires et préservent l’existant. Un backend terminant en erreur fournit une fin de journal bornée ; délais et budgets ont leurs propres messages. Avec `--json`, les erreurs gérées de l’auxiliaire produisent `status: "error"` ; analyse des arguments, auxiliaire/interpréteur absent, Python antérieur à 3.10 ou interruption peuvent échouer plus tôt sur stderr.

Pour une tranche chiffrée, fournissez une entrée lisible ; pour une architecture absente, examinez les tranches disponibles ; pour Swift, sélectionnez le véritable demangler. Consultez les raisons exactes et diagnostics des méthodes omises. Augmenter `--max-func` aide seulement les fonctions exclues par cette limite. Dispositions, signatures, en-têtes externes, exceptions ou ABI manquants exigent une implémentation ou des métadonnées valides supplémentaires, pas une affirmation de récupération complète. Conservez les mentions de licence applicables lors de la distribution des outils ou paquets générés.
