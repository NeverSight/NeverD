**Langues** : [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← Index de la documentation](README.md)

# Backends de preuve bitvector

Par défaut, NeverD utilise son solveur bitvector intégré. La dérivation MBA exacte reste indépendante d’un solveur général. La synthèse d’expressions n’accepte un candidat qu’après preuve d’équivalence ; un contre-exemple ou une requête indécise conserve l’expression d’origine.

Le circuit de multiplication intégré retire les bits de poids faible connus nuls des deux facteurs, construit le produit restant, puis restitue le mot de sortie complet. L’adaptateur Z3 optionnel encode aussi les produits à coefficient constant pair à la largeur exacte qu’ils observent. Les deux conservent les requêtes d’égalité sur toute la largeur, les modèles d’entrée indépendants et les limites de ressources existantes. Les optimisations de source CLI et GUI partagent le circuit intégré sans option supplémentaire.

<!-- i18n-section: builtin-comparisons -->

## Circuits de comparaison intégrés

Au-delà de huit bits, les comparaisons examinent d’abord les moitiés hautes et utilisent les moitiés basses si les premières sont égales. Les petits fragments utilisent les retenues de soustraction. L’encodeur partagé peut réutiliser les portes du préfixe haut lorsqu’une mise à jour partielle de registre ne change que les bits bas. Les comparaisons signées inversent toujours les deux bits de signe. La sémantique des expressions et les limites de ressources restent inchangées ; leur épuisement renvoie toujours `Unknown`.

`BitBlaster.WidePredicatesAgreeWithTheEvaluator` confronte les prédicats signés et non signés à l’évaluateur pour des largeurs sélectionnées de 8 à 256 bits, avec des largeurs impaires, des valeurs voisines des frontières et des bits significatifs au-delà de 64 bits. Il exclut aussi les sorties incorrectes. Les tests de compteurs partiels vérifient les requêtes complètes, les modèles de contre-exemple et l’épuisement du budget de portes. Les régressions natives avec comparaison mémorisée prouvent les deux ordres de comparaison et de mise à jour en observant tous les registres et indicateurs, et rejettent un corps de boucle original modifié.

<!-- i18n-section: pristine-encoding -->

## Copies de l’encodage avant recherche

`BitVectorSolver::cloneEncoding()` copie un encodage complet avant toute tentative de recherche SAT. La méthode renvoie null après une recherche ou un échec d’encodage. Chaque copie possède ses clauses modifiables, sa propagation à la racine, ses portes et ses correspondances de bits ; elle conserve l’ordre des variables, le décompte des portes et les paramètres du solveur. Le contexte doit survivre aux deux solveurs ; le solveur source peut être modifié ou détruit indépendamment.

Le moteur SAT conserve quatre entrées de surveillance dans chaque liste de littéral ; les listes plus longues grandissent dynamiquement. Cela évite des allocations séparées pour les listes courtes lors de la construction, de la copie avant recherche et de la destruction. L’ordre de propagation, les clauses, la propriété indépendante et tous les plafonds de travail restent inchangés.

<!-- i18n-section: context-finite-proofs -->

## Preuves terminées dans un même contexte

Le vérificateur natif d’indépendance lie son cache de domaines finis à son contexte symbolique réel, où seuls des nœuds sont ajoutés. Les clés compactes conservent le prédicat exact, les projections ordonnées et la limite de valeurs. Seuls les domaines numériques complets ou la non-unicité prouvée sont réutilisables ; chaque échec du cache exige encore l’énumération complète et la preuve finale d’exclusion. Le contexte et le sens des nœuds existants doivent rester stables pendant la vie du cache. Un jeton préparé ne passe ni à un autre propriétaire ni à son remplaçant à la même adresse. Le plafond de stockage en mots reste inchangé. Les autres utilisateurs conservent les clés structurelles et le renommage des variables.

<!-- i18n-section: z3-build -->

## Build Z3 facultatif

Lorsque Z3 est activé, CMake télécharge avec `FetchContent` la révision source figée 4.13.3 et construit une bibliothèque statique avec NeverD. Aucune installation système de Z3 n’est requise :

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
python scripts/build.py build-release --parallel 4 --target neverd \
  NeverDSolverTests NeverDSymbolicTests neverd-solver-bench
```

`NEVERD_Z3_PROVIDER=FETCH` est le défaut. La première configuration télécharge les sources ; les suivantes réutilisent `_deps`. La CLI Z3, ses tests, exemples, documentation et bindings ne sont pas construits. Ses générateurs Python utilisent l’interpréteur déjà requis par NeverD. Pour une bibliothèque installée, choisir `-DNEVERD_Z3_PROVIDER=SYSTEM` et éventuellement `-DZ3_ROOT=/path/to/prefix` ; l’absence des fichiers de développement échoue sans changement implicite de fournisseur. Hors ligne, fournir un checkout local via `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3`.

Avec `NEVERD_ENABLE_Z3=OFF` (défaut), NeverD ne télécharge, ne recherche ni ne lie Z3. Une demande runtime explicite d’un backend indisponible échoue sans fallback.

<!-- i18n-section: synthesis -->

## Synthèse d’expressions soumise à preuve

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

`--solver=builtin` est le défaut. Le choix du solveur exige `--synthesize` ; le simplificateur MBA ordinaire n’appelle pas Z3. Le timeout Z3 s’applique à chaque vérification, hors traduction d’expression. L’annulation est coopérative, pas une limite stricte de temps réel. Zéro sélectionne 1000 ms ; `--exhaustive` supprime la limite. Les limites de conflits SAT, de propagation et de visites de watch concernent uniquement le backend intégré et sont rejetées avec Z3. Les vérifications Z3 incrémentent le compteur de requêtes de preuve ; les compteurs de travail SAT intégrés restent à zéro.

L’API C ajoute `solver_backend` et `solver_timeout_ms` à `neverd_synthesize_options`. Les lecteurs limités par taille conservent le backend intégré pour les anciens clients. `neverd_solver_backend_available()` indique la capacité de build ; Python propose le même choix via `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)`. Ce choix couvre actuellement la synthèse d’expressions uniquement. L’exécution concolique, l’analyse de sûreté et les valeurs par défaut de l’optimisation IR gardent leurs politiques actuelles. Les utilisateurs internes peuvent injecter le vérificateur Z3 au callback de preuve du simplificateur sémantique.

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## Vérifications indépendantes et export des requêtes

Avec Z3 activé, `NeverDSolverTests` comprend une évaluation indépendante des expressions et une comparaison inter-backends. Les expressions de référence sont construites directement, afin qu’un bug des constructeurs NeverD ne simplifie pas les deux côtés du test. Sans Z3, ces oracles sont explicitement ignorés, mais le contrat d’indisponibilité reste testé.

Les tests de produits pondérés comparent le circuit intégré à une multiplication longue indépendante et à l’arithmétique entière jusqu’à 257 bits. Les tests Z3 prouvent l’accord avec un encodage indépendant sur toute la largeur et vérifient les bits d’entrée hauts ainsi qu’un bit de sortie le plus significatif volontairement erroné.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

Chaque ligne non commentée suit `original ; candidate` ; sans point-virgule, le résultat du simplificateur MBA sert de candidat. L’outil rapporte verdicts, rejeu de modèles et durées (construction de session, traduction et résolution incluses ; parsing, simplification MBA, export et destruction exclus). Chaque répétition crée un solveur neuf. Les modèles SAT doivent reproduire la différence dans l’évaluateur d’expressions. Des verdicts décisifs opposés ou des requêtes/modèles invalides font échouer l’exécution ; `unknown` est enregistré et ne prouve pas l’équivalence.

Le SMT-LIB exporté comprend le DAG encodé équivalent, assertions permanentes et hypothèses de la dernière requête ; il se rejoue avec `z3 query-N.smt2`. Consigner limites de ressources et versions : les backends ont des unités de budget différentes, donc il s’agit de charges bornées, pas d’un travail identique. Les preuves bitvector utilisent la sémantique totale à largeur fixe du langage d’expressions. Exceptions machine, effets mémoire et LLVM poison restent du ressort des frontières de lifting/traduction ; une preuve d’expression ne les certifie pas.

Une copie de l’encodage avant recherche retire de sa propre file de décision les variables affectées à la racine et reconstruit l’ordre strict du tas par activité puis indice. Les affectations et clauses restent intactes ; les faits racine survivent à tout retour arrière. La propriété de la source, les décisions effectives, les modèles complets et tous les budgets de recherche restent inchangés.

Le DAG exporté en SMT-LIB est un encodage sémantiquement équivalent, pouvant contenir le quotient exact décrit ci-dessus ; sa structure peut différer du DAG d’origine.
