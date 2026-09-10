**Langues**: [English](../android.md) | [简体中文](../zh-CN/android.md) | [繁體中文](../zh-TW/android.md) | [日本語](../ja/android.md) | [한국어](../ko/android.md) | [Français](android.md) | [Deutsch](../de/android.md) | [Español](../es/android.md) | [Italiano](../it/android.md) | [Русский](../ru/android.md) | [العربية](../ar/android.md)

# Reconstruction Java pour Android

[← Index de la documentation](README.md)

`neverd mobile` restaure du Java lisible depuis APK, DEX et smali avec le moteur intégré de NeverD par défaut. Les lecteurs, développés indépendamment, partagent un modèle Dalvik typé et un générateur Java au travail borné. Cette fonction CLI expérimentale ne promet ni la parité avec JADX ni la restauration complète de tout APK. Les conteneurs APK et la sortie Java ne sont pas accessibles via le SDK C natif, le SDK des plugins Python, le chargeur graphique ou `neverd decompile --language`.

Le Java produit est une reconstruction du bytecode. Les commentaires, la mise en forme, les choix propres au langage source d’origine et les identifiants supprimés ne sont pas disponibles ; le bytecode Kotlin produit lui aussi du Java. Une exécution réussie ne prouve pas l’équivalence sémantique et ne garantit pas que chaque méthode se recompilera. Ce traitement ne lance aucune application analysée.

## Démarrage rapide

Après avoir préparé les environnements d’exécution ci-dessous, choisissez un nouveau répertoire de sortie :

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Ouvrez `recovered-app/sources/` pour lire les fichiers Java et `recovered-app/report.json` pour consulter l’inventaire des entrées et les limites. Une entrée de type répertoire est préférable lorsque les classes smali se référencent entre elles.

## Configuration des environnements d’exécution

| Composant | Exigence | Sélection |
|-----------|----------|-----------|
| NeverD | Compilez la cible `neverd` avec une chaîne compatible C++20. Le traitement mobile est intégré à la CLI native et n’appelle aucun interpréteur Python. Distribuez l’exécutable avec les bibliothèques natives nécessaires à votre compilation. | `build/bin/neverd` / PATH |

Le moteur par défaut est implémenté en C++20 et ne nécessite aucun environnement Python, Java ou JADX à l’exécution. Il accepte les déclarations et opérations ordinaires représentables de DEX 035, 037–040 et smali. DEX 041, les appels dynamiques tels que `invoke-custom`, certains chemins d’initialisation, les annotations sémantiques ou opérations inconnues et les identifiants impossibles à exprimer en Java échouent explicitement. Accepter un format ne signifie pas en prendre en charge toutes les instructions ou déclarations.

La résolution des noms Java distingue l’en-tête du corps d’une classe et peut traiter les masquages connus au sein d’un même package ; elle refuse explicitement les cas où l’absence de déclarations d’une superclasse ou d’une interface externe empêche de déterminer les types ou les références du code auxiliaire Java généré, et seul `java.lang.Object` est supposé ne fournir aucun type membre héritable sans déclaration disponible. Les littéraux à virgule flottante smali sont arrondis directement à la précision simple ou double cible, en préservant la représentation binaire résultante.

Le moteur C++ intégré conserve et valide les métadonnées `Signature` prises en charge pour les classes, champs et méthodes : variables de type, tableaux, jokers, bornes et masquage au niveau des méthodes. L’effacement doit correspondre à l’identité de la déclaration DEX d’origine. `Throws` est conservé ; une hiérarchie d’exceptions non prouvée est refusée. L’héritage générique ou la substitution des types membres, la régénération des méthodes ponts, les appels de méthodes génériques, les types internes paramétrés et la correspondance des paramètres cachés des constructeurs restent explicitement non pris en charge sans les preuves nécessaires.

La CI traite des exemples Java 8 propres au projet avec D8 et NeverD, recompile tout le Java généré et compare les métadonnées `Signature` complètes, les résultats de réflexion et le comportement. Ces vérifications ne certifient pas la récupération complète d’applications réelles.

### Linux et macOS

```sh
cmake --build build --target neverd

./build/bin/neverd mobile app.apk -o recovered-app
```

Ni `NEVERD_JADX` ni un exécutable `jadx` dans PATH ne sélectionnent le moteur externe : seul `--jadx PATH` explicite le fait. Aucun repli automatique n’est effectué. Entourez de guillemets les chemins contenant des espaces.

### Windows PowerShell

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app
```

Une compilation multiconfiguration peut placer l’exécutable dans `build/bin/Release/`. Respectez les règles habituelles de déploiement des bibliothèques natives de cette compilation.

## Entrées prises en charge et limites

| Entrée | Comportement | Limite importante |
|--------|--------------|-------------------|
| `.apk` | Valider le ZIP entier, puis analyser ensemble tous les `classes.dex`, `classes2.dex` et autres DEX numérotés à la racine de l’archive | Code uniquement ; aucun décodage des ressources ou du manifeste |
| `.dex` | Validation et analyse de DEX 035 ou 037–040 par le lecteur intégré | DEX 041 et les déclarations ou opérations non prises en charge échouent ; renommer ou tronquer un fichier ne produit pas un bytecode valide |
| `.smali` | Analyser la classe fournie | Les classes voisines référencées ne sont pas chargées implicitement |
| Répertoire smali | Collecter récursivement les fichiers `.smali` et les analyser ensemble | Inclure les classes imbriquées et les racines smali dépendantes dans le répertoire d’entrée |

Pour analyser une arborescence d’APK décodé contenant `smali/` et `smali_classes2/`, passez leur répertoire parent commun. Seuls les fichiers `.smali` parviennent au backend, mais l’arborescence entière est d’abord validée et copiée ; les ressources volumineuses sans rapport avec le code comptent donc aussi dans les limites d’entrée. Un répertoire compact contenant uniquement les racines smali pertinentes réduit le travail.

Les APK fractionnés constituent des entrées séparées. Chaque APK contenant du DEX peut être traité indépendamment, mais cette commande ne fusionne pas un ensemble d’APK ; les fractions constituées uniquement de ressources échouent, car elles ne contiennent aucun DEX à la racine. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat` et `.vdex` ne sont pas acceptés comme entrées mobiles. La prise en charge de certains de ces formats par le backend ne signifie pas qu’ils sont pris en charge par cette commande NeverD.

Les ressources APK, `AndroidManifest.xml`, les assets, les bibliothèques JNI/natives et le code téléchargé à l’exécution ne sont pas reconstruits en Java. Extrayez séparément une bibliothèque native `.so` et utilisez `neverd decompile library.so -o library.c`. Pour ce traitement statique, les charges utiles chiffrées ou empaquetées doivent déjà être disponibles sous forme de DEX/smali ordinaires ; aucun dépaquetage de protection, aucune connexion à un appareil et aucun contournement de protection ne sont effectués.

## Options et priorité

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| Option | Valeur par défaut | Signification |
|--------|-------------------|---------------|
| `-o DIRECTORY` | Obligatoire | Nouveau répertoire de sortie, extérieur à toute entrée de type répertoire ; ne jamais écraser une sortie existante |
| `--platform=auto\|android` | `auto` | Choisir Android explicitement ou déduire la plateforme de l’entrée |
| `--jadx PATH` | Non défini : moteur intégré | Sélection explicite de l’adaptateur de compatibilité JADX installé séparément ; aucune sélection par l’environnement ni aucun repli automatique |
| `--timeout N` | `300` | Budget de temps positif pour l’analyse intégrée ; limite en secondes par processus externe, vérification de version comprise |
| `--max-files N` | `20000` | Limite positive du nombre d’entrées, répertoires créés compris |
| `--max-bytes N` | `2147483648` | Limite positive en octets pour l’entrée, les données extraites et la sortie finale |
| `--json` | Désactivé | Afficher le rapport en JSON plutôt que sous forme de résumé destiné à l’utilisateur |

Une sélection `--arch` autre que la valeur par défaut, `--artifact`, `--metadata-only` et un `--max-func` non nul sont réservés à iOS et refusés pour Android ; `--arch=auto` explicite est accepté. Aucune transmission d’options arbitraires au backend n’est prévue. L’adaptateur JADX explicite isole les répertoires de configuration, cache et fichiers temporaires et n’importe pas les réglages ambiants ni la configuration des plugins.

Les budgets de fichiers et d’octets restent applicables aux entrées, aux données extraites et à la sortie finale. Les lecteurs et le générateur intégrés vérifient aussi une quantité de travail bornée et le temps écoulé. Les espaces de travail externes autorisent jusqu’à trois fois les budgets d’entrées et d’octets pour les copies et résultats intermédiaires ; les journaux sont limités à 16 MiB par processus. Ces contrôles de ressources ne constituent pas un bac à sable. Augmenter une limite ne désactive pas les autres.

## Organisation de la sortie et rapport JSON

```text
recovered-app/
  sources/                       packages et classes Java restaurés
  metadata/android-methods.json  couverture des méthodes du moteur intégré
  report.json                    inventaire versionné et limites
```

Les entrées temporaires sont supprimées. Des classes imbriquées peuvent partager le fichier de leur classe englobante : le nombre de fichiers Java n’est donc pas celui des classes DEX. Les méthodes générées peuvent employer une boucle de répartition Java ; elles n’exécutent pas le DEX original et ne l’appellent pas via une passerelle d’exécution.

Le rapport intégré contient `android_method_recovery`, également écrit dans `metadata/android-methods.json`, et conserve chaque méthode d’origine. L’invariant est `method_count = recovered_method_count + projected_method_count + declaration_only_method_count + unrecovered_method_count` ; un `projected_method_count` absent vaut zéro, et `unrecovered_method_count` reste nul avant publication. Les méthodes `native` et `abstract` d’origine portent le statut `declaration-only` et ne comptent pas comme corps restaurés.

Un sous-ensemble de classes locales nommées sans capture peut être émis dans la méthode statique exacte qui les contient. Il exige une méthode ordinaire à types scalaires, une classe sans champs héritant directement d’`Object`, un véritable constructeur sans argument, des méthodes d’instance scalaires et des usages d’objets dont l’absence d’échappement hors du périmètre pris en charge est vérifiée. Les classes anonymes, captures, modificateurs non pris en charge et usages non prouvés échouent explicitement.

Ces méthodes locales et leur méthode englobante reçoivent `source-projected`, avec `projection_kind: "named-method-local"`. La couverture reste `partial` même si le rapport global indique `success`. Les noms binaires et les indicateurs d’accès après recompilation restent non vérifiés. Le compilateur Java pouvant choisir un autre nom binaire local, `class_source_bindings` conserve la classe d’origine, la méthode englobante exacte, le chemin source et le nom local avec `binary_name_status: "unverified"`.

`generated_source_helpers` inventorie les méthodes supplémentaires avec les types exacts `throw-helper`, `constant-helper`, `default-constructor` et `field-initializer`. Ce dernier désigne un `<clinit>` supplémentaire généré qui ne figurait pas dans l’inventaire des méthodes d’origine. Ces ajouts ne comptent pas dans le total des méthodes d’origine. Une compilation réussie ou une concordance ponctuelle des noms ne constitue pas une restauration complète. L’exemple abrégé suivant ne contient aucune méthode projetée :

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {
    "name": "neverd",
    "version": "1",
    "execution": "builtin"
  },
  "input_code_files": [
    "classes.dex",
    "classes2.dex"
  ],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": [
    "sources/example/Main.java",
    "sources/example/Peer.java"
  ],
  "logs": [],
  "android_method_recovery": {
    "schema_version": 1,
    "status": "recovered",
    "class_count": 2,
    "method_count": 6,
    "recovered_method_count": 5,
    "declaration_only_method_count": 1,
    "unrecovered_method_count": 0
  }
}
```

`input_kind` vaut `apk`, `dex`, `smali` ou `smali-directory`. `input_code_files` répertorie les noms de bytecode ou les chemins smali en entrée, tandis que `java_sources` et `logs` sont relatifs à la racine de sortie. `source` est le nom de base de l’entrée. Les rapports réels contiennent d’autres limites de reconstruction ; conservez-les lorsque vous transmettez les résultats à d’autres outils.

Pour l’automatisation, vérifiez le code de sortie du processus avant d’exploiter `status` et, si vous redirigez stdout, placez le rapport hors du nouveau répertoire de sortie :

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

La CLI native renvoie zéro en cas de succès et une valeur non nulle en cas d’échec. Avec `--json`, les erreurs traitées comprennent `schema_version`, `status: "error"` et `error`. L’analyse des arguments, le démarrage de l’exécutable ou des bibliothèques natives et les interruptions peuvent être signalés uniquement sur stderr. Vérifiez d’abord le code de sortie.

## Gestion des échecs et dépannage

La publication est transactionnelle : la sortie existante est préservée et les résultats temporaires d’un échec sont supprimés. Les opérations non prises en charge, flux de registres non résolus, déclarations impossibles à représenter, traitements d’exceptions mal formés et budgets épuisés font échouer le moteur intégré au lieu de publier des corps manquants. L’adaptateur externe refuse aussi les sorties non nulles, erreurs d’assemblage ou de décompilation consignées, omissions de classes dupliquées, marqueurs de code incomplet, fichiers Java vides et absence de Java. La réussite ne prouve pas l’équivalence sémantique.

| Symptôme | Action |
|----------|--------|
| DEX, instruction, déclaration ou initialisation non pris en charge | Lire le diagnostic et vérifier le sous-ensemble accepté. Utiliser `--jadx PATH` uniquement pour choisir délibérément l’adaptateur distinct |
| Entrée invalide ou classe dupliquée | Corriger le bytecode ou l’ensemble de classes ; aucun corps non pris en charge n’est omis silencieusement |
| Délai ou budget dépassé | Réduire l’entrée ou ajuster `--timeout`, `--max-files` et `--max-bytes` selon les ressources disponibles |
| Sortie déjà présente | Choisir un nouveau répertoire de sortie |

## Adaptateur de compatibilité JADX facultatif

`--jadx PATH` sélectionne JADX externe, et non l’implémentation intégrée. Installez JADX 1.5.6 ou ultérieur avec ses plugins d’entrée DEX/smali standard et Java 11 ou ultérieur. Obtenez la [distribution JADX complète](https://github.com/skylot/jadx/releases/tag/v1.5.6), conservez sa structure `bin/` et `lib/` ainsi que les licences des dépendances fournies lors d’une redistribution. Aucun téléchargement n’est automatique. Le rapport identifie le moteur réel `jadx` et sa version détectée ; il ne prétend pas fournir la couverture des méthodes du moteur intégré.

Sous Windows, indiquez le lanceur `.bat`/`.cmd` de la distribution ou `lib/jadx-*-all.jar`. NeverD résout le JAR et appelle Java directement, sans faire passer les chemins d’application dans un interpréteur de commandes. `JAVA_HOME` ou PATH sélectionne Java. Les exécutions réussies conservent `logs/jadx-version.log` et `logs/jadx.log` ; les répertoires temporaires et journaux d’un échec sont supprimés. Seule une sortie non nulle du backend inclut une fin de journal bornée. Les échecs de lancement, délais dépassés et dépassements de budget ont leurs propres diagnostics.

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## Vérification et profondeur de prise en charge

Python sert uniquement aux scripts de test de développement ci-dessous ; la récupération mobile intégrée s’exécute dans la CLI native C++20.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
```

Les tests des composants et de la CLI vérifient l’analyse, les contrats de sortie et le nettoyage après échec. Le test d’exécution interne utilise un JDK (`java` et `javac`) et D8 pour construire des exemples DEX/APK indépendants, puis compiler et exécuter le Java restauré. Ce sont des dépendances de test, pas des prérequis de restauration intégrée. Exécutez-le sur la compilation actuelle et examinez les résultats avant de déclarer un cas vérifié. Le test de compatibilité distinct nécessite en plus JADX et vérifie cet adaptateur. La réussite d’exemples ne démontre pas la restauration complète de toute application.

Consultez la [présentation mobile](../mobile.md) pour le parcours iOS associé.
