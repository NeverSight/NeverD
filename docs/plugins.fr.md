**Langues**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← Index de la documentation](README.fr.md)

# Plugins natifs

Les plugins natifs NeverD sont des bibliothèques partagées de confiance,
chargées dans le processus hôte. Ils utilisent les déclarations en C pur de
`neverd/sdk/NeverDPlugin.h` et appellent l’API C publique de
`neverd/sdk/NeverDCAPI.h`. Utilisez le
[guide des plugins Python](python-plugins.fr.md) lorsque le développement en
Python dans le même processus est plus approprié.

## Compatibilité et frontière de confiance

Le descripteur `neverd_plugin_t` actuel ne comporte ni champ de version d’ABI,
ni champ de taille de structure. Compilez un plugin avec les en-têtes produits
par la révision exacte de NeverD qui le chargera, puis recompilez-le à chaque
mise à niveau de NeverD. Le plugin et l’hôte doivent également utiliser le même
système d’exploitation, la même architecture et des chaînes d’outils
compatibles avec l’ABI.

Les plugins natifs sont du code arbitraire exécuté dans le processus. NeverD ne
les place pas dans une sandbox, n’isole pas leurs plantages et ne limite pas
leur accès à la session ou au processus hôte. Ne chargez que des plugins
auxquels vous faites confiance.

## Descripteur et callbacks

Chaque bibliothèque exporte un unique symbole de données nommé exactement
`neverd_plugin` :

```c
#include "neverd/sdk/NeverDPlugin.h"

static int on_init(neverd_session_t session) {
  (void)session;
  return 0;
}

static void on_term(void) {}

static int on_run(neverd_session_t session, int arg) {
  (void)session;
  return arg;
}

static int on_event(const neverd_event_t *event) {
  if (event && event->Type == NEVERD_EVT_BINARY_LOADED) {
    const char *path = event->Data.BinaryLoaded.Path; /* borrowed */
    (void)path;
  }
  return 0;
}

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "My Plugin",
    .Version = "1.0.0",
    .Author = "Your Name",
    .Description = "A native NeverD extension",
    .Type = NEVERD_PLUGIN_GENERIC,
    .Init = on_init,
    .Term = on_term,
    .Run = on_run,
    .Event = on_event,
};
```

`NEVERD_PLUGIN_EXPORT` se développe en `__declspec(dllexport)` sous Windows et
en visibilité ELF/Mach-O par défaut ailleurs. Conservez le source en C ou
attribuez explicitement une liaison C au descripteur si une implémentation C++
est inévitable.

`Name` ne doit pas être vide et doit être unique dans un même hôte. L’hôte
copie les quatre chaînes de métadonnées lors du chargement. `Version`, `Author`
et `Description` peuvent être vides. Les quatre valeurs de type ne sont que des
métadonnées de classification :

| Valeur | Signification actuelle |
|--------|-------------------------|
| `NEVERD_PLUGIN_GENERIC` | Étiquette d’extension générale |
| `NEVERD_PLUGIN_LOADER` | Étiquette de loader ; elle n’enregistre aucun chargeur de binaire |
| `NEVERD_PLUGIN_PROCESSOR` | Étiquette d’analyse/traitement ; elle ne planifie aucun travail |
| `NEVERD_PLUGIN_UI` | Étiquette d’interface ; NeverD ne fournit aucun hôte GUI pour les plugins natifs |

Tous les pointeurs de callback sont facultatifs. Les appels sont directs et
synchrones sur le thread de l’appelant hôte.

| Callback | Contrat |
|----------|---------|
| `Init(session)` | Retourner `0` en cas de succès. Un résultat non nul enregistre une erreur ; `Term` ne sera pas appelé après cet échec d’initialisation. |
| `Term()` | Appelé pendant la finalisation uniquement après un `Init` réussi. La bibliothèque est ensuite déchargée. |
| `Run(session, arg)` | Effectuer le travail du plugin et retourner un entier. La CLI transmet `0` ; l’API C d’intégration peut choisir un autre argument. Un callback absent retourne `-1`. |
| `Event(event)` | Traiter un événement distribué par l’hôte. Retourner `0` en cas de succès ; un résultat non nul enregistre une erreur. Un callback absent ne fait rien. |

L’ordre normal d’intégration est le chargement, l’initialisation, l’exécution ou
la distribution d’événements, puis la finalisation. L’API C n’impose pas cet
ordre pour `Run` ou `Event` ; l’hôte d’intégration est donc responsable du cycle
de vie.

## Les événements sont distribués par l’hôte

`neverd_event_t` porte l’une des six valeurs d’événement :

| Événement | Charge utile dans `event->Data` |
|-----------|---------------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | Aucune charge utile |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | Aucune charge utile |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

Les événements ne sont émis automatiquement ni par les API de session, ni par
l’outil en ligne de commande `neverd`. Un hôte d’intégration construit et
distribue l’événement :

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

L’événement et les chaînes de sa charge utile sont empruntés jusqu’au retour du
callback ; ne les libérez pas et ne les conservez pas. Le handle de session
appartient à l’hôte : un plugin ne doit ni le détruire, ni l’utiliser après la
finalisation de l’hôte. Les résultats alloués par l’API C de NeverD appartiennent
à NeverD et doivent être libérés avec `neverd_free_string()`. NeverD ne garantit
pas l’accès concurrent aux callbacks ou à la session : les hôtes d’intégration
doivent sérialiser les appels de cycle de vie, d’exécution et d’événement, sauf
s’ils fournissent leur propre synchronisation sûre.

## Compiler l’exemple fourni

Activez explicitement les exemples ; la valeur par défaut reste
`NEVERD_BUILD_PLUGINS=OFF` :

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

Pour un générateur à configuration unique, les artefacts pertinents sont :

| Artefact | Chemin |
|----------|--------|
| CLI | `build/bin/neverd` (`neverd.exe` sous Windows) |
| Bibliothèque hôte | `build/bin/libneverd.so`, `build/bin/libneverd.dylib` ou `build/bin/neverd.dll` |
| En-têtes produits | `build/bin/sdk/neverd/sdk/` |
| Exemple | `build/bin/plugins/example_plugin.so`, `.dylib` ou `.dll` |

Une compilation multiconfiguration place les mêmes artefacts d’exécution sous
la configuration choisie, par exemple `build/bin/Release/`, avec le plugin dans
`build/bin/Release/plugins/` et les en-têtes dans `build/bin/Release/sdk/` :

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## Compiler un plugin autonome

NeverD n’installe pas encore son SDK et ne fournit pas de
`NeverDConfig.cmake` ; il n’existe donc aucun `find_package(NeverD)` pris en
charge. Pointez une compilation autonome vers les en-têtes produits et vers
une bibliothèque de liaison explicite provenant exactement de la même
compilation de l’hôte :

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_neverd_plugin LANGUAGES C)

set(NEVERD_SDK_ROOT "" CACHE PATH
    "Directory containing neverd/sdk/NeverDPlugin.h")
set(NEVERD_LINK_LIBRARY "" CACHE FILEPATH
    "libneverd.so, libneverd.dylib, or the Windows neverd.lib import library")

if(NOT EXISTS "${NEVERD_SDK_ROOT}/neverd/sdk/NeverDPlugin.h")
  message(FATAL_ERROR "Set NEVERD_SDK_ROOT to the staged NeverD SDK")
endif()
if(NOT EXISTS "${NEVERD_LINK_LIBRARY}")
  message(FATAL_ERROR "Set NEVERD_LINK_LIBRARY to the matching libneverd")
endif()

add_library(my_plugin SHARED my_plugin.c)
target_include_directories(my_plugin PRIVATE "${NEVERD_SDK_ROOT}")
target_link_libraries(my_plugin PRIVATE "${NEVERD_LINK_LIBRARY}")
set_target_properties(my_plugin PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  PREFIX "")
```

Configurez avec `NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk` (ou le
répertoire `sdk` propre à la configuration). Sous Linux/macOS,
`NEVERD_LINK_LIBRARY` est le `.so`/`.dylib` correspondant. Sous Windows, il
s’agit de la bibliothèque d’importation `neverd.lib` produite par le générateur,
tandis que le `neverd.dll` correspondant doit rester auprès de l’exécutable
hôte. L’emplacement des bibliothèques d’importation dépend du générateur ;
transmettez donc explicitement le fichier réel.

## Découverte et visite guidée de la CLI

La CLI parcourt les répertoires dans cet ordre ; les premiers chemins
canoniques et noms de plugin l’emportent :

1. `plugins` à côté de l’exécutable `neverd` en cours d’exécution.
2. `$HOME/.neverd/plugins` (`HOME` est utilisé lorsqu’il n’est pas vide ; sous Windows, le répertoire de profil natif sert de repli).
3. Chaque entrée non vide de `NEVERD_PLUGIN_PATH`, dans l’ordre.
4. Le répertoire fourni par `--plugin-dir`.

`NEVERD_PLUGIN_PATH` sépare les entrées par `:` sous Linux/macOS et par `;`
sous Windows. Les répertoires canoniquement équivalents ne sont parcourus
qu’une fois. Pour les bibliothèques natives, seul le suffixe de l’hôte est pris
en compte : `.so` sous Linux, `.dylib` sous macOS et `.dll` sous Windows. Les
compilations avec Python activé parcourent aussi les fichiers `.py`.

L’exemple de l’arborescence de compilation se trouve déjà dans le répertoire
voisin de l’exécutable :

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

Pour une compilation multiconfiguration, remplacez `build/bin` par
`build/bin/Release`. Pour installer une copie destinée à un exécutable NeverD
qui ne possède pas déjà le même plugin à ses côtés, utilisez la commande de la
plateforme hôte, puis lancez cet exécutable avec les mêmes options `--list` et
`--run` :

```bash
# Linux
mkdir -p "$HOME/.neverd/plugins"
cp build/bin/plugins/example_plugin.so "$HOME/.neverd/plugins/"

# macOS
mkdir -p "$HOME/.neverd/plugins"
cp build/bin/plugins/example_plugin.dylib "$HOME/.neverd/plugins/"

# Windows PowerShell
New-Item -ItemType Directory -Force "$HOME/.neverd/plugins"
Copy-Item build/bin/plugins/example_plugin.dll "$HOME/.neverd/plugins/"
```

Les répertoires facultatifs absents auprès de l’exécutable ou dans le profil
sont ignorés. Un plugin mal formé dans un répertoire facultatif produit un
avertissement et le parcours continue. Chaque répertoire nommé par
`NEVERD_PLUGIN_PATH` ou `--plugin-dir` est obligatoire : un répertoire absent
ou un candidat rejeté fait terminer la CLI avec un code non nul. Les fichiers
canoniques en double, les noms de plugin en double, l’absence de l’export
`neverd_plugin` et les types de descripteur invalides sont rejetés. Un appel
direct à `neverd_plugins_load_file` rejette également les fichiers dont le
suffixe n’est pas pris en charge.

Les hôtes d’intégration doivent vérifier à la fois les résultats de gestion des
plugins et `neverd_last_error(session)`. `neverd_plugins_load_file` retourne `1`
ou `0` ; `neverd_plugins_load_dir` retourne le nombre de plugins chargés et peut
signaler des candidats rejetés même après un succès partiel.
`neverd_plugins_run` retourne le résultat du plugin et utilise `-1` lorsque le
plugin est absent ou ne peut pas s’exécuter. Libérez avec
`neverd_free_string()` toute chaîne d’erreur ou JSON retournée par l’API C.
