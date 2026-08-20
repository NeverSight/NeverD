**Langues**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Index de la documentation](README.fr.md)

# Audit et chasse de sûreté mémoire

NeverD analyse un binaire chargé selon deux familles de défauts de sûreté mémoire et les rapporte en JSON structuré. Les deux pistes s’exécutent sur l’IR levé, indépendant du format, donc **PE/COFF, ELF et Mach-O sont des cibles de premier rang, à égalité** : une découverte n’est jamais conditionnée à un scanner ou une table d’imports d’un seul format.

| Piste | Commande | Rapporte |
|-------|----------|----------|
| **Audit** | `neverd audit <binary>` | Défauts de durée de vie du tas : fuite, double libération, utilisation après libération |
| **Hunt** | `neverd hunt <binary>` | Débordements de copies dangereuses, avec un témoin concret reproductible |

Le moteur réutilise l’exécution symbolique et le solveur bitvector internes de NeverD pour les témoins et l’atteignabilité. Aucun solveur externe, VM ou conteneur.

---

## Invariant central : échec fermé

Une opération non levée, un appel dont l’ABI n’a pas recouvré les arguments, une cible indirecte non résolue ou un budget épuisé donnent **UNKNOWN**, jamais SAFE. Une destination dont la capacité n’est pas recouvrable est UNKNOWN. Le lifting strict reste inchangé ; la couche de sûreté n’ajoute que des verdicts conservateurs par-dessus.

---

## Contrat d’identité par format

Les deux pistes exigent le pipeline de lift (il recouvre les arguments par appel). Chaque callee est nommé via la même vue d’identité que le reste de NeverD. L’ordre de découverte des informations de débogage est inchangé.

| Format | Débogage (par ordre de priorité) | Résolution imports / thunks |
|--------|----------------------------------|-----------------------------|
| **PE/COFF** | `--pdb`, répertoire de débogage ou `.pdb` voisin, puis `/MAP` MSVC | Emplacements IAT et thunks `__imp_`, imports ordinaux |
| **ELF** | DWARF dans l’image, `*.debug` séparé, puis MAP GNU/LLD | Stubs PLT résolus vers le nom importé |
| **Mach-O** | DWARF dans l’image, `.dSYM` adjacent, puis `-map` ld64 | Bind dyld / emplacements de symboles indirects et helpers de stub |

`--pdb` / `--map` désignent un fichier compagnon faisant autorité : un échec de lecture est une erreur, pas un repli silencieux. `--no-debug` lit l’image seule, pour tous les formats.

### Priorité de `name_source`

Chaque découverte porte un `name_source` indiquant d’où vient le nom du callee, selon cette priorité :

1. `rename` — un renommage fourni par l’appelant
2. `import` — une entrée IAT (PE), PLT (ELF) ou dyld-bind / stub (Mach-O)
3. `pdb` / `dwarf` / `map` — un symbole de débogage, selon le chargeur
4. `export` / `symbol` — une exportation ou une entrée de table de symboles
5. `sig` — une correspondance de signatures
6. `synthetic` — un placeholder pour une routine sans nom

Un `memcpy` lié statiquement nommé par DWARF rapporte `dwarf` ; un `memcpy` importé rapporte `import` sur tous les formats. Une correspondance de signatures ne remplace jamais un nom déjà établi par le débogueur ou la table d’imports.

---

## Catalogue de puits et de sources

Le catalogue est une table configurable, pas un ensemble figé. Chaque **puits** déclare sa classe de faiblesse, son rôle (copy, format, alloc, free, realloc) et les emplacements d’arguments concernés (destination, source, longueur, capacité). Chaque **source** nomme un fournisseur d’entrée influencée par un attaquant.

Le catalogue intégré couvre la famille de copies C courante (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), les variantes fortifiées `_chk` (borne de destination explicite), la famille d’allocation et de libération (`malloc`/`calloc`/`realloc`/`free`, opérateurs `new`/`delete`) et des API tas Win32 optionnelles. Les sources d’entrée incluent POSIX (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, arguments du programme) **et** Win32 (`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`) : une chasse PE n’est pas limitée aux entrées POSIX.

Les graphies propres à un format se replient sur une seule entrée : les underscores de tête sont ôtés (`_malloc`, `___strcpy_chk`) et les opérateurs `new`/`delete` mangled passent par des alias.

Étendez ou remplacez le catalogue par un fichier de spécification :

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 }
] }
```

---

## Hunt : verdicts de débordement de copie

Pour chaque puits de copie, la chasse recouvre la capacité de destination — taille de tableau déclarée par le débogage, puis site d’allocation tas de taille connue, puis borne saine de cadre de pile — et classifie l’argument qui décide de la longueur d’écriture par un parcours SSA arrière (en suivant spill/reload via les emplacements de pile) :

- **Longueur constante** comparée directement à la capacité → SAFE ou UNSAFE.
- Copies **fortifiées** `_chk` portant une borne runtime → SAFE.
- Longueur **prouvablement bornée** (appel renvoyant une longueur, masque, clamp) retirée en SAFE skip, avec la raison.
- Longueur **influencée par un attaquant** et capacité connue : le solveur bitvector est consulté. Si une longueur supérieure à la capacité est satisfaisable, le verdict est UNSAFE et le modèle du solveur devient le témoin concret.
- Tout le reste — longueur ou capacité inconnue — est UNKNOWN.

Toute capacité recouvrée est une **borne supérieure** de la taille réelle, donc un débordement prouvé n’est jamais un faux positif.

---

## Audit : verdicts de durée de vie du tas

Pour chaque allocation, l’audit suit le handle dans le graphe de flot de contrôle, y compris via spill/reload de pile, et applique un résumé d’évasion (retourné, stocké via une adresse hors pile, ou passé à un callee opaque) :

- **Fuite** — le handle n’est ni libéré ni autorisé à s’échapper.
- **Double libération** — une seconde libération est atteignable après une première sur un chemin.
- **Utilisation après libération** — un déréférencement ou un usage opaque est atteignable après une libération.

Les **enveloppes** d’allocation et de libération sont reconnues par des résumés d’évasion par fonction, donc un transmetteur `malloc`/`free` ne masque pas le défaut. Des libérations sur des branches mutuellement exclusives ne sont pas rapportées comme double libération.

---

## Budgets, sortie et liaisons

L’exploration de chasse et le solveur sont bornés (`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`) ; l’épuisement du budget donne UNKNOWN. Les deux commandes impriment du JSON et honorent `-o`. Le code de sortie est `0` pour une exécution propre, `2` s’il y a une découverte UNSAFE, `1` en cas d’erreur.

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "17 bytes" } }
}
```

---

## Bornes de faux positifs et périmètre

- La capacité est toujours une borne supérieure, donc UNSAFE reflète un vrai débordement. Un tampon trop petit dont la taille déclarée est indisponible peut être rapporté SAFE plutôt qu’UNSAFE (manque conservateur, jamais fausse alarme).
- Une copie bornée en longueur est retirée en SAFE skip ; cela privilégie la précision des cas contrôlés par l’attaquant que la chasse vise à prouver.
- **P0** (cette version, les trois formats) : catalogue de puits, préfiltre d’arguments, chasse de débordement de copie, audit de durée de vie du tas.
- **P1** : débordements pile/global, lectures non initialisées, chaînes de format, types de pile PDB plus riches, allocateurs de plateforme supplémentaires.
- **P2** : contrôles runtime insérés par patch, atteignabilité interprocédurale de l’attaquant.
