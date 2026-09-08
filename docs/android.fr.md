**Langues**: [English](android.md) | [简体中文](android.zh-CN.md) | [繁體中文](android.zh-TW.md) | [日本語](android.ja.md) | [한국어](android.ko.md) | [Français](android.fr.md) | [Deutsch](android.de.md) | [Español](android.es.md) | [Italiano](android.it.md) | [Русский](android.ru.md) | [العربية](android.ar.md)

# Reconstruction Java pour Android

[← Index de la documentation](README.fr.md)

`neverd mobile` reconstruit du Java lisible à partir d’entrées APK, DEX et smali grâce à un backend JADX installé séparément. Le traitement valide le bytecode, prépare des copies de travail, analyse ensemble les classes liées, contrôle les fichiers générés et publie un répertoire de sources accompagné d’un rapport lisible par machine. Cette fonctionnalité CLI est expérimentale. Les conteneurs APK et la sortie Java ne sont accessibles ni par le SDK C natif, ni par le SDK de plugins Python, ni par le chargeur de l’interface graphique, ni par `neverd decompile --language`.

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
| NeverD | Compiler la cible `neverd` ; distribuer aussi le répertoire `mobile/` placé à côté de l’exécutable | `build/bin/neverd` ou un exécutable dans PATH |
| Python | Python 3.10 ou ultérieur, indépendamment de l’hôte de plugins intégré | `--python`, puis `NEVERD_PYTHON`, puis `python3`/`python` dans PATH |
| Backend Java | JADX 1.5.6 ou ultérieur avec les plugins d’entrée DEX et smali standard | `--jadx`, puis `NEVERD_JADX`, puis `jadx` dans PATH |
| Environnement Java | Java 11 ou ultérieur ; un JDK est nécessaire pour vérifier par compilation et exécution | `JAVA_HOME` ou Java dans PATH |

NeverD ne télécharge pas automatiquement les dépendances. Procurez-vous la [distribution JADX](https://github.com/skylot/jadx/releases/tag/v1.5.6) complète, conservez sa structure `bin/` et `lib/`, et gardez les licences incluses en cas de redistribution. La version du backend testée est la 1.5.6 ; les versions ultérieures doivent respecter le même contrat CLI. La configuration de ces dépendances est distincte de la compilation du pipeline LLVM de NeverD.

### Linux et macOS

```sh
cmake --build build --target neverd
python3 --version
java -version
/opt/jadx/bin/jadx --version

./build/bin/neverd mobile app.apk -o recovered-app \
  --python python3 --jadx /opt/jadx/bin/jadx
```

Pour une utilisation régulière, définissez `NEVERD_JADX=/opt/jadx/bin/jadx` et, si nécessaire, `NEVERD_PYTHON` avec le chemin d’un interpréteur. Faites pointer `JAVA_HOME` vers le répertoire d’installation du JDK si Java n’est pas déjà disponible. Les chemins contenant des espaces doivent être placés entre guillemets.

### Windows PowerShell

```powershell
$env:JAVA_HOME = 'C:\Tools\jdk'
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe' `
  --jadx 'C:\Tools\jadx\bin\jadx.bat'
```

Le chemin `.bat`/`.cmd` du backend permet de trouver l’unique `lib/jadx-*-all.jar` de la distribution ; NeverD appelle Java directement. Vous pouvez aussi passer ce JAR à `--jadx`. Les chemins d’application ne sont jamais insérés dans une commande shell. Les compilations à plusieurs configurations peuvent placer l’exécutable sous `build/bin/Release/`. Déplacer uniquement l’exécutable sans son répertoire `mobile/` provoque une erreur indiquant l’absence de l’outil auxiliaire.

## Entrées prises en charge et limites

| Entrée | Comportement | Limite importante |
|--------|--------------|-------------------|
| `.apk` | Valider le ZIP entier, puis analyser ensemble tous les `classes.dex`, `classes2.dex` et autres DEX numérotés à la racine de l’archive | Code uniquement ; aucun décodage des ressources ou du manifeste |
| `.dex` | Valider la signature DEX et laisser le backend décoder le contenu | Un fichier renommé ou tronqué n’est pas du bytecode valide |
| `.smali` | Analyser la classe fournie | Les classes voisines référencées ne sont pas chargées implicitement |
| Répertoire smali | Collecter récursivement les fichiers `.smali` et les analyser ensemble | Inclure les classes imbriquées et les racines smali dépendantes dans le répertoire d’entrée |

Pour analyser une arborescence d’APK décodé contenant `smali/` et `smali_classes2/`, passez leur répertoire parent commun. Seuls les fichiers `.smali` parviennent au backend, mais l’arborescence entière est d’abord validée et copiée ; les ressources volumineuses sans rapport avec le code comptent donc aussi dans les limites d’entrée. Un répertoire compact contenant uniquement les racines smali pertinentes réduit le travail.

Les APK fractionnés constituent des entrées séparées. Chaque APK contenant du DEX peut être traité indépendamment, mais cette commande ne fusionne pas un ensemble d’APK ; les fractions constituées uniquement de ressources échouent, car elles ne contiennent aucun DEX à la racine. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat` et `.vdex` ne sont pas acceptés comme entrées mobiles. La prise en charge de certains de ces formats par le backend ne signifie pas qu’ils sont pris en charge par cette commande NeverD.

Les ressources APK, `AndroidManifest.xml`, les assets, les bibliothèques JNI/natives et le code téléchargé à l’exécution ne sont pas reconstruits en Java. Extrayez séparément une bibliothèque native `.so` et utilisez `neverd decompile library.so -o library.c`. Pour ce traitement statique, les charges utiles chiffrées ou empaquetées doivent déjà être disponibles sous forme de DEX/smali ordinaires ; aucun dépaquetage de protection, aucune connexion à un appareil et aucun contournement de protection ne sont effectués.

## Options et priorité

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --jadx /opt/jadx/bin/jadx --timeout=600 \
  --max-files=30000 --max-bytes=4294967296 --json
```

| Option | Valeur par défaut | Signification |
|--------|-------------------|---------------|
| `-o DIRECTORY` | Obligatoire | Nouveau répertoire de sortie, extérieur à toute entrée de type répertoire ; ne jamais écraser une sortie existante |
| `--platform=auto\|android` | `auto` | Choisir Android explicitement ou déduire la plateforme de l’entrée |
| `--jadx PATH` | Environnement/PATH | Lanceur du backend ou JAR de la distribution ; l’option explicite est prioritaire |
| `--python PATH` | Environnement/PATH | Interpréteur de l’outil auxiliaire fourni ; l’option explicite est prioritaire |
| `--timeout N` | `300` | Nombre positif de secondes par processus backend, y compris la détection de version |
| `--max-files N` | `20000` | Limite positive du nombre d’entrées, répertoires créés compris |
| `--max-bytes N` | `2147483648` | Limite positive en octets pour l’entrée, les données extraites et la sortie finale |
| `--json` | Désactivé | Afficher le rapport en JSON plutôt que sous forme de résumé destiné à l’utilisateur |

Une valeur de `--arch` différente de celle par défaut, `--artifact`, `--metadata-only` et une valeur non nulle de `--max-func` concernent iOS et sont refusés pour Android ; `--arch=auto` reste accepté lorsqu’il est indiqué explicitement. Il n’existe pas de transmission libre d’options au backend. Les répertoires de configuration, de cache et de fichiers temporaires du backend sont isolés pour chaque exécution ; les réglages ambiants et la configuration des plugins ne sont pas importés dans le traitement.

Les limites contrôlent les ressources, sans constituer un bac à sable pour le processus backend. L’espace de travail temporaire est lui aussi surveillé, avec une marge pour l’entrée, les données extraites et la sortie allant jusqu’à trois fois les budgets configurés en entrées et en octets. Les journaux sont plafonnés à 16 MiB par processus. Les entrées volumineuses peuvent malgré tout nécessiter davantage de mémoire de tas Java ou un délai plus long ; augmenter une limite ne désactive pas les autres.

## Organisation de la sortie et rapport JSON

```text
recovered-app/
  sources/                 paquets et classes Java reconstruits
  logs/jadx-version.log    détection de version du backend
  logs/jadx.log            diagnostics du backend
  report.json              inventaire versionné et limites de reconstruction
```

Les copies temporaires et les caches du backend sont supprimés. Les noms exacts des fichiers Java et leur nombre dépendent de la reconstruction effectuée par le backend ; les classes imbriquées peuvent partager le fichier source de leur classe englobante. Le nombre de sources Java n’est donc pas le nombre de classes DEX.

Exemple de rapport abrégé :

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {"name": "jadx", "version": "1.5.6"},
  "input_code_files": ["classes.dex", "classes2.dex"],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": ["sources/example/Main.java", "sources/example/Peer.java"],
  "logs": ["logs/jadx-version.log", "logs/jadx.log"],
  "limitations": ["Java is reconstructed from bytecode; original comments, formatting, and stripped names cannot be restored."]
}
```

`input_kind` vaut `apk`, `dex`, `smali` ou `smali-directory`. `input_code_files` répertorie les noms de bytecode ou les chemins smali en entrée, tandis que `java_sources` et `logs` sont relatifs à la racine de sortie. `source` est le nom de base de l’entrée. Les rapports réels contiennent d’autres limites de reconstruction ; conservez-les lorsque vous transmettez les résultats à d’autres outils.

Pour l’automatisation, vérifiez le code de sortie du processus avant d’exploiter `status` et, si vous redirigez stdout, placez le rapport hors du nouveau répertoire de sortie :

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

Les exécutions réussies de l’outil auxiliaire renvoient zéro. Les échecs de reconstruction renvoient une valeur non nulle ; dès que la gestion des erreurs de l’outil auxiliaire est active, `--json` produit un objet d’erreur contenant `schema_version`, `status: "error"` et `error`. L’analyse native des arguments, l’absence de Python, une version de Python antérieure à 3.10 ou l’absence de l’outil auxiliaire peuvent échouer plus tôt et produire un message sur stderr plutôt que du JSON. Une interruption peut aussi être signalée sur stderr. Les outils consommateurs doivent gérer ces cas.

## Gestion des échecs et dépannage

La publication est transactionnelle : la sortie existante est préservée et les données de travail d’une exécution échouée sont supprimées. Une sortie non nulle du backend, des erreurs d’assemblage ou de décompilation dans les journaux, des classes omises à cause de doublons, des marqueurs explicites de code incomplet, des fichiers Java vides ou l’absence de Java font tous échouer la commande. Un succès signalé par le backend ne prouve pas à lui seul la correction de chaque méthode.

| Symptôme | Action |
|----------|--------|
| Python ou outil auxiliaire absent | Installer ou sélectionner Python 3.10+ et conserver le répertoire `mobile/` à côté de NeverD |
| Backend inexécutable ou version non prise en charge | Vérifier `--jadx`, la structure complète de la distribution, Java et la version minimale du backend |
| En-tête DEX invalide / aucun DEX à la racine | Vérifier le véritable format d’entrée ; utiliser un APK contenant du code, un DEX ordinaire ou du smali |
| Aucun fichier smali | Désigner un répertoire contenant des `.smali`, pas des sources Java ni une arborescence contenant seulement des assets |
| Classe en double ou reconstruction partielle | Supprimer les définitions en double ou analyser séparément l’ensemble de bytecode pertinent ; corriger le smali mal formé au lieu d’accepter un résultat incomplet |
| Délai dépassé / limite d’octets ou d’entrées | Réduire l’entrée aux éléments utiles ou augmenter délibérément la limite correspondante |
| Chemin d’archive ou lien dangereux | Recréer une entrée régulière et portable, sans traversée de répertoires, liens, fichiers spéciaux ni chemins en conflit |
| La sortie existe déjà | Choisir un autre répertoire de sortie ; ne pas réutiliser celui d’une exécution réussie précédente |

Les exécutions réussies conservent les journaux du backend. Les répertoires de travail des exécutions échouées, journaux compris, sont supprimés. Une sortie non nulle du backend inclut un extrait borné de la fin des diagnostics dans l’erreur ; les dépassements de délai ou de budget de ressources produisent leurs propres messages. Pour une investigation propre au backend, reproduisez le problème sur une entrée isolée, avec sa propre CLI et un répertoire de diagnostic distinct. Ne déduisez jamais la réussite de la seule apparition de fichiers Java avant un échec.

## Vérification et profondeur de prise en charge

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

Les deux premières commandes vérifient les composants mobiles et les contrats de la CLI compilée ; les exigences de certaines données de test propres à une plateforme peuvent entraîner des exclusions explicites. Le test du backend réel nécessite aussi un JDK (`java` et `javac`). Il construit des cas à fichier smali unique, à références entre classes et classes imbriquées, en DEX et en véritables APK multidex, puis compile et exécute le Java reconstruit. Les cas couvrent les branches, boucles, tableaux, exceptions, références de classes, entrées mal formées et omissions de classes en double. Ils apportent des preuves pour ces données de test, sans promettre une reconstruction complète d’applications quelconques.

Le [workflow Mobile Decompilation](../.github/workflows/mobile.yml) exécute les tests de composants sous Linux, macOS et Windows avec Python 3.10 et 3.13, ainsi qu’un test du véritable backend Android sous Linux dont la distribution est figée par somme de contrôle. Consultez la [vue d’ensemble mobile](mobile.md) pour le traitement iOS distinct et ses limites actuelles.
