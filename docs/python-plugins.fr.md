**Langues** : [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← Index de la documentation](README.fr.md)

# Plugins Python

NeverD peut charger un fichier Python comme un plugin de premier ordre. Les plugins Python partagent avec les plugins natifs les mêmes métadonnées, le même cycle de vie, le même ordre, les mêmes règles de noms dupliqués, le même flux d’événements et la même ABI C de session. Le paquet de développement pris en charge est `neverd-plugin` ; n’importez pas directement le pont privé `_neverd_plugin`.

## Prérequis de compilation et d’exécution

`NEVERD_ENABLE_PYTHON_PLUGINS` vaut `ON` par défaut. Une compilation activée requiert un interpréteur CPython 3.10 ou ultérieur ainsi que sa bibliothèque de développement pour l’intégration, détectables par CMake :

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

Utilisez `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` pour produire un `libneverd` uniquement natif, sans dépendance de liaison à CPython. Une compilation avec Python place le paquet correspondant et les exemples dans `build/bin/sdk/python/` ; ce répertoire s’installe aussi directement avec `python3 -m pip install build/bin/sdk/python`.

## Écrire un plugin

Un module déclare exactement une classe décorée :

```python
from neverd_plugin import Event, Plugin, PluginType, Session


@Plugin(
    name="Analysis Report",
    version="1.0.0",
    author="Your team",
    description="Reports basic information about the loaded binary",
    type=PluginType.PROCESSOR,
)
class AnalysisReport:
    def on_init(self, session: Session) -> int | None:
        print(session.architecture)
        return None

    def on_run(self, session: Session, arg: int) -> int | None:
        print(session.file_path, session.function_count)
        return 0

    def on_event(self, event: Event) -> int | None:
        print(event.type.name)
        return None

    def on_term(self) -> None:
        pass
```

Tous les hooks sont facultatifs. `None` signifie que l’opération a réussi ; un résultat entier doit tenir dans un `int` C. Les versions des métadonnées suivent strictement SemVer. Les noms doivent être des chaînes UTF-8 non vides et toute métadonnée contenant un caractère NUL interne est rejetée.

Le dépôt fournit les exemples [`minimal.py`](../pluginsdk/python/examples/minimal.py), [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py) et [`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py), qui illustre les API d’optimisation soumises à preuve.

## Charger et inspecter les plugins

L’API C peut charger de façon déterministe un fichier `.py` précis ou parcourir un répertoire :

```c
if (!neverd_plugins_load_file(session, "plugins/report.py")) {
  const char *message = neverd_last_error(session);
  /* log message */
  neverd_free_string(message);
}

neverd_plugins_init(session);
int result = neverd_plugins_run(session, "Analysis Report", 0);
neverd_plugins_term(session);
```

`neverd_plugins_list_json` identifie chaque élément avec `"kind":"python"` ou `"kind":"native"`. La découverte de répertoire est triée par chemin canonique et accepte à la fois les bibliothèques natives et les fichiers Python dans le même répertoire. Les chemins canoniques et noms de plugins dupliqués sont des erreurs.

## API de session et d’événements

`Session` revalide les capacités de l’hôte avant chaque appel C. Son interface typée couvre les métadonnées de fichier, d’architecture et de format, la largeur en bits et les nombres de tables, les vues de fonctions, le chargement et l’analyse, la lecture d’octets, le désassemblage, la décompilation et les requêtes usuelles. Pour les opérations avancées, `session.raw` expose toutes les déclarations de `neverd_plugin.abi` :

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### Exploration symbolique bornée des chemins

Pour les fonctions LowIR natives, `session.symbolic_explore` renvoie des résultats de chemin typés, la trace des blocs de base, l’utilisation des ressources et, facultativement, les prédicats de chemin :

```python
result = session.symbolic_explore(
    0x401000,
    max_paths=64,
    max_steps=1 << 16,
    max_block_visits=3,
    include_expressions=True,
)
if not result.exact:
    print(result.unmodelled_ops)
for path in result.paths:
    print(path.outcome, path.blocks, path.predicate)
```

`complete` vaut false lorsqu’une limite de chemins, d’étapes, de visites de boucle ou de branche non résolue arrête le parcours. `exact` exige en outre qu’aucune opération n’ait été remplacée de façon conservatrice par un état inconnu ; les opérations LowIR non prises en charge, les appels sans résumé et les écritures via une adresse non résolue sont comptés dans `unmodelled_ops`. Les sessions EVM et SBF n’exposent pas l’exploration LowIR native.

### Inversions concoliques vérifiées de branches LowIR

`session.lowir_concolic` suit un chemin LowIR natif depuis des plages d’octets explicites du registre d’entrée et ne renvoie que les candidats du solveur qu’une nouvelle exécution vérifie à la même occurrence de décision de contrôle :

```python
from neverd_plugin import ConcolicRegisterSeed

report = session.lowir_concolic(
    0x401000,
    [ConcolicRegisterSeed(offset=56, bytes=4, value=0)],
)
for flip in report.flips:
    if flip.candidate_id is not None:
        print(report.candidates[flip.candidate_id].seed)
```

Le décalage de registre est un décalage en octets dans le fichier de registres de NeverD, pas un pointeur natif ni un numéro de registre. Le rapport reste toujours non exhaustif ; UNSAT, les limites du solveur et les refus de projection ou de réexécution sont des résultats typés, pas des exceptions.

### Audit et chasse de sûreté mémoire

`session.audit()` et `session.hunt()` renvoient des rapports JSON analysés (même schéma que le CLI). Ils exigent une session native levée :

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

Les sessions EVM et SBF rejettent ces appels.

Les six variantes d’événement immuables sont `BINARY_LOADED`, `BINARY_CLOSING`, `FUNCTION_SELECTED`, `ADDRESS_CHANGED`, `ANALYSIS_DONE` et `PATCH_APPLIED`. Les chaînes du payload sont copiées pendant le callback ; les champs sans rapport avec la variante valent `None`.

Ne conservez jamais une `Session` pour l’utiliser après la terminaison. La capsule native est invalidée avant le début de `on_term` et avant que la session native puisse être libérée. Un appel ultérieur échoue avec `RuntimeError` au lieu de déréférencer une mémoire périmée.

### Publication stricte du sanitizer binaire

`session.sanitize()` exécute la transaction expérimentale tout-ou-refus `binary-sanitizer-v1` et ne renvoie un `SanitizeResult` immuable qu’après validation d’un receipt authentifié, complet et cohérent. Les hôtes non Darwin refusent avant le lifting, la génération des gardes, la création du candidat ou la mutation du namespace. `PUBLISH_INDETERMINATE` et `PUBLISHED_INCOMPLETE` sont des échecs : la destination peut exister et doit être inspectée avant usage ou nouvelle tentative.

Un receipt Darwin complet authentifie seulement l’objet répertoire de destination conservé pendant la transaction. Comme il peut être renommé après son ouverture, le receipt ne garantit pas que le pathname d’origine continue de désigner l’objet pendant ou après le retour et n’est pas une liaison de chemin durable et vérifiable indépendamment. Le code qui rouvre ensuite le chemin doit conserver une ancre externe et réauthentifier l’objet. Python ne fournit actuellement aucun rejeu natif de processus complet ; `NativeProcessReplayAdapter` est uniquement une limite C++ fail-closed de disponibilité/factory Phase 0, où tous les hôtes renvoient toutes les capacités à false et aucune table d’opérations.

### Synthèse contrôlée par preuve et optimisation LLVM

`synthesize_expression` est distinct de `simplify_expression`, conservé pour
la compatibilité ABI et limité au MBA. Une réécriture n’est validée que si le
solveur renvoie `ProofStatus.EQUIVALENT`. Un contre-exemple, une preuve
incomplète ou un budget de recherche épuisé conserve l’expression d’origine et
rapporte séparément le résultat ainsi que le travail de recherche et de preuve.
`ProofStatus.INVALID` signale une question de preuve mal formée et reste
distinct de `ProofStatus.UNKNOWN`, dû au budget ; tous deux refusent la
réécriture de façon sûre.

`optimize_llvm_ir` combine le point fixe sémantique de NeverD et le pipeline
LLVM standard choisi sur une copie transactionnelle, puis ne renvoie que le
module validé et engagé :

```python
from neverd_plugin import (
    LLVMOptimizationLevel,
    OptimizationMode,
    ProofStatus,
    optimize_llvm_ir,
    synthesize_expression,
)

rewrite = synthesize_expression(
    "(x >> 4) + ((x >> 2) >> 2)", exhaustive=True
)
if rewrite.changed:
    assert rewrite.proof_status is ProofStatus.EQUIVALENT

module = optimize_llvm_ir(
    llvm_ir,
    mode=OptimizationMode.DEEP,
    llvm_level=LLVMOptimizationLevel.O2,
    enable_synthesis=True,
    exhaustive=True,
)
print(module.output_ir, module.semantic_rewrites, module.proof_queries)
```

Les appels de production peuvent borner séparément le travail et l’arité MBA,
la recherche de synthèse et le travail SAT, ainsi que la convergence LLVM. Pour
`simplify_expression`, `exhaustive=True` sélectionne la politique MBA sans
plafond d’arité ni de travail et retire les plafonds d’imbrication et de largeur
du parseur natif. Pour `synthesize_expression`, il retire les plafonds du
parseur, du travail de recherche et de SAT tout en conservant la grammaire
définie par l’appelant ; pour `optimize_llvm_ir`, il retire les plafonds de
convergence, de recherche et de SAT. Python n’ajoute aucune autre limite
d’expression ; les bornes de sûreté mémoire et de représentation IR restent
applicables. Les points d’entrée C sont `neverd_simplify_expr`,
`neverd_synthesize_expr` et `neverd_optimize_llvm_ir`, avec des fonctions de
libération typées et des adaptateurs JSON versionnés.

## Erreurs, isolation et confiance

Les exceptions Python ne traversent jamais C++ lors du déroulement de pile. NeverD capture le traceback complet mis en forme et l’expose via `neverd_last_error`. Chaque chemin canonique de plugin est chargé sous un nom de module unique ; la terminaison supprime le module et un rechargement ultérieur repart d’un état neuf pour le module et la classe. CPython est initialisé une seule fois, le GIL d’amorçage est libéré et les callbacks acquièrent le GIL sur tout thread hôte. NeverD ne finalise jamais un interpréteur susceptible d’être partagé avec un autre composant.

Les plugins exécutent du Python arbitraire dans le processus NeverD et peuvent appeler l’ensemble de l’API C. Ne chargez que des fichiers de confiance. Il s’agit d’une frontière d’extension, pas d’une sandbox.

## Développement, tests et paquets

Pour l’assistance de l’éditeur et du vérificateur de types, installez le paquet Python pur ou ajoutez l’arborescence source à `PYTHONPATH` :

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

L’audit exige une correspondance exacte entre chaque déclaration C exportée et sa signature `ctypes` ainsi que sa règle de propriété. Il vérifie également les valeurs de langue de sortie, les versions CMake et du paquet, les options de fonctionnalité CI, les versions figées des Actions, le flux des artefacts et la politique OIDC de PyPI. Les tests de l’adaptateur natif sont `NeverDPluginRuntimeTests` ; ceux de Python intégré sont `NeverDPythonRuntimeTests` et `NeverDPythonPluginTests`.

Le workflow `Python Plugin SDK` construit un wheel et une distribution source, installe les deux dans des environnements propres puis téléverse les artefacts vérifiés. La publication n’a lieu que pour une GitHub Release publiée, via l’environment `pypi` soumis à approbation et Trusted Publishing ; aucun token PyPI de longue durée n’est utilisé.
