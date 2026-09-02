**Langues**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Index de la documentation](README.fr.md)

# Audit et chasse de sûreté mémoire

NeverD analyse un binaire chargé selon deux familles de défauts de sûreté mémoire et les rapporte en JSON structuré. Les deux pistes s’exécutent sur l’IR levé, indépendant du format, donc **PE/COFF, ELF et Mach-O sont des cibles de premier rang, à égalité** : une découverte n’est jamais conditionnée à un scanner ou une table d’imports d’un seul format.

| Piste | Commande | Rapporte |
|-------|----------|----------|
| **Audit** | `neverd audit <binary>` | Défauts de durée de vie du tas et lectures locales de pile non initialisées |
| **Hunt** | `neverd hunt <binary>` | Débordements de copies dangereuses avec preuves symboliques et valeurs candidates ; `replayable=true` uniquement avec un plan `process-input-v1` complet |

Le moteur réutilise l’exécution symbolique et le solveur bitvector internes de NeverD pour les témoins et l’atteignabilité. Aucun solveur externe, VM ou conteneur.

---

## Invariant central : échec fermé

Une opération non levée, un appel dont l’ABI n’a pas recouvré les arguments, une cible indirecte non résolue ou un budget épuisé donnent **UNKNOWN**, jamais SAFE. Une destination dont la capacité n’est pas recouvrable est UNKNOWN. Le lifting strict reste inchangé ; la couche de sûreté n’ajoute que des verdicts conservateurs par-dessus.

Les effets d’appel suivent une sémantique en monde fermé : un résumé ne s’applique que si ses préconditions et tous les effets pertinents sont connus. Un effet inconnu ou un résumé seulement partiellement applicable reste UNKNOWN ; la lacune n’est jamais assimilée à une absence d’effet ou à un appel réussi.

---

## Contrat d’identité par format

Les deux pistes exigent le pipeline de lift (il recouvre les arguments par appel). Chaque callee est nommé via la même vue d’identité que le reste de NeverD. L’ordre de découverte des informations de débogage est inchangé.

| Format | Débogage (par ordre de priorité) | Résolution imports / thunks |
|--------|----------------------------------|-----------------------------|
| **PE/COFF** | `--pdb`, répertoire de débogage ou `.pdb` voisin, puis `/MAP` MSVC | Emplacements IAT et thunks `__imp_`, imports ordinaux |
| **ELF** | DWARF dans l’image, `*.debug` séparé, puis MAP GNU/LLD | Stubs PLT résolus vers le nom importé |
| **Mach-O** | DWARF dans l’image, `.dSYM` adjacent, puis `-map` ld64 | Bind dyld / emplacements de symboles indirects et helpers de stub |

`--pdb` / `--map` désignent un fichier compagnon faisant autorité : un échec de lecture est une erreur, pas un repli silencieux. `--no-debug` lit l’image seule, pour tous les formats.

Les signatures de procédure PDB servent à distinguer les allocateurs qui renvoient une valeur des fonctions de libération `void`. La récupération fine des types locaux et de pile depuis un PDB reste limitée ; lorsqu’elle ne parvient pas à établir une taille d’objet exacte, la chasse se replie sur le modèle de trame ou d’allocation et rapporte UNKNOWN plutôt que d’inventer une taille.

### Priorité de `name_source`

Chaque découverte porte un `name_source` indiquant d’où vient le nom du callee, selon cette priorité :

1. `rename` — un renommage fourni par l’appelant
2. `import` — une entrée IAT (PE), PLT (ELF) ou dyld-bind / stub (Mach-O)
3. `export` / `symbol` — une exportation ou entrée de symboles déjà déclarée par l’image
4. `pdb` / `dwarf` / `map` — un symbole de débogage qui établit un placeholder ou concorde avec le nom déclaré
5. `sig` — une correspondance de signatures
6. `synthetic` — un placeholder pour une routine sans nom

Un `memcpy` lié statiquement nommé par DWARF rapporte `dwarf` ; un `memcpy` importé rapporte `import` sur tous les formats. Une correspondance de signatures ne remplace jamais un nom déjà établi par le débogueur ou la table d’imports.

---

## Catalogue de puits et de sources

Le catalogue est une table configurable, pas un ensemble figé. Chaque **puits** déclare sa classe de faiblesse, son rôle (copy, format, alloc, free, realloc) et les emplacements d’arguments concernés (destination, source, longueur, capacité). Un puits JSON de type copy ou format fournit aussi un effet d’appel exécutable. Chaque **source** nomme un fournisseur d’entrée influencée par un attaquant.

Les entrées intégrées vivent dans [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) et [`SafetySources.def`](../include/neverd/safety/SafetySources.def) ; elles couvrent la famille de copies C courante (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), les variantes fortifiées `_chk` (borne de destination explicite), la famille d’allocation et de libération (`malloc`/`calloc`/`realloc`/`free`, opérateurs `new`/`delete`) et des API tas Win32 optionnelles. Les sources d’entrée incluent POSIX (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, arguments du programme) **et** Win32 (`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`) : une chasse PE n’est pas limitée aux entrées POSIX.

Les graphies propres à un format se replient sur une seule entrée : les underscores de tête sont ôtés (`_malloc`, `___strcpy_chk`) et les opérateurs `new`/`delete` mangled passent par des alias.

Quand un puits JSON de type copy ou format omet `effect`, son applicabilité est déduite de l’emplacement d’argument référencé le plus élevé. Une copie exige alors exactement cette arité ; un puits de format accepte les appels depuis cette arité minimale jusqu’au maximum variadique. Un objet `effect` facultatif peut définir explicitement avec `min_arity` et `max_arity` (ou `"variadic"`) une plage d’arités acceptée, y compris des arguments de wrapper supplémentaires au-delà de l’arité copy exacte déduite ; `min_arity` doit valoir au moins l’emplacement de rôle référencé le plus élevé plus un, tandis que `formats` et `abis` restreignent l’applicabilité. Si l’arité, le format objet ou l’ABI de l’appel ne correspond pas, aucun résumé ne s’applique et le résultat en monde fermé reste UNKNOWN.

Étendez ou remplacez le catalogue par un fichier de spécification :

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 },
    { "name": "my_format", "kind": "format", "dst": 0, "fmt": 2,
      "effect": { "min_arity": 3, "max_arity": "variadic",
                  "formats": ["elf"], "abis": ["sysv"] } }
  ],
  "sources": [
    { "name": "my_read", "out": 1, "return_tainted": true }
  ]
}
```

Pour une source personnalisée, `out` et `return_tainted` ne sont que des métadonnées de découverte. Ils n’établissent aucun effet exécutable sur la mémoire, la valeur de retour ou le taint. Le schéma de source actuel ne contient pas les contrats typés de réussite, de mutation, de format et d’ABI nécessaires à ces sémantiques ; une analyse qui dépend de l’effet d’une source personnalisée reste donc UNKNOWN. Les sources intégrées ne sont pas concernées : leurs descripteurs typés et soumis aux contrôles d’applicabilité continuent de fournir des effets exécutables.

Un puits personnalisé non borné avec pour seul argument une destination n’est pas déduit d’une entrée de source du même nom. Un puits personnalisé analogue à `gets` doit activer explicitement `"unbounded": true` ; ajouter le même nom au catalogue de sources ne lui confère aucun effet exécutable, et les champs source/longueur contradictoires sont rejetés transactionnellement.

---

## Hunt : verdicts de débordement de copie

Pour chaque puits de copie, la chasse recouvre la capacité de destination — taille de tableau déclarée par le débogage, puis site d’allocation tas de taille connue, puis borne saine de cadre de pile — et classifie l’argument qui décide de la longueur d’écriture par un parcours SSA arrière (en suivant spill/reload via les emplacements de pile) :

- Une **longueur constante** dans une capacité exacte est SAFE. Un débordement constant n’est UNSAFE que si le puits est atteignable sur un chemin corroboré ; sinon il reste UNKNOWN.
- Les copies **fortifiées** `_chk` portent une borne de destination runtime. Un rejet ou une borne prouvée compatible est SAFE ; une écriture réalisable au-delà de l’objet est UNSAFE ; une borne non recouvrée ou non concluante est UNKNOWN.
- Longueur **prouvablement bornée** (appel renvoyant une longueur, masque, clamp) retirée avant résolution, avec la raison. Elle n’est SAFE que si la taille de destination est exacte ; une simple borne de région reste UNKNOWN.
- Longueur **influencée par un attaquant** et capacité connue : le solveur bitvector est consulté. Si une longueur supérieure à la capacité est satisfaisable, le verdict est UNSAFE. Les candidats ne sont rejouables qu’avec un plan `process-input-v1` complet : initialement les valeurs littérales exactes de l’environnement et, au plus, les octets renvoyés par la première consommation de l’entrée standard. argv, fichiers, réseau, sources personnalisées ou ambiguës restent non rejouables avec une raison.
- Tout le reste — longueur ou capacité inconnue — est UNKNOWN.

Toute capacité recouvrée est une **borne supérieure** de la taille réelle, donc un débordement prouvé n’est jamais un faux positif.

### Entrée formatée

Pour `scanf`/`fscanf` et leurs graphies versionnées, un format constant lisible associe chaque conversion non supprimée à son véritable argument de sortie variadique. Les sorties `%s`/`%[` non bornées propagent le taint aux usages ultérieurs de la chaîne ; les sorties numériques et de caractères propagent le taint aux valeurs chargées depuis l’objet écrit, mais pas à la valeur du pointeur de sortie lui-même. `sscanf` ne propage ces effets que si sa chaîne d’entrée est déjà influencée par un attaquant. Les sorties texte bornées telles que `%Ns`/`%N[` propagent le taint avec une étendue `MaxBytes` qui inclut le terminateur ; les variantes à caractères larges calculent cette étendue en octets à l’aide de la largeur de `wchar_t` de la plateforme. Les conversions supprimées, les arguments excédentaires, les formats dépendant de la position ou non pris en charge et `%n` restent UNKNOWN au lieu d’être devinés.

---

## Audit : verdicts de durée de vie du tas

Pour chaque allocation, l’audit suit le handle dans le graphe de flot de contrôle, y compris via spill/reload de pile, et applique un résumé d’évasion (retourné, stocké via une adresse hors pile, ou passé à un callee opaque) :

- **Fuite** — le handle n’est ni libéré ni autorisé à s’échapper.
- **Double libération** — une seconde libération est atteignable après une première sur un chemin.
- **Utilisation après libération** — un déréférencement ou un usage opaque est atteignable après une libération.

Les **enveloppes** d’allocation et de libération sont reconnues par des résumés d’évasion par fonction, donc un transmetteur `malloc`/`free` ne masque pas le défaut. Des libérations sur des branches mutuellement exclusives ne sont pas rapportées comme double libération.

La machine à états du tas produit d’abord une séquence d’événements candidate (allocation, libération, utilisation ou sortie par retour). Une seconde passe doit rejouer cette séquence sur un chemin LowIR symbolique et prouver que son prédicat de chemin est satisfiable avant que le résultat ne devienne un UNSAFE de confiance HAUTE. LowIR absent, opérations opaques, appels sans résumé, incertitude du solveur et limites d’exploration rétrogradent le candidat en UNKNOWN. Le havoc mémoire may-alias conservateur est suivi séparément, de sorte que des écritures ordinaires dans la trame de pile n’invalident pas une preuve d’atteignabilité par ailleurs exacte.

---

## Atteignabilité interprocédurale depuis les entrées connues

Chaque découverte porte trois affirmations indépendantes qu’il ne faut pas
confondre :

| Champ | Question | Valeurs |
|-------|----------|---------|
| `verdict` | Que prouve l’analyse de sûreté locale sur l’opération ? | `SAFE`, `UNSAFE`, `UNKNOWN` |
| `reachability.status` | La fonction englobante se trouve-t-elle sur un chemin de contrôle reconstruit depuis une entrée native connue ? | `REACHABLE`, `UNREACHABLE`, `UNKNOWN` |
| `reachability.attacker_control` | Que prouve la tranche de l’argument sur l’influence de l’attaquant à cette découverte ? | `TAINTED`, `BOUNDED`, `UNKNOWN` |

L’atteignabilité est une preuve additive : elle ne modifie ni le `verdict` de la
découverte, ni le verdict agrégé, ni le code de sortie CLI. Un débordement prouvé
localement peut donc porter `verdict=UNSAFE` et
`reachability.status=UNREACHABLE`. Un consommateur qui exige un chemin d’attaque
exécutable doit tester les deux champs.

Les racines sont les entrées d’application reconnues (`application`, par
exemple `main` ou `WinMain`), l’entrée de l’image (`image`) et les routines
exportées (`export`). Si une fonction possède plusieurs identités, la priorité
déterministe est `application`, puis `image`, puis `export`.
`reachability.entry` enregistre `va`, `name` et `kind`. Pour une découverte
atteignable hors racine, `call_chain` contient aussi le plus court chemin
déterministe d’arêtes internes exactes, avec `caller_va`, le site `call_va`,
`callee_va` et un `kind` `direct` ou `indirect`.

`UNREACHABLE` n’est émis que si une racine existe, que l’inventaire des appels
internes est complet et que la profondeur n’a pas été épuisée. Pour une fonction
qui n’est pas déjà positivement atteinte, racines absentes, identités de
fonction dupliquées ou ambiguës, inventaires CFG/appels incohérents, cibles
internes exécutables non résolues et épuisement de la profondeur empêchent la
preuve négative et donnent `reachability.status=UNKNOWN`, avec `reason` et
`budget_hit` le cas échéant. ABI inconnue, largeur d’argument incompatible, slot seulement
variadique, tranche incomplète ou budget de profondeur/résumé épuisé laissent
aussi à UNKNOWN tout contrôle attaquant encore non prouvé ; les faits déjà
établis restent valides et aucune propagation n’est inventée.

Les compteurs du rapport dénombrent les découvertes, pas les fonctions ni les
chemins. `control_reachable` compte `status=REACHABLE` ;
`attacker_reachable` est le sous-ensemble qui a aussi
`attacker_control=TAINTED`. `reachability_unknown` et `unreachable` comptent les
autres états de contrôle. Ils sont distincts de `safe`, `unsafe` et `unknown`,
qui comptent les verdicts.

---

## Budgets, sortie et liaisons

L’exploration de chasse et le solveur sont bornés (`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`). Pour l’interprocédural, `max_call_depth` borne le nombre d’arêtes d’appel internes depuis une entrée connue et `max_summary_iterations` borne les tours de point fixe du contrôle attaquant. Les valeurs par défaut sont 64 arêtes et la profondeur effective plus un tour. L’épuisement échoue fermé comme décrit ci-dessus. Épuiser `max_call_depth` peut laisser `status=UNKNOWN` pour une fonction pas encore atteinte ; épuiser `max_summary_iterations` n’efface pas le témoin structurel, si bien que `status=REACHABLE` peut coexister avec `attacker_control=UNKNOWN` et `budget_hit=true`. Les deux commandes impriment du JSON et honorent `-o`. Le code de sortie est `0` pour SAFE, `2` pour UNSAFE et `1` pour UNKNOWN ou une erreur.

Zéro sélectionne la valeur par défaut du moteur sur toutes les interfaces :

| Interface | Profondeur de contrôle | Résumé attaquant |
|-----------|-------------------------|------------------|
| CLI (`audit` et `hunt`) | `--max-call-depth <n>` | `--max-summary-iterations <n>` |
| C (`neverd_safety_options`) | `max_call_depth` | `max_summary_iterations` |
| Python (`Session.audit()` / `Session.hunt()`) | `max_call_depth=<n>` | `max_summary_iterations=<n>` |

L’appelant C initialise `neverd_safety_options` à zéro et fixe
`struct_size=sizeof(neverd_safety_options)` ; les anciennes tailles conservent
les valeurs par défaut. Python valide les deux valeurs comme entiers non signés
sur 32 bits.

Les mêmes analyses sont disponibles via l’API C (`neverd_session_audit_json` / `neverd_session_hunt_json`, `neverd_safety_options` versionnées) et le SDK Python (`Session.audit()` / `Session.hunt()`).

### Schéma d’une découverte

```json
{
  "class": "buffer_overflow",
  "function": "parse_header",
  "name": "strcpy",
  "name_source": "import",
  "call_va": "0x11a4",
  "source": "reader.c:42",
  "sink": "strcpy",
  "arg_index": 1,
  "flow": "TAINTED",
  "verdict": "UNSAFE",
  "confidence": "HIGH",
  "capacity": 16,
  "capacity_kind": "exact",
  "corroboration": "path predicate and overflow are jointly satisfiable",
  "reachability": { "status": "REACHABLE", "attacker_control": "TAINTED", "budget_hit": false, "entry": { "va": "0x1000", "name": "main", "kind": "application" }, "call_chain": [{ "caller_va": "0x1000", "call_va": "0x1080", "callee_va": "0x1100", "kind": "direct" }] },
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "replay": { "adapter": "process-input-v1", "reason": "argv input is not supported by process-input-v1" }, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

`replayable` est une preuve dérivée, pas une promesse indépendante : il n’est vrai que si `replay` contient un plan d’entrée complet pour l’adaptateur `process-input-v1`. Le plan enregistre les octets exacts de l’environnement, la première séquence d’entrée standard si elle est utilisée et les liaisons depuis les identifiants d’affectation du solveur ; sinon `replay.reason` en explique la raison. Les champs de rejeu et d’atteignabilité sont additifs ; le `schema_version` de tête reste `1`.

---

## Bornes de faux positifs et périmètre

- La capacité est exacte ou une borne supérieure de la taille réelle ; UNSAFE reflète donc un vrai débordement. Sans taille exacte, une borne de région insuffisante pour prouver la sûreté produit UNKNOWN.
- Une copie bornée en longueur est retirée avant résolution et comptée dans `skipped` ; une capacité exacte peut établir SAFE, une borne seule reste UNKNOWN.
- Les copies cataloguées de caractères larges et d’ajout restent UNKNOWN jusqu’au recouvrement de la largeur d’élément et de l’étendue actuelle de la destination. Les allocateurs par paramètre de sortie et la propriété conditionnelle de `realloc` restent aussi UNKNOWN si la transition du handle ne peut être prouvée.
- **P0** (cette version, les trois formats) : catalogue de puits, préfiltre d’arguments, chasse de débordement de copie, audit de durée de vie du tas. Chaque hôte teste les six fixtures PE, ELF et Mach-O pour x86-64 et AArch64.
- **P1** : les débordements pile/global, les lectures locales non initialisées et les contrôles de chaînes de format sont disponibles ; les types de pile PDB plus riches et les allocateurs de plateforme supplémentaires restent une couverture incrémentale, et l’absence de résumé exact reste UNKNOWN.
- La tranche actuelle couvre les entrées connues, l’atteignabilité interprocédurale structurelle et la propagation monotone des paramètres attaquants. L’adaptateur expérimental distinct `lowir-concolic-v1` fournit désormais des inversions de branche, alimentées par des graines de registres et vérifiées par rejeu, sur la matrice native obligatoire de formats et d’architectures ; il reste non exhaustif et ne modifie pas les verdicts de sûreté. Le `binary-sanitizer-v1` expérimental fournit maintenant sur Darwin des gardes d’écriture comptée tout-ou-refus et une publication authentifiée ; son receipt authentifie l’objet répertoire conservé pendant la transaction, et non une liaison durable et revérifiable du pathname d’origine. Le `process-replay-v1` élargi reste limité à une frontière fail-closed de Phase 0 pour le plan, le coordinateur et la disponibilité ; aucun hôte n’exécute actuellement de rejeu natif.
